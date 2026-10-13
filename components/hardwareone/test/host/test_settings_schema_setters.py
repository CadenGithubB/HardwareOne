#!/usr/bin/env python3
"""The settings schema offers a setter only where one exists, and the bonded
editors share one remote-edit policy.

Compiles System_Settings.cpp's buildSettingsSchemaJson with the editor core's
command predicate (System_SettingsEditorCore.cpp settingsEditorCommandName /
settingsEditorHasCommand) against a command-registry double, and checks that a
row carries "cmdKey" only when its setter is registered in this build (a row
whose setter is compiled out is listed without one, so the web page and the
bonded peer's OLED editor show it read-only instead of sending "Unknown
command"); every other field is unchanged.

Compiles BondedPeer::remoteEditAllowed and filterSchemaForRemoteEdit
(System_BondedPeer.cpp) and checks that a worker schema is narrowed to the
policy: users, bond and wifi are removed, clock keeps ntpServer and
tzOffsetMinutes, keyless rows go, everything kept is unchanged and "count"
follows. Source checks: GET /api/bond/settings/schema (WebPage_Bond.cpp) serves
that narrowed schema, and WebPage_Settings.h keeps no JS copy of the policy and
never derives a command from a key.

--component / --source-dir point the suite at another copy of the production
sources (the pre-fix tree fails both parts); the harness and this script are
always read from this directory.
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

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import JSON_INCLUDE, extract_block, require  # noqa: E402

HARNESS = HERE / "settings_schema_setters_harness.cpp"
BUILDER = "void buildSettingsSchemaJson(JsonDocument& doc)"
EDITOR = ("const char* settingsEditorCommandName(const SettingEntry* entry)",
          "bool settingsEditorHasCommand(const SettingEntry* entry)")
POLICY = "bool remoteEditAllowed(const char* moduleName, const char* key)"
FILTER = "void filterSchemaForRemoteEdit(JsonDocument& schema)"
# A command spelled from a schema key instead of the schema's cmdKey.
DERIVED_COMMAND = (
    r"opts\.cmdKey\s*\|\|\s*key\b",
    r"\(\s*cmd\s*\|\|\s*k\s*\)",
    r"getAttribute\('data-cmd'\)\s*\|\|",
    r"var\s+cmd\s*=\s*e\.cmdKey\s*\|\|\s*e\.key",
    r"sw\(\s*e\.cmdKey\s*\|\|\s*e\.key",
)


def struct(header: str, name: str) -> str:
    # Slice from the definition so a forward declaration is never matched.
    return extract_block(header[header.index(f"struct {name} {{"):], f"struct {name}") + ";"


def settings_types(read) -> str:
    header = read("System_Settings.h")
    return "\n".join([
        extract_block(header, "enum SettingType") + ";",
        struct(header, "SettingEntry"),
        "typedef bool (*ConnectionCheckFunc)();",
        struct(header, "SettingsModule"),
    ])


def production(read) -> str:
    editor = read("System_SettingsEditorCore.cpp")
    settings = read("System_Settings.cpp")
    bonded = read("System_BondedPeer.cpp")
    require(settings.count(BUILDER) == 1, "one buildSettingsSchemaJson definition")
    pieces = [extract_block(editor, marker) for marker in EDITOR]
    pieces.append(extract_block(settings, BUILDER))
    policy = [extract_block(bonded, POLICY)]
    if FILTER in bonded:
        policy.append(extract_block(bonded, FILTER))
        pieces.append("#define HW1_HAS_SCHEMA_FILTER 1")
    pieces.append("namespace BondedPeer {\n" + "\n\n".join(policy) + "\n}  // namespace BondedPeer")
    return "\n\n".join(pieces)


def source_checks(read) -> list[str]:
    failures = []
    page = read("WebPage_Settings.h")
    for table in ("BONDED_MODULE_DENYLIST", "BONDED_KEY_WHITELIST"):
        if table in page:
            failures.append(f"WebPage_Settings.h keeps its own copy of the remote-edit policy ({table})")
    for pattern in DERIVED_COMMAND:
        for match in re.finditer(pattern, page):
            line = page.count("\n", 0, match.start()) + 1
            failures.append(f"WebPage_Settings.h:{line} derives a command from a key: {match.group(0)}")
    bond = read("WebPage_Bond.cpp")
    handler = extract_block(bond, "static esp_err_t handleBondSettingsSchema(")
    if "BondedPeer::readRemoteEditableSchemaJson(" not in handler or "readCachedSchemaJson(" in handler:
        failures.append("GET /api/bond/settings/schema does not serve the schema narrowed by "
                        "BondedPeer::readRemoteEditableSchemaJson")
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("clang++") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=HERE.parents[1])
    parser.add_argument("--source-dir", type=Path,
                        help="overlay directory; a file present here wins over --component")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")

    def read(name: str) -> str:
        overlay = args.source_dir / name if args.source_dir else None
        return (overlay if overlay and overlay.exists() else args.component / name).read_text()

    failures = source_checks(read)
    harness = HARNESS.read_text()
    for marker, body in (("// INSERT_SETTINGS_TYPES", settings_types(read)),
                         ("// INSERT_PRODUCTION", production(read))):
        require(harness.count(marker) == 1, f"harness marker missing/duplicated: {marker}")
        harness = harness.replace(marker, body)
    with tempfile.TemporaryDirectory(prefix="hw1-settings-schema-setters-") as directory:
        unit, binary = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-missing-field-initializers", "-I", str(JSON_INCLUDE), str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=300)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
        sys.stdout.write(result.stdout)
        if result.returncode != 0 or not re.search(r"(?m)^checks=\d+$", result.stderr):
            failures.append(f"runtime cases failed:\n{result.stderr}")
    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    if failures:
        raise SystemExit("settings_schema_setters: FAILED")
    print("settings_schema_setters: cmdKey only for registered setters; one remote-edit policy")


if __name__ == "__main__":
    main()
