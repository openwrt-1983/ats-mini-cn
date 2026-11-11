#include "Web.h"
#include "Common.h"
#include "Storage.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Memories.h"
#include "Draw.h"
#include "Splash.h"
#include "Remote.h"
#include "BleMode.h"
#include "Ota.h"
#include "Patches.h"
#include "Backup.h"
#include "StationNames.h"
#include "PageTemplate.h"
#include "PageCommon.h"
#include "PageStatus.h"
#include "PageMemory.h"
#include "PageConfig.h"
#include "PageWiFi.h"
#include "PageUpdate.h"
#include "PagePatches.h"

#include <new>
#include <mutex>
#include <atomic>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <time.h>
#include <esp_heap_caps.h>

#define SPLASH_MAX_FILE_SIZE (512U * 1024U)

static String loginUsername = "";
static String loginPassword = "";
static AsyncWebServer server(80);

// Guards the two credential strings: webInit() runs on the receiver task while
// the HTTP handlers run on the AsyncTCP task.
static std::mutex authMutex;

// The HTTP handlers run on the AsyncTCP task, the receiver loop on its own.
// A Preferences object keeps the open NVS handle in its own fields, so both
// tasks must not share one: a second begin() would drop the first handle and
// an end() would close the namespace the other task is still using. The web
// side therefore owns a private instance; NVS serializes access internally.
static Preferences webPrefs;

static void webSetConfig(AsyncWebServerRequest *request);
static void webSetWiFi(AsyncWebServerRequest *request);
static void webSetMemories(AsyncWebServerRequest *request);
static void webUploadSplash(AsyncWebServerRequest *request, const String &filename,
                            size_t index, uint8_t *data, size_t len, bool final);
static void webBackupDownload(AsyncWebServerRequest *request);
static void webUploadBackup(AsyncWebServerRequest *request, const String &filename,
                            size_t index, uint8_t *data, size_t len, bool final);
static void webRestoreComplete(AsyncWebServerRequest *request);
static bool webIsAuthenticated(AsyncWebServerRequest *request);
static void webUploadFirmwareComplete(AsyncWebServerRequest *request);
static void webUpdatePage(AsyncWebServerRequest *request, const OtaStatus &status = otaStatus(), int code = 0);
static void webUploadFirmware(AsyncWebServerRequest *request, const String &filename,
                              size_t index, uint8_t *data, size_t len, bool final);
static void webPatchesPage(AsyncWebServerRequest *request);
static void webPatchesComplete(AsyncWebServerRequest *request);
static void webUploadPatch(AsyncWebServerRequest *request, const String &filename,
                           size_t index, uint8_t *data, size_t len, bool final);
static bool webParseDateTime(const String &text, uint32_t *epoch);

static String webPage(const char *body, const char *title, std::initializer_list<PageValue> values,
                      std::initializer_list<PageFragment> fragments = {});
static const String webRadioPage();
static const String webMemoryPage();
static const String webConfigPage();
static const String webWiFiPage();

// Apply Config settings on the main task, alongside receiver controls.
static struct
{
  std::mutex mutex;
  bool pending = false;
  int layout, region, stereo, brightness, sleepTimer, sleepMode, offTimer;
  int usbMode, tcpMode, bleMode, rds;
  int utcOffset, themeIndex;
  bool clamp, reverseScroll, halfStep, zoom;
  bool setClock;
  uint32_t epoch;
} configSettings;

static std::atomic<int> pendingWiFiMode{-1};

struct SplashUploadState
{
  bool incomplete;
  bool tooLarge;
};

struct BackupUploadState
{
  bool incomplete;
  bool tooLarge;
  bool failed;
};

static bool webIsAuthenticated(AsyncWebServerRequest *request)
{
  // Copy under the lock: authenticate() keeps using the strings while webInit()
  // may reassign them on the receiver task, and a reassignment reallocates the
  // buffer the pointers refer to.
  String user, pass;
  {
    std::lock_guard<std::mutex> lock(authMutex);
    user = loginUsername;
    pass = loginPassword;
  }

  return(user == "" || pass == "" || request->authenticate(user.c_str(), pass.c_str()));
}

// Keep in sync with MDNS.begin() in Network.cpp.
#define WEB_MDNS_HOST "atsmini"

// Extract the host part of an Origin/Referer value ("http://user@host:port/x").
// Returns an empty string when the value cannot be parsed.
static String webHeaderHost(const String &value)
{
  int scheme = value.indexOf("://");
  if(scheme < 0) return "";

  int start = scheme + 3;
  int end = value.indexOf('/', start);
  if(end < 0) end = value.length();
  String authority = value.substring(start, end);

  int at = authority.lastIndexOf('@');
  if(at >= 0) authority = authority.substring(at + 1);

  // Literal IPv6 address, optionally with a port.
  if(authority.startsWith("["))
  {
    int close = authority.indexOf(']');
    return close > 1 ? authority.substring(1, close) : "";
  }

  int colon = authority.indexOf(':');
  if(colon >= 0) authority = authority.substring(0, colon);
  return authority;
}

// Browsers attach the stored Basic credentials to a cross-site form post, so
// authentication alone does not stop a page served from another host from
// driving the receiver (changing settings, uploading a firmware image). Reject
// a write whose Origin/Referer names a different host. Requests carrying
// neither header -- curl, the app, command line tools -- are allowed, because
// those clients cannot be confused into sending someone else's request.
static bool webIsSameOrigin(AsyncWebServerRequest *request)
{
  const AsyncWebHeader *header = request->getHeader("Origin");
  if(!header) header = request->getHeader("Referer");
  if(!header) return true;

  String host = webHeaderHost(header->value());
  if(!host.length()) return true;

  if(host == WiFi.localIP().toString()) return true;
  if(host == WiFi.softAPIP().toString()) return true;

  String name = host;
  if(name.endsWith(".local")) name = name.substring(0, name.length() - 6);
  return(name.equalsIgnoreCase(WEB_MDNS_HOST));
}

// Gate for every handler that changes state: authentication plus the
// cross-origin check above. Sends the refusal itself and returns false.
static bool webAllowWrite(AsyncWebServerRequest *request)
{
  if(!webIsAuthenticated(request))
  {
    request->requestAuthentication();
    return false;
  }

  if(webIsSameOrigin(request)) return true;

  request->send(403, "text/plain", "Cross-origin request rejected.");
  return false;
}

// One download owns the PSRAM buffer until completion, disconnect, or failure.
static struct
{
  enum State { Pending, Ready, Failed } state = Pending;
  std::mutex mutex;
  uint8_t *bmp = nullptr;
  char filename[sizeof("screenshot-yyyy-mm-dd-hh-mm-ss.bmp")];

  // Caller holds mutex, including on capture timeout or failure.
  void release()
  {
    free(bmp);
    bmp = nullptr;
  }
} screenshot;

static const char *webScreenshotAcquire()
{
  std::unique_lock<std::mutex> lock(screenshot.mutex);
  if(screenshot.bmp) return "A screenshot download is already in progress.";
  screenshot.bmp = (uint8_t *)heap_caps_malloc(BMP_FILE_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if(!screenshot.bmp) return "无法分配截图缓冲区。";
  screenshot.state = screenshot.Pending;

  // Yield to the main loop, but do not hold up the async web task indefinitely.
  // One loop iteration is enough when the receiver is responsive; a longer
  // wait only blocks every other request while the main task is stuck elsewhere.
  uint32_t started = millis();
  while(screenshot.state == screenshot.Pending && millis() - started < 500)
  {
    lock.unlock();
    delay(1);
    lock.lock();
  }
  if(screenshot.state == screenshot.Ready) return nullptr;
  screenshot.release();
  return screenshot.state == screenshot.Pending ? "接收机忙，请稍后再试。" : "无法截取屏幕截图。";
}

static void webScreenshotRelease()
{
  std::lock_guard<std::mutex> lock(screenshot.mutex);
  screenshot.release();
}

void webScreenshotTick()
{
  // Capture between redraws. A timeout cancels pending work; once ready,
  // the buffer stays unchanged until the download ends.
  std::lock_guard<std::mutex> lock(screenshot.mutex);
  if(screenshot.bmp && screenshot.state == screenshot.Pending)
  {
    strcpy(screenshot.filename, "screenshot.bmp");
    if(clockGetDate(nullptr, nullptr, nullptr, nullptr))
    {
      time_t now = time(NULL) + getCurrentUTCOffset() * 15 * 60;
      struct tm fields;
      gmtime_r(&now, &fields);
      strftime(screenshot.filename, sizeof(screenshot.filename), "screenshot-%Y-%m-%d-%H-%M-%S.bmp", &fields);
    }
    screenshot.state = remoteCaptureBmp(screenshot.bmp) ? screenshot.Ready : screenshot.Failed;
  }
}

// Apply queued web operations on the main task. Screenshot capture runs earlier
// in the loop so these potentially blocking operations cannot delay it.
bool webTick()
{
  otaTick();
  // NVS is written from this task only; a restore may also schedule a restart.
  backupTick();
  bool redraw = patchesTick();
  std::unique_lock<std::mutex> lock(configSettings.mutex);
  if(configSettings.pending)
  {
    utcOffsetIdx = configSettings.utcOffset;
    if(rdsModeIdx != configSettings.rds)
      doRDSMode(configSettings.rds - rdsModeIdx);
    if(configSettings.setClock) clockSetEpoch(configSettings.epoch);
    themeIdx = configSettings.themeIndex;
    scrollDirection = configSettings.reverseScroll? -1 : 1;
    zoomMenu = configSettings.zoom;
    setEncoderHalfStep(configSettings.halfStep);
    uiLayoutIdx = configSettings.layout;
    if(FmRegionIdx != configSettings.region)
      doFmRegion(configSettings.region - FmRegionIdx);
    if(fmStereoIdx != configSettings.stereo)
      doFmStereo(configSettings.stereo - fmStereoIdx);
    currentBrt = configSettings.brightness;
    doBrt(0);
    currentSleep = configSettings.sleepTimer;
    sleepModeIdx = configSettings.sleepMode;
    if(offTimerMinutes != configSettings.offTimer)
    {
      offTimerMinutes = configSettings.offTimer;
      offTimerRestart();
    }
    usbModeIdx = configSettings.usbMode;
    tcpModeIdx = configSettings.tcpMode;
    if(configSettings.bleMode == BLE_UNPAIR_ALL || bleModeIdx != configSettings.bleMode)
    {
      bleModeIdx = configSettings.bleMode;
      bleInit(bleModeIdx);
    }
    audioClamp = configSettings.clamp;
    digitalWrite(AUDIO_MUTE, audioClamp && (muteOn(MUTE_MAIN) || muteOn(MUTE_SQUELCH)));
    prefsRequestSave(SAVE_SETTINGS, true);
    configSettings.pending = false;
    redraw = true;
  }
  lock.unlock();

  int wifiMode = pendingWiFiMode.exchange(-1);
  if(wifiMode >= 0)
  {
    wifiModeIdx = wifiMode;
    prefsRequestSave(SAVE_SETTINGS, true);
    netInit(wifiModeIdx);
    redraw = true;
  }
  return redraw;
}

//
// Initialize internal web server
//
void webInit()
{
  // Reload authentication on each network initialization, but register routes once.
  String user, pass;
  webPrefs.begin("network", true, STORAGE_PARTITION);
  user = webPrefs.getString("loginusername", "");
  pass = webPrefs.getString("loginpassword", "");
  webPrefs.end();
  {
    std::lock_guard<std::mutex> lock(authMutex);
    loginUsername = user;
    loginPassword = pass;
  }

  static bool initialized = false;
  if(initialized) return;
  initialized = true;

  server.on("/", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    // The status page carries the receiver IP, MAC, SSID and firmware version,
    // so it must not be readable without the same credentials as /config.
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    request->send(200, "text/html", webRadioPage());
  });

  server.on("/memory", HTTP_GET, [] (AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    request->send(200, "text/html", webMemoryPage());
  });
  server.on("/memory", HTTP_POST, webSetMemories);

  server.on("/config", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    request->send(200, "text/html", webConfigPage());
  });

  server.on("/wifi", HTTP_GET, [] (AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    request->send(200, "text/html", webWiFiPage());
  });
  server.on("/wifi", HTTP_POST, webSetWiFi);

  server.on("/screenshot.bmp", HTTP_GET, [] (AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    if(const char *error = webScreenshotAcquire())
      return request->send(503, "text/plain", error);
    AsyncWebServerResponse *response = request->beginResponse(200, "image/bmp", screenshot.bmp, BMP_FILE_SIZE);
    if(!response)
    {
      webScreenshotRelease();
      return request->send(503, "text/plain", "Unable to start screenshot download.");
    }
    // The server closes the connection after the response; aborts release it too.
    request->onDisconnect(webScreenshotRelease);
    response->addHeader("Content-Disposition", String("attachment; filename=\"") + screenshot.filename + "\"");
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
  });

  server.on("/splash.png", HTTP_GET, [] (AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    if(!LittleFS.exists(SPLASH_PATH))
      return request->send(404, "text/plain", "Not found");
    request->send(LittleFS, SPLASH_PATH, "image/png");
  });

  server.onNotFound([] (AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  // This method saves configuration form contents
  server.on("/setconfig", HTTP_POST, webSetConfig, webUploadSplash);

  // Configuration backup: download the whole setup as text, upload it back
  // after a reflash so nothing has to be dialled in again.
  server.on("/backup.txt", HTTP_GET, webBackupDownload);
  server.on("/restore", HTTP_POST, webRestoreComplete, webUploadBackup);

  // Register subpaths first: the server also matches /update to /update/... .
  server.on("/update/upload", HTTP_POST, webUploadFirmwareComplete, webUploadFirmware);
  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    webUpdatePage(request, otaStatus(), 200);
  });
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request) {
    if(!webAllowWrite(request)) return;
    if(!otaRequestLatest(request->hasParam("action", true) && request->getParam("action", true)->value() == "install"))
      return webUpdatePage(request, {OTA_FAILED, "已有升级正在进行。"}, 409);
    request->redirect("/update");
  });

  server.on("/patches", HTTP_GET, webPatchesPage);
  server.on("/patches", HTTP_POST, webPatchesComplete, webUploadPatch);

  // Start web server
  server.begin();
}

static void webUploadFirmware(AsyncWebServerRequest *request, const String &filename,
                              size_t index, uint8_t *data, size_t len, bool)
{
  if(request->getResponse()) return;
  if(!webAllowWrite(request)) return;

  if(!index)
  {
    if(!filename.endsWith(".bin"))
      return webUpdatePage(request, {OTA_FAILED, "请选择固件 .bin 文件。"});
    const auto *param = request->getParam("size", true);
    if(!param) return webUpdatePage(request, {OTA_FAILED, "固件大小无效。"});
    size_t imageSize = strtoul(param->value().c_str(), nullptr, 10);
    // Round-trip the number to reject signs, whitespace, suffixes, and overflow.
    if(!imageSize || imageSize > request->contentLength() || String(imageSize) != param->value())
      return webUpdatePage(request, {OTA_FAILED, "固件大小无效。"});
    if(!otaBegin(imageSize))
      return webUpdatePage(request, {OTA_FAILED, "已有升级正在进行。"}, 409);
    request->client()->setRxTimeout(15);
    request->onDisconnect([]() { otaEndUpload(); });
  }

  if(!otaWrite(data, len)) return webUpdatePage(request);
}

static void webUploadFirmwareComplete(AsyncWebServerRequest *request)
{
  if(!webAllowWrite(request)) return;
  if(request->getResponse()) return; // Preserve an upload error queued above.
  if(!request->hasParam("firmware", true, true))
    return webUpdatePage(request, {OTA_FAILED, "未上传完整固件。"});
  otaFinish();
  webUpdatePage(request);
}

static void webUploadSplash(AsyncWebServerRequest *request, const String &filename,
                            size_t index, uint8_t *data, size_t len, bool final)
{
  // Same short-circuit as the other upload bodies: once a refusal or an error
  // page is queued, the remaining chunks must not touch LittleFS.
  if(request->getResponse()) return;
  if(!webAllowWrite(request)) return;

  if(index == 0)
  {
    LittleFS.remove(SPLASH_TEMP_PATH);

    SplashUploadState *state = static_cast<SplashUploadState *>(calloc(1, sizeof(SplashUploadState)));
    if(!state) return;

    request->_tempObject = state;
    request->onDisconnect([request, state]() {
      if(state->incomplete)
        request->_tempFile.close();

      // Discard an upload that was not installed by webSetConfig().
      LittleFS.remove(SPLASH_TEMP_PATH);
    });

    // The browser filter is only advisory, so enforce the extension here too.
    if(filename.endsWith(".png"))
    {
      request->_tempFile = LittleFS.open(SPLASH_TEMP_PATH, "w");
      state->incomplete = request->_tempFile;
    }
  }

  SplashUploadState *state = static_cast<SplashUploadState *>(request->_tempObject);
  // No context means the first chunk could not allocate one (or a non-.png
  // upload created none); there is nothing to track, and writing without it
  // would dereference a null pointer.
  if(!state) return;

  if(request->_tempFile && len)
  {
    if((index + len) > SPLASH_MAX_FILE_SIZE)
    {
      state->tooLarge = true;
      state->incomplete = false;
      request->_tempFile.close();
      LittleFS.remove(SPLASH_TEMP_PATH);
    }
    else if(request->_tempFile.write(data, len) != len)
    {
      state->incomplete = false;
      request->_tempFile.close();
      LittleFS.remove(SPLASH_TEMP_PATH);
    }
  }

  if(final && request->_tempFile)
    request->_tempFile.close();

  if(final && state)
    state->incomplete = false;
}

//
// Configuration backup: everything the receiver persists, as a text file.
//

static void webBackupDownload(AsyncWebServerRequest *request)
{
  if(!webIsAuthenticated(request)) return request->requestAuthentication();

  const String body = backupBuild();
  AsyncWebServerResponse *response = request->beginResponse(200, "text/plain; charset=utf-8", body);
  if(!response) return request->send(503, "text/plain", "无法生成配置文件。");

  // Suffix with the tail of the MAC so backups from two receivers on the same
  // computer do not overwrite each other in the downloads folder.
  String name = getMACAddress();
  name.replace(":", "");
  if(name.length() > 4) name = name.substring(name.length() - 4);
  String filename = "ats-mini-config-" + name + ".txt";

  response->addHeader("Content-Disposition", String("attachment; filename=\"") + filename + "\"");
  response->addHeader("Cache-Control", "no-store");
  response->addHeader("Connection", "close");
  request->send(response);
}

static void webUploadBackup(AsyncWebServerRequest *request, const String &,
                            size_t index, uint8_t *data, size_t len, bool final)
{
  if(request->getResponse()) return;
  if(!webAllowWrite(request)) return;

  if(index == 0)
  {
    LittleFS.remove(BACKUP_TMP_PATH);

    BackupUploadState *state = new(std::nothrow) BackupUploadState();
    if(!state) return;
    request->_tempObject = state;
    request->onDisconnect([request, state]() {
      if(state->incomplete) request->_tempFile.close();
      // Removed again on purpose: the main task deletes it right after a
      // successful parse, so this only cleans up failed or abandoned uploads.
      LittleFS.remove(BACKUP_TMP_PATH);
      request->_tempObject = nullptr;
      delete state;
    });

    request->_tempFile = LittleFS.open(BACKUP_TMP_PATH, "w");
    state->incomplete = !!request->_tempFile;
    if(!state->incomplete) state->failed = true;
  }

  BackupUploadState *state = static_cast<BackupUploadState *>(request->_tempObject);
  if(!state) return;

  if(request->_tempFile && len)
  {
    if((index + len) > BACKUP_MAX_BYTES)
    {
      state->tooLarge = true;
      state->incomplete = false;
      request->_tempFile.close();
      LittleFS.remove(BACKUP_TMP_PATH);
    }
    else if(request->_tempFile.write(data, len) != len)
    {
      state->failed = true;
      state->incomplete = false;
      request->_tempFile.close();
      LittleFS.remove(BACKUP_TMP_PATH);
    }
  }

  if(final && request->_tempFile) request->_tempFile.close();
  if(final) state->incomplete = false;
}

static void webRestoreComplete(AsyncWebServerRequest *request)
{
  if(!webAllowWrite(request)) return;
  if(request->getResponse()) return;

  BackupUploadState *state = static_cast<BackupUploadState *>(request->_tempObject);
  const char *error = nullptr;
  if(state && state->tooLarge) error = "文件超过 64 KB，不像是配置文件。";
  else if(state && state->failed) error = "无法写入临时文件，请重试。";
  else if(!backupRequestRestore()) error = "已经有一个导入正在进行，请等待设备重启。";

  if(!error)
  {
    // The main task owns NVS writes. Wait for it here so the upload context
    // (and its cleanup) cannot delete the file before it has been parsed.
    uint32_t started = millis();
    while(backupRestorePending() && (uint32_t)(millis() - started) < 3000) delay(10);
    if(backupRestorePending()) error = "设备忙，导入未开始，请重试。";
    else if(!backupRestoreDone()) error = backupRestoreError();
  }

  String message;
  int code = 200;
  if(error)
  {
    message = error;
    code = 400;
  }
  else
  {
    message = "已导入 " + String(backupRestoreApplied()) + " 项设置";
    if(backupRestoreSkipped())
      message += "，跳过 " + String(backupRestoreSkipped()) + " 行无法识别的内容";
    message += "。设备将在几秒内自动重启，重启后按新的设置工作。";
  }

  const String page = webPage(pageRestore, pageRestoreTitle, {
    {"title", pageRestoreTitle}, {"message", message}
  }, {
    {"navigation", [](String &out) { webNavigation(out, "/config"); }}
  });
  AsyncWebServerResponse *response = request->beginResponse(code, "text/html", page);
  if(!response) return request->send(503, "text/plain", "内存不足，请重试。");
  response->addHeader("Cache-Control", "no-store");
  response->addHeader("Connection", "close");
  request->send(response);
}

static bool webConfigNumber(AsyncWebServerRequest *request, const char *name, int &value,
                            int minimum, int maximum, int step = 1)
{
  if(!request->hasParam(name, true)) return false;
  const String &text = request->getParam(name, true)->value();
  if(!text.length()) return false;
  int number = 0;
  for(size_t i = 0; i < text.length(); i++)
  {
    if(text[i] < '0' || text[i] > '9') return false;
    number = number * 10 + text[i] - '0';
    if(number > maximum) return false;
  }
  if(number < minimum || (number - minimum) % step) return false;
  value = number;
  return true;
}

void webSetConfig(AsyncWebServerRequest *request)
{
  if(!webAllowWrite(request)) return;

  int layout, region, stereo, brightness, sleepTimer, sleepMode, offTimer;
  int usbMode, tcpMode, bleMode, rds;
  int utcOffset, themeIndex;
  if(!webConfigNumber(request, "uilayout", layout, 0, getTotalUILayouts() - 1) ||
     !webConfigNumber(request, "fmregion", region, 0, getTotalFMRegions() - 1) ||
     !webConfigNumber(request, "fmstereo", stereo, 0, getTotalFMStereoModes() - 1) ||
     !webConfigNumber(request, "brightness", brightness, 10, 255, 5) ||
     !webConfigNumber(request, "sleeptimer", sleepTimer, 0, 255, 5) ||
     !webConfigNumber(request, "sleepmode", sleepMode, 0, getTotalSleepModes() - 1) ||
     !webConfigNumber(request, "offtimer", offTimer, 0, 240, 5) ||
     !webConfigNumber(request, "usbmode", usbMode, 0, getTotalUSBModes() - 1) ||
     !webConfigNumber(request, "tcpmode", tcpMode, 0, getTotalTCPModes() - 1) ||
     !webConfigNumber(request, "blemode", bleMode, 0, getTotalBluetoothModes() - 1) ||
     !webConfigNumber(request, "rds", rds, 0, getTotalRDSModes() - 1) ||
     !webConfigNumber(request, "utcoffset", utcOffset, 0, getTotalUTCOffsets() - 1) ||
     !webConfigNumber(request, "theme", themeIndex, 0, getTotalThemes() - 1))
    return request->send(400, "text/plain", "Invalid configuration value. Reload the Config page and try again.");

  uint32_t epoch;
  bool setClock = false;

  if(request->hasParam("datetime", true))
  {
    String dateTime = request->getParam("datetime", true)->value();
    if(dateTime != "")
    {
      if(!webParseDateTime(dateTime, &epoch))
        return request->send(400, "text/plain", "Date/time must use the YYYY-mm-dd HH:MM:SS format and contain a valid date and time.");
      // The form now takes local time, so shift it back to UTC before it
      // reaches the RTC. Use the offset submitted with this same request:
      // webTick() applies configSettings.utcOffset later, so utcOffsetIdx still
      // holds the previous zone here.
      int64_t utc = (int64_t)epoch - (int64_t)utcOffsets[utcOffset].offset * 15 * 60;
      if(utc <= 0)
        return request->send(400, "text/plain", "Date/time is out of range for the selected time zone.");
      epoch = (uint32_t)utc;
      setClock = true;
    }
  }

  if(request->hasParam("deletesplash", true))
  {
    LittleFS.remove(SPLASH_TEMP_PATH);
    LittleFS.remove(SPLASH_PATH);
  }
  else if(request->hasParam("splash", true, true))
  {
    String filename = request->getParam("splash", true, true)->value();

    if(filename != "")
    {
      SplashUploadState *state = static_cast<SplashUploadState *>(request->_tempObject);
      if(state && state->tooLarge)
        return request->send(413, "text/plain", "The splash image must not exceed 512 KB.");

      if(!filename.endsWith(".png"))
      {
        LittleFS.remove(SPLASH_TEMP_PATH);
        return request->send(400, "text/plain", "The splash image filename must end in .png.");
      }

      if(!LittleFS.exists(SPLASH_TEMP_PATH))
        return request->send(500, "text/plain", "The splash image could not be stored.");

      String error = splashValidate();
      if(error != "")
      {
        LittleFS.remove(SPLASH_TEMP_PATH);
        return request->send(400, "text/plain", error);
      }

      if(!LittleFS.rename(SPLASH_TEMP_PATH, SPLASH_PATH))
      {
        LittleFS.remove(SPLASH_TEMP_PATH);
        return request->send(500, "text/plain", "The splash image could not be installed.");
      }
    }
  }

  {
    std::lock_guard<std::mutex> lock(configSettings.mutex);
    configSettings.utcOffset = utcOffset;
    configSettings.themeIndex = themeIndex;
    configSettings.reverseScroll = request->hasParam("scroll", true);
    configSettings.zoom = request->hasParam("zoom", true);
    configSettings.halfStep = request->hasParam("encoderhalfstep", true);
    configSettings.rds = rds;
    configSettings.setClock = setClock;
    configSettings.epoch = setClock? epoch : 0;
    configSettings.layout = layout;
    configSettings.region = region;
    configSettings.stereo = stereo;
    configSettings.brightness = brightness;
    configSettings.sleepTimer = sleepTimer;
    configSettings.sleepMode = sleepMode;
    configSettings.offTimer = offTimer;
    configSettings.usbMode = usbMode;
    configSettings.tcpMode = tcpMode;
    configSettings.bleMode = bleMode;
    configSettings.clamp = request->hasParam("audioclamp", true);
    configSettings.pending = true;
  }

  // Show config page again
  request->redirect("/config");
}

static void webSetWiFi(AsyncWebServerRequest *request)
{
  if(!webAllowWrite(request)) return;

  int wifiMode;
  if(!webConfigNumber(request, "wifimode", wifiMode, 0, getTotalWiFiModes() - 1))
    return request->send(400, "text/plain", "Invalid Wi-Fi mode.");

  // Validate every credential before touching NVS: a rejected form must not
  // leave the earlier entries -- including the login pair below -- already
  // rewritten.
  // 802.11 limits: SSID is 0..32 octets (empty clears the slot), a WPA
  // passphrase is 8..63 or empty for an open network. Without this an
  // over-long value is stored truncated and the receiver then fails to
  // associate with a password that still looks correct on the page.
  for(int j=0 ; j<3 ; j++)
  {
    char nameSSID[16], namePASS[16];

    sprintf(nameSSID, "wifissid%d", j+1);
    sprintf(namePASS, "wifipass%d", j+1);

    if(!request->hasParam(nameSSID, true) || !request->hasParam(namePASS, true)) continue;

    const String &ssid = request->getParam(nameSSID, true)->value();
    const String &pass = request->getParam(namePASS, true)->value();
    if(ssid.length() > 32 || pass.length() > 63 || (pass.length() && pass.length() < 8))
      return request->send(400, "text/plain",
        "SSID 需为 1-32 字节，密码需为空（开放网络）或 8-63 字节。");
  }

  webPrefs.begin("network", false, STORAGE_PARTITION);

  // Save user name and password
  if(request->hasParam("username", true) && request->hasParam("password", true))
  {
    String user = request->getParam("username", true)->value();
    String pass = request->getParam("password", true)->value();

    {
      std::lock_guard<std::mutex> lock(authMutex);
      loginUsername = user;
      loginPassword = pass;
    }

    webPrefs.putString("loginusername", user);
    webPrefs.putString("loginpassword", pass);
  }

  // Save SSIDs and their passwords
  bool haveSSID = false;
  for(int j=0 ; j<3 ; j++)
  {
    char nameSSID[16], namePASS[16];

    sprintf(nameSSID, "wifissid%d", j+1);
    sprintf(namePASS, "wifipass%d", j+1);

    if(request->hasParam(nameSSID, true) && request->hasParam(namePASS, true))
    {
      String ssid = request->getParam(nameSSID, true)->value();
      String pass = request->getParam(namePASS, true)->value();
      webPrefs.putString(nameSSID, ssid);
      webPrefs.putString(namePASS, pass);
      haveSSID |= ssid != "" && pass != "";
    }
  }

  // Save hidden SSID scanning preference
  bool wifiScanHidden = request->hasParam("wifiscanhidden", true);
  netSetScanHidden(wifiScanHidden);
  webPrefs.putBool("wifiscanhidden", wifiScanHidden);

  webPrefs.end();

  if(wifiMode != wifiModeIdx)
  {
    // Finish the response before switching off or reconfiguring its network.
    AsyncWebServerResponse *response = request->beginResponse(200, "text/plain",
      "Settings saved. Wi-Fi mode is changing. Reconnect to the receiver if needed. "
      "Off and Sync Only do not provide a web interface.");
    response->addHeader("Connection", "close");
    request->onDisconnect([wifiMode]() { pendingWiFiMode.store(wifiMode); });
    request->send(response);
    return;
  }

  request->redirect("/wifi");

  // If we are currently in AP mode, and infrastructure mode requested,
  // and there is at least one SSID / PASS pair, request network connection
  if(haveSSID && (wifiModeIdx>NET_AP_ONLY) && (WiFi.status()!=WL_CONNECTED))
    netRequestConnect();
}

// Parses the form's YYYY-mm-dd HH:MM:SS by reading the fields as if they were
// UTC (that is clockUTCDateTimeToEpoch's contract). The page collects local
// time, so the caller subtracts the selected zone before storing the value.
static bool webParseDateTime(const String &text, uint32_t *epoch)
{
  int year, month, day, hour, minute, second;
  return(epoch && text.length() == 19 &&
         sscanf(text.c_str(), "%4d-%2d-%2d %2d:%2d:%2d",
                &year, &month, &day, &hour, &minute, &second) == 6 &&
         clockUTCDateTimeToEpoch(year, month, day, hour, minute, second, epoch));
}

static String webPage(const char *body, const char *title, std::initializer_list<PageValue> values,
                      std::initializer_list<PageFragment> fragments)
{
  String page;
  pageReserve(page, sizeof(pageStart) + strlen(title) + strlen(body) + sizeof(pageEnd) + 64);
  pageAppend(page, pageStart, {{"title", title}});
  pageAppend(page, body, values, fragments);
  pageReserve(page, sizeof(pageEnd) + 64);
  // Snapshot after allocating the page, before the HTTP response copies it.
  multi_heap_info_t heap, psram;
  heap_caps_get_info(&heap, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  heap_caps_get_info(&psram, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  pageAppend(page, pageEnd, {
    {"heap_used", String(heap.total_allocated_bytes / 1024.0, 1)},
    {"heap_free", String(heap.total_free_bytes / 1024.0, 1)},
    {"psram_used", String(psram.total_allocated_bytes / 1024.0, 1)},
    {"psram_free", String(psram.total_free_bytes / 1024.0, 1)}
  });
  return page;
}

static const String webRadioPage()
{
  String ip = "";
  String ssid = "";
  String receiverTime = "Not synchronized";
  int offsetMinutes = getCurrentUTCOffset() * 15;
  int offsetMagnitude = abs(offsetMinutes);
  char utcOffset[10];
  snprintf(utcOffset, sizeof(utcOffset), "UTC%c%02d:%02d",
           offsetMinutes < 0? '-' : '+', offsetMagnitude / 60, offsetMagnitude % 60);
  String freq = currentMode == FM?
    String(currentFrequency / 1000000.0) + "MHz "
  : String(currentFrequency / 1000.0) + "kHz ";

  if(clockAvailable())
  {
    time_t localTime = time(NULL) + offsetMinutes * 60;
    struct tm fields;
    gmtime_r(&localTime, &fields);
    char text[20];

    strftime(text, sizeof(text),
             clockGetDate(NULL, NULL, NULL, NULL)? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S",
             &fields);

    receiverTime = text;
  }

  receiverTime += " (" + String(utcOffset) + ")";

  if(WiFi.status()==WL_CONNECTED)
  {
    ip = WiFi.localIP().toString();
    ssid = WiFi.SSID();
  }
  else
  {
    ip = WiFi.softAPIP().toString();
    ssid = String(RECEIVER_NAME);
  }

  return webPage(pageStatus, pageStatusTitle, {
    {"title", pageStatusTitle},
    {"ip", ip}, {"ssid", ssid},
    {"mac", String(getMACAddress())}, {"version", String(getVersion(true))},
    {"time", receiverTime}, {"band", getCurrentBand()->bandName},
    {"frequency", freq}, {"mode", bandModeDesc[currentMode]},
    {"rssi", String(rssi)}, {"snr", String(snr)}, {"battery", String(batteryMonitor())}
  }, {
    {"navigation", [](String &out) { webNavigation(out, "/"); }}
  });
}

// A memory name is UTF-8: printable ASCII or well-formed multi-byte
// sequences, at most MEMORY_NAME_LEN - 1 bytes.
static bool webNameValid(const String &text)
{
  if(text.length() >= MEMORY_NAME_LEN) return false;

  size_t i = 0;
  while(i < text.length())
  {
    uint8_t c = (uint8_t)text[i];
    if(c < 0x80)
    {
      if(c < 0x20 || c == 0x7f) return false;
      i++;
      continue;
    }

    size_t length = (c & 0xe0)==0xc0? 2 : (c & 0xf0)==0xe0? 3 : (c & 0xf8)==0xf0? 4 : 0;
    if(!length) return false;
    if(i + length > text.length()) return false;
    for(size_t k=1 ; k<length ; k++)
      if(((uint8_t)text[i + k] & 0xc0)!=0x80) return false;
    i += length;
  }

  return true;
}

static void webSetMemories(AsyncWebServerRequest *request)
{
  if(!webAllowWrite(request)) return;

  // Validate a complete replacement before modifying any live slots.
  Memory *pending = static_cast<Memory *>(ps_calloc(getTotalMemories(), sizeof(Memory)));
  if(!pending) return request->send(503, "text/plain", "Not enough memory to save the slots.");
  const char *error = nullptr;
  int slot = 0;
  for(; slot<getTotalMemories() ; slot++)
  {
    String suffix(slot);
    const auto *name = request->getParam("name" + suffix, true);
    const auto *band = request->getParam("band" + suffix, true);
    const auto *freq = request->getParam("freq" + suffix, true);
    const auto *mode = request->getParam("mode" + suffix, true);
    if(!name || !band || !freq || !mode)
    {
      error = "Missing slot fields.";
      break;
    }

    Memory &mem = pending[slot];
    const String &label = name->value();
    if(!webNameValid(label)) error = "Names must be UTF-8 text of at most 8 Chinese (or 24 ASCII) characters.";
    if(error) break;
    memcpy(mem.name, label.c_str(), label.length());

    const String &number = freq->value();
    if(!number.length() || number.length()>10) error = "Invalid frequency in Hz.";
    uint64_t hz = 0;
    for(size_t i=0 ; !error && i<number.length() ; i++)
    {
      if(number[i]<'0' || number[i]>'9') error = "Invalid frequency in Hz.";
      else hz = hz * 10 + number[i] - '0';
    }
    if(hz>UINT32_MAX) error = "Frequency is out of range.";
    if(error) break;
    mem.freq = hz;

    mem.mode = AM;
    if(mode->value().length() || mem.freq)
    {
      mem.mode = 0xff;
      for(int i=0 ; i<getTotalModes() ; i++)
        if(mode->value() == bandModeDesc[i]) mem.mode = i;
      if(mem.mode == 0xff)
      {
        error = "Unknown mode.";
        break;
      }
    }

    mem.band = 0xff;
    for(int i=0 ; i<getTotalBands() ; i++)
    {
      if(band->value() != bands[i].bandName) continue;
      // Resolve duplicate band names using the frequency and mode.
      if(mem.freq && !isMemoryInBand(&bands[i], &mem)) continue;
      mem.band = i;
      break;
    }
    if(mem.band == 0xff)
    {
      if(!mem.freq && !band->value().length()) mem.band = 0;
      else
      {
        error = "Frequency or mode does not match the band.";
        break;
      }
    }
  }

  if(error)
  {
    free(pending);
    return request->send(400, "text/html", webPage(pageMemoryError, pageMemoryTitle, {
      {"title", pageMemoryTitle},
      {"slot", String(slot + 1)}, {"error", error}
    }));
  }

  setMemories(pending);
  free(pending);
  prefsRequestSave(SAVE_MEMORIES, true);
  request->redirect("/memory");
}

static const String webMemoryPage()
{
  return webPage(pageMemory, pageMemoryTitle, {
    {"title", pageMemoryTitle}, {"toolbar", pageMemoryToolbar}
  }, {
    {"navigation", [](String &out) { webNavigation(out, "/memory"); }},
    {"presets", [](String &out) {
      out += F("<DATALIST ID=\"station-names\">");
      for(int i=0 ; i<stationNamePresetCount ; i++)
      {
        out += F("<OPTION VALUE=\"");
        out += stationNamePresets[i];
        out += F("\">");
      }
      out += F("</DATALIST>");
    }},
    {"rows", [](String &out) {
      for(int j=0 ; j<MEMORY_COUNT ; j++)
      {
        const Memory mem = getMemory(j);
        const char *band = mem.freq && mem.band<getTotalBands()? bands[mem.band].bandName : "";
        char slot[4];
        snprintf(slot, sizeof(slot), "%02d", j+1);
        pageAppend(out, pageMemoryRow, {
          {"index", String(j)}, {"slot", slot}, {"name", mem.name}, {"frequency", String(mem.freq)},
          {"up_disabled", j==0? "DISABLED" : ""},
          {"down_disabled", j==getTotalMemories()-1? "DISABLED" : ""}
        }, {
          {"bands", [&](String &out) {
            for(int i=0 ; i<getTotalBands() ; i++)
            {
              bool duplicate = false;
              for(int k=0 ; k<i ; k++)
                if(!strcmp(bands[k].bandName, bands[i].bandName)) duplicate = true;
              if(duplicate) continue;
              pageAppend(out, pageMemoryOption, {
                {"value", bands[i].bandName}, {"selected", !strcmp(band, bands[i].bandName)? "SELECTED" : ""}
              });
            }
          }},
          {"modes", [&](String &out) {
            for(int i=0 ; i<getTotalModes() ; i++)
              pageAppend(out, pageMemoryOption, {
                {"value", bandModeDesc[i]}, {"selected", mem.freq && mem.mode==i? "SELECTED" : ""}
              });
          }}
        });
      }
    }}
  });
}

static void webConfigOptions(String &out, const char *const *labels, int count, int selected)
{
  for(int index = 0; index < count; index++)
  {
    pageAppend(out, pageConfigThemeOption, {
      {"value", String(index)}, {"selected", index==selected? "SELECTED" : ""}, {"label", labels[index]}
    });
  }
}

const String webConfigPage()
{
  int layout, region, stereo, brightness, sleepTimer, sleepMode, offTimer;
  int usbMode, tcpMode, bleMode, rds;
  int utcOffset, themeIndex;
  bool clamp, reverseScroll, halfStep, zoom;
  {
    // A redirect may arrive before the main task has applied the saved values.
    std::lock_guard<std::mutex> lock(configSettings.mutex);
    utcOffset = configSettings.pending ? configSettings.utcOffset : utcOffsetIdx;
    themeIndex = configSettings.pending ? configSettings.themeIndex : themeIdx;
    reverseScroll = configSettings.pending ? configSettings.reverseScroll : scrollDirection < 0;
    zoom = configSettings.pending ? configSettings.zoom : zoomMenu;
    halfStep = configSettings.pending ? configSettings.halfStep : encoderHalfStep;
    rds = configSettings.pending ? configSettings.rds : rdsModeIdx;
    layout = configSettings.pending ? configSettings.layout : uiLayoutIdx;
    region = configSettings.pending ? configSettings.region : FmRegionIdx;
    stereo = configSettings.pending ? configSettings.stereo : fmStereoIdx;
    brightness = configSettings.pending ? configSettings.brightness : currentBrt;
    sleepTimer = configSettings.pending ? configSettings.sleepTimer : currentSleep;
    sleepMode = configSettings.pending ? configSettings.sleepMode : sleepModeIdx;
    offTimer = configSettings.pending ? configSettings.offTimer : offTimerMinutes;
    usbMode = configSettings.pending ? configSettings.usbMode : usbModeIdx;
    tcpMode = configSettings.pending ? configSettings.tcpMode : tcpModeIdx;
    bleMode = configSettings.pending ? configSettings.bleMode : bleModeIdx;
    if(bleMode == BLE_UNPAIR_ALL) bleMode = BLE_OFF;
    clamp = configSettings.pending ? configSettings.clamp : audioClamp;
  }
  String splashResolution = String(spr.width()) + "x" + String(spr.height());

  return webPage(pageConfig, pageConfigTitle, {
    {"title", pageConfigTitle},
    {"brightness", String(brightness)}, {"sleep_timer", String(sleepTimer)},
    {"off_timer", String(offTimer)}, {"audio_clamp", clamp? "CHECKED" : ""},
    {"scroll", reverseScroll? "CHECKED" : ""},
    {"half_step", halfStep? "CHECKED" : ""}, {"zoom", zoom? "CHECKED" : ""},
    {"resolution", splashResolution}
  }, {
    {"navigation", [](String &out) { webNavigation(out, "/config"); }},
    {"usb_options", [&](String &out) { webConfigOptions(out, usbModeDesc, getTotalUSBModes(), usbMode); }},
    {"tcp_options", [&](String &out) { webConfigOptions(out, tcpModeDesc, getTotalTCPModes(), tcpMode); }},
    {"ble_options", [&](String &out) { webConfigOptions(out, bleModeDesc, getTotalBluetoothModes(), bleMode); }},
    {"layout_options", [&](String &out) { webConfigOptions(out, uiLayoutDesc, getTotalUILayouts(), layout); }},
    {"rds_options", [&](String &out) {
      for(int i=0 ; i<getTotalRDSModes(); i++)
        pageAppend(out, pageConfigThemeOption, {
          {"value", String(i)}, {"selected", rds==i? "SELECTED" : ""}, {"label", rdsMode[i].desc}
        });
    }},
    {"region_options", [&](String &out) {
      for(int i=0 ; i<getTotalFMRegions(); i++)
        pageAppend(out, pageConfigThemeOption, {
          {"value", String(i)}, {"selected", region==i? "SELECTED" : ""}, {"label", fmRegions[i].desc}
        });
    }},
    {"stereo_options", [&](String &out) { webConfigOptions(out, fmStereoDesc, getTotalFMStereoModes(), stereo); }},
    {"sleep_options", [&](String &out) { webConfigOptions(out, sleepModeDesc, getTotalSleepModes(), sleepMode); }},
    {"utc_options", [&](String &out) {
      for(int i=0 ; i<getTotalUTCOffsets(); i++)
        pageAppend(out, pageConfigUtcOption, {
          {"value", String(i)}, {"minutes", String(utcOffsets[i].offset * 15)},
          {"selected", utcOffset==i? "SELECTED" : ""}, {"label", utcOffsets[i].desc}
        });
    }},
    {"theme_options", [&](String &out) {
      for(int i=0 ; i<getTotalThemes(); i++)
        pageAppend(out, pageConfigThemeOption, {
          {"value", String(i)}, {"selected", themeIndex==i? "SELECTED" : ""}, {"label", theme[i].name}
        });
    }},
    {"splash", [](String &out) {
      if(LittleFS.exists(SPLASH_PATH)) pageAppend(out, pageSplash, {{"version", String(millis())}});
      else out += "Not installed";
    }}
  });
}

static const String webWiFiPage()
{
  String user, pass;
  {
    std::lock_guard<std::mutex> lock(authMutex);
    user = loginUsername;
    pass = loginPassword;
  }

  webPrefs.begin("network", true, STORAGE_PARTITION);
  String ssid1 = webPrefs.getString("wifissid1", "");
  String pass1 = webPrefs.getString("wifipass1", "");
  String ssid2 = webPrefs.getString("wifissid2", "");
  String pass2 = webPrefs.getString("wifipass2", "");
  String ssid3 = webPrefs.getString("wifissid3", "");
  String pass3 = webPrefs.getString("wifipass3", "");
  bool scanHidden = webPrefs.getBool("wifiscanhidden", false);
  webPrefs.end();

  return webPage(pageWiFi, pageWiFiTitle, {
    {"title", pageWiFiTitle},
    {"username", user}, {"password", pass},
    {"ssid1", ssid1}, {"pass1", pass1}, {"ssid2", ssid2}, {"pass2", pass2},
    {"ssid3", ssid3}, {"pass3", pass3}, {"scan_hidden", scanHidden? "CHECKED" : ""}
  }, {
    {"navigation", [](String &out) { webNavigation(out, "/wifi"); }},
    {"wifi_options", [](String &out) {
      webConfigOptions(out, wifiModeDesc, getTotalWiFiModes(), wifiModeIdx);
    }}
  });
}

// Explicit request errors leave the active operation's status unchanged.
static void webUpdatePage(AsyncWebServerRequest *request, const OtaStatus &status, int code)
{
  const bool busy = status.phase == OTA_CHECK_QUEUED || status.phase == OTA_QUEUED ||
                    status.phase == OTA_CONNECTING || status.phase == OTA_WRITING;
  const bool complete = status.phase == OTA_COMPLETE || status.phase == OTA_REBOOT_PENDING;
  const bool available = status.phase == OTA_AVAILABLE;
  const char *refresh = complete? pageUpdateComplete : busy? pageUpdateBusy : "";
  const String page = webPage(pageUpdate, pageUpdateTitle, {
    {"title", pageUpdateTitle},
    {"message", status.message},
    {"action", available? "install" : "check"}, {"disabled", busy || complete? "DISABLED" : ""},
    {"button", available? "Update" : "Check for updates"}, {"refresh", refresh}
  }, {
    {"navigation", [](String &out) { webNavigation(out, "/update"); }}
  });
  if(!code) code = status.phase == OTA_FAILED? 400 : 200;
  AsyncWebServerResponse *response = request->beginResponse(code, "text/html", page);
  if(!response) return request->send(503, "text/plain", "内存不足，请重试。");
  response->addHeader("Cache-Control", "no-store");
  response->addHeader("Connection", "close");
  request->send(response);
}

static PatchUpload *webPatchBegin(AsyncWebServerRequest *request)
{
  if(request->_tempObject) return static_cast<PatchUpload *>(request->_tempObject);
  PatchUpload *state = new(std::nothrow) PatchUpload();
  if(!state) return nullptr;
  request->_tempObject = state;
  request->onDisconnect([request, state]() {
    // Destroy the C++ upload context before the server frees _tempObject.
    request->_tempObject = nullptr;
    delete state;
  });
  if(!request->hasParam("slot", true) || !request->hasParam("action", true))
  {
    state->error = "Missing patch slot or action.";
    return state;
  }
  String slot = request->getParam("slot", true)->value();
  String action = request->getParam("action", true)->value();
  if(slot.length() != 1 || slot[0] < '0' || slot[0] > '3' ||
     action != "upload")
  {
    state->error = "Invalid patch slot or action.";
    return state;
  }
  uint8_t mode = PATCH_MODE_COUNT;
  if(request->hasParam("mode", true))
    for(uint8_t i = 0; i < PATCH_MODE_COUNT; i++)
      if(request->getParam("mode", true)->value() == patchModeNames[i]) mode = i;
  state->begin(slot[0] - '0', static_cast<PatchMode>(mode));
  return state;
}

static void webUploadPatch(AsyncWebServerRequest *request, const String &filename,
                           size_t index, uint8_t *data, size_t len, bool final)
{
  if(request->getResponse()) return;
  if(!webAllowWrite(request)) return;
  if(filename.isEmpty() && index == 0 && len == 0 && final) return;
  PatchUpload *state = webPatchBegin(request);
  if(!state) return request->send(503, "text/plain", "Not enough memory to process the patch set.");
  if(state->error) return;
  if(index == 0)
  {
    String extension = filename;
    extension.toLowerCase();
    if(!extension.endsWith(".bin"))
    {
      state->error = "Upload exactly one .bin patch file.";
      return;
    }
  }

  state->write(index, data, len, final);
}

static void webPatchesComplete(AsyncWebServerRequest *request)
{
  if(!webAllowWrite(request)) return;
  if(request->getResponse()) return;
  if(request->hasParam("delete", true))
  {
    PatchUpload *state = static_cast<PatchUpload *>(request->_tempObject);
    if(state)
    {
      // Discard any uploaded file and release its reservation before deletion.
      state->error = "Upload discarded.";
      state->finish();
    }
    const char *error = nullptr;
    String slot = request->hasParam("slot", true)? request->getParam("slot", true)->value() : "";
    if(slot.length() != 1 || slot[0] < '1' || slot[0] > '3') error = "Invalid patch slot.";
    else patchesDelete(slot[0] - '0', error);
    if(error) return request->send(400, "text/plain", error);
    return request->redirect("/patches");
  }
  if(!request->_tempObject)
  {
    if(!request->hasParam("slot", true) || !request->hasParam("action", true))
      return request->send(400, "text/plain", "Missing patch slot or action.");
    String slot = request->getParam("slot", true)->value();
    String action = request->getParam("action", true)->value();
    const char *error = nullptr;
    if(slot.length() != 1 || slot[0] < '0' || slot[0] > '3') error = "Invalid patch slot.";
    else if(action == "select") patchesRequestSelection(slot[0] - '0', error);
    else error = "Upload one complete .bin patch.";
    if(error) return request->send(400, "text/plain", error);
    return request->redirect("/patches");
  }
  PatchUpload *state = static_cast<PatchUpload *>(request->_tempObject);
  state->finish();
  if(state->error)
    return request->send(400, "text/plain", state->error);
  request->redirect("/patches");
}

static void webPatchesPage(AsyncWebServerRequest *request)
{
  if(!webIsAuthenticated(request)) return request->requestAuthentication();
  bool busy;
  uint8_t selected;
  patchesSnapshot(selected, busy);
  const String page = webPage(pagePatches, pagePatchesTitle, {
    {"title", pagePatchesTitle}, {"busy", busy? pagePatchesBusy : ""},
    {"disabled", busy? "DISABLED" : ""}
  }, {
    {"navigation", [](String &out) { webNavigation(out, "/patches"); }},
    {"options", [&](String &out) {
      for(uint8_t slot = 0; slot <= PATCH_SET_COUNT; slot++)
      {
        if(slot && !patchesModes(slot)) continue;
        pageAppend(out, pagePatchOption, {
          {"value", String(slot)}, {"selected", slot == selected? "SELECTED" : ""}, {"label", patchSetNames[slot]}
        });
      }
    }},
    {"sets", [&](String &out) {
      for(uint8_t slot = 1; slot <= PATCH_SET_COUNT; slot++)
      {
        const char *disabled = busy || selected == slot? "DISABLED" : "";
        uint8_t modes = patchesModes(slot);
        String installed;
        for(uint8_t mode = 0; mode < PATCH_MODE_COUNT; mode++)
        {
          if(mode) installed += " · ";
          installed += patchModeNames[mode];
          installed += (modes & (1U << mode))? ": uploaded" : ": Default";
        }
        pageAppend(out, pagePatchSet, {
          {"slot", String(slot)}, {"name", patchSetNames[slot]}, {"installed", installed}, {"disabled", disabled}
        }, {
          {"options", [](String &out) {
            for(uint8_t mode = 0; mode < PATCH_MODE_COUNT; mode++)
              pageAppend(out, pagePatchOption, {
                {"value", patchModeNames[mode]}, {"selected", ""}, {"label", patchModeNames[mode]}
              });
          }},
          {"delete", [&](String &out) {
            if(modes) pageAppend(out, pagePatchDelete, {{"slot", String(slot)}, {"disabled", disabled}});
          }}
        });
      }
    }}
  });
  AsyncWebServerResponse *response = request->beginResponse(200, "text/html", page);
  if(!response) return request->send(503, "text/plain", "内存不足，请重试。");
  response->addHeader("Cache-Control", "no-store");
  if(busy) response->addHeader("Refresh", "2; url=/patches");
  request->send(response);
}
