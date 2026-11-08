#!/usr/bin/env python3
"""Execute the production LittleFS log-overflow latch.

The free-space cache, resolveLogPath and resolveOverflowPath come unchanged
from System_VFS.cpp; the clock, the LittleFS size report and the SD mount and
write-probe answers are host-controlled. Checks that a latched resolution with
no writable card is Refused (the reserve is kept), that a mounted but
unwritable card is not used, and that the latch re-arms when space returns.
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


def generate() -> str:
    vfs = (COMPONENT / "System_VFS.cpp").read_text()
    start = vfs.index("static constexpr size_t LOG_OVERFLOW_DEFAULT_RESERVE")
    block = extract_block(vfs, "bool resolveOverflowPath(const char* primaryPath")
    end = vfs.index(block) + len(block)
    production = vfs[start:end]
    require("LogTier resolveLogPath(" in production, "resolveLogPath lives in the overflow region")
    harness = (HERE / "log_overflow_latch_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one production insertion marker")
    return harness.replace("// INSERT_PRODUCTION", production)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-log-overflow-") as temporary:
        unit, binary = Path(temporary) / "test.cpp", Path(temporary) / "test"
        unit.write_text(generate())
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-function", "-Wno-unused-parameter", str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        require(result.returncode == 0, f"log overflow latch failed:\n{result.stdout}\n{result.stderr}")
        count = re.search(r"(?m)^checks=(\d+)$", result.stderr)
        require(count is not None, "harness reports its check count")
        print(f"log overflow latch: {count.group(1)} checks passed")


if __name__ == "__main__":
    main()
