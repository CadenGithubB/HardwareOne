#!/usr/bin/env python3
"""The OLED ESP-NOW screens send only registered commands and show the verdict.

The "Device Config" menu of a selected peer used to build `espnow cmd <mac>
restart|meshrole|espnowname|room|zone|prettyname`. No command is named
`espnow`, the result was not read, and the screen printed "Sent ... command".
The six rows now fill the command line of the remote form (which collects an
account of the target device) with a registered command and open it; the form
sends `espnowremote <mac> <user> <pass> <command>` and reads the reply.

Part one reads sources only and runs first:

- every command line OLED_ESPNow.cpp can submit starts with a registered
  command name: literals handed to the OLED command executors, printf formats
  written into a command buffer, `cmd = "..."` assignments, and the commands
  the Device Config rows put into the remote form. Names come from the
  CommandEntry tables through tools/command_registry.py, as in the
  command-registry gate; a help-module name such as `espnow` is not a command;
- the unused remote file view and its result sink are gone.

Part two compiles the production Device Config handler, its keyboard arm, the
remote form input and the form's send against recorded doubles
(oled_espnow_remote_actions_harness.cpp). The replies of `espnowremote` it is
fed come from System_ESPNow.cpp (espnow_remote_replies.py): the text
acknowledgement printed by the production formatter, the JSON one, and every
refusal, as the executor stamps them. A refusal must be shown as one, also
where the reply does not lead with "Error".

--component points the suite at another component directory (the pre-fix tree
fails part one and is not compiled).
"""
from __future__ import annotations

import argparse
import importlib.util
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from test_web_batch_handlers import extract_block, require
from reply_words_source import reply_prelude
import espnow_remote_replies as replies

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[3]

EXECUTORS = ("executeOledCommand", "executeOledCommandWithResult", "oledEspNowRunOwnerCommand",
             "oledRunCommand")
# File-local functions whose first argument is the command name of the line
# they build ("%s <MAC>"); the names are checked at their calls.
VERB_FORWARDERS = ("oledEspNowAnswerPairRequest",)
FUNCTION_HEAD = re.compile(r"(?m)^[A-Za-z_][^\n;]*?\b(\w+)\s*\([^\n;]*\)\s*\{")
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
RETIRED = ("storeRemoteFileBrowseResult", "RemoteFileBrowseState", "oledEspNowSendBrowseRequest",
           "oledEspNowDisplayRemoteFiles", "oledEspNowHandleRemoteFilesInput")
TABLE = "static const DeviceConfigAction kDeviceConfigActions[] ="


def registered_names() -> set[str]:
    spec = importlib.util.spec_from_file_location("hw1_command_registry",
                                                  REPO / "tools" / "command_registry.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return {row["name"].lower() for rows in module.parse_commands().values() for row in rows}


def call_argument(source: str, opening: int) -> str:
    """Text of the first argument of the call whose '(' is at `opening`."""
    depth, pos, in_string = 0, opening, False
    while pos < len(source):
        char = source[pos]
        if in_string:
            if char == "\\":
                pos += 1
            elif char == '"':
                in_string = False
        elif char == '"':
            in_string = True
        elif char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
            if depth == 0:
                break
        elif char == "," and depth == 1:
            break
        pos += 1
    return source[opening + 1:pos]


def command_lines(oled: str) -> list[tuple[int, str, str]]:
    """(line number, where it is built, command text) for every command line
    the file can submit. A literal that continues another (after '+') is an
    argument, not the head of a line."""
    found: list[tuple[int, str, str]] = []

    def line_of(index: int) -> int:
        return oled.count("\n", 0, index) + 1

    for name in EXECUTORS + VERB_FORWARDERS:
        for match in re.finditer(r"\b%s\s*\(" % name, oled):
            argument = call_argument(oled, match.end() - 1)
            for literal in LITERAL.finditer(argument):
                before = argument[:literal.start()].rstrip()
                if before.endswith("+"):
                    continue
                found.append((line_of(match.start()), name, literal[1]))
    for match in re.finditer(r'\bsnprintf\(\s*(\w*[cC]md\w*)\s*,[^,]*,\s*"((?:[^"\\\n]|\\.)*)"', oled):
        heads = [head[1] for head in FUNCTION_HEAD.finditer(oled, 0, match.start())]
        if match[2].startswith("%s") and heads and heads[-1] in VERB_FORWARDERS:
            continue  # the command name is this function's argument, checked at its calls
        found.append((line_of(match.start()), f"snprintf({match[1]})", match[2]))
    for match in re.finditer(r'\b(\w*[cC]md\w*)\s*=\s*"((?:[^"\\\n]|\\.)*)"', oled):
        found.append((line_of(match.start()), f"{match[1]} =", match[2]))
    if TABLE in oled:
        start = oled.index(TABLE)
        table = extract_block(oled, TABLE)
        for row in re.finditer(r'\{\s*"[^"]*"\s*,\s*(?:"((?:[^"\\\n]|\\.)*)"|nullptr)\s*,', table):
            if row[1] is not None:
                found.append((line_of(start + table.index(row[0])), "Device Config row", row[1]))
    return found


def static_failures(component: Path) -> list[str]:
    failures: list[str] = []
    oled = (component / "OLED_ESPNow.cpp").read_text(encoding="utf-8")
    header = (component / "OLED_ESPNow.h").read_text(encoding="utf-8")
    names = registered_names()
    require("espnowremote" in names and "reboot" in names and "espnow" not in names,
            "command registry read failed (espnowremote/reboot missing, or `espnow` is a command)")
    lines = command_lines(oled)
    require(len(lines) >= 12, f"OLED_ESPNow.cpp: only {len(lines)} command lines found; the scan is broken")
    for number, where, text in lines:
        words = text.split()
        first = words[0].lower() if words else ""
        if first not in names:
            failures.append(f"OLED_ESPNow.cpp:{number} {where} submits '{text}': "
                            f"'{first}' is not a registered command")
    if TABLE in oled:
        remote_rows = [text for _, where, text in lines if where == "Device Config row"]
        if len(remote_rows) != 6:
            failures.append(f"the Device Config menu has {len(remote_rows)} remote rows, expected 6")
    else:
        failures.append("OLED_ESPNow.cpp has no kDeviceConfigActions table (the rows are not routed "
                        "into the remote form)")
    for symbol in RETIRED:
        for name, text in (("OLED_ESPNow.cpp", oled), ("OLED_ESPNow.h", header)):
            if symbol in text:
                failures.append(f"{name} still has {symbol} (unused remote file view)")
    for path in sorted(component.glob("*.cpp")) + sorted(component.glob("*.h")):
        if path.name.startswith("OLED_ESPNow"):
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for symbol in RETIRED:
            if symbol in text:
                failures.append(f"{path.name} still names {symbol}")
    return failures


def owner_replies(component: Path, cxx: str, workdir: Path) -> str:
    """The generated reply tables of the harness, from System_ESPNow.cpp."""
    block = replies.remote_block(component)
    stamp = replies.ok_stamp(component)
    mac = bytes([0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03])
    cases = [("reboot", "AA:BB:CC:01:02:03", mac, 77),
             ("espnowfriendlyname Bench Two", "lab", mac, 4294967295),
             ("espnowsend lab " + "long text " * 20, "AA:BB:CC:01:02:03", mac, 4000000001)]
    acks = replies.render_text_acks(component, cases, cxx, workdir)
    for ack, (command, target, _, request) in zip(acks, cases):
        require(ack.endswith(f"(match reqId {request})"), f"acknowledgement lost its request id: {ack}")
        require(len(ack) <= 255, "acknowledgement longer than the reply buffer")
    require(acks[0] == "Sent 'reboot' to AA:BB:CC:01:02:03. Output is ASYNC - read it with:  "
                       "espnowmessages json 0 AA:BB:CC:01:02:03  (match reqId 77)",
            f"the text acknowledgement changed: {acks[0]}")
    texts = [replies.stamped(ack, stamp) for ack in acks]
    refusals = []
    for text, json_text in replies.refusals(block):
        if text is not None:
            refusals.append(replies.stamped(text.replace("%s", "lab"), stamp))
        if json_text is not None:
            refusals.append(json_text)

    def c_literal(text: str) -> str:
        return '"' + "".join("\\x%02x\"\"" % b if b < 0x20 or b in (0x22, 0x5C) or b > 0x7E else chr(b)
                             for b in text.encode()) + '"'

    def table(name: str, rows) -> str:
        return ("static const char* const %s[] = {\n" % name
                + "".join("  %s,\n" % c_literal(row) for row in rows) + "};\n")

    json_ack = replies.json_ack_format(block).replace("%lu", "77")
    return (table("kTextAcks", texts) + table("kRefusals", refusals)
            + "static const char kJsonAck[] = %s;\n" % c_literal(json_ack))


def generated(component: Path, cxx: str, workdir: Path) -> str:
    oled = (component / "OLED_ESPNow.cpp").read_text(encoding="utf-8")
    header = (component / "OLED_ESPNow.h").read_text(encoding="utf-8")
    display = (component / "OLED_Display.h").read_text(encoding="utf-8")
    types = [extract_block(header, marker) + ";" for marker in (
        "enum OledEspNowView", "enum OledEspNowMode", "struct OledEspNowState")]
    types.append(extract_block(display, "struct NavEvents") + ";")
    count = re.search(r"(?m)^static const int DEVICE_CONFIG_COUNT =[^;]*;", oled)
    require(count is not None, "OLED_ESPNow.cpp: DEVICE_CONFIG_COUNT is not derived from the table")
    production = [extract_block(oled, marker) for marker in (
        "static bool oledEspNowRunOwnerCommand(",
        "static void oledEspNowScrubRemoteForm(",
        "static void oledEspNowOpenRemoteFormWith(",
        "static bool oledEspNowRemoteAcknowledged(",
        "void oledEspNowBackToList(",
        "void oledEspNowUnpairDevice(",
        "bool oledEspNowHandleRemoteFormInput(",
        "bool oledEspNowSendRemoteCommand(")]
    production.append(extract_block(oled, "struct DeviceConfigAction") + ";")
    production.append(extract_block(oled, TABLE) + ";")
    production.append(count[0])
    production += [extract_block(oled, marker) for marker in (
        "bool oledEspNowHandleDeviceConfigInput(",
        "void oledEspNowApplyDeviceConfigEdit(")]
    # ApplyDeviceConfigEdit is called by the keyboard arm before its definition.
    production.insert(0, "void oledEspNowApplyDeviceConfigEdit(const String& value);")
    handler = extract_block(oled, "bool oledEspNowHandleInput(")
    arm = extract_block(handler, "case ESPNOW_VIEW_DEVICE_CONFIG_KEYBOARD:")
    harness = (HERE / "oled_espnow_remote_actions_harness.cpp").read_text()
    for marker, body in (("// INSERT_TYPES", "\n\n".join(types)),
                         ("// INSERT_PRODUCTION", "\n\n".join(production)),
                         ("// INSERT_CONFIG_KEYBOARD_ARM", arm),
                         ("// INSERT_OWNER_REPLIES", owner_replies(component, cxx, workdir))):
        require(harness.count(marker) == 1, f"missing/duplicate marker: {marker}")
        harness = harness.replace(marker, body)
    return harness


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1],
                        help="production component directory (overlay for base runs)")
    args = parser.parse_args()

    failures = static_failures(args.component)
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        print(f"oled_espnow_remote_actions: {len(failures)} source failure(s); harness not built")
        return 1
    print("PASS source: every OLED ESP-NOW command line starts with a registered command")

    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-parameter", "-Wno-unused-function"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-oled-espnow-remote-") as directory:
        work = Path(directory)
        unit, executable = work / "test.cpp", work / "test"
        (work / "System_MacText.h").write_text((args.component / "System_MacText.h").read_text())
        unit.write_text(reply_prelude(args.component) + generated(args.component, args.cxx, work))
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)], check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=60)
    print("PASS oled_espnow_remote_actions")
    return 0


if __name__ == "__main__":
    sys.exit(main())
