#!/usr/bin/env python3
"""Run production ToF init/shutdown and ps_delete against host boundary mocks.

This extracts the actual tofInit, tofTask, and System_MemUtil.h ps_delete
definitions on every run. The driver, I2C execution boundary, allocation
backend, and FreeRTOS task exit are mocks. Tests do not claim real sensor or
PSRAM behavior; the existing memutil tests separately cover fallback policy.
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

# Reuse the tested C++ definition extractor; importing this runner performs no
# build/test or firmware reads. It ignores braces in comments and literals.
from test_web_batch_handlers import extract_block, require


HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    if not args.cxx:
        raise SystemExit("a host C++17 compiler is required")
    source = (COMPONENT / "i2csensor_vl53l4cx.cpp").read_text()
    memory = (COMPONENT / "System_MemUtil.h").read_text()
    init = extract_block(source, "\nbool tofInit()")
    task = extract_block(source, "void tofTask(void* parameter)")
    destruction = extract_block(memory, "template <typename T>\ninline void ps_delete(")
    require("new VL53L4CX" not in source, "ToF restored ordinary allocating new")
    require(not re.search(r"\bdelete\s+gVL53L4CX\s*;", source),
            "ToF mixes delete with PSRAM placement construction")
    require(len(re.findall(r"ps_delete\s*\(\s*gVL53L4CX\s*\)\s*;\s*"
                           r"gVL53L4CX\s*=\s*nullptr\s*;", source)) == 5,
            "every one of the five ownership releases must destroy/free and null the pointer")
    require(init.count("ps_delete(gVL53L4CX)") == 4,
            "reinit and all three driver-init failure paths must release ownership")
    require(task.count("ps_delete(gVL53L4CX)") == 1,
            "task shutdown must release its driver ownership")
    require(re.search(r"ps_alloc\s*\(\s*sizeof\s*\(\s*VL53L4CX\s*\)\s*,\s*"
                      r"AllocPref::PreferPSRAM\s*,\s*\"tof\.obj\"\s*\)", init) is not None,
            "ToF object must request its exact sizeof with tagged PSRAM preference")
    require(re.search(r"new\s*\(\s*\w+\s*\)\s*VL53L4CX\s*\(", init) is not None,
            "ToF object must be placement-constructed in allocated storage")
    harness = (HERE / "tof_psram_lifecycle_harness.cpp").read_text()
    replacements = {
        "// INSERT_PRODUCTION_PS_DELETE_HERE": destruction,
        "// INSERT_PRODUCTION_TOF_FUNCTIONS_HERE": init + "\n\n" + task,
    }
    for marker, definition in replacements.items():
        require(harness.count(marker) == 1, f"expected one harness marker: {marker}")
        harness = harness.replace(marker, definition)
    with tempfile.TemporaryDirectory(prefix="hw1-tof-lifecycle-host-") as temp:
        generated = Path(temp) / "tof_lifecycle.cpp"
        generated.write_text(harness)
        # The production XSHUT branch is conditional on the board's A1 pin.
        for pin_defined in (False, True):
            executable = Path(temp) / ("tof_with_a1" if pin_defined else "tof_without_a1")
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                       "-Wno-unused-parameter", "-pedantic", str(generated),
                       "-o", str(executable)]
            if pin_defined:
                command.insert(1, "-DA1=23")
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-g"]
            subprocess.run(command, check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
