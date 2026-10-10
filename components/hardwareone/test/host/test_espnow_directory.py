#!/usr/bin/env python3
"""Run the production deferred ESP-NOW listing handler and VFS directory reader.

Wire declarations and the complete handler are extracted unchanged. Reuse the
listing suite's Arduino/File/VFS boundary doubles; mock transport, identity,
permission results and allocation to inspect replies and error cleanup. This
does not emulate radio delivery or replace the separate ACL contract tests.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require
from test_filesystem_psram_listing import directory_reader_source

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generate() -> str:
    listing = (HERE / "filesystem_psram_listing_harness.cpp").read_text()
    boundaries = listing[listing.index("static size_t checks = 0;"):
                         listing.index("// INSERT_PRODUCTION_LISTING_HERE")]
    wire = (COMPONENT / "System_ESPNow_Wire.h").read_text()
    declarations = wire[wire.index("struct __attribute__((packed)) V4PayloadFsListReq {"):
                        wire.index("// ---- FS STAT request/reply")]
    source = (COMPONENT / "System_ESPNow_FsList.cpp").read_text()
    marker = "static void processListDeferred("
    handler = extract_block(source[source.rindex(marker):], marker)
    harness = (HERE / "espnow_directory_harness.cpp").read_text()
    for marker, replacement in {
        "// INSERT_BOUNDARIES": boundaries,
        "// INSERT_PRODUCTION_TYPES": declarations + directory_reader_source(),
        "// INSERT_PRODUCTION_HANDLER": handler,
    }.items():
        require(harness.count(marker) == 1, f"one insertion point required: {marker}")
        harness = harness.replace(marker, replacement)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-espnow-directory-") as directory:
        source, binary = Path(directory) / "test.cpp", Path(directory) / "test"
        source.write_text(generate())
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", "-Wno-unused-variable", str(source), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
