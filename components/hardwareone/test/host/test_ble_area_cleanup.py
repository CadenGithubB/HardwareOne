#!/usr/bin/env python3
"""BLE area dead code: connection typing, writable characteristics, removed helpers.

The BLE server types every connection (glasses temple, ring, phone, unknown) in
onConnect with bleIdentifyDeviceByMAC(); bledisconnect and
bleDisconnectAppClients() keep a link open only when it is typed as a G2 temple
or the R1 ring. The function and its MAC helpers are extracted unchanged from
Bluetooth.cpp and run against a host table standing in for the peer owner's
saved targets (BLE_Peers.h snapshot), so a re-pair must change the typing of
the next connection.

Source contracts, read from the same files:
* every characteristic Bluetooth.cpp creates with PROPERTY_WRITE has write
  callbacks (a writable characteristic without them acknowledges writes that do
  nothing);
* the uncalled Bluetooth helpers and the MAC classification cache are gone, and
  Bluetooth.h (also with ENABLE_BLUETOOTH=0) no longer declares them;
* G2_Ring.cpp no longer carries the unregistered ring-to-glasses spoof push and
  temple bridge (`ringtoglasses`, `ringbridge`, their tasks and state), while
  g2RingNoteBridgePoll() stays defined for the G2 receive path, and once the
  code is gone G2_Ring.cpp names the commit that still holds it (f4cc542).

--component points the suite at another copy of Bluetooth.cpp/.h and
G2_Ring.cpp/.h (the base tree) to show the pre-fix behavior.
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


def optional(source: str, marker: str) -> list[str]:
    return [extract_block(source, marker)] if marker in source else []


def generated(component: Path) -> str:
    owner = (component / "Bluetooth.cpp").read_text()
    peers = (COMPONENT / "BLE_Peers.h").read_text()
    utils = (COMPONENT / "System_Utils.h").read_text()

    constants = re.findall(r"^constexpr (?:size_t|uint8_t) BLE_PEER_(?:ADDRESS_TEXT_CAPACITY|"
                           r"DEFAULT_ADDRESS_TYPE) = [^;]+;", peers, re.MULTILINE)
    require(len(constants) == 2, "saved-target constants are present in BLE_Peers.h")
    types = [(COMPONENT / "BLE_Types.h").read_text(),
             extract_block(peers, "enum BlePeerKind") + ";",
             *constants,
             extract_block(peers, "struct BlePeerSavedTarget ") + ";",
             extract_block(peers, "struct BlePeerSavedTargetSnapshot") + ";"]

    definitions = [extract_block(utils, "inline void macToDisplay(")]
    cache = re.search(r"static struct \{[^}]*\} gBLEMACCache = [^;]*;", owner)
    if cache:
        print("Bluetooth.cpp keeps a MAC classification cache (pre-fix source)")
        definitions.append(cache.group())
    definitions += [extract_block(owner, "static void macToStackBuf("),
                    extract_block(owner, "static void copyMacUpper(")]
    definitions += optional(owner, "static bool bleSavedMacMatches(")
    definitions += optional(owner, "static void bleUpdateMACCache(")
    definitions += [extract_block(owner, "const char* bleDeviceTypeToString("),
                    extract_block(owner, "BLEDeviceType bleIdentifyDeviceByMAC(")]

    harness = (HERE / "ble_area_cleanup_harness.cpp").read_text()
    for marker, body in (("// INSERT_TYPES", types), ("// INSERT_PRODUCTION", definitions)):
        require(harness.count(marker) == 1, f"bad harness marker: {marker}")
        harness = harness.replace(marker, "\n\n".join(body))
    return harness


def writable_without_callbacks(owner: str) -> list[str]:
    created = re.findall(r"(\w+)\s*=\s*\w+->createCharacteristic\(\s*(\w+)\s*,([^;]*?)\);",
                         owner, re.DOTALL)
    require(len(created) >= 3, "Bluetooth.cpp creates its GATT characteristics")
    return [f"{pointer} ({uuid})" for pointer, uuid, props in created
            if "PROPERTY_WRITE" in props
            and not re.search(rf"\b{pointer}->setCallbacks\(", owner)]


def symbol_failures(component: Path) -> list[str]:
    failures: list[str] = []
    owner = (component / "Bluetooth.cpp").read_text()
    header = (component / "Bluetooth.h").read_text()
    ring = (component / "G2_Ring.cpp").read_text()
    ring_h = (component / "G2_Ring.h").read_text()

    silent = writable_without_callbacks(owner)
    if silent:
        failures.append("writable characteristic without write callbacks (writes are acknowledged "
                        "and ignored): " + ", ".join(silent))

    for symbol, why in (
            ("getBLEConnectionDuration", "uncalled helper"),
            ("isBLERunning", "uncalled helper"),
            ("getBLEStatus", "uncalled helper"),
            ("bleApplySettings", "uncalled helper (the only MAC-cache refresh)"),
            ("gBLEMACCache", "MAC classification cache filled once and never refreshed"),
            ("bleUpdateMACCache", "MAC classification cache filler"),
            ("pStreamControlChar", "write-only characteristic with no handler"),
            ("BLE_STREAM_CONTROL_CHAR_UUID", "write-only characteristic with no handler")):
        for name, text in (("Bluetooth.cpp", owner), ("Bluetooth.h", header)):
            if re.search(rf"\b{symbol}\b", text):
                failures.append(f"{name} still has {symbol} ({why})")

    for symbol in ("cmd_ringtoglasses", "cmd_ringbridge", "ringSpoofSendOnce", "ringSpoofTaskBody",
                   "ringSpoofStart", "ringSpoofStop", "gSpoofEnabled", "ringBridgeHeartbeatBody",
                   "ringBridgeHeartbeatStart", "gBridgeHbEnabled", "gBridgeRequested",
                   "gBridgeProgress", "ringEnqueueAdvStart", "RING_INTENT_ADV_START"):
        if re.search(rf"\b{symbol}\b", ring):
            failures.append(f"G2_Ring.cpp still has {symbol} (unregistered ring bridge/spoof code)")
    for task in ('"ring_spoof"', '"ring_bridge_hb"'):
        if task in ring:
            failures.append(f"G2_Ring.cpp still creates the unreachable task {task}")
    if not re.search(r"\bcmd_ringbridge\b", ring) and "f4cc542" not in ring:
        failures.append("G2_Ring.cpp dropped the ring bridge/spoof code without naming the commit "
                        "that still holds it (f4cc542), so nothing points to the experiment any more")
    if "void g2RingNoteBridgePoll(" not in ring:
        failures.append("G2_Ring.cpp no longer defines g2RingNoteBridgePoll (G2_Glasses.cpp calls it)")
    if "void g2RingNoteBridgePoll(" not in ring_h:
        failures.append("G2_Ring.h no longer declares g2RingNoteBridgePoll (G2_Glasses.cpp calls it)")
    return failures


DISABLED_CHECK = '''#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include "Bluetooth.h"
void webBatchTestStringAppend() {}
int main() {
  return !isBLEConnected() && !bleSubsystemActive() ? 0 : 1;
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="component directory whose Bluetooth.cpp/.h and G2_Ring.cpp/.h are tested")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]

    failures: list[str] = []
    with tempfile.TemporaryDirectory(prefix="hw1-ble-area-cleanup-") as directory:
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

        if not compile_and_run("connection-typing", generated(args.component), []):
            failures.append("connection typing does not follow the saved peer targets")

        disabled = work / "disabled-include"
        disabled.mkdir()
        shutil.copyfile(args.component / "Bluetooth.h", disabled / "Bluetooth.h")
        (disabled / "System_BuildConfig.h").write_text("#define ENABLE_BLUETOOTH 0\n")
        (disabled / "System_User.h").write_text(
            "#include <cstdint>\nusing TransportSessionEpoch = uint32_t;\n")
        if not compile_and_run("disabled", DISABLED_CHECK,
                               ["-I", str(disabled), "-I", str(JSON_INCLUDE)]):
            failures.append("Bluetooth.h with ENABLE_BLUETOOTH=0 does not compile")

    for failure in symbol_failures(args.component):
        print(f"FAIL [source]: {failure}")
        failures.append(failure)
    require(not failures, f"BLE area cleanup failed ({len(failures)}): " + "; ".join(failures))
    print("BLE area cleanup: connection typing follows the saved peer targets, every writable "
          "characteristic has write callbacks, removed helpers and ring bridge/spoof code absent, "
          "g2RingNoteBridgePoll kept, disabled header compiles")


if __name__ == "__main__":
    main()
