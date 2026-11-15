#!/usr/bin/env python3
"""millis() deadlines are asked through System_Deadline.h, which survives the wrap.

A deadline is a 32-bit millis() value with 0 for "none". The production header
System_Deadline.h (deadlineAfter, deadlinePending) is compiled as it is,
together with System_ESPNow_Deadline.h, whose two names forward to it. The
cases put the clock on both sides of 2^31 and of the wrap at 2^32: a deadline
that was never set is pending at no uptime, one set just before the wrap is
pending until it is reached, one left over from before the wrap stays passed,
and no deadline is ever stored as 0.

Source rules: each listed deadline variable is asked through deadlinePending()
in its file, and none of the old forms is left for it: a comparison with
millis() through < or >=, or a signed difference without the zero test.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --component <dir> to run
the same cases and rules against another components/hardwareone directory.
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

from test_web_batch_handlers import require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

# (file, deadline variable as the file spells it, number of deadlinePending calls on it)
SITES = (
    # Deadlines whose 0 means "none" or "now" and that were read as a bare signed difference.
    ("G2_Page_LLM.cpp", "noticeUntil", 1),
    ("OLED_Mode_Transcription.cpp", "v.pollAt", 2),
    ("OLED_Mode_Camera.cpp", "retryAt", 1),
    ("OLED_Mode_R1_Health.cpp", "sHealthDeniedUntilMs", 1),
)


def strip_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", " ", source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def site_failures(component: Path) -> list[str]:
    failures = []
    for name, variable, calls in SITES:
        path = component / name
        if not path.exists():
            failures.append(f"{name}: file not found")
            continue
        code = strip_comments(path.read_text())
        var = re.escape(variable)
        now = r"(?:millis\(\)|\w*[nN]ow\w*)"
        found = len(re.findall(rf"deadlinePending\(\s*{var}\s*,", code))
        if found != calls:
            failures.append(f"{name}: {variable} is asked through deadlinePending() {found} time(s), expected {calls}")
        if '#include "System_Deadline.h"' not in code:
            failures.append(f"{name}: does not include System_Deadline.h")
        old_forms = (
            (rf"{now}\s*(?:<|>=|>|<=)\s*{var}\b", "millis() compared with the deadline"),
            (rf"\b{var}\s*(?:<|>=|>|<=)\s*{now}", "the deadline compared with millis()"),
            (rf"int32_t\s*\)?\s*\(\s*{now}\s*-\s*{var}\b", "a bare signed difference"),
            (rf"int32_t\s*\)?\s*\(\s*{var}\s*-\s*{now}", "a bare signed difference"),
            (rf"\(long\)\s*\(\s*(?:{now}\s*-\s*{var}\b|{var}\s*-\s*{now})", "a bare signed difference"),
        )
        for pattern, what in old_forms:
            for match in re.finditer(pattern, code):
                failures.append(f"{name}: {what}: {match.group(0)!r}")
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    failures = []
    if not (component / "System_Deadline.h").exists():
        failures.append("System_Deadline.h, the deadline owner, does not exist")
    else:
        flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-I", str(component)]
        if args.sanitize:
            flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                      "-fno-omit-frame-pointer", "-g"]
        with tempfile.TemporaryDirectory(prefix="hw1-millis-deadlines-") as directory:
            binary = Path(directory) / "millis_deadlines"
            built = subprocess.run([args.cxx, *flags, str(HERE / "millis_deadlines_harness.cpp"),
                                    "-o", str(binary)], timeout=180)
            if built.returncode != 0:
                failures.append("the deadline harness does not compile")
            elif subprocess.run([str(binary)], timeout=60).returncode != 0:
                failures.append("the deadline harness failed")
    failures += site_failures(component)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        print(f"millis() deadlines: {len(failures)} failure(s)", file=sys.stderr)
        sys.exit(1)
    print(f"millis() deadlines: the owner header and {len(SITES)} deadline variables passed", flush=True)


if __name__ == "__main__":
    main()
