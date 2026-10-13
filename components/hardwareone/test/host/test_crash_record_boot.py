#!/usr/bin/env python3
"""Execute the shipping boot-time crash counters and the OTA crash-loop escape decision.

The RTC layout, crashRecordBootConsume, the counter getters,
crashRecordMarkBootHealthy, crashRecordConsumeCrashLoopEscape and
crashRecordDetail come from System_CrashRecord.cpp; crashLoopEscapeWanted,
restartIntoCrashLoopRecovery and kCrashLoopEscapeCount from System_OTA.cpp; the
boot-phase enum from System_CrashRecord.h, all unchanged. RTC storage is process
memory, esp_app_get_elf_sha256_str is settable and esp_restart unwinds.
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

STATE_BEGIN = "#define CRASH_LAYOUT_VERSION"
STATE_END = "// Hand-rolled bounded copy"
CRASH_RECORD_MARKERS = (
    "static void IRAM_ATTR crashCopyStr(",
    "static uint32_t fnv1a(",
    "static bool isFaultReset(",
    "static bool isSoftwareFaultReset(",
    "void crashRecordBootConsume(",
    "bool        crashRecordPreviousBuildMatches()",
    "uint32_t    crashRecordConsecutive()",
    "uint32_t    crashRecordConsecutiveSoftwareFaults()",
    "uint8_t     crashRecordPrevPhase()",
    "size_t crashRecordDetail(",
    "void crashRecordMarkBootHealthy(",
    "void crashRecordConsumeCrashLoopEscape(",
)
OTA_MARKERS = (
    "static bool crashLoopEscapeWanted(",
    "[[noreturn]] static void restartIntoCrashLoopRecovery(",
)


def unique_block(source: str, marker: str, where: str) -> str:
    require(source.count(marker) == 1, f"{where} defines {marker.strip()} once")
    return extract_block(source, marker)


def generate() -> str:
    crash = (COMPONENT / "System_CrashRecord.cpp").read_text()
    crash_h = (COMPONENT / "System_CrashRecord.h").read_text()
    ota = (COMPONENT / "System_OTA.cpp").read_text()

    phase_enum = re.search(r"(?ms)^enum CrashBootPhase : uint8_t \{.*?^\};", crash_h)
    require(phase_enum is not None, "System_CrashRecord.h defines the CrashBootPhase enum")

    require(crash.count(STATE_BEGIN) == 1 and crash.count(STATE_END) == 1,
            "System_CrashRecord.cpp has one RTC layout region")
    state = crash[crash.index(STATE_BEGIN):crash.index(STATE_END)]
    require("rtcConsecutiveSoftware" in state,
            "the RTC state record carries a software-fault counter beside the fault counter")

    functions = "\n\n".join(unique_block(crash, marker, "System_CrashRecord.cpp")
                            for marker in CRASH_RECORD_MARKERS)

    threshold = re.search(r"(?m)^constexpr uint32_t kCrashLoopEscapeCount = \d+;$", ota)
    require(threshold is not None, "System_OTA.cpp defines kCrashLoopEscapeCount")
    escape = "\n\n".join([threshold[0]] + [unique_block(ota, marker, "System_OTA.cpp")
                                           for marker in OTA_MARKERS])
    require("crashRecordConsecutiveSoftwareFaults()" in escape,
            "the escape decision reads the software-fault count")
    require("crashRecordConsumeCrashLoopEscape()" in escape,
            "the committed escape consumes the counters before restarting")

    harness = (HERE / "crash_record_boot_harness.cpp").read_text()
    for marker, value in (("// INSERT_PHASE_ENUM", phase_enum[0]),
                          ("// INSERT_CRASH_RECORD_STATE", state),
                          ("// INSERT_CRASH_RECORD_FUNCTIONS", functions),
                          ("// INSERT_OTA_ESCAPE", escape)):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, value)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-crash-record-boot-") as temporary:
        unit, binary = Path(temporary) / "test.cpp", Path(temporary) / "test"
        unit.write_text(generate())
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-unused-function", str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        require(result.returncode == 0, f"crash record boot failed:\n{result.stdout}\n{result.stderr}")
        count = re.search(r"(?m)^checks=(\d+)$", result.stderr)
        require(count is not None, "missing completed check count")
        print(f"{result.stdout.strip()} ({count[1]} checks)", flush=True)


if __name__ == "__main__":
    main()
