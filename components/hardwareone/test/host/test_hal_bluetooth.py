#!/usr/bin/env python3
"""Compile the REAL HAL_Bluetooth.cpp against controlled native and Hosted
driver substitutes and check the app/backend contract.

Ported from experiments/p4_connectivity/test_hal_bluetooth.py so the HAL that
ships on every Bluetooth board is covered by the tracked suite. The checks
prove the interface (incomplete startup, uncertain remote state, failed or
incomplete teardown, restart, native power error propagation and explicit
Hosted power unavailability); they do not prove controller behaviour, Wi-Fi
coexistence or C6 recovery.

The HAL sources are copied into a temporary directory with the stub headers
because HAL_Bluetooth.h includes "System_BuildConfig.h" with quotes, which
would otherwise resolve to the real firmware header beside it.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

SOURCE = Path(__file__).resolve().parents[2]

STUBS = {
    "System_BuildConfig.h": "#pragma once\n#define ENABLE_BLUETOOTH 1\n",
    "esp_err.h": """#pragma once
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_ERR_INVALID_ARG=1, ESP_ERR_NOT_SUPPORTED=2;
""",
    "esp_bt_main.h": """#pragma once
extern int mockHost;
using esp_bluedroid_status_t = int;
constexpr int ESP_BLUEDROID_STATUS_UNINITIALIZED=0, ESP_BLUEDROID_STATUS_ENABLED=2;
inline int esp_bluedroid_get_status() { return mockHost; }
""",
    "esp32-hal-bt.h": """#pragma once
extern int mockController;
inline int btHostedControllerStatus() { return mockController; }
""",
    "esp_bt.h": """#pragma once
#include "esp_err.h"
extern int mockController, powerCalls, powerFailAt;
enum esp_power_level_t { ESP_PWR_LVL_N12, ESP_PWR_LVL_N9, ESP_PWR_LVL_N6, ESP_PWR_LVL_N3,
                         ESP_PWR_LVL_N0, ESP_PWR_LVL_P3, ESP_PWR_LVL_P6, ESP_PWR_LVL_P9 };
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
  return 0;
}
"""


def compile_and_run(cxx: str, sanitize: bool, hosted: bool) -> None:
    with tempfile.TemporaryDirectory(prefix="hw1-ble-hal-") as directory:
        work = Path(directory)
        for filename in ("HAL_Bluetooth.cpp", "HAL_Bluetooth.h"):
            shutil.copy2(SOURCE / filename, work / filename)
        for filename, text in STUBS.items():
            (work / filename).write_text(text)
        (work / "checks.cpp").write_text(CHECKS)
        command = [cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   f"-DCONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID={int(hosted)}",
                   f"-DCONFIG_BT_CONTROLLER_ENABLED={int(not hosted)}",
                   "-I", str(work),
                   str(work / "HAL_Bluetooth.cpp"), str(work / "checks.cpp"),
                   "-o", str(work / "checks")]
        if sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-g"]
        result = subprocess.run(command, capture_output=True, text=True, timeout=120)
        if result.returncode != 0:
            raise SystemExit(f"compile failed (hosted={hosted}):\n{result.stdout}{result.stderr}")
        result = subprocess.run([str(work / "checks")], capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            raise SystemExit(f"checks failed (hosted={hosted}):\n{result.stdout}{result.stderr}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=shutil.which("c++") or "c++")
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if not (SOURCE / "HAL_Bluetooth.cpp").is_file():
        raise SystemExit(f"HAL_Bluetooth.cpp not found under {SOURCE}")
    compile_and_run(args.cxx, args.sanitize, hosted=False)
    compile_and_run(args.cxx, args.sanitize, hosted=True)
    print("hal_bluetooth: native and hosted contracts pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
