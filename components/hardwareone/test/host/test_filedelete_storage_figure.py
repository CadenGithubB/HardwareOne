#!/usr/bin/env python3
"""Run the production doFiledelete against a host filesystem stand-in and check
that a LittleFS delete marks the internal status storage figure stale once.

Status polls (buildSystemInfoJson) read the internal capacity snapshot at any
age so they never walk LittleFS; a user delete is one of the two operations
(with a web upload) that mark it stale. --source takes a saved
System_Filesystem.cpp for a before/after run.
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
    parser.add_argument("--source", type=Path, help="saved System_Filesystem.cpp")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    source = (args.source or COMPONENT / "System_Filesystem.cpp").read_text()
    routine = extract_block(source, "static const char* doFiledelete(const String& path, const AuthContext& ctx)")
    harness = (HERE / "filedelete_storage_figure_harness.cpp").read_text()
    require(harness.count("// INSERT_DO_FILEDELETE") == 1, "one insertion marker")
    harness = harness.replace("// INSERT_DO_FILEDELETE", routine)
    with tempfile.TemporaryDirectory(prefix="hw1-filedelete-figure-") as directory:
        unit, binary = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                   "-Wno-unused-parameter", str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=30)


if __name__ == "__main__":
    main()
