"""Offline checks of the actual HAL source with controlled native/Hosted drivers.

These verify the app/backend contract, not C6 hardware or the Arduino adapter.
Reconstruct private/app first; missing copy or host C++ compiler produces a skip.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


SOURCE = Path(__file__).resolve().parent / "private/app/components/hardwareone"
COMPILER = shutil.which("c++")

STUBS = {
    "System_BuildConfig.h": "#pragma once\n#define ENABLE_BLUETOOTH 1\n",
    "esp_err.h": """#pragma once
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_ARG=1, ESP_ERR_NOT_SUPPORTED=2;
""",
    "esp_bt_main.h": """#pragma once
extern int mockHost;
constexpr int ESP_BLUEDROID_STATUS_ENABLED=2;
inline int esp_bluedroid_get_status() { return mockHost; }
""",
    "esp32-hal-bt.h": """#pragma once
extern int mockController;
inline int btHostedControllerStatus() { return mockController; }
""",
    "esp_bt.h": """#pragma once
#include "esp_err.h"
extern int mockController, powerCalls, powerFailAt;
using esp_power_level_t=int;
enum { ESP_BLE_PWR_TYPE_DEFAULT, ESP_BLE_PWR_TYPE_ADV, ESP_BLE_PWR_TYPE_SCAN };
inline int esp_bt_controller_get_status() { return mockController; }
inline esp_err_t esp_ble_tx_power_set(int, esp_power_level_t) {
  return ++powerCalls == powerFailAt ? 91 : ESP_OK;
}
""",
    "BLEDevice.h": """#pragma once
#include <stdint.h>
#include <assert.h>
extern int mockHost, mockController, initCalls;
extern bool mockInitialized, initFail, stopFail, stopLeavesController;
struct BLEDeviceDeinitResult {
  bool success;
  uint8_t phase;
  int32_t hostStatusBefore, hostStatusAfter, controllerStatusBefore, controllerStatusAfter;
  bool clientQuarantined;
};
struct BLEDevice {
  static bool getInitialized() { return mockInitialized; }
  static void init(const char*) {
    ++initCalls;
    mockInitialized=true;
    mockHost=2;
    mockController=initFail ? -1 : 2;
  }
  static BLEDeviceDeinitResult deinitChecked(bool releaseMemory) {
    assert(!releaseMemory);
    const int oldHost=mockHost, oldController=mockController;
    if (!stopFail) {
      mockInitialized=false;
      mockHost=0;
      mockController=stopLeavesController ? 2 : 0;
    }
    return {!stopFail, 7, oldHost, mockHost, oldController, mockController, stopFail};
  }
};
""",
}

CHECKS = r"""
#include "HAL_Bluetooth.h"
#include <assert.h>
int mockHost=0, mockController=0, initCalls=0, powerCalls=0, powerFailAt=-1;
bool mockInitialized=false, initFail=false, stopFail=false, stopLeavesController=false;
int main() {
  assert(!bluetoothHalInit(nullptr));
  assert(!bluetoothHalInit(""));
  assert(initCalls==0);
  initFail=true;
  assert(!bluetoothHalInit("test")); // host initialized alone is insufficient
  assert(!bluetoothHalStatus().ready());
  initFail=false;
  assert(bluetoothHalInit("test"));
  assert(bluetoothHalStatus().ready());
#if CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID
  assert(bluetoothHalStatus().backend==BluetoothHalBackend::Hosted);
  assert(!bluetoothHalStatus().controllerStateIsLive);
  mockController=-1;
  assert(bluetoothHalStatus().controller==BluetoothHalControllerState::Unknown);
  assert(!bluetoothHalStatus().ready());
  mockController=2;
  assert(!bluetoothHalSupportsTxPower());
  assert(bluetoothHalSetTxPower(4)==ESP_ERR_NOT_SUPPORTED);
  assert(powerCalls==0);
#else
  assert(bluetoothHalStatus().backend==BluetoothHalBackend::Native);
  assert(bluetoothHalStatus().controllerStateIsLive);
  assert(bluetoothHalSupportsTxPower());
  assert(bluetoothHalSetTxPower(4)==ESP_OK && powerCalls==3);
  powerCalls=0; powerFailAt=2;
  assert(bluetoothHalSetTxPower(4)==91 && powerCalls==2);
#endif
  assert(bluetoothHalSetTxPower(8)==ESP_ERR_INVALID_ARG);
  stopFail=true;
  auto failed=bluetoothHalDeinit();
  assert(!failed.success && failed.clientQuarantined);
  assert(bluetoothHalStatus().ready());
  stopFail=false; stopLeavesController=true;
  assert(!bluetoothHalDeinit().success); // a success flag cannot hide a live controller
  stopLeavesController=false;
  assert(bluetoothHalDeinit().success);
  assert(!bluetoothHalStatus().ready());
  assert(bluetoothHalInit("restart"));
  assert(bluetoothHalDeinit().success);
}
"""


@unittest.skipUnless(COMPILER and (SOURCE / "HAL_Bluetooth.cpp").is_file(),
                     "Reconstructed connectivity copy and C++17 compiler required")
class BluetoothHalTests(unittest.TestCase):
    def compile_and_run(self, hosted: bool) -> None:
        with tempfile.TemporaryDirectory(prefix="hw1-ble-hal-") as directory:
            work = Path(directory)
            for filename in ("HAL_Bluetooth.cpp", "HAL_Bluetooth.h"):
                shutil.copy2(SOURCE / filename, work / filename)
            for filename, text in STUBS.items():
                (work / filename).write_text(text)
            (work / "checks.cpp").write_text(CHECKS)
            command = [COMPILER, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       f"-DCONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID={int(hosted)}",
                       f"-DCONFIG_BT_CONTROLLER_ENABLED={int(not hosted)}", "-I", str(work),
                       str(work / "HAL_Bluetooth.cpp"), str(work / "checks.cpp"),
                       "-o", str(work / "checks")]
            result = subprocess.run(command, capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(work / "checks")], capture_output=True, text=True, timeout=5)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_native_lifecycle_and_power(self):
        self.compile_and_run(False)

    def test_hosted_acknowledgement_and_no_power_api(self):
        self.compile_and_run(True)


if __name__ == "__main__":
    unittest.main()
