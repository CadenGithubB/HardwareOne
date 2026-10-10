#!/usr/bin/env python3
"""Run production MIC/Logging completions, redraw dispatch and live rendering.

Only transport, RTOS and audio services are stubbed. The shipping completion,
handoff, live renderer and redraw queue path execute with fault injection.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_g2_redraw import declaration
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generated() -> str:
    source = (COMPONENT / "G2_Glasses.cpp").read_text()
    public = (COMPONENT / "G2_Glasses.h").read_text()
    header = (COMPONENT / "G2_HijackCmd.h").read_text()
    bridge = (COMPONENT / "G2_HijackCmd.cpp").read_text()
    types = [extract_block(public, "enum G2HijackPage") + ";"]
    types += [extract_block(header, marker) + ";" for marker in (
        "struct G2CmdCookie", "struct RedrawSpec", "enum class LensJobKind",
        "struct LensUiJob", "enum class G2LensEnqueueWait")]
    types += [declaration(header, marker) for marker in (
        "bool g2EnqueueLensJob(", "bool g2EnqueueRedraw(")]
    production = [extract_block(bridge, "bool g2EnqueueRedraw(")]
    production += [extract_block(source, marker) for marker in (
        "static void requestMicDetailRecreate(",
        "static void buildMicReadoutText(", "static bool renderMicDetailLive(")]
    production.append("static const G2PageModule kMicDetailPage{1000, renderMicDetailLive};")
    production.append(extract_block(source, "bool g2ShowMicDetail("))
    production.append("#if ENABLE_MICROPHONE")
    production += [extract_block(source, marker) for marker in (
        "static void onMicDetailRuntimeDone(", "static void onMicDetailRecordDone(")]
    production.append("#endif")
    production.append(extract_block(source, "static void onMicDetailSourceDone("))
    production += [extract_block(source, marker) for marker in (
        "static void g2RefreshLoggingMenu(", "static void onLoggingCmdDone(")]
    worker = extract_block(source, "static void pageSwapWorkerLoop(")
    redraw = extract_block(worker, "case LensJobKind::Redraw:")
    harness = (HERE / "g2_mic_completion_harness.cpp").read_text()
    for marker, body in (("// INSERT_TYPES", "\n\n".join(types)),
                         ("// INSERT_PRODUCTION", "\n\n".join(production)),
                         ("// INSERT_DISPATCH", redraw)):
        require(harness.count(marker) == 1, f"bad marker: {marker}")
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
    with tempfile.TemporaryDirectory(prefix="hw1-g2-mic-completion-") as directory:
        unit, executable = Path(directory) / "test.cpp", Path(directory) / "test"
        unit.write_text(generated())
        for microphone in (0, 1):
            subprocess.run([args.cxx, *flags, f"-DENABLE_MICROPHONE={microphone}",
                            str(unit), "-o", str(executable)], check=True, timeout=120)
            subprocess.run([str(executable)], check=True, timeout=60)


if __name__ == "__main__":
    main()
