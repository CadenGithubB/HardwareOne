#!/usr/bin/env python3
"""Exercise the production automation-log owner, commands and executor hooks.

Only Arduino allocation, RTOS scheduling, guarded VFS and task identity are
boundary doubles. The owner, argument parser, filesystem guard and output
redactor are compiled directly from the firmware sources.
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


def generated() -> str:
    owner = (COMPONENT / "System_Automation.cpp").read_text()
    utils = (COMPONENT / "System_Utils.cpp").read_text()
    command = (COMPONENT / "System_Command.cpp").read_text()
    command_header = (COMPONENT / "System_Command.h").read_text()
    user_header = (COMPONENT / "System_User.h").read_text()
    mutex = (COMPONENT / "System_Mutex.cpp").read_text()
    mutex_header = (COMPONENT / "System_Mutex.h").read_text()
    start = owner.index("// Automation log ownership")
    end = owner.index("// Forward declarations", start)
    guard = extract_block(mutex_header, "struct FsLockGuard") + ";\n"
    guard += "\n".join(extract_block(mutex, signature) for signature in (
        "FsLockGuard::FsLockGuard(const char* owner)",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()"))
    parser = extract_block(command_header, "class CommandArgs") + ";\n"
    parser += "const String CommandArgs::empty_;\n"
    parser += "\n".join(extract_block(command, signature) for signature in (
        "CommandArgs::CommandArgs(", "void CommandArgs::parse(",
        "const String& CommandArgs::arg("))
    command_start = utils.index("// Log command execution if automation logging")
    output_start = utils.index("// Log command output if this is an automation sub-command")
    hooks = []
    for offset in (command_start, output_start):
        hooks.append(extract_block(utils[offset:], "if (automationLogActive()"))
    replacements = {
        "// INSERT_AUTH_TYPES": extract_block(user_header, "enum CommandSource") + ";\n" +
                               extract_block(user_header, "struct AuthContext") + ";",
        "// INSERT_FS_GUARD": guard,
        "// INSERT_COMMAND_PARSER": parser,
        "// INSERT_OWNER": owner[start:end],
        "// INSERT_AUTOLOG_COMMAND": extract_block(owner, "const char* cmd_autolog("),
        "// INSERT_REDACTOR": extract_block(utils, "String redactOutputForLog("),
        "// INSERT_COMMAND_HOOK": hooks[0],
        "// INSERT_OUTPUT_HOOK": hooks[1],
    }
    harness = (HERE / "automation_log_harness.cpp").read_text()
    for marker, replacement in replacements.items():
        require(harness.count(marker) == 1, f"exactly one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    for obsolete in ("gAutomationLogActive", "gAutomationLogFile", "gAutomationLogOwnerCtx"):
        require(obsolete not in owner + utils + (COMPONENT / "System_Automation.h").read_text(),
                f"retired logging global remains: {obsolete}")
    require("appendAutoLogEntry" not in (COMPONENT / "System_Utils.h").read_text(),
            "automation append declaration must belong to its owner")
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    sanitizer = parser.add_mutually_exclusive_group()
    sanitizer.add_argument("--sanitize", action="store_true")
    sanitizer.add_argument("--thread-sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread",
             "-Wno-unused-parameter", "-Wno-unused-function", "-Wno-unused-variable",
             "-Wno-sign-compare"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    if args.thread_sanitize:
        flags += ["-fsanitize=thread", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-automation-log-") as directory:
        work = Path(directory)
        source, binary = work / "test.cpp", work / "test"
        source.write_text(generated())
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=60)
        # The actual disabled public header must need no filesystem, mutex,
        # authentication or owner implementation to link its unavailable API.
        shutil.copyfile(COMPONENT / "System_Automation.h", work / "System_Automation.h")
        shutil.copyfile(COMPONENT / "System_CommandRegistry.h", work / "System_CommandRegistry.h")
        shutil.copyfile(HERE / "web_batch_stubs/Arduino.h", work / "Arduino.h")
        (work / "System_BuildConfig.h").write_text("#pragma once\n#define ENABLE_AUTOMATION 0\n")
        source.write_text('''#include "System_Automation.h"
#include <cassert>
#include <cstring>
void webBatchTestStringAppend() {}
int main() {
  assert(!automationLogActive());
  assert(!appendAutoLogEntry("COMMAND", "unused"));
  const char* result = cmd_autolog("start /unused.log");
  assert(result && (strstr(result, "disabled") || strstr(result, "unavailable") || strstr(result, "not enabled")));
}
''')
        subprocess.run([args.cxx, *flags, "-I", str(work), str(source), "-o", str(binary)],
                       check=True, timeout=120)
        subprocess.run([str(binary)], check=True, timeout=60)
    print("automation logging: disabled owner API passed without platform headers", flush=True)


if __name__ == "__main__":
    main()
