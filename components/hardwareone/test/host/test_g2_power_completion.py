#!/usr/bin/env python3
"""Exercise the shipping G2 Power page and guarded redraw path on the host.

The complete page, command-completion redraw helper, public message types, and
worker's Redraw dispatch arm are compiled from production sources. Commands,
display calls, RTOS primitives, and power services are the host boundaries.
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


def declaration(source: str, marker: str) -> str:
    start = source.index(marker)
    return source[start:source.index(";", start) + 1]


def generated() -> str:
    header = (COMPONENT / "G2_HijackCmd.h").read_text()
    bridge = (COMPONENT / "G2_HijackCmd.cpp").read_text()
    glasses = (COMPONENT / "G2_Glasses.cpp").read_text()
    public = (COMPONENT / "G2_Glasses.h").read_text()
    page = (COMPONENT / "G2_Page_Power.cpp").read_text()
    types = [extract_block(public, "enum G2HijackPage") + ";"]
    types += [extract_block(header, marker) + ";" for marker in (
        "struct G2CmdCookie", "struct RedrawSpec", "enum class LensJobKind",
        "struct LensUiJob", "enum class G2LensEnqueueWait")]
    types += [declaration(header, marker) for marker in (
        "bool g2EnqueueLensJob(", "bool g2EnqueueRedraw(")]
    production = re.sub(r"^#include.*\n", "", page, flags=re.M)
    worker = extract_block(glasses, "static void pageSwapWorkerLoop(")
    harness = (HERE / "g2_power_completion_harness.cpp").read_text()
    replacements = (
        ("// INSERT_TYPES", "\n\n".join(types)),
        ("// INSERT_REDRAW_HELPER", extract_block(bridge, "bool g2EnqueueRedraw(")),
        ("// INSERT_PRODUCTION", production),
        ("// INSERT_REDRAW_DISPATCH", extract_block(worker, "case LensJobKind::Redraw:")),
    )
    for marker, body in replacements:
        require(harness.count(marker) == 1, f"missing/duplicate marker: {marker}")
        harness = harness.replace(marker, body)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-g2-power-") as directory:
        unit, executable = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(generated())
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)],
                       check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=60)


if __name__ == "__main__":
    main()
