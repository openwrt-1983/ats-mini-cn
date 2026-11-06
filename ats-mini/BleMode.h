#ifndef BLE_MODE_H
#define BLE_MODE_H

#include "Remote.h"
#include "BleUartPeripheral.h"

void bleInit(uint8_t bleMode);
void bleStop();
int8_t getBleStatus();
bool bleLoop(uint8_t bleMode);
// Stretch the advertising interval while in TV style standby and restore the
// fast interval on wake, so the app still finds the device either way.
void bleStandbyAdvertising(bool slow);
// True from a held rotation until release, disconnect, or cancellation.
bool blePushAndRotate();
bool bleConsumeAbortPending(uint8_t bleMode);

#endif
