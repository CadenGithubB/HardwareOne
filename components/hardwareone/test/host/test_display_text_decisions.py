#!/usr/bin/env python3
"""No decision is read from display text.

The rule (CONTRIBUTING.md, "Display text and reply text"): a title, a label
or a reason line is what the user reads. Code that has to know which row,
page or state it is looking at carries that as a value (a mode, an enum, the
setting a row edits) next to the text, and branches on the value. A branch on
the text breaks without a compile error the day the wording changes.

The test fails on a title, label or reason compared with a string literal:

    unavailableOledTitle == "Thermal"
    strcmp(s.label, "Resolution") == 0
    unavailableOledReason.indexOf("Not built") >= 0

ALLOWED lists the uses that remain, per file, each with its reason. A file may
not go above its number and a file not listed has none; when a file drops
below its number the test asks for the smaller number.

Reads the working tree. --component points the test at another component
directory; --print-allowances prints the table for the current tree.
"""
from __future__ import annotations

import argparse
from collections import Counter
from pathlib import Path
import re
import sys

from test_shared_text_helpers import blank_comments, sources

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

NAME = r"[\w.\->\[\]]*(?:[Tt]itle|[Ll]abel|[Rr]eason)\w*"
PATTERNS = (
    re.compile(r"\b" + NAME + r"(?:\.c_str\(\))?\s*(?:==|!=)\s*\""),
    re.compile(r"\bstr(?:n|case|ncase)?cmp\s*\(\s*" + NAME + r"(?:\.c_str\(\))?\s*,\s*\""),
    re.compile(r"\bstrstr\s*\(\s*" + NAME + r"(?:\.c_str\(\))?\s*,\s*\""),
    re.compile(r"\b" + NAME + r"\s*(?:\.|->)\s*(?:indexOf|startsWith|endsWith|equals|equalsIgnoreCase)\s*\(\s*\""),
)

# Uses that remain, per file.
ALLOWED: dict[str, int] = {
    # Not display text: the words a user may not give a mesh as its label.
    "System_ESPNow.cpp": 5,
}


def measure(component: Path):
    found: Counter[str] = Counter()
    where: dict[str, list[str]] = {}
    checked = 0
    for relative, text in sources(component, (".h", ".cpp", ".c", ".inc")):
        checked += 1
        code = blank_comments(text, keep_strings=True)
        starts = set()
        for pattern in PATTERNS:
            for match in pattern.finditer(code):
                starts.add(match.start())
        for start in sorted(starts):
            number = code.count("\n", 0, start) + 1
            line = code.splitlines()[number - 1].strip()
            found[relative] += 1
            where.setdefault(relative, []).append(f"{relative}:{number} {line[:110]}")
    return checked, found, where


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    parser.add_argument("--print-allowances", action="store_true")
    args = parser.parse_args()
    component = args.component.resolve()

    checked, found, where = measure(component)
    if not checked:
        raise SystemExit(f"display text decisions: no sources under {component}")
    if args.print_allowances:
        for relative in sorted(found):
            print(f'    "{relative}": {found[relative]},')
            for line in where[relative]:
                print(f"        # {line}")
        return

    failures = []
    for relative in sorted(set(found) | set(ALLOWED)):
        have, limit = found.get(relative, 0), ALLOWED.get(relative, 0)
        if have > limit:
            failures.append(f"{relative} branches on display text {have} time(s), allowed {limit}; "
                            f"carry a value next to the text and branch on it\n    "
                            + "\n    ".join(where[relative]))
        elif have < limit:
            failures.append(f"{relative} has {have}, the table says {limit}; lower ALLOWED to {have}")
    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    if failures:
        raise SystemExit(f"display text decisions: {len(failures)} violation(s)")
    print(f"display text decisions: {checked} source files, {sum(found.values())} listed use(s)")


if __name__ == "__main__":
    main()
