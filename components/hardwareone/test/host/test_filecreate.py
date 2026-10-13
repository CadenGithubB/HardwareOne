#!/usr/bin/env python3
"""Compile the shipping filecreate command handler with a content-keeping VFS double.

The real cmd_filecreate runs against a name -> content store whose "w" open
creates or truncates like VFS::open, so a create that reuses an existing name
must refuse and leave the content alone while a new name is still created.
Permission rules, the real tokenizer and the storage drivers are not covered.
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
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    source = (COMPONENT / "System_Filesystem.cpp").read_text()
    marker = "const char* cmd_filecreate("
    require(source.count(marker) == 1, "cmd_filecreate definition marker must be unique")
    handler = extract_block(source, marker)
    for contract in ('FsLockGuard fsGuard("filecreate")', 'VFS::openGuarded(path, "w", ctx, /*create=*/true)',
                     "requireQuotedPath(a, 0, path)"):
        require(contract in handler, f"filecreate boundary changed: {contract}")
    harness = (HERE / "filecreate_harness.cpp").read_text()
    insertion = "// INSERT_PRODUCTION_FILECREATE_HERE"
    require(harness.count(insertion) == 1, "expected exactly one insertion marker")
    harness = harness.replace(insertion, handler)
    with tempfile.TemporaryDirectory(prefix="hw1-filecreate-host-") as temp:
        generated = Path(temp) / "filecreate.cpp"
        generated.write_text(harness)
        executable = Path(temp) / "filecreate"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   str(generated), "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
