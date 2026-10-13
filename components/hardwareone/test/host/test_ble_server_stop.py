#!/usr/bin/env python3
"""Run the production BLE server stop and the `closeble` / `blemode` commands.

deinitBluetooth(), its session and advertising helpers, the role-claim and
lifecycle-guard classes and both command handlers are extracted unchanged from
Bluetooth.cpp; the role transition, Ring owner, host/controller teardown,
transport sessions and settings writes are host boundaries. A stop the Ring
owner or a busy role transition defers, or a host teardown that fails, must be
reported to the caller: both commands answer an error and leave `bleMode`
alone, and a deferred stop leaves the app sessions and advertising running,
except under a companion reset's RECOVERING claim, whose forced host teardown
follows: there a Ring-deferred stop closes session admission (epochs, GATT
interface id) and advertising before it returns.
Built with and without the G2 client; the disabled-Bluetooth header stub is
compiled too. --component points the suite at another copy of the BLE sources
(Bluetooth.cpp/.h) to show the pre-fix behavior.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require, JSON_INCLUDE

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generated(component: Path) -> str:
    owner = (component / "Bluetooth.cpp").read_text()
    header = (component / "Bluetooth.h").read_text()
    settings = (COMPONENT / "System_Settings.h").read_text()
    hal = (COMPONENT / "HAL_Bluetooth.h").read_text()

    modes = re.findall(r"^#define BLE_MODE_(?:SERVER|G2_CLIENT)\s+\d+", header, re.MULTILINE)
    require(len(modes) == 2, "BLE mode constants are present in Bluetooth.h")
    default = re.search(r"static constexpr int kBleModeDefaultForBuild = [^;]+;", settings)
    require(default is not None, "kBleModeDefaultForBuild is present in System_Settings.h")
    types = [*modes, default[0], (COMPONENT / "BLE_Types.h").read_text(),
             extract_block(header, "enum class BleRoleTransition") + ";",
             extract_block(hal, "struct BluetoothHalShutdownResult") + ";",
             extract_block(header, "struct BLEConnection") + ";",
             extract_block(header, "struct BLESystemState") + ";"]

    stop = "bool deinitBluetooth(" if "bool deinitBluetooth(" in owner else "void deinitBluetooth("
    if stop.startswith("void"):
        print("deinitBluetooth returns void (pre-fix source)")
    definitions = [extract_block(owner, "class BleRoleTransitionClaim") + ";",
                   extract_block(owner, "class BleLifecycleGuard") + ";"]
    definitions += [extract_block(owner, marker) for marker in (
        "int findConnectionSlotByConnIdLocked(",
        "void bleInvalidateSessionSlotLocked(",
        "TransportSessionEpoch bleSessionEpochForConnection(",
        "bool bleSessionEpochMatches(",
        "void stopBLEAdvertising(",
        "bool isBleServerInitialized(",
        stop,
        "static const char* cmd_blestop(",
        "static const char* cmd_blemode(")]
    harness = (HERE / "ble_server_stop_harness.cpp").read_text()
    return harness.replace("// INSERT_TYPES", "\n\n".join(types)).replace(
        "// INSERT_PRODUCTION", "\n\n".join(definitions))


DISABLED_CHECK = '''#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include "Bluetooth.h"
#include <cstdio>
#include <type_traits>
void webBatchTestStringAppend() {}
static_assert(std::is_same<decltype(deinitBluetooth()), bool>::value,
              "disabled deinitBluetooth() must report its result like the real one");
int main() {
  if (!deinitBluetooth()) {
    std::puts("FAIL [disabled]: deinitBluetooth() with Bluetooth compiled out must report stopped");
    return 1;
  }
  return 0;
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="component directory whose Bluetooth.cpp/.h are tested")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-function", "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="hw1-ble-server-stop-") as directory:
        work = Path(directory)
        shutil.copyfile(HERE / "web_batch_stubs" / "Arduino.h", work / "Arduino.h")

        def compile_and_run(name: str, source: str, extra: list[str]) -> bool:
            path = work / (name + ".cpp")
            path.write_text(source)
            built = subprocess.run([args.cxx, *flags, "-I", str(work), *extra, str(path),
                                    "-o", str(work / name)],
                                   text=True, capture_output=True, timeout=180)
            if built.returncode != 0:
                print(built.stdout + built.stderr)
                print(f"FAIL [{name}]: does not compile")
                return False
            ran = subprocess.run([str(work / name)], text=True, capture_output=True, timeout=60)
            print(ran.stdout + ran.stderr, end="")
            return ran.returncode == 0

        source = generated(args.component)
        for g2 in (1, 0):
            if not compile_and_run(f"server-stop-g2-{g2}", source, [f"-DENABLE_G2_GLASSES={g2}"]):
                failures.append(f"server stop (ENABLE_G2_GLASSES={g2})")

        disabled = work / "disabled-include"
        disabled.mkdir()
        shutil.copyfile(args.component / "Bluetooth.h", disabled / "Bluetooth.h")
        (disabled / "System_BuildConfig.h").write_text("#define ENABLE_BLUETOOTH 0\n")
        (disabled / "System_User.h").write_text(
            "#include <cstdint>\nusing TransportSessionEpoch = uint32_t;\n")
        if not compile_and_run("disabled", DISABLED_CHECK,
                               ["-I", str(disabled), "-I", str(JSON_INCLUDE)]):
            failures.append("disabled-Bluetooth deinitBluetooth() stub")
    require(not failures, "BLE server stop failed: " + "; ".join(failures))
    print("BLE server stop: deferred/failed stops reported, mode kept, sessions kept on deferral "
          "(closed under a companion reset); success, no-server and disabled builds passed")


if __name__ == "__main__":
    main()
