#!/usr/bin/env python3
"""Execute the shipping system-log status and flag formatters.

The status reading (systemLogState, SystemLogStatus, systemLogReadStatus), its
text form (formatSystemLogStatus) and json form (formatSystemLogStatusJson),
both formatSystemLogFlags overloads, parseSystemLogFlags,
systemLogApplyPersistedFlags and systemLogFollowFlagChange come from
System_Debug.cpp; DebugFlagMask, DEBUG_BIT, the MSG_ROUTE_* values, the
SystemLogState enum and its key/label from System_Debug.h, all unchanged. The
buffer sizes are read from the production callers (CLI `log status`, the web
status endpoint and the flag messages), so a caller change fails here first.

Source contracts pin the three readers of the one state: the CLI (text and
`log status json`), the web endpoint (json, never the text) and the OLED System
Logging screen (the state label; its Start/Stop rows keep following the
running flag).
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def caller_sizes() -> dict[str, int]:
    debug = (COMPONENT / "System_Debug.cpp").read_text()
    web = (COMPONENT / "WebServer_Server.cpp").read_text()
    cli = re.findall(r"formatSystemLogStatus\(gDebugBuffer, (\d+)\);", debug)
    require(len(cli) == 1, "one CLI log-status caller with a literal buffer size")
    cli_json = re.findall(r"formatSystemLogStatusJson\(gDebugBuffer, (\d+)\);", debug)
    require(cli_json == cli, "`log status json` uses the CLI buffer size")
    page = re.findall(r"EXT_RAM_BSS_ATTR static char logBuf\[(\d+)\];\s*\n\s*"
                      r"formatSystemLogStatusJson\(logBuf, sizeof\(logBuf\)\);", web)
    require(len(page) == 1, "one web log-status caller: the json form in a sized PSRAM buffer")
    require("formatSystemLogStatus(" not in web, "the web endpoint sends the json form, never the status text")
    flags = set(re.findall(r"char flagsText\[(\d+)\];\s*\n\s*formatSystemLogFlags\(", debug))
    require(len(flags) == 1, "the flag-message callers share one buffer size")
    return {"HW1_CLI_STATUS_BYTES": int(cli[0]), "HW1_WEB_STATUS_BYTES": int(page[0]),
            "HW1_FLAGS_TEXT_BYTES": int(flags.pop())}


def reader_contracts() -> None:
    """The OLED screen and the Logging page read the same state as the CLI."""
    oled = (COMPONENT / "OLED_Mode_Logging.cpp").read_text()
    start = oled.index("case LOG_MENU_SYSTEM: {")
    screen = oled[start:oled.index("case LOG_MENU_SENSOR_CONFIG: {", start)]
    require('oledDisplay->print(systemLogStateLabel(systemLogState()));' in screen,
            "the OLED System Logging status line prints the shared state label")
    require('"Status: ACTIVE"' not in screen and '"Status: STOPPED"' not in screen,
            "the OLED System Logging status line no longer derives its own label from the running flag")
    require("if (i == 0 && gSystemLogRunning) enabled = false;" in screen and
            "if (i == 1 && !gSystemLogRunning) enabled = false;" in screen,
            "the OLED Start/Stop rows keep following the running flag")
    require("loggingMenuSelection == 0 && !gSystemLogRunning" in oled and
            "loggingMenuSelection == 1 && gSystemLogRunning" in oled,
            "the OLED Start/Stop actions keep following the running flag")
    require(not re.search(r"\bextern\b[^;\n]*systemLogState", oled),
            "OLED_Mode_Logging.cpp takes systemLogState from System_Debug.h")
    web = (COMPONENT / "WebServer_Server.cpp").read_text()
    handler = extract_block(web, "static esp_err_t handleLoggingStatus(")
    for command in ('executeInternalStatusCommand("time json"', 'executeInternalStatusCommand("sensorlog status json"'):
        require(command in handler, f"/api/logging/status runs {command}")
    order = [handler.index(text) for text in ("results.add(timeBuf);", "results.add(sensorBuf);", "results.add(logBuf);")]
    require(order == sorted(order), "/api/logging/status keeps its slot order (time, sensorlog, system log)")
    require('doc["systemLog"]' in handler and "isAdminUser(ctx.user)" in handler,
            "the admins' systemLog block stays")
    page = (COMPONENT / "WebPage_Logging.h").read_text()
    for wording in ("logging ACTIVE", "'Unknown command'", "'not found'", "/File:", "/Interval:", "/Format:", "/Max size:",
                    "/Rotations:", "/Sensors:", "/Last write:", "/Auto-start:", "/Output flags:", "/Time:"):
        require(wording not in page, f"the Logging page still matches on text-reply wording {wording!r}")
    for command in ("time", "sensorlog status", "log status"):
        require(not re.search(r"cmd:\s*'" + command + r"'", page),
                f"the Logging page requests the text form of '{command}'")


def generate() -> str:
    debug = (COMPONENT / "System_Debug.cpp").read_text()
    debug_h = (COMPONENT / "System_Debug.h").read_text()
    routes = re.findall(r"(?m)^#define MSG_ROUTE_\w+ .*$", debug_h)
    bit = re.findall(r"(?m)^#define DEBUG_BIT\(n\) .*$", debug_h)
    require(routes and len(bit) == 1, "MSG_ROUTE_* and DEBUG_BIT come from System_Debug.h")
    json_text = (COMPONENT / "System_JsonText.h").read_text().replace("#pragma once\n", "")
    types = "\n".join([extract_block(debug_h, "struct DebugFlagMask") + ";", *routes, *bit, json_text,
                       extract_block(debug_h, "enum class SystemLogState : uint8_t") + ";",
                       "SystemLogState systemLogState();",
                       extract_block(debug_h, "inline const char* systemLogStateKey(SystemLogState state)"),
                       extract_block(debug_h, "inline const char* systemLogStateLabel(SystemLogState state)")])
    def block(marker: str) -> str:
        # A struct definition needs its closing semicolon back.
        return extract_block(debug, marker) + (";" if marker.startswith("struct ") else "")
    production = "\n\n".join(block(marker) for marker in (
        "bool parseSystemLogFlags(const String& flagsStr, DebugFlagMask& out)",
        "void formatSystemLogFlags(DebugFlagMask mask, char* out, size_t outSize)",
        "String formatSystemLogFlags(DebugFlagMask mask)",
        "bool systemLogApplyPersistedFlags()",
        "bool systemLogFollowFlagChange(DebugFlagMask touched)",
        "static size_t systemLogMaxSizeBytes()",
        "static int systemLogMaxRotations()",
        "static bool systemLogBlockedByReserve(const String& path)",
        "SystemLogState systemLogState()",
        "struct SystemLogStatus",
        "static void systemLogReadStatus(SystemLogStatus& st)",
        "static void systemLogFormatStatusText(const SystemLogStatus& st, char* out, size_t outSize)",
        "void formatSystemLogStatus(char* out, size_t outSize)",
        "static bool systemLogFormatStatusJson(const SystemLogStatus& st, char* out, size_t outSize)",
        "bool formatSystemLogStatusJson(char* out, size_t outSize)"))
    harness = (HERE / "system_log_status_harness.cpp").read_text()
    for marker, value in (("// INSERT_TYPES", types), ("// INSERT_PRODUCTION", production)):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, value)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    sizes = caller_sizes()
    reader_contracts()
    with tempfile.TemporaryDirectory(prefix="hw1-system-log-status-") as temporary:
        unit, binary = Path(temporary) / "test.cpp", Path(temporary) / "test"
        unit.write_text(generate())
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", *(f"-D{name}={value}" for name, value in sizes.items()),
                   "-I", str(HERE / "web_batch_stubs"), str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        require(result.returncode == 0, f"system log status failed:\n{result.stdout}\n{result.stderr}")
        count = re.search(r"(?m)^checks=(\d+)$", result.stderr)
        require(count is not None, "missing completed check count")
        print(f"{result.stdout.strip()} ({count[1]} checks; CLI {sizes['HW1_CLI_STATUS_BYTES']} B, "
              f"web {sizes['HW1_WEB_STATUS_BYTES']} B, flags {sizes['HW1_FLAGS_TEXT_BYTES']} B)", flush=True)


if __name__ == "__main__":
    main()
