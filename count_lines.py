#!/usr/bin/env python3
"""
Line counter for /Users/morgan/esp/hardwareone-idf/components/hardwareone
Lists every file with line count.
"""

import os
from pathlib import Path

TARGET_DIR = Path("/Users/morgan/esp/hardwareone-idf/components/hardwareone")
SOURCE_EXTENSIONS = {'.cpp', '.h', '.c'}

def count_lines(file_path):
    try:
        with open(file_path, 'r', encoding='utf-8', errors='ignore') as f:
            return sum(1 for _ in f)
    except:
        return 0

def main():
    files_data = []
    
    for filename in sorted(os.listdir(TARGET_DIR)):
        file_path = TARGET_DIR / filename
        if not file_path.is_file():
            continue
        ext = file_path.suffix.lower()
        if ext not in SOURCE_EXTENSIONS:
            continue
        
        lines = count_lines(file_path)
        files_data.append((filename, ext, lines))
    
    # Print all files sorted by name
    print("=" * 70)
    print("HARDWAREONE COMPONENT - ALL FILES")
    print(f"Directory: {TARGET_DIR}")
    print("=" * 70)
    
    print(f"\n{'File':<45} {'Ext':<6} {'Lines':>10}")
    print("-" * 63)
    
    total_lines = 0
    ext_totals = {}
    
    for filename, ext, lines in files_data:
        print(f"{filename:<45} {ext:<6} {lines:>10,}")
        total_lines += lines
        ext_totals[ext] = ext_totals.get(ext, 0) + lines
    
    # Summary
    print("-" * 63)
    print(f"{'TOTAL':<45} {'':<6} {total_lines:>10,}")
    
    print(f"\n### BY EXTENSION ###\n")
    for ext in sorted(ext_totals.keys()):
        count = len([f for f in files_data if f[1] == ext])
        print(f"{ext}: {count} files, {ext_totals[ext]:,} lines")
    
    print(f"\n{'=' * 70}")
    print(f"TOTAL: {len(files_data)} files, {total_lines:,} lines")
    print("=" * 70)

if __name__ == "__main__":
    main()
