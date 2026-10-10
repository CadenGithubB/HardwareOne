#!/usr/bin/env python3
"""Exercise real writeText/writeTextAtomic with injected file and scheduling faults.

The write algorithms and filesystem lock guard are extracted unchanged from
production; File/VFS, Arduino String allocation, and RTOS waits are boundaries.
--baseline accepts the saved System_Utils.cpp used to reproduce the old bugs.
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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--case", choices=("all", "short-write", "overlap"), default="all")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    source = (args.baseline or COMPONENT / "System_Utils.cpp").read_text()
    mutex = (COMPONENT / "System_Mutex.cpp").read_text()
    mutex_header = (COMPONENT / "System_Mutex.h").read_text()
    guard = extract_block(mutex_header, "struct FsLockGuard") + ";\n"
    guard += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()"))
    helpers = "\n\n".join(extract_block(source, marker) for marker in (
        "bool writeText(", "bool writeTextAtomic("))
    harness = (HERE / "checked_text_writes_harness.cpp").read_text()
    for marker, replacement in {"// INSERT_FS_GUARD": guard, "// INSERT_WRITERS": helpers}.items():
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    with tempfile.TemporaryDirectory(prefix="hw1-checked-writes-") as directory:
        source_path, binary = Path(directory) / "test.cpp", Path(directory) / "test"
        source_path.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-parameter", "-Wno-unused-function", "-pthread",
                   "-I", str(COMPONENT), str(source_path), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(binary), args.case], check=True, timeout=30)


if __name__ == "__main__":
    main()
