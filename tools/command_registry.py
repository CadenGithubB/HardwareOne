#!/usr/bin/env python3
"""Command registry tool — generate the command reference or audit for drift.

Subcommands:
  reference  Read the command registry from C++ source and write
             docs/COMMAND_REFERENCE.md (default if no subcommand given).
             With --check, write nothing: render the reference, compare it
             with the tracked docs/COMMAND_REFERENCE.md and exit 1 with a
             diff summary if they differ.
  audit      Report registry drift without writing anything:
               - duplicate command names (two registry slots, first wins)
               - one handler registered under two names (an alias row)
               - a removed command name (RETIRED_NAMES) registered again
               - CommandEntry arrays never registered in gCommandModules[]
               - modules referencing an array with no definition
               - command strings invoked in code that resolve to nothing
               - SettingEntry cmdKey values that bind to no command
               - a 'Usage: <command>' line or a "run/use/type '<command>'"
                 hint that names no registered command (words that are
                 not commands on purpose: USAGE_NON_COMMAND_WORDS)
               - a non-admin command that saves an *autostart/*enabled
                 setting (CONTRIBUTING.md, "Command names"): by its row or a
                 setting's cmdKey, or by a write to the setting's gSettings
                 field in its handler or a function the handler calls
             Exits non-zero on any finding, so a check can gate on it.

A CommandEntry array defined more than once in one file, in mutually
exclusive branches of the same #if/#elif/#else chain (System_OTA.cpp's
otaCommands[] under `#if HW1_OTA_LAYOUT` / `#else`), is one table: only one
branch is ever compiled. The first branch is read, as for gCommandModules[];
the alternatives are neither counted nor reported as duplicates, but
`audit` fails if an alternative has a command name the first branch lacks,
and `reference` notes under the module that the table has alternatives.

Both commands read components/hardwareone/*.cpp and *.h. Neither writes to
source files; `reference` only writes docs/COMMAND_REFERENCE.md.

Companion to settings_registry.py, which does the same for SettingEntry.

Usage:
    python3 tools/command_registry.py              # same as `reference`
    python3 tools/command_registry.py reference
    python3 tools/command_registry.py reference --check
    python3 tools/command_registry.py audit
"""

from __future__ import annotations
import argparse
import collections
import difflib
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
COMPONENTS = REPO / "components" / "hardwareone"
OUT_REFERENCE = REPO / "docs" / "COMMAND_REFERENCE.md"

# One name per command (CONTRIBUTING.md, "Command names"). Removed name ->
# the name that stays. The host test command_name_form holds the same table
# (REMOVED_NAMES) and checks that the two agree and that no string sends a
# removed name; this audit fails when one is a registry row again.
RETIRED_NAMES = {
    "apdsenabled": "apdsautostart",
    "automationadd": "automation add",
    "automationrun": "automation run",
    "automationtrigger": "automation trigger",
    "bleenabled": "bleautostart",
    "bleread": "blestatus",
    "c6_console": "c6console",
    "cameraenabled": "cameraautostart",
    "debugespnow": "debugespnowcore",
    "eienable": "eiautostart",
    "espnowenabled": "espnowautostart",
    "espnowpair": "espnowpairsecure",
    "espnowread": "espnowstatus",
    "fmradioenabled": "fmradioautostart",
    "g2ai-direct": "g2aidirect",
    "g2ai-noask": "g2ainoask",
    "gpsenabled": "gpsautostart",
    "httpenabled": "httpautostart",
    "httpread": "httpstatus",
    "imuenabled": "imuautostart",
    "inputenabled": "inputautostart",
    "llmenabled": "llmautostart",
    "micenabled": "micautostart",
    "mqttclientenabled": "mqttautostart",
    "oledenabled": "oledautostart",
    "oledread": "oledstatus",
    "oledstart": "openoled",
    "oledstop": "closeoled",
    "presenceenabled": "presenceautostart",
    "ring panel off": "ring panel feed",
    "ring panel on": "ring panel feed",
    "rtcenabled": "rtcautostart",
    "sensorlogenabled": "sensorlog autostart",
    "sleep": "wait",
    "srenabled": "srautostart",
    "srstart": "opensr",
    "srstop": "closesr",
    "systemlogenabled": "log autostart",
    "thermalenabled": "thermalautostart",
    "tofenabled": "tofautostart",
    "validate-conditions": "validateconditions",
    "wifienabled": "wifiautostart",
    "wifiread": "wifistatus",
}

# A reply's usage line ("Usage: <command> ...") and its hints ("run
# '<command>'", "use '<command>'", ...) name a registered command. These words
# appear there on purpose and are not registry commands; each says why.
USAGE_NON_COMMAND_WORDS = {
    "dictate": "the UART link's in-band dictation protocol (System_Dictation.cpp), "
               "answered before the registry",
    "yes": "the answer to a confirmation prompt (System_CLIConfirm.cpp)",
    "cancel": "leaves an interactive prompt (setup wizard, features)",
    "n": "the 'no' answer of an interactive prompt",
    "p": "help paging: p<N> (System_CLI.cpp)",
    "rescan": "the first-time setup Wi-Fi prompt's retry word",
    "color": "an apdsmode argument (apdsmode color|proximity|gesture)",
}
USAGE_LINE_RE = re.compile(r"Usage:\s+([A-Za-z][^\"\\|<\[\n]*)")
USAGE_HINT_RE = re.compile(r"\b(?:[Rr]un|[Uu]se|[Tt]ry|[Tt]ype|[Ss]end|[Ss]ee|with) '([a-z][a-z0-9_]*)")


# Handlers that serve several rows on purpose: stubs that answer "not built"
# or "unavailable" for a whole family. Not aliases.
SHARED_STUB_HANDLERS = {"phoneNotBuilt", "unavailable"}

# A non-admin command that saves an *autostart/*enabled setting, left open
# for a stated reason. Every entry is an open owner question; the audit fails
# when an entry no longer saves a switch, so the list cannot go stale.
OPEN_PERSISTENT_SWITCHES = {
    "healthlogging": "the row stays open for its status verb; every other verb "
                     "refuses an account below admin in the handler (logging is "
                     "admin-only, owner decision 2026-11-21)",
    "batterylog": "the row stays open for its status and tail verbs; on, off, "
                  "interval and clear refuse an account below admin in the "
                  "handler (logging is admin-only, owner decision 2026-11-21)",
}


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

# A path constant such as HW1_USERS_JSON_PATH is spliced into help text by
# literal concatenation ("Deletes " HW1_USERS_JSON_PATH ", ..."). The names
# below are the object-like macros whose whole value is one string literal;
# a name defined with two different values is left alone.
_STRING_MACRO_RE = re.compile(
    r'^[ \t]*#[ \t]*define[ \t]+([A-Za-z_]\w*)[ \t]+("(?:[^"\\]|\\.)*")[ \t]*(?://.*)?$', re.M)
_TOKEN_RE = re.compile(r'"(?:[^"\\]|\\.)*"|[A-Za-z_]\w*')
_string_macros = None


def string_macros() -> dict[str, str]:
    global _string_macros
    if _string_macros is None:
        values = collections.defaultdict(set)
        for path in sources():
            for name, literal in _STRING_MACRO_RE.findall(read(path)):
                values[name].add(literal)
        _string_macros = {name: next(iter(found)) for name, found in values.items() if len(found) == 1}
    return _string_macros


def _expand_string_macro(match) -> str:
    token = match.group(0)
    return token if token.startswith('"') else string_macros().get(token, token)



def cstr(field: str) -> str:
    """Concatenate adjacent C string literals into one plain string.

    Help and usage text is routinely written as several literals across
    lines (`"Usage: foo\\n"  "  bar"`). Taking only the first literal
    truncates the text; taking the raw field leaks stray quotes into the
    generated table.
    """
    parts = STRLIT_RE.findall(_TOKEN_RE.sub(_expand_string_macro, field))
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
# Command keys may contain spaces; the firmware's longest-prefix lookup uses
# those rows for subcommands such as ``cm5 status``. Keep the generated
# reference aligned with the actual registry instead of silently listing only
# the single-token root row.
NAME_RE = re.compile(
    r'\{\s*"([A-Za-z0-9_\-]+(?: [A-Za-z0-9_\-]+)*)"\s*,')
SUPER_RE = re.compile(r"requiresSuperAdmin\s*=\s*\*/\s*true")


DIRECTIVE_RE = re.compile(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b")


def directive(line: str) -> str:
    """'if', 'elif', 'else' or 'endif' for a conditional directive line
    (any spacing after '#'; #ifdef/#ifndef count as 'if'), else ''."""
    m = DIRECTIVE_RE.match(line)
    if not m:
        return ""
    kw = m.group(1)
    return "if" if kw.startswith("if") else kw


def preprocessor_paths(text: str) -> list[tuple]:
    """Per line, the #if nesting it sits in: a tuple of (chain id, branch index).

    Each #if/#ifdef/#ifndef opens a new chain with a unique id; #elif and #else
    move to the chain's next branch; #endif closes it. Conditions are not
    evaluated — only the shape matters here.
    """
    paths, stack, next_id = [], [], 0
    for line in text.splitlines():
        kw = directive(line)
        if kw == "if":
            stack.append((next_id, 0))
            next_id += 1
        elif kw in ("else", "elif") and stack:
            chain, branch = stack[-1]
            stack[-1] = (chain, branch + 1)
        elif kw == "endif" and stack:
            stack.pop()
        paths.append(tuple(stack))
    return paths


def mutually_exclusive(a: tuple, b: tuple) -> bool:
    """True if two #if paths can never be compiled together: at the first
    level where they differ, both sit in the same chain on different branches."""
    for x, y in zip(a, b):
        if x == y:
            continue
        return x[0] == y[0] and x[1] != y[1]
    return False


def parse_commands_and_alternates():
    """(array name -> command dicts, alternative-branch definitions skipped).

    The second value lists (file, array, line, row names) for each later
    definition of an array that is mutually exclusive with one already read
    from the same file. Its rows are not added to the first value.
    """
    by_array: dict[str, list[dict]] = collections.defaultdict(list)
    alternates: list[tuple[str, str, int, list[str]]] = []
    for path in sources():
        src = read(path)
        paths = None
        kept: dict[str, list[tuple]] = collections.defaultdict(list)
        for m in ARRAY_RE.finditer(src):
            array = m.group(1)
            if paths is None:
                paths = preprocessor_paths(src)
            line = src.count("\n", 0, m.start())
            here = paths[line] if line < len(paths) else ()
            start = m.end() - 1
            body = src[start:match_brace(src, start)]
            if any(mutually_exclusive(here, p) for p in kept[array]):
                alt_names = [nm.group(1) for nm in
                             (NAME_RE.match(i) for i in iter_initializers(body)) if nm]
                alternates.append((path.name, array, line + 1, alt_names))
                continue
            kept[array].append(here)
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
                    handler=(f[3].strip().lstrip("&") if len(f) > 3 else ""),
                    admin=(len(f) > 2 and f[2].strip() == "true"),
                    superadmin=bool(SUPER_RE.search(item)),
                    usage=usage,
                    file=path.name,
                    array=array,
                ))
    return by_array, alternates


def parse_commands() -> dict[str, list[dict]]:
    """array name -> list of command dicts (one branch of each #if/#else table)."""
    return parse_commands_and_alternates()[0]


MODULES_RE = re.compile(r"CommandModule\s+gCommandModules\s*\[\]\s*=\s*\{")
MOD_NAME_RE = re.compile(r'\{\s*"([A-Za-z0-9_\-]+)"')
MOD_ARRAY_RE = re.compile(r",\s*(\w+Commands|commands)\s*,")


def first_branches(text: str) -> str:
    """Drop each #else/#elif alternative, keeping every #if's first branch.

    The reference describes builds that have a feature; a module's
    "not in this build" alternative row would otherwise read as a duplicate.
    """
    out, skipping = [], []
    for line in text.splitlines(keepends=True):
        kw = directive(line)
        if kw == "if":
            skipping.append(False)
        elif kw in ("else", "elif") and skipping:
            skipping[-1] = True
        elif kw == "endif" and skipping:
            skipping.pop()
        elif not any(skipping):
            out.append(line)
    return "".join(out)


def parse_modules() -> list[dict]:
    """gCommandModules[] in registration order — this is also help-page order."""
    for path in sources():
        src = read(path)
        m = MODULES_RE.search(src)
        if not m:
            continue
        start = m.end() - 1
        body = first_branches(src[start:match_brace(src, start)])
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

def render_reference() -> tuple[str, int, int]:
    """(reference text, unique command count, module count)."""
    by_array, alternates = parse_commands_and_alternates()
    # Only alternatives that register rows: an empty "feature off" stub needs no note.
    alt_arrays = {array: fname for fname, array, _, alt in alternates if alt}
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
        if m["array"] in alt_arrays:
            add(f"_`{m['array']}` has `#if` alternatives in "
                f"`{alt_arrays[m['array']]}`; this table is the first branch. "
                f"Other build configurations register a different version of "
                f"these rows (for example the disabled branch of the feature)._")
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

    return "\n".join(out) + "\n", len(unique), len(modules)


ROW_NAME_RE = re.compile(r"^\| `([^`]+)` \|", re.M)


def check_reference(text: str) -> int:
    """Compare a rendered reference with the tracked file; 0 if identical."""
    rel = OUT_REFERENCE.relative_to(REPO)
    if not OUT_REFERENCE.exists():
        print(f"DRIFT: {rel} does not exist; run "
              f"`python3 tools/command_registry.py reference`")
        return 1
    tracked = OUT_REFERENCE.read_text(encoding="utf-8")
    if tracked == text:
        print(f"{rel} is current")
        return 0
    diff = list(difflib.unified_diff(tracked.splitlines(), text.splitlines(),
                                     fromfile=f"{rel} (tracked)",
                                     tofile=f"{rel} (from source)",
                                     lineterm="", n=0))
    added = sum(1 for d in diff if d.startswith("+") and not d.startswith("+++"))
    removed = sum(1 for d in diff if d.startswith("-") and not d.startswith("---"))
    old_rows = collections.Counter(ROW_NAME_RE.findall(tracked))
    new_rows = collections.Counter(ROW_NAME_RE.findall(text))
    missing = sorted((new_rows - old_rows).elements())
    stale = sorted((old_rows - new_rows).elements())
    print(f"DRIFT: {rel} differs from the registry in source "
          f"({removed} line(s) out of date, {added} line(s) missing)")
    if missing:
        print(f"  command rows missing from the file ({len(missing)}): "
              + ", ".join(missing))
    if stale:
        print(f"  command rows only in the file ({len(stale)}): "
              + ", ".join(stale))
    limit = 40
    for d in diff[:limit]:
        print(f"  {d[:200]}")
    if len(diff) > limit:
        print(f"  ... {len(diff) - limit} more diff line(s)")
    print("Regenerate with `python3 tools/command_registry.py reference` "
          "and commit the result.")
    return 1


def cmd_reference(args) -> int:
    text, unique, modules = render_reference()
    if getattr(args, "check", False):
        return check_reference(text)
    OUT_REFERENCE.parent.mkdir(parents=True, exist_ok=True)
    OUT_REFERENCE.write_text(text, encoding="utf-8")
    print(f"wrote {OUT_REFERENCE.relative_to(REPO)} "
          f"({unique} commands, {modules} modules)")
    return 0


# ============================================================================
# audit
# ============================================================================

CALL_RE = re.compile(
    r'\b(executeCommand|executeOledCommand|executeOledCommandShowResult|executeCommandForUser'
    r'|submitAndExecuteSync|enqueueCommand|runCommand|oledRunCommand'
    r'|oledRunCommandAsync|executeOledCommandWithResult|executeCommandThroughRegistry'
    r'|runUnifiedSystemCommand|executeUnifiedWebCommand|executeInternalStatusCommand'
    r'|healthRunCommand|g2SubmitHijackCommand|g2FmTunerSubmit)\s*\(\s*"([^"]{2,60})"'
)
SETTING_ARRAY_RE = re.compile(r"SettingEntry\s+\w+\s*\[\]\s*=\s*\{")


# A function definition at the start of a line: `const char* cmd_x(...) {`,
# `static bool helper(...) {`. Enough to follow a handler into its helpers.
FUNCTION_DEF_RE = re.compile(
    r"(?m)^(?:static\s+|inline\s+)*(?:const\s+)?[A-Za-z_][\w:<>]*[\s*&]+\**(\w+)"
    r"\s*\(([^;{}]*)\)\s*(?:const\s*)?\{")
SETTING_WRITE_RE = re.compile(
    r"setSetting\s*\(\s*gSettings\.(\w+)|gSettings\.(\w+)\s*=(?!=)")
FUNCTION_CALL_RE = re.compile(r"\b(\w+)\s*\(")
# How many calls deep a handler is followed. Three finds every known write
# (gpslog -> cmd_sensorlog_autostart -> sensorLogAutoStartSet); deeper adds
# only run time.
SWITCH_WRITE_DEPTH = 3


def persistent_switch_findings(by_array) -> list[tuple[str, str]]:
    """(command, reason) for each non-admin row that saves an *autostart or
    *enabled setting: the row a setting's cmdKey resolves to (longest row
    name, as the dispatcher does), a row whose last word ends in autostart
    or enabled, or a row whose handler (or a function it calls, up to
    SWITCH_WRITE_DEPTH deep) writes such a setting's gSettings field. The
    last one also catches an open parent row that still writes the switch of
    an admin sub-command row: a typed line the admin row does not match byte
    for byte (two spaces, a tab, a quoted word) reaches the parent."""
    rows = {}
    for v in by_array.values():
        for c in v:
            rows.setdefault(c["name"].lower(), c)

    def resolve(text: str):
        words = text.lower().split()
        for n in range(len(words), 0, -1):
            row = rows.get(" ".join(words[:n]))
            if row:
                return row
        return None

    found: dict[str, str] = {}
    definitions: dict[str, str] = {}
    switch_fields: dict[str, str] = {}
    for path in sources():
        src = read(path)
        for m in FUNCTION_DEF_RE.finditer(src):
            start = m.end() - 1
            body = src[start:match_brace(src, start) + 1]
            definitions.setdefault(m.group(1), re.sub(r"//[^\n]*", "", body))
        for m in SETTING_ARRAY_RE.finditer(src):
            start = m.end() - 1
            body = src[start:match_brace(src, start)]
            for item in iter_initializers(body):
                f = split_fields(item)
                if len(f) < 3 or not f[0].startswith('"'):
                    continue
                key = f[0].strip('"').lower()
                field = re.fullmatch(r"&\s*gSettings\.(\w+)", f[2].strip())
                if field and (key.endswith("autostart") or key.endswith("enabled")):
                    switch_fields.setdefault(field.group(1), f[0].strip('"'))
                if len(f) < 13:
                    continue
                raw = f[12].strip()
                if raw in ("", "nullptr") or not (key.endswith("autostart") or key.endswith("enabled")):
                    continue
                row = resolve(raw.strip('"'))
                if row and not row["admin"] and not row["superadmin"]:
                    found.setdefault(row["name"].lower(),
                                     f"is the cmdKey of setting {f[0].strip(chr(34))} ({path.name})")
    for name, row in rows.items():
        last = name.split()[-1]
        if (last.endswith("autostart") or last.endswith("enabled")) \
                and not row["admin"] and not row["superadmin"]:
            found.setdefault(name, f"is named like a saved switch ({row['file']})")

    def writes(function: str, depth: int, seen: set) -> dict[str, str]:
        """Switch field -> the function that writes it, from `function` down."""
        if function in seen or function not in definitions:
            return {}
        seen.add(function)
        body = definitions[function]
        out = {}
        for m in SETTING_WRITE_RE.finditer(body):
            field = m.group(1) or m.group(2)
            if field in switch_fields:
                out.setdefault(field, function)
        if depth > 0:
            for m in FUNCTION_CALL_RE.finditer(body):
                for field, writer in writes(m.group(1), depth - 1, seen).items():
                    out.setdefault(field, writer)
        return out

    for name, row in rows.items():
        if row["admin"] or row["superadmin"] or not row["handler"] or name in found:
            continue
        written = writes(row["handler"], SWITCH_WRITE_DEPTH, set())
        if written:
            what = ", ".join(f"{switch_fields[f]} (in {w})" for f, w in sorted(written.items()))
            found[name] = f"its handler {row['handler']} saves {what} ({row['file']})"
    return sorted(found.items())


def usage_hint_findings(names: set[str]) -> dict[str, list[str]]:
    """Words a usage line or a hint names that are neither a registered command
    (multi-word names included) nor in USAGE_NON_COMMAND_WORDS: word -> sites."""
    found = collections.defaultdict(list)
    for path in sources():
        for lineno, line in enumerate(read(path).splitlines(), 1):
            stripped = line.strip()
            if stripped.startswith(("//", "*", "/*")):
                continue
            for m in USAGE_LINE_RE.finditer(line):
                text = " ".join(m.group(1).lower().split())
                first = text.split()[0] if text.split() else ""
                if any(text == n or text.startswith(n + " ") for n in names):
                    continue
                if first in USAGE_NON_COMMAND_WORDS:
                    continue
                found[first].append(f"{path.name}:{lineno}  Usage: {m.group(1).strip()}")
            for m in USAGE_HINT_RE.finditer(line):
                word = m.group(1)
                if word in names or word in USAGE_NON_COMMAND_WORDS:
                    continue
                found[word].append(f"{path.name}:{lineno}  {m.group(0)}'")
    return found


def cmd_audit(args) -> int:
    by_array, alternates = parse_commands_and_alternates()
    modules = parse_modules()
    names = {c["name"].lower() for v in by_array.values() for c in v}
    module_names = {m["module"].lower() for m in modules}
    problems = 0

    # -- #if/#else alternatives (information, not a problem) ---------------
    print(f"== tables in mutually exclusive #if branches "
          f"({len(alternates)} alternative definition(s), first branch read) ==")
    for fname, array, line, _ in alternates:
        print(f"  {array} ({fname}:{line})")
    if not alternates:
        print("  (none)")
    print()

    # -- commands that exist only in an alternative branch ----------------
    # Those rows are neither documented nor duplicate-checked, so flag them.
    read_names = collections.defaultdict(set)
    for a, v in by_array.items():
        read_names[a].update(c["name"].lower() for c in v)
    only_alt = [(fname, array, line, n) for fname, array, line, alt in alternates
                for n in alt if n.lower() not in read_names[array]]
    print(f"== commands only in an alternative #if branch ({len(only_alt)}) ==")
    for fname, array, line, n in only_alt:
        problems += 1
        print(f"  {n}  {array} ({fname}:{line}) — not in the branch the "
              f"reference documents")
    if not only_alt:
        print("  (none)")
    print()

    # -- duplicate registrations ------------------------------------------
    where = collections.defaultdict(list)
    for v in by_array.values():
        for c in v:
            where[c["name"]].append(c)
    dupes = {n: l for n, l in where.items() if len(l) > 1}
    real = dupes
    print(f"== duplicate command names ({len(real)}) ==")
    for n, l in sorted(real.items()):
        problems += 1
        print(f"  {n}")
        for c in l:
            print(f"      {c['array']} ({c['file']})  admin={c['admin']} "
                  f"super={c['superadmin']}")
        print("      first registered wins; the other slot is dead")
    if not real:
        print("  (none)")

    # -- one handler under two names (alias rows) --------------------------
    by_handler = collections.defaultdict(list)
    for v in by_array.values():
        for c in v:
            if c["handler"] and c["handler"] not in SHARED_STUB_HANDLERS:
                by_handler[c["handler"]].append(c)
    aliased = {h: l for h, l in by_handler.items()
               if len({c["name"].lower() for c in l}) > 1}
    print(f"\n== one handler registered under two names ({len(aliased)}) ==")
    for h, l in sorted(aliased.items()):
        problems += 1
        print(f"  {h}: " + ", ".join(f"{c['name']} ({c['file']})" for c in l))
        print("      one name per command (CONTRIBUTING.md, \"Command names\")")
    if not aliased:
        print("  (none)")

    # -- removed names registered again ------------------------------------
    back = sorted(n for n in names if n in RETIRED_NAMES)
    print(f"\n== removed command names registered again ({len(back)}) ==")
    for n in back:
        problems += 1
        print(f"  {n}  (removed; the command is {RETIRED_NAMES[n]})")
    if not back:
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

    # -- usage lines and hints that name no command ------------------------
    print(f"\n== usage lines and hints naming no command ==")
    unknown = usage_hint_findings(names)
    for word in sorted(unknown):
        problems += 1
        print(f"  {word}")
        for h in unknown[word]:
            print(f"      {h}")
    if not unknown:
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

    # -- persistent autostart/enabled switches are admin-only ---------------
    print(f"\n== non-admin commands that save an *autostart/*enabled setting ==")
    open_switches = persistent_switch_findings(by_array)
    for name, why in open_switches:
        if name in OPEN_PERSISTENT_SWITCHES:
            print(f"  {name}  (left open: {OPEN_PERSISTENT_SWITCHES[name]})")
            continue
        problems += 1
        print(f"  {name}  {why}; make the row admin-only (CONTRIBUTING.md, \"Command names\")")
    for name in sorted(set(OPEN_PERSISTENT_SWITCHES) - {n for n, _ in open_switches}):
        problems += 1
        print(f"  {name}  is listed in OPEN_PERSISTENT_SWITCHES but no longer saves a switch "
              f"as a non-admin; remove the entry")
    if not open_switches:
        print("  (none)")
    elif all(n in OPEN_PERSISTENT_SWITCHES for n, _ in open_switches):
        print("  (none outside the open owner questions above)")

    print(f"\n{'PROBLEMS: %d' % problems if problems else 'clean'}")
    return 1 if problems else 0


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="command")
    ref = sub.add_parser("reference", help="write docs/COMMAND_REFERENCE.md")
    ref.add_argument("--check", action="store_true",
                     help="write nothing; exit 1 if the tracked file is out of date")
    sub.add_parser("audit", help="report registry drift; non-zero exit on findings")
    args = p.parse_args()
    if args.command == "audit":
        return cmd_audit(args)
    return cmd_reference(args)


if __name__ == "__main__":
    sys.exit(main())
