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

from test_mqtt_boolean_commands import extract_macro
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
               "bool ImageManager::deleteImage(", "bool ImageManager::deleteOldestImages(",
               "StorageStats ImageManager::getStorageStats("]
    # The capture-folder preparation sweeps stale photo temporaries once per tier.
    if "void ImageManager::sweepStaleTemporaries(" in source:
        markers.append("void ImageManager::sweepStaleTemporaries(")
    # SD availability asked of VFS per operation rather than latched at init.
    if "bool ImageManager::tierAvailable(" in source:
        markers += ["bool ImageManager::isSDAvailable() const", "bool ImageManager::tierAvailable("]
    # The owner's managed-photo rule, shared by the reader and the deleter.
    if "bool ImageManager::managedImagePath(" in source:
        markers.append("bool ImageManager::managedImagePath(")
    methods = "\n\n".join(extract_block(source, marker) for marker in markers)
    filesystem, command = read("System_Filesystem.cpp"), read("System_Command.cpp")
    parser = extract_block(read("System_Command.h"), "class CommandArgs") + ";\n"
    parser += "const String CommandArgs::empty_;\n"
    parser += "\n".join(extract_block(command, marker) for marker in (
        "CommandArgs::CommandArgs(", "void CommandArgs::parse(", "const String& CommandArgs::arg("))
    parser += "\n" + extract_block(filesystem, "const char* requireQuotedToken(")
    parser += "\n" + extract_macro(read("System_Utils.h"), "RETURN_VALID_IF_VALIDATE_CSTR") + "\n"
    # The image and recording list commands; the recordings command is camera
    # owned and, before it used the debug buffer, kept its own text buffer.
    commands = "\n".join(extract_block(source, marker) for marker in (
        "const char* cmd_capture(", "const char* cmd_images(", "const char* cmd_imagedelete("))
    camera = read("System_Camera_DVP.cpp")
    old_buffer = re.search(r"(?m)^EXT_RAM_BSS_ATTR static char gCameraCmdBuffer\[\d+\];", camera)
    commands += "\n" + (old_buffer[0] if old_buffer else "") + "\n"
    commands += extract_block(camera, "const char* cmd_cameravideolist(")
    # One shared "serialize a list into the debug buffer" helper, when present.
    debug_json = baseline / "System_DebugJson.h" if baseline and (baseline / "System_DebugJson.h").exists() \
        else COMPONENT / "System_DebugJson.h"
    debug_json = extract_block(debug_json.read_text(), "inline const char* serializeJsonListToDebugBuffer(") \
        if debug_json.exists() else ""
    harness = (HERE / "image_storage_harness.cpp").read_text()
    for marker, value in {
        "// INSERT_DEBUG_JSON": debug_json,
        "// INSERT_ARG_WANTS_JSON": extract_block(read("System_Utils.cpp"), "bool argWantsJson("),
        "// INSERT_FS_GUARD": guard,
        "// INSERT_COMMAND_PARSER": parser,
        "// INSERT_NORMALIZE_FS_PATH": extract_block(filesystem, "bool normalizeFsPath("),
        "// INSERT_VFS_PATHS": paths,
        "// INSERT_DIRECTORY_READER": reader,
        "// INSERT_IMAGE_MANAGER": declarations + "\n" + methods,
        "// INSERT_IMAGE_COMMANDS": commands,
    }.items():
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, value)
    return harness


def delete_takes_ctx(baseline: Path | None) -> bool:
    """True when deleteImage takes the caller's AuthContext (saved sources may predate it)."""
    path = baseline / "System_ImageManager.h" if baseline and (baseline / "System_ImageManager.h").exists() \
        else COMPONENT / "System_ImageManager.h"
    return "deleteImage(const String& path, const AuthContext& ctx)" in path.read_text()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--case", choices=("all", "mount", "commands", "routing", "scope", "locking", "capacity"),
                        default="all")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-image-storage-") as directory:
        source, binary = Path(directory) / "test.cpp", Path(directory) / "test"
        source.write_text(generate(args.baseline))
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", "-Wno-unused-parameter",
                   f"-DIMAGE_DELETE_TAKES_CTX={int(delete_takes_ctx(args.baseline))}",
                   "-I", str(COMPONENT.parent / "hardwareone_libs/ArduinoJson/src"),
                   str(source), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(binary), args.case], check=True, timeout=30)


if __name__ == "__main__":
    main()
