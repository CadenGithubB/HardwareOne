#!/usr/bin/env python3
"""Run the shipping g2micwav power-cut checkpoint on the host.

Compiles the production packet writer, checkpoint, header patch and close from
G2_Glasses.cpp over a file double whose flush()/close() decide what survives a
power cut, then cuts captures at random points (several seeds) and checks the
surviving file parses as a WAV whose RIFF and data sizes match the bytes on
disk and that holds every packet up to the last checkpoint. Also covers a cut
inside a checkpoint, a normal stop, a LittleFS path and a failed seek back.

The start path's header sync and SD/LittleFS choice are checked for presence.
Real FAT/SD durability across a power cut, and the stall a sync adds to the
BLE notify task, need a device.

--source <G2_Glasses.cpp> runs the harness against another copy (for example
the pre-fix file, where the cut capture holds no audio and the test fails).
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
SEEDS = (1, 7, 42, 2026, 90210)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source", type=Path, default=COMPONENT / "G2_Glasses.cpp")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    source = args.source.read_text()

    blocks = []
    for marker in ("static void micWavWriteHeader(",
                   "static bool micWavPatchSizesLocked(",
                   "static void micWavPatchSizesLocked(",
                   "static bool micWavCheckpointLocked(",
                   "static void micWavCloseLocked(",
                   "static bool micWavWritePacketLocked("):
        if marker in source:
            blocks.append(extract_block(source, marker))
    constant = re.search(r"static constexpr uint32_t kMicWavCheckpointBytes = [^;]+;", source)
    cap = re.search(r"static constexpr uint32_t kMicWavMaxBytes = [^;]+;", source)
    require(cap is not None, "kMicWavMaxBytes found")
    production = "\n\n".join(
        [cap.group(0),
         constant.group(0) if constant else
         "static constexpr uint32_t kMicWavCheckpointBytes = 40u * 1600u;  // pre-fix run"]
        + blocks)

    if args.source == COMPONENT / "G2_Glasses.cpp":
        start = extract_block(source, "static const char* cmd_g2micwav(")
        header_at = start.index("micWavWriteHeader(*f, /*pcmBytes*/ 0);")
        require(start.index("f->flush();", header_at) < start.index("gMicWavFile        = f;"),
                "start syncs the placeholder header before the writer sees the file")
        require("gMicWavCheckpointBytes = 0;" in start,
                "start resets the checkpoint")
        # The open routes on the normalized path (openGuarded -> normalizeFsPath ->
        # VFS::getStorageType), so "//sd/x.wav" or "/./sd/x.wav" land on the SD card;
        # the header choice must use the same routing, not the raw spelling.
        require('path.startsWith("/sd/")' not in start[header_at:] and
                re.search(r"gMicWavPatchHeader\s*=\s*normalizeFsPath\(path,\s*(\w+)\)\s*&&\s*"
                          r"VFS::getStorageType\(\1\)\s*==\s*VFS::SDCARD;", start) is not None,
                "start rewrites headers only on the SD card, as the open routed the path")
        notify = extract_block(source, "static void handleAudioNotify(")
        wav_at = notify.index("micWavWritePacketLocked(data);")
        require("micWavCloseLocked(" not in notify[wav_at:notify.index("xSemaphoreGive(gMicWavMutex);", wav_at)],
                "the notify path does not close the WAV after the write released the FS lock "
                "(the cap close is inside micWavWritePacketLocked)")

    harness = (HERE / "g2_mic_wav_checkpoint_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one insertion marker")
    harness = harness.replace("// INSERT_PRODUCTION", production)

    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
             "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-g2-mic-wav-checkpoint-") as temp:
        unit, executable = Path(temp) / "checkpoint.cpp", Path(temp) / "checkpoint"
        unit.write_text(harness)
        subprocess.run([args.cxx, *flags, str(unit), "-o", str(executable)],
                       check=True, timeout=180)
        for seed in SEEDS:
            extra = ["cap"] if seed == SEEDS[0] else []  # the 16 MB cap case runs once
            result = subprocess.run([str(executable), str(seed), *extra], capture_output=True,
                                    text=True, timeout=120)
            require(result.returncode == 0, result.stderr)
            print(result.stderr.strip())


if __name__ == "__main__":
    main()
