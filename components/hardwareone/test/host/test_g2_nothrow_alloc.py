#!/usr/bin/env python3
"""G2 heap objects use the nothrow `new`, and their launchers refuse on OOM.

The firmware builds with C++ exceptions off, so a plain `new` that cannot
allocate ends in abort and a panic reboot. Two checks:

1. Every `= new T` in G2_Glasses.cpp is `new (std::nothrow)`.
2. The production Health, Map and BMP launchers, compiled against an
   operator new that aborts when the heap is exhausted (the firmware's
   behaviour) and a nothrow form that returns nullptr, return false and queue
   nothing instead of aborting.

--component points the suite at another copy of the component (the pre-fix tree
fails both).
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
PLAIN_NEW = re.compile(r"=\s*new\s+(?!\(std::nothrow\))[A-Za-z_]")

SITES = (
    ("struct HealthPageArgs", True),
    ("static void g2HealthPageDrop(void* p)", False),
    ("static bool g2ShowHealthPage(void (*onDone)())", False),
    ("struct MapPageArgs", True),
    ("static void g2MapPageDrop(void* p)", False),
    ("static bool g2ShowMapPage(void (*onDone)())", False),
    ("struct BmpViewerArgs", True),
    ("static void g2BmpViewerDrop(void* p)", False),
    ("bool g2ShowBmpFile(const char* path, void (*onDone)())", False),
)


def plain_new_sites(source: str) -> list[str]:
    found = []
    for number, line in enumerate(source.splitlines(), 1):
        code = line.split("//", 1)[0]
        if PLAIN_NEW.search(code):
            found.append(f"G2_Glasses.cpp:{number}: {line.strip()}")
    return found


def generated(source: str) -> str:
    blocks = []
    for marker, is_type in SITES:
        # Definitions only: skip forward declarations ending in ';'.
        start = 0
        while True:
            start = source.index(marker, start)
            tail = source[start + len(marker):].lstrip()
            if tail.startswith("{"):
                break
            start += len(marker)
        block = extract_block(source[start:], marker)
        blocks.append(block + (";" if is_type else ""))
    harness = (HERE / "g2_nothrow_alloc_harness.cpp").read_text()
    require(harness.count("// INSERT_G2_SITES") == 1, "exactly one insertion marker")
    return harness.replace("// INSERT_G2_SITES", "\n\n".join(blocks))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--component", type=Path, default=COMPONENT)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    source = (args.component / "G2_Glasses.cpp").read_text()
    failures = plain_new_sites(source)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
             "-Wno-unused-function", "-Wno-unused-variable"]
    if args.sanitize:
        flags += ["-fsanitize=undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-g2-nothrow-") as directory:
        work = Path(directory)
        test_source, binary = work / "test.cpp", work / "test"
        test_source.write_text(generated(source))
        subprocess.run([args.cxx, *flags, str(test_source), "-o", str(binary)],
                       check=True, timeout=120)
        result = subprocess.run([str(binary)], timeout=60)
        if result.returncode != 0:
            failures.append(f"launcher harness exited {result.returncode} (abort = plain new under OOM)")
    for failure in failures:
        print(f"FAIL: {failure}")
    require(not failures, f"{len(failures)} G2 allocation site(s) can abort on OOM")
    print("g2 allocation sites: all nothrow; launchers refuse on OOM")


if __name__ == "__main__":
    main()
