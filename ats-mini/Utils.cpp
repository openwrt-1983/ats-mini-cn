#include <driver/rtc_io.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include "Storage.h"
#include "BleMode.h"
#include "Common.h"
#include "Themes.h"
#include "Button.h"
#include "Utils.h"
#include "Menu.h"
#include "Patches.h"
#include "Draw.h"

#include <sys/time.h>
#include <time.h>

extern ButtonTracker pb1;

// Current sleep status, returned by sleepOn()
static bool sleep_on = false;

// Whether Bluetooth standby applied its own main mute (not the user's mute).
static bool bleStandbyMuted = false;

// TV style standby powers the SI4732 down. Its tuning state is lost, so the
// wake path re-runs the cold start sequence and re-applies the current band.
static bool radioStandbyOn = false;

// The standby timer can enter Bluetooth standby even when another sleep mode
// is configured. The override only lasts until the display wakes up.
static bool standbyForced = false;

bool radioInStandby()
{
  return radioStandbyOn;
}

bool sleepIsBleStandby()
{
  return sleepModeIdx == SLEEP_BLE_KEEPALIVE || standbyForced;
}

bool sleepKeepsBle()
{
  return sleepIsBleStandby() || sleepModeIdx == SLEEP_LIGHT;
}

static void radioStandbyEnter()
{
  if(radioStandbyOn) return;
  rx.setAudioMute(true);
  delay(20);
  rx.powerDown();
  radioStandbyOn = true;
}

static void radioStandbyLeave()
{
  if(!radioStandbyOn) return;
  // Power down wipes the tuning state and any DSP patch: bring the chip back
  // up like a cold start, then reselect the band so frequency, bandwidth,
  // AGC/AVC and patch state are all restored. Starting in the band's own mode
  // (FM vs AM) saves the extra mode switch selectBand() would otherwise do.
  rx.setup(RESET_PIN, bands[bandIdx].bandType == FM_BAND_TYPE ? FM_BAND_TYPE : MW_BAND_TYPE);
  patchesResetLoaded();
  selectBand(bandIdx, false);
  // The chip powers up unmuted at the library default volume, with the
  // amplifier and headphone clamp still shut. Keep the DAC silent here; the
  // volume returns with the fade in muteOn() when the standby ends.
  rx.setVolume(0);
  rx.setMaxSeekTime(SEEK_TIMEOUT);
  radioStandbyOn = false;
}

// esp_timer includes time spent in light sleep and is independent of the clock.
uint16_t offTimerMinutes = 0;
static int64_t offTimerDeadline = 0;

// In the last minutes before a scheduled power off, drop Wi-Fi and Bluetooth so
// the unit is already offline when it goes down instead of tearing the links
// down at the very last moment.
#define OFF_TIMER_QUIET_US ((int64_t)3 * 60000000)
static bool offTimerQuiet = false;
// Restoring the links can block for seconds (Wi-Fi association), so it is
// deferred to the timer tick instead of running inside the encoder handler.
static bool offTimerQuietRestore = false;

static int64_t offTimerRemainingUs()
{
  int64_t remaining = offTimerDeadline - esp_timer_get_time();
  return offTimerDeadline && remaining > 0 ? remaining : 0;
}

static void offTimerQuietEnter()
{
  if(offTimerQuiet) return;
  offTimerQuiet = true;
  offTimerQuietRestore = false;
  netStop();
  bleStop();
}

static void offTimerQuietLeave()
{
  if(!offTimerQuiet) return;
  offTimerQuiet = false;
  offTimerQuietRestore = true;
}

void offTimerRestart()
{
  offTimerDeadline = offTimerMinutes ? esp_timer_get_time() + (int64_t)offTimerMinutes * 60000000 : 0;
  // Pushing the deadline out of the quiet window gives the links back.
  if(offTimerQuiet && (!offTimerDeadline || offTimerRemainingUs() > OFF_TIMER_QUIET_US))
    offTimerQuietLeave();
}

uint16_t offTimerRemainingMinutes()
{
  return (offTimerRemainingUs() + 59999999) / 60000000;
}

// Standby timer: same countdown, but it enters Bluetooth standby at the end
// instead of powering the receiver off. It fires once, so waking up does not
// drop straight back into standby.
uint16_t standbyTimerMinutes = 0;
static int64_t standbyTimerDeadline = 0;

static int64_t standbyTimerRemainingUs()
{
  int64_t remaining = standbyTimerDeadline - esp_timer_get_time();
  return standbyTimerDeadline && remaining > 0 ? remaining : 0;
}

void standbyTimerRestart()
{
  standbyTimerDeadline = standbyTimerMinutes ? esp_timer_get_time() + (int64_t)standbyTimerMinutes * 60000000 : 0;
}

uint16_t standbyTimerRemainingMinutes()
{
  return (standbyTimerRemainingUs() + 59999999) / 60000000;
}

// One place where "the user did something" is recorded. Physical input calls
// it from the main loop; every remote channel calls it once per executed
// command, so a remote session renews the display sleep timer and the standby
// countdown exactly like turning the encoder on the unit. Deliberately NOT
// called for the periodic status stream: an app parked on the mirror screen
// must not keep the display awake forever, and the watchdog's occasional 'g'
// re-arm is rare enough to be harmless. The timestamps themselves
// (elapsedSleep / elapsedCommand) are defined in the main sketch and declared
// extern in Utils.h.
void touchActivity()
{
  elapsedSleep = elapsedCommand = millis();
  standbyTimerRestart();
}

// Drive a rail low for the whole deep sleep. The digital domain is powered
// down, so a pin left on the digital mux loses its configuration and floats:
// it has to be routed to the RTC IO mux, which keeps its own configuration and
// keeps driving the pad while the rest of the chip is off.
//
// Note what is deliberately NOT used here: gpio_hold_en()/gpio_deep_sleep_hold
// _en(). On the S3 the only hold available is RTC_CNTL_DG_PAD_AUTOHOLD_EN, and
// it holds *every* digital pad at the level it had when the chip went down --
// including the encoder button input, frozen at its released (high) level. The
// ext0 wake source could then never see a press again.
static void railOutputLow(int pin)
{
  if(pin < 0) return;
  pinMode(pin, OUTPUT);
  digitalWrite(pin, LOW);
  // Not an RTC capable pad (e.g. the backlight on GPIO38): nothing more can be
  // done, it goes down with the digital domain.
  if(rtc_io_number_get((gpio_num_t)pin) < 0) return;
  rtc_gpio_init((gpio_num_t)pin);
  rtc_gpio_pullup_dis((gpio_num_t)pin);
  rtc_gpio_pulldown_dis((gpio_num_t)pin);
  rtc_gpio_set_level((gpio_num_t)pin, 0);
  rtc_gpio_set_direction((gpio_num_t)pin, RTC_GPIO_MODE_OUTPUT_ONLY);
}

// Hand the rails back to the digital domain after a deep sleep: the RTC IO mux
// keeps its routing across the wake, so setup() could not drive them otherwise.
static void railsRelease()
{
  gpio_deep_sleep_hold_dis();  // clears a hold left by a firmware that used it
  // The backlight is parked on the RTC mux as well (railOutputLow in
  // deepSleepEnter), and setup() only attaches the LEDC after this runs. Left
  // routed to the RTC domain, the pad ignores the LEDC and the panel stays dark
  // for the whole session after a standby wake.
  rtc_gpio_deinit((gpio_num_t)PIN_LCD_BL);
  if(PIN_AMP_EN >= 0) rtc_gpio_deinit((gpio_num_t)PIN_AMP_EN);
  rtc_gpio_deinit((gpio_num_t)PIN_POWER_ON);
}

// How long the wake press must still be held once setup() runs. Only a guard
// against contact bounce and a brief knock on the knob: the press itself
// already started before the boot loader, so any deliberate hold passes.
#define STANDBY_WAKE_HOLD_MS 400

// Upper bound for letting the standby long press end before the wake source is
// armed. A pad that somehow stays low must not leave the unit spinning here
// with the screen already dark: that looks exactly like a receiver that sleeps
// and then ignores every further press.
#define STANDBY_RELEASE_TIMEOUT_MS 3000

// Deep sleep is the lowest power state the unit has: the CPU stops, the
// backlight, amplifier and receiver supply are held low, and the SI4732 is
// powered down. The long press standby and the elapsed off timer pass
// wakeOnButton, which arms the encoder button so the same gesture brings the
// unit back; receiverShutdown() is the variant without a wake source, undone
// only by a power cycle.
static void deepSleepEnter(bool wakeOnButton)
{
  // Bluetooth standby already muted the audio and cut the amplifier, and the
  // tuner is powered down, so touching them again would only poke a sleeping
  // chip (and, on boards without the clamp, drive AUDIO_MUTE the wrong way).
  if(!radioInStandby()) muteOn(MUTE_FORCE, true, true);
  netStop();
  bleStop();
  prefsSave();
  // Already powered down by TV style standby: sending POWER_DOWN again is
  // harmless but pointless, and it would wait for a chip that is asleep.
  if(!radioInStandby()) rx.powerDown();
  ledcWrite(PIN_LCD_BL, 0);
  tft.sleep();
  delay(120);

  // Keep the receiver supply, amplifier, and backlight off in deep sleep.
  ledcDetach(PIN_LCD_BL);
  railOutputLow(PIN_LCD_BL);
  railOutputLow(PIN_AMP_EN);
  railOutputLow(PIN_POWER_ON);

  // Discard any pending light-sleep timer wake.
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  // INPUT_PULLUP, not INPUT: plain INPUT drops the internal pull-up that
  // setup() installed, and a floating encoder pin can read low forever.
  pinMode(ENCODER_PUSH_BUTTON, INPUT_PULLUP);
  pinMode(ENCODER_PIN_A, INPUT_PULLUP);
  pinMode(ENCODER_PIN_B, INPUT_PULLUP);

  if(wakeOnButton)
  {
    // The wake level is "button down", and the long press that got us here is
    // still held: let it end, or the chip would wake up on the spot. Bounded,
    // so a stuck-low reading cannot trap the unit before ext0 is armed.
    uint32_t released = millis();
    while(digitalRead(ENCODER_PUSH_BUTTON) == LOW && millis() - released < STANDBY_RELEASE_TIMEOUT_MS) delay(10);
    delay(50); // Debounce margin, so a bouncing release is not a new press.
    esp_sleep_enable_ext0_wakeup((gpio_num_t)ENCODER_PUSH_BUTTON, 0);
    // The digital domain is off in deep sleep, so the pin needs the RTC
    // pull-up: left floating it reads low and wakes the unit immediately.
    rtc_gpio_pulldown_dis((gpio_num_t)ENCODER_PUSH_BUTTON);
    rtc_gpio_pullup_en((gpio_num_t)ENCODER_PUSH_BUTTON);
    // ext0 keeps its configuration in the RTC peripheral domain.
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  }
  else
  {
    rtc_gpio_pullup_dis((gpio_num_t)ENCODER_PUSH_BUTTON);
    rtc_gpio_pulldown_dis((gpio_num_t)ENCODER_PUSH_BUTTON);
    rtc_gpio_deinit((gpio_num_t)ENCODER_PUSH_BUTTON);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_OFF);
  }
  esp_deep_sleep_start();
}

void receiverShutdown()
{
  deepSleepEnter(false);
}

//
// Long press standby: deep sleep with the encoder button armed as the wake
// source. The elapsed off timer lands here as well, so both ways of switching
// the unit off are undone by the same long press. Waking is a full reset, so
// the receiver is set up from scratch and the audio comes back with the stored
// band and volume. Never returns.
//
void standbySleepEnter()
{
  deepSleepEnter(true);
}

//
// True when the unit was woken from standby. The rails were driven through the
// RTC IO mux while the chip was down, so they are handed back to the digital
// domain here -- before setup() configures any pin.
//
bool standbyWoke()
{
  railsRelease();
  return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
}

//
// The wake press started before setup() runs (boot loader, PSRAM and flash
// init take a few hundred milliseconds), so the button is already down here.
// Keeping it down for STANDBY_WAKE_HOLD_MS more separates a deliberate press
// from a knock against the knob; the time already spent booting counts towards
// the gesture, so a two second press is never rejected on slow boots.
// When the press was too short this does not return.
//
void standbyWakeConfirm()
{
  uint32_t started = millis();
  while(millis() - started < STANDBY_WAKE_HOLD_MS)
  {
    if(digitalRead(ENCODER_PUSH_BUTTON) != LOW)
    {
      // Nothing has been loaded or changed yet, so the stored settings must
      // not be written back: prefsSave() here would save defaults over them.
      //
      // standbyWoke() has already released the rails to the digital domain and
      // setup() drove PIN_POWER_ON high for the SI4732. Going back to sleep
      // without parking them on the RTC mux again would leave the receiver
      // supply floating for the whole sleep instead of held low.
      railOutputLow(PIN_LCD_BL);
      railOutputLow(PIN_AMP_EN);
      railOutputLow(PIN_POWER_ON);

      esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
      esp_sleep_enable_ext0_wakeup((gpio_num_t)ENCODER_PUSH_BUTTON, 0);
      rtc_gpio_pulldown_dis((gpio_num_t)ENCODER_PUSH_BUTTON);
      rtc_gpio_pullup_en((gpio_num_t)ENCODER_PUSH_BUTTON);
      esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
      esp_deep_sleep_start();
    }
    delay(10);
  }
}

void offTimerTick()
{
  // Bring the links back outside the input handlers: Wi-Fi association can
  // block for seconds, and doing it while a menu item is being turned stalls
  // the encoder. Skip it if the deadline moved back into the quiet window.
  if(offTimerQuietRestore && (!offTimerDeadline || offTimerRemainingUs() > OFF_TIMER_QUIET_US))
  {
    offTimerQuietRestore = false;
    netInit(wifiModeIdx);
    bleInit(bleModeIdx);
    // Stay on the slow advertising interval if the unit is still in standby.
    if(sleepOn() && sleepIsBleStandby()) bleStandbyAdvertising(true);
  }

  if(!offTimerDeadline) return;
  if(!offTimerRemainingUs())
  {
    // The countdown ends in the same state as a long press: deep sleep with the
    // encoder button armed, so the same gesture brings the receiver back on the
    // stored band instead of leaving it dark until the battery is unplugged.
    //
    // One shot, like the standby timer. The duration is stored, and a wake from
    // this sleep is a full reset: leaving it armed would make every later
    // session drop back into standby after the same time, with nothing on the
    // panel to announce it. deepSleepEnter() saves the cleared value.
    offTimerDeadline = 0;
    offTimerMinutes = 0;
    // Both countdowns belong to the session that is ending. A standby timer
    // still armed here can no longer fire in this one, and leaving it stored
    // would make the next session drop into Bluetooth standby after the same
    // delay without anyone asking for it.
    standbyTimerDeadline = 0;
    standbyTimerMinutes = 0;
#if ENCODER_PUSH_BUTTON == 0
    // On boards where the encoder button doubles as BOOT, holding it at reset
    // enters the ROM downloader, so it cannot be the wake source. Fall back to
    // the shutdown that only a power cycle undoes.
    receiverShutdown();
#else
    standbySleepEnter();
#endif
    return;
  }
  // Last minutes: go offline early instead of at the moment of the shutdown.
  if(offTimerRemainingUs() <= OFF_TIMER_QUIET_US) offTimerQuietEnter();
}

void standbyTimerTick()
{
  if(!standbyTimerDeadline) return;
  if(standbyTimerRemainingUs()) return;
  // The off timer already took Wi-Fi and Bluetooth offline for its final
  // minutes. Going into standby now would leave no way to wake the unit over
  // Bluetooth and it is about to power off anyway, so hold the deadline: if the
  // off timer is pushed past the quiet window the standby fires afterwards.
  if(offTimerQuiet) return;
  // One shot: clear the deadline before sleeping, otherwise the next loop after
  // a wake-up would drop straight back into standby. The stored duration is
  // cleared too, so the settings entry reads "off" instead of showing a
  // countdown that will never fire again.
  standbyTimerDeadline = 0;
  if(standbyTimerMinutes)
  {
    standbyTimerMinutes = 0;
    prefsRequestSave(SAVE_SETTINGS);
  }
  standbyEnter();
}

// Time
static bool clockHasBeenSet = false;

// Status is owned by the main task, including updates from network operations.
char statusLines[2][96];
static bool statusDirty = false;
static uint32_t statusStarted;
static uint32_t statusDuration;

void statusShow(const char *line1, const char *line2, uint32_t duration)
{
  strlcpy(statusLines[0], line1? line1 : "", sizeof(statusLines[0]));
  strlcpy(statusLines[1], line2? line2 : "", sizeof(statusLines[1]));
  statusStarted = millis();
  statusDuration = (statusLines[0][0] || statusLines[1][0])? duration : 0;
  statusDirty = true;
}

bool statusTick(uint32_t now)
{
  if(statusDuration && (uint32_t)(now - statusStarted) >= statusDuration)
  {
    statusLines[0][0] = statusLines[1][0] = '\0';
    statusDuration = 0;
    statusDirty = true;
  }

  bool changed = statusDirty;
  statusDirty = false;
  return(changed);
}

//
// Get firmware version and build time, as a string
//
const char *getVersion(bool shorter)
{
  static char versionString[35] = "\0";

  sprintf(versionString, "%s%sF/W: v%1.1d.%2.2d %s",
    shorter ? "" : RECEIVER_NAME,
    shorter ? "" : " ",
    VER_APP / 100,
    VER_APP % 100,
    __DATE__
  );

  return(versionString);
}

//
// HTTP User-Agent for downloads
//
String getUserAgent()
{
  return("ATS-Mini/" + String(getVersion(true)));
}

//
// Get MAC address
//
const char *getMACAddress()
{
  static char macString[20] = "\0";

  if(!macString[0])
  {
    uint64_t mac = ESP.getEfuseMac();
    sprintf(
      macString,
      "%02X:%02X:%02X:%02X:%02X:%02X",
      (uint8_t)mac,
      (uint8_t)(mac >> 8),
      (uint8_t)(mac >> 16),
      (uint8_t)(mac >> 24),
      (uint8_t)(mac >> 32),
      (uint8_t)(mac >> 40)
    );
  }
  return(macString);
}

//
// Ramp the SI4732 digital volume to the target level in small steps. Writing
// the volume property in one jump makes the amplifier reproduce the whole
// level change as a thump, so mute transitions fade instead.
//
// The ramp runs on the main loop, and doSeek() starts one per encoder detent,
// so its duration is part of the input latency: the previous 4 units per 2 ms
// spent ~40 ms on a full 0..63 sweep and swallowed detents. 8 units per ms
// keeps the transition continuous for the amplifier and costs about 10 ms.
//
void volumeFade(uint8_t target)
{
  int v = rx.getVolume();

  while(v != (int)target)
  {
    v = (target > v) ? min(v + 8, (int)target) : max(v - 8, (int)target);
    rx.setVolume(v);
    delay(1);
  }
}

//
// Mute sound on (x=1) or off (x=0), or get current status (x=2)
// With a hardware clamp, leave the amplifier powered to avoid an enable thump.
// Do not call this too often because a short PIN_AMP_EN impulse can trigger amplifier mode D,
// see the NS4160 datasheet https://esp32-si4732.github.io/ats-mini/hardware.html#datasheets
//
bool muteOn(uint8_t mode, int x, bool muteAmp)
{
  // Current mute status
  static bool muted = false;

  // Current squelch status
  static bool squelched = false;

  // Effective mute status
  static bool status = false;

  bool unmute = false;
  bool mute = false;

  if(x==1) {
    status = true;
    switch(mode) {
    case MUTE_FORCE:
      mute = true;
      break;
    case MUTE_MAIN:
      if(!muted && !squelched) {
        mute = true;
      }
      muted = true;
      break;
    case MUTE_SQUELCH:
      if(!muted && !squelched) {
        mute = true;
      }
      squelched = true;
      break;
    case MUTE_TEMP:
      if(!muted && !squelched) {
        mute = true;
      }
      break;
    }
  } else if(x==0) {
    status = false;
    switch(mode) {
    case MUTE_FORCE:
      unmute = true;
      break;
    case MUTE_MAIN:
      if(muted && !squelched) {
        unmute = true;
      }
      muted = false;
      break;
    case MUTE_SQUELCH:
      if(!muted && squelched) {
        unmute = true;
      }
      squelched = false;
      break;
    case MUTE_TEMP:
      if(!muted && !squelched) {
        unmute = true;
      }
      break;
    }
  }

  if(mute) {
    // Fade the SI4732 output down first: it is the level step that the
    // amplifier reproduces as a thump, and a short ramp turns it into a
    // soft fade.
    volumeFade(0);
    if(muteAmp && PIN_AMP_EN >= 0) digitalWrite(PIN_AMP_EN, LOW);
    if(audioClamp) digitalWrite(AUDIO_MUTE, HIGH);
    rx.setAudioMute(true);
  }

  if(unmute) {
    if(audioClamp) digitalWrite(AUDIO_MUTE, LOW);
    rx.setAudioMute(false);
    // Enable audio amplifier to restore speaker output
    if(muteAmp && PIN_AMP_EN >= 0) digitalWrite(PIN_AMP_EN, HIGH);
    // Let the amplifier input settle (coupling capacitors, enable transient)
    // in silence, then bring the volume back with the same soft ramp.
    delay(20);
    volumeFade(volume);
  }

  switch(mode) {
  case MUTE_MAIN:
    return muted;
  case MUTE_SQUELCH:
    return squelched;
  case MUTE_FORCE:
  case MUTE_TEMP:
  default:
    return status;
  }
}

//
// Everything Bluetooth standby shuts down, so it can also be applied on top of
// an already dark display (the standby timer may fire while CPU sleep is on).
//
static void bleStandbyEnter()
{
  bleStandbyMuted = !muteOn(MUTE_MAIN);
  if(bleStandbyMuted) muteOn(MUTE_MAIN, true, true);
  if(PIN_AMP_EN >= 0) digitalWrite(PIN_AMP_EN, LOW);
  netStop();
  radioStandbyEnter();
  setCpuFrequencyMhz(CPU_FREQ_SLEEP);
  bleStandbyAdvertising(true);
}

// Upper bound for waiting until the encoder button is released again. The
// panel is blanked before this wait, so a key that stays low (dirty contact,
// mechanical jam, water) would otherwise park the main loop here forever: the
// radio keeps playing, the screen stays dark and no input is processed --
// recoverable only by pulling the battery, and the watchdog cannot help
// because delay() keeps feeding it. 3 s is far longer than any real press.
#define ENCODER_RELEASE_TIMEOUT_MS 3000

// Wait for the encoder button to come back up. The debounce interval follows
// the caller: entering sleep must not be fooled by contact bounce, while
// waking wants the release seen immediately. Returns early on a stuck key; the
// caller then continues its normal sequence instead of blocking.
static void waitEncoderRelease(unsigned int debounceInterval = DEBOUNCE_INTERVAL)
{
  uint32_t started = millis();
  while(pb1.update(digitalRead(ENCODER_PUSH_BUTTON) == LOW, debounceInterval).isPressed)
  {
    if((uint32_t)(millis() - started) >= ENCODER_RELEASE_TIMEOUT_MS) return;
    delay(100);
  }
}

// Turn sleep on (1) or off (0), or get current status (2)
//
bool sleepOn(int x)
{
  if((x==1) && !sleep_on)
  {
    sleep_on = true;
    ledcWrite(PIN_LCD_BL, 0);
    spr.fillSprite(TFT_BLACK);
    spr.pushSprite(0, 0);
    tft.sleep();
    delay(120); // Allow sleep entry before a possible immediate wake.

    // Wait till the button is released to prevent immediate wakeup. Bounded:
    // a stuck key (dirty contact, mechanical jam) must not park the unit here
    // with the panel already dark and every input dead.
    waitEncoderRelease();

    if(sleepIsBleStandby())
    {
      // TV style standby: the panel, the audio path and the receiver are all
      // shut down; only the BLE radio keeps running so the app can find the
      // unit and wake it. The station state is restored on wake.
      bleStandbyEnter();
    }
    else if(sleepModeIdx == SLEEP_LIGHT)
    {
      // CPU sleep only drops the display, Wi-Fi and CPU clock: the radio keeps
      // playing, so audio is deliberately left untouched. The BLE stack also
      // stays up, so the app can reconnect or send a command that wakes us.
      netStop();
      setCpuFrequencyMhz(CPU_FREQ_SLEEP);
    }
  }
  else if((x==0) && sleep_on)
  {
    sleep_on = false;
    // Bring the CPU and the receiver back before touching audio or display.
    // CPU_FREQ_RUN, not 240: this used to hardcode 240 while the build runs at
    // 80 (sketch.yaml CPUFreq), so every wake silently tripled the clock and it
    // stayed there -- the unit's speed depended on whether it had slept since
    // power-on. Restoring the configured frequency keeps one clock per uptime.
    if(sleepModeIdx == SLEEP_LIGHT || sleepIsBleStandby())
      setCpuFrequencyMhz(CPU_FREQ_RUN);
    bleStandbyAdvertising(false);
    radioStandbyLeave();
    if(bleStandbyMuted)
    {
      // Only undo the mute we applied on entry, so a mute the user set
      // beforehand is not cleared when the display wakes up. muteOn() re-enables
      // the amplifier itself after unmuting, which keeps the order clean and
      // avoids an enable thump.
      muteOn(MUTE_MAIN, false, true);
      bleStandbyMuted = false;
    }
    // A standby started by the timer lasts until the next wake.
    standbyForced = false;
    tft.wakeup();
    delay(120); // Let the panel stabilize before redrawing and enabling the backlight.
    drawScreen();
    ledcWrite(PIN_LCD_BL, currentBrt);
    // Wait till the button is released to prevent the main loop clicks.
    // Debounce 0 on this path, as before: the click that woke the unit has
    // already been debounced, and waiting out a second interval here would
    // swallow the next press on a fast double tap.
    pb1.reset(); // Reset the button state (its timers could be stale due to CPU sleep)
    waitEncoderRelease(0);
  }

  return(sleep_on);
}

//
// Wake up and leave the sleep mode on "CPU sleep" (SLEEP_LIGHT).
//
// "BLE standby" is meant to be a one-shot state, but it *is* a selectable sleep
// mode: once it is stored, every later long press mutes the radio and powers
// the SI4732 down. Waking therefore drops the mode back to CPU sleep -- display
// off, radio still playing, Bluetooth still reachable -- which is what a long
// press is expected to do. Without this the unit could stay stuck in standby
// (an older build also wrote SLEEP_BLE_KEEPALIVE whenever the app sent 'Z').
//
// Waking first matters: sleepOn(false) decides what to restore from the
// *current* mode, so switching the mode beforehand would skip the receiver
// bring-up and leave the unit silent.
//
bool sleepWakeLight()
{
  sleepOn(false);
  if(sleepModeIdx != SLEEP_LIGHT)
  {
    sleepModeIdx = SLEEP_LIGHT;
    prefsRequestSave(SAVE_SETTINGS);
  }
  return(true);
}

//
// Enter Bluetooth standby now, whatever the configured sleep mode is. Used by
// the standby timer; the override is dropped on the next wake.
//
bool standbyEnter()
{
  standbyForced = true;
  if(!sleepOn())
  {
    sleepOn(true);
    return(true);
  }
  // Already dark in another mode: apply standby on top without flashing the
  // panel, so a running CPU sleep turns into a power saving standby.
  if(!radioInStandby()) bleStandbyEnter();
  return(true);
}

//
// Set and count time
//

bool clockAvailable()
{
  return(clockHasBeenSet);
}

const char *clockGet()
{
  static char text[6];
  if(switchThemeEditor()) return("00:00");
  if(!clockHasBeenSet) return(NULL);

  time_t now = time(NULL) + getCurrentUTCOffset() * 15 * 60;
  struct tm tm;
  gmtime_r(&now, &tm);
  sprintf(text, "%02d:%02d", tm.tm_hour, tm.tm_min);
  return(text);
}

bool clockGetHM(uint8_t *hours, uint8_t *minutes)
{
  if(!clockHasBeenSet) return(false);

  time_t now = time(NULL);
  struct tm tm;
  gmtime_r(&now, &tm);

  if(hours) *hours = tm.tm_hour;
  if(minutes) *minutes = tm.tm_min;
  return(true);
}

bool clockGetDate(uint16_t *year, uint8_t *month, uint8_t *day, uint8_t *weekday)
{
  if(!clockHasBeenSet) return(false);

  time_t now = time(NULL);
  struct tm tm;
  gmtime_r(&now, &tm);
  if(tm.tm_year + 1900 < CLOCK_MIN_YEAR) return(false);

  now += getCurrentUTCOffset() * 15 * 60;
  gmtime_r(&now, &tm);

  if(year) *year = tm.tm_year + 1900;
  if(month) *month = tm.tm_mon + 1;
  if(day) *day = tm.tm_mday;
  if(weekday) *weekday = tm.tm_wday == 0? 7 : tm.tm_wday;
  return(true);
}

bool clockUTCDateTimeToEpoch(int year, int month, int day, int hour, int minute, int second, uint32_t *epoch)
{
  if(!epoch || year < CLOCK_MIN_YEAR) return(false);

  struct tm fields = {};
  fields.tm_year = year - 1900;
  fields.tm_mon = month - 1;
  fields.tm_mday = day;
  fields.tm_hour = hour;
  fields.tm_min = minute;
  fields.tm_sec = second;

  time_t value = mktime(&fields);
  if(value < 0 || value > UINT32_MAX ||
     fields.tm_year != year - 1900 || fields.tm_mon != month - 1 || fields.tm_mday != day ||
     fields.tm_hour != hour || fields.tm_min != minute || fields.tm_sec != second)
    return(false);

  *epoch = value;
  return(true);
}

// SNTP has already updated system time; do not set it again from a stale sample.
void clockTimeSynchronized()
{
  clockHasBeenSet = true;
  identifyFrequency(currentFrequency);
}

void clockReset()
{
  clockHasBeenSet = false;
}

bool clockUpdate()
{
  static time_t lastMinute = -1;
  if(!clockHasBeenSet) return(false);

  time_t minute = time(NULL) / 60;
  if(minute == lastMinute) return(false);

  lastMinute = minute;
  return(true);
}

bool clockSetEpoch(uint32_t epoch)
{
  time_t now = epoch;
  bool changed = !clockHasBeenSet || time(NULL) / 60 != now / 60;
  struct timeval tv = { now, 0 };

  if(settimeofday(&tv, NULL)) return(false);
  clockHasBeenSet = true;

  if(changed)
    identifyFrequency(currentFrequency);

  return(changed);
}

//
// Check if given frequency in Hz belongs to given band
//
bool isFreqInBand(const Band *band, uint32_t freqHz)
{
  return((freqHz>=band->minimumFreq) && (freqHz<=band->maximumFreq));
}

//
// Convert a frequency from Hz to mode-specific units
//
uint16_t freqFromHz(uint32_t freq, uint8_t mode)
{
  return(mode == FM ? freq / 10000 : freq / 1000);
}

//
// Convert a frequency from mode-specific units to Hz
//
uint32_t freqToHz(uint16_t freq, uint8_t mode)
{
  return(mode == FM ? freq * 10000 : freq * 1000);
}

//
// Get S-level signal strength from RSSI value
//
int getStrength(int rssi)
{
  // Preview normal, strong-signal and empty bar colors together.
  if(switchThemeEditor()) return(13);

  if(currentMode!=FM)
  {
    // dBuV to S point conversion HF
    if (rssi <=  1) return  1; // S0
    if (rssi <=  2) return  2; // S1
    if (rssi <=  3) return  3; // S2
    if (rssi <=  4) return  4; // S3
    if (rssi <= 10) return  5; // S4
    if (rssi <= 16) return  6; // S5
    if (rssi <= 22) return  7; // S6
    if (rssi <= 28) return  8; // S7
    if (rssi <= 34) return  9; // S8
    if (rssi <= 44) return 10; // S9
    if (rssi <= 54) return 11; // S9 +10
    if (rssi <= 64) return 12; // S9 +20
    if (rssi <= 74) return 13; // S9 +30
    if (rssi <= 84) return 14; // S9 +40
    if (rssi <= 94) return 15; // S9 +50
    if (rssi <= 95) return 16; // S9 +60
    return                 17; //>S9 +60
  }
  else
  {
    // dBuV to S point conversion FM
    if (rssi <=  1) return  1; // S0
    if (rssi <=  2) return  7; // S6
    if (rssi <=  8) return  8; // S7
    if (rssi <= 14) return  9; // S8
    if (rssi <= 24) return 10; // S9
    if (rssi <= 34) return 11; // S9 +10
    if (rssi <= 44) return 12; // S9 +20
    if (rssi <= 54) return 13; // S9 +30
    if (rssi <= 64) return 14; // S9 +40
    if (rssi <= 74) return 15; // S9 +50
    if (rssi <= 76) return 16; // S9 +60
    return                 17; //>S9 +60
  }
}
