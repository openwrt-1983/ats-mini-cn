#include "Common.h"
#include "Storage.h"
#include "Menu.h"
#include "Memories.h"
#include "Themes.h"
#include "Utils.h"
#include "Patches.h"
#include "Backup.h"

#include <LittleFS.h>
#include <time.h>

//
// A full configuration dump that survives a complete flash erase.
//
// Everything the receiver stores in NVS is written to one plain text file:
// the global settings, the per-band tuning state including the SSB
// calibration, both VFO slots, the Wi-Fi credentials, the web login and all
// 99 memory channels. Flashing at 0x0 wipes the settings partition along with
// the application, so this file is the only way to get a receiver back to the
// way it was without touching the menus again.
//
// The format is deliberately boring: one "key=value" per line, "#" starts a
// comment, unknown keys are ignored. That way the file stays readable and a
// value can be edited by hand before importing it.
//

#define BACKUP_FORMAT 1

// Set by the web task once the upload finished, cleared by the main task.
static volatile bool restoreRequested = false;
static volatile bool restoreActive = false;

// Non-zero once a restart has been scheduled, so the HTTP response can be
// flushed before the device goes down.
static uint32_t restartAt = 0;

// Filled in by backupTick() so the result page can report what happened.
static int restoreApplied = 0;
static int restoreSkipped = 0;
static const char *restoreError = nullptr;

//
// Helpers
//

static void backupLine(String &out, const char *key, long value)
{
  out += key;
  out += '=';
  out += String(value);
  out += '\n';
}

static void backupValue(String &out, const char *key, const String &value)
{
  out += key;
  out += '=';
  out += value;
  out += '\n';
}

// Decimal only: no signs, no suffixes, no whitespace.
static bool parseNumber(const String &text, long minimum, long maximum, long &result)
{
  if(!text.length() || text.length() > 11) return false;

  size_t start = text[0] == '-' ? 1 : 0;
  if(start >= text.length()) return false;
  for(size_t i = start; i < text.length(); i++)
    if(text[i] < '0' || text[i] > '9') return false;

  long value = strtol(text.c_str(), nullptr, 10);
  if(value < minimum || value > maximum) return false;
  result = value;
  return true;
}

// Split "a|b|c" into count numeric fields.
static bool parseFields(const String &line, char separator, long *values, int count)
{
  int start = 0;
  for(int i = 0; i < count; i++)
  {
    int end = i == count - 1 ? (int)line.length() : line.indexOf(separator, start);
    if(end < 0 || end < start) return false;
    if(!parseNumber(line.substring(start, end), LONG_MIN, LONG_MAX, values[i])) return false;
    start = end + 1;
  }
  return true;
}

static String backupTimestamp()
{
  time_t now = time(nullptr);
  if(now < 1600000000L) return String("未对时");

  // The header is read by people, so print local time and name the zone it
  // used instead of raw UTC.
  int offsetMinutes = getCurrentUTCOffset() * 15;
  now += offsetMinutes * 60;
  struct tm fields;
  gmtime_r(&now, &fields);
  char text[24];
  strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &fields);

  char zone[16];
  int magnitude = abs(offsetMinutes);
  snprintf(zone, sizeof(zone), " (UTC%c%02d:%02d)",
           offsetMinutes < 0? '-' : '+', magnitude / 60, magnitude % 60);
  return String(text) + zone;
}

//
// Export
//

String backupBuild()
{
  String out;
  out.reserve(12288);

  out += "# ATS-Mini 配置文件\n";
  out += "# 固件 v";
  out += String(VER_APP / 100);
  out += '.';
  out += String(VER_APP % 100);
  out += " · 导出时间 ";
  out += backupTimestamp();
  out += "\n";
  out += "#\n";
  out += "# 恢复：浏览器打开 http://<收音机IP>/config ，页面最下方「配置备份」\n";
  out += "#       选择本文件后点「导入并重启」，设备会自动重启并套用配置。\n";
  out += "# 注意：文件里含 Wi-Fi 密码与 Web 登录密码，请勿外传。\n";
  out += "\n";

  backupLine(out, "format", BACKUP_FORMAT);
  backupLine(out, "app", VER_APP);
  backupLine(out, "settings.version", VER_SETTINGS);
  backupLine(out, "bands.version", VER_BANDS);
  backupLine(out, "memories.version", VER_MEMORIES);

  out += "\n# ---- 全局设置 ----\n";
  backupLine(out, "settings.Volume",        volume);
  backupLine(out, "settings.Band",          bandIdx);
  backupLine(out, "settings.WiFiMode",      wifiModeIdx);
  backupLine(out, "settings.Brightness",    currentBrt);
  backupLine(out, "settings.FmAGC",         FmAgcIdx);
  backupLine(out, "settings.AmAGC",         AmAgcIdx);
  backupLine(out, "settings.SsbAGC",        SsbAgcIdx);
  backupLine(out, "settings.AmAVC",         AmAvcIdx);
  backupLine(out, "settings.SsbAVC",        SsbAvcIdx);
  backupLine(out, "settings.AmSoftMute",    AmSoftMuteIdx);
  backupLine(out, "settings.SsbSoftMute",   SsbSoftMuteIdx);
  backupLine(out, "settings.Sleep",         currentSleep);
  backupLine(out, "settings.OffTimer",      offTimerMinutes);
  backupLine(out, "settings.StbyTimer",     standbyTimerMinutes);
  backupLine(out, "settings.Theme",         themeIdx);
  backupLine(out, "settings.RDSMode",       rdsModeIdx);
  backupLine(out, "settings.SleepMode",     sleepModeIdx);
  backupLine(out, "settings.ZoomMenu",      zoomMenu ? 1 : 0);
  backupLine(out, "settings.ScrollReverse", scrollDirection < 0 ? 1 : 0);
  backupLine(out, "settings.EncHalfStep",   encoderHalfStep ? 1 : 0);
  backupLine(out, "settings.UTCOffset",     utcOffsetIdx);
  backupLine(out, "settings.FmRegion",      FmRegionIdx);
  backupLine(out, "settings.FmStereo",      fmStereoIdx);
  backupLine(out, "settings.AudioClamp",    audioClamp ? 1 : 0);
  backupLine(out, "settings.DSPPatchset",   dspPatchesIdx);
  backupLine(out, "settings.UILayout",      uiLayoutIdx);
  backupLine(out, "settings.BLEMode",       bleModeIdx);
  backupLine(out, "settings.USBMode",       usbModeIdx);
  backupLine(out, "settings.TCPMode",       tcpModeIdx);

  out += "settings.Squelch=";
  for(int i = 0; i < 4; i++)
  {
    if(i) out += ',';
    out += String(currentSquelch[i]);
  }
  out += '\n';

  out += "\n# ---- Wi-Fi 与 Web 登录 ----\n";
  {
    prefs.begin("network", true, STORAGE_PARTITION);
    for(int j = 0; j < 3; j++)
    {
      char key[16];
      snprintf(key, sizeof(key), "network.ssid%d", j + 1);
      backupValue(out, key, prefs.getString((String("wifissid") + String(j + 1)).c_str(), ""));
      snprintf(key, sizeof(key), "network.pass%d", j + 1);
      backupValue(out, key, prefs.getString((String("wifipass") + String(j + 1)).c_str(), ""));
    }
    backupLine(out, "network.scanhidden", prefs.getBool("wifiscanhidden", false) ? 1 : 0);
    backupValue(out, "network.loginusername", prefs.getString("loginusername", ""));
    backupValue(out, "network.loginpassword", prefs.getString("loginpassword", ""));
    prefs.end();
  }

  out += "\n# ---- 波段：频率(Hz)|模式|步进|带宽|USB校准|LSB校准 ----\n";
  for(int i = 0; i < getTotalBands(); i++)
  {
    const Band &band = bands[i];
    // bands[].currentFreq only follows memory/VFO recalls, so the band the
    // receiver is tuned to right now has to come from the live frequency.
    uint32_t freq = i == bandIdx ? currentFrequency : band.currentFreq;
    char key[24];
    snprintf(key, sizeof(key), "band.%d", i);
    out += key;
    out += '=';
    out += String(freq);
    out += '|';
    out += String((int)band.bandMode);
    out += '|';
    out += String((int)band.currentStepIdx);
    out += '|';
    out += String((int)band.bandwidthIdx);
    out += '|';
    out += String(band.usbCal);
    out += '|';
    out += String(band.lsbCal);
    out += "   # ";
    out += band.bandName;
    out += '\n';
  }

  out += "\n# ---- VFO：频率|波段|模式|步进|带宽 ----\n";
  backupLine(out, "vfo.active", vfoIdx);
  for(int i = 0; i < 2; i++)
  {
    // Same as prefsSave(): the active slot is whatever the receiver shows now.
    VfoState slot = (i == vfoIdx && vfoInitialized) ? captureVfo() : vfos[i];
    char key[16];
    snprintf(key, sizeof(key), "vfo.%d", i);
    out += key;
    out += '=';
    out += String(slot.freq);
    out += '|';
    out += String((int)slot.band);
    out += '|';
    out += String((int)slot.mode);
    out += '|';
    out += String((int)slot.stepIdx);
    out += '|';
    out += String((int)slot.bandwidthIdx);
    out += '\n';
  }

  out += "\n# ---- 电台记忆：频率(Hz)|波段|模式|名称 ----\n";
  for(int i = 0; i < getTotalMemories(); i++)
  {
    const Memory mem = getMemory(i);
    char name[sizeof(mem.name) + 1];
    memset(name, 0, sizeof(name));
    memcpy(name, mem.name, sizeof(mem.name));

    char key[16];
    snprintf(key, sizeof(key), "memory.%d", i);
    out += key;
    out += '=';
    out += String(mem.freq);
    out += '|';
    out += String((int)mem.band);
    out += '|';
    out += String((int)mem.mode);
    out += '|';
    out += name;
    out += '\n';
  }

  return out;
}

//
// Import
//

// One parsed line. Returns true when the key was recognised and accepted.
static bool applySetting(const String &name, const String &value, long &parsed)
{
  if(name == "Volume")        { if(!parseNumber(value, 0, 63, parsed)) return false; volume = parsed; return true; }
  if(name == "Band")          { if(!parseNumber(value, 0, getTotalBands() - 1, parsed)) return false; bandIdx = parsed; return true; }
  if(name == "WiFiMode")      { if(!parseNumber(value, 0, getTotalWiFiModes() - 1, parsed)) return false; wifiModeIdx = parsed; return true; }
  if(name == "Brightness")    { if(!parseNumber(value, 10, 255, parsed)) return false; currentBrt = parsed; return true; }
  if(name == "FmAGC")         { if(!parseNumber(value, 0, 127, parsed)) return false; FmAgcIdx = parsed; return true; }
  if(name == "AmAGC")         { if(!parseNumber(value, 0, 127, parsed)) return false; AmAgcIdx = parsed; return true; }
  if(name == "SsbAGC")        { if(!parseNumber(value, 0, 127, parsed)) return false; SsbAgcIdx = parsed; return true; }
  if(name == "AmAVC")         { if(!parseNumber(value, 0, 127, parsed)) return false; AmAvcIdx = parsed; return true; }
  if(name == "SsbAVC")        { if(!parseNumber(value, 0, 127, parsed)) return false; SsbAvcIdx = parsed; return true; }
  if(name == "AmSoftMute")    { if(!parseNumber(value, 0, 127, parsed)) return false; AmSoftMuteIdx = parsed; return true; }
  if(name == "SsbSoftMute")   { if(!parseNumber(value, 0, 127, parsed)) return false; SsbSoftMuteIdx = parsed; return true; }
  if(name == "Sleep")         { if(!parseNumber(value, 0, 255, parsed)) return false; currentSleep = parsed; return true; }
  if(name == "OffTimer")      { if(!parseNumber(value, 0, 240, parsed)) return false; offTimerMinutes = parsed; return true; }
  if(name == "StbyTimer")     { if(!parseNumber(value, 0, 240, parsed)) return false; standbyTimerMinutes = parsed; return true; }
  if(name == "Theme")         { if(!parseNumber(value, 0, getTotalThemes() - 1, parsed)) return false; themeIdx = parsed; return true; }
  if(name == "RDSMode")       { if(!parseNumber(value, 0, getTotalRDSModes() - 1, parsed)) return false; rdsModeIdx = parsed; return true; }
  if(name == "SleepMode")     { if(!parseNumber(value, 0, getTotalSleepModes() - 1, parsed)) return false; sleepModeIdx = parsed; return true; }
  if(name == "ZoomMenu")      { if(!parseNumber(value, 0, 1, parsed)) return false; zoomMenu = parsed != 0; return true; }
  if(name == "ScrollReverse") { if(!parseNumber(value, 0, 1, parsed)) return false; scrollDirection = parsed ? -1 : 1; return true; }
  if(name == "EncHalfStep")   { if(!parseNumber(value, 0, 1, parsed)) return false; setEncoderHalfStep(parsed != 0); return true; }
  if(name == "UTCOffset")     { if(!parseNumber(value, 0, getTotalUTCOffsets() - 1, parsed)) return false; utcOffsetIdx = parsed; return true; }
  if(name == "FmRegion")      { if(!parseNumber(value, 0, getTotalFMRegions() - 1, parsed)) return false; FmRegionIdx = parsed; return true; }
  if(name == "FmStereo")      { if(!parseNumber(value, 0, getTotalFMStereoModes() - 1, parsed)) return false; fmStereoIdx = parsed; return true; }
  if(name == "AudioClamp")    { if(!parseNumber(value, 0, 1, parsed)) return false; audioClamp = parsed != 0; return true; }
  if(name == "DSPPatchset")   { if(!parseNumber(value, 0, PATCH_SET_COUNT, parsed)) return false; dspPatchesIdx = parsed; return true; }
  if(name == "UILayout")      { if(!parseNumber(value, 0, getTotalUILayouts() - 1, parsed)) return false; uiLayoutIdx = parsed; return true; }
  if(name == "BLEMode")       { if(!parseNumber(value, 0, getTotalBluetoothModes() - 1, parsed)) return false; bleModeIdx = parsed; return true; }
  if(name == "USBMode")       { if(!parseNumber(value, 0, getTotalUSBModes() - 1, parsed)) return false; usbModeIdx = parsed; return true; }
  if(name == "TCPMode")       { if(!parseNumber(value, 0, getTotalTCPModes() - 1, parsed)) return false; tcpModeIdx = parsed; return true; }
  return false;
}

static bool applySquelch(const String &value)
{
  long fields[4];
  int start = 0;
  for(int i = 0; i < 4; i++)
  {
    int end = i == 3 ? (int)value.length() : value.indexOf(',', start);
    if(end < 0 || !parseNumber(value.substring(start, end), 0, 255, fields[i])) return false;
    start = end + 1;
  }
  for(int i = 0; i < 4; i++) currentSquelch[i] = fields[i];
  return true;
}

static bool applyBand(const String &name, const String &value)
{
  long index;
  if(!parseNumber(name.substring(5), 0, getTotalBands() - 1, index)) return false;
  if(name.substring(0, 5) != "band.") return false;

  // Trim the trailing comment the export adds after the band name
  String fields = value;
  int comment = fields.indexOf('#');
  if(comment >= 0) fields = fields.substring(0, comment);

  long values[6];
  if(!parseFields(fields, '|', values, 6)) return false;
  if(values[0] < 0 || values[0] > 1000000000L) return false;
  if(values[1] < 0 || values[1] > getTotalModes() - 1) return false;
  // Step and bandwidth indices are clamped again by selectBand(), but keeping
  // them in a sane window avoids relying on that correction.
  if(values[2] < 0 || values[2] > 31) return false;
  if(values[3] < 0 || values[3] > 15) return false;
  if(values[4] < -MAX_CAL || values[4] > MAX_CAL) return false;
  if(values[5] < -MAX_CAL || values[5] > MAX_CAL) return false;

  Band &band = bands[index];
  band.currentFreq    = values[0];
  band.bandMode       = values[1];
  band.currentStepIdx = values[2];
  band.bandwidthIdx   = values[3];
  band.usbCal         = values[4];
  band.lsbCal         = values[5];
  return true;
}

static bool applyVfo(const String &name, const String &value)
{
  long index;
  if(name.substring(0, 4) != "vfo.") return false;
  if(!parseNumber(name.substring(4), 0, 1, index)) return false;

  long values[5];
  if(!parseFields(value, '|', values, 5)) return false;
  if(values[0] < 0 || values[0] > 1000000000L) return false;
  if(values[1] < 0 || values[1] > getTotalBands() - 1) return false;
  if(values[2] < 0 || values[2] > getTotalModes() - 1) return false;
  if(values[3] < 0 || values[3] > 31) return false;
  if(values[4] < 0 || values[4] > 15) return false;

  vfos[index].freq         = values[0];
  vfos[index].band         = values[1];
  vfos[index].mode         = values[2];
  vfos[index].stepIdx      = values[3];
  vfos[index].bandwidthIdx = values[4];
  return true;
}

static bool applyMemory(const String &name, const String &value)
{
  long index;
  if(name.substring(0, 7) != "memory.") return false;
  if(!parseNumber(name.substring(7), 0, getTotalMemories() - 1, index)) return false;

  int first = value.indexOf('|');
  int second = first < 0 ? -1 : value.indexOf('|', first + 1);
  int third = second < 0 ? -1 : value.indexOf('|', second + 1);
  if(third < 0) return false;

  long freq, band, mode;
  if(!parseNumber(value.substring(0, first), 0, 1000000000L, freq)) return false;
  if(!parseNumber(value.substring(first + 1, second), 0, getTotalBands() - 1, band)) return false;
  if(!parseNumber(value.substring(second + 1, third), 0, getTotalModes() - 1, mode)) return false;

  Memory mem = {};
  mem.freq = freq;
  mem.band = band;
  mem.mode = mode;

  // Names are UTF-8 and at most MEMORY_NAME_LEN - 1 bytes. Cut on a character
  // boundary so the display never gets half a Chinese character.
  String label = value.substring(third + 1);
  size_t length = label.length();
  if(length > MEMORY_NAME_LEN - 1) length = MEMORY_NAME_LEN - 1;
  while(length && ((uint8_t)label[length] & 0xc0) == 0x80) length--;

  char text[MEMORY_NAME_LEN] = {};
  size_t used = 0;
  for(size_t i = 0; i < length && used < MEMORY_NAME_LEN - 1; i++)
  {
    uint8_t c = (uint8_t)label[i];
    if(c < 0x20 || c == 0x7f) continue;
    text[used++] = (char)c;
  }
  text[used] = '\0';
  memcpy(mem.name, text, used + 1);

  setMemory(index, mem);
  return true;
}

static bool applyNetwork(const String &name, const String &value, bool &written)
{
  written = true;
  if(name == "loginusername") { prefs.putString("loginusername", value); return true; }
  if(name == "loginpassword") { prefs.putString("loginpassword", value); return true; }
  if(name == "scanhidden")
  {
    long parsed;
    if(!parseNumber(value, 0, 1, parsed)) return false;
    prefs.putBool("wifiscanhidden", parsed != 0);
    return true;
  }

  for(int j = 1; j <= 3; j++)
  {
    char ssid[16], pass[16];
    snprintf(ssid, sizeof(ssid), "ssid%d", j);
    snprintf(pass, sizeof(pass), "pass%d", j);
    if(name == ssid) { prefs.putString((String("wifissid") + String(j)).c_str(), value); return true; }
    if(name == pass) { prefs.putString((String("wifipass") + String(j)).c_str(), value); return true; }
  }

  written = false;
  return false;
}

bool backupRequestRestore()
{
  if(restoreRequested || restoreActive) return false;
  restoreRequested = true;
  return true;
}

bool backupTick()
{
  // Restart once the HTTP response has had time to reach the browser.
  if(restartAt && (int32_t)(millis() - restartAt) >= 0)
  {
    restartAt = 0;
    Serial.println("Backup restored, restarting");
    Serial.flush();
    ESP.restart();
  }

  if(!restoreRequested) return false;
  restoreRequested = false;
  restoreActive = true;

  restoreApplied = 0;
  restoreSkipped = 0;
  restoreError = nullptr;

  File file = LittleFS.open(BACKUP_TMP_PATH, "r");
  if(!file)
  {
    restoreError = "找不到上传的配置文件。";
    restoreActive = false;
    return false;
  }

  bool networkOpen = false;
  bool seenFormat = false;

  while(file.available())
  {
    String line = file.readStringUntil('\n');
    line.trim();
    if(!line.length() || line[0] == '#') continue;

    int equals = line.indexOf('=');
    if(equals <= 0) { restoreSkipped++; continue; }

    String key = line.substring(0, equals);
    String value = line.substring(equals + 1);
    key.trim();
    value.trim();

    long parsed = 0;

    if(key == "format")            { seenFormat = true; restoreApplied++; continue; }
    if(key == "app" ||
       key == "settings.version" ||
       key == "bands.version" ||
       key == "memories.version")  { restoreApplied++; continue; }

    if(key.startsWith("settings."))
    {
      String name = key.substring(9);
      if(name == "Squelch")
      {
        if(applySquelch(value)) restoreApplied++; else restoreSkipped++;
        continue;
      }
      if(applySetting(name, value, parsed)) restoreApplied++; else restoreSkipped++;
      continue;
    }

    if(key.startsWith("network."))
    {
      if(!networkOpen) { prefs.begin("network", false, STORAGE_PARTITION); networkOpen = true; }
      bool written = false;
      if(applyNetwork(key.substring(8), value, written) && written) restoreApplied++;
      else restoreSkipped++;
      continue;
    }

    if(key.startsWith("vfo.active"))
    {
      if(parseNumber(value, 0, 1, parsed)) { vfoIdx = parsed; restoreApplied++; }
      else restoreSkipped++;
      continue;
    }

    if(key.startsWith("vfo."))
    {
      if(applyVfo(key, value)) restoreApplied++; else restoreSkipped++;
      continue;
    }

    if(key.startsWith("band."))
    {
      if(applyBand(key, value)) restoreApplied++; else restoreSkipped++;
      continue;
    }

    if(key.startsWith("memory."))
    {
      if(applyMemory(key, value)) restoreApplied++; else restoreSkipped++;
      continue;
    }

    // Unknown keys are tolerated: a newer firmware may have added settings
    // that this build simply does not know about.
    restoreSkipped++;
  }

  file.close();
  LittleFS.remove(BACKUP_TMP_PATH);

  if(networkOpen) prefs.end();

  if(!seenFormat)
  {
    restoreError = "文件不是 ATS-Mini 配置文件（缺少 format 行）。";
    restoreActive = false;
    return false;
  }

  if(restoreApplied < 10)
  {
    restoreError = "配置文件里没有可用的设置项。";
    restoreActive = false;
    return false;
  }

  // prefsSave(SAVE_ALL) would overwrite the imported VFO slot with the state
  // the receiver is in right now, so drop that flag for the one save.
  bool wasInitialized = vfoInitialized;
  vfoInitialized = false;
  prefsSave(SAVE_ALL);
  vfoInitialized = wasInitialized;

  Serial.printf("Backup restored: %d applied, %d skipped\n", restoreApplied, restoreSkipped);
  Serial.flush();

  restoreActive = false;
  restartAt = millis() + 2500;
  return true;
}

int backupRestoreApplied() { return restoreApplied; }
int backupRestoreSkipped() { return restoreSkipped; }
const char *backupRestoreError() { return restoreError; }
bool backupRestoreDone()   { return restartAt != 0; }

bool backupRestorePending()
{
  return restoreRequested || restoreActive;
}
