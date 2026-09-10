#!/usr/bin/env python3
"""Execute production sensor-parent init and exact shutdown blocks with mocks.

The extracted code is the firmware implementation, not a test-local init/retry
algorithm. Driver internals, I2C, allocation, and task exit are mocked. Full
polling loops and vendor-owned nested allocations are intentionally outside
this parent-object migration test. Real fallback policy has separate MemUtil
tests in PSRAM-enabled and no-PSRAM builds.
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
    modules = {
        "imu": ("bno055", "gBNO055", "Adafruit_BNO055", "imu.obj", 1, 4),
        "apds": ("apds9960", "gAPDS9960", "Adafruit_APDS9960", "apds.obj", 1, 2),
        "servo": ("pca9685", "gPwmDriver", "Adafruit_PWMServoDriver", "servo.obj", 1, 1),
        "gamepad": ("seesaw", "gGamepadSeesaw", "Adafruit_seesaw", "input.gamepad.obj", 2, 0),
        "ano": ("ano_encoder", "gAnoSeesaw", "Adafruit_seesaw", "input.ano.obj", 1, 0),
    }
    sources = {}
    for name, (file, pointer, driver, tag, allocations, releases) in modules.items():
        source = (COMPONENT / f"i2csensor_{file}.cpp").read_text()
        sources[name] = source
        require(not re.search(rf"\bnew\s+{driver}\s*\(", source),
                f"{name} restored ordinary parent-object new")
        require(not re.search(rf"\bdelete\s+{pointer}\s*;", source),
                f"{name} mixes ordinary delete with PSRAM placement new")
        require(len(re.findall(rf"ps_alloc\s*\(\s*sizeof\s*\(\s*{driver}\s*\)\s*,\s*"
                               rf"AllocPref::PreferPSRAM\s*,\s*\"{re.escape(tag)}\"\s*\)", source)) == allocations,
                f"{name} parent allocations must use sizeof/type, PSRAM preference, and the tag")
        require(len(re.findall(rf"ps_delete\s*\(\s*{pointer}\s*\)\s*;\s*"
                               rf"{pointer}\s*=\s*nullptr\s*;", source)) == releases,
                f"{name} changed parent release/nulling count or retained-object ownership")
    init_definitions = []
    for module, function in (("imu", "imuInit"), ("apds", "apdsInit"),
                             ("servo", "servoInit"), ("gamepad", "gamepadInit"),
                             ("gamepad", "gamepadInitConnection"),
                             ("ano", "anoEncoderInit"), ("ano", "anoEncoderInitConnection")):
        init_definitions.append(extract_block(sources[module], f"\nbool {function}()"))
    shutdowns = []
    for module, task, condition, wrapper, prefix in (
        ("imu", "imuTask", "if (!gImuRunning)", "stopImuBranch", ""),
        ("apds", "apdsTask", "if (!anyEnabled)", "stopApdsBranch", "bool anyEnabled = false;"),
        ("gamepad", "inputTask", "if (!gInputRunning)", "stopGamepadBranch", ""),
        ("ano", "inputTask", "if (!gAnoEncoderEnabled)", "stopAnoBranch", ""),
    ):
        task_definition = extract_block(sources[module], f"void {task}(void* parameter)")
        branch = extract_block(task_definition, condition)
        shutdowns.append(f"static void {wrapper}() {{ {prefix}\n{branch}\n}}")
    memory = (COMPONENT / "System_MemUtil.h").read_text()
    destruction = extract_block(memory, "template <typename T>\ninline void ps_delete(")
    harness = (HERE / "sensor_parent_lifecycle_harness.cpp").read_text()
    for marker, definition in {
        "// INSERT_PRODUCTION_PS_DELETE_HERE": destruction,
        "// INSERT_PRODUCTION_SENSOR_CODE_HERE": "\n\n".join(init_definitions + shutdowns),
    }.items():
        require(harness.count(marker) == 1, f"expected one marker: {marker}")
        harness = harness.replace(marker, definition)
    with tempfile.TemporaryDirectory(prefix="hw1-sensor-parent-host-") as temp:
        generated = Path(temp) / "sensor_parent_lifecycle.cpp"
        executable = Path(temp) / "sensor_parent_lifecycle"
        generated.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   str(generated), "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
