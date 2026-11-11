#include "Common.h"
#include "Themes.h"
#include "Utils.h"

#define VBAT_MON  4                 // GPIO04 -- Battery Monitor PIN (ADC1 channel 3)

//
// Hardware, see docs/source/hardware.md and the schematics:
//
//   VBAT --- R8 (100k) ---+--- BAT_ADC (GPIO4)
//                         |
//                      R10 (100k)
//                         |
//                        GND          C12 (10nF) from BAT_ADC to GND
//
// So the pin sees VBAT / 2 and the firmware has to multiply back by (R8+R10)/R10.
//
// The nominal ratio is 2.0 (R8 = R10 = 100k). The original ats-mini board was
// calibrated against a meter at 1.83, but clone boards use the exact nominal
// divider, where 1.83 reads every voltage ~8.5% low: the USB rail (~4.4 V)
// then falls below the 4.3 V charging threshold, so the charging icon never
// appears, and a 3.7 V cell is displayed as 3.42 V. The default is therefore
// the nominal 2.0.
//
// To recalibrate a unit, measure the cell with a meter while the receiver is
// idle and set this to 2.0 * (meter volts / displayed volts).
#define BATT_DIVIDER         2.00f  // (R8 + R10) / R10, nominal 100k/100k
// Nine samples go into every measurement: two conversions are burnt after the
// channel switch, the two lowest and the two highest of the remaining thirteen
// are dropped, and the middle nine are averaged. Nine averaged samples reject
// the switching spikes of the backlight and the amplifier that a single
// conversion follows, and the whole burst is still only ~1.8 ms once per 500 ms,
// so the encoder (which counts in an interrupt) is not affected.
#define BATT_ADC_READS         15   // Conversions per measurement (must be <= 32)
#define BATT_ADC_WARMUP         2   // First samples after the channel switch are junk
#define BATT_ADC_TRIMMED        2   // Drop this many lowest and highest samples
// Sampling blocks the main loop, which also polls the rotary encoder, so the
// burst is kept to ~1.8 ms and runs once per BATT_SAMPLE_INTERVAL only. The
// divider is only 50k into 10nF (tau = 0.5 ms) and the cell voltage drifts far
// slower than that, so a long settle time buys nothing here.
#define BATT_ADC_SPACING_US   120
#define BATT_FILTER_ALPHA    0.30f  // IIR weight of a new measurement (0..1)
#define BATT_FILTER_ALPHA_SLOW 0.02f  // IIR weight of a new measurement in the slow SOC path (0..1)
#define BATT_SAMPLE_INTERVAL  500   // ms between real measurements
#define BATT_DISPLAY_HYST    0.01f  // Volts; keeps the last digit from flickering

// The bar has nine levels, i.e. ten states from empty to full, so it moves in
// ~10% steps instead of the 25% jumps of the old four bucket display.
#define BATT_SOC_LEVELS        9

// Level thresholds on a realistic Li-ion open circuit curve, in ascending
// order: battSocLevel[i] is the voltage where level i+1 begins, so the level
// is the number of thresholds the cell voltage is above. The mid points keep
// the previous calibration (3.74 V = 25%, 3.87 V = 50%, 4.00 V = 75%), which
// was measured with the receiver playing rather than at rest.
static const float battSocLevel[BATT_SOC_LEVELS] =
{
  3.620f,  // 10%
  3.680f,  // 20%
  3.750f,  // 30%
  3.810f,  // 40%
  3.870f,  // 50%
  3.935f,  // 60%
  4.000f,  // 70%
  4.060f,  // 80%
  4.120f,  // 90%
};

// Full width of the hysteresis band. A level only changes when the slow average
// leaves the band around the boundary, i.e. when it is half of this away on
// either side. The narrowest gap between two thresholds is 60 mV, so the bands
// of neighbouring levels can never overlap.
#define BATT_SOC_HYST        0.030

// A single cell never reads outside this window at the ADC pin. Anything else
// means the ADC is not connected/not settled yet, so the reading is dropped
// instead of being displayed as a plausible but wrong voltage.
#define BATT_VALID_MIN       1.000f
#define BATT_VALID_MAX       2.600f

// USB present: with the load sharing circuit VBUS minus one diode drop sits on
// the cell rail, well above what a single Li-ion cell can reach. The test is
// done on the battery scale (after the divider factor), so a full cell cannot
// be mistaken for a charging one.
#define BATT_USB_THRESHOLD   4.300f

// State machine used for battery state of charge (SOC) detection with
// hysteresis (Default = Illegal state)
static uint8_t batteryState = 255;

// Current battery voltage (filtered)
static float batteryVolts = 4.0;

// Slowly tracked cell voltage that drives the SOC state machine. A transient
// load (Wi-Fi burst, backlight step) moves the terminal voltage for a second
// or two, but the state of charge does not change at that pace, so the bars
// follow this average instead of the display voltage.
static float batterySlow = 4.0;

// Whether batterySlow holds a real measurement (not the 4.0 V default)
static bool slowReady = false;

// Last value handed to the display, with hysteresis applied
static float batteryShown = 0.0;

// Whether the USB supply is present (charging indicator)
static bool batteryUsb = false;

// Measurement state
static bool     batteryReady = false;
static uint32_t batteryLastMs = 0;

//
// Map a voltage to a bar level (0..BATT_SOC_LEVELS). Used both by the state
// machine and to seed it, so that the very first screen after power on already
// shows the right number of bars instead of starting at empty.
//
static uint8_t batteryLevelFor(float volts)
{
  uint8_t level = 0;
  while(level < BATT_SOC_LEVELS && volts > battSocLevel[level]) level++;
  return(level);
}

//
// One calibrated measurement, in volts at the ADC pin (before the divider).
//
static float batteryReadOnce()
{
  uint32_t mv[BATT_ADC_READS];
  uint8_t  i, j, n = 0;

  // analogReadMilliVolts() runs the value through the per-chip ADC
  // calibration burnt into eFuse, which is what makes the absolute voltage
  // trustworthy. analogRead() returns raw counts that drift with chip and
  // temperature, which is why the old code needed a magic 1.702 factor.
  for(i = 0; i < BATT_ADC_READS; i++)
  {
    uint32_t sample = analogReadMilliVolts(VBAT_MON);

    // Burn the first few conversions: the sample-and-hold capacitor and C12
    // still carry charge from the previous channel when the pin is switched.
    if(i >= BATT_ADC_WARMUP) mv[n++] = sample;

    delayMicroseconds(BATT_ADC_SPACING_US);
  }

  // Too few usable samples means the burst failed. Report 0 V, the caller's
  // validity window rejects it: returning the filtered cell voltage here would
  // mix two scales, because this function answers in volts at the ADC pin.
  if(n < (2 * BATT_ADC_TRIMMED + 1)) return(0.0f);

  // Insertion sort, the array holds at most BATT_ADC_READS - BATT_ADC_WARMUP
  // entries
  for(i = 1; i < n; i++)
  {
    uint32_t key = mv[i];
    for(j = i; j > 0 && mv[j - 1] > key; j--) mv[j] = mv[j - 1];
    mv[j] = key;
  }

  // Mean of the middle samples: kills the switching regulator spikes from
  // the LCD backlight and the audio amplifier without slowing the response.
  uint32_t sum = 0;
  uint8_t  lo = BATT_ADC_TRIMMED, hi = n - BATT_ADC_TRIMMED;
  for(i = lo; i < hi; i++) sum += mv[i];

  return ((float)sum / (float)(hi - lo)) / 1000.0f;
}

//
// Measure and return battery voltage
//
float batteryMonitor()
{
  static bool adcConfigured = false;

  if(!adcConfigured)
  {
    // 12 bits is the default, but ask explicitly: the calibration table used
    // by analogReadMilliVolts() depends on resolution and attenuation.
    analogReadResolution(12);
    // 11 dB attenuation is required here: the divider halves the cell, but
    // 4.2 V still arrives as 2.1 V, above the 0 dB / 2.5 dB ranges.
    //
    // Both calls are needed. analogSetPinAttenuation() only configures the
    // hardware range of this channel, while analogReadMilliVolts() builds its
    // calibration table from the *global* attenuation: with the two out of
    // step every reading is off by a fixed percentage, which looks exactly like
    // a miscalibrated divider.
    analogSetAttenuation(ADC_11db);
    analogSetPinAttenuation(VBAT_MON, ADC_11db);
    adcConfigured = true;
  }

  uint32_t now = millis();

  // Both the status stream (every 200 ms) and the layout (every redraw) ask
  // for the voltage. Sample once per interval and hand out the cached value
  // in between, so the display does not jitter with every frame.
  if(batteryReady && (uint32_t)(now - batteryLastMs) < BATT_SAMPLE_INTERVAL)
    return batteryVolts;

  batteryLastMs = now;

  float pin = batteryReadOnce();

  // Reject impossible readings, keep the previous value
  if(pin < BATT_VALID_MIN || pin > BATT_VALID_MAX) return batteryVolts;

  // Cell voltage, i.e. the ADC pin multiplied back by the divider ratio.
  float raw = pin * BATT_DIVIDER;

  // With USB connected the rail is VBUS minus one diode drop, which says
  // nothing about the cell. Track it in the fast filter only (the charging
  // indicator keys off it) and freeze the slow SOC path, so unplugging resumes
  // from the last real cell voltage.
  static bool usbPrev = false;
  bool charging = raw > BATT_USB_THRESHOLD;
  bool unplugged = usbPrev && !charging;
  batteryUsb = usbPrev = charging;

  if(!batteryReady)
  {
    batteryVolts = raw;
    batteryReady = true;
  }
  else
  {
    // Exponential filter: responds in about three measurements (~1.5 s)
    // but a single noisy conversion cannot move the bars.
    batteryVolts += (raw - batteryVolts) * BATT_FILTER_ALPHA;
  }

  if(charging) return batteryVolts;

  // The sample right after the cable is pulled is the first real cell reading.
  // The fast filter is still parked on the USB rail (~4.4 V) and would take
  // several measurements to come down, so the display would linger near 4.4 V
  // (and the status stream would report it) for seconds. Snap everything to
  // this reading: the fast filter, and also the slow path and the state
  // machine, because charging forced the level to full and the tracked slow
  // average is either stale (plugged for a long time) or one behind reality.
  // Re-seeding here keeps the bars from walking down from 100% level by level.
  if(unplugged)
  {
    batteryVolts = raw;
    batterySlow  = raw;
    slowReady    = true;
    batteryState = batteryLevelFor(batterySlow);
    return batteryVolts;
  }

  if(!slowReady)
  {
    batterySlow = batteryVolts;
    slowReady = true;
  }
  else
  {
    // The bars trail the display voltage by design: at this rate the average
    // needs minutes to cross a level, so neither a load transient nor a
    // single good/bad measurement can step them.
    batterySlow += (batteryVolts - batterySlow) * BATT_FILTER_ALPHA_SLOW;
  }

  // State machine over the nine levels. It moves one level per measurement and
  // only when the slow average leaves the hysteresis band around the boundary,
  // so a cell sitting on a threshold cannot make the bar chatter.
  if(batteryState > BATT_SOC_LEVELS)
  {
    // First measurement after power on: start where the voltage says we are
    batteryState = batteryLevelFor(batterySlow);
  }
  else if(batteryState < BATT_SOC_LEVELS && batterySlow > (battSocLevel[batteryState] + BATT_SOC_HYST))
  {
    batteryState++;
  }
  else if(batteryState > 0 && batterySlow < (battSocLevel[batteryState - 1] - BATT_SOC_HYST))
  {
    batteryState--;
  }

  // Return current voltage
  return(batteryVolts);
}

//
// Whether a USB supply is present. The value is refreshed by batteryMonitor(),
// which is throttled to BATT_SAMPLE_INTERVAL and returns the cached state in
// between, so the caller may poll this from the main loop.
//
bool batteryUsbPresent()
{
  batteryMonitor();
  return batteryUsb;
}

//
// Show last measured battery voltage and status at given screen
// coordinates. Return true if voltage was drawn.
//
bool drawBattery(int x, int y)
{
  if(sleepOn()) return false;

  // Measure battery voltage and status
  batteryMonitor();

  // Set display information
  spr.drawRoundRect(x, y + 1, 28, 14, 3, TH.batt_border);
  spr.drawLine(x + 29, y + 5, x + 29, y + 10, TH.batt_border);
  spr.drawLine(x + 30, y + 6, x + 30, y + 9, TH.batt_border);

  spr.setTextDatum(TR_DATUM);
  spr.setTextColor(TH.batt_voltage);

  // Theme editor preview: the nine bar levels plus the charging icon, one every
  // 5 seconds. Kept in locals so that leaving the editor resumes from the real
  // measurement: writing the preview into batteryState/batteryVolts parks the
  // IIR filter and the SOC state machine on 4.5 V, and the bars then read
  // "full" for a while after.
  const bool preview = switchThemeEditor();
  const uint8_t previewSlot = (uint8_t)((millis() % 50000u) / 5000u);
  const bool shownUsb = batteryUsb || (preview && previewSlot >= BATT_SOC_LEVELS);
  const uint8_t shownState = preview ? (previewSlot < BATT_SOC_LEVELS ? previewSlot : 0) : batteryState;
  const float shownVolts = preview ? (shownUsb ? 4.5f : 4.0f) : batteryVolts;

  // The hardware has a load sharing circuit to allow simultaneous charge and power
  // With USB(5V) connected the voltage reading will be approx. VBUS - Diode Drop = 4.65V
  // If the measured cell voltage is greater than 4.3V, show ligtning on the display
  if(shownUsb)
  {
    // Charging: the cell is being topped up, so it is by definition full enough.
    // Only a real USB supply may move the state machine; the preview must not.
    if(batteryUsb) batteryState = BATT_SOC_LEVELS;

    spr.fillRoundRect(x + 2, y + 3, 24, 10, 2, TH.batt_charge);
    spr.drawLine(x + 9 + 8, y + 1, x + 9 + 6, y + 1 + 5, TH.bg);
    spr.drawLine(x + 9 + 6, y + 1 + 5, x + 9 + 10, y + 1 + 5, TH.bg);
    spr.drawLine(x + 9 + 11, y + 1 + 6, x + 9 + 4, y + 1 + 13, TH.bg);
    spr.drawLine(x + 9 + 2, y + 1 + 13, x + 9 + 4, y + 1 + 8, TH.bg);
    spr.drawLine(x + 9 + 4, y + 1 + 8, x + 9 + 0, y + 1 + 8, TH.bg);
    spr.drawLine(x + 9 - 1, y + 1 + 7, x + 9 + 6, y + 1 + 0, TH.bg);
    spr.fillTriangle(x + 9 + 7, y + 1, x + 9 + 4, y + 1 + 6, x + 9, y + 1 + 7, TH.batt_icon);
    spr.fillTriangle(x + 9 + 5, y + 1 + 6, x + 9 + 10, y + 1 + 6, x + 9 + 3, y + 1 + 13, TH.batt_icon);
    spr.fillRect(x + 9 + 1, y + 1 + 6, 9, 2, TH.batt_icon);
    spr.drawPixel(x + 9 + 3, y + 1 + 12, TH.batt_icon);
    return false;
  }
  else
  {
    char voltage[8];

    // Keep the last digit stable: only move the shown value when the filtered
    // voltage really moved by 10 mV, otherwise "4.06V" flickers to "4.05V"
    // and back on every redraw.
    if(batteryShown <= 0.0f || fabsf(shownVolts - batteryShown) >= BATT_DISPLAY_HYST)
      batteryShown = shownVolts;

    // Text representation of the voltage
    sprintf(voltage, "%.02fV", batteryShown);

    // Battery bar: nine steps across the 24 px interior, rounded up so that a
    // non-empty level always puts ink on the panel (24 * 1 / 9 = 2.7 px). The
    // empty level draws nothing, leaving just the outline.
    const int level = ((24 * (int)shownState) + BATT_SOC_LEVELS - 1) / BATT_SOC_LEVELS;
    const uint16_t color = shownState <= 1 ? TH.batt_low : TH.batt_full;

    if(level > 0) spr.fillRoundRect(x + 2, y + 3, level, 10, 2, color);
    spr.drawString(voltage, x - 3, y, FONT_SMALL);
    return true;
  }
}
