#!/usr/bin/env python3
"""Execute the shipping taskstats/perftop handlers with controlled RTOS samples.

Uses vendored ArduinoJson and intact handlers/helpers; RTOS, allocation, command
validation and loop profiling are boundaries. Optional --baseline repeats every
fixture against a saved old System_Utils.cpp and compares complete output.
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
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs/ArduinoJson/src"
SCENARIOS = (
    "normal", "rollover", "cutoff", "cutoff-odd", "wide", "cutoff-wrap",
    "empty-before", "empty-after", "zero-time", "no-visible", "zero-tasks",
    "fail-first", "fail-second", "validate", "states", "stats-null",
    "stats-cache", "stats-empty", "stats-fail", "stats-zero",
)


def generate(source: str) -> str:
    helpers = []
    for marker in ("const char* taskStateLabel(", "struct TaskCpuSample"):
        if marker in source:
            helpers.append(extract_block(source, marker) + (";" if marker.startswith("struct") else ""))
    handlers = [extract_block(source, marker) for marker in
                ("const char* cmd_taskstats(", "const char* cmd_perftop(")]
    harness = (HERE / "task_reporting_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "missing production insertion point")
    return harness.replace("// INSERT_PRODUCTION", "\n\n".join(helpers + handlers))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path, help="saved pre-change System_Utils.cpp (not committed)")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    sources = [("current", COMPONENT / "System_Utils.cpp")]
    if args.baseline:
        sources.append(("baseline", args.baseline))
    transcripts: dict[str, dict[str, str]] = {}
    with tempfile.TemporaryDirectory(prefix="hw1-task-reporting-") as temporary:
        directory = Path(temporary)
        for label, source in sources:
            unit, binary = directory / f"{label}.cpp", directory / label
            unit.write_text(generate(source.read_text()))
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-Wno-unused-function", "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1",
                       "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0", "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0",
                       "-DARDUINOJSON_ENABLE_PROGMEM=0", "-I", str(HERE / "web_batch_stubs"),
                       "-I", str(JSON_INCLUDE), str(unit), "-o", str(binary)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                                "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=120)
            transcripts[label] = {}
            assertions = 0
            for scenario in SCENARIOS:
                result = subprocess.run([str(binary), scenario], capture_output=True, text=True, timeout=30)
                require(result.returncode == 0, f"{label}/{scenario} failed:\n{result.stdout}\n{result.stderr}")
                count = re.search(r"(?m)^checks=(\d+)$", result.stderr)
                require(count is not None, f"{label}/{scenario}: missing completed assertion count")
                assertions += int(count[1])
                transcripts[label][scenario] = result.stdout
            print(f"task reporting {label}: {len(SCENARIOS)} scenarios, {assertions} assertions passed", flush=True)
        if args.baseline:
            for scenario in SCENARIOS:
                require(transcripts["current"][scenario] == transcripts["baseline"][scenario],
                        f"changed command output for {scenario}:\ncurrent:\n{transcripts['current'][scenario]}"
                        f"\nbaseline:\n{transcripts['baseline'][scenario]}")
            print("task reporting: all text/JSON transcripts byte-identical to baseline", flush=True)


if __name__ == "__main__":
    main()
