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


def unique_block(source: str, marker: str) -> str:
    require(source.count(marker) == 1, f"marker is not unique: {marker}")
    return extract_block(source, marker)


def production(migration: str, mutex: str, mutex_header: str, utils: str) -> dict:
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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", help="run one harness case by name (for per-case evidence)")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    migration = (COMPONENT / "WebServer_MigrationTool.cpp").read_text()
    harness = (HERE / "migration_restore_harness.cpp").read_text()
    replacements = production(migration, (COMPONENT / "System_Mutex.cpp").read_text(),
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


if __name__ == "__main__":
    main()
