#!/usr/bin/env python3
"""Run the shipping interrupted-recording repair decision on the host.

Covers the pure decision only (which recorder files a boot pass completes,
removes or leaves). The directory walk, the header rewrite and the periodic
checkpoint flush in the capture loop are checked for presence, not executed:
storage durability across a real power cut needs a device.
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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    source = (COMPONENT / "System_Microphone.cpp").read_text()
    production = "\n".join(extract_block(source, marker) + tail for marker, tail in (
        ("struct WavHeader", ";"),
        ("enum class MicWavRepairAction", ";"),
        ("struct MicWavRepairDecision", ";"),
        ("static MicWavRepairDecision micWavRepairDecide(", ""),
    ))
    harness = (HERE / "mic_wav_repair_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one insertion marker")
    harness = harness.replace("// INSERT_PRODUCTION", production)

    # The decision is only useful with its two callers in place.
    task = extract_block(source, "static void recordingTask(")
    require("recordingFile.flush();" in task[:task.index("  // Finalize WAV file.")],
            "the capture loop checkpoints manual recordings with a flush")
    repair = extract_block(source, "static void micRecordingRepairFolder(")
    require('"r+"' in repair and '"w"' not in repair,
            "the repair rewrites the header in place and never truncates")
    boot = (COMPONENT / "HardwareOne.cpp").read_text()
    require(boot.count("micRecordingRepairInterrupted();") == 1 and
            boot.index("micRecordingRepairInterrupted();") <
            boot.index('runUnifiedSystemCommand("opensr")'),
            "the repair runs once at boot, before the microphone auto-start")

    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-mic-wav-repair-") as temp:
        unit, executable = Path(temp) / "repair.cpp", Path(temp) / "repair"
        unit.write_text(harness)
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)],
                       check=True, timeout=120)
        result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
        require(result.returncode == 0, result.stderr)
        print(result.stderr.strip())


if __name__ == "__main__":
    main()
