#!/usr/bin/env python3
"""Exercise production APDS cache commands and task sampling with controlled I/O.

The reader-only variant deliberately declares no hardware or event interfaces.
The sampling variant substitutes only driver, bus admission and RTOS boundaries.
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


def generate() -> str:
    owner = (COMPONENT / "i2csensor_apds9960.cpp").read_text()
    header = (COMPONENT / "i2csensor_apds9960.h").read_text()
    mutex = (COMPONENT / "System_Mutex.cpp").read_text()
    types = [extract_block(header, "struct APDSCache") + ";",
             extract_block((COMPONENT / "System_Mutex.h").read_text(), "struct SensorCacheGuard") + ";"]
    state = [re.search(r"(?m)^static[^\n]*\b" + name + r"\b[^\n]*;", owner)[0]
             for name in ("sApdsCache", "sApdsCacheMutex")]
    state.append(re.search(r"bool apdsCacheSnapshot\([^;]+;", header)[0])
    functions = [extract_block(mutex, "SensorCacheGuard::SensorCacheGuard("),
                 extract_block(mutex, "SensorCacheGuard::~SensorCacheGuard("),
                 extract_block(owner, "bool apdsCacheSnapshot("),
                 extract_block(owner, "static bool apdsInvalidateCache("),
                 extract_block((COMPONENT / "System_Utils.cpp").read_text(), "bool argWantsJson("),
                 extract_block((COMPONENT / "System_I2C.cpp").read_text(), "int sensorEnvelopeBegin("),
                 extract_block(owner, "int apdsBuildDataJSON(")]
    if "static const char* apdsGestureName(" in owner:
        functions.append(extract_block(owner, "static const char* apdsGestureName("))
    functions.append(extract_block(owner, "static const char* apdsReadSnapshot("))
    functions += [extract_block(owner, f"const char* cmd_apds{mode}(")
                  for mode in ("color", "proximity", "gesture", "read")]
    reset = extract_block(extract_block(owner, "bool apdsStartInternal()"), "// Clean up any stale cache")
    functions.append("static bool resetApdsReading() {\n" + reset + "\nreturn true;\n}")
    # The task's polling step is its own function (cadence, cache and events).
    # Older sources polled inline in the task; wrap that block in the same
    # signature (it has no separate gesture cadence) to run the same checks.
    if "static bool apdsPollDevice(" in owner:
        poll = extract_block(owner, "static bool apdsPollDevice(")
    else:
        block = extract_block(extract_block(owner, "void apdsTask("),
                              "if ((nowMs - lastApdsRead) >= apdsPollMs)")
        poll = ("static bool apdsPollDevice(unsigned long nowMs, unsigned long apdsPollMs, "
                "unsigned long& lastApdsRead, unsigned long&) {\n"
                "const uint8_t apdsBus = static_cast<uint8_t>(gSettings.apdsBus);\n"
                "do {\n" + block + "\n} while (false);\nreturn true;\n}")
    json_errors = "\n".join(re.findall(r"(?m)^#define SENSOR_JSON_(?:NOBUF|UNAVAILABLE)[^\n]+",
                                       (COMPONENT / "System_I2C.h").read_text()))
    for old in ("apdsColorPoll", "apdsProximityPoll", "apdsGesturePoll"):
        require(not re.search(r"\b" + old + r"\s*\(", owner + header),
                f"obsolete hardware read entry point remains: {old}")
    source = (HERE / "apds_read_commands_harness.cpp").read_text()
    for marker, text in {
        "// INSERT_TYPES": "\n\n".join(types),
        "// INSERT_STATE": "\n".join(state),
        "// INSERT_READER_FUNCTIONS": json_errors + "\n" + "\n\n".join(functions),
        "// INSERT_SAMPLING_FUNCTION": poll,
    }.items():
        require(source.count(marker) == 1, f"expected one insertion point: {marker}")
        source = source.replace(marker, text)
    return source


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-apds-read-commands-") as directory:
        source = Path(directory) / "test.cpp"
        source.write_text(generate())
        for sampling in (0, 1):
            binary = Path(directory) / f"test-{sampling}"
            subprocess.run([args.cxx, *flags, f"-DTEST_SAMPLING={sampling}",
                            "-I", str(HERE / "web_batch_stubs"), str(source), "-o", str(binary)],
                           check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
