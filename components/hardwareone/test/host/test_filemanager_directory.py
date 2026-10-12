#!/usr/bin/env python3
"""Exercise production FileManager cache/fallback traversal against directory fixtures.

The existing listing suite supplies Arduino/File/VFS boundary doubles. Real
FileManager declarations and methods are extracted unchanged; when present,
the shared production VFS directory reader is included as well. --baseline
selects a saved directory containing the pre-change FileManager .cpp/.h so
the same assertions can demonstrate the original defects.
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
from test_filesystem_psram_listing import directory_reader_source

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generate(component: Path) -> str:
    source = (component / "System_FileManager.cpp").read_text()
    header = (component / "System_FileManager.h").read_text()
    # Reuse boundary doubles only, never a copied directory traversal.
    listing = (HERE / "filesystem_psram_listing_harness.cpp").read_text()
    boundaries = listing[listing.index("static size_t checks = 0;"):
                         listing.index("// INSERT_PRODUCTION_LISTING_HERE")]
    # FileManager's existing path join also needs this ordinary String method.
    boundaries = boundaries.replace(
        "  String substring(size_t i) const",
        "  void remove(size_t i) { text_.erase(i); }\n  String substring(size_t i) const")
    boundaries = boundaries.replace("void close() {}", "void close() { node.reset(); }")
    # The selected-file read walks the file with the Arduino byte API.
    boundaries = boundaries.replace(
        "  size_t cursor = 0;\n  explicit operator bool() const { return !!node; }",
        "  size_t cursor = 0, readCursor = 0;\n"
        "  explicit operator bool() const { return !!node; }\n"
        "  bool available() const { return node && readCursor < node->bytes; }\n"
        "  int read() { return available() ? 'a' + int(readCursor++ % 3) : -1; }")
    declarations = re.sub(r"(?m)^#include[^\n]*\n", "", header)
    count_marker = "static uint16_t countFolderEntries("
    methods = [extract_block(source[source.rindex(count_marker):], count_marker)]
    methods += [extract_block(source, marker) for marker in (
        "String formatPath(",
        "FileManager::FileManager()", "bool FileManager::navigate(",
        "bool FileManager::refresh()", "bool FileManager::forceRescan()",
        "bool FileManager::navigateUp()", "bool FileManager::navigateInto()",
        "void FileManager::moveUp()", "void FileManager::moveDown()",
        "void FileManager::moveToTop()", "void FileManager::moveToBottom()",
        "bool FileManager::getItem(", "bool FileManager::getCurrentItem(",
        "int FileManager::getPageEnd()", "bool FileManager::loadDirectory()",
        "void FileManager::setVisibilityFilter(", "void FileManager::setCountFolderChildren(",
        "void FileManager::ensureValidSelection()",
        "bool FileManager::deleteItem()", "bool FileManager::renameItem(",
        "bool FileManager::readFile(")]
    # Operations on the selection resolve its full path when the source has it.
    selected_path = "bool FileManager::resolveSelectedPath(" in source
    if selected_path:
        methods += [extract_block(source, marker) for marker in (
            "bool FileManager::resolveSelectedPath(", "bool FileManager::getSelectedPath(",
            "bool FileManager::readSelectedFile(", "bool FileManager::readPath(")]
    constants = re.search(r"(?m)^static constexpr uint16_t kFolderCountCap[^\n]+", source)[0]
    reader, conversion = "", []
    if "VFS::DirectoryReader" in source:
        reader = directory_reader_source()
        conversion = [extract_block(source, marker) for marker in (
            "static FileEntry makeBrowserEntry(", "bool FileManager::isVisible(")]
    production = (f"#define TEST_DIRECTORY_READER {int(bool(reader))}\n"
                  f"#define TEST_SELECTED_PATH {int(selected_path)}\n" + reader +
                  declarations + "\n" + constants + "\n" + "\n\n".join(conversion + methods))
    harness = (HERE / "filemanager_directory_harness.cpp").read_text()
    for marker, replacement in {
        "// INSERT_BOUNDARIES": boundaries,
        "// INSERT_PRODUCTION": production,
    }.items():
        require(harness.count(marker) == 1, f"one insertion point required: {marker}")
        harness = harness.replace(marker, replacement)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--case", choices=("all", "virtual-boundary", "picker-beyond-cache", "compatibility", "selected-operations"), default="all")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-filemanager-") as directory:
        source, binary = Path(directory) / "test.cpp", Path(directory) / "test"
        source.write_text(generate(args.baseline or COMPONENT))
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", "-Wno-unused-variable", str(source), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(binary), args.case], check=True, timeout=60)


if __name__ == "__main__":
    main()
