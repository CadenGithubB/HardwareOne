#!/usr/bin/env python3
"""Run the production factoryreset callback on the shared fake VFS.

The harness compiles factoryreset_confirmed, its removal helpers and the
cmd_factoryreset prompt from System_Utils.cpp, the real FsLockGuard and the
real VFS DirectoryReader against fake_vfs.h. It checks the removal set
(users.json, pending_users.json, every user_settings file, every u<id> folder
under /stt and, when mounted, /sd/stt) and what stays (settings, IP bans,
other /stt and /sd content), the order (id record, account data, users.json,
settings files), that a failure before users.json stops the reset with no
reboot scheduled, and that the prompt names what is deleted. Not covered: the
real esp_timer reboot, LittleFS/FAT behaviour, the System_User id record
itself (test_user_store_mutations.py) and the setup wizard after the reboot.
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


def string_double() -> str:
    """The shared host String plus the members the VFS reader calls."""
    stub = (HERE / "web_batch_stubs" / "Arduino.h").read_text()
    anchor = "  bool concat(const char* text) {"
    require(stub.count(anchor) == 1, "one concat(const char*) in the String double")
    extra = """  bool startsWith(const String& prefix) const { return startsWith(prefix.c_str()); }
  bool endsWith(const String& suffix) const { return endsWith(suffix.c_str()); }
"""
    return stub.replace(anchor, extra + anchor)


def unique_block(source: str, marker: str) -> str:
    require(source.count(marker) == 1, f"marker is not unique: {marker}")
    return extract_block(source, marker)


def generate() -> str:
    utils = (COMPONENT / "System_Utils.cpp").read_text()
    vfs_header = (COMPONENT / "System_VFS.h").read_text()
    vfs = (COMPONENT / "System_VFS.cpp").read_text()
    mutex = (COMPONENT / "System_Mutex.cpp").read_text()
    mutex_header = (COMPONENT / "System_Mutex.h").read_text()

    guard = extract_block(mutex_header, "struct FsLockGuard") + ";\n"
    guard += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()"))

    reader = "\n".join(unique_block(vfs_header, marker) + ";" for marker in (
        "struct VirtualEntry", "struct DirectoryEntry", "class DirectoryReader"))
    reader += "\n" + "\n".join(unique_block(vfs, marker) for marker in (
        "size_t listVirtualEntries(",
        "DirectoryReader::DirectoryReader(", "bool DirectoryReader::next("))

    # Everything from the reboot timer callback through the confirm callback:
    # the removal helpers sit between the two on a tree that has them.
    start_marker = "static void factoryreset_doRestart("
    end_marker = "static const char* factoryreset_confirmed("
    require(utils.count(start_marker) == 1 and utils.count(end_marker) == 1, "one factoryreset callback")
    start = utils.index(start_marker)
    end = utils.index(end_marker)
    reset = utils[start:end] + extract_block(utils[end:], end_marker)
    reset += "\n\n" + unique_block(utils, "static const char* factoryreset_cancelled(")
    reset += "\n\n" + unique_block(utils, "const char* cmd_factoryreset(")

    harness = (HERE / "factory_reset_wipe_harness.cpp").read_text()
    for marker, replacement in (("// INSERT_FS_GUARD", guard),
                                ("// INSERT_DIRECTORY_READER", reader),
                                ("// INSERT_FACTORY_RESET", reset)):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", help="run one harness case by name")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-factory-reset-") as temporary:
        directory = Path(temporary)
        (directory / "Arduino.h").write_text(string_double())
        unit = directory / "factory_reset_wipe.cpp"
        unit.write_text(generate())
        binary = directory / "factory_reset_wipe"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-parameter", "-Wno-unused-function",
                   "-I", str(directory), "-I", str(HERE), str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=180)
        subprocess.run([str(binary)] + ([args.case] if args.case else []), check=True, timeout=60)


if __name__ == "__main__":
    main()
