#!/usr/bin/env python3
"""Run the production BLE status owner, CLI and HTTP handler with host boundaries.

Uses vendored ArduinoJson, extracted BLE types/state formatter/lifecycle guard,
and the actual disabled-feature header. Hardware, RTOS admission, HTTP transport
and allocation failures are substitutes. --baseline compares successful CLI
JSON/text output against a saved pre-change component directory.

Also runs the production blesend/blestream/bleevent handlers (real CommandArgs
parser and stream-flag helpers) and the data-service push functions against a
fake directed GATT sender. --component points the suite at another copy of the
BLE sources (Bluetooth.cpp/.h, WebPage_Bluetooth.cpp/.h).

Also runs the production `blestatus`/`bleread` command against the same table:
both forms read the guarded snapshot `bleinfo json` uses, so the JSON views
list the same client set (connId, authed, user) and a busy lifecycle guard
gives an explicit unavailable reply.

Also runs the production `bledisconnect` command and its owner functions
against a recording GATT-server double: a connection id closes exactly that app
client, the bare form closes every app client and never a paired ring/glasses
link, and unknown, peer and malformed ids are refused without a close.
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


def stream_definitions(owner: str) -> list[str]:
    """The stream CLI handlers, their parser/flag helpers and the push functions."""
    command_header = (COMPONENT / "System_Command.h").read_text()
    command_source = (COMPONENT / "System_Command.cpp").read_text()
    out = [extract_block(command_header, "class CommandArgs") + ";",
           "const String CommandArgs::empty_;"]
    out += [extract_block(command_source, marker) for marker in
            ("CommandArgs::CommandArgs(", "void CommandArgs::parse()",
             "const String& CommandArgs::arg(", "int CommandArgs::argInt(")]
    out += [extract_block(owner, marker) for marker in
            ("bool isBLEConnected(", "void bleEnableStream(", "void bleDisableStream(",
             "void bleSetStreamInterval(", "bool bleIsStreamEnabled(",
             "static size_t jsonEscapeInto(")]
    # Present only once the streams use directed per-session delivery.
    if "static bool bleStreamNotifyDirected(" in owner:
        out.append(extract_block(owner, "static bool bleStreamNotifyDirected("))
    else:
        print("directed stream helper absent (pre-fix source)")
    out += [extract_block(owner, "bool blePushSensorData("),
            extract_block(owner, "bool blePushSystemStatus(")]
    # The header declares the default `details`; the event JSON prints millis()
    # with %lu (unsigned long on the target) while the host millis() stays
    # uint32_t for the 32-bit connection-age case.
    out += ["bool blePushEvent(BLEEventType eventType, const char* message, "
            "const char* details = nullptr);",
            '#pragma GCC diagnostic push', '#pragma GCC diagnostic ignored "-Wformat"',
            extract_block(owner, "bool blePushEvent(BLEEventType"),
            '#pragma GCC diagnostic pop']
    if "static const char* bleStreamSelect(" in owner:
        out.append(extract_block(owner, "static const char* bleStreamSelect("))
    out += [extract_block(owner, marker) for marker in
            ("static const char* cmd_blesend(", "static const char* cmd_blestream(",
             "static const char* cmd_bleevent(")]
    return out


def disconnect_definitions(owner: str) -> list[str]:
    """The bledisconnect command with its owner functions (after isBLEConnected)."""
    out = [extract_block(owner, "int findConnectionSlotByConnIdLocked(")]
    if "BleDisconnectResult bleDisconnectConnection(" in owner:
        out += [extract_block(owner, marker) for marker in
                ("static bool bleIsPeerLinkType(",
                 "BleDisconnectResult bleDisconnectConnection(",
                 "int bleDisconnectAppClients(")]
    else:
        out.append(extract_block(owner, "void disconnectBLE("))
    out.append(extract_block(owner, "static const char* cmd_bledisconnect("))
    return out


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
    types += "\n" + extract_block(owner, "struct BleSessionFence") + ";"
    # The disconnect owner API exists once bledisconnect targets a connection id.
    if "enum class BleDisconnectResult" in header:
        types += ("\n#define BLE_DISCONNECT_OWNER 1\n" +
                  extract_block(header, "enum class BleDisconnectResult") + ";")
    else:
        types += "\n#define BLE_DISCONNECT_OWNER 0"
        if not baseline:
            print("ble disconnect owner absent (pre-fix source)")
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
    # blestatus/bleread and buildBLEInfoJson render one guarded snapshot.
    if "struct BleStatusSnapshot" in owner:
        definitions += [extract_block(owner, "struct BleStatusSnapshot") + ";",
                        extract_block(owner, "static bool bleCaptureStatusSnapshot(")]
    elif not baseline:
        print("BLE status snapshot absent (pre-fix source)")
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
        definitions += stream_definitions(owner)
        definitions += disconnect_definitions(owner)
    definitions.append(extract_block(owner, "static const char* cmd_bleinfo("))
    if not baseline:
        # The pre-fix blestatus passes uint32_t to %lu (unsigned long on the target).
        definitions += ['#pragma GCC diagnostic push', '#pragma GCC diagnostic ignored "-Wformat"',
                        extract_block(owner, "static const char* cmd_blestatus("),
                        '#pragma GCC diagnostic pop']
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


def check_web_kick(page: Path) -> None:
    """The per-client Kick passes that client's connId (Node is not required)."""
    source = page.read_text()

    def iife_member(start: str, end: str) -> str:
        # Top-level members of the page IIFE end at their two-space indent.
        marker = re.search(rf"^  {re.escape(start)}[^\n]*", source, re.MULTILINE)
        require(marker is not None, f"missing page member: {start}")
        closing = re.search(rf"^  {re.escape(end)}$", source[marker.end():], re.MULTILINE)
        require(closing is not None, f"unterminated page member: {start}")
        return source[marker.start():marker.end() + closing.end()]

    render = iife_member("function renderConnections(", "}")
    kick = re.search(r"""onclick="btDisconnect\(' \+ (\w+) \+ '\)\"""", render)
    require(kick is not None and "btDisconnect()" not in render,
            "Kick must name its client's connection id, not run the bare bledisconnect")
    require(re.search(rf"var {kick.group(1)}\s*=\s*parseInt\(c\.connId\b", render) is not None,
            "Kick's id must come from the client's connId")
    handler = iife_member("window.btDisconnect = function(", "};")
    param = re.match(r"\s*window\.btDisconnect = function\((\w+)\)", handler)
    require(param is not None and
            f"cli('bledisconnect ' + {param.group(1)})" in handler and
            "cli('bledisconnect')" not in handler,
            "btDisconnect must send `bledisconnect <connId>`")
    print("BLE web Kick: per-client button sends bledisconnect <connId>")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="component directory whose BLE sources are tested")
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
        # CommandArgs::argInt converts tokens with toInt().
        string_stub = string_stub.replace(
            "  char operator[]",
            "  long toInt() const { return std::strtol(text_.c_str(), nullptr, 10); }\n"
            "  char operator[]")
        string_stub = string_stub.replace("#include <algorithm>",
                                          "#include <algorithm>\n#include <cstdlib>")
        (work / "Arduino.h").write_text(string_stub)

        def run(name: str, source: str, extra: list[str]) -> str:
            path = work / (name + ".cpp")
            path.write_text(source)
            subprocess.run([args.cxx, *flags, "-I", str(work), "-I", str(JSON_INCLUDE),
                            *extra, str(path), "-o", str(work / name)], check=True, timeout=120)
            return subprocess.check_output([str(work / name)], text=True, timeout=60)

        output = None
        for g2 in (0, 1):
            result = run(f"ble-{g2}", generated(args.component, False),
                         ["-DTEST_BASELINE=0", f"-DENABLE_G2_GLASSES={g2}"])
            require(output is None or output == result, "G2 must not change BLE CLI output")
            output = result
        if args.baseline:
            old = run("baseline", generated(args.baseline, True),
                      ["-DTEST_BASELINE=1", "-DENABLE_G2_GLASSES=0"])
            require(old == output, "BLE JSON/text output changed from characterized baseline")
            print("BLE status: successful CLI JSON/text matches pre-change production")

        shutil.copyfile(args.component / "Bluetooth.h", work / "Bluetooth.h")
        (work / "System_BuildConfig.h").write_text("#define ENABLE_BLUETOOTH 0\n")
        (work / "System_User.h").write_text("#include <cstdint>\nusing TransportSessionEpoch = uint32_t;\n")
        # Disabled bledisconnect owner stubs report unavailable / nothing done.
        disconnect_stubs = ""
        if "bleDisconnectConnection(" in (args.component / "Bluetooth.h").read_text():
            disconnect_stubs = ("  assert(bleDisconnectConnection(1) == BleDisconnectResult::UNAVAILABLE &&\n"
                                "         bleDisconnectAppClients() == 0);\n")
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
''' + disconnect_stubs + '''}
''', [])
    check_web_kick(args.component / "WebPage_Bluetooth.h")
    print("BLE status: owner, CLI, HTTP/G2, disabled API and failure paths passed")


if __name__ == "__main__":
    main()
