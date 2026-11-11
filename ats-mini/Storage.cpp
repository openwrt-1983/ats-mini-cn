#include "Common.h"
#include "Storage.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Memories.h"
#include <LittleFS.h>
#include "nvs_flash.h"

// Time of inactivity to start writing preferences
#define STORE_TIME    10000

// Preferences saved here
Preferences prefs;

//
// NVS is shared with other firmware versions and can be left half written by a
// power loss, so a stored index may point outside the table it is used with.
// Read it as an index only when it is in range, otherwise keep the default.
//
static uint8_t loadIndex(const char *key, uint8_t fallback, int total)
{
  uint8_t value = prefs.getUChar(key, fallback);
  if(value < total) return(value);
  return(fallback < total? fallback : 0);
}

// Same idea for the SSB calibration values.
static int16_t clampCal(int16_t value)
{
  if(value < -MAX_CAL) return(-MAX_CAL);
  if(value >  MAX_CAL) return( MAX_CAL);
  return(value);
}

static uint32_t itIsTimeToSave = 0;       // Preferences to save, or 0 for none
static bool savingPrefsFlag    = false;   // TRUE: Saving preferences
static uint32_t storeTime      = millis();

// To store any change to preferences, we need at least STORE_TIME
// milliseconds of inactivity.
// Saves are requested from the receiver task (menu, encoder) and from the web
// task (HTTP handlers). Without a lock an |= from the web task can be dropped
// by the clear below, so the request is lost and the change never reaches NVS.
static portMUX_TYPE prefsMux = portMUX_INITIALIZER_UNLOCKED;

void prefsRequestSave(uint32_t what, bool now)
{
  portENTER_CRITICAL(&prefsMux);
  // Underflow is ok here, see prefsTickTime()
  storeTime = millis() - (now? STORE_TIME : 0);
  itIsTimeToSave |= what;
  portEXIT_CRITICAL(&prefsMux);
}

void prefsTickTime()
{
  uint32_t items;

  // Take the request under the lock, then write to NVS outside of it: a flash
  // write takes milliseconds and must not block the other task's request.
  portENTER_CRITICAL(&prefsMux);
  items = itIsTimeToSave && ((millis() - storeTime) >= STORE_TIME)? itIsTimeToSave : 0;
  if(items) itIsTimeToSave = 0;
  portEXIT_CRITICAL(&prefsMux);

  if(items)
  {
    prefsSave(items);

    portENTER_CRITICAL(&prefsMux);
    // A request that arrived while writing keeps its own timestamp
    if(!itIsTimeToSave) storeTime = millis();
    portEXIT_CRITICAL(&prefsMux);
  }
}

// Return true if preferences have been written
bool prefsAreWritten()
{
  bool result = savingPrefsFlag;
  savingPrefsFlag = false;
  return(result);
}

// Invlaidate all currently saved preferences
void prefsInvalidate()
{
  static const char *sections[] =
  { "settings", "memories", "bands", "network", 0 };

  // Clear all applicable sections
  for(int j = 0 ; sections[j] ; ++j)
  {
    prefs.begin(sections[j], false, STORAGE_PARTITION);
    prefs.clear();
    prefs.end();
  }
}

struct SavedBand
{
  uint32_t currentFreq;   // Current frequency in Hz
  uint8_t bandMode;       // Band mode (FM, AM, LSB, or USB)
  int8_t currentStepIdx;  // Current frequency step
  int8_t bandwidthIdx;    // Index of the table bandwidthFM, bandwidthAM or bandwidthSSB;
  int16_t usbCal;         // USB calibration value
  int16_t lsbCal;         // LSB calibration value
};

void prefsSaveBand(uint8_t idx, bool openPrefs)
{
  SavedBand value;
  char name[32];

  // Will be saving to bands
  if(openPrefs) prefs.begin("bands", false, STORAGE_PARTITION);

  // Compose preference name and value
  sprintf(name, "Band-%d", idx);
  value.currentFreq    = bands[idx].currentFreq;     // Frequency
  value.bandMode       = bands[idx].bandMode;        // Modulation
  value.currentStepIdx = bands[idx].currentStepIdx;  // Step
  value.bandwidthIdx   = bands[idx].bandwidthIdx;    // Bandwidth
  value.usbCal         = bands[idx].usbCal;          // USB Calibration
  value.lsbCal         = bands[idx].lsbCal;          // LSB Calibration

  // Write a preference
  prefs.putBytes(name, &value, sizeof(value));

  // Done with band preferences
  if(openPrefs) prefs.end();
}

bool prefsLoadBand(uint8_t idx, bool openPrefs)
{
  SavedBand value;
  char name[32];

  // Will be loading from bands
  if(openPrefs) prefs.begin("bands", true, STORAGE_PARTITION);

  // Compose preference name
  sprintf(name, "Band-%d", idx);

  // Read preference
  bool result = prefs.getBytes(name, &value, sizeof(value))==sizeof(value);
  if(result)
  {
    // Keep only the values that still fit this firmware's tables. A band mode
    // outside the mode table, or a frequency outside the band, would be sent
    // straight to the receiver and index other tables further down.
    if(value.currentFreq >= bands[idx].minimumFreq &&
       value.currentFreq <= bands[idx].maximumFreq)
      bands[idx].currentFreq = value.currentFreq;

    if(value.bandMode < getTotalModes())
      bands[idx].bandMode = value.bandMode;

    if(value.currentStepIdx >= 0)
      bands[idx].currentStepIdx = value.currentStepIdx;

    bands[idx].bandwidthIdx = value.bandwidthIdx;     // Checked again on use
    bands[idx].usbCal       = clampCal(value.usbCal);
    bands[idx].lsbCal       = clampCal(value.lsbCal);
  }

  // Done with band preferences
  if(openPrefs) prefs.end();

  // Done
  return(result);
}

//
// A memory name is UTF-8 text: printable ASCII or any multi-byte sequence.
// Control characters and overlong/truncated encodings are rejected so the
// display never receives a half character.
//
static bool isValidName(const char *text, size_t size)
{
  size_t i = 0;
  bool terminated = false;

  while(i < size)
  {
    unsigned char c = (unsigned char)text[i];
    if(!c)
    {
      terminated = true;
      break;
    }

    if(c < 0x80)
    {
      if(c < 0x20 || c == 0x7f) return(false);
      i++;
      continue;
    }

    size_t length;
    uint32_t code;
    if((c & 0xe0) == 0xc0)      { length = 2; code = c & 0x1f; }
    else if((c & 0xf0) == 0xe0) { length = 3; code = c & 0x0f; }
    else if((c & 0xf8) == 0xf0) { length = 4; code = c & 0x07; }
    else return(false);

    if(i + length > size) return(false);
    for(size_t k = 1; k < length; k++)
    {
      unsigned char next = (unsigned char)text[i + k];
      if((next & 0xc0) != 0x80) return(false);
      code = (code << 6) | (next & 0x3f);
    }

    // Reject overlong forms, surrogates and out-of-range values.
    if((length == 2 && code < 0x80) ||
       (length == 3 && code < 0x800) ||
       (length == 4 && code < 0x10000) ||
       (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff)
      return(false);
    i += length;
  }

  return(terminated);
}

// Layout used before names could hold Chinese; kept so an existing receiver
// keeps its 99 slots after the upgrade.
typedef struct __attribute__((packed))
{
  uint32_t freq;
  uint8_t  band;
  uint8_t  mode;
  char     name[10];
} LegacyMemory;

void prefsSaveMemory(uint8_t idx, bool openPrefs)
{
  char name[32];

  // Will be saving to memories
  if(openPrefs) prefs.begin("memories", false, STORAGE_PARTITION);

  // Compose preference name
  sprintf(name, "Memory-%d", idx);

  // Write a preference
  Memory memory = getMemory(idx);
  prefs.putBytes(name, &memory, sizeof(memory));

  // Done with memory preferences
  if(openPrefs) prefs.end();
}

bool prefsLoadMemory(uint8_t idx, bool openPrefs)
{
  char name[32];

  // Will be loading from memories
  if(openPrefs) prefs.begin("memories", true, STORAGE_PARTITION);

  // Compose preference name
  sprintf(name, "Memory-%d", idx);

  // Read a preference
  Memory memory = {};
  bool result = !!prefs.getBytes(name, &memory, sizeof(memory));

  if(!result && prefs.getBytesLength(name)==sizeof(LegacyMemory))
  {
    // Slot written by a firmware that only stored ASCII names.
    LegacyMemory legacy = {};
    if(prefs.getBytes(name, &legacy, sizeof(legacy))==sizeof(legacy))
    {
      memory.freq = legacy.freq;
      memory.band = legacy.band;
      memory.mode = legacy.mode;
      memcpy(memory.name, legacy.name, sizeof(legacy.name));
      memory.name[sizeof(legacy.name)] = '\0';
      result = true;
    }
  }

  if(result)
  {
    if(!isValidName(memory.name, sizeof(memory.name)))
      memset(memory.name, 0, sizeof(memory.name));

    // A slot written by another firmware or damaged by a power loss must never
    // reach the band tables: tuneToMemory() copies these fields into the live
    // band, and an unknown mode or band indexes tables out of range. Drop the
    // whole record, leaving the slot empty.
    if(memory.band >= getTotalBands() || memory.mode >= getTotalModes())
      memory = {};

    setMemory(idx, memory);
  }

  // Done with memory preferences
  if(openPrefs) prefs.end();

  // Done
  return(result);
}

void prefsSave(uint32_t items)
{
  if(items & SAVE_SETTINGS)
  {
    // Will be saving to settings
    prefs.begin("settings", false, STORAGE_PARTITION);

    // Save main global settings
    prefs.putUChar("Version",  VER_SETTINGS);      // Settings version
    prefs.putUShort("App",     VER_APP);           // Application version
    prefs.putUChar("Volume",   volume);            // Current volume
    prefs.putUChar("Band",     bandIdx);           // Current band
    prefs.putUChar("WiFiMode", wifiModeIdx);       // WiFi connection mode

    // Save additional global settings
    prefs.putUShort("Brightness", currentBrt);     // Brightness
    prefs.putUChar("FmAGC",       FmAgcIdx);       // FM AGC/ATTN
    prefs.putUChar("AmAGC",       AmAgcIdx);       // AM AGC/ATTN
    prefs.putUChar("SsbAGC",      SsbAgcIdx);      // SSB AGC/ATTN
    prefs.putUChar("AmAVC",       AmAvcIdx);       // AM AVC
    prefs.putUChar("SsbAVC",      SsbAvcIdx);      // SSB AVC
    prefs.putUChar("AmSoftMute",  AmSoftMuteIdx);  // AM soft mute
    prefs.putUChar("SsbSoftMute", SsbSoftMuteIdx); // SSB soft mute
    prefs.putUShort("Sleep",      currentSleep);   // Sleep delay
    prefs.putUShort("OffTimer", offTimerMinutes);
    prefs.putUShort("StbyTimer", standbyTimerMinutes);
    prefs.putUChar("Theme",       themeIdx);       // Color theme
    prefs.putUChar("RDSMode",     rdsModeIdx);     // RDS mode
    prefs.putUChar("SleepMode",   sleepModeIdx);   // Sleep mode
    prefs.putUChar("ZoomMenu",    zoomMenu);       // TRUE: Zoom menu
    prefs.putBool("ScrollDir", scrollDirection<0); // TRUE: Reverse scroll
    prefs.putBool("EncHalfStep", encoderHalfStep); // TRUE: Encoder half-step mode
    prefs.putUChar("UTCOffset",   utcOffsetIdx);   // UTC Offset
    prefs.putUInt("Squelch",      ((uint32_t)currentSquelch[FM]) |
                                  ((uint32_t)currentSquelch[LSB] << 8) |
                                  ((uint32_t)currentSquelch[USB] << 16) |
                                  ((uint32_t)currentSquelch[AM] << 24)); // Squelch
    prefs.putUChar("FmRegion",    FmRegionIdx);    // FM region
    // Stored under a new key: the list gained "立体声" in the middle, so the
    // value 1 used to mean mono and now means stereo. See the migration in
    // loadSettings().
    prefs.putUChar("FmStereo2",   fmStereoIdx);    // FM stereo mode
    prefs.putBool("AudioClamp",   audioClamp);     // Hardware audio clamp present
    prefs.putUChar("DSPPatchset", dspPatchesIdx);  // DSP patches
    prefs.putUChar("UILayout",    uiLayoutIdx);    // UI Layout
    prefs.putUChar("BLEMode",     bleModeIdx);     // Bluetooth mode
    prefs.putUChar("USBMode",     usbModeIdx);     // USB mode
    prefs.putUChar("TCPMode",     tcpModeIdx);     // TCP mode

    // Done with global settings
    prefs.end();
  }

  if(items & (SAVE_BANDS | SAVE_CUR_BAND))
  {
    prefs.begin("bands", false, STORAGE_PARTITION);
    if(items & SAVE_BANDS)
    {
      prefs.putUChar("Version", VER_BANDS);
      for(int i=0 ; i<getTotalBands() ; i++) prefsSaveBand(i, false);
    }
    else
      prefsSaveBand(bandIdx, false);

    // Startup may save defaults before the receiver is ready to capture.
    if(vfoInitialized && vfoIdx < ITEM_COUNT(vfos)) vfos[vfoIdx] = captureVfo();
    prefs.putBytes("VFOSlots", vfos, sizeof(vfos));
    prefs.putUChar("VFOActive", vfoIdx);
    prefs.end();
  }

  if(items & SAVE_MEMORIES)
  {
    // Will be saving to memories
    prefs.begin("memories", false, STORAGE_PARTITION);
    prefs.putUChar("Version", VER_MEMORIES);
    // Save current memories
    for(int i=0 ; i<getTotalMemories() ; i++) prefsSaveMemory(i, false);
    // Done with memories
    prefs.end();
  }

  // Preferences have been saved
  savingPrefsFlag = true;
}

bool prefsLoad(uint32_t items)
{
  if(items & SAVE_SETTINGS)
  {
    // Will be loading from settings
    prefs.begin("settings", true, STORAGE_PARTITION);

    // A section written by a newer firmware uses keys this build does not
    // know, and a missing section means "use defaults", so only those two cases
    // are refused. An older section is migrated key by key below instead of
    // being thrown away: every key falls back to its current value.
    uint8_t version = prefs.getUChar("Version", 0);
    if((items & SAVE_VERIFY) && (version == 0 || version > VER_SETTINGS))
    {
      prefs.end();
      return(false);
    }

    // Load main global settings
    volume         = prefs.getUChar("Volume", volume);          // Current volume
    if(volume > 63) volume = 63;                                // Six bit volume
    bandIdx        = loadIndex("Band", bandIdx, getTotalBands());
    wifiModeIdx    = loadIndex("WiFiMode", wifiModeIdx, getTotalWiFiModes());
    currentBrt     = prefs.getUShort("Brightness", currentBrt); // Brightness
    if(currentBrt < 10) currentBrt = 10;                        // Never blank the screen
    FmAgcIdx       = prefs.getUChar("FmAGC", FmAgcIdx);         // FM AGC/ATTN
    AmAgcIdx       = prefs.getUChar("AmAGC", AmAgcIdx);         // AM AGC/ATTN
    SsbAgcIdx      = prefs.getUChar("SsbAGC", SsbAgcIdx);       // SSB AGC/ATTN
    AmAvcIdx       = prefs.getUChar("AmAVC", AmAvcIdx);         // AM AVC
    SsbAvcIdx      = prefs.getUChar("SsbAVC", SsbAvcIdx);       // SSB AVC
    AmSoftMuteIdx  = prefs.getUChar("AmSoftMute", AmSoftMuteIdx);   // AM soft mute
    SsbSoftMuteIdx = prefs.getUChar("SsbSoftMute", SsbSoftMuteIdx); // SSB soft mute
    currentSleep   = prefs.getUShort("Sleep", currentSleep);    // Sleep delay
    offTimerMinutes = prefs.getUShort("OffTimer", 0);
    standbyTimerMinutes = prefs.getUShort("StbyTimer", 0);
    themeIdx       = loadIndex("Theme", themeIdx, getTotalThemes());
    rdsModeIdx     = loadIndex("RDSMode", rdsModeIdx, getTotalRDSModes());
    sleepModeIdx   = loadIndex("SleepMode", sleepModeIdx, getTotalSleepModes());
    zoomMenu       = prefs.getUChar("ZoomMenu", zoomMenu);      // TRUE: Zoom menu
    scrollDirection = prefs.getBool("ScrollDir", scrollDirection<0)? -1:1; // TRUE: Reverse scroll
    setEncoderHalfStep(prefs.getBool("EncHalfStep", encoderHalfStep)); // Encoder half-step mode
    utcOffsetIdx   = loadIndex("UTCOffset", utcOffsetIdx, getTotalUTCOffsets());
    uint32_t squelch = prefs.getUInt("Squelch", ((uint32_t)currentSquelch[FM]) |
                                                ((uint32_t)currentSquelch[LSB] << 8) |
                                                ((uint32_t)currentSquelch[USB] << 16) |
                                                ((uint32_t)currentSquelch[AM] << 24)); // Squelch
    currentSquelch[FM]  = squelch & 0xff;
    currentSquelch[LSB] = (squelch >> 8) & 0xff;
    currentSquelch[USB] = (squelch >> 16) & 0xff;
    currentSquelch[AM]  = (squelch >> 24) & 0xff;
    FmRegionIdx    = loadIndex("FmRegion", FmRegionIdx, getTotalFMRegions());
    // The stereo list is 自动 / 立体声 / 单声道 now, so a unit coming from an
    // older build has the old meaning in "FmStereo": 1 was mono. Read the old
    // key once (it is never written again), translate it, and let the new key
    // take over from the first save onwards. Without this an owner who had
    // chosen mono would silently come up in stereo after the update.
    if(prefs.isKey("FmStereo2"))
      fmStereoIdx  = loadIndex("FmStereo2", fmStereoIdx, getTotalFMStereoModes());
    else
      fmStereoIdx  = (prefs.getUChar("FmStereo", FM_STEREO_AUTO) == 1) ? FM_STEREO_MONO : FM_STEREO_AUTO;
    audioClamp     = prefs.getBool("AudioClamp", true);        // Hardware audio clamp present
    dspPatchesIdx  = prefs.getUChar("DSPPatchset", DSP_PATCHES_DEFAULT); // Uploaded patch set
    uiLayoutIdx    = loadIndex("UILayout", uiLayoutIdx, getTotalUILayouts());
    bleModeIdx     = loadIndex("BLEMode", bleModeIdx, getTotalBluetoothModes());
    usbModeIdx     = loadIndex("USBMode", usbModeIdx, getTotalUSBModes());
    tcpModeIdx     = loadIndex("TCPMode", tcpModeIdx, getTotalTCPModes());

    // Done with global settings
    prefs.end();
  }

  if(items & SAVE_BANDS)
  {
    // Missing or incompatible VFO data is initialized after radio startup.
    vfos[0] = vfos[1] = {};
    vfoIdx = 0;

    // Will be loading from bands
    prefs.begin("bands", true, STORAGE_PARTITION);

    // Same rule as the settings section: an older layout is migrated, because
    // prefsLoadBand() keeps the band's built-in defaults for anything that no
    // longer fits. Refusing outright would silently wipe the tuned frequency
    // and the SSB calibration of every band after a firmware update.
    uint8_t version = prefs.getUChar("Version", 0);
    if((items & SAVE_VERIFY) && (version == 0 || version > VER_BANDS))
    {
      prefs.end();
      return(false);
    }

    // Read band settings
    for(int i=0 ; i<getTotalBands() ; i++) prefsLoadBand(i, false);

    if(prefs.getBytesLength("VFOSlots")==sizeof(vfos) &&
       prefs.getBytes("VFOSlots", vfos, sizeof(vfos))==sizeof(vfos))
    {
      // The saved index is used as an array subscript in prefsSave() and
      // restoreVfo(), so it has to be in range before it is trusted. A slot
      // pair written by an older layout, or a half-erased namespace, can hold
      // any byte value here.
      vfoIdx = prefs.getUChar("VFOActive", 0);
      if(vfoIdx >= ITEM_COUNT(vfos)) vfoIdx = 0;
    }
    else
      vfos[0] = vfos[1] = {};

    // Done with bands
    prefs.end();
  }
  else if(items & SAVE_CUR_BAND)
  {
    // Load current band only
    prefsLoadBand(bandIdx);
  }

  if(items & SAVE_MEMORIES)
  {
    // Will be loading from memories
    prefs.begin("memories", true, STORAGE_PARTITION);

    // Same rule again: prefsLoadMemory() understands the older slot layout, so
    // an OTA update keeps all 99 memories instead of returning early.
    uint8_t version = prefs.getUChar("Version", 0);
    if((items & SAVE_VERIFY) && (version == 0 || version > VER_MEMORIES))
    {
      prefs.end();
      return(false);
    }

    // Read all memories
    for(int i=0 ; i<getTotalMemories() ; i++) prefsLoadMemory(i, false);

    // Done with memories
    prefs.end();
  }

  return(true);
}

bool diskInit(bool force)
{
  if(!force)
  {
    prefs.begin("storage", true, STORAGE_PARTITION);
    force = prefs.getUChar("Version", 0) != VER_STORAGE;
    prefs.end();
  }

  if(force) LittleFS.end();

  // formatOnFail = force: after a factory reset or a storage version bump the
  // partition holds no filesystem yet. The library formats only after the mount
  // failed, and LittleFSFS::format() uses the partition label that begin()
  // installs -- calling format() before begin() would run it with that label
  // still unset and format the wrong partition (or nothing at all).
  bool mounted = LittleFS.begin(force, "/littlefs", 10, "littlefs");

  if(!mounted && !force)
  {
    // The image exists but cannot be read (power loss during a write, damaged
    // table). Rebuild it once instead of booting without a filesystem.
    if(!LittleFS.format())
    {
      return(false);
    }

    mounted = LittleFS.begin(false, "/littlefs", 10, "littlefs");
  }

  if(!mounted)
  {
    return(false);
  }

  prefs.begin("storage", false, STORAGE_PARTITION);
  prefs.putUChar("Version", VER_STORAGE);
  prefs.end();

  return(true);
}

bool nvsErase()
{
  return(nvs_flash_erase() == ESP_OK &&
         nvs_flash_init() == ESP_OK &&
         nvs_flash_erase_partition(STORAGE_PARTITION) == ESP_OK &&
         nvs_flash_init_partition(STORAGE_PARTITION) == ESP_OK);
}
