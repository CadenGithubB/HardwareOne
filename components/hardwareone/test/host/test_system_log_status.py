#!/usr/bin/env python3
"""Execute the shipping system-log status and flag formatters.

formatSystemLogStatus, both formatSystemLogFlags overloads, parseSystemLogFlags
and systemLogApplyPersistedFlags come from System_Debug.cpp; DebugFlagMask,
DEBUG_BIT and the MSG_ROUTE_* values from System_Debug.h, all unchanged. The
buffer sizes are read from the production callers (CLI `log status`, the web
status page and the flag messages), so a caller change fails here first.
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
    page = re.findall(r"char logBuf\[(\d+)\];\s*\n\s*formatSystemLogStatus\(logBuf, sizeof\(logBuf\)\);", web)
    require(len(page) == 1, "one web log-status caller with a sized local buffer")
    flags = set(re.findall(r"char flagsText\[(\d+)\];\s*\n\s*formatSystemLogFlags\(", debug))
    require(len(flags) == 1, "the flag-message callers share one buffer size")
    return {"HW1_CLI_STATUS_BYTES": int(cli[0]), "HW1_WEB_STATUS_BYTES": int(page[0]),
            "HW1_FLAGS_TEXT_BYTES": int(flags.pop())}


def generate() -> str:
    debug = (COMPONENT / "System_Debug.cpp").read_text()
    debug_h = (COMPONENT / "System_Debug.h").read_text()
    routes = re.findall(r"(?m)^#define MSG_ROUTE_\w+ .*$", debug_h)
    bit = re.findall(r"(?m)^#define DEBUG_BIT\(n\) .*$", debug_h)
    require(routes and len(bit) == 1, "MSG_ROUTE_* and DEBUG_BIT come from System_Debug.h")
    types = "\n".join([extract_block(debug_h, "struct DebugFlagMask") + ";", *routes, *bit])
    production = "\n\n".join(extract_block(debug, marker) for marker in (
        "bool parseSystemLogFlags(const String& flagsStr, DebugFlagMask& out)",
        "void formatSystemLogFlags(DebugFlagMask mask, char* out, size_t outSize)",
        "String formatSystemLogFlags(DebugFlagMask mask)",
        "bool systemLogApplyPersistedFlags()",
        "void formatSystemLogStatus(char* out, size_t outSize)"))
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
