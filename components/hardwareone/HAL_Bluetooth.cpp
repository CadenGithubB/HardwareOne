#include "HAL_Bluetooth.h"

#if ENABLE_BLUETOOTH

#include <BLEDevice.h>
#include <esp_bt_main.h>

#if CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID
#include <esp32-hal-bt.h>
#elif CONFIG_BT_CONTROLLER_ENABLED
#include <esp_bt.h>
#endif

BluetoothHalStatus bluetoothHalStatus() {
  BluetoothHalStatus status{};
  const esp_bluedroid_status_t host = esp_bluedroid_get_status();
  status.hostEnabled = host == ESP_BLUEDROID_STATUS_ENABLED;
  status.hostUninitialized = host == ESP_BLUEDROID_STATUS_UNINITIALIZED;
  status.stackInitialized = BLEDevice::getInitialized();
#if CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID
  status.backend = BluetoothHalBackend::Hosted;
  // Pinned Hosted has no remote controller-status RPC. The Arduino adapter
  // records only successful lifecycle acknowledgements, and -1 on uncertainty.
  const int32_t controller = btHostedControllerStatus();
  status.controller = controller >= 0 && controller <= 2
                          ? static_cast<BluetoothHalControllerState>(controller)
                          : BluetoothHalControllerState::Unknown;
  status.controllerStateIsLive = false;
#else
  status.backend = BluetoothHalBackend::Native;
#if CONFIG_BT_CONTROLLER_ENABLED
  status.controller = static_cast<BluetoothHalControllerState>(esp_bt_controller_get_status());
  status.controllerStateIsLive = true;
#else
  status.controller = BluetoothHalControllerState::Unknown;
  status.controllerStateIsLive = false;
#endif
#endif
  return status;
}

bool bluetoothHalInit(const char* deviceName) {
  if (!deviceName || !deviceName[0]) return false;
  BLEDevice::init(deviceName);
  return bluetoothHalStatus().ready();
}

BluetoothHalShutdownResult bluetoothHalDeinit() {
  // Arduino owns host callback retirement and controller shutdown ordering.
  // Never release controller memory: this application supports BLE off/on.
  const BLEDeviceDeinitResult native = BLEDevice::deinitChecked(false);
  const BluetoothHalStatus status = bluetoothHalStatus();
  return {
    native.success && status.stopped(),
    static_cast<uint8_t>(native.phase),
    native.hostStatusBefore, native.hostStatusAfter,
    native.controllerStatusBefore, native.controllerStatusAfter,
    native.clientQuarantined
  };
}

bool bluetoothHalSupportsTxPower() {
#if CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID
  return false; // No remote power setter in the pinned Hosted API.
#elif CONFIG_BT_CONTROLLER_ENABLED
  return true;
#else
  return false;
#endif
}

esp_err_t bluetoothHalSetTxPower(uint8_t level) {
  if (level > 7) return ESP_ERR_INVALID_ARG;
#if !CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID && CONFIG_BT_CONTROLLER_ENABLED
  // Persisted setting 0..7 means -12..+9 dBm. Controller enums differ by SoC:
  // ESP32 starts at -12 dBm, whereas S3's controller starts at -24 dBm.
  static constexpr esp_power_level_t levels[] = {
    ESP_PWR_LVL_N12, ESP_PWR_LVL_N9, ESP_PWR_LVL_N6, ESP_PWR_LVL_N3,
    ESP_PWR_LVL_N0, ESP_PWR_LVL_P3, ESP_PWR_LVL_P6, ESP_PWR_LVL_P9
  };
  const esp_power_level_t power = levels[level];
  esp_err_t result = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, power);
  if (result != ESP_OK) return result;
  result = esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, power);
  if (result != ESP_OK) return result;
  return esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_SCAN, power);
#else
  return ESP_ERR_NOT_SUPPORTED;
#endif
}

#endif // ENABLE_BLUETOOTH
