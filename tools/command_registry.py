#!/usr/bin/env python3
"""Command registry tool — generate the command reference or audit for drift.

Subcommands:
  reference  Read the command registry from C++ source and write
             docs/COMMAND_REFERENCE.md (default if no subcommand given).
  audit      Report registry drift without writing anything:
               - duplicate command names (two registry slots, first wins)
               - CommandEntry arrays never registered in gCommandModules[]
               - modules referencing an array with no definition
               - command strings invoked in code that resolve to nothing
               - SettingEntry cmdKey values that bind to no command
             Exits non-zero if anything but the known-intentional
             voice-alias duplicates is found, so CI can gate on it.

Both commands read components/hardwareone/*.cpp and *.h. Neither writes to
source files; `reference` only writes docs/COMMAND_REFERENCE.md.

Companion to settings_registry.py, which does the same for SettingEntry.

Usage:
    python3 tools/command_registry.py              # same as `reference`
    python3 tools/command_registry.py reference
    python3 tools/command_registry.py audit
"""

from __future__ import annotations
import argparse
import collections
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
COMPONENTS = REPO / "components" / "hardwareone"
OUT_REFERENCE = REPO / "docs" / "COMMAND_REFERENCE.md"

# Disabled-build stubs: empty arrays that mirror real names. Excluded so they
# don't read as duplicate registrations of the real thing.
STUB_FILE = "System_SensorStubs.cpp"

# Same command name registered twice on purpose, to give it a second voice
# phrase (CommandEntry has no alias list). Not drift.
INTENTIONAL_DUPLICATES = {"voicecancel"}


# ============================================================================
# Shared parsing primitives
# ============================================================================

def sources() -> list[Path]:
    return sorted(COMPONENTS.glob("*.cpp")) + sorted(COMPONENTS.glob("*.h"))


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def match_brace(s: str, i: int) -> int:
    """Index of the `}` closing the `{` at s[i], skipping strings and // comments."""
    depth = 0
    j = i
    while j < len(s):
        c = s[j]
        if c == '"':
            j += 1
            while j < len(s) and s[j] != '"':
                if s[j] == "\\":
                    j += 1
                j += 1
        elif c == "/" and j + 1 < len(s) and s[j + 1] == "/":
            while j < len(s) and s[j] != "\n":
                j += 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return j
        j += 1
    return len(s) - 1


def split_fields(item: str) -> list[str]:
    """Split one `{...}` initializer into top-level comma-separated fields."""
    out, cur, depth, i = [], "", 0, 1
    while i < len(item) - 1:
        c = item[i]
        if c == '"':
            cur += c
            i += 1
            while i < len(item) and item[i] != '"':
                if item[i] == "\\":
                    cur += item[i]
                    i += 1
                cur += item[i]
                i += 1
            cur += '"'
        elif c in "({[":
            depth += 1
            cur += c
        elif c in ")}]":
            depth -= 1
            cur += c
        elif c == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
        i += 1
    out.append(cur.strip())
    return out


STRLIT_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')

# C++ escapes that appear in help/usage text. Everything is flattened to a
# single line — these strings become one table cell.
_UNESCAPE = {r"\n": " ", r"\t": " ", r'\"': '"', r"\\": "\\"}


def cstr(field: str) -> str:
    """Concatenate adjacent C string literals into one plain string.

    Help and usage text is routinely written as several literals across
    lines (`"Usage: foo\\n"  "  bar"`). Taking only the first literal
    truncates the text; taking the raw field leaks stray quotes into the
    generated table.
    """
    parts = STRLIT_RE.findall(field)
    if not parts:
        return ""
    out = "".join(parts)
    for esc, repl in _UNESCAPE.items():
        out = out.replace(esc, repl)
    return " ".join(out.split())


def iter_initializers(body: str):
    """Yield each top-level `{...}` initializer inside an array body."""
    k = 0
    while k < len(body):
        if body[k] == "{" and k > 0:
            end = match_brace(body, k)
            yield body[k:end + 1]
            k = end + 1
            continue
        k += 1


# ============================================================================
# Registry extraction
# ============================================================================

ARRAY_RE = re.compile(r"CommandEntry\s+(\w+)\s*\[\]\s*=\s*\{")
NAME_RE = re.compile(r'\{\s*"([A-Za-z0-9_\-]+)"\s*,')
SUPER_RE = re.compile(r"requiresSuperAdmin\s*=\s*\*/\s*true")


def parse_commands(include_stubs: bool = False) -> dict[str, list[dict]]:
    """array name -> list of command dicts."""
    by_array: dict[str, list[dict]] = collections.defaultdict(list)
    for path in sources():
        if not include_stubs and path.name == STUB_FILE:
            continue
        src = read(path)
        for m in ARRAY_RE.finditer(src):
            array = m.group(1)
            start = m.end() - 1
            body = src[start:match_brace(src, start)]
            for item in iter_initializers(body):
                nm = NAME_RE.match(item)
                if not nm:
                    continue
                f = split_fields(item)
                help_ = cstr(f[1]) if len(f) > 1 and f[1].startswith('"') else ""
                usage = next((cstr(x) for x in f[4:] if x.startswith('"Usage:')), "")
                by_array[array].append(dict(
                    name=nm.group(1),
                    help=help_,
                    admin=(len(f) > 2 and f[2].strip() == "true"),
                    superadmin=bool(SUPER_RE.search(item)),
                    usage=usage,
                    file=path.name,
                    array=array,
                ))
    return by_array


MODULES_RE = re.compile(r"CommandModule\s+gCommandModules\s*\[\]\s*=\s*\{")
MOD_NAME_RE = re.compile(r'\{\s*"([A-Za-z0-9_\-]+)"')
MOD_ARRAY_RE = re.compile(r",\s*(\w+Commands|commands)\s*,")


def parse_modules() -> list[dict]:
    """gCommandModules[] in registration order — this is also help-page order."""
    for path in sources():
        src = read(path)
        m = MODULES_RE.search(src)
        if not m:
            continue
        start = m.end() - 1
        body = src[start:match_brace(src, start)]
        mods = []
        for item in iter_initializers(body):
            nm = MOD_NAME_RE.match(item)
            if not nm:
                continue
            fields = split_fields(item)
            arr = MOD_ARRAY_RE.findall(item)
            mods.append(dict(
                module=nm.group(1),
                summary=cstr(fields[2]) if len(fields) > 2 else "",
                array=arr[0] if arr else None,
            ))
        return mods
    return []


# ============================================================================
# reference
# ============================================================================

def cmd_reference(args) -> int:
    by_array = parse_commands()
    modules = parse_modules()
    total = sum(len(v) for v in by_array.values())
    unique = {c["name"] for v in by_array.values() for c in v}

    out: list[str] = []
    add = out.append
    add("# Command Reference")
    add("")
    add("> **Generated file — do not edit by hand.**")
    add("> Regenerate with `python3 tools/command_registry.py reference`.")
    add("> Source of truth is the `CommandEntry` tables in "
        "`components/hardwareone/*.cpp`.")
    add("")
    add(f"{len(unique)} commands across {len(modules)} modules "
        f"({total} registry entries).")
    add("")
    add("Commands are matched case-insensitively, and lookup uses "
        "longest-prefix matching, so `automation list` resolves to the "
        "`automation` dispatcher with `list` as its argument.")
    add("")
    add("Legend: **A** = requires admin &nbsp; **S** = requires super admin")
    add("")
    add("## Modules")
    add("")
    for m in modules:
        n = len(by_array.get(m["array"], []))
        add(f"- [`{m['module']}`](#{m['module'].replace('_', '-')}) — {n} commands")
    add("")
    add("---")
    add("")

    seen_arrays = set()
    for m in modules:
        rows = by_array.get(m["array"], [])
        seen_arrays.add(m["array"])
        add(f"## {m['module']}")
        add("")
        if m["summary"]:
            # The module blurb is one long C++ string; wrap it as a blockquote.
            add(f"> {m['summary'].strip()}")
            add("")
        if not rows:
            add("_No commands compiled in for this build configuration._")
            add("")
            continue
        add("| Command | | Description |")
        add("| ------- | :-: | ----------- |")
        for c in sorted(rows, key=lambda r: r["name"].lower()):
            flag = "S" if c["superadmin"] else ("A" if c["admin"] else "")
            desc = c["help"].replace("|", "\\|").strip()
            if c["usage"]:
                u = c["usage"].replace("|", "\\|").strip()
                desc = f"{desc}<br/>`{u}`"
            add(f"| `{c['name']}` | {flag} | {desc} |")
        add("")

    orphans = {a: v for a, v in by_array.items() if a not in seen_arrays}
    if orphans:
        add("---")
        add("")
        add("## Unregistered arrays")
        add("")
        add("`CommandEntry` tables found in source but not listed in "
            "`gCommandModules[]` — these do not reach the registry:")
        add("")
        for a, v in sorted(orphans.items()):
            add(f"- `{a}` ({len(v)} entries, {v[0]['file']})")
        add("")

    OUT_REFERENCE.parent.mkdir(parents=True, exist_ok=True)
    OUT_REFERENCE.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"wrote {OUT_REFERENCE.relative_to(REPO)} "
          f"({len(unique)} commands, {len(modules)} modules)")
    return 0


# ============================================================================
# audit
# ============================================================================

CALL_RE = re.compile(
    r'\b(executeCommand|executeOLEDCommand|executeCommandForUser'
    r'|submitAndExecuteSync|enqueueCommand|runCommand)\s*\(\s*"([^"]{2,60})"'
)
SETTING_ARRAY_RE = re.compile(r"SettingEntry\s+\w+\s*\[\]\s*=\s*\{")


def cmd_audit(args) -> int:
    by_array = parse_commands()
    modules = parse_modules()
    names = {c["name"].lower() for v in by_array.values() for c in v}
    module_names = {m["module"].lower() for m in modules}
    problems = 0

    # -- duplicate registrations ------------------------------------------
    where = collections.defaultdict(list)
    for v in by_array.values():
        for c in v:
            where[c["name"]].append(c)
    dupes = {n: l for n, l in where.items() if len(l) > 1}
    real = {n: l for n, l in dupes.items() if n not in INTENTIONAL_DUPLICATES}
    print(f"== duplicate command names ({len(real)} unexpected, "
          f"{len(dupes) - len(real)} intentional) ==")
    for n, l in sorted(real.items()):
        problems += 1
        print(f"  {n}")
        for c in l:
            print(f"      {c['array']} ({c['file']})  admin={c['admin']} "
                  f"super={c['superadmin']}")
        print("      first registered wins; the other slot is dead")
    if not real:
        print("  (none)")

    # -- array/module wiring ----------------------------------------------
    registered = {m["array"] for m in modules if m["array"]}
    orphan = sorted(set(by_array) - registered)
    print(f"\n== arrays never registered in gCommandModules[] ==")
    for a in orphan:
        problems += 1
        print(f"  {a} ({len(by_array[a])} entries, {by_array[a][0]['file']})")
    if not orphan:
        print("  (none)")

    dangling_mod = [m for m in modules if m["array"] and m["array"] not in by_array]
    print(f"\n== modules referencing a missing array ==")
    for m in dangling_mod:
        problems += 1
        print(f"  {m['module']} -> {m['array']}")
    if not dangling_mod:
        print("  (none)")

    # -- command strings invoked from code --------------------------------
    print(f"\n== command strings in code that resolve to nothing ==")
    dangling = collections.defaultdict(list)
    for path in sources():
        for lineno, line in enumerate(read(path).splitlines(), 1):
            for m in CALL_RE.finditer(line):
                text = m.group(2).strip()
                if not text:
                    continue
                low = text.lower()
                first = low.split()[0]
                if any(low == n or low.startswith(n + " ") for n in names):
                    continue
                if first in module_names:
                    continue
                dangling[first].append(f"{path.name}:{lineno}  {m.group(1)}(\"{text}\")")
    for k in sorted(dangling):
        problems += 1
        print(f"  {k}")
        for h in dangling[k]:
            print(f"      {h}")
    if not dangling:
        print("  (none)")

    # -- settings cmdKey bindings -----------------------------------------
    print(f"\n== SettingEntry cmdKey values binding to no command ==")
    bad = collections.defaultdict(list)
    checked = 0
    for path in sources():
        src = read(path)
        for m in SETTING_ARRAY_RE.finditer(src):
            start = m.end() - 1
            body = src[start:match_brace(src, start)]
            for item in iter_initializers(body):
                f = split_fields(item)
                if len(f) < 13 or not f[0].startswith('"'):
                    continue
                key = f[0].strip('"')
                raw = f[12].strip()
                if not raw or raw == "nullptr":
                    continue
                checked += 1
                val = raw.strip('"')
                base = val.split()[0].lower() if val.split() else ""
                if val.lower() in names or base in names:
                    continue
                bad[val].append(f"{path.name} :: {key}")
    for k in sorted(bad):
        problems += 1
        print(f'  cmdKey "{k}" <- {bad[k]}')
    if not bad:
        print(f"  (none — all {checked} cmdKey bindings resolve)")

    print(f"\n{'PROBLEMS: %d' % problems if problems else 'clean'}")
    return 1 if problems else 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command")
    sub.add_parser("reference", help="write docs/COMMAND_REFERENCE.md")
    sub.add_parser("audit", help="report registry drift; non-zero exit on findings")
    args = p.parse_args()
    if args.command == "audit":
        return cmd_audit(args)
    return cmd_reference(args)


if __name__ == "__main__":
    sys.exit(main())
