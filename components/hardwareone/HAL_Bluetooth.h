/**
 * HAL_Bluetooth — controller and stack lifecycle for the shared BLE application.
 * GATT services, application authentication and role ownership stay in Bluetooth.
 * Native targets use the local controller; Hosted targets use its acknowledged
 * remote lifecycle. A Hosted acknowledgement is not a live controller query.
 */
#ifndef HAL_BLUETOOTH_H
#define HAL_BLUETOOTH_H

#include "System_BuildConfig.h"
#include <stdint.h>
#include "esp_err.h"

#if ENABLE_BLUETOOTH

enum class BluetoothHalBackend : uint8_t { Native, Hosted };
enum class BluetoothHalControllerState : int8_t {
  Unknown = -1, Idle = 0, Initialized = 1, Enabled = 2
};

struct BluetoothHalStatus {
  BluetoothHalBackend backend;
  BluetoothHalControllerState controller;
  bool controllerStateIsLive;
  bool hostEnabled;
  bool hostUninitialized;
  bool stackInitialized;

  bool ready() const {
    return stackInitialized && hostEnabled &&
           controller == BluetoothHalControllerState::Enabled;
  }

  // A disabled but still initialized host can retain callback routes. Only a
  // terminal host/controller generation may release quarantined app clients.
  bool stopped() const {
    return !stackInitialized && hostUninitialized &&
           controller == BluetoothHalControllerState::Idle;
  }
};

// Preserve useful teardown diagnostics without exposing a backend's C++ type.
struct BluetoothHalShutdownResult {
  bool success;
  uint8_t phase;
  int32_t hostStatusBefore;
  int32_t hostStatusAfter;
  int32_t controllerStatusBefore;
  int32_t controllerStatusAfter;
  bool clientQuarantined;
};

// Caller owns the existing BLE role-transition token. Neither operation may run
// in a BLE callback. Stop retains controller memory for restart and, when hosted,
// releases only the BLE use of the transport; Wi-Fi/ESP-NOW remain alive.
bool bluetoothHalInit(const char* deviceName);
BluetoothHalShutdownResult bluetoothHalDeinit();
BluetoothHalStatus bluetoothHalStatus();

// Existing setting scale: 0..7. Unsupported backends never claim to apply it.
bool bluetoothHalSupportsTxPower();
esp_err_t bluetoothHalSetTxPower(uint8_t level);

#endif // ENABLE_BLUETOOTH
#endif // HAL_BLUETOOTH_H
