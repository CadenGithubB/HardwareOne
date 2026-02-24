#!/usr/bin/env python3
"""
Line counter for first-party HardwareOne source code.
Scans (flat, no recursion into subdirs):
  - components/hardwareone   — main firmware component
  - main/                    — app entry point
  - tools/                   — host-side tools
  - randomscripts/           — utility scripts
"""

import os
from pathlib import Path

REPO_ROOT = Path("/Users/morgan/esp/hardwareone-idf")

# Flat scan dirs (no recursion) — avoids picking up third-party libs
SCAN_DIRS = [
    (REPO_ROOT / "components" / "hardwareone", {'.cpp', '.h', '.c'}),
    (REPO_ROOT / "main",                       {'.cpp', '.h', '.c'}),
    (REPO_ROOT / "randomscripts",              {'.cpp', '.h', '.c', '.py'}),
]

def count_lines(file_path):
    try:
        with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
            return sum(1 for _ in f)
    except:
        return 0

def scan_dir(directory, extensions):
    results = []
    if not directory.is_dir():
        return results
    for filename in sorted(os.listdir(directory)):
        file_path = directory / filename
        if not file_path.is_file():
            continue
        if file_path.suffix.lower() not in extensions:
            continue
        lines = count_lines(file_path)
        rel = str(file_path.relative_to(REPO_ROOT))
        results.append((rel, file_path.suffix.lower(), lines))
    return results

def main():
    all_files = []
    section_data = {}

    for scan_dir_path, exts in SCAN_DIRS:
        label = str(scan_dir_path.relative_to(REPO_ROOT))
        files = scan_dir(scan_dir_path, exts)
        section_data[label] = files
        all_files.extend(files)

    W = 70
    print("=" * W)
    print("HARDWAREONE — FIRST-PARTY SOURCE LINE COUNT")
    print("=" * W)

    total_lines = 0
    ext_totals = {}

    for label, files in section_data.items():
        if not files:
            continue
        sec_lines = sum(f[2] for f in files)
        print(f"\n[ {label} ]  ({len(files)} files, {sec_lines:,} lines)")
        print(f"  {'File':<55} {'Lines':>8}")
        print(f"  {'-'*55} {'-'*8}")
        for rel, ext, lines in files:
            name = Path(rel).name
            print(f"  {name:<55} {lines:>8,}")
            total_lines += lines
            ext_totals[ext] = ext_totals.get(ext, 0) + lines

    print(f"\n{'=' * W}")
    print(f"BY EXTENSION:")
    for ext in sorted(ext_totals.keys()):
        count = sum(1 for f in all_files if f[1] == ext)
        print(f"  {ext}: {count} files, {ext_totals[ext]:,} lines")

    print(f"\nGRAND TOTAL: {len(all_files)} files, {total_lines:,} lines")
    print("=" * W)

if __name__ == "__main__":
    main()
