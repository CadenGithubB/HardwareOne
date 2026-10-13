#!/usr/bin/env python3
"""Drive the production OLED "New automation" wizard against host doubles.

Splices in the real wizard from OLED_Mode_Automations.cpp (state, keyboard
completion wizKeyboardDone, wizCreate, the confirm screen and wizHandleInput,
from `enum AutoWizStep` through the end of wizHandleInput) and its footer toast
autoShowToast, plus OLED_KEYBOARD_MAX_LENGTH and the keyboard enums from
OLED_Utils.h. The display, keyboard, event catalog and OLED command executor
are doubles.

The cases check that a 40-character name and command, and a command of the
keyboard's full length, reach `automationadd` whole; that text which does not
fit is refused instead of stored cut short; that the confirm screen shows every
command character (Down scrolls the rows) and keeps the old four-line layout
for a short command. `--component <dir>` runs the same cases against saved
sources (the base tree keeps 32 characters and shows 18 on confirm).
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block, require  # noqa: E402

WIZARD_START = "enum AutoWizStep"
WIZARD_LAST = "static bool wizHandleInput("
TOAST = "static void autoShowToast("


def unique(source: str, marker: str, name: str) -> int:
    require(source.count(marker) == 1, f"{name}: marker missing or not unique: {marker}")
    return source.index(marker)


def wizard_region(source: str) -> str:
    start = unique(source, WIZARD_START, "OLED_Mode_Automations.cpp")
    last = unique(source, WIZARD_LAST, "OLED_Mode_Automations.cpp")
    require(start < last, "wizard state must precede wizHandleInput")
    return source[start:last + len(extract_block(source, WIZARD_LAST))]


def keyboard_types(utils_header: str) -> str:
    cap = re.search(r"^#define OLED_KEYBOARD_MAX_LENGTH\s+\d+\s*$", utils_header, re.M)
    require(cap is not None, "OLED_Utils.h: OLED_KEYBOARD_MAX_LENGTH missing")
    return "\n\n".join([cap.group(0),
                        extract_block(utils_header, "enum OLEDKeyboardMode") + ";",
                        extract_block(utils_header, "enum class OLEDKeyboardDictationPolicy") + ";"])


def generated(read) -> str:
    automations = read("OLED_Mode_Automations.cpp")
    unique(automations, TOAST, "OLED_Mode_Automations.cpp")
    harness = (HERE / "oled_automation_wizard_harness.cpp").read_text()
    inserts = (("// INSERT_KEYBOARD_TYPES", keyboard_types(read("OLED_Utils.h"))),
               ("// INSERT_TOAST", extract_block(automations, TOAST)),
               ("// INSERT_WIZARD", wizard_region(automations)))
    for marker, body in inserts:
        require(harness.count(marker) == 1, f"harness marker missing/duplicated: {marker}")
        harness = harness.replace(marker, body)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1],
                        help="production component directory (point at a saved tree for a base run)")
    parser.add_argument("--source-dir", type=Path,
                        help="overlay directory; a file present here wins over --component")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")

    def read(name: str) -> str:
        overlay = args.source_dir / name if args.source_dir else None
        return (overlay if overlay and overlay.exists() else args.component / name).read_text()

    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-oled-automation-wizard-") as directory:
        unit, executable = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(generated(read))
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)], check=True, timeout=300)
        run = subprocess.run([str(executable)], timeout=60)
    if run.returncode != 0:
        raise SystemExit("oled_automation_wizard: FAILED")


if __name__ == "__main__":
    main()
