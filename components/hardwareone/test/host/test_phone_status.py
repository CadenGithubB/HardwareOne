#!/usr/bin/env python3
"""Exercise the production phone status owner (BLE_PhoneStatus) on the host.

BLE_PhoneStatus.cpp, the real CompactJson and argWantsJson compile beside fake
pipe, console-server, glasses, peer-registry and SSE boundaries with vendored
ArduinoJson, linked with the real BLE_LinkStatus.cpp (bleLinkStatus), so every
word comes from BLE_LinkStatusCore.h as shipped. Checks the lens/OLED word, the
compact `phone-status` SSE event (seeded first tick, a push only on a change,
250 ms cadence; the setup step, enrolling and the text are each a change; "p"
carries the pipe id and "r" its reason; the documented 89-character worst
case), the /api/ble/status `phone` object (schema 2) and `phone status [json]`
in G2, phone-only and server modes with their size guards, `status json`
connectivity.bluetooth (that block of buildSystemInfoJson compiled as shipped,
plus a source check of the document's size guard), that no surface carries an
address or the phone secret, and the builds without the pipe (the tick stub
only; `phone` is then BLE_PhonePipe.cpp's one-row "not in this build" table).
Phone link v2: bleLinkStatus() reads the owner's summary, the console driver's
status and its row's leaf snapshot (fakes here); the console stream comes first
in the console word, `status json` carries the compact `phoneLink` block, and
the secret's kind comes from its length and Settings::phonePipeSecretGenerated
(`phone pair new`'s mark), read directly: a marked code reads "code", an
unmarked secret "passphrase". This does not qualify the phone link, Android or
the web/lens renderers that consume it.
"""
from __future__ import annotations

import argparse
import json
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require, JSON_INCLUDE

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

JSON_CONFIG = """#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#define ARDUINOJSON_ENABLE_ARDUINO_STREAM 0
#define ARDUINOJSON_ENABLE_ARDUINO_PRINT 0
#define ARDUINOJSON_ENABLE_PROGMEM 0
"""


def stubs(work: Path, component: Path, pipe: int) -> None:
    arduino = (HERE / "web_batch_stubs/Arduino.h").read_text()
    (work / "Arduino.h").write_text(
        arduino + "\n#define EXT_RAM_BSS_ATTR\nuint32_t millis();\n")
    (work / "System_BuildConfig.h").write_text(
        f"#pragma once\n#define ENABLE_BLUETOOTH 1\n#define ENABLE_BLE_PHONE_PIPE {pipe}\n"
        + JSON_CONFIG)
    peers = (component / "BLE_Peers.h").read_text()
    (work / "BLE_Peers.h").write_text(
        "#pragma once\n#include <cstdint>\n"
        + extract_block(peers, "enum BlePeerKind") + ";\n"
        + extract_block(peers, "struct BlePeerReconnectSnapshot") + ";\n"
        "bool blePeerReconnectSnapshot(BlePeerKind kind, BlePeerReconnectSnapshot& out);\n")
    # The pipe's public status calls, with the real link summary (phone link v2).
    pipe_header = (component / "BLE_PhonePipe.h").read_text()
    (work / "BLE_PhonePipe.h").write_text(
        "#pragma once\n#include <cstdint>\nbool phonePipeActive();\nbool phonePipeStandalone();\n"
        "bool phonePipeReady();\nconst char* phonePipeStatus();\n"
        "uint8_t phonePipeStep();\nbool phonePipeEnrolling();\n"
        + extract_block(pipe_header, "struct PhonePipeLinkSummary") + ";\n"
        "bool phonePipeLinkSummary(PhonePipeLinkSummary* out);\n")
    bluetooth = (component / "Bluetooth.h").read_text()
    (work / "Bluetooth.h").write_text(
        "#pragma once\n#include <cstdint>\nusing TransportSessionEpoch = uint32_t;\n"
        + extract_block(bluetooth, "struct BleConsoleView") + ";\n"
        + extract_block(bluetooth, "struct BlePipeConsoleIdentity") + ";\n"
        "BleConsoleView bleConsoleView();\nbool isBleServerInitialized();\nbool bleServerUp();\n"
        "void blePipeConsoleIdentity(BlePipeConsoleIdentity* out);\n")
    (work / "G2_Glasses.h").write_text("#pragma once\nbool isG2ClientInitialized();\n")
    # gSettings.bleMode is an int in the real Settings (0 server, 1 G2, 2 phone);
    # the console setting, the phone secret and the mark `phone pair new` sets
    # on a device-made code.
    (work / "System_Settings.h").write_text(
        "#pragma once\n#include <Arduino.h>\nstruct HostSettings {\n  int bleMode = 0;\n"
        "  bool bleConsoleOnPipe = true;\n  String phonePipeSecret;\n"
        "  bool phonePipeSecretGenerated = false;\n};\n"
        "extern HostSettings gSettings;\n")
    (work / "System_CommandRegistry.h").write_text(
        '#pragma once\n#include <Arduino.h>\nbool argWantsJson(const String& args);\n')
    (work / "System_MemUtil.h").write_text(
        "#pragma once\n#define PSRAM_JSON_DOC(name) JsonDocument name\n")
    # Copied, not included from the component: their quoted includes must
    # resolve to these stubs, never to the real build configuration.
    for name in ("BLE_PhoneStatus.h", "BLE_Events.h", "BLE_LinkStatus.h",
                 "BLE_LinkStatusCore.h", "BLE_LinkStatus.cpp", "BLE_PhonePipeConsole.h"):
        shutil.copyfile(component / name, work / name)


def status_json_bluetooth(component: Path) -> str:
    """`status json` connectivity.bluetooth as shipped (System_Utils.cpp)."""
    info = extract_block((component / "System_Utils.cpp").read_text(), "void buildSystemInfoJson(")
    start = info.index("#if ENABLE_BLUETOOTH\n  {")
    end = info.index("#endif\n", info.index("bleLinkStatusJson(bt, link);", start)) + len("#endif\n")
    return info[start:end]


def check_status_json_guard(component: Path) -> None:
    """`status json` measures its document before the bounded serialize."""
    body = extract_block((component / "System_Utils.cpp").read_text(), "const char* cmd_status(")
    path = body[body.index("if (argWantsJson(argsInput))"):]
    guard = path.index("if (measureJson(doc) >= kStatusJsonCap) return")
    require(path.index("doc.overflowed()") < guard < path.index("serializeJson(doc, statusJsonBuf, kStatusJsonCap)"),
            "status json refuses an overflowed or oversized document before serializing")
    require("ps_alloc(kStatusJsonCap," in path, "status json serializes into the buffer it measured for")


def check_generated_mark(component: Path) -> None:
    """The link reads Settings::phonePipeSecretGenerated as declared, with no
    fallback that would call a missing mark a passphrase."""
    source = (component / "BLE_LinkStatus.cpp").read_text()
    inputs = extract_block(source, "hw1link::PhoneLinkInputs bleLinkPhoneInputs()")
    require("gSettings.phonePipeSecretGenerated ?" in inputs and "decltype" not in source,
            "bleLinkPhoneInputs reads gSettings.phonePipeSecretGenerated directly")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]

    events = (COMPONENT / "BLE_Events.cpp").read_text()
    production = "\n\n".join([
        events[events.index("CompactJson::CompactJson("):events.index("// blePushEvent\n")],
        extract_block((COMPONENT / "System_Utils.cpp").read_text(), "bool argWantsJson("),
        (COMPONENT / "BLE_PhoneStatus.cpp").read_text().replace(
            '#include "BLE_PhoneStatus.h"', ""),
    ])
    with tempfile.TemporaryDirectory(prefix="hw1-phone-status-") as directory:
        work = Path(directory)
        stubs(work, COMPONENT, 1)
        unit = work / "phone_status.cpp"
        unit.write_text((HERE / "phone_status_harness.cpp").read_text().replace(
            "// INSERT_PRODUCTION", production).replace(
            "// INSERT_STATUS_JSON", status_json_bluetooth(COMPONENT)))
        binary = work / "phone_status"
        subprocess.run([args.cxx, *flags, "-I", str(work), "-I", str(JSON_INCLUDE),
                        str(unit), str(work / "BLE_LinkStatus.cpp"),
                        "-o", str(binary)], check=True, timeout=180)
        lines = subprocess.check_output([str(binary)], text=True, timeout=60).splitlines()
        # `phone pair new`'s mark makes a code; an unmarked secret is a passphrase
        # (the kinds are checked again below, over every surface).
        require(any('"secret":"code"' in line for line in lines) and
                any('"secret":"passphrase"' in line for line in lines),
                "a marked device code reads as secret code, an unmarked secret as a passphrase")
        records = [line.split("\t", 1) for line in lines]

        pushed = [payload.split("\t", 1) for kind, payload in records if kind == "EVENT"]
        require(all(clean == "1" and len(body) < 128 for clean, body in pushed),
                "SSE payloads fit the 128-character data field cleanly")
        events_out = [json.loads(body) for _, body in pushed]
        require([e["p"] for e in events_out] ==
                ["searching", "ready", "searching", "linking", "waiting", "pairing",
                 "fail", "fail", "not-paired", "fault"],
                "SSE pushes on every phase, step, enrolling and text change; 'p' is the pipe id")
        require(events_out[0] == {"u": False, "on": True, "own": False, "en": False,
                                  "ar": False, "p": "searching"},
                "SSE event carries the flags and the pipe id in compact keys")
        require(events_out[1]["u"] and events_out[1]["en"] and events_out[1]["ar"],
                "SSE event reads enrollment when it goes out")
        require([e.get("r") for e in events_out] ==
                [None] * 6 + ["not found", "timed out", None, "cleanup"],
                "'r' is the short reason of a failure or fault, only when there is one")
        require(events_out[4]["own"] and not events_out[4]["en"],
                "a phone-only pipe that is not enrolled")
        require(all("s" not in e for e in events_out),
                "the pipe's own text stays out of the event")
        bound = int(next(p for k, p in records if k == "BOUND"))
        require(bound == 89, f"the documented SSE worst case is 89 characters, got {bound}")
        require("Worst case 89 of 127 characters" in (COMPONENT / "BLE_PhoneStatus.cpp").read_text(),
                "the SSE worst case is stated beside the event")

        objects = [json.loads(p) for k, p in records if k == "JSON"]
        require(objects[0] == {"schema": 2, "active": True, "standalone": False,
                               "connected": True, "state": "ready", "text": "ready",
                               "role": "with-glasses", "enrolled": True, "autoReconnect": False,
                               "pipe": "ready", "pipeText": "ready", "console": "signed-in",
                               "consoleText": "app signed in"},
                "/api/ble/status phone object, G2 mode with the pipe ready and the console on its link")
        require(objects[1] == {"schema": 2, "active": True, "standalone": True,
                               "connected": False, "state": "phone not found",
                               "text": "fail: not found", "role": "phone-only",
                               "enrolled": True, "autoReconnect": True, "pipe": "fail",
                               "pipeText": "fail", "pipeReason": "not found",
                               "console": "not-here",
                               "consoleText": "not available in this mode"},
                "phone object, phone-only after a failed search")
        require(objects[2] == {"schema": 2, "active": False, "standalone": False,
                               "connected": False, "state": "stopped", "text": "off",
                               "role": "off", "enrolled": False, "autoReconnect": False,
                               "pipe": "off", "pipeText": "off", "console": "signed-in",
                               "consoleText": "app signed in"},
                "phone object, server mode with a signed-in app")
        require(objects[3]["pipe"] == "searching" and objects[3]["text"] == "searching" and
                objects[3]["console"] == "not-here",
                "phone object, a standalone pipe while the saved mode is server")
        require(objects[4]["role"] == "with-glasses" and objects[4]["pipe"] == "searching" and
                objects[4]["console"] == "waiting-phone" and
                objects[4]["consoleText"] == "waiting for the phone link",
                "phone object, G2 mode: the console waits for the phone link while the pipe searches")
        require(objects[5] == {"schema": 2, "active": True, "standalone": True,
                               "connected": True, "state": "ready", "text": "ready",
                               "role": "phone-only", "enrolled": True, "autoReconnect": False,
                               "pipe": "ready", "pipeText": "ready", "console": "signed-in",
                               "consoleText": "app signed in"},
                "phone object, phone-only: the console stream open and signed in (phone link v2)")
        require(objects[6]["role"] == "phone-only" and objects[6]["console"] == "waiting-phone" and
                objects[6]["consoleText"] == "waiting for the phone link",
                "phone object, phone-only: an offered stream the app does not want waits for the phone")
        require(len(objects) == 7, "seven phone objects")
        cmd_objects = [json.loads(p) for k, p in records if k == "CMDJSON"]
        require(cmd_objects == objects, "`phone status json` is the same object")
        texts = [p for k, p in records if k == "TEXT"]
        require(texts == [
            # role= is the pipe's place (as in the JSON); blestatus shows the device role.
            "pipe=ready role=with-glasses paired=yes auto=off console=signed-in state='ready'",
            "pipe=fail: not found role=phone-only paired=yes auto=on console=not-here "
            "state='phone not found'",
            "pipe=off role=off paired=no auto=off console=signed-in state='stopped'",
            "pipe=searching role=phone-only paired=no auto=off console=not-here state='scanning'",
            "pipe=searching role=with-glasses paired=no auto=off console=waiting-phone "
            "state='scanning'",
            "pipe=ready role=phone-only paired=yes auto=off console=signed-in state='ready'",
            "pipe=ready role=phone-only paired=yes auto=off console=waiting-phone state='ready'",
        ], "`phone status` text")
        bluetooth = [json.loads(p) for k, p in records if k == "STATUSBT"]
        base = {"running": True, "state": "connected", "client": False, "g2Connected": False}
        # The phone link block, compact: no texts and no end-reason counts.
        require(bluetooth[0] == {**base, "mode": "client", "server": False,
                                 "phoneConnected": True, "phoneState": "ready",
                                 "phoneLink": {"state": "ready", "paired": True, "secret": "passphrase",
                                               "console": "needs-code", "relay": "on",
                                               "streams": {"relay": 1, "console": 0},
                                               "ended": 2, "lastEnd": "paused"},
                                 "role": "glasses-pipe", "roleText": "Glasses+pipe",
                                 "saved": "glasses-pipe", "savedText": "Glasses+pipe", "savedDiffers": False,
                                 "pipe": "ready", "pipeText": "ready", "pipePaired": True,
                                 "console": "signed-in", "consoleText": "app signed in",
                                 "consoleClients": 1, "advertising": False},
                "status json connectivity.bluetooth, G2 mode: the console signed in on the pipe's link")
        require(bluetooth[1] == {**base, "mode": "client", "server": False,
                                 "phoneConnected": False, "phoneState": "fail: not found",
                                 "phoneLink": {"state": "fail", "reason": "not found", "paired": True,
                                               "secret": "none", "console": "needs-code",
                                               "relay": "unknown",
                                               "streams": {"relay": 0, "console": 0},
                                               "ended": 2, "lastEnd": "paused"},
                                 "role": "phone-pipe", "roleText": "Phone pipe",
                                 "saved": "glasses-pipe", "savedText": "Glasses+pipe", "savedDiffers": False,
                                 "pipe": "fail", "pipeText": "fail", "pipeReason": "not found",
                                 "pipePaired": True, "console": "not-here",
                                 "consoleText": "not available in this mode",
                                 "consoleClients": 0, "advertising": False},
                "status json connectivity.bluetooth, phone-only after a failed search")
        require(bluetooth[2] == {**base, "mode": "server", "server": True,
                                 "phoneConnected": False, "phoneState": "off",
                                 "phoneLink": {"state": "off", "paired": False, "secret": "none",
                                               "console": "needs-code", "relay": "unknown",
                                               "streams": {"relay": 0, "console": 0}, "ended": 0},
                                 "role": "console", "roleText": "App console",
                                 "saved": "console", "savedText": "App console", "savedDiffers": False,
                                 "pipe": "off", "pipeText": "off", "pipePaired": False,
                                 "console": "signed-in", "consoleText": "app signed in",
                                 "consoleClients": 1, "advertising": True},
                "status json connectivity.bluetooth, server mode with a signed-in app")
        require(bluetooth[3] == {**base, "mode": "client", "server": False,
                                 "phoneConnected": False, "phoneState": "searching",
                                 "phoneLink": {"state": "searching", "paired": False, "secret": "none",
                                               "console": "needs-code", "relay": "unknown",
                                               "streams": {"relay": 0, "console": 0}, "ended": 0},
                                 "role": "glasses-pipe", "roleText": "Glasses+pipe",
                                 "saved": "glasses-pipe", "savedText": "Glasses+pipe", "savedDiffers": False,
                                 "pipe": "searching", "pipeText": "searching", "pipePaired": False,
                                 "console": "waiting-phone",
                                 "consoleText": "waiting for the phone link",
                                 "consoleClients": 0, "advertising": False},
                "status json connectivity.bluetooth, G2 mode: a slot left by the pipe's old "
                "link is not counted")
        require(bluetooth[4] == {**base, "mode": "client", "server": False,
                                 "phoneConnected": True, "phoneState": "ready",
                                 "phoneLink": {"state": "ready", "paired": True, "secret": "code",
                                               "console": "signed-in", "relay": "off",
                                               "streams": {"relay": 0, "console": 1}, "ended": 0},
                                 "role": "phone-pipe", "roleText": "Phone pipe",
                                 "saved": "glasses-pipe", "savedText": "Glasses+pipe", "savedDiffers": False,
                                 "pipe": "ready", "pipeText": "ready", "pipePaired": True,
                                 "console": "signed-in", "consoleText": "app signed in",
                                 "consoleClients": 1, "advertising": False},
                "status json connectivity.bluetooth, phone-only: the console stream signed in "
                "(one session even before the console view counts its row), relay off")
        require(bluetooth[5]["phoneLink"] == {"state": "ready", "paired": True, "secret": "code",
                                              "console": "not-wanted", "relay": "off",
                                              "streams": {"relay": 0, "console": 0}, "ended": 0} and
                bluetooth[5]["console"] == "waiting-phone" and bluetooth[5]["consoleClients"] == 0,
                "status json connectivity.bluetooth, phone-only: offered, the app does not want it")
        require(len(bluetooth) == 6, "six status json blocks")
        check_status_json_guard(COMPONENT)
        check_generated_mark(COMPONENT)
        largest = int(next(p for k, p in records if k == "LARGEST"))
        require(largest < 384, f"the largest phone object fits the reply ({largest} bytes)")
        joined = "\n".join(p for _, p in records)
        # The phone link names the secret's kind, never the secret: the values
        # the cases store never appear, and "secret" holds only a kind word.
        require(not any(word in joined.lower() for word in ("mac", "addr")) and
                "Pass-phrase1!" not in joined and "0123456789ABCDEF" not in joined,
                "no address or phone secret on any surface")
        kinds = set(re.findall(r'"secret":"([^"]*)"', joined))
        require(kinds == {"passphrase", "none", "code"} and joined.count('"secret"') ==
                joined.count('"secret":"'), f"secret is only ever a kind word: {kinds}")

        # Without the pipe: only the tick stub; no phone status command exists.
        stubs(work, COMPONENT, 0)
        disabled = work / "disabled.cpp"
        disabled.write_text('#include "BLE_PhoneStatus.h"\n#include <cassert>\n#include <cstring>\n'
                            "void webBatchTestStringAppend() {}\n"
                            "int main() {\n  phoneStatusTick();\n}\n")
        subprocess.run([args.cxx, *flags, "-I", str(work), "-I", str(JSON_INCLUDE),
                        str(disabled), "-o", str(work / "disabled")], check=True, timeout=120)
        subprocess.run([str(work / "disabled")], check=True, timeout=30)
        disabled_source = (COMPONENT / "BLE_PhoneStatus.cpp").read_text()
        require(disabled_source.count("#if ENABLE_BLUETOOTH && ENABLE_BLE_PHONE_PIPE") == 1,
                "the owner compiles to nothing without the pipe")
        checks = next(p for k, p in records if k == "CHECKS")
    print(f"Phone status: pipe/console words (console stream first), SSE phase pushes "
          f"(worst case {bound} chars), status JSON schema 2 and CLI text, status json "
          f"connectivity.bluetooth with the compact phone link block, "
          f"size guards and the pipe-off stubs passed "
          f"({checks} harness checks; largest object {largest} bytes)")


if __name__ == "__main__":
    main()
