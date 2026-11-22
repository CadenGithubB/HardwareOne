#!/usr/bin/env python3
"""Four <0|1> setting commands read their value with the shared on/off parser.

httpautostart and httpsenabled (System_Settings.cpp) and
thermalrollingminmaxenabled and thermalinterpolationenabled
(i2csensor_mlx90640.cpp) each had their own test, "starts with 1 or with
true", and read every other word as off (RAD-5; espnowenabled, the fifth, was
removed with the other <x>enabled switches on 2026-11-21). The four handlers,
parseBoolArg and setSetting are extracted intact on every run and compiled
into settings_onoff_commands_harness.cpp with the real System_CommandReply.h;
String storage, the reply buffer, persistence and the printed line are the
host boundaries.

Checked for each command, from both starting values: on/true/1/enable and
off/false/0/disable in any case and with surrounding spaces store the value
with the reply the command always gave; a save is requested only when the
value changes; the thermal pair print their line; a word that is neither
(also "10", "truex", "1 extra") is refused with kCommandReplyNotOnOff and
changes, saves and prints nothing; no argument still answers with the usage
line; validate-only still answers VALID.

--baseline DIR replays the same assertions against a saved component
directory; the tree before this change fails the on/enable words and the
refusals.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

SETTINGS_HANDLERS = ("cmd_httpAutoStart", "cmd_httpsEnabled")
THERMAL_HANDLERS = ("cmd_thermalrollingminmaxenabled", "cmd_thermalinterpolationenabled")


def extract_macro(source, name):
    lines = source[source.index(f"#define {name}("):].splitlines()
    result = []
    for line in lines:
        result.append(line)
        if not line.endswith("\\"):
            return "\n".join(result)
    raise AssertionError(f"unterminated macro {name}")


def handler(source, name, where):
    marker = f"const char* {name}(const String& argsInput) {{"
    assert source.count(marker) == 1, f"{where}: expected one definition of {name}"
    return extract_block(source, marker[:-1])


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    source_dir = (args.baseline or COMPONENT).resolve()
    settings = (source_dir / "System_Settings.cpp").read_text()
    thermal = (source_dir / "i2csensor_mlx90640.cpp").read_text()
    utils = (source_dir / "System_Utils.h").read_text()
    settings_h = (source_dir / "System_Settings.h").read_text()
    definitions = "\n\n".join([
        extract_macro(utils, "RETURN_VALID_IF_VALIDATE_CSTR"),
        extract_block(utils, "inline int parseBoolArg("),
        extract_block(settings_h, "template<typename T>\ninline void setSetting("),
        *[handler(settings, name, "System_Settings.cpp") for name in SETTINGS_HANDLERS],
        *[handler(thermal, name, "i2csensor_mlx90640.cpp") for name in THERMAL_HANDLERS],
    ])
    harness = (HERE / "settings_onoff_commands_harness.cpp").read_text()
    marker = "// INSERT_PRODUCTION_DEFINITIONS"
    assert harness.count(marker) == 1
    results = []
    with tempfile.TemporaryDirectory(prefix="hw1-settings-onoff-") as temp:
        unit = Path(temp) / "commands.cpp"
        unit.write_text(harness.replace(marker, definitions))
        for espnow in (1, 0):
            executable = Path(temp) / f"commands-{espnow}"
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       f"-DENABLE_ESPNOW={espnow}", "-I", str(source_dir),
                       str(unit), "-o", str(executable)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=120)
            results.append(subprocess.run([str(executable)], timeout=60).returncode)
    raise SystemExit(1 if any(results) else 0)


if __name__ == "__main__":
    main()
