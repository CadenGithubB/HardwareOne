#!/usr/bin/env python3
"""Execute the production migration restore and backup walk on the shared fake VFS.

The harness compiles applyStagedMigrationRestore, writeRestoreFilesFromDoc and
the backup directory walk from WebServer_MigrationTool.cpp with the real
FsLockGuard, readText/writeText/writeTextAtomic and vendored ArduinoJson
against fake_vfs.h. Crypto, events and output are doubles.

It checks that one failing file write makes the whole restore fail, leaves
every device file as it was, removes the staging files and does not set
gRestoreComplete; that a successful restore moves users.json into place last;
that a failed cross-device credential-setup marker fails the restore; that the
backup walk keeps nested paths longer than 128 bytes whole and leaves out (with
a warning) a map that would push the bundle past the 512 KB the restore accepts.
The source contract checks that /api/backup refuses an overflowed or oversized
bundle and shares its limit with /api/restore.

It also checks the staged bundle's provenance summary (claimed hostname and
board, bytes received, SHA-256 prefix, same or other device) on serial and in
the three-line OLED prompt, with terminal escapes in the claims neutralised
and a bundle without a fingerprint shown as unverified, never "this device";
what the operator is told to compare the size and SHA-256 with (a forced
cross-device upload carries an added "force" key and cannot match the file);
that discarding frees the staged buffer while the confirm flag is still set;
and, in source, that first-time setup shows the summary before its default-No
prompt and that the restore server is stopped before its staged buffer is
discarded. The bundle's formatVersion: missing is format 1, a newer or
non-whole-number one is refused, and /api/restore refuses it after the magic
check and before the fingerprint check or staging.
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
COMPONENT = HERE.parents[1]
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"

RESTORE_START = "static char* sStagedRestoreBuf = nullptr;"
RESTORE_END = "bool applyStagedMigrationRestore()"


def string_double() -> str:
    """The shared host String plus the members the migration code calls."""
    stub = (HERE / "web_batch_stubs" / "Arduino.h").read_text()
    anchor = "  bool concat(const char* text) {"
    require(stub.count(anchor) == 1, "one concat(const char*) in the String double")
    extra = """  bool concat(const String& other) { return concat(other.c_str()); }
  bool reserve(size_t capacity) { text_.reserve(capacity); return true; }
"""
    return stub.replace(anchor, extra + anchor)


def code_only(source: str) -> str:
    """The source with C/C++ comments removed (string literals kept)."""
    out, pos, state = [], 0, "code"
    while pos < len(source):
        char, following = source[pos], source[pos + 1 : pos + 2]
        if state == "line":
            if char == "\n":
                state = "code"
                out.append(char)
        elif state == "comment":
            if char == "*" and following == "/":
                state = "code"
                pos += 1
        elif state in ("string", "character"):
            out.append(char)
            if char == "\\":
                out.append(following)
                pos += 1
            elif char == ('"' if state == "string" else "'"):
                state = "code"
        elif char == "/" and following == "/":
            state, pos = "line", pos + 1
        elif char == "/" and following == "*":
            state, pos = "comment", pos + 1
        else:
            if char in ('"', "'"):
                state = "string" if char == '"' else "character"
            out.append(char)
        pos += 1
    return "".join(out)


def unique_block(source: str, marker: str) -> str:
    require(source.count(marker) == 1, f"marker is not unique: {marker}")
    return extract_block(source, marker)


def production(migration: str, header: str, mutex: str, mutex_header: str, utils: str) -> dict:
    guard = extract_block(mutex_header, "struct FsLockGuard") + ";\n"
    guard += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()"))
    text_io = "\n\n".join(extract_block(utils, marker)
                          for marker in ("bool readText(", "bool writeText(", "bool writeTextAtomic("))

    constants = re.findall(r'^static const char\* [A-Z_]+\s*=\s*"[^"]*";', migration, re.M)
    require(len(constants) == 8, "the eight migration path constants")
    constants += re.findall(r"^static constexpr size_t k\w+ = [^;]+;", migration, re.M)
    constants += re.findall(r"^constexpr uint32_t kBackupFormatVersion = \d+;", header, re.M)
    require(len(constants) >= 9 and constants[-1].startswith("constexpr uint32_t kBackupFormatVersion"),
            "the backup format constant from WebServer_MigrationTool.h")

    require(migration.count(RESTORE_START) == 1 and migration.count(RESTORE_END) == 1, "one restore section")
    start = migration.index(RESTORE_START)
    end_start = migration.index(RESTORE_END, start)
    restore = migration[start:end_start] + extract_block(migration[end_start:], RESTORE_END)

    pieces = [unique_block(migration, "static bool isCertFile("),
              unique_block(migration, "static void stripAesCiphertextValues(")]
    capped = "static void keepBackupWithinRestoreLimit(" in migration
    if capped:
        pieces += [unique_block(migration, "static void warnBackupOmitted("),
                   unique_block(migration, "static void keepBackupWithinRestoreLimit(")]
    pieces.append(unique_block(migration, "static void addDirectoryToBackup("))
    pieces.append(restore)
    cap_argument = ", &doc" if capped else ""
    adapter = ("static void addMapsToBackup(JsonDocument& doc, JsonObject& files, JsonArray& warnings) {\n"
               "  AuthContext ctx;\n"
               f"  addDirectoryToBackup(files, warnings, \"/maps\", ctx{cap_argument});\n"
               "  (void)doc;\n}")
    return {"// INSERT_FS_GUARD": guard, "// INSERT_TEXT_IO": text_io,
            "// INSERT_CONSTANTS": "\n".join(constants),
            "// INSERT_PRODUCTION": "\n\n".join(pieces), "// INSERT_BACKUP_ADAPTER": adapter}


def source_contract(migration: str) -> None:
    """The export never sends what the restore refuses or what lost data."""
    backup = unique_block(migration, "static esp_err_t handleBackup(")
    require("doc.overflowed()" in backup, "handleBackup must refuse an overflowed bundle")
    require("kMigrationPayloadMax" in backup, "handleBackup must check the restore limit")
    restore = unique_block(migration, "static esp_err_t handleRestore(")
    require("kMigrationPayloadMax" in restore, "handleRestore must use the shared limit")
    writer = unique_block(migration, "static bool writeRestoreFilesFromDoc(")
    require("filesWritten > 0 ||" not in writer, "a partial restore must not report success")
    require("stageMigrationRestore(buf," in restore, "handleRestore must stage through stageMigrationRestore")
    # The export writes the named format; the restore checks it after the
    # magic and refuses (freeing the body, staging nothing) before the
    # fingerprint check or staging.
    require('doc["formatVersion"] = kBackupFormatVersion;' in backup,
            "handleBackup must write the named backup format")
    require(re.search(r'formatVersion"\]\s*=\s*\d', migration) is None,
            "no bundle formatVersion literal")
    check = restore.find("checkBackupFormat(doc.as<JsonVariantConst>(), &bundleFormat)")
    require(restore.find('strcmp(magic, "HWBACKUP")') < check, "the format is checked after the magic")
    require(0 < check < restore.find("bool compatible = true;") < restore.find("stageMigrationRestore(buf,"),
            "the format is checked before the fingerprint check and staging")
    # The refusal, comments removed: it follows the check directly, its first
    # statement frees the body and its last returns. Nothing else in the
    # handler branches on the check (handleRestore is not compiled here).
    code = code_only(restore)
    call = "const BackupFormatCheck format = checkBackupFormat(doc.as<JsonVariantConst>(), &bundleFormat);"
    guard = "if (format != BackupFormatCheck::Supported) {"
    require(code.count(call) == 1 and code.count("checkBackupFormat(") == 1, "one format check")
    require(code[code.index(call) + len(call):].lstrip().startswith(guard),
            "the refusal directly follows the format check")
    require(code.count("BackupFormatCheck::") == 2 and "goto " not in code,
            "only the refusal (and its Newer message) branch on the format")
    refusal = extract_block(code, guard[:-2])
    body = refusal[refusal.index("{") + 1 : -1].strip()
    require(body.startswith("free(buf);") and body.endswith("return ESP_OK;")
            and "stageMigrationRestore" not in refusal,
            "a refused format frees the body first and returns before staging")
    stop = unique_block(migration, "void stopRestoreOnlyHttpServer()")
    discard = stop.find("discardStagedMigrationRestore();")
    require(discard > stop.rfind("httpd_stop(server);") > 0,
            "the restore server must be stopped before its staged buffer is discarded")


def setup_contract(setup: str) -> None:
    """First-time setup shows the bundle's provenance before a default-No confirm."""
    marker = "if (migrationRestoreAwaitingConfirm())"
    require(setup.count(marker) == 1, "one staged-restore confirm in first-time setup")
    confirm = extract_block(setup[setup.index(marker):], marker)
    summary = confirm.find('broadcastOutput("Backup staged " + migrationStagedSummary());')
    advice = confirm.find("broadcastOutput(migrationStagedCheckAdvice());")
    prompt = confirm.find("getOLEDYesNoPrompt(migrationStagedOledPrompt().c_str(), false)")
    serial = confirm.find("waitForSerialInputBlocking()")
    require(0 <= summary < prompt and summary < serial, "provenance is shown before either confirm")
    require(summary < advice < prompt and advice < serial, "the check advice is shown before either confirm")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", help="run one harness case by name (for per-case evidence)")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    migration = (COMPONENT / "WebServer_MigrationTool.cpp").read_text()
    harness = (HERE / "migration_restore_harness.cpp").read_text()
    replacements = production(migration, (COMPONENT / "WebServer_MigrationTool.h").read_text(),
                              (COMPONENT / "System_Mutex.cpp").read_text(),
                              (COMPONENT / "System_Mutex.h").read_text(),
                              (COMPONENT / "System_Utils.cpp").read_text())
    for marker, replacement in replacements.items():
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    with tempfile.TemporaryDirectory(prefix="hw1-migration-") as temporary:
        directory = Path(temporary)
        (directory / "Arduino.h").write_text(string_double())
        unit = directory / "migration_restore.cpp"
        unit.write_text(harness)
        binary = directory / "migration_restore"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-variable",
                   "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1", "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                   "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0", "-DARDUINOJSON_ENABLE_PROGMEM=0",
                   "-I", str(directory), "-I", str(HERE), "-I", str(COMPONENT), "-I", str(JSON_INCLUDE),
                   str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=180)
        subprocess.run([str(binary)] + ([args.case] if args.case else []), check=True, timeout=60)
    source_contract(migration)
    setup_contract((COMPONENT / "System_FirstTimeSetup.cpp").read_text())


if __name__ == "__main__":
    main()
