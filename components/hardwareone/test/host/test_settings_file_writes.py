#!/usr/bin/env python3
"""Execute the shipping settings writers with real ArduinoJson and file faults.

The shared snapshot helper, main/debug/restamp and per-user entry points,
load/mutate/save transactions, writer mutex and filesystem guard are extracted
without rewriting their bodies. File/VFS/stat, the live-settings document builder,
RTOS and cache invalidation are observed boundaries. --baseline replays targeted
regressions against saved code. --case user-parity prints synthetic JSON hashes
for byte comparisons; --case user also characterizes the legacy save failures.
These checks do not establish power-loss durability or device flash behavior.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"


def generate(source: str) -> str:
    mutex = (COMPONENT / "System_Mutex.cpp").read_text()
    mutex_header = (COMPONENT / "System_Mutex.h").read_text()
    guard = extract_block(mutex_header, "struct FsLockGuard") + ";\n"
    guard += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()"))
    start = source.index("static StaticSemaphore_t sSettingsFileWriteMutexStorage;")
    end = source.index("static constexpr uint64_t kExplicitBatchDomain", start)
    helper = (extract_block(source, "static bool writeSettingsSnapshot(")
              if "static bool writeSettingsSnapshot(" in source else "")
    shared_json = "static JsonSnapshotWriteResult writeJsonSnapshot(" in source
    if shared_json:
        helper = (extract_block(source, "struct JsonSnapshotWriteResult") + ";\n" +
                  extract_block(source, "static JsonSnapshotWriteResult writeJsonSnapshot(") +
                  "\n" + helper)
    user_functions = "\n\n".join(extract_block(source, marker) for marker in (
        "String getUserSettingsPath(", "bool loadUserSettings(",
        "struct UserPrefsInvalidateOnExit", "bool saveUserSettings(",
        "UserSettingsTxnStatus runUserSettingsTransaction(",
        "struct UserSettingsMergeContext", "bool mergeUserSettingsMutation(",
        "bool mergeAndSaveUserSettings("))
    # Struct extractors deliberately omit the trailing declaration semicolon.
    for marker in ("struct UserPrefsInvalidateOnExit", "struct UserSettingsMergeContext"):
        declaration = extract_block(source, marker)
        user_functions = user_functions.replace(declaration, declaration + ";")
    definitions = [extract_block(source, marker) for marker in (
        "bool writeSettingsJson(", "static void buildDebugJsonDoc(",
        "bool writeDebugJson(", "bool settingsFirmwareVersionRestampPending(",
        "bool settingsRestampFirmwareVersion(")]
    harness = (HERE / "settings_file_writes_harness.cpp").read_text()
    for marker, text in {
        "// INSERT_FS_GUARD": guard,
        "// INSERT_WRITER_GUARD": source[start:end],
        "// INSERT_SNAPSHOT": helper,
        "// INSERT_WRITERS": "\n\n".join(definitions),
        "// INSERT_USER_WRITERS": user_functions,
    }.items():
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, text)
    return (f"#define HAS_SNAPSHOT_HELPER {int(bool(helper))}\n"
            f"#define HAS_SHARED_JSON {int(shared_json)}\n" + harness)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--case", choices=("all", "rename", "merge-open", "restamp-short", "restamp-overlap", "user", "user-parity"), default="all")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    production = (args.baseline or COMPONENT / "System_Settings.cpp").read_text()
    with tempfile.TemporaryDirectory(prefix="hw1-settings-file-writes-") as directory:
        unit = Path(directory) / "settings.cpp"
        unit.write_text(generate(production))
        for bonded in (0, 1):
            binary = Path(directory) / f"settings-{bonded}"
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-Wno-unused-function", "-Wno-unused-parameter", "-pthread",
                       "-DENABLE_ESPNOW=1", f"-DENABLE_BONDED_MODE={bonded}",
                       "-I", str(HERE / "web_batch_stubs"), "-I", str(COMPONENT),
                       "-I", str(JSON_INCLUDE), str(unit), "-o", str(binary)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                 "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=120)
            subprocess.run([str(binary), args.case], check=True, timeout=30)


if __name__ == "__main__":
    main()
