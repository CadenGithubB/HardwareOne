#!/usr/bin/env python3
"""Run the shipping OTA probation supervisor against a fake FreeRTOS tick.

The phase enum, timing constants, atomics, elapsedTicks, probationSupervisor,
otaSafetyAcceptProvisioningBoot, otaSafetySetupReachedRunning,
otaSafetyLoopHeartbeat and otaSafetyIsPendingVerification come from
System_OTASafety.cpp, and OtaProbationCause from System_OTASafety.h, unchanged.
The harness owns the tick, the journal check, the otadata write and the reboot.
Each scenario runs in a fresh process.
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

FUNCTION_MARKERS = (
    "void probationSupervisor(",
    "bool otaSafetyAcceptProvisioningBoot(",
    "bool otaSafetyIsPendingVerification(",
    "void otaSafetySetupReachedRunning(",
    "void otaSafetyLoopHeartbeat(",
)
SCENARIOS = (
    "accept_early",
    "accept_after_45s",
    "accept_after_290s",
    "stall_during_accept",
    "mark_failure",
    "running_stall",
    "healthy_probation",
)


def unique_block(source: str, marker: str) -> str:
    require(source.count(marker) == 1, f"System_OTASafety.cpp defines {marker.strip()} once")
    return extract_block(source, marker)


def generate() -> str:
    source = (COMPONENT / "System_OTASafety.cpp").read_text()
    header = (COMPONENT / "System_OTASafety.h").read_text()

    cause = re.search(r"(?ms)^enum OtaProbationCause : uint8_t \{.*?^\};", header)
    require(cause is not None, "System_OTASafety.h defines OtaProbationCause")

    constants = re.findall(r"(?m)^constexpr (?:TickType_t|uint32_t) k\w+ = [^\n]+;$", source)
    require(len(constants) >= 7, "System_OTASafety.cpp defines the probation limits")
    atomics = re.findall(r"(?m)^std::atomic<[^>]+> s\w+\{[^}\n]*\};$", source)
    require(len(atomics) >= 7, "System_OTASafety.cpp defines the probation state atomics")
    state = "\n".join(constants + [unique_block(source, "enum class ProbationPhase : uint32_t") + ";"]
                      + atomics)
    elapsed = unique_block(source, "inline TickType_t elapsedTicks(")
    functions = "\n\n".join(unique_block(source, marker) for marker in FUNCTION_MARKERS)

    harness = (HERE / "ota_probation_supervisor_harness.cpp").read_text()
    for marker, value in (("// INSERT_CAUSE_ENUM", cause[0]),
                          ("// INSERT_STATE", state),
                          ("// INSERT_ELAPSED", elapsed),
                          ("// INSERT_FUNCTIONS", functions)):
        require(harness.count(marker) == 1, f"one insertion marker: {marker}")
        harness = harness.replace(marker, value)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-ota-probation-") as temporary:
        unit, binary = Path(temporary) / "test.cpp", Path(temporary) / "test"
        unit.write_text(generate())
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   str(unit), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        failures = []
        for scenario in SCENARIOS:
            result = subprocess.run([str(binary), scenario], capture_output=True, text=True,
                                    timeout=30)
            if result.returncode != 0:
                failures.append(f"{scenario}: {result.stdout}{result.stderr}".strip())
        require(not failures, "OTA probation supervisor failed:\n" + "\n".join(failures))
    print(f"OTA probation supervisor: {len(SCENARIOS)} fresh-process scenarios passed", flush=True)


if __name__ == "__main__":
    main()
