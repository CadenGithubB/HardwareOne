#!/usr/bin/env python3
"""Exercise the production ToF command, snapshot and JSON against cache states.

RTOS admission and command presentation are controlled boundaries. No sensor
driver or I2C substitute is linked: read commands must use the owner's cache.
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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    owner = (COMPONENT / "i2csensor_vl53l4cx.cpp").read_text()
    header = (COMPONENT / "i2csensor_vl53l4cx.h").read_text()
    mutex_header = (COMPONENT / "System_Mutex.h").read_text()
    mutex_source = (COMPONENT / "System_Mutex.cpp").read_text()
    i2c_header = (COMPONENT / "System_I2C.h").read_text()
    require("readToFDistance" not in owner, "superseded direct hardware reader remains")
    types = [extract_block(header, "struct TofCache") + ";",
             extract_block(mutex_header, "struct SensorCacheGuard") + ";"]
    state = [re.search(r"(?m)^static constexpr uint32_t CACHE_MUTEX_TIMEOUT_MS[^\n]+", mutex_header)[0]]
    for name in ("sTofCache", "sTofCacheMutex"):
        state.append(re.search(r"(?m)^static[^\n]*\b" + name + r"\b[^\n]*;", owner)[0])
    for name in ("SENSOR_JSON_NOBUF", "SENSOR_JSON_UNAVAILABLE"):
        state.append(re.search(r"(?m)^#define " + name + r"[^\n]+", i2c_header)[0])
    functions = [extract_block(mutex_source, "SensorCacheGuard::SensorCacheGuard("),
                 extract_block(mutex_source, "SensorCacheGuard::~SensorCacheGuard("),
                 extract_block(owner, "bool tofCacheSnapshot("),
                 extract_block((COMPONENT / "System_I2C.cpp").read_text(), "int sensorEnvelopeBegin("),
                 extract_block(owner, "int tofBuildDataJSON("),
                 extract_block(owner, "const char* cmd_tof(")]
    harness = (HERE / "tof_read_command_harness.cpp").read_text()
    # The guard's default argument requires the production timeout constant
    # before the type; the cache storage requires the cache type first.
    replacements = {"// INSERT_TIMEOUT": state[0],
                    "// INSERT_TYPES": "\n\n".join(types),
                    "// INSERT_STATE": "\n".join(state[1:]),
                    "// INSERT_FUNCTIONS": "\n\n".join(functions)}
    for marker, text in replacements.items():
        require(harness.count(marker) == 1, f"expected one insertion point: {marker}")
        harness = harness.replace(marker, text)
    with tempfile.TemporaryDirectory(prefix="hw1-tof-read-command-") as directory:
        source = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        source.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                   "-Wno-unused-parameter", "-pedantic", str(source), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
