#!/usr/bin/env python3
"""Execute the production CLI mode lifecycle and completion owner check.

The entire framework implementation is compiled with controlled task, mutex,
clock and session boundaries. Callbacks model staged work without duplicating
the framework's exit reasons or eligibility rules.
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


def generate() -> str:
    source = (COMPONENT / "System_CLIMode.cpp").read_text()
    header = (COMPONENT / "System_CLIMode.h").read_text()
    user = (COMPONENT / "System_User.h").read_text()
    command = (COMPONENT / "System_CommandTypes.h").read_text()
    require("bool cliModeExitCanComplete();" in header, "completion owner API missing")
    types = extract_block(user, "enum CommandSource") + ";\n"
    types += extract_block(user, "struct AuthContext") + ";\n"
    types += "using TransportSessionEpoch = uint32_t;\n"
    types += extract_block(command, "enum CommandContextBehavior") + ";\n"
    types += re.sub(r"(?m)^#include[^\n]*\n", "", header)
    source = re.sub(r"(?m)^#include[^\n]*\n", "", source)
    harness = (HERE / "climode_completion_owner_harness.cpp").read_text()
    for marker, replacement in (("// INSERT_TYPES", types), ("// INSERT_PRODUCTION", source)):
        require(harness.count(marker) == 1, f"bad harness marker: {marker}")
        harness = harness.replace(marker, replacement)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-climode-completion-owner-") as temporary:
        directory = Path(temporary)
        unit = directory / "climode.cpp"
        unit.write_text(generate())
        output = directory / "climode"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-pthread", str(unit), "-o", str(output)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        subprocess.run([str(output)], check=True, timeout=30)


if __name__ == "__main__":
    main()
