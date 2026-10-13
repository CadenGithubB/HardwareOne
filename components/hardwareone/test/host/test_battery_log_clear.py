#!/usr/bin/env python3
"""Run the shipping `batterylog clear` branch against the shared fake VFS.

Extracts the clear branch of cmd_batterylog and the log path constants from
System_Battery.cpp, compiles them with battery_log_clear_harness.cpp and the
shared fake_vfs.h, and checks that an admin clear removes /battery.csv and its
rotations under the FS lock and replies "Battery log cleared."; that an account
the filesystem rules only let read the log (a role user) gets an "Error: ...
not cleared" reply and keeps every file; that a remove that fails for an admin
is reported with the count of files left; and that an absent log clears
cleanly.

Not covered: the permission table itself (canDelete is a double that answers
as the /battery.csv row does; the row is covered by the filesystem permission
suites), the on/off/interval/tail/status branches and the logger.
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
SCENARIOS = ("admin", "user_refused", "remove_failed", "nothing_to_clear", "other_args")
CLEAR_MARKER = 'if (a.equalsIgnoreCase("clear"))'


def generate() -> str:
    source = (COMPONENT / "System_Battery.cpp").read_text()
    require(source.count(CLEAR_MARKER) == 1, f"marker must be unique: {CLEAR_MARKER}")
    command = extract_block(source, "const char* cmd_batterylog(")
    require(CLEAR_MARKER in command, "the clear branch belongs to cmd_batterylog")
    clear = extract_block(command, CLEAR_MARKER)
    constants = re.findall(r"(?m)^static const (?:char\*|int)\s+kBatteryLog(?:Path|MaxRotations)\s*=[^\n]+",
                           source)
    require(len(constants) == 2, "battery log path and rotation constants")
    harness = (HERE / "battery_log_clear_harness.cpp").read_text()
    for marker, text in (("// INSERT_CONSTANTS", "\n".join(constants)),
                         ("// INSERT_CLEAR_BRANCH", clear)):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, text)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
    failures = []
    with tempfile.TemporaryDirectory(prefix="hw1-battery-log-clear-") as temp:
        work = Path(temp)
        unit, binary = work / "battery_log_clear.cpp", work / "battery_log_clear"
        unit.write_text(generate())
        subprocess.run([args.cxx, *flags, "-I", str(HERE / "web_batch_stubs"), "-I", str(HERE),
                        str(unit), "-o", str(binary)], check=True, timeout=120)
        for scenario in SCENARIOS:
            result = subprocess.run([str(binary), scenario], capture_output=True, text=True, timeout=30)
            output = (result.stdout + result.stderr).strip()
            print(f"battery_log_clear {scenario}: {output}")
            if result.returncode != 0:
                failures.append(scenario)
    require(not failures, "failed: " + ", ".join(failures))
    print(f"Battery log clear: {len(SCENARIOS)} scenarios passed")


if __name__ == "__main__":
    main()
