#!/usr/bin/env python3
"""A handler named after a command serves that command.

A command table row names its handler function. Most handlers are named after
their command (`cmd_wifistatus` serves `wifistatus`). A handler whose name is
the name of ANOTHER command that exists reads as that command's code and is
not: `cmd_wifidisconnect` was the handler of `closewifi` (which also stops the
web server), while the command `wifidisconnect` ran `cmd_wifidrop`.

The rule: when the name of a handler, without its `cmd` prefix and its
underscores, is the name of a command in some table, the handler is on a row
of that command. HANDLERS pins the rows of the two Wi-Fi disconnects.
RETIRED lists handler names that were never commands: `wifidrop` had found
its way into comments and into the user guide as if it could be typed. No
firmware source and no user document may name one (CHANGELOG.md keeps its
history and is not read).

Reads the working tree. --component points the test at another component
directory.
"""
from __future__ import annotations

import argparse
import collections
from pathlib import Path
import re
import sys

from test_command_name_form import COMMAND_TABLE, literal, rows
from test_shared_text_helpers import blank_comments, sources

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

HANDLER_FIELD = 3  # CommandEntry: name, help, requiresAdmin, handler
HANDLER_NAME = re.compile(r"cmd_?(\w+)")

# Command -> the handler its row names.
HANDLERS: dict[str, str] = {
    "closewifi": "cmd_closewifi",
    "wifidisconnect": "cmd_wifidisconnect",
}
# Retired handler words -> the command that was meant.
RETIRED: dict[str, str] = {
    "wifidrop": "wifidisconnect",
}
# User documents, relative to the repository root.
DOCUMENTS = ("docs/USERGUIDE.md", "docs/QUICKSTART.md", "docs/KNOWN_ISSUES.md", "README.md",
             "docs/README_DETAILS.md")


def measure(component: Path):
    served = collections.defaultdict(set)   # handler -> command names
    where = {}                               # (handler, command) -> "file:line"
    for relative, text in sources(component, (".h", ".cpp", ".inc")):
        code = blank_comments(text, keep_strings=True)
        for table in COMMAND_TABLE.finditer(code):
            for offset, fields in rows(code, table.end()):
                name = literal(fields[0])
                if name is None or len(fields) <= HANDLER_FIELD:
                    continue
                handler = fields[HANDLER_FIELD]
                served[handler].add(name)
                where[(handler, name)] = f"{relative}:{code.count(chr(10), 0, offset) + 1}"
    return served, where


def flat(name: str) -> str:
    return name.replace(" ", "")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    component = args.component.resolve()

    served, where = measure(component)
    commands = {name for names in served.values() for name in names}
    if len(commands) < 500:
        raise SystemExit(f"command handler names: only {len(commands)} commands found under {component}")
    by_flat = {flat(name): name for name in commands}

    failures = []
    for handler, names in sorted(served.items()):
        match = HANDLER_NAME.fullmatch(handler)
        if not match:
            continue
        key = match.group(1).replace("_", "").lower()
        if key in by_flat and key not in {flat(name) for name in names}:
            rows_text = ", ".join(f"`{name}` ({where[(handler, name)]})" for name in sorted(names))
            failures.append(f"{handler} is named after the command `{by_flat[key]}` but is the handler of "
                            f"{rows_text}")
    handler_of = collections.defaultdict(set)
    for handler, names in served.items():
        for name in names:
            handler_of[name].add(handler)
    for command, handler in sorted(HANDLERS.items()):
        if handler_of.get(command) != {handler}:
            failures.append(f"`{command}` is handled by {sorted(handler_of.get(command, []))}, "
                            f"expected {handler}")

    texts = {relative: text for relative, text in sources(component, (".h", ".cpp", ".inc"))}
    repo = component.parents[1]
    for document in DOCUMENTS:
        if (repo / document).exists():
            texts[document] = (repo / document).read_text(encoding="utf-8")
    for word, meant in sorted(RETIRED.items()):
        if word in commands:
            failures.append(f"RETIRED lists `{word}`, which is a command now; remove the entry")
        for relative, text in sorted(texts.items()):
            for number, line in enumerate(text.splitlines(), 1):
                if re.search(rf"\b(?:cmd_)?{word}\b", line):
                    failures.append(f"{relative}:{number} names `{word}`, which is not a command; "
                                    f"the command is `{meant}`")

    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    if failures:
        raise SystemExit(f"command handler names: {len(failures)} violation(s)")
    print(f"command handler names: {len(commands)} commands, {len(served)} handlers, "
          f"none named after another command")


if __name__ == "__main__":
    main()
