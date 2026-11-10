#ifndef UTILS_H
#define UTILS_H

#include "Common.h"

#define MUTE_FORCE   1
#define MUTE_MAIN    2
#define MUTE_SQUELCH 3
#define MUTE_TEMP    4

#define CLOCK_MIN_YEAR 2026

// Get firmware version
const char *getVersion(bool shorter = false);

// HTTP User-Agent for downloads
String getUserAgent();

// Hardware info
const char *getMACAddress();

// Convert RSSI to signal strength
int getStrength(int rssi);

// Set, reset, toggle, or query switches
bool sleepOn(int x = 2);
// True while the SI4732 is powered down by TV style standby: every radio
// access must be skipped until it has been powered back up.
bool radioInStandby();
// True when the display is off in Bluetooth standby, including a standby that
// was started by the standby timer rather than by the configured sleep mode.
bool sleepIsBleStandby();
// True when the display is off but Bluetooth keeps running, so the app can
// still wake the receiver (Bluetooth standby or CPU sleep).
bool sleepKeepsBle();
// Enter Bluetooth standby now, whatever the configured sleep mode is.
bool standbyEnter();
// Wake up and set the sleep mode to CPU sleep (SLEEP_LIGHT).
bool sleepWakeLight();
bool muteOn(uint8_t mode, int x = 2, bool muteAmp = false);
// Ramp the SI4732 digital volume to the target in small steps (click-free mute transitions)
void volumeFade(uint8_t target);
void receiverShutdown();
// Long press standby: deep sleep with the encoder button as the only wake
// source. Never returns; waking is a full reset that sets the receiver up
// again and restores the stored band and volume.
void standbySleepEnter();
// True when the unit was woken from standby. Releases the pad holds that the
// standby left behind, so call it before setup() configures any pin.
bool standbyWoke();
// After a standby wake: returns once the button has been held long enough.
// A press that is released too early goes straight back to standby, so this
// does not return in that case.
void standbyWakeConfirm();

// Configured duration is saved; a fresh countdown starts on power-up.
extern uint16_t offTimerMinutes;
void offTimerRestart();
uint16_t offTimerRemainingMinutes();
void offTimerTick();

// Standby timer: same units and limits as the off timer, but it enters
// Bluetooth standby instead of powering the receiver off.
extern uint16_t standbyTimerMinutes;
void standbyTimerRestart();
uint16_t standbyTimerRemainingMinutes();
void standbyTimerTick();

// Any user input -- a knob turn, a click, a command from the app, web or a
// serial terminal -- counts as activity: it renews the display sleep timer
// and the standby countdown. Defined in Utils.cpp; the timestamps live in
// the main sketch.
extern long elapsedSleep;
extern long elapsedCommand;
void touchActivity();

// Main task only. Strings are copied; duration 0 keeps the status until replaced or cleared.
// Pass nullptr for both lines to clear the status.
void statusShow(const char *line1, const char *line2 = nullptr, uint32_t duration = 2000);
bool statusTick(uint32_t now);
// Display reads these buffers directly; use statusShow() to update them.
extern char statusLines[2][96];

// Wall clock functions
const char *clockGet();
bool clockAvailable();
bool clockGetHM(uint8_t *hours, uint8_t *minutes);
bool clockGetDate(uint16_t *year, uint8_t *month, uint8_t *day, uint8_t *weekday);
bool clockUTCDateTimeToEpoch(int year, int month, int day, int hour, int minute, int second, uint32_t *epoch);
bool clockSetEpoch(uint32_t epoch);
void clockReset();
void clockTimeSynchronized();
bool clockUpdate();

// Helpers to convert from/to Hz
uint16_t freqFromHz(uint32_t freq, uint8_t mode);
uint32_t freqToHz(uint16_t freq, uint8_t mode);

// Check if given frequency in Hz belongs to a band
bool isFreqInBand(const Band *band, uint32_t freqHz);

#endif // UTILS_H
