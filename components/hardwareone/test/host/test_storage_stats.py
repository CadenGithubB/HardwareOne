#!/usr/bin/env python3
"""Exercise production VFS capacity queries and the shared files-stats JSON builder.

Sampling, cache integration, the log writer's free-space check (which seeds the
status snapshot), path routing, JSON formatting and FsLockGuard are extracted
unchanged; the cache core is the real production header. Arduino
String, driver results, time and RTOS synchronization are host boundaries.
--baseline overlays saved component sources to reproduce old false successes.

The files-stats JSON has two entry points over one formatter: the web Files
page's (buildFilesStatsJson: the shared internal reading when it is recent, no
LittleFS walk per folder load; SD read now) and the `files stats` command's
(buildFilesStatsJsonFresh: always read now). Both are extracted with the
formatter, its age limit and the save hook that marks the reading stale. A
saved tree from before the split has one builder; it stands in for both, so
--baseline --case regression still runs (the cases of the split then fail, as
they should).

Source rules for the status figure (not run with --baseline): without an
internal reading buildSystemInfoJson (status json, /api/system, SSE) leaves
total_kb, used_kb and free_kb out and keeps the storage object, and the
dashboard row then reads "unavailable" instead of "0 / 0 KB".
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

# getStorageType reads an already normalized path in place through these
# (an older tree has neither).
NORMALIZED_VIEW = ("static bool alreadyNormalized(", "static const String& normalizedView(")

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

WEB_ENTRY = "void buildFilesStatsJson("
FRESH_ENTRY = "void buildFilesStatsJsonFresh("
FORMATTER = "static void filesStatsJson("
SAVE_HOOK = "void runFileWritePostSaveHooks("
# The save hook's automation part is compiled out here (ENABLE_AUTOMATION 0).
OLD_TREE_SHIM = """
static constexpr uint32_t kFilesStatsInternalMaxAgeMs = 10000;
static void buildFilesStatsJsonFresh(const String& path, char* out, size_t outSize) {
  buildFilesStatsJson(path, out, outSize);
}
static void runFileWritePostSaveHooks(const String&) {}
"""


def json_builders(filesystem: str) -> str:
    """The files-stats entry points as the harness compiles them."""
    if FRESH_ENTRY not in filesystem:  # a saved tree from before the split
        return extract_block(filesystem, WEB_ENTRY) + OLD_TREE_SHIM
    limit = re.search(r"(?m)^static constexpr uint32_t kFilesStatsInternalMaxAgeMs = \d+;$", filesystem)
    require(limit is not None, "the Files page's age limit for the internal reading")
    require(filesystem.count(FORMATTER) == 1, "one files-stats formatter")
    for name in ("cmd_mkdir", "cmd_rmdir"):
        body = extract_block(filesystem, f"const char* {name}(")
        mark = "if (VFS::getStorageType(path) != VFS::SDCARD) VFS::invalidateStatsSnapshot(VFS::INTERNAL);"
        require(body.count(mark) == 1, f"{name} marks the internal reading stale once")
        require(body.index("Guarded(path, ctx)") < body.index(mark) < body.rindex("snprintf("),
                f"{name} marks the reading only after the folder was changed")
    command = extract_block(filesystem, "const char* cmd_files(")
    require("buildFilesStatsJsonFresh(path, statsBuf, sizeof(statsBuf));" in command
            and "buildFilesStatsJson(" not in command, "`files stats` reads now")
    require("invalidateStatsSnapshot" not in extract_block(filesystem, "const char* cmd_filecreate("),
            "filecreate has no mark (an empty file takes no data block; its harness has no stub)")
    return "\n\n".join([limit.group(0)] + [extract_block(filesystem, marker)
                                             for marker in (FORMATTER, WEB_ENTRY, FRESH_ENTRY, SAVE_HOOK)])


DASHBOARD_ROW = ("if(d.storage){if(d.storage.used_kb!=null){var usedTxt=d.storage.used_kb;"
                 "if(d.storage.total_kb!=null)usedTxt+=' / '+d.storage.total_kb+' KB';"
                 "window.Dash.setText('sys-storage-used',usedTxt)}"
                 "else{window.Dash.setText('sys-storage-used','unavailable')}")


def status_figure_rules() -> None:
    """A failed internal reading is 'unavailable' on the dashboard, not 0 / 0 KB."""
    info = extract_block((COMPONENT / "System_Utils.cpp").read_text(), "void buildSystemInfoJson(")
    made = info.find('JsonObject storage = doc["storage"].to<JsonObject>();')
    read_at = info.find("const bool haveInternal =\n        VFS::getStatsSnapshot(VFS::INTERNAL, stats,")
    require(0 <= made < read_at, "the storage object is made before the internal reading, so it stays without one")
    guarded = extract_block(info[read_at:], "if (haveInternal)")
    for key in ("total_kb", "used_kb", "free_kb"):
        field = f'storage["{key}"]'
        require(info.count(field) == 1 and field in guarded,
                f"{key} is written only with an internal reading (no 0 for a failed read)")
    # The page keeps its last figure when a reply has no storage object at all
    # (the Bluetooth-only updates), so the text is set inside if(d.storage).
    page = (COMPONENT / "WebPage_Dashboard.h").read_text()
    require(page.count(DASHBOARD_ROW) == 1,
            "the dashboard storage row shows 'unavailable' when storage has no used_kb")


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

    if args.baseline is None:
        status_figure_rules()
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
        "String normalize(", *[m for m in NORMALIZED_VIEW if m in vfs], "StorageType getStorageType(",
        "static FreshCapacitySample sampleCapacityFreshLocked(",
        "bool getStats(", "bool getStatsSnapshot(", "void invalidateStatsSnapshot("))
    # The log writer's free-space cache, whose walk also seeds the INTERNAL
    # snapshot that status polls read.
    reserve = re.search(r"(?m)^static constexpr size_t LOG_OVERFLOW_DEFAULT_RESERVE[^\n]*$", vfs)
    require(reserve is not None, "log reserve constant")
    free_start = re.search(r"(?m)^static unsigned long\s+gLogFreeCheckLastMs", vfs)
    free_end = "size_t getCachedLittleFsFree() { return refreshLittleFsFreeCached(); }"
    require(free_start is not None and free_end in vfs, "log free-space cache region")
    definitions += "\n" + reserve.group(0) + "\n"
    definitions += vfs[free_start.start():vfs.index(free_end) + len(free_end)] + "\n"
    harness = (HERE / "storage_stats_harness.cpp").read_text()
    for marker, replacement in {
        "// INSERT_FS_GUARD": guard,
        "// INSERT_STORAGE_TYPE": extract_block(header, "enum StorageType") + ";",
        "// INSERT_VFS": definitions,
        "// INSERT_JSON_BUILDER": json_builders(read("System_Filesystem.cpp")),
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
