#!/usr/bin/env python3
"""Source-contract guards for opt-in VFS capacity snapshots."""

from pathlib import Path


COMPONENT = Path(__file__).resolve().parents[2]


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for pos in range(brace, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : pos + 1]
    raise AssertionError(f"unterminated function: {signature}")


vfs_h = (COMPONENT / "System_VFS.h").read_text(encoding="utf-8")
vfs_cpp = (COMPONENT / "System_VFS.cpp").read_text(encoding="utf-8")
g2_cpp = (COMPONENT / "G2_Page_Files.cpp").read_text(encoding="utf-8")
utils_cpp = (COMPONENT / "System_Utils.cpp").read_text(encoding="utf-8")

# The compatibility surface stays intact. Caching is a separate, explicitly
# age-bounded API whose default still means fresh.
assert (
    "bool getStats(StorageType type, uint64_t& totalBytes, "
    "uint64_t& usedBytes, uint64_t& freeBytes);"
) in vfs_h
assert "bool getStatsSnapshot(StorageType type, CapacitySnapshot& out," in vfs_h
assert "uint32_t maxAgeMs = 0" in vfs_h
assert "void invalidateStatsSnapshot(StorageType type);" in vfs_h

legacy = function_body(vfs_cpp, "bool getStats(StorageType type")
assert "sampleCapacityFreshLocked(type)" in legacy
assert "readCapacitySnapshot" not in legacy
assert "publishCapacitySnapshot(type, generation" in legacy
assert "if (!result.apiOk) return false;" in legacy

snapshot = function_body(vfs_cpp, "bool getStatsSnapshot(StorageType type")
assert snapshot.count("readCapacitySnapshot(type, maxAgeMs, out)") == 2
assert "FsLockGuard guard(\"VFS.getStatsSnapshot\")" in snapshot
assert "sampleCapacityFreshLocked(type)" in snapshot
assert "if (!result.apiOk) return false" in snapshot
assert "publishCapacitySnapshot(type, generation" in snapshot

fresh = function_body(vfs_cpp, "static FreshCapacitySample sampleCapacityFreshLocked")
assert "const esp_err_t infoErr" in fresh
assert "result.apiOk = true" in fresh
assert "infoErr == ESP_OK" in fresh
assert "result.snapshot.totalBytes > 0" in fresh

invalidate = function_body(vfs_cpp, "void invalidateStatsSnapshot(StorageType type)")
assert "gCapacitySnapshots.invalidate(tier)" in invalidate
assert "portENTER_CRITICAL(&gCapacitySnapshotMux)" in invalidate
assert "portEXIT_CRITICAL(&gCapacitySnapshotMux)" in invalidate

# Every SD driver lifecycle transition invalidates the shared SD tier while it
# owns the filesystem transition lock.
for signature in (
    "static bool tryMountSD()",
    "bool unmountSD()",
    "bool remountSD()",
    "bool formatSD()",
):
    body = function_body(vfs_cpp, signature)
    assert "FsLockGuard guard(" in body
    assert "invalidateStatsSnapshot(SDCARD);" in body

# G2 is the only migrated consumer in this change. It retains the old 10-second
# browsing age, a zero-age forced refresh, and whole-sidebar mutation invalidation.
assert "static constexpr uint32_t kFilesStorageTtlMs = 10000;" in g2_cpp
g2_sample = function_body(g2_cpp, "static FilesStorageSnapshot getFilesStorageSnapshot")
assert "const uint32_t maxAgeMs = force ? 0 : kFilesStorageTtlMs;" in g2_sample
assert g2_sample.count("VFS::getStatsSnapshot(") == 2
g2_invalidate = function_body(g2_cpp, "static void invalidateFilesStorageSnapshot()")
assert "invalidateStatsSnapshot(VFS::INTERNAL)" in g2_invalidate
assert "invalidateStatsSnapshot(VFS::SDCARD)" in g2_invalidate
for retired_local_cache in (
    "gFilesStorageMux",
    "gFilesStorageInvalidateGen",
    "static FilesStorageSnapshot gFilesStorage",
):
    assert retired_local_cache not in g2_cpp

# Periodic system JSON is the other presentation consumer. Its zero-initialized
# local snapshots preserve the historical zero-on-failure JSON fields.
system_info = function_body(utils_cpp, "void buildSystemInfoJson(")
assert "static constexpr uint32_t kSystemInfoStatsMaxAgeMs = 5000;" in system_info
assert system_info.count("VFS::getStatsSnapshot(") == 2
assert system_info.count("VFS::CapacitySnapshot stats = {};") == 2

# Explicit command diagnostics remain authoritative even though periodic
# system-info projection is cached. Mount output is included because it reports
# the just-mounted card's capacity and must not inherit an older sample.
for source, signature, expected_tier in (
    (utils_cpp, "const char* cmd_fsusage(", "VFS::INTERNAL"),
    (vfs_cpp, "static const char* cmd_sdmount(", "VFS::SDCARD"),
    (vfs_cpp, "static const char* cmd_sdinfo(", "VFS::SDCARD"),
):
    diagnostic = function_body(source, signature)
    assert f"VFS::getStats({expected_tier}" in diagnostic
    assert "getStatsSnapshot" not in diagnostic

filesystem_cpp = (COMPONENT / "System_Filesystem.cpp").read_text(encoding="utf-8")
logtier = function_body(filesystem_cpp, "static const char* cmd_logtier(")
assert "VFS::getStats(VFS::INTERNAL" in logtier
assert "VFS::getStats(VFS::SDCARD" in logtier
assert "getStatsSnapshot" not in logtier

# Capacity/admission call sites remain on the authoritative legacy API.
for relative in (
    "WebServer_Server.cpp",
    "System_SensorLogging.cpp",
    "System_ImageManager.cpp",
):
    source = (COMPONENT / relative).read_text(encoding="utf-8")
    assert "VFS::getStats(" in source
    assert "VFS::getStatsSnapshot(" not in source

# Keep the migration set reviewable: no production caller outside the two
# explicitly approved presentation projections may opt into staleness.
for path in COMPONENT.glob("*.cpp"):
    if path.name in {"G2_Page_Files.cpp", "System_Utils.cpp"}:
        continue
    assert "VFS::getStatsSnapshot(" not in path.read_text(encoding="utf-8"), path

# The specialized overflow cache remains separate from presentation snapshots.
overflow = vfs_cpp[
    vfs_cpp.index("static bool           gLogOverflowActive") :
    vfs_cpp.index("bool unmountSD()")
]
assert "gLogFreeCheckCached" in overflow
assert "getStatsSnapshot" not in overflow
assert "gCapacitySnapshots" not in overflow

print("VFS capacity snapshot source guards passed")
