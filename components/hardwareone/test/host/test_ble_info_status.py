#!/usr/bin/env python3
"""Run the production BLE status owner, CLI and HTTP handler with host boundaries.

Uses vendored ArduinoJson, extracted BLE types/state formatter/lifecycle guard,
and the actual disabled-feature header. Hardware, RTOS admission, HTTP transport
and allocation failures are substitutes. --baseline compares successful CLI
JSON/text output against a saved pre-change component directory.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require, JSON_INCLUDE

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generated(component: Path, baseline: bool) -> str:
    owner = (component / "Bluetooth.cpp").read_text()
    header = (component / "Bluetooth.h").read_text()
    web = (component / "WebPage_Bluetooth.cpp").read_text()
    g2 = (COMPONENT / "G2_Glasses.h").read_text()
    types = (COMPONENT / "BLE_Types.h").read_text()
    types += "\n" + extract_block((COMPONENT / "BLE_Peers.h").read_text(),
                                  "enum BlePeerKind") + ";"
    types += "\n" + "\n".join(extract_block(header, marker) + ";" for marker in
                                ("struct BLEConnection", "struct BLESystemState"))
    types += "\n" + "\n".join(extract_block(g2, marker) + ";" for marker in
                                ("struct G2TempleConnectionStatus", "struct G2ConnectionStatus",
                                 "enum G2ControlDesired", "enum G2ControlObserved",
                                 "enum G2ControlPhase", "struct G2ControlFeatureStatus",
                                 "struct G2ControlStatus"))
    definitions = [extract_block(owner, "class BleLifecycleGuard") + ";",
                   extract_block((COMPONENT / "System_Utils.h").read_text(),
                                 "inline void macToDisplay("),
                   extract_block(owner, "static void macToStackBuf("),
                   extract_block(owner, "const char* getBLEStateString(")]
    if not baseline:
        definitions += [extract_block(owner, "class CmdRequestCallbacks") + ";",
                        extract_block(owner, "bool startBLEAdvertising("),
                        extract_block(owner, "void stopBLEAdvertising(")]
        definitions.append(extract_block(owner, "bool buildBLEInfoJson("))
        handler = extract_block(web, "static esp_err_t handleBleStatus(")
        require("bleRunInternal" not in web and "executeCommand" not in web,
                "status HTTP must not execute BLE or ring CLI commands")
        require("g2RingBuildStatusJson" in handler and "ringText" not in handler,
                "ring status must fill response directly without parse fallback")
        require('bleRaw' not in handler and 'PSRAM_JSON_DOC(ble)' not in handler,
                "BLE status must fill its response directly without parse fallback")
        require(handler.index("WEB_AUTH_OR_RETURN") < handler.index("buildBLEInfoJson"),
                "authentication must precede BLE owner access")
    definitions.append(extract_block(owner, "static const char* cmd_bleinfo("))
    if not baseline:
        definitions += ["#if ENABLE_G2_GLASSES", *[
            extract_block(web, marker) for marker in
            ("static const char* g2DesiredJsonName(",
             "static const char* g2ObservedJsonName(", "static const char* g2PhaseJsonName(",
             "static void addG2FeatureJson(",
             "static void addG2TempleConnectionJson(")], "#endif", handler]
    harness = (HERE / "ble_info_status_harness.cpp").read_text()
    return harness.replace("// INSERT_TYPES", types).replace(
        "// INSERT_PRODUCTION", "\n\n".join(definitions))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread",
             "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-ble-info-") as directory:
        work = Path(directory)
        # The standard host String boundary, with an explicit append-failure
        # seam so the production final-serialization check can be exercised.
        string_stub = (HERE / "web_batch_stubs/Arduino.h").read_text()
        string_stub = string_stub.replace("void webBatchTestStringAppend();",
                                          "bool bleTestStringAppend();")
        string_stub = string_stub.replace("webBatchTestStringAppend();",
                                          "if (!bleTestStringAppend()) return false;")
        (work / "Arduino.h").write_text(string_stub)

        def run(name: str, source: str, extra: list[str]) -> str:
            path = work / (name + ".cpp")
            path.write_text(source)
            subprocess.run([args.cxx, *flags, "-I", str(work), "-I", str(JSON_INCLUDE),
                            *extra, str(path), "-o", str(work / name)], check=True, timeout=120)
            return subprocess.check_output([str(work / name)], text=True, timeout=60)

        output = None
        for g2 in (0, 1):
            result = run(f"ble-{g2}", generated(COMPONENT, False),
                         ["-DTEST_BASELINE=0", f"-DENABLE_G2_GLASSES={g2}"])
            require(output is None or output == result, "G2 must not change BLE CLI output")
            output = result
        if args.baseline:
            old = run("baseline", generated(args.baseline, True),
                      ["-DTEST_BASELINE=1", "-DENABLE_G2_GLASSES=0"])
            require(old == output, "BLE JSON/text output changed from characterized baseline")
            print("BLE status: successful CLI JSON/text matches pre-change production")

        shutil.copyfile(COMPONENT / "Bluetooth.h", work / "Bluetooth.h")
        (work / "System_BuildConfig.h").write_text("#define ENABLE_BLUETOOTH 0\n")
        (work / "System_User.h").write_text("#include <cstdint>\nusing TransportSessionEpoch = uint32_t;\n")
        run("disabled", '''#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
#include "Bluetooth.h"
#include <cassert>
bool bleTestStringAppend() { return true; }
int main() {
  JsonDocument doc;
  JsonObject object = doc.to<JsonObject>();
  object["sentinel"] = 9;
  assert(!buildBLEInfoJson(object));
  assert(object.size() == 1 && object["sentinel"] == 9);
  assert(!buildBLEInfoJson(JsonObject()));
}
''', [])
    print("BLE status: owner, CLI, HTTP/G2, disabled API and failure paths passed")


if __name__ == "__main__":
    main()
