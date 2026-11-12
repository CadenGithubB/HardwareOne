#!/usr/bin/env python3
"""Command names are lower-case words without separators.

The rule (CONTRIBUTING.md, "Command names"):

1. A command name is lower-case letters and digits: `mqtthost`,
   `httpautostart`, `validateconditions`. No capital letter, hyphen or
   underscore.
2. A family of sub-commands puts one space between its words, each word in the
   same form: `phone pair new`, `ring panel on`, `cm5 power halt`.
3. The `cmdKey` of a setting row is the name of its set command, so it has the
   same form.
4. A name that was public in another form before 1.0 stays as an alias row for
   one release. OLD_NAMES lists these rows; the test checks that each one
   still exists, says "Alias of <name>." and points at a row that exists.
   KEPT_NAMES lists names that keep another form, each with its reason.

Lookup ignores case (System_CommandLookupCore.h), so `mqttHost` typed by a user
or sent by an older script still runs `mqtthost`; that needs no alias row.

Reads the working tree. --component points the test at another component
directory.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

from test_shared_text_helpers import blank_comments, sources

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

FORM = re.compile(r"[a-z0-9]+(?: [a-z0-9]+)*")
COMMAND_TABLE = re.compile(r"\bCommandEntry\s+(\w+)\s*\[\s*\]\s*(?:PROGMEM\s*)?=\s*\{")
SETTING_TABLE = re.compile(r"\bSettingEntry\s+(\w+)\s*\[\s*\]\s*=\s*\{")
CMD_KEY_FIELD = 12  # SettingEntry: the 13th field

# Old public name -> the name it is an alias of. Remove a row here together
# with its table row when the alias is retired.
OLD_NAMES: dict[str, str] = {
    "c6_console": "c6console",
    "g2ai-direct": "g2aidirect",
    "g2ai-noask": "g2ainoask",
    "validate-conditions": "validateconditions",
}
# Names that keep another form, with the reason.
KEPT_NAMES: dict[str, str] = {
    "cm5 power sleep_for": "sleep_for is the action word of the CM5 host protocol",
}


def rows(code: str, start: int):
    """Yield (offset, fields) for each brace row of the table that opens before `start`."""
    depth, i, row_start, fields, field_start = 1, start, None, [], start
    while depth and i < len(code):
        c = code[i]
        if c == '"':
            i += 1
            while i < len(code) and code[i] != '"':
                i += 2 if code[i] == "\\" else 1
        elif c in "{(":
            depth += 1
            if depth == 2 and c == "{":
                row_start, fields, field_start = i, [], i + 1
        elif c in "})":
            depth -= 1
            if depth == 1 and c == "}" and row_start is not None:
                fields.append(code[field_start:i].strip())
                yield row_start, fields
                row_start = None
        elif c == "," and depth == 2 and row_start is not None:
            fields.append(code[field_start:i].strip())
            field_start = i + 1
        i += 1


def literal(field: str) -> str | None:
    """The text of a field that is one string literal, else None."""
    if len(field) >= 2 and field[0] == '"' and field[-1] == '"':
        return field[1:-1]
    return None


def measure(component: Path):
    commands = []   # (name, help, file, line)
    keys = []       # (cmdKey, file, line)
    checked = 0
    for relative, text in sources(component, (".h", ".cpp", ".inc")):
        checked += 1
        code = blank_comments(text, keep_strings=True)
        for table in COMMAND_TABLE.finditer(code):
            for offset, fields in rows(code, table.end()):
                name = literal(fields[0])
                if name is None:
                    continue
                line = code.count("\n", 0, offset) + 1
                helptext = literal(fields[1]) if len(fields) > 1 else None
                commands.append((name, helptext or "", relative, line))
        for table in SETTING_TABLE.finditer(code):
            for offset, fields in rows(code, table.end()):
                if len(fields) <= CMD_KEY_FIELD:
                    continue
                key = literal(fields[CMD_KEY_FIELD])
                if key is not None:
                    keys.append((key, relative, code.count("\n", 0, offset) + 1))
    return checked, commands, keys


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    component = args.component.resolve()

    checked, commands, keys = measure(component)
    if not commands or not keys:
        raise SystemExit(f"command name form: no command or setting tables under {component}")

    failures = []
    names = {name for name, _, _, _ in commands}
    for name, helptext, relative, line in commands:
        if FORM.fullmatch(name) or name in KEPT_NAMES:
            continue
        if name in OLD_NAMES:
            want = f"Alias of {OLD_NAMES[name]}."
            if helptext != want:
                failures.append(f"alias row \"{name}\" has the help \"{helptext}\", expected \"{want}\"\n"
                                f"    {relative}:{line}")
            continue
        fixed = re.sub(r"[^a-z0-9 ]", "", name.lower())
        failures.append(f"command name \"{name}\" is not lower-case letters and digits, expected \"{fixed}\"\n"
                        f"    {relative}:{line}")
    for old, new in sorted(OLD_NAMES.items()):
        if old not in names:
            failures.append(f"OLD_NAMES lists \"{old}\" but no table has that row; remove the entry")
        if new not in names:
            failures.append(f"OLD_NAMES maps \"{old}\" to \"{new}\", which is not a command")
        if not FORM.fullmatch(new):
            failures.append(f"OLD_NAMES maps \"{old}\" to \"{new}\", which does not have the form either")
    for kept in sorted(KEPT_NAMES):
        if kept not in names:
            failures.append(f"KEPT_NAMES lists \"{kept}\" but no table has that row; remove the entry")
    for key, relative, line in keys:
        if not FORM.fullmatch(key):
            failures.append(f"setting cmdKey \"{key}\" is not lower-case letters and digits, "
                            f"expected \"{key.lower()}\"\n    {relative}:{line}")

    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    if failures:
        raise SystemExit(f"command name form: {len(failures)} violation(s)")
    spaced = sum(1 for name in names if " " in name)
    print(f"command name form: {checked} source files, {len(names)} command names "
          f"({spaced} with sub-command words, {len(OLD_NAMES)} old-name aliases, "
          f"{len(KEPT_NAMES)} kept), {len(keys)} setting cmdKeys")


if __name__ == "__main__":
    main()
