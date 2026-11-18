#!/usr/bin/env python3
"""CommandArgs reads a key only where it stands as a token.

value() and hasKey() used to find "<key>=" anywhere in the line: dayofmonth=5
was read as month=5, bootdelayms= as delayms=, condition= as on=, and an id=
inside commands="..." as the id of the automation to replace. The production
class (System_Command.h) and its constructor, parse(), nextKeyToken(), value()
and hasKey() with their helpers (System_Command.cpp) are compiled as they are:
a key counts at the start of a token (start of the line or after white space)
and outside a quoted value, quoted values keep their spaces and unescape \\",
and the token walk tells key=value tokens from bare words.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --component <dir> to run
the same cases against another components/hardwareone directory.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
MEMBERS = ("CommandArgs::CommandArgs(", "void CommandArgs::parse(",
           "static int scanKeyValueToken(", "bool CommandArgs::nextKeyToken(",
           "static int keyValueStart(", "String CommandArgs::value(", "bool CommandArgs::hasKey(")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    header = (component / "System_Command.h").read_text()
    source = (component / "System_Command.cpp").read_text()
    missing = [member for member in MEMBERS if member not in source]
    if missing:
        # An older tree: its value() matches the key as a substring.
        for member in missing:
            print(f"FAIL: System_Command.cpp: no {member.rstrip('(')}", file=sys.stderr)
        sys.exit(1)
    production = "\n\n".join([extract_block(header, "class CommandArgs") + ";",
                              "const String CommandArgs::empty_;"] +
                             [extract_block(source, member) for member in MEMBERS])
    harness = (HERE / "command_args_keys_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one insertion marker")
    harness = harness.replace("// INSERT_PRODUCTION", production)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-command-args-keys-") as directory:
        unit = Path(directory) / "command_args_keys.cpp"
        binary = Path(directory) / "command_args_keys"
        unit.write_text(harness)
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(binary)], check=True, timeout=180)
        if subprocess.run([str(binary)], timeout=60).returncode != 0:
            print("FAIL: the command key harness failed", file=sys.stderr)
            sys.exit(1)
    print("a key=value key is read only at the start of a token and outside a quoted value", flush=True)


if __name__ == "__main__":
    main()
