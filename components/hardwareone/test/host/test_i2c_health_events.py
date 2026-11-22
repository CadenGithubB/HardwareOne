#!/usr/bin/env python3
"""Run production I2C health edges through the production notification rules."""
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
    manager = (COMPONENT / "System_I2C_Manager.cpp").read_text()
    notifications = (COMPONENT / "System_Notifications.cpp").read_text()
    constant = re.search(r"^#define I2C_WIRE1_DEFAULT_FREQ[^\n]+", manager, re.M)
    require(constant is not None, "missing production default bus clock")
    definitions = [constant.group()] + [extract_block(manager, marker) for marker in (
        "I2cDeviceManager::I2cDeviceManager()", "I2cDevice::I2cDevice()",
        "void I2cDevice::init(", "static void postDeviceHealthEvent(",
        "static void announceUnresponsive(", "void I2cDevice::recordSuccess(",
        "void I2cDevice::recordError(", "bool I2cDevice::isDegraded(",
        "void I2cDevice::attemptRecovery(",
    )] + [extract_block(notifications, marker) for marker in (
        "static NotifRule notifDefaultRuleFor(", "void notifFormatEvent(",
    )]
    harness = (HERE / "i2c_health_events_harness.cpp").read_text()
    harness = harness.replace("// INSERT_PRODUCTION_DEFINITIONS", "\n\n".join(definitions))
    with tempfile.TemporaryDirectory(prefix="hw1-i2c-health-") as directory:
        source = Path(directory) / "test.cpp"
        source.write_text(harness)
        # The second build gives the encoder four instances (System_I2CInstanceCore.h).
        for instances in (1, 4):
            binary = Path(directory) / f"test-{instances}"
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-Wno-keyword-macro", "-I", str(HERE / "i2c_stubs"),
                       "-I", str(COMPONENT), str(source),
                       str(COMPONENT / "System_EventCatalog.cpp"), "-o", str(binary)]
            if instances > 1:
                command.insert(1, f"-DHW1_HOST_I2C_TEST_ENCODER_MAX={instances}")
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-g"]
            subprocess.run(command, check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
