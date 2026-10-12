#!/usr/bin/env python3
"""Execute the shipping log-level owner and its CLI and boot callers.

applyLogLevel and cmd_loglevel come from System_Debug.cpp, the boot clamp block
from applySettings() in System_Settings.cpp, setDebugSetting from
System_Settings.h and the LOG_LEVEL_*/DEBUG_MANAGER macros from their headers,
all unchanged. DebugManager, esp_log_level_set and the debug-settings
persistence request are recorded boundaries.
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
BOOT_MARKER = "// Apply severity-based log level from settings"


def generate() -> str:
    debug = (COMPONENT / "System_Debug.cpp").read_text()
    debug_h = (COMPONENT / "System_Debug.h").read_text()
    manager_h = (COMPONENT / "System_Debug_Manager.h").read_text()
    settings = (COMPONENT / "System_Settings.cpp").read_text()
    settings_h = (COMPONENT / "System_Settings.h").read_text()
    levels = re.findall(r"(?m)^#define LOG_LEVEL_(?:ERROR|WARN|INFO|DEBUG)[ \t]+\d+[ \t]*$", debug_h)
    require(len(levels) == 4, "System_Debug.h defines LOG_LEVEL_ERROR/WARN/INFO/DEBUG")
    manager = re.findall(r"(?m)^#define DEBUG_MANAGER .*$", manager_h)
    require(len(manager) == 1, "System_Debug_Manager.h defines DEBUG_MANAGER once")
    require(re.search(r"template<typename T>\s*\ninline void setDebugSetting\(", settings_h) is not None,
            "setDebugSetting is the inline template in System_Settings.h")
    setter = "template<typename T>\n" + extract_block(settings_h, "inline void setDebugSetting(")
    require(settings.count(BOOT_MARKER) == 1, "one boot log-level block in System_Settings.cpp")
    boot = extract_block(settings, BOOT_MARKER)
    require("applyLogLevel(" in boot, "the boot block applies the level through applyLogLevel")
    production = "\n\n".join(extract_block(debug, marker) for marker in
                             ("void applyLogLevel(uint8_t level)", "const char* cmd_loglevel(const String& argsInput)"))
    harness = (HERE / "log_level_application_harness.cpp").read_text()
    for marker, value in (("// INSERT_MACROS", "\n".join(levels + manager)),
                          ("// INSERT_SET_DEBUG_SETTING", setter),
                          ("// INSERT_PRODUCTION", production),
                          ("// INSERT_BOOT_BLOCK", boot)):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, value)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-log-level-") as temporary:
        unit, binary = Path(temporary) / "test.cpp", Path(temporary) / "test"
        unit.write_text(generate())
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", "-I", str(HERE / "web_batch_stubs"), str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        require(result.returncode == 0, f"log level application failed:\n{result.stdout}\n{result.stderr}")
        count = re.search(r"(?m)^checks=(\d+)$", result.stderr)
        require(count is not None, "missing completed check count")
        print(f"{result.stdout.strip()} ({count[1]} checks)", flush=True)


if __name__ == "__main__":
    main()
