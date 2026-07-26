#!/usr/bin/env python3
"""Settings registry tool — generate matrix doc or upgrade entry rows.

Subcommands:
  matrix    Read the settings registry from C++ source and write
            docs/SETTINGS_MATRIX.md (default if no subcommand given).
  upgrade   Rewrite SettingEntry literals in components/hardwareone/*.cpp
            to expand short-form rows (10 fields) into the full long form
            (13 fields: + isSecret, group, cmdKey). Idempotent: rows that
            are already long-form are left alone.

Both commands read components/hardwareone/*.cpp. Only `upgrade` writes to
source files; `matrix` only writes docs/SETTINGS_MATRIX.md.

Usage:
    python3 tools/settings_registry.py             # same as `matrix`
    python3 tools/settings_registry.py matrix
    python3 tools/settings_registry.py upgrade
"""

from __future__ import annotations
import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
COMPONENTS = REPO / "components" / "hardwareone"
WEB_PAGE_H = COMPONENTS / "WebPage_Settings.h"
OUT_MATRIX = REPO / "docs" / "SETTINGS_MATRIX.md"


# ============================================================================
# Shared parsing primitives
# ============================================================================

# Find every `*SettingEntries[]` array body. Used by both subcommands —
# `matrix` reads each row's fields semantically, `upgrade` rewrites them.
ARRAY_RE = re.compile(
    r"((?:static|extern)?\s*const\s+SettingEntry\s+\w+\[\]\s*=\s*\{)(.*?)(\};)",
    re.DOTALL,
)


def split_top_level(body: str) -> list[str]:
    """Split body into comma-separated fields at top depth, preserving content.

    Respects string literals (including escaped quotes) and nested braces /
    parens / brackets. Used by `upgrade` to manipulate individual fields.
    """
    fields, current, depth, in_string, esc = [], [], 0, False, False
    for c in body:
        if esc:
            current.append(c)
            esc = False
            continue
        if in_string:
            current.append(c)
            if c == "\\":
                esc = True
            elif c == '"':
                in_string = False
            continue
        if c == '"':
            in_string = True
            current.append(c)
        elif c in "({[":
            depth += 1
            current.append(c)
        elif c in ")}]":
            depth -= 1
            current.append(c)
        elif c == "," and depth == 0:
            fields.append("".join(current))
            current = []
        else:
            current.append(c)
    if current and "".join(current).strip():
        fields.append("".join(current))
    return fields


def find_row_spans(array_body: str):
    """Yield (start, end) of each top-level `{ ... }` row inside an array body."""
    depth = 0
    start = None
    in_string = False
    esc = False
    for i, c in enumerate(array_body):
        if esc:
            esc = False
            continue
        if in_string:
            if c == "\\":
                esc = True
            elif c == '"':
                in_string = False
            continue
        if c == '"':
            in_string = True
            continue
        if c == "{":
            if depth == 0:
                start = i
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0 and start is not None:
                yield start, i + 1
                start = None


# ============================================================================
# `matrix` subcommand
# ============================================================================

def parse_modules(src: str) -> dict:
    """Return entries_array_name -> {name, jsonSection}.

    Matches both `static const SettingsModule X = {...}` and `extern const
    SettingsModule X = {...}`. The body must contain at least name,
    jsonSection, and the entries-array identifier in that order.
    """
    pattern = re.compile(
        r"(?:static|extern)\s+const\s+SettingsModule\s+\w+\s*=\s*\{\s*"
        r'"([^"]+)"\s*,\s*'      # 1: name
        r'"([^"]+)"\s*,\s*'      # 2: jsonSection (may be a dotted path)
        r"(\w+)\s*,",            # 3: entries-array identifier
        re.DOTALL,
    )
    return {
        m.group(3): {"name": m.group(1), "jsonSection": m.group(2)}
        for m in pattern.finditer(src)
    }


def parse_entries_array_bodies(src: str) -> dict:
    """Return entries_array_name -> raw body text."""
    pattern = re.compile(
        r"(?:static|extern)?\s*const\s+SettingEntry\s+(\w+)\[\]\s*=\s*\{(.*?)\};",
        re.DOTALL,
    )
    return {m.group(1): m.group(2) for m in pattern.finditer(src)}


# Each entry row may use SHORT (10 fields, ending after `options`) or LONG
# (13 fields with isSecret/group/cmdKey trailing). Trailing segments are
# optional so this matches short(10) / +isSec(11) / +group(12) / long(13)
# all in one pass.
ROW_RE = re.compile(
    r"\{\s*"
    r'"([^"]+)"\s*,\s*'                 # 1: jsonKey
    r"SETTING_(\w+)\s*,\s*"             # 2: type
    r"[^,]+,\s*"                        # ptr
    r"[^,]+,\s*"                        # intDef
    r"[^,]+,\s*"                        # floatDef
    r"(?:nullptr|\"[^\"]*\")\s*,\s*"    # strDef
    r"[^,]+,\s*"                        # min
    r"[^,]+,\s*"                        # max
    r'"([^"]+)"\s*,\s*'                 # 3: label
    r"(?:nullptr|\"[^\"]*\")"           # options (terminal in short form)
    r"(?:\s*,\s*"
    r"(?:true|false)"                   # isSecret
    r"(?:\s*,\s*"
    r"(nullptr|\"[^\"]*\")"             # 4: group (or nullptr)
    r"(?:\s*,\s*"
    r"(nullptr|\"[^\"]+\")"             # 5: cmdKey (or nullptr)
    r")?)?)?"
    r"\s*\}",
    re.DOTALL,
)


def parse_rows(body: str):
    for m in ROW_RE.finditer(body):
        json_key, type_name, label, group_raw, cmd_key_raw = m.groups()
        if group_raw is None or group_raw == "nullptr":
            group = None
        else:
            group = group_raw.strip('"')
        if cmd_key_raw is None or cmd_key_raw == "nullptr":
            cmd_key = ""
        else:
            cmd_key = cmd_key_raw.strip('"')
        yield {
            "jsonKey": json_key,
            "type": type_name,
            "label": label,
            "group": group,
            "cmdKey": cmd_key,
        }


def parse_gl_dict(web_src: str) -> dict:
    """Pull the GL = {key:'Label', ...} dictionary out of WebPage_Settings.h."""
    m = re.search(r"var\s+GL\s*=\s*\{([^}]+)\}", web_src)
    if not m:
        return {}
    body = m.group(1)
    out = {}
    for kv in re.finditer(r"(?:'([^']+)'|(\w+))\s*:\s*'([^']+)'", body):
        key = kv.group(1) or kv.group(2)
        out[key] = kv.group(3)
    return out


def cmd_matrix(_args) -> int:
    modules = {}
    arrays = {}
    for cpp in sorted(COMPONENTS.glob("*.cpp")):
        text = cpp.read_text(errors="replace")
        modules.update(parse_modules(text))
        arrays.update(parse_entries_array_bodies(text))
    web = WEB_PAGE_H.read_text()
    gl = parse_gl_dict(web)             # group_id -> display label

    lines = [
        "# Settings Page Matrix",
        "",
        "_Auto-generated by `tools/settings_registry.py matrix`. Do not_",
        "_hand-edit; re-run after changes to the settings registry._",
        "",
        "**Levels:** *DRAWER* (module/section) → *FOLDER* (group) → *FILE* (entry)",
        "",
    ]

    for entries_name, body in arrays.items():
        if entries_name not in modules:
            continue  # array exists but no SettingsModule wraps it (uncommon)
        mod = modules[entries_name]

        # jsonSection may be a dotted path (e.g. "hardware.sensors.camera") —
        # render each segment as its own bracketed key so the matrix shows the
        # real JSON structure rather than one literal key with dots inside.
        path_parts = mod["jsonSection"].split(".") if mod["jsonSection"] else []
        path_str = "".join(f'["{p}"]' for p in path_parts) if path_parts else ""
        # The debug module was split into its own file (DEBUG_JSON_FILE); every
        # other module still persists in settings.json.
        persist_file = "debug.json" if mod["name"] == "debug" else "settings.json"
        lines.append(
            f'## DRAWER: `{mod["name"]}` '
            f"(persists under `{persist_file}{path_str}`)"
        )
        lines.append("")
        lines.append("| Folder (group) | File (label) | jsonKey | CLI command | Type |")
        lines.append("|---|---|---|---|---|")

        rows = list(parse_rows(body))
        rows.sort(key=lambda r: (r["group"] is None, r["group"] or "", r["label"]))

        prev_group = object()
        for r in rows:
            g = r["group"]
            if g is None:
                folder = "*(no folder — loose row)*"
            else:
                pretty = gl.get(g, f"~~missing GL entry~~ `{g}`")
                folder = f"{pretty} (`{g}`)"
            display_folder = folder if g != prev_group else ""
            lines.append(
                f'| {display_folder} | {r["label"]} | `{r["jsonKey"]}` | `{r["cmdKey"]}` | {r["type"]} |'
            )
            prev_group = g

        lines.append("")

    orphans = sorted(set(arrays) - set(modules))
    if orphans:
        lines.append("## Notes")
        lines.append("")
        lines.append(
            "Entry arrays not registered as a SettingsModule (won't appear in UI):"
        )
        for o in orphans:
            lines.append(f"- `{o}`")
        lines.append("")

    OUT_MATRIX.parent.mkdir(parents=True, exist_ok=True)
    OUT_MATRIX.write_text("\n".join(lines))
    print(f"Wrote {OUT_MATRIX.relative_to(REPO)}")
    print(f"  modules: {len(modules)}")
    print(f"  total entries: {sum(1 for body in arrays.values() for _ in parse_rows(body))}")
    print(f"  GL entries: {len(gl)}")
    return 0


# ============================================================================
# `upgrade` subcommand
# ============================================================================

def upgrade_row(row_text: str) -> tuple[str, str]:
    """Return (new_row_text, form_label).

    form_label is one of: short / +isSec / +group / long / skip / skip(n=N).
    Long-form rows are returned unchanged.
    """
    if not row_text.startswith("{") or "SETTING_" not in row_text:
        return row_text, "skip"

    inner = row_text[1:-1]
    fields = split_top_level(inner)
    n = len(fields)

    if n == 10:
        added = [" false", " nullptr", " nullptr"]   # +isSecret, group, cmdKey
        label = "short"
    elif n == 11:
        added = [" nullptr", " nullptr"]             # +group, cmdKey
        label = "+isSec"
    elif n == 12:
        added = [" nullptr"]                         # +cmdKey
        label = "+group"
    elif n == 13:
        return row_text, "long"
    else:
        return row_text, f"skip(n={n})"

    new_inner = (
        ", ".join(f.strip() for f in fields)
        + ", "
        + ", ".join(s.strip() for s in added)
    )
    return "{ " + new_inner + " }", label


def process_file(path: Path) -> dict:
    """Rewrite *.cpp in place if any short-form rows were upgraded.

    Returns counts per form label so main() can summarise.
    """
    text = path.read_text(errors="replace")
    stats = {"short": 0, "+isSec": 0, "+group": 0, "long": 0, "skip": 0, "arrays": 0}

    def replace_array(m: re.Match) -> str:
        head, body, tail = m.group(1), m.group(2), m.group(3)
        new_body_parts = []
        last = 0
        for s, e in find_row_spans(body):
            new_body_parts.append(body[last:s])
            new_row, label = upgrade_row(body[s:e])
            stats[label] = stats.get(label, 0) + 1
            new_body_parts.append(new_row)
            last = e
        new_body_parts.append(body[last:])
        stats["arrays"] += 1
        return head + "".join(new_body_parts) + tail

    new_text = ARRAY_RE.sub(replace_array, text)
    if new_text != text:
        path.write_text(new_text)
    return stats


def cmd_upgrade(_args) -> int:
    total = {
        "short": 0, "+isSec": 0, "+group": 0, "long": 0, "skip": 0,
        "arrays": 0, "files_changed": 0,
    }
    for cpp in sorted(COMPONENTS.glob("*.cpp")):
        before = cpp.read_text(errors="replace")
        stats = process_file(cpp)
        after = cpp.read_text(errors="replace")
        changed = before != after
        if any(stats[k] > 0 for k in ("short", "+isSec", "+group")):
            print(
                f"{cpp.name:<35}  upgrades: short={stats['short']}, "
                f"+isSec={stats['+isSec']}, +group={stats['+group']}, "
                f"kept long={stats['long']}, arrays={stats['arrays']}"
                f"{'  [changed]' if changed else ''}"
            )
        for k in total:
            if k in stats:
                total[k] += stats[k]
        if changed:
            total["files_changed"] += 1

    print()
    print("=== TOTALS ===")
    for k, v in total.items():
        print(f"  {k}: {v}")
    return 0


# ============================================================================
# Entry point
# ============================================================================

# ============================================================================
# `check` subcommand — the settings/command binding guard rail
# ============================================================================
#
# WHY THIS EXISTS. A SettingEntry is only editable if its `cmdKey` names a real
# command. Nothing in C++ enforces that: cmdKey is a string literal the compiler
# never validates, so a typo, a renamed command, or a deleted one turns a control
# into a silent no-op. An audit of all 407 controls found 243 writes silently
# discarded and 6 firing a DIFFERENT, real command — the debug row keyed
# "capture" fired the camera's `capture` verb and took a real photo.
#
# The jsonKey fallback that caused the misfires is now deleted in C++
# (settingsEditorCommandName returns cmdKey only), so a broken cmdKey is now
# merely dead rather than dangerous. This check keeps it from going dead
# unnoticed — run it in CI or before a commit that renames commands.

# A CommandEntry row: { "name", <help>, <bool admin>, handler, ... }
#
# <help> is NOT always one string literal. Several rows build it by concatenating
# literals with a macro, e.g.
#     { "i2csdapin", "Set I2C1 SDA pin: <0.." HW_GPIO_MAX_STR "> (max)", true, ... }
# An earlier version of this regex required a single literal there and so missed
# four real commands, reporting their settings rows as DEAD. Accept any run of
# string literals and/or bare identifiers before the admin bool.
CMD_ENTRY_RE = re.compile(
    r'\{\s*"([A-Za-z0-9_ ]+)"\s*,\s*'
    r'(?:"[^"]*"|[A-Za-z_]\w*)(?:\s*(?:"[^"]*"|[A-Za-z_]\w*))*'
    r'\s*,\s*(?:true|false)\s*,\s*[A-Za-z_]'
)
# Debug commands are emitted from the X-macro table, not literal CommandEntry
# rows, so they must be harvested separately or every debug row reads as DEAD.
DEBUG_CMD_RE = re.compile(r'"(debug[a-z0-9]+)"')


def collect_command_names() -> set[str]:
    """Every command name the firmware registers, lowercased."""
    names: set[str] = set()
    for src in list(COMPONENTS.glob("*.cpp")) + list(COMPONENTS.glob("*.h")):
        text = src.read_text(errors="ignore")
        for m in CMD_ENTRY_RE.finditer(text):
            names.add(m.group(1).lower())
        if src.name == "System_DebugFlags.h":
            for m in DEBUG_CMD_RE.finditer(text):
                names.add(m.group(1).lower())
    return names


def command_resolves(cmd_key: str, names: set[str]) -> bool:
    """Mirror findCommand(): case-insensitive, whole-word longest-prefix.

    A two-word cmdKey such as "sensorlog autostart" is legitimate — it resolves
    to the `sensorlog` command with "autostart" as its first argument.
    """
    k = cmd_key.strip().lower()
    if not k:
        return False
    return k in names or k.split()[0] in names


# A settings-tree access written as a literal bracket chain, e.g.
#     mergedDoc["network"]["wifi"]["autoReconnect"] = true;
# The compiler cannot see these, so renaming a jsonKey silently turns them into
# writes nobody reads. That is not hypothetical: the cross-device restore in
# WebServer_MigrationTool.cpp forced WiFi back on via ["network"]["wifi"]
# ["autoReconnect"], and a jsonKey rename left a restored device unable to
# rejoin the network, with no error anywhere.
#
# We only flag a chain whose LEADING segments match a real module jsonSection.
# That keeps status documents (doc["enabled"] = gCameraRunning) out of it -
# those are a different document with their own vocabulary.
CHAIN_RE = re.compile(r'((?:\[\s*"[A-Za-z0-9_]+"\s*\]){2,})')
SEG_RE = re.compile(r'\[\s*"([A-Za-z0-9_]+)"\s*\]')

# Structured data that lives in the settings tree but is NOT a scalar
# SettingEntry, so it legitimately has no jsonKey. These are managed by their
# own code (the saved-network list, the BLE peer registry) and are not part of
# the rename surface. Keep this list SHORT - every entry is a hole in the check.
NON_ENTRY_SUBTREES = {"networks", "peers"}


def check_settings_tree_literals(modules_by_section: dict) -> list[str]:
    """Find literal settings-tree accesses whose final key is not a real jsonKey."""
    problems: list[str] = []
    for src in sorted(list(COMPONENTS.glob("*.cpp")) + list(COMPONENTS.glob("*.h"))):
        for lineno, line in enumerate(src.read_text(errors="ignore").splitlines(), 1):
            if "SETTING_" in line:      # the registry rows themselves
                continue
            for chain in CHAIN_RE.findall(line):
                segs = SEG_RE.findall(chain)
                if len(segs) < 2:
                    continue
                key = segs[-1]
                if key in NON_ENTRY_SUBTREES:
                    continue
                # Try progressively shorter prefixes as the section path.
                for cut in range(len(segs) - 1, 0, -1):
                    section = ".".join(segs[:cut])
                    mod = modules_by_section.get(section)
                    if not mod:
                        continue
                    if key not in mod["keys"]:
                        problems.append(
                            f"{src.name}:{lineno} -> [\"{section.replace('.', '\"][\"')}\"][\"{key}\"] "
                            f"(module '{mod['name']}' has no jsonKey '{key}')"
                        )
                    break
    return problems


def cmd_check(args) -> int:
    names = collect_command_names()
    unbound, dead, dupes = [], [], []
    seen: dict[str, str] = {}
    total = 0

    for cpp in sorted(list(COMPONENTS.glob("*.cpp")) + list(COMPONENTS.glob("*.h"))):
        text = cpp.read_text(errors="ignore")
        for name, body in parse_entries_array_bodies(text).items():
            for row in parse_rows(body):
                total += 1
                key, ck = row["jsonKey"], row["cmdKey"]
                where = f"{cpp.name}::{name}::{key}"
                if not ck:
                    unbound.append(where)
                    continue
                if not command_resolves(ck, names):
                    dead.append(f"{where} -> '{ck}' (no such command)")
                prev = seen.get(ck.lower())
                if prev and prev != where:
                    dupes.append(f"'{ck}' claimed by both {prev} and {where}")
                seen.setdefault(ck.lower(), where)

    # Build section -> {name, keys} so the settings-tree literal check can
    # validate the final key of a bracket chain against the right module.
    modules_by_section = {}
    for cpp in sorted(list(COMPONENTS.glob("*.cpp")) + list(COMPONENTS.glob("*.h"))):
        text = cpp.read_text(errors="ignore")
        mods = parse_modules(text)
        bodies = parse_entries_array_bodies(text)
        for arr, meta in mods.items():
            keys = {r["jsonKey"] for r in parse_rows(bodies.get(arr, ""))}
            modules_by_section[meta["jsonSection"]] = {"name": meta["name"], "keys": keys}
    tree_problems = check_settings_tree_literals(modules_by_section)

    print(f"registered commands : {len(names)}")
    print(f"setting rows        : {total}")
    print(f"bound to a command  : {total - len(unbound)} "
          f"({100 * (total - len(unbound)) // max(total, 1)}%)")
    print(f"unbound (read-only) : {len(unbound)}")
    print(f"DEAD cmdKeys        : {len(dead)}")
    print(f"duplicate cmdKeys   : {len(dupes)}")
    print(f"settings modules    : {len(modules_by_section)}")
    print(f"bad tree literals   : {len(tree_problems)}")

    if args.verbose and unbound:
        print("\n-- unbound rows (render read-only; not an error) --")
        for u in unbound:
            print(f"   {u}")
    if dead:
        print("\n-- DEAD: cmdKey names a command that does not exist --")
        for d in dead:
            print(f"   {d}")
    if dupes:
        print("\n-- DUPLICATE: two settings share one command --")
        for d in dupes:
            print(f"   {d}")

    if tree_problems:
        print("\n-- BAD SETTINGS-TREE LITERAL: code touches a jsonKey that does not exist --")
        for t in tree_problems:
            print(f"   {t}")

    if dead or dupes or tree_problems:
        print("\nFAIL: fix the rows above, or set cmdKey to nullptr to make the "
              "row honestly read-only.")
        return 1
    print("\nOK")
    return 0


# ============================================================================
# Entry point
# ============================================================================

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Settings registry tool: check bindings, dump matrix, or upgrade rows.",
    )
    sub = parser.add_subparsers(dest="cmd")
    sub.add_parser("matrix", help="Regenerate docs/SETTINGS_MATRIX.md (WRITES)")
    sub.add_parser("upgrade", help="Expand short-form SettingEntry rows to long form (WRITES)")
    chk = sub.add_parser("check", help="Verify every cmdKey resolves to a real command (read-only)")
    chk.add_argument("-v", "--verbose", action="store_true", help="list unbound rows too")
    args = parser.parse_args()

    # No default subcommand: `matrix` and `upgrade` REWRITE tracked files, and
    # defaulting to one of them means an exploratory run silently edits the repo.
    if not args.cmd:
        parser.print_help()
        return 2
    if args.cmd == "matrix":
        return cmd_matrix(args)
    if args.cmd == "upgrade":
        return cmd_upgrade(args)
    if args.cmd == "check":
        return cmd_check(args)
    parser.error(f"unknown subcommand: {args.cmd}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
