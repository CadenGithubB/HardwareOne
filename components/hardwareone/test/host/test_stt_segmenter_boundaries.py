#!/usr/bin/env python3
"""Segment cuts and PCM conservation of stt/stt_segmenter.h under ASan/UBSan, any push chunk size.

Ported from the speech repository's version 3.1 (tests/test_segmenter_boundaries.py): modes 0-5
are the fixed and adaptive detectors, 6-9 repeat the adaptive cases with the level-robust onset
(decision_pre_emphasis), 10-11 add its noise controls. No board or recordings are used.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="hw1-stt-segmenter-boundaries-") as tmp:
        binary = Path(tmp) / "audit"
        subprocess.run([
            args.cxx, "-std=c++17", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
            "-I" + str(HERE.parents[1]), str(HERE / "stt_segmenter_boundary_test.cpp"), "-o", str(binary),
        ], check=True, timeout=300)
        subprocess.run([str(binary)], check=True, timeout=600)


if __name__ == "__main__":
    main()
