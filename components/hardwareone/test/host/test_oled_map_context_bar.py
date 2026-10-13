#!/usr/bin/env python3
"""Run the OLED Map context bar text builder with names up to 63 characters.

oled_map_context_bar_harness.cpp: System_Maps.h's LocationContext, the
context-bar append helper from OLED_Mode_Map.cpp (when the tree has one) and
the displayGPSMap block from "char contextBuf[128];" up to the scroll update
are extracted unchanged. Inside them snprintf/vsnprintf follow the C contract
and write each byte from instrumented code, so --sanitize (ASan/UBSan) reports
any write past contextBuf. Every case runs in its own process.

--component points the unit at another component tree (e.g. the base) to show
which cases fail there.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

HELPER_MARKER = "static int appendMapContextText("
BLOCK_START = "char contextBuf[128];"
BLOCK_END = "    // Update scroll offset for animation"


def context_bar_unit(component: Path) -> str:
    oled = (component / "OLED_Mode_Map.cpp").read_text()
    maps_h = (component / "System_Maps.h").read_text()
    display = extract_block(oled, "static void displayGPSMap(")
    require(display.count(BLOCK_START) == 1, "displayGPSMap must declare contextBuf once")
    begin = display.index(BLOCK_START)
    block = display[begin:display.index(BLOCK_END, begin)]
    types = [
        extract_block(maps_h, "enum MapFeatureType") + ";",
        extract_block(maps_h, "struct LocationContext") + ";",
    ]
    helper = extract_block(oled, HELPER_MARKER) if HELPER_MARKER in oled else ""
    harness = (HERE / "oled_map_context_bar_harness.cpp").read_text()
    for marker in ("// INSERT_TYPES", "// INSERT_HELPER", "// INSERT_CONTEXT_BLOCK"):
        require(harness.count(marker) == 1, f"missing {marker} marker")
    return (harness.replace("// INSERT_TYPES", "\n\n".join(types))
            .replace("// INSERT_HELPER", helper)
            .replace("// INSERT_CONTEXT_BLOCK", block))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="component source tree to extract from (default: this checkout)")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    component = args.component.resolve()
    failed = []
    with tempfile.TemporaryDirectory(prefix="hw1-oled-map-context-bar-") as directory:
        unit = Path(directory) / "context_bar.cpp"
        binary = Path(directory) / "context_bar"
        unit.write_text(context_bar_unit(component))
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(binary)], check=True, timeout=180)
        names = subprocess.run([str(binary), "--list"], check=True, timeout=60,
                               capture_output=True, text=True).stdout.split()
        require(bool(names), "harness listed no cases")
        for name in names:
            print(f"== {name}", flush=True)
            if subprocess.run([str(binary), name], timeout=60).returncode != 0:
                failed.append(name)
    require(not failed, "failing: " + ", ".join(failed))
    print(f"test_oled_map_context_bar: all {len(names)} cases passed")


if __name__ == "__main__":
    main()
