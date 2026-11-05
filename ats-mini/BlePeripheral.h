#ifndef BLE_PERIPHERAL_H
#define BLE_PERIPHERAL_H

#include <BLEDevice.h>
#include <BLEServer.h>
#include <host/ble_gap.h>

class BlePeripheral : protected BLEServerCallbacks {
public:
  BlePeripheral() = default;

  void begin(const char* deviceName);
  void end();

  bool isStarted() const;
  bool isConnected() const;
  uint8_t connectedCount() const;

  // Call regularly from the main loop: restarts advertising whenever the
  // device is idle and not advertising, so clients can always reconnect.
  void tick();

protected:
  virtual void configureDefaults() {}
  virtual void configureSecurity() {}
  virtual void createServices() {}
  virtual void destroyServices() {}
  virtual void configureAdvertising(BLEAdvertising& advertising) {}

  BLEServer* server() const;
  void onConnect(BLEServer* server, ble_gap_conn_desc* desc) override {}
  void onDisconnect(BLEServer* server, ble_gap_conn_desc* desc) override;
  void scheduleAdvertisingRestart(uint32_t delayMs);

private:
  bool started = false;
  uint32_t advRestartAtMs = 0;
  uint32_t lastAdvAttemptMs = 0;
};

#endif
