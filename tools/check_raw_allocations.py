#!/usr/bin/env python3
"""Inventory direct libc allocation calls in the main HardwareOne component.

The scanner deliberately ignores comments and string/character literals so a
comment such as ``use malloc() here`` cannot move the baseline.  System_MemUtil
is excluded because it is the one implementation boundary allowed to call the
libc allocation family directly.  The standalone recovery updater is a
separate C project and intentionally remains outside this component-scoped
baseline.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".ino"}
EXCLUDED_PARTS = {"test", "tests", "third_party", "vendor", "managed_components"}
EXCLUDED_FILES = {"System_MemUtil.h", "System_MemUtil.cpp"}
CALL_RE = re.compile(r"(?<![A-Za-z0-9_])(malloc|calloc|realloc)\s*\(")


def blank_literals_and_comments(source: str) -> str:
    """Replace non-code bytes with spaces while preserving newlines/offsets."""

    out = list(source)
    size = len(source)
    i = 0

    def blank(start: int, end: int) -> None:
        for pos in range(start, end):
            if out[pos] not in ("\n", "\r"):
                out[pos] = " "

    while i < size:
        if source.startswith("//", i):
            end = source.find("\n", i + 2)
            if end < 0:
                end = size
            blank(i, end)
            i = end
            continue

        if source.startswith("/*", i):
            end = source.find("*/", i + 2)
            end = size if end < 0 else end + 2
            blank(i, end)
            i = end
            continue

        # C++ raw string: R"delimiter(contents)delimiter".  Delimiters are at
        # most 16 characters and cannot contain whitespace, parentheses or a
        # backslash.  If malformed, leave it alone rather than hiding code.
        if source.startswith('R"', i):
            open_paren = source.find("(", i + 2, min(size, i + 19))
            if open_paren >= 0:
                delimiter = source[i + 2 : open_paren]
                if not re.search(r"[\s()\\]", delimiter):
                    marker = ")" + delimiter + '"'
                    close = source.find(marker, open_paren + 1)
                    if close >= 0:
                        end = close + len(marker)
                        blank(i, end)
                        i = end
                        continue

        if source[i] in ('"', "'"):
            quote = source[i]
            end = i + 1
            while end < size:
                if source[end] == "\\":
                    end = min(size, end + 2)
                    continue
                end += 1
                if source[end - 1] == quote:
                    break
            blank(i, end)
            i = end
            continue

        i += 1

    return "".join(out)


def source_files(root: Path) -> list[Path]:
    component = root / "components" / "hardwareone"
    files: list[Path] = []
    for path in component.rglob("*"):
        if not path.is_file() or path.suffix.lower() not in SOURCE_SUFFIXES:
            continue
        relative = path.relative_to(component)
        if path.name in EXCLUDED_FILES or any(part in EXCLUDED_PARTS for part in relative.parts):
            continue
        files.append(path)
    return sorted(files)


def inventory(root: Path) -> list[tuple[Path, int, str]]:
    matches: list[tuple[Path, int, str]] = []
    for path in source_files(root):
        source = path.read_text(encoding="utf-8", errors="replace")
        code = blank_literals_and_comments(source)
        for match in CALL_RE.finditer(code):
            line = source.count("\n", 0, match.start()) + 1
            matches.append((path.relative_to(root), line, match.group(1)))
    return matches


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parents[1],
        help="repository root (defaults to the parent of tools/)",
    )
    parser.add_argument(
        "--expect",
        type=int,
        help="fail unless the inventory contains exactly this many calls",
    )
    parser.add_argument(
        "--expect-malloc",
        type=int,
        help="fail unless the inventory contains exactly this many malloc calls",
    )
    args = parser.parse_args()

    root = args.root.resolve()
    matches = inventory(root)
    for path, line, operation in matches:
        print(f"{path}:{line}: {operation}")
    operation_counts = {
        operation: sum(1 for _, _, found in matches if found == operation)
        for operation in ("malloc", "calloc", "realloc")
    }
    print(
        "operations: "
        + " ".join(f"{name}={operation_counts[name]}" for name in operation_counts)
    )
    print(f"direct allocation calls: {len(matches)}")

    if args.expect is not None and len(matches) != args.expect:
        print(
            f"expected {args.expect} direct allocation calls, found {len(matches)}",
            file=sys.stderr,
        )
        return 1
    if args.expect_malloc is not None and operation_counts["malloc"] != args.expect_malloc:
        print(
            f"expected {args.expect_malloc} malloc calls, "
            f"found {operation_counts['malloc']}",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
