#!/usr/bin/env python3
"""Run the production boot-automation pass and count its command copies.

runAutomationsOnBoot() strdup()s each automation's commands while it parses
the object, then evaluates the global condition. A false condition must free
those copies before it moves on: the harness counts every strdup and
free made by the inserted owner code and fails on any copy still live after the
pass. The file read, condition evaluator, executor, logging and event post are
boundary doubles; the pass and its JSON object scanner come from the firmware.

--component points the suite at another copy of the component (the pre-fix tree
fails case 1).
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
ONCE_GUARD = "static bool s_ran = false;"


def generated(component: Path) -> str:
    owner = (component / "System_Automation.cpp").read_text()
    boot = extract_block(owner, "void runAutomationsOnBoot()")
    require(boot.count(ONCE_GUARD) == 1, "boot pass keeps its once-per-boot guard")
    boot = boot.replace(ONCE_GUARD, "bool& s_ran = gTestBootRan;")
    replacements = {
        "// INSERT_OBJECT_END": extract_block(owner, "static int findJsonObjectEnd("),
        "// INSERT_BOOT_PASS": boot,
    }
    harness = (HERE / "boot_automation_leak_harness.cpp").read_text()
    for marker, replacement in replacements.items():
        require(harness.count(marker) == 1, f"exactly one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--component", type=Path, default=COMPONENT)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
             "-Wno-unused-function", "-Wno-unused-variable", "-Wno-sign-compare"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-boot-automation-") as directory:
        work = Path(directory)
        source, binary = work / "test.cpp", work / "test"
        source.write_text(generated(args.component))
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
