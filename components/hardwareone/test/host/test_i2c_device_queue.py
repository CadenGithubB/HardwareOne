#!/usr/bin/env python3
"""Exercise production I2C start/stop admission with a controlled RTOS mutex."""
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
    source = (COMPONENT / "System_I2C_Manager.cpp").read_text()
    constant = re.search(r"^#define I2C_WIRE1_DEFAULT_FREQ[^\n]+", source, re.M)
    require(constant is not None, "missing production default bus clock")
    definitions = [constant.group()] + [extract_block(source, marker) for marker in (
        "I2CDeviceManager::I2CDeviceManager()", "I2CDevice::I2CDevice()",
        "bool I2CDeviceManager::enqueueDeviceStart(",
        "bool I2CDeviceManager::enqueueDeviceStop(",
        "bool I2CDeviceManager::dequeueDeviceRequest(",
        "bool I2CDeviceManager::hasPendingDeviceStop(",
        "bool I2CDeviceManager::isInQueue(",
        "int I2CDeviceManager::getQueuePosition(",
        "int I2CDeviceManager::getQueueDepth(",
    )]
    harness = (HERE / "i2c_device_queue_harness.cpp").read_text()
    marker = "// INSERT_PRODUCTION_DEFINITIONS"
    require(harness.count(marker) == 1, "expected one production insertion point")
    harness = harness.replace(marker, "\n\n".join(definitions))
    with tempfile.TemporaryDirectory(prefix="hw1-i2c-device-queue-") as directory:
        generated = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        generated.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-keyword-macro", "-I", str(HERE / "i2c_stubs"),
                   "-I", str(COMPONENT), str(generated), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
