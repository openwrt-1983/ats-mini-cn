#include "BlePeripheral.h"

#ifndef BLE_POWER_LEVEL
#define BLE_POWER_LEVEL ESP_PWR_LVL_N0
#endif

void BlePeripheral::begin(const char* deviceName)
{
  if (started) return;

  BLEDevice::init(deviceName);
  BLEDevice::setPower(BLE_POWER_LEVEL);
  configureDefaults();
  configureSecurity();

  BLEServer* currentServer = server();
  if (currentServer == nullptr)
    currentServer = BLEDevice::createServer();

  BLEAdvertising* advertising = BLEDevice::getAdvertising();
  currentServer->setCallbacks(this);
  createServices();
  configureAdvertising(*advertising);
  advertising->start();
  started = true;
}

void BlePeripheral::end()
{
  if (!started) return;

  started = false;
  BLEServer* currentServer = server();
  if (currentServer)
  {
    currentServer->getAdvertising()->stop();
    std::map<uint16_t, conn_status_t> peers = currentServer->getPeerDevices(false);
    for (auto const& peer : peers)
      currentServer->disconnect(peer.first);
  }

  destroyServices();
}

bool BlePeripheral::isStarted() const
{
  return started;
}

bool BlePeripheral::isConnected() const
{
  return connectedCount() > 0;
}

uint8_t BlePeripheral::connectedCount() const
{
  BLEServer* currentServer = server();
  return currentServer ? currentServer->getConnectedCount() : 0;
}

BLEServer* BlePeripheral::server() const
{
  return BLEDevice::getServer();
}

void BlePeripheral::onDisconnect(BLEServer* server, ble_gap_conn_desc* desc)
{
  if(!started) return;

  // The BLE stack may reject an immediate advertising restart while the
  // disconnect is still being processed (a common cause of "the device can
  // no longer be found after the app exits"). Try right away, then the main
  // loop tick() keeps retrying until advertising is really active.
  scheduleAdvertisingRestart(200);
  server->getAdvertising()->start();
}

void BlePeripheral::scheduleAdvertisingRestart(uint32_t delayMs)
{
  uint32_t at = millis() + delayMs;
  if(!advRestartAtMs || (int32_t)(at - advRestartAtMs) < 0)
    advRestartAtMs = at;
}

void BlePeripheral::tick()
{
  if(!started) return;

  BLEServer* currentServer = server();
  if(currentServer == nullptr) return;

  if(currentServer->getConnectedCount())
  {
    advRestartAtMs = 0;
    return;
  }

  BLEAdvertising* advertising = currentServer->getAdvertising();
  if(advertising == nullptr) return;
  if(advertising->isAdvertising())
  {
    advRestartAtMs = 0;
    return;
  }

  uint32_t now = millis();
  if(advRestartAtMs && (int32_t)(now - advRestartAtMs) < 0) return;
  if((int32_t)(now - lastAdvAttemptMs) < 250) return;

  lastAdvAttemptMs = now;
  advertising->start();
}
