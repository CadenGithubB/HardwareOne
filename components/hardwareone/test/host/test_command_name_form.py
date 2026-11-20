#!/usr/bin/env python3
"""Command names are lower-case words without separators.

The rule (CONTRIBUTING.md, "Command names"):

1. A command name is lower-case letters and digits: `mqtthost`,
   `httpautostart`, `validateconditions`. No capital letter, hyphen or
   underscore.
2. A family of sub-commands puts one space between its words, each word in the
   same form: `phone pair new`, `ring panel feed`, `cm5 power halt`.
3. The `cmdKey` of a setting row is the name of its set command, so it has the
   same form.
4. One name per command, no alias rows. REMOVED_NAMES maps each removed name
   to the name that stays. The test fails when a removed name is a row again,
   when its kept name is not a row, or when a string in the firmware sources,
   the web assets, tools/ or integrations/ still sends a removed name (the
   audit-log masks, which keep matching a stale line, are listed in
   REMOVED_NAME_USES). KEPT_NAMES lists names that keep another form, each
   with its reason.

Lookup ignores case (System_CommandLookupCore.h), so `mqttHost` typed by a user
or sent by an older script still runs `mqtthost`; that needs no alias row.

Reads the working tree. --component points the test at another component
directory.
"""
from __future__ import annotations

import argparse
import ast
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

# Removed command name -> the name that stays (CONTRIBUTING.md, "Command names").
# Keep this table in step with RETIRED_NAMES in tools/command_registry.py.
REMOVED_NAMES: dict[str, str] = {
    "automationadd": "automation add",
    "automationrun": "automation run",
    "automationtrigger": "automation trigger",
    "bleread": "blestatus",
    "c6_console": "c6console",
    "debugespnow": "debugespnowcore",
    "espnowread": "espnowstatus",
    "g2ai-direct": "g2aidirect",
    "g2ai-noask": "g2ainoask",
    "httpread": "httpstatus",
    "oledread": "oledstatus",
    "oledstart": "openoled",
    "oledstop": "closeoled",
    "ring panel off": "ring panel feed",
    "ring panel on": "ring panel feed",
    "sleep": "wait",
    "srstart": "opensr",
    "srstop": "closesr",
    "validate-conditions": "validateconditions",
    "wifiread": "wifistatus",
}
# (file relative to the repo, removed name) -> why a string there may still
# hold the name. Each listed use must still exist.
REMOVED_NAME_USES: dict[tuple[str, str], str] = {
    ("components/hardwareone/System_Utils.cpp", "automationadd"): "audit-log mask for a stale line",
    ("components/hardwareone/System_Utils.cpp", "validate-conditions"): "audit-log mask for a stale line",
    ("components/hardwareone/System_Automation.cpp", "automationadd"): "audit-log mask for a stale nested line",
    ("components/hardwareone/System_Automation.cpp", "validate-conditions"): "audit-log mask for a stale nested line",
    ("tools/phone_link_peer/hw1peer/app.py", "automationadd"): "peer log mask for a stale line",
    ("components/hardwareone/HAL_Radio.cpp", "c6_console"): "FreeRTOS task name, not a command",
    ("components/hardwareone/R1_Protocol.cpp", "sleep"): "R1 ring sleep subject, not a command",
    ("components/hardwareone/G2_Ring.cpp", "sleep"): "R1 ring sleep subject, not a command",
}
# Folders (relative to the repo) whose strings are searched for removed names.
REMOVED_NAME_SCAN = (
    ("components/hardwareone", (".cpp", ".h", ".inc")),
    ("components/hardwareone/web_assets", (".js", ".html")),
    ("tools", (".py", ".js", ".sh")),
    ("integrations", (".py", ".js", ".ts", ".json", ".yaml", ".yml", ".sh")),
)
SKIPPED_SCAN_PARTS = ("/test/", "/third_party/", "/__pycache__/", "/node_modules/")
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


def quoted_uses(text: str, name: str, quotes: str = "\"'`"):
    """Line numbers where a quoted string starts with `name` as a command:
    the name is the whole string or is followed by a space or '='."""
    pattern = re.compile("[" + quotes + r"]\s*" + re.escape(name) + "(?=[" + quotes + "]|[ =])")
    for match in pattern.finditer(text):
        yield text.count("\n", 0, match.start()) + 1


def removed_name_hits(repo: Path):
    """(file relative to the repo, removed name, line) for each string that sends a removed name."""
    seen = set()
    for folder, suffixes in REMOVED_NAME_SCAN:
        base = repo / folder
        if not base.is_dir():
            continue
        for path in sorted(base.rglob("*")):
            relative = path.relative_to(repo).as_posix()
            if (path.suffix not in suffixes or not path.is_file() or relative in seen
                    or any(part in "/" + relative for part in SKIPPED_SCAN_PARTS)
                    or relative == "tools/command_registry.py"):
                continue
            seen.add(relative)
            text = path.read_text(encoding="utf-8", errors="replace")
            for name in REMOVED_NAMES:
                if path.suffix in (".cpp", ".h", ".inc"):
                    # C++ strings without comments, and the page scripts' JS
                    # strings ('...' and `...`) inside the raw page literals.
                    lines = set(quoted_uses(blank_comments(text, keep_strings=True), name, '"'))
                    lines |= set(quoted_uses(text, name, "'`"))
                else:
                    lines = set(quoted_uses(text, name))
                for line in sorted(lines):
                    yield relative, name, line


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
        fixed = re.sub(r"[^a-z0-9 ]", "", name.lower())
        failures.append(f"command name \"{name}\" is not lower-case letters and digits, expected \"{fixed}\"\n"
                        f"    {relative}:{line}")
    for old, new in sorted(REMOVED_NAMES.items()):
        for name, _, relative, line in commands:
            if name.lower() == old:
                failures.append(f"\"{old}\" was removed (one name per command); the command is "
                                f"\"{new}\"\n    {relative}:{line}")
        # The kept name is a row, or a sub-command of a dispatcher row (`automation add`).
        if new not in names and new.split()[0] not in names:
            failures.append(f"REMOVED_NAMES maps \"{old}\" to \"{new}\", which is not a command")
        if not FORM.fullmatch(new):
            failures.append(f"REMOVED_NAMES maps \"{old}\" to \"{new}\", which does not have the form")
    repo = component.parents[1]
    used = set()
    for relative, old, line in removed_name_hits(repo):
        if (relative, old) in REMOVED_NAME_USES:
            used.add((relative, old))
            continue
        failures.append(f"a string still sends the removed name \"{old}\"; use \"{REMOVED_NAMES[old]}\"\n"
                        f"    {relative}:{line}")
    registry = repo / "tools" / "command_registry.py"
    if registry.is_file():
        retired = None
        for node in ast.parse(registry.read_text(encoding="utf-8")).body:
            if (isinstance(node, ast.Assign) and len(node.targets) == 1
                    and getattr(node.targets[0], "id", None) == "RETIRED_NAMES"):
                retired = ast.literal_eval(node.value)
        if retired != REMOVED_NAMES:
            failures.append("RETIRED_NAMES in tools/command_registry.py differs from REMOVED_NAMES here")
    if (repo / "tools").is_dir():
        for relative, old in sorted(set(REMOVED_NAME_USES) - used):
            failures.append(f"REMOVED_NAME_USES lists \"{old}\" in {relative}, which no longer uses it; "
                            f"remove the entry")
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
          f"({spaced} with sub-command words, {len(REMOVED_NAMES)} removed names guarded, "
          f"{len(KEPT_NAMES)} kept), {len(keys)} setting cmdKeys")


if __name__ == "__main__":
    main()
