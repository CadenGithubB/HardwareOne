#!/usr/bin/env python3
"""Contract: each login policy (serial, UART link, local display) has one owner.

Scans every production source for handlers that write localDisplayRequireAuth,
serialRequireAuth or uartRequireAuth, then compiles the production settings rows
and command tables that hold those policies (always the OLED and output rows and
the OLED, Settings and settings-editor commands, plus any other table the scan
finds) with generated link fixtures. It checks that every row and command
writing a policy is super-admin only, that each policy is persisted by exactly
one row (one JSON key), that every policy write sits in a registered command
handler, and that every OLED row names an existing command. Table metadata
only: authorizeCommand, the web UI and device behavior are not run.
--component/--source-dir point the suite at another production tree.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
POLICY_FIELDS = ("localDisplayRequireAuth", "serialRequireAuth", "uartRequireAuth")
POLICY_JSON_KEYS = ("displayRequireAuth", "serialRequireAuth", "uartRequireAuth")
POLICY_CMD_KEYS = ("displayrequireauth", "serialrequireauth", "uartrequireauth")
FIELD = "|".join(POLICY_FIELDS)
# A policy write: a typed setter or assignment on the field, or a generic
# setting command aimed at the policy row by jsonKey or cmdKey.
POLICY_WRITE = re.compile(
    rf"setSetting\(\s*gSettings\.(?:{FIELD})\b"
    rf"|gSettings\.(?:{FIELD})\s*=(?!=)"
    rf"|findOutputEntry\(\s*\"(?:{'|'.join(POLICY_JSON_KEYS)})\""
    rf"|findSettingByCmdKey\(\s*\"(?:{'|'.join(POLICY_CMD_KEYS)})\"")
HANDLER = re.compile(
    r"(?m)^(?:static\s+)?const\s+char\s*\*\s*(\w+)\s*\(\s*const\s+String\s*&\s*\w*\s*\)\s*(?=\{)")
COMMAND_TABLE = re.compile(r"(?m)^(?:static\s+)?const\s+CommandEntry\s+(\w+)\[\]")
ROW_TABLE = re.compile(r"(?m)^(?:static\s+)?const\s+SettingEntry\s+(\w+)\[\]")
ROW = re.compile(r"\{\s*\"\w+\"\s*,\s*(SETTING_\w+)\s*,\s*&gSettings\.(\w+)\b")
EDITOR_CMD = re.compile(rf"SETTING_EDITOR_CMD\(\s*\w+\s*,\s*\"(?:{'|'.join(POLICY_CMD_KEYS)})\"")
FIXED_ROW_TABLES = ("oledSettingEntries", "outputSettingEntries")
FIXED_COMMAND_TABLES = ("oledCommands", "settingsCommands", "settingEditorCommands")
CTYPE = {"SETTING_BOOL": "bool", "SETTING_INT": "int", "SETTING_U8": "uint8_t",
         "SETTING_U16": "uint16_t", "SETTING_U32": "uint32_t", "SETTING_FLOAT": "float",
         "SETTING_STRING": "String"}


def line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def production_sources(component: Path, source_dir: Path | None) -> dict[str, str]:
    sources = {}
    for path in sorted(component.rglob("*")):
        relative = path.relative_to(component)
        if (path.suffix not in (".cpp", ".h") or relative.parts[0] == "test"
                or path.name.startswith("._") or not path.is_file()):
            continue
        overlay = source_dir / relative if source_dir else None
        chosen = overlay if overlay and overlay.exists() else path
        sources[relative.as_posix()] = chosen.read_text(encoding="utf-8", errors="replace")
    return sources


def definition_at(text: str, match: re.Match) -> str:
    # Slice at the match so an earlier declaration or mention cannot be taken.
    return extract_block(text[match.start():], match.group(0).rstrip())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    parser.add_argument("--source-dir", type=Path)
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    component = args.component.resolve()
    sources = production_sources(component, args.source_dir)
    failures: list[str] = []

    # Policy writers: command-handler-shaped definitions whose body writes a policy.
    writers: dict[str, list[str]] = {}
    handler_spans: list[tuple[str, int, int, str]] = []
    for name, text in sources.items():
        for match in HANDLER.finditer(text):
            body = definition_at(text, match)
            handler_spans.append((name, match.start(), match.start() + len(body), match.group(1)))
            if POLICY_WRITE.search(body):
                writers.setdefault(match.group(1), []).append(f"{name}:{line_of(text, match.start())}")
    print("policy writers:", ", ".join(f"{w} ({', '.join(at)})" for w, at in sorted(writers.items())), flush=True)

    # Every policy write must sit in such a handler; anything else is unaudited.
    for name, text in sources.items():
        for match in POLICY_WRITE.finditer(text):
            inside = any(file == name and start <= match.start() < end
                         for file, start, end, _ in handler_spans)
            if not inside:
                failures.append(f"policy write outside a command handler at {name}:"
                                f"{line_of(text, match.start())}: {match.group(0)}")
        for match in EDITOR_CMD.finditer(text):
            failures.append(f"generic editor command for a login policy at {name}:"
                            f"{line_of(text, match.start())}: {match.group(0)}")

    # Tables to compile: the fixed OLED/output owners plus every table the scan ties
    # to a policy (rows persisting a policy field, commands naming a writer).
    row_tables: dict[str, str] = {}
    command_tables: dict[str, str] = {}
    referenced: set[str] = set()
    for name, text in sources.items():
        for match in ROW_TABLE.finditer(text):
            block = definition_at(text, match)
            if match.group(1) in FIXED_ROW_TABLES or re.search(rf"&gSettings\.(?:{FIELD})\b", block):
                require(match.group(1) not in row_tables, f"duplicate row table {match.group(1)}")
                row_tables[match.group(1)] = block + ";"
        for match in COMMAND_TABLE.finditer(text):
            block = definition_at(text, match)
            hits = {w for w in writers if re.search(rf"\b{w}\b", block)}
            referenced |= hits
            if match.group(1) in FIXED_COMMAND_TABLES or hits:
                require(match.group(1) not in command_tables, f"duplicate command table {match.group(1)}")
                command_tables[match.group(1)] = block + ";"
    for table in FIXED_ROW_TABLES:
        require(table in row_tables, f"missing production row table {table}")
    for table in FIXED_COMMAND_TABLES:
        require(table in command_tables, f"missing production command table {table}")
    for writer in sorted(set(writers) - referenced):
        failures.append(f"policy writer {writer} ({', '.join(writers[writer])}) is not registered "
                        "in any command table, so no admin gate applies to it")
    print("compiled row tables:", ", ".join(row_tables), flush=True)
    print("compiled command tables:", ", ".join(command_tables), flush=True)

    # Generated link fixtures.
    fields = {field: "bool" for field in POLICY_FIELDS}
    for block in row_tables.values():
        for setting_type, field in ROW.findall(block):
            require(setting_type in CTYPE, f"unknown setting type {setting_type}")
            fields.setdefault(field, CTYPE[setting_type])
    settings = ("struct Settings {\n" + "".join(f"  {ctype} {field}{{}};\n" for field, ctype in sorted(fields.items()))
                + "};\nstatic Settings gSettings;")
    handlers = sorted({h for block in command_tables.values() for h in re.findall(r"\bcmd_\w+\b", block)}
                      | (set(writers) & referenced))
    stubs = "\n".join(f"[[maybe_unused]] static const char* {h}(const String&) {{ return \"{h}\"; }}"
                      for h in handlers)
    count = "sizeof({0}) / sizeof({0}[0])"
    lists = "\n".join([
        "static const RowTable kRowTables[] = {",
        *[f'  {{"{t}", {t}, {count.format(t)}}},' for t in row_tables],
        "};",
        "static const CommandTable kCommandTables[] = {",
        *[f'  {{"{t}", {t}, {count.format(t)}}},' for t in command_tables],
        "};",
        "static const Writer kPolicyWriters[] = {",
        *[f'  {{"{w}", {w}}},' for w in sorted(set(writers) & referenced)],
        "  {nullptr, nullptr},",
        "};"])

    settings_h = sources["System_Settings.h"]
    types = "\n\n".join(extract_block(settings_h, marker) + ";"
                        for marker in ("enum SettingType", "struct SettingEntry "))
    harness = (HERE / "display_auth_policy_harness.cpp").read_text()
    for marker, body in (("// INSERT_TYPES", types), ("// INSERT_SETTINGS", settings),
                         ("// INSERT_STUBS", stubs),
                         ("// INSERT_TABLES", "\n\n".join([*row_tables.values(), *command_tables.values()])),
                         ("// INSERT_LISTS", lists)):
        require(harness.count(marker) == 1, f"bad harness marker: {marker}")
        harness = harness.replace(marker, body)

    with tempfile.TemporaryDirectory(prefix="hw1-display-auth-policy-") as temporary:
        directory = Path(temporary)
        source = directory / "display_auth_policy.cpp"
        executable = directory / "display_auth_policy"
        source.write_text(harness)
        registry = directory / "System_CommandRegistry.h"
        registry.write_text(sources["System_CommandRegistry.h"])
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-I", str(directory), "-I", str(HERE / "web_batch_stubs"),
                   str(source), "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(executable)], timeout=60)
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures or result.returncode != 0:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
