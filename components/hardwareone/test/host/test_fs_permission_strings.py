#!/usr/bin/env python3
"""The file permission path answers as before and makes no String on the way.

Every guarded file access asks checkPerm. It used to build Strings on the way:
lookupRule wrapped each rule prefix it tried in a String (String::startsWith
of a const char*), checkPerm looked the rule up twice, the sensitive-extension
and image tests each lower-cased a copy of the path (and the first one copied
the base name) before the role was asked, though system and super ignore both
answers, and pathWithinScope copied the scope twice. VFS::systemAuth made the
"system:<reason>" text as a sum and then a copy of it. Now the rule prefixes
and the scope are compared in place, the filename tests read the path in place
and run only for a role they apply to, and systemAuth makes its text in one
block.

The production table and functions (System_Filesystem.cpp, System_VFS.cpp)
run on the real Arduino String with its heap calls counted; the roster, the FS
lock and the debug queue are doubles. The harness prints, for a path set that
covers every rule of the table (the prefix itself, below it, a trailing slash
added or taken away, an upper-case spelling, "..", sensitive and image names),
plus the empty path, the root, paths at the length limits and paths with an
embedded NUL: the matched rule, both filename tests, isAdminOnlyPath and, for
eleven identities, the six canX answers, getPermissions and the reason
logFsAccessDeny gives for each refusal; then a pathWithinScope matrix and the
contexts systemAuth builds. That report must equal fs_permission_answers.txt,
which the tree before the change printed (one line differs since B11,
2026-11-22: normalizeFsPath refuses a card path in a spelling FatFs would
resolve to another name, so getPermissions now answers 0 for "/sd/STT/u2" for
the super admin and the system too). The harness then checks that no
permission query made a String block and that systemAuth made one per kept
text.

Options: --cxx <compiler>, --sanitize, --component <dir> to run against
another components/hardwareone directory, --print to write the report to
stdout instead of comparing it.
"""
from __future__ import annotations

import argparse
import difflib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from arduino_string_host import arduino_string_counted
from identity_names_source import identity_constants, identity_predicates
from test_web_batch_handlers import extract_block, require
from sd_path_core_host import sd_path_core, with_sd_path_core

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
ANSWERS = HERE / "fs_permission_answers.txt"

# Helpers the new bodies call; an older tree has none of them.
OPTIONAL = ("static char asciiLower(", "static bool lowerEndsWith(", "static bool lowerBaseNameContains(")


def permission_pieces(component: Path) -> dict[str, str]:
    """The production text for the harness markers: the transcript path policy,
    the identity names and AuthContext, and the permission path with both
    systemAuth forms (also used by test_guarded_path_normalize.py)."""
    fs = (component / "System_Filesystem.cpp").read_text()
    fs_h = (component / "System_Filesystem.h").read_text()
    user_h = (component / "System_User.h").read_text()
    vfs = (component / "System_VFS.cpp").read_text()
    policy = (component / "System_TranscriptPathPolicy.h").read_text().replace("#pragma once", "")

    bond = re.search(r'^inline constexpr const char\* kBondAdminUser = "[^"\n]*";$', user_h, re.M)
    require(bond is not None, "System_User.h names kBondAdminUser")
    identity = "\n\n".join([identity_constants(user_h), bond.group(0),
                            extract_block(user_h, "struct AuthContext") + ";",
                            identity_predicates(user_h)])

    # The state-file rows name their paths through System_StatePaths.h on a tree that has it.
    paths = component / "System_StatePaths.h"
    owner = paths.read_text().replace("#pragma once\n", "", 1) if paths.exists() else ""
    pieces = [extract_block(fs_h, "enum FilePermission") + ";", owner,
              extract_block(fs, "struct PathRule") + ";",
              extract_block(fs, "static const PathRule sPathRules[]") + ";"]
    pieces += [extract_block(fs, marker) for marker in OPTIONAL if marker in fs]
    for marker in ("static const PathRule& lookupRule(", "static bool hasSensitiveExtension(",
                   "static bool isImageFile(", "bool normalizeFsPath(", "enum class FsRole",
                   "static inline bool isUnrestrictedRole(", "static FsRole resolveRole(",
                   "static const char* roleName(", "static uint8_t permsForRole(",
                   "static bool pathWithinScope(", "static uint32_t transcriptAccountId(",
                   "static uint8_t transcriptPermissionMask(", "static uint8_t permissionsForResolvedRole(",
                   "static bool checkPerm(", "bool canRead ", "bool canEdit ", "bool canDelete ",
                   "bool canRename ", "bool canCreate ", "bool canImport ", "void logFsAccessDeny(",
                   "uint8_t getPermissions(", "bool isAdminOnlyPath("):
        pieces.append(extract_block(fs, marker) + (";" if marker.startswith("enum") else ""))
    pieces.append("namespace VFS {\n" + extract_block(vfs, "AuthContext systemAuth(const char* reason)")
                  + "\n\n" + extract_block(vfs, "AuthContext systemAuth(const char* scope, const char* reason)")
                  + "\n}  // namespace VFS")
    # normalizeFsPath refuses aliased card paths through System_SdPathCore.h (B11).
    if (component / "System_SdPathCore.h").exists():
        policy += "\n" + sd_path_core(component)
    return {"// INSERT_POLICY": policy, "// INSERT_IDENTITY": identity,
            "// INSERT_PRODUCTION": "\n\n".join(pieces)}


def generate(component: Path) -> str:
    harness = (HERE / "fs_permission_strings_harness.cpp").read_text()
    for marker, value in permission_pieces(component).items():
        require(harness.count(marker) == 1, f"one {marker} marker")
        harness = harness.replace(marker, value)
    return harness


def source_contract(component: Path) -> None:
    fs = (component / "System_Filesystem.cpp").read_text()
    check_perm = extract_block(fs, "static bool checkPerm(")
    # One rule lookup; the filename tests are asked only for a restricted role.
    require(check_perm.count("lookupRule(") == 1, "checkPerm looks the rule up once")
    require("!isUnrestrictedRole(role) && hasSensitiveExtension(path)" in check_perm,
            "checkPerm asks the role before the sensitive-extension test")
    require("!isUnrestrictedRole(role) && isImageFile(path)" in check_perm,
            "checkPerm asks the role before the image test")
    deny = extract_block(fs, "void logFsAccessDeny(")
    require("!isUnrestrictedRole(role) && hasSensitiveExtension(path)" in deny
            and "!isUnrestrictedRole(role) && isImageFile(path)" in deny,
            "logFsAccessDeny asks the role before the filename tests")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    parser.add_argument("--print", action="store_true", dest="print_report")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
    with tempfile.TemporaryDirectory(prefix="hw1-fs-permission-strings-") as directory:
        d = Path(directory)
        includes, sources = arduino_string_counted(d, sanitize)
        unit, binary = d / "test.cpp", d / "test"
        unit.write_text(generate(args.component))
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-Wno-sign-compare", *sanitize, *includes, str(unit), *sources, "-o", str(binary)],
                       check=True, timeout=180)
        run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
    sys.stderr.write(run.stderr)
    if args.print_report:
        sys.stdout.write(run.stdout)
        sys.exit(run.returncode)
    expected = ANSWERS.read_text()
    if run.stdout != expected:
        diff = difflib.unified_diff(expected.splitlines(True), run.stdout.splitlines(True),
                                    "fs_permission_answers.txt", "this tree", n=0)
        sys.stderr.writelines(list(diff)[:60])
        raise AssertionError("the permission answers differ from fs_permission_answers.txt")
    require(run.returncode == 0, "the String block checks failed")
    source_contract(args.component)
    print(f"fs permission strings: {len(expected.splitlines())} report lines equal, source contract passed")


if __name__ == "__main__":
    main()
