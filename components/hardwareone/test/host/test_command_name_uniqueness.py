#!/usr/bin/env python3
"""Command names are unique across the real registered CommandEntry tables.

Lookup keeps the first registered row of a name (ASCII case ignored), so a
second row with the same name is dead code that `help` still lists. Part one
scans the source: every CommandEntry table named in gCommandModules
(System_CommandRegistry.cpp), every row in it, with the preprocessor branch
each row sits in. Two rows with one name are a defect unless no build can
link both: they sit in different branches of one #if group in one file, or in
two definitions of the same table (a program links exactly one definition of
a table symbol: the OTA table's layout and no-layout variants, a disabled
module's stub in System_SensorStubs.cpp). The scan does not evaluate macros
otherwise, so rows of different tables count as compiled together;
DEFERRED_DUPLICATES lists any name left to another session, with its reason. Part two compiles the production registration from
System_Command.cpp against fixture modules and checks that
initializeCommandSystem logs a CMDREG line for each shadowed row.
"""
from __future__ import annotations

import argparse
import collections
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

# Duplicate names allowed to remain until another session removes them:
# name -> reason. Empty: every known duplicate is fixed.
DEFERRED_DUPLICATES: dict[str, str] = {}

TABLE_RE = re.compile(r"CommandEntry\s+(\w+)\s*\[\s*\]\s*=\s*\{")
MODULES_RE = re.compile(r"CommandModule\s+gCommandModules\s*\[\s*\]\s*=\s*\{")
MODULE_TABLE_RE = re.compile(r"\b(\w+)\s*,\s*&\1Count\b")
DIRECTIVE_RE = re.compile(r"^\s*#\s*(\w+)")


def skip_literal(text: str, pos: int) -> int:
    """Index after the string or character literal starting at pos."""
    quote = text[pos]
    pos += 1
    while pos < len(text) and text[pos] != quote:
        pos += 2 if text[pos] == "\\" else 1
    return pos + 1


def matching_brace(text: str, opening: int) -> int:
    """Index of the brace closing text[opening], skipping comments and literals."""
    depth, pos = 0, opening
    while pos < len(text):
        char, following = text[pos], text[pos + 1:pos + 2]
        if char == "/" and following == "/":
            pos = text.find("\n", pos)
            if pos < 0:
                break
            continue
        if char == "/" and following == "*":
            pos = text.index("*/", pos) + 2
            continue
        if char in "\"'":
            pos = skip_literal(text, pos)
            continue
        if char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return pos
        pos += 1
    raise SystemExit(f"unbalanced braces from offset {opening}")


def row_names(text: str, opening: int, closing: int) -> list[tuple[int, str]]:
    """(offset, name) of each row `{ "name", ...}` directly inside a table body."""
    rows, depth, pos = [], 0, opening + 1
    while pos < closing:
        char, following = text[pos], text[pos + 1:pos + 2]
        if char == "/" and following == "/":
            pos = text.find("\n", pos)
            continue
        if char == "/" and following == "*":
            pos = text.index("*/", pos) + 2
            continue
        if char in "\"'":
            pos = skip_literal(text, pos)
            continue
        if char == "{":
            if depth == 0:
                match = re.match(r'\{\s*"((?:[^"\\]|\\.)*)"', text[pos:])
                require(match is not None, f"row without a literal name at offset {pos}")
                rows.append((pos, match.group(1)))
            depth += 1
        elif char == "}":
            depth -= 1
        pos += 1
    return rows


def branch_stacks(text: str) -> list[tuple[tuple[int, int], ...]]:
    """Per line: the (group, branch) pairs of the enclosing #if groups."""
    stacks, stack, groups = [], [], 0
    for line in text.split("\n"):
        directive = DIRECTIVE_RE.match(line)
        word = directive.group(1) if directive else ""
        if word in ("if", "ifdef", "ifndef"):
            groups += 1
            stack.append([groups, 0])
        elif word in ("elif", "else") and stack:
            stack[-1][1] += 1
        elif word == "endif" and stack:
            stack.pop()
        stacks.append(tuple((group, branch) for group, branch in stack))
    return stacks


def exclusive(left: tuple, right: tuple) -> bool:
    """True when no build links both rows (see the module docstring)."""
    if left[2] == right[2] and left[5] != right[5]:
        return True  # two definitions of one table symbol
    if left[0] != right[0]:
        return False
    branches = dict(left[1])
    return any(group in branches and branches[group] != branch for group, branch in right[1])


def registered_tables() -> set[str]:
    source = (COMPONENT / "System_CommandRegistry.cpp").read_text()
    match = MODULES_RE.search(source)
    require(match is not None, "gCommandModules in System_CommandRegistry.cpp")
    opening = match.end() - 1
    body = source[opening:matching_brace(source, opening)]
    tables = set(MODULE_TABLE_RE.findall(body))
    require(len(tables) >= 30, f"gCommandModules names its tables ({len(tables)} found)")
    return tables


def scan() -> tuple[dict[str, list[tuple]], int]:
    tables = registered_tables()
    rows: dict[str, list[tuple]] = collections.defaultdict(list)
    defined, total = set(), 0
    for path in sorted(COMPONENT.glob("*.cpp")) + sorted(COMPONENT.glob("*.h")):
        text = path.read_text(encoding="utf-8", errors="replace")
        if "CommandEntry" not in text:
            continue
        stacks = None
        for match in TABLE_RE.finditer(text):
            table = match.group(1)
            if table not in tables:
                continue
            defined.add(table)
            stacks = stacks or branch_stacks(text)
            opening = match.end() - 1
            for offset, name in row_names(text, opening, matching_brace(text, opening)):
                line = text.count("\n", 0, offset)
                rows[name.lower()].append((path.name, stacks[line], table, line + 1, name,
                                           (path.name, match.start())))
                total += 1
    require(tables <= defined, f"tables with no definition: {sorted(tables - defined)}")
    require(total >= 300, f"the scan found the command rows ({total} found)")
    return rows, total


def source_check() -> None:
    rows, total = scan()
    problems = []
    for name, found in sorted(rows.items()):
        for i in range(len(found)):
            for j in range(i + 1, len(found)):
                if exclusive(found[i], found[j]) or name in DEFERRED_DUPLICATES:
                    continue
                a, b = found[i], found[j]
                problems.append(f"  {a[4]}: {a[2]} ({a[0]}:{a[3]}) and {b[2]} ({b[0]}:{b[3]})")
    for name in DEFERRED_DUPLICATES:
        require(len(rows.get(name, [])) > 1, f"deferred duplicate {name} is gone; drop it from the list")
    require(not problems, "command names registered twice (the later row is dead):\n" + "\n".join(problems))
    print(f"command name scan: {total} rows in {len(rows)} names, no duplicates")


def generate() -> str:
    source = (COMPONENT / "System_Command.cpp").read_text()
    header = (COMPONENT / "System_Command.h").read_text()
    max_commands = re.findall(r"^#define MAX_COMMANDS \d+$", header, re.M)
    require(len(max_commands) == 1, "one MAX_COMMANDS")
    declarations = max_commands[0] + "\n" + extract_block(header, "struct ModuleInfo") + ";\n"
    start_marker = "EXT_RAM_BSS_ATTR static const CommandEntry* commandRegistry[MAX_COMMANDS];"
    lookup_banner = "// Command Lookup Functions"
    init_banner = "// System Initialization"
    for marker in (start_marker, lookup_banner, init_banner, "void initializeCommandSystem()"):
        require(source.count(marker) == 1, f"marker is not unique: {marker}")
    storage = source[source.index(start_marker):source.index(lookup_banner)]
    init_start = source.index(init_banner)
    init_end = source.index("void initializeCommandSystem()")
    initialization = source[init_start:init_end] + extract_block(source[init_end:], "void initializeCommandSystem()")
    harness = (HERE / "command_name_uniqueness_harness.cpp").read_text()
    for marker, replacement in (("// INSERT_COMMAND_HEADER", declarations),
                                ("// INSERT_REGISTRATION", storage + "\n" + initialization)):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", help="run one harness case by name")
    parser.add_argument("--skip-scan", action="store_true", help="run only the registration harness")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-command-names-") as temporary:
        directory = Path(temporary)
        unit = directory / "command_name_uniqueness.cpp"
        unit.write_text(generate())
        binary = directory / "command_name_uniqueness"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-parameter", "-Wno-unused-function",
                   "-I", str(COMPONENT), str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=180)
        subprocess.run([str(binary)] + ([args.case] if args.case else []), check=True, timeout=60)
    if not args.skip_scan:
        source_check()


if __name__ == "__main__":
    main()
