#!/usr/bin/env python3
"""A guarded file call normalizes its path once and hands the backend the path it always did.

Every guarded call (openGuarded, existsGuarded, removeGuarded, renameGuarded,
mkdirGuarded, rmdirGuarded, canWriteGuarded) runs normalizeFsPath, asks the
permission path on that text, and hands it to the backend dispatcher, which
normalized it again (VFS::normalize, a copy) and asked getStorageType, which
normalized it a third time. normalizeFsPath also made a trimmed copy of its
input first. Now normalizeFsPath reads its input in place, and normalize()'s
copy is skipped where the text is already what normalize() would return: a
normalizeFsPath result passes unless its last segment ends in a byte trim()
drops ("/a /" gives "/a ", which open still normalizes to "/a").

The production functions (System_Filesystem.cpp, System_VFS.cpp) run on the
real Arduino String with its heap calls counted; LittleFS, SD, the roster, the
lock and the event post are doubles that record every path they are handed.
The harness prints, for a generated corpus of about 5000 paths (every rule
prefix with tails, slashes, dots, spaces, tabs and case changes; token
sequences of up to three pieces; every byte 0x80-0xFF; paths at 127, 128, 255
and 256 bytes and one past; embedded NULs), normalizeFsPath's answer and
text, normalize, getStorageType, and for six identities each guarded call
with its answer and the backend calls with their paths. That report must
match the report of the tree before this change (REPORT_SHA256, its digest);
with --baseline <component dir> the old tree is built and run too and the two
reports are compared line by line. The harness also checks, in the same
binary, that the backend path is normalize(normalizeFsPath(path)), and counts
the String blocks per call.

Options: --cxx <compiler>, --sanitize, --component <dir> to run against
another components/hardwareone directory, --baseline <dir>, --print to write
the report to stdout.
"""
from __future__ import annotations

import argparse
import difflib
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

from arduino_string_host import arduino_string_counted
from test_fs_permission_strings import permission_pieces
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

# sha256 of the report on the host. Recorded first at 1cea1a79; changed
# 2026-11-21 only by the 1.0 user-rights rule (accounts below admin no longer
# append, write over, remove or rmdir on catch-all paths: 23 user columns of
# case and double-slash variants, checked with --baseline against 3076e0c6);
# changed 2026-11-22 only by the same rule on /espnow/ (M-espnowfs: the user
# and admin columns of the /espnow/ paths, checked with --baseline against
# d7522e16); changed 2026-11-22 only by B11's card-path rule (normalizeFsPath
# refuses a card path with a segment that ends in a dot or a space,
# System_SdPathCore.h: the 19 such paths of the corpus, checked with --baseline
# against 3eba119a).
REPORT_SHA256 = "60f7e1208e7588f6f2b88e721f2fe25e14224f53fbf8ae67f3465e758a196c68"

# Helpers the new code calls; an older tree has none of them.
OPTIONAL = ("static bool alreadyNormalized(", "static const String& normalizedView(")
VFS_FUNCTIONS = ("String normalize(const String& path)", "StorageType getStorageType(const String& path)",
                 "static bool exists(const String& path)", "static File open(const String& path,",
                 "static bool mkdir(const String& path)", "static void postFileDeleted(",
                 "static bool remove(const String& path)", "static bool rename(const String& pathFrom,",
                 "static bool rmdir(const String& path)", "static bool guardedNormalize(",
                 "static bool writePermitted(", "File openGuarded(", "bool canWriteGuarded(",
                 "bool existsGuarded(", "bool removeGuarded(", "bool renameGuarded(",
                 "bool mkdirGuarded(", "bool rmdirGuarded(")


def generate(component: Path) -> str:
    vfs = (component / "System_VFS.cpp").read_text()
    pieces = [extract_block(vfs, marker) for marker in OPTIONAL if marker in vfs]
    pieces += [extract_block(vfs, marker) for marker in VFS_FUNCTIONS]
    markers = permission_pieces(component)
    markers["// INSERT_VFS"] = "\n\n".join(pieces)
    harness = (HERE / "guarded_path_normalize_harness.cpp").read_text()
    for marker, value in markers.items():
        require(harness.count(marker) == 1, f"one {marker} marker")
        harness = harness.replace(marker, value)
    return harness


def run(component: Path, cxx: str, sanitize: list[str], d: Path, label: str) -> subprocess.CompletedProcess:
    (d / label).mkdir()
    includes, sources = arduino_string_counted(d / label, sanitize)
    unit, binary = d / label / "test.cpp", d / label / "test"
    unit.write_text(generate(component))
    subprocess.run([cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-Wno-sign-compare", *sanitize, *includes, str(unit), *sources, "-o", str(binary)],
                   check=True, timeout=240)
    return subprocess.run([str(binary)], capture_output=True, timeout=300)


def source_contract(component: Path) -> None:
    vfs = (component / "System_VFS.cpp").read_text()
    for marker in ("StorageType getStorageType(const String& path)", "static bool exists(const String& path)",
                   "static File open(const String& path,", "static bool mkdir(const String& path)",
                   "static bool remove(const String& path)", "static bool rmdir(const String& path)"):
        body = extract_block(vfs, marker)
        require("normalizedView(path, scratch)" in body and "normalize(path)" not in body,
                f"{marker} normalizes only a path that is not normalized yet")
    rename = extract_block(vfs, "static bool rename(const String& pathFrom,")
    require("normalizedView(pathFrom," in rename and "normalizedView(pathTo," in rename, "rename reads both in place")
    # The traversal defence stays on the text that is opened.
    for marker in ("static bool exists(const String& path)", "static File open(const String& path,",
                   "static bool mkdir(const String& path)", "static bool remove(const String& path)",
                   "static bool rmdir(const String& path)"):
        require('p.indexOf("..") >= 0' in extract_block(vfs, marker), f"{marker} still rejects '..'")
    fs = (component / "System_Filesystem.cpp").read_text()
    require("String trimmed" not in extract_block(fs, "bool normalizeFsPath("), "normalizeFsPath makes no trimmed copy")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--print", action="store_true", dest="print_report")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
    with tempfile.TemporaryDirectory(prefix="hw1-guarded-path-") as directory:
        d = Path(directory)
        current = run(args.component, args.cxx, sanitize, d, "current")
        baseline = run(args.baseline, args.cxx, sanitize, d, "baseline") if args.baseline else None
    sys.stderr.write(current.stderr.decode("latin-1"))
    if args.print_report:
        sys.stdout.buffer.write(current.stdout)
        sys.exit(current.returncode)
    lines = current.stdout.decode("latin-1").splitlines()
    if baseline is not None:
        sys.stderr.write("baseline: " + baseline.stderr.decode("latin-1"))
        old = baseline.stdout.decode("latin-1").splitlines()
        if old != lines:
            diff = difflib.unified_diff(old, lines, "baseline", "this tree", n=0, lineterm="")
            sys.stderr.write("\n".join(list(diff)[:60]) + "\n")
            raise AssertionError("the guarded path report differs from the baseline tree")
        print(f"baseline: {len(old)} report lines equal, digest {hashlib.sha256(baseline.stdout).hexdigest()}")
    digest = hashlib.sha256(current.stdout).hexdigest()
    require(digest == REPORT_SHA256,
            f"the guarded path report ({len(lines)} lines) has digest {digest}, not the recorded "
            f"{REPORT_SHA256}; run with --baseline <old component> to see the differing lines")
    require(current.returncode == 0, "the backend path or String block checks failed")
    source_contract(args.component)
    print(f"guarded path normalize: {len(lines)} report lines match, source contract passed")


if __name__ == "__main__":
    main()
