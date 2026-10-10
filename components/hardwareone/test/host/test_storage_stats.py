#!/usr/bin/env python3
"""Exercise production VFS capacity queries and the shared files-stats JSON builder.

Sampling, cache integration, path routing, JSON formatting and FsLockGuard are
extracted unchanged; the cache core is the real production header. Arduino
String, driver results, time and RTOS synchronization are host boundaries.
--baseline overlays saved component sources to reproduce old false successes.
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
    parser.add_argument("--baseline", type=Path, help="saved component directory")
    parser.add_argument("--case", choices=("all", "regression"), default="all")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")

    def read(name: str) -> str:
        candidate = args.baseline / name if args.baseline else COMPONENT / name
        return (candidate if candidate.is_file() else COMPONENT / name).read_text()

    vfs = read("System_VFS.cpp")
    header = read("System_VFS.h")
    mutex = read("System_Mutex.cpp")
    guard = extract_block(read("System_Mutex.h"), "struct FsLockGuard") + ";\n"
    guard += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()"))
    definitions = vfs[vfs.index("static detail::CapacitySnapshotCache gCapacitySnapshots;"):
                      vfs.index("// Why both flags exist:")]
    definitions += "\n" + extract_block(vfs, "struct FreshCapacitySample") + ";\n"
    definitions += "\n\n".join(extract_block(vfs, marker) for marker in (
        "String normalize(", "StorageType getStorageType(",
        "static FreshCapacitySample sampleCapacityFreshLocked(",
        "bool getStats(", "bool getStatsSnapshot(", "void invalidateStatsSnapshot("))
    harness = (HERE / "storage_stats_harness.cpp").read_text()
    for marker, replacement in {
        "// INSERT_FS_GUARD": guard,
        "// INSERT_STORAGE_TYPE": extract_block(header, "enum StorageType") + ";",
        "// INSERT_VFS": definitions,
        "// INSERT_JSON_BUILDER": extract_block(read("System_Filesystem.cpp"), "void buildFilesStatsJson("),
    }.items():
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, replacement)
    with tempfile.TemporaryDirectory(prefix="hw1-storage-stats-") as directory:
        source, executable = Path(directory) / "test.cpp", Path(directory) / "test"
        source.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-parameter", "-Wno-unused-function", "-I", str(COMPONENT),
                   str(source), "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(executable), args.case], check=True, timeout=30)


if __name__ == "__main__":
    main()
