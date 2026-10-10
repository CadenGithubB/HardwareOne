#!/usr/bin/env python3
"""Execute the incremental vendor APDS gesture decoder with controlled FIFO/time.

The production methods are extracted on every run. The harness supplies only
the existing register, FIFO, gesture-valid and Arduino clock boundaries.
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
LIBRARY = COMPONENT.parent / "hardwareone_libs" / "Adafruit_APDS9960_Library"


def generate(source_path: Path) -> str:
    source = source_path.read_text()
    header = (LIBRARY / "Adafruit_APDS9960.h").read_text()
    definitions = [extract_block(source, "uint8_t Adafruit_APDS9960::read("),
                   extract_block(source, "void Adafruit_APDS9960::resetCounts()"),
                   extract_block(source, "uint8_t Adafruit_APDS9960::readGesture()")]
    constants = []
    for name in ("UP", "DOWN", "LEFT", "RIGHT"):
        constants.append(re.search(r"(?m)^#define APDS9960_" + name + r"[^\n]+", header)[0])
    for name in ("GFLVL", "GFIFO_U"):
        value = re.search(r"\bAPDS9960_" + name + r"\s*=\s*(0x[0-9A-Fa-f]+)", header)[1]
        constants.append(f"static constexpr uint8_t APDS9960_{name} = {value};")
    counters = [re.search(r"\buint8_t " + name + r";", header)[0]
                for name in ("gestCnt", "UCount", "DCount", "LCount", "RCount")]
    counters.append(re.search(r"\bunsigned long gestureLastActivity[^;]*;", header)[0])
    harness = (HERE / "apds_gesture_driver_harness.cpp").read_text()
    for marker, text in {"// INSERT_CONSTANTS": "\n".join(constants),
                         "// INSERT_COUNTERS": "\n  ".join(counters),
                         "// INSERT_METHODS": "\n\n".join(definitions)}.items():
        require(harness.count(marker) == 1, f"expected one insertion point: {marker}")
        harness = harness.replace(marker, text)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source", type=Path, default=LIBRARY / "Adafruit_APDS9960.cpp",
                        help="vendor source override for checking a captured pre-fix version")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-apds-gesture-driver-") as directory:
        source = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        source.write_text(generate(args.source))
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
