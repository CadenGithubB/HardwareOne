#!/usr/bin/env python3
"""Execute the OLED Power > Sleep Settings rows against the real powersave command.

Splices in the production Sleep Settings row action and input handler
(OLED_Mode_Power.cpp executePowerSleepAction, powerSleepInputHandler), the OLED
action helper (OLED_Utils.cpp oledRunCommand) and the real `powersave` handler
(System_Power.cpp cmd_powersave). The OLED command executor is a double that
applies the admin gate with the requiresAdmin flag of the production `powersave`
row in powerCommands[], then runs cmd_powersave and records a [CMD] audit line;
setSetting is a double that records whether a write ran inside a command or
directly from the OLED task.

The cases check that the "Power Saving" row submits `powersave <next step>`
instead of writing the setting: a non-admin is refused ("Admin required" toast,
value kept), an admin's press moves along the preset ladder with one audit
record and no direct write, a guest is still blocked, and the other rows run
their commands as before. Static checks: no OLED_Mode_*.cpp calls setSetting
(settings change only through commands), and the production `powersave` row is
admin-only. `--component <dir>` runs the same cases against saved sources (the
base tree writes the setting directly, so a non-admin changes it unaudited).
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

RUN_COMMAND = "bool oledRunCommand("
REPLY_TEXT = "void oledCommandReplyText("
POWER_SLEEP = ("static void executePowerSleepAction(", "static bool powerSleepInputHandler(")
CMD_POWERSAVE = "static const char* cmd_powersave("
POWERSAVE_ROW = re.compile(
    r'\{\s*"powersave"\s*,\s*"(?:[^"\\]|\\.)*"\s*,\s*(true|false)\s*,\s*cmd_powersave\b')


def functions(source: str, name: str, markers: tuple[str, ...]) -> str:
    parts = []
    for marker in markers:
        require(source.count(marker) == 1, f"{name}: marker missing or not unique: {marker}")
        parts.append(extract_block(source, marker))
    return "\n\n".join(parts)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"(?m)(^|\s)//[^\n]*", r"\1", text)


def static_checks(names: list[str], read, power_row_admin: bool) -> list[str]:
    failures = []
    for name in names:
        calls = [" ".join(call.split())
                 for call in re.findall(r"\bsetSetting\s*\(([^;]*)\);", strip_comments(read(name)))]
        if calls:
            failures.append(f"{name} writes settings directly from the OLED task (no command, no admin "
                            "check, no [CMD] audit): " + ", ".join(f"setSetting({c})" for c in calls))
    if not power_row_admin:
        failures.append("System_Power.cpp: the powersave command is not admin-only")
    return failures


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

    mode_files = {path.name for path in args.component.glob("OLED_Mode_*.cpp")}
    if args.source_dir:
        mode_files |= {path.name for path in args.source_dir.glob("OLED_Mode_*.cpp")}
    mode_files = sorted(name for name in mode_files if not name.startswith("._"))
    require("OLED_Mode_Power.cpp" in mode_files, "OLED_Mode_Power.cpp not found")

    utils = read("OLED_Utils.cpp")
    have_helper = RUN_COMMAND in utils
    power = read("System_Power.cpp")
    rows = POWERSAVE_ROW.findall(power)
    require(len(rows) == 1, "System_Power.cpp: the powersave command row is missing or not unique")
    power_row_admin = rows[0] == "true"

    harness = (HERE / "oled_power_settings_harness.cpp").read_text()
    require(power.count(CMD_POWERSAVE) == 1, f"System_Power.cpp: marker missing or not unique: {CMD_POWERSAVE}")
    inserts = (
        ("// INSERT_CMD_POWERSAVE", extract_block(power, CMD_POWERSAVE)),
        ("// INSERT_RUN_COMMAND", "\n\n".join(extract_block(utils, marker)
                                               for marker in (REPLY_TEXT, RUN_COMMAND)
                                               if marker in utils)),
        ("// INSERT_POWER_SLEEP", functions(read("OLED_Mode_Power.cpp"), "OLED_Mode_Power.cpp", POWER_SLEEP)),
    )
    for marker, body in inserts:
        require(harness.count(marker) == 1, f"harness marker missing/duplicated: {marker}")
        harness = harness.replace(marker, body)

    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function",
             "-Wno-unused-parameter",
             f"-DHW1_HAVE_RUN_COMMAND={1 if have_helper else 0}",
             f"-DHW1_POWERSAVE_REQUIRES_ADMIN={1 if power_row_admin else 0}"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-oled-power-settings-") as directory:
        unit, executable = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(harness)
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)], check=True, timeout=300)
        run = subprocess.run([str(executable)], timeout=60)
    failures = static_checks(mode_files, read, power_row_admin)
    if not have_helper:
        failures.insert(0, "OLED_Utils.cpp has no oledRunCommand(): the OLED action helper is missing")
    for failure in failures:
        print(f"FAIL [static] {failure}", file=sys.stderr)
    if run.returncode != 0 or failures:
        raise SystemExit("oled_power_settings: FAILED")
    print("oled_power_settings: Power Saving goes through the admin-only powersave command; "
          "no OLED_Mode_*.cpp writes settings directly")


if __name__ == "__main__":
    main()
