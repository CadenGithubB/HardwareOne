#!/usr/bin/env python3
"""Which requests and commands reach the auth log, and what it costs not to.

logAuthAttempt runs for every authenticated web request and for every
command. It used to copy the path, undo two URL escapes on the copy, and only
then test whether the path is a security event (a "<transport>/login" context,
the G2 pair event, the ESP-NOW bond), which nearly no call is; callers also
passed a char buffer for a String reason. The path is now tested on the text
at hand (a path without '%' is its own cleaned path) and the reason is plain
text, copied only for a line that is written.

The production logAuthAttempt, its path test, authLogPath and appendAuthLogLine
(System_Debug.cpp) run on the real Arduino String with its heap calls counted.
A table of paths, escaped and not, fixes which ones are written and the exact
line and file of each (the old tree writes the same lines); two calls that are
not security events make no String block. The declaration in System_User.h
takes the reason as const char*.

Options: --cxx <compiler>, --sanitize, --component <dir> to run against
another components/hardwareone directory.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from arduino_string_host import arduino_string_counted
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generate(component: Path) -> str:
    debug = (component / "System_Debug.cpp").read_text()
    pieces = [extract_block(debug, "static String authLogPath(const char* path)"),
              extract_block(debug, "static void appendAuthLogLine(")]
    if "static bool isSecurityAuditPath(" in debug:
        pieces.append(extract_block(debug, "static bool isSecurityAuditPath("))
    pieces.append(extract_block(debug, "void logAuthAttempt(bool success, const char* path,"))
    harness = (HERE / "auth_log_paths_harness.cpp").read_text()
    marker = "// INSERT_PRODUCTION"
    require(harness.count(marker) == 1, "one insertion marker")
    return harness.replace(marker, "\n\n".join(pieces))


def source_contract(component: Path) -> None:
    header = (component / "System_User.h").read_text()
    require("void logAuthAttempt(bool success, const char* path, const String& userTried,\n"
            "                    const String& ip, const char* reason);" in header,
            "System_User.h declares logAuthAttempt with a const char* reason")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
    with tempfile.TemporaryDirectory(prefix="hw1-auth-log-paths-") as directory:
        d = Path(directory)
        includes, sources = arduino_string_counted(d, sanitize)
        unit, binary = d / "test.cpp", d / "test"
        unit.write_text(generate(args.component))
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-Wno-sign-compare", *sanitize, *includes, str(unit), *sources, "-o", str(binary)],
                       check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=30)
    source_contract(args.component)
    print("auth log paths source contract passed")


if __name__ == "__main__":
    main()
