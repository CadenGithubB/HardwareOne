#!/usr/bin/env python3
"""Log retention: what may go when a capture volume runs low, and in what order.

System_LogRetentionCore.h (the rule, its constants and the bounded step
machine) is compiled as it is and run against an in-memory volume:

- the water marks (the P4X-EYE's 9068 KiB flash prunes below 1.81 MB free and
  stops at 2.27 MB; a small flash uses two log reserves; a large card is
  capped), and the low/high latch: a pass is wanted only below the low-water
  mark, then until the high one;
- twenty stored day folders: secondary files go oldest first, one day a pass;
  the three newest stored days keep theirs; the health files of the fourteen
  newest stored days always stay, and the older ones go only when nothing
  secondary is left;
- never removed: the open session and its rotations, today's folder and
  folders dated after today, dot files (.anchors.csv), .tmp and .bak files,
  files at the root of the sensors folder, sub-folders of a day, every
  transcript and note under stt/ (B12), and everything outside the capture
  tree;
- boot-named folders: earlier boots without an anchor only, lowest first,
  never their health files;
- the "recent" day horizon is at least the catch-up ledger's (the files of
  the three newest day folders it offers a peer), read from
  System_CatchupLedger.cpp;
- the card's tree under /sd by the same rule; a volume with nothing to remove
  ends the pass empty-handed;
- one step lists at most kEntriesPerStep entries (plus the one-entry look at a
  folder that may be empty) and removes at most kRemovalsPerStep files; no
  listing goes past kWalkMaxEntries entries of a folder; every pass ends.

The tick that steps the pass on the device, with the real directory listing on
the fake VFS, is in test_time_anchor_persistence.py.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --case <name>.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def source_contract() -> None:
    """The core stays pure; the tick owns the device side and the pace."""
    core = (COMPONENT / "System_LogRetentionCore.h").read_text()
    for include in re.findall(r'^#include\s+[<"]([^>"]+)[>"]', core, re.M):
        require(include in ("stddef.h", "stdint.h", "stdio.h", "string.h"),
                f"System_LogRetentionCore.h includes {include}: it must stay free of Arduino and ESP-IDF headers")
    for constant in ("kProtectedDays = 14", "kRecentDaysKept = 3",
                     "kLowWaterPercent = 20", "kLowWaterReserveMultiple = 2", "kEntriesPerStep = 16",
                     "kRemovalsPerStep = 1", "kWalkMaxEntries = 512"):
        require(f"constexpr unsigned {constant};" in core, f"the named constant {constant}")
    # A file the catch-up ledger may have offered to a peer is not pruned
    # before the peer could fetch it: the recent horizons cover the ledger's.
    ledger = (COMPONENT / "System_CatchupLedger.cpp").read_text()
    day_folders = re.search(r"constexpr uint8_t kSensorDayFolders = (\d+),", ledger)
    require(bool(day_folders), "the catch-up ledger names its day horizon")
    require(int(day_folders.group(1)) <= 3, "kRecentDaysKept covers the ledger's day folders")
    # B12: transcripts are exempt. The core never names the transcript tree
    # and the device side holds no transcript scope.
    require("/stt" not in re.sub(r"//[^\n]*", "", core), "the retention pass never looks under stt/")
    anchors = (COMPONENT / "System_TimeAnchors.cpp").read_text()
    code = re.sub(r"//[^\n]*", "", anchors)
    require("Scopes::TRANSCRIPTS" not in code, "the retention pass holds no transcript scope (B12)")
    require("EXT_RAM_BSS_ATTR static LogRetention::Pass gRetentionPass;" in code,
            "the pass object (with its listing scratch) is explicit PSRAM")
    # Free space: the internal flash only by its cached figure (a fresh one
    # walks the volume); the card by one authoritative reading per decision.
    require(code.count("VFS::peekStatsSnapshot(VFS::INTERNAL, capacity)") == 1 and
            code.count("VFS::peekStatsSnapshot(") == 1,
            "the internal flash is read through the cached figure only")
    require(code.count("VFS::getStats(") == 1 and "VFS::getStats(VFS::SDCARD, total, used, freeBytes)" in code,
            "the only fresh reading is the card's")
    require("getStatsSnapshot(" not in code and "getCachedLittleFsFree" not in code,
            "no other free-space source")
    figure = code[code.index("static bool retentionFreeFigure("):code.index("static bool retentionConsiderPass(")]
    require(figure.index("if (card) {") < figure.index("VFS::getStats(") < figure.index("VFS::peekStatsSnapshot("),
            "the fresh reading is on the card branch")
    require("LogRetention::kFreeFigureMaxAgeMs" in figure, "an old cached figure is not acted on")
    require("LogRetention::kFreeFigureRetryMs" in code, "a decision without a figure is retried soon")
    # The listing names entries; it opens none of them.
    port = code[code.index("struct RetentionVfsPort final"):code.index("static uint32_t dayKeyOf(")]
    require("getNextFileName(&isDirectory)" in port and "openNextFile" not in port,
            "the retention listing does not open the entries it passes")
    tick = code[code.index("void timeAnchorsTick()"):]
    require(tick.index("if (!Clock::isSynced()) return;") < tick.index("retentionStep();"),
            "the retention step needs a set clock")
    require(tick.index('FsLockGuard tickGuard("timeAnchors.tick", 0);') < tick.index("retentionStep();"),
            "the retention step runs under the tick's try-lock")
    require(code.count("logSystemEvent(\"STORAGE\"") == 1, "one system-log line per pass that removed something")
    # The pass is not run from boot code: only the main-loop tick steps it.
    for path in COMPONENT.glob("*.cpp"):
        if path.name == "System_TimeAnchors.cpp":
            continue
        require("LogRetention::" not in path.read_text(encoding="utf-8", errors="replace"),
                f"{path.name} drives the retention pass; only timeAnchorsTick does")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--case", help="run one harness case by name")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-log-retention-") as temporary:
        binary = Path(temporary) / "log_retention"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-I", str(COMPONENT), str(HERE / "log_retention_harness.cpp"), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=180)
        subprocess.run([str(binary)] + ([args.case] if args.case else []), check=True, timeout=60)
    source_contract()


if __name__ == "__main__":
    main()
