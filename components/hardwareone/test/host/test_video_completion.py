#!/usr/bin/env python3
"""Run the shipping AVI recorder's file path and the VFS SD-in-use registration.

Part one compiles the real header skeleton, frame writer, first-frame geometry
patch, finalizer and failed-start cleanup from System_Camera_Video.cpp against
the shared fake VFS (with the real FsLockGuard) and checks that VIDEO_SAVED is
posted only for a complete clip: a short idx1 write, a failed seek, a failed
header patch, a flush error or an earlier frame-write failure post no saved
event; the header carries the measured timing, the largest frame as the buffer
size and the real geometry; a failed start removes its half-made file.

Part two compiles the real SD-in-use registration, unmountSD/remountSD/formatSD
and the sdunmount/sdmount/sdformat commands from System_VFS.cpp against a fake
SD driver and checks that every lifecycle call is refused, with a message,
while a registration is held, and works again after it ends.

Not covered: the recorder task loop, camera capture, RTOS task creation and
real SD driver behaviour.
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

VIDEO_SCENARIOS = ("saved", "short_idx", "failed_seek", "failed_patch", "flush_error",
                   "frame_failed", "dims_seek", "dims_write", "failed_start")
SD_SCENARIOS = ("unmount", "remount", "format", "slots")


def unique_block(source: str, marker: str) -> str:
    require(source.count(marker) == 1, f"marker must be unique: {marker}")
    return extract_block(source, marker)


def fs_guard() -> str:
    mutex_header = (COMPONENT / "System_Mutex.h").read_text()
    mutex = (COMPONENT / "System_Mutex.cpp").read_text()
    guard = extract_block(mutex_header, "struct FsLockGuard") + ";\n"
    guard += "\n".join(extract_block(mutex, marker) for marker in (
        "FsLockGuard::FsLockGuard(const char* owner)",
        "FsLockGuard::FsLockGuard(const char* owner, TickType_t timeoutTicks)",
        "FsLockGuard::~FsLockGuard()"))
    return guard


def video_unit() -> str:
    source = (COMPONENT / "System_Camera_Video.cpp").read_text()
    constants = re.findall(r"(?m)^static const uint32_t (?:AVI_HEADER_SIZE|OFF_\w+)\s*=[^\n]+", source)
    require(any(c.startswith("static const uint32_t AVI_HEADER_SIZE") for c in constants),
            "AVI header offsets")
    owner = re.findall(r"(?m)^static const char\* const VIDEO_SD_OWNER[^\n]+", source)
    start = source.index("static File s_file;")
    end = source.index("static size_t s_idxCap = 0;") + len("static size_t s_idxCap = 0;")
    state = source[start:end]
    blocks = [unique_block(source, marker) for marker in (
        "static inline void put32(", "static inline void put16(", "static inline uint32_t fourcc(",
        "static void buildHeaderSkeleton(", "static bool probeJpegDims(", "static bool writeFrameChunk(")]
    has_dims = "static bool patchFirstFrameDims(" in source
    has_discard = "static void discardFailedStart(" in source
    has_outcome = "static bool finalizeAndClose(" in source
    if has_dims:
        blocks.append(unique_block(source, "static bool patchFirstFrameDims("))
    blocks.append(unique_block(source, "static bool finalizeAndClose(" if has_outcome
                               else "static void finalizeAndClose("))
    if has_discard:
        blocks.append(unique_block(source, "static void discardFailedStart("))
    production = "\n".join(constants + owner) + "\n\n" + state + "\n\n" + "\n\n".join(blocks)
    harness = (HERE / "video_completion_harness.cpp").read_text()
    for marker, text in (("// INSERT_FS_GUARD", fs_guard()), ("// INSERT_RECORDER", production)):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, text)
    flags = (f"#define HAS_DIMS_HELPER {int(has_dims)}\n#define HAS_DISCARD_HELPER {int(has_discard)}\n"
             f"#define HAS_FINALIZE_OUTCOME {int(has_outcome)}\n")
    return flags + harness


def sd_unit() -> str:
    source = (COMPONENT / "System_VFS.cpp").read_text()
    require("bool sdInUseBegin(" in source, "VFS has no SD-in-use registration (sdInUseBegin)")
    registration =source[source.index("static constexpr size_t kSdInUseSlots"):
                          source.index("static bool sdLifecycleRefusedLocked(")]
    vfs = [registration, unique_block(source, "static bool sdLifecycleRefusedLocked(")]
    vfs += [unique_block(source, marker) for marker in ("bool unmountSD()", "bool remountSD()", "bool formatSD()")]
    commands = [unique_block(source, marker) for marker in (
        "static const char* cmd_sdmount(", "static const char* cmd_sdunmount(", "static const char* cmd_sdformat(")]
    harness = (HERE / "sd_in_use_harness.cpp").read_text()
    for marker, text in (("// INSERT_VFS", "\n\n".join(vfs)), ("// INSERT_COMMANDS", "\n\n".join(commands))):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, text)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function",
             "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
    failures = []
    with tempfile.TemporaryDirectory(prefix="hw1-video-completion-") as temp:
        work = Path(temp)
        for name, generate, scenarios in (("video_completion", video_unit, VIDEO_SCENARIOS),
                                          ("sd_in_use", sd_unit, SD_SCENARIOS)):
            unit, binary = work / f"{name}.cpp", work / name
            try:
                unit.write_text(generate())
            except (AssertionError, ValueError) as error:
                print(f"{name}: production code missing: {error}")
                failures.append(f"{name} extraction")
                continue
            subprocess.run([args.cxx, *flags, "-I", str(HERE / "web_batch_stubs"), "-I", str(HERE),
                            str(unit), "-o", str(binary)], check=True, timeout=120)
            for scenario in scenarios:
                result = subprocess.run([str(binary), scenario], capture_output=True, text=True, timeout=30)
                output = (result.stdout + result.stderr).strip()
                print(f"{name} {scenario}: {output}")
                if result.returncode != 0:
                    failures.append(f"{name} {scenario}")
    require(not failures, "failed: " + ", ".join(failures))
    print(f"Video completion: {len(VIDEO_SCENARIOS)} recorder and {len(SD_SCENARIOS)} SD-in-use scenarios passed")


if __name__ == "__main__":
    main()
