#!/usr/bin/env python3
"""Exercise production image saves with per-tier admission and write faults.

The save method, filename generation, capture-folder owner, path helpers and
FsLockGuard are extracted unchanged. File/VFS, clock/events and retention are
boundaries. --baseline overlays a saved component directory for reproductions.
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


def generate(baseline: Path | None) -> str:
    def read(name: str) -> str:
        path = baseline / name if baseline and (baseline / name).exists() else COMPONENT / name
        return path.read_text()

    source = read("System_ImageManager.cpp")
    header = re.sub(r"(?m)^#include[^\n]*\n", "", read("System_ImageManager.h"))
    mutex, mutex_header = read("System_Mutex.cpp"), read("System_Mutex.h")
    guard = extract_block(mutex_header, "struct FsLockGuard") + ";\n"
    guard += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()"))
    # Reuse only the established String/RTOS boundary, never image-save logic.
    inventory = (HERE / "image_storage_harness.cpp").read_text()
    boundaries = inventory[inventory.index("static size_t checks = 0;"):
                           inventory.index("// INSERT_FS_GUARD")]
    boundaries = boundaries.replace("  String& operator+=(const String& value)",
        "  bool concat(const String& value) { if (++concatCalls == failConcatCall) return false; "
        "value_ += value.value_; return true; }\n  String& operator+=(const String& value)")
    vfs, vfs_header = read("System_VFS.cpp"), read("System_VFS.h")
    paths = "namespace VFS {\n" + "\n".join(extract_block(vfs, marker) for marker in (
        "String normalize(", "StorageType getStorageType(", "String stripSdPrefix(",
        "size_t listVirtualEntries(")) + "\n}\n"
    # The stale-temporary sweep walks the folder with the shared directory reader.
    reader = "namespace VFS {\n" + vfs_header[vfs_header.index("struct DirectoryEntry {"):
                                              vfs_header.index("bool exists(const String& path);")]
    reader += vfs[vfs.index("DirectoryReader::DirectoryReader("):vfs.index("static FS* fsForPath(")] + "\n}\n"
    markers = ["ImageManager::ImageManager()", "ImageManager::~ImageManager()",
               "bool ImageManager::init()", "bool ImageManager::initSD()",
               "String ImageManager::getMountPrefix(", "String ImageManager::getCaptureFolder(",
               "bool ImageManager::ensureCaptureFolder(", "String ImageManager::generateFilename(",
               "String ImageManager::saveImage("]
    validator = "const char* ImageManager::captureFolderRejectReason(" in source
    if validator:
        markers.append("const char* ImageManager::captureFolderRejectReason(")
    sweep = "void ImageManager::sweepStaleTemporaries(" in source
    if sweep:
        markers.append("void ImageManager::sweepStaleTemporaries(")
    methods = (f"#define HAS_FOLDER_VALIDATOR {int(validator)}\n"
               f"#define HAS_STALE_SWEEP {int(sweep)}\n" + "\n\n".join(
        extract_block(source, marker) for marker in markers))
    constant = re.search(r"(?m)^#define MIN_FREE_SPACE_BYTES[^\n]+", source)[0]
    harness = (HERE / "image_writes_harness.cpp").read_text()
    for marker, value in {
        "// INSERT_BOUNDARIES": boundaries,
        "// INSERT_FS_GUARD": guard,
        "// INSERT_NORMALIZE_FS_PATH": extract_block(read("System_Filesystem.cpp"), "bool normalizeFsPath("),
        "// INSERT_VFS_PATHS": paths,
        "// INSERT_DIRECTORY_READER": reader,
        "// INSERT_IMAGE_MANAGER": header + "\n" + constant + "\n" + methods,
    }.items():
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, value)
    return f"#define HAS_TEMP_SAVE {int('VFS::replaceFileGuarded(' in source)}\n" + harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--case", choices=("all", "short", "folder", "capacity", "overflow", "collision"), default="all")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-image-writes-") as directory:
        source, binary = Path(directory) / "test.cpp", Path(directory) / "test"
        source.write_text(generate(args.baseline))
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", "-Wno-unused-parameter", str(source), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(binary), args.case], check=True, timeout=30)


if __name__ == "__main__":
    main()
