#!/usr/bin/env python3
"""Exercise the production OLED System Logging > Configure screens.

systemlog_config_ui_harness.cpp runs the whole System Logging > Configure
block of OLED_Mode_Logging.cpp over the shipping debug-flag table, mask
parse/format and systemLogConfiguredFlags (System_Debug), and the shipping
scroll primitives and single-line renderer (OLED_Utils). It checks every frame
against the 128x64 panel, every family and flag row against the shared table,
the commands the rows send (`logcategorytags`, `systemlogflags`), that a flag
toggle keeps the other saved categories, and that Back returns to the row it
came from.
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

SCROLL_FUNCTIONS = ("void oledScrollInit(", "bool oledScrollAddItem(",
                    "void oledScrollClearKeepSelection(", "void oledScrollClampSelection(",
                    "void oledScrollUp(", "void oledScrollDown(", "bool oledScrollHandleNav(",
                    "void oledScrollRenderSimple(")
DEBUG_FUNCTIONS = ("bool parseSystemLogFlags(const String& flagsStr, DebugFlagMask& out)",
                   "void formatSystemLogFlags(DebugFlagMask mask, char* out, size_t outSize)",
                   "bool systemLogApplyPersistedFlags()",
                   "DebugFlagMask systemLogConfiguredFlags()")


def generate(read) -> str:
    debug_h, debug = read("System_Debug.h"), read("System_Debug.cpp")
    utils_h, utils = read("OLED_Utils.h"), read("OLED_Utils.cpp")
    oled = read("OLED_Mode_Logging.cpp")

    bit = re.findall(r"(?m)^#define DEBUG_BIT\(n\) .*$", debug_h)
    cap = re.search(r"(?m)^#define OLED_SCROLL_MAX_ITEMS\s+\d+\s*$", utils_h)
    require(len(bit) == 1 and cap is not None, "DEBUG_BIT and OLED_SCROLL_MAX_ITEMS present")
    types = [extract_block(debug_h, "struct DebugFlagMask") + ";", bit[0],
             read("System_DebugFlags.h"),
             extract_block(utils_h, "enum LoggingMenuState") + ";",
             cap.group(0),
             extract_block(utils_h, "struct OLEDScrollItem") + ";",
             extract_block(utils_h, "struct OLEDScrollState") + ";",
             read("OLED_Layout.h").replace("#pragma once", ""),
             extract_block(utils_h, "inline constexpr int oledVisibleRows(")]

    production = []
    for source, name, marker in ([(debug, "System_Debug.cpp", m) for m in DEBUG_FUNCTIONS] +
                                 [(utils, "OLED_Utils.cpp", m) for m in SCROLL_FUNCTIONS]):
        require(source.count(marker) == 1, f"{name}: marker missing or not unique: {marker}")
        production.append(extract_block(source, marker))
    # The whole Configure block: its first row constant through its input handler.
    start_marker, end_marker = "static constexpr int sysCfgTagsRow", "static bool handleSystemConfigInput("
    require(oled.count(start_marker) == 1 and oled.count(end_marker) == 1,
            "OLED_Mode_Logging.cpp: System Logging > Configure block markers")
    end = oled.index(end_marker)
    end += len(extract_block(oled[end:], end_marker))
    production.append(oled[oled.index(start_marker):end])

    harness = (HERE / "systemlog_config_ui_harness.cpp").read_text()
    for marker, body in (("// INSERT_TYPES", "\n\n".join(types)),
                         ("// INSERT_PRODUCTION", "\n\n".join(production))):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, body)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="production component directory (point at a saved tree for a base run)")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")

    def read(name: str) -> str:
        return (args.component / name).read_text()

    command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
               "-Wno-unused-parameter", "-I", str(HERE / "web_batch_stubs")]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-systemlog-config-ui-") as directory:
        unit, executable = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(generate(read))
        subprocess.run([*command, str(unit), "-o", str(executable)], check=True, timeout=120)
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
        require(result.returncode == 0,
                f"systemlog config UI failed:\n{result.stdout}\n{result.stderr}")
        print(result.stdout.strip(), flush=True)


if __name__ == "__main__":
    main()
