#ifndef BACKUP_H
#define BACKUP_H

#include <Arduino.h>

// Upload target for a restore. Written by the web task, consumed by the main
// task, so the NVS flush never happens from the async web context.
#define BACKUP_TMP_PATH   "/backup-restore.txt"
#define BACKUP_MAX_BYTES  (64U * 1024U)

//
// Build the complete configuration as a plain text file: global settings,
// bands (frequency / mode / step / bandwidth / SSB calibration), VFO slots,
// Wi-Fi credentials and the web login, plus all 99 memory channels.
//
String backupBuild();

//
// Called from the web task once an upload has been stored at BACKUP_TMP_PATH.
// Returns false when a restore is already in flight.
//
bool backupRequestRestore();

//
// Called from the main task. Parses and applies the uploaded file, writes it
// to NVS and schedules a restart so every subsystem picks the values up.
// Returns true when a restart has been scheduled.
//
bool backupTick();

// True between backupRequestRestore() and the end of the main task's attempt,
// so the web task can wait for the outcome before answering the browser.
bool backupRestorePending();

// Outcome of the last attempt, for the result page.
int backupRestoreApplied();
int backupRestoreSkipped();
const char *backupRestoreError();   // nullptr when the restore succeeded
bool backupRestoreDone();           // true once a restart has been scheduled

#endif // BACKUP_H
