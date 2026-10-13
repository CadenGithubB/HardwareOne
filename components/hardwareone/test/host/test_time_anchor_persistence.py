#!/usr/bin/env python3
"""Execute the production time-anchor registry writers on the shared fake VFS.

The harness compiles System_TimeAnchors.cpp's registry and sweep (everything
after its includes: load, append, prune rewrite, promote pass and
timeAnchorsTick) with the real FsLockGuard and the real checked text writers
(writeText, writeTextAtomic) against fake_vfs.h. The clock, boot counter and
sensor-logging state are doubles. It checks that a failed or short prune
rewrite keeps the previous registry (and the pruned row in RAM), that a failed
or short append leaves the boot unanchored and a later tick records it, that a
fragment left by an interrupted append is never merged into the next row, and
that the sweep re-arms once a late anchor lands. Each case runs in a fresh
process. Power loss, LittleFS durability and the device clock sources are not
modelled.
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


def unique_block(source: str, marker: str) -> str:
    require(source.count(marker) == 1, f"marker is not unique: {marker}")
    return extract_block(source, marker)


def string_double() -> str:
    """The shared host String plus the String overload writeTextAtomic calls."""
    stub = (HERE / "web_batch_stubs" / "Arduino.h").read_text()
    anchor = "  bool concat(const char* text) {"
    require(stub.count(anchor) == 1, "one concat(const char*) in the String double")
    return stub.replace(anchor, "  bool concat(const String& other) { return concat(other.c_str()); }\n" + anchor)


def production() -> dict:
    anchors = (COMPONENT / "System_TimeAnchors.cpp").read_text()
    utils = (COMPONENT / "System_Utils.cpp").read_text()
    utils_header = (COMPONENT / "System_Utils.h").read_text()
    mutex = (COMPONENT / "System_Mutex.cpp").read_text()
    mutex_header = (COMPONENT / "System_Mutex.h").read_text()

    guard = extract_block(mutex_header, "struct FsLockGuard") + ";\n"
    guard += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()"))
    text_io = "\n\n".join(unique_block(utils, marker) for marker in ("bool writeText(", "bool writeTextAtomic("))

    # The module body: its anonymous namespace (registry, sweep) and the tick.
    head = "extern uint32_t gBootCounter;"
    require(anchors.count(head) == 1, "one gBootCounter declaration ahead of the module body")
    body = anchors[anchors.index(head) + len(head):]
    require(body.lstrip().startswith("namespace {"), "the module body opens with its anonymous namespace")
    module = extract_block(body, "namespace") + "  // namespace\n\n"
    module += unique_block(anchors, "void timeAnchorsTick()")
    return {
        "// INSERT_EVERY_MS": unique_block(utils_header, "inline bool everyMs("),
        "// INSERT_FS_GUARD": guard,
        "// INSERT_TEXT_IO": text_io,
        "// INSERT_TIME_ANCHORS": module,
    }


def source_contract() -> None:
    """Registry writes go through the checked writer; the latch follows the result."""
    anchors = (COMPONENT / "System_TimeAnchors.cpp").read_text()
    require("renameGuarded(tmpPath, kAnchorsPath" not in anchors,
            "the registry is not promoted by an unchecked rename")
    tick = unique_block(anchors, "void timeAnchorsTick()")
    require("writeAnchorLine()" in tick and "gAnchoredThisBoot = true;\n  }" not in tick,
            "the tick does not latch the anchor regardless of the append result")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", help="run one harness case by name")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    harness = (HERE / "time_anchor_persistence_harness.cpp").read_text()
    for marker, replacement in production().items():
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    with tempfile.TemporaryDirectory(prefix="hw1-time-anchor-") as temporary:
        directory = Path(temporary)
        (directory / "Arduino.h").write_text(string_double())
        unit = directory / "time_anchor_persistence.cpp"
        unit.write_text(harness)
        binary = directory / "time_anchor_persistence"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-parameter", "-Wno-unused-function",
                   "-I", str(directory), "-I", str(HERE), "-I", str(COMPONENT),
                   str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=180)
        cases = [args.case] if args.case else subprocess.run(
            [str(binary), "--list"], check=True, capture_output=True, text=True, timeout=30).stdout.split()
        require(bool(cases), "the harness lists its cases")
        failed = []
        for case in cases:
            if subprocess.run([str(binary), case], timeout=60).returncode != 0:
                failed.append(case)
        require(not failed, f"time anchor cases failed: {', '.join(failed)}")
    source_contract()


if __name__ == "__main__":
    main()
