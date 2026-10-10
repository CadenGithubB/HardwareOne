#!/usr/bin/env python3
"""Run production PCA9685 commands and parsing with controlled driver/bus I/O."""
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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    owner = (COMPONENT / "i2csensor_pca9685.cpp").read_text()
    command = (COMPONENT / "System_Command.cpp").read_text()
    types = [extract_block((COMPONENT / "System_Command.h").read_text(), "class CommandArgs") + ";",
             extract_block((COMPONENT / "i2csensor_pca9685.h").read_text(), "struct ServoProfile") + ";"]
    state = [re.search(r"(?m)^.*\b" + name + r"(?:\[.*?\])?\s*(?:=|;).*", owner)[0]
             for name in ("gPwmDriver", "gPwmDriverConnected", "servoProfiles")]
    definitions = ["const String CommandArgs::empty_;"] + [extract_block(command, marker) for marker in (
        "CommandArgs::CommandArgs(", "void CommandArgs::parse()", "const String& CommandArgs::arg(",
        "String CommandArgs::remaining(",
    )] + [extract_block(owner, marker) for marker in (
        "bool servoInit()", "static bool parseServoNumber(", "const char* cmd_servo(",
        "const char* cmd_servoprofile(", "const char* cmd_servocalibrate(", "const char* cmd_pwm(",
    )]
    harness = (HERE / "servo_command_outcome_harness.cpp").read_text()
    harness = (harness.replace("// INSERT_TYPES", "\n\n".join(types))
               .replace("// INSERT_STATE", "\n".join(state))
               .replace("// INSERT_FUNCTIONS", "\n\n".join(definitions)))
    with tempfile.TemporaryDirectory(prefix="hw1-servo-commands-") as directory:
        source = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        source.write_text(harness)
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run([args.cxx, *flags, "-I", str(HERE / "web_batch_stubs"),
                        str(source), "-o", str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
