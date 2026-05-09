#!/usr/bin/env python3
"""
Build-time guard for the permission refactor.

After Phases 1-6 (see commit history / docs), every file read/write goes
through VFS::*Guarded with a real AuthContext. That keeps the [PERM] DENY
audit trail meaningful and the role-based rule table authoritative.

This guard fails the build if anyone reintroduces raw LittleFS file I/O
outside System_VFS.cpp (the one allowed implementation site). It catches
LittleFS.open / .exists / .remove / .rename / .mkdir / .rmdir on any non-
comment line. Stat queries (LittleFS.totalBytes, LittleFS.usedBytes) and
mount/format (LittleFS.begin, LittleFS.format) are NOT file I/O, so they
stay legitimate and aren't flagged.

Invoked from components/hardwareone/CMakeLists.txt via execute_process at
configure time. Usage:
    python3 check_no_raw_littlefs.py <component_dir>

Exit code: 0 = clean, 1 = offenders found (with detailed message on stderr).
"""

from __future__ import annotations

import glob
import os
import re
import sys

# Match the file-I/O API surface only. .totalBytes / .usedBytes / .begin /
# .format are intentionally excluded — those are mount and stat helpers, not
# I/O, and have no permission story.
PATTERN = re.compile(r"LittleFS\.(open|exists|remove|rename|mkdir|rmdir)\s*\(")

# Skip lines that are clearly comments. Catches both "// LittleFS.open(...)"
# and " *  LittleFS.open(...)" inside /** ... */ blocks. Doesn't try to handle
# block comments that span multiple lines without per-line leaders — those
# are rare and easy to spot if they false-positive.
COMMENT_LINE = re.compile(r"^\s*[/*]")

ALLOWED_FILE = "System_VFS.cpp"


def scan(root: str) -> list[tuple[str, int, str]]:
    offenders: list[tuple[str, int, str]] = []
    for ext in ("*.cpp", "*.h"):
        for path in sorted(glob.glob(os.path.join(root, ext))):
            if os.path.basename(path) == ALLOWED_FILE:
                continue
            try:
                with open(path, encoding="utf-8", errors="replace") as f:
                    for lineno, raw in enumerate(f, 1):
                        if COMMENT_LINE.match(raw):
                            continue
                        if PATTERN.search(raw):
                            offenders.append((path, lineno, raw.rstrip()))
            except OSError as e:
                print(f"WARN: could not read {path}: {e}", file=sys.stderr)
    return offenders


def main() -> int:
    if len(sys.argv) != 2:
        print(
            "usage: check_no_raw_littlefs.py <component_dir>", file=sys.stderr
        )
        return 2
    root = os.path.abspath(sys.argv[1])
    offenders = scan(root)
    if not offenders:
        return 0

    print(
        "ERROR: Raw LittleFS file I/O detected outside System_VFS.cpp.",
        file=sys.stderr,
    )
    print(
        "All file reads/writes must go through VFS::*Guarded so the [PERM]",
        file=sys.stderr,
    )
    print(
        "audit trail and role-based rules apply. Offending lines:",
        file=sys.stderr,
    )
    print("", file=sys.stderr)
    for path, lineno, content in offenders:
        rel = os.path.relpath(path, root)
        print(f"  {rel}:{lineno}:  {content.strip()}", file=sys.stderr)
    print("", file=sys.stderr)
    print(
        "Replace each call with the matching VFS::*Guarded variant from",
        file=sys.stderr,
    )
    print("System_VFS.h. The AuthContext argument should be:", file=sys.stderr)
    print(
        "  - gExecAuthContext               for CLI / serial / lens handlers",
        file=sys.stderr,
    )
    print(
        "  - the request's AuthContext      for web handlers",
        file=sys.stderr,
    )
    print(
        '  - VFS::systemAuth("reason")      for trusted internal code',
        file=sys.stderr,
    )
    print("", file=sys.stderr)
    print(
        "If the call is genuinely policy-free infra (log rotation, etc.),",
        file=sys.stderr,
    )
    print(
        "use the unguarded VFS::open / VFS::remove / etc. inside",
        file=sys.stderr,
    )
    print(
        "System_VFS.cpp's namespace — never raw LittleFS outside that file.",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main())
