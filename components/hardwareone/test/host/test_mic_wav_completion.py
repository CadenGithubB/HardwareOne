#!/usr/bin/env python3
"""Run the shipping WAV header writer and recording finalization tail.

File, filesystem locking, disposition sealing, and external publication/RTOS
services are observed boundaries. Injected errors represent reported Print
errors read after flush; Arduino flush does not propagate every backend
fflush/fsync failure. This checks completion/error propagation and event
ordering, not device storage durability, audio capture, or task races.
An optional saved --baseline component directory compares unchanged behavior
and proves the old code fails the three new completion regression checks.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generate(component: Path) -> str:
    source = (component / "System_Microphone.cpp").read_text()
    header = (COMPONENT / "System_Microphone.h").read_text()
    live = (COMPONENT / "System_LiveAudio.h").read_text()
    types = "\n".join(line for line in header.splitlines() if line.startswith((
        "using MicRecordingOwner =", "static constexpr MicRecordingOwner MIC_RECORDING_OWNER_MANUAL =")))
    types += "\n" + extract_block(live, "enum class LiveAudioRecorderOutcome") + ";\n"
    types += "\n".join(extract_block(source, marker) + ";" for marker in (
        "struct WavHeader", "struct MicRecordingDispositionSnapshot"))
    task = extract_block(source, "static void recordingTask(")
    start = task.index("  // Finalize WAV file.")
    # Keep all shipping finalization, success events, source teardown, and IDLE
    # publication together; do not reproduce a second completion algorithm.
    finalization = "static void runFinalization() {\n" + task[start:]
    production = "\n\n".join([
        extract_block(source, "static bool writeWavHeader("),
        extract_block(source, "static void micRecordingMarkFailed("),
        finalization,
    ])
    harness = (HERE / "mic_wav_completion_harness.cpp").read_text()
    for marker, text in (("// INSERT_TYPES", types), ("// INSERT_PRODUCTION", production)):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, text)
    return f"#define HAS_FINAL_FLUSH {int('recordingFile.flush();' in finalization)}\n" + harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path, help="saved component directory containing System_Microphone.cpp")
    parser.add_argument("--evidence", type=Path, help="retain generated sources and results")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    production_path = COMPONENT / "System_Microphone.cpp"
    digest = hashlib.sha256(production_path.read_bytes()).hexdigest()
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-mic-wav-completion-") as temp:
        work = args.evidence.resolve() if args.evidence else Path(temp)
        work.mkdir(parents=True, exist_ok=True)
        versions = [("current", COMPONENT)]
        if args.baseline:
            versions.append(("baseline", args.baseline.resolve()))
        transcripts = {}
        for label, component in versions:
            unit, executable = work / f"{label}.cpp", work / label
            unit.write_text(generate(component))
            subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)],
                           check=True, timeout=120)
            result = subprocess.run([str(executable), "unchanged"], capture_output=True,
                                    text=True, timeout=30)
            (work / f"{label}-transcript.txt").write_text(result.stdout)
            (work / f"{label}-assertions.log").write_text(result.stderr)
            require(result.returncode == 0, f"{label}: {result.stderr}")
            print(f"{label}: {result.stderr.strip()}")
            transcripts[label] = result.stdout
            for fault in ("short", "long", "error"):
                regression = subprocess.run([str(executable), fault], capture_output=True,
                                            text=True, timeout=30)
                (work / f"{label}-{fault}.log").write_text(regression.stdout + regression.stderr)
                if label == "baseline":
                    require(regression.returncode == 2 and
                            "FAIL: incomplete WAV must fail" in regression.stderr,
                            f"baseline must reproduce {fault} false success: {regression.stderr}")
                    print(f"baseline: reproduced {fault} false success")
                else:
                    require(regression.returncode == 0, f"{fault}: {regression.stderr}")
                    print(f"current {fault}: {regression.stderr.strip()}")
        if args.baseline:
            require(transcripts["current"] == transcripts["baseline"],
                    "unchanged baseline/current finalization transcripts differ")
            print("Unchanged lifecycle/publication and file-byte transcripts are byte-identical; the new explicit flush is checked separately.")
        require(hashlib.sha256(production_path.read_bytes()).hexdigest() == digest,
                "production source changed during verification")
        print(f"Stable System_Microphone.cpp SHA256: {digest}")


if __name__ == "__main__":
    main()
