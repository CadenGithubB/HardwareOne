#!/usr/bin/env python3
"""Exercise production ImageManager inventory against fault-injected FS boundaries.

ImageManager, VFS path routing/directory traversal, path validation, and the
filesystem lock guard are extracted unchanged. --baseline overlays a saved
component directory, making the original routing/locking/capacity bugs repeatable.
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
    declarations = re.sub(r"(?m)^#include[^\n]*\n", "", read("System_ImageManager.h"))
    mutex, mutex_header = read("System_Mutex.cpp"), read("System_Mutex.h")
    guard = extract_block(mutex_header, "struct FsLockGuard") + ";\n"
    guard += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()"))
    vfs, header = read("System_VFS.cpp"), read("System_VFS.h")
    paths = "namespace VFS {\n" + "\n".join(extract_block(vfs, marker) for marker in (
        "String normalize(", "StorageType getStorageType(", "String stripSdPrefix(",
        "size_t listVirtualEntries(")) + "\n}\n"
    reader = "namespace VFS {\n" + header[header.index("struct DirectoryEntry {"):
        header.index("bool exists(const String& path);")]
    reader += vfs[vfs.index("DirectoryReader::DirectoryReader("):vfs.index("static FS* fsForPath(")] + "\n}\n"
    markers = ["ImageManager::ImageManager()", "ImageManager::~ImageManager()",
               "bool ImageManager::init()", "bool ImageManager::initSD()",
               "String ImageManager::getMountPrefix(", "String ImageManager::getCaptureFolder(",
               "bool ImageManager::ensureCaptureFolder(", "std::vector<ImageInfo> ImageManager::listImages(",
               "int ImageManager::getImageCount(", "uint8_t* ImageManager::getImage(",
               "bool ImageManager::getImageInfo(", "bool ImageManager::deleteImage(",
               "bool ImageManager::deleteOldestImages(",
               "StorageStats ImageManager::getStorageStats("]
    # The capture-folder preparation sweeps stale photo temporaries once per tier.
    if "void ImageManager::sweepStaleTemporaries(" in source:
        markers.append("void ImageManager::sweepStaleTemporaries(")
    methods = "\n\n".join(extract_block(source, marker) for marker in markers)
    harness = (HERE / "image_storage_harness.cpp").read_text()
    for marker, value in {
        "// INSERT_FS_GUARD": guard,
        "// INSERT_NORMALIZE_FS_PATH": extract_block(read("System_Filesystem.cpp"), "bool normalizeFsPath("),
        "// INSERT_VFS_PATHS": paths,
        "// INSERT_DIRECTORY_READER": reader,
        "// INSERT_IMAGE_MANAGER": declarations + "\n" + methods,
    }.items():
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, value)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--case", choices=("all", "routing", "locking", "capacity"), default="all")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-image-storage-") as directory:
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
