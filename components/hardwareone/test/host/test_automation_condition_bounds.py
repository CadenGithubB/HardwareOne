#!/usr/bin/env python3
"""The THEN and ELSE scans of the automation command parser stay inside the command.

executeConditionalCommand and validateConditionSyntax (System_Automation.cpp)
looked for " THEN " with `for (size_t i = 3; i < len - 5; i++)`. For a command
of 3 or 4 characters that starts with "IF " the unsigned bound wraps, and the
loop reads i+1 .. i+5 past the terminator until it meets THEN between spaces or
faults. `automation add` refuses such a command, but a hand-edited or synced
automations.json reaches the executor without that check.

Both production functions are compiled and run. Each short command is followed,
after its terminator, by the text " THEN go ELSE no": a scan that leaves the
command finds it and queues "go". Well-formed commands, the shortest complete
ones included, must behave as before. Source rule: no loop in the file has a
bound of the form `i < length - n` on a size_t.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --component <dir> to run
the same cases against another components/hardwareone directory.
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

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
FUNCTIONS = ("const char* validateConditionSyntax(const char* condition)",
             "const char* executeConditionalCommand(const char* command, const char* owner, const char* autoName)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    automation = (args.component.resolve() / "System_Automation.cpp").read_text()
    for function in FUNCTIONS:
        require(automation.count(function + " {") == 1, f"one definition of {function}")
    harness = (HERE / "automation_condition_bounds_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one insertion marker")
    # The file declares these functions before it defines them: start at the definition.
    harness = harness.replace("// INSERT_PRODUCTION", "\n\n".join(
        extract_block(automation[automation.index(function + " {"):], function) for function in FUNCTIONS))
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-variable",
             "-Wno-unused-but-set-variable", "-Wno-sign-compare", "-I", str(HERE / "web_batch_stubs")]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    failures = []
    with tempfile.TemporaryDirectory(prefix="hw1-automation-condition-bounds-") as directory:
        source = Path(directory) / "condition_bounds.cpp"
        binary = Path(directory) / "condition_bounds"
        source.write_text(harness)
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=180)
        if subprocess.run([str(binary)], timeout=60).returncode != 0:
            failures.append("the condition bounds harness failed")
    for match in re.finditer(r"for \(size_t \w+ = [^;]+; \w+ < \w+ - \d+;", automation):
        line = automation.count("\n", 0, match.start()) + 1
        failures.append(f"System_Automation.cpp:{line}: a size_t bound that wraps for a short text: {match.group(0)!r}")
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        sys.exit(1)
    print("the automation THEN and ELSE scans stay inside the command", flush=True)


if __name__ == "__main__":
    main()
