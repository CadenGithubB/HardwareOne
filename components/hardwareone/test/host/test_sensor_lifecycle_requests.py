#!/usr/bin/env python3
"""Run production sensor commands, lifecycle worker and manager request queue.

Only owner startup, RTOS scheduling, hardware probes and output are controlled.
The complete worker and manager methods are extracted without rewriting their
logic. One-shot boundary callbacks inject close/cleanup races deterministically.
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
    manager = (COMPONENT / "System_I2C_Manager.cpp").read_text()
    controller = (COMPONENT / "System_I2C.cpp").read_text()
    header = (COMPONENT / "System_I2C.h").read_text()
    manager_definitions = [re.search(r"^#define I2C_WIRE1_DEFAULT_FREQ[^\n]+", manager, re.M)[0]]
    manager_definitions += [extract_block(manager, marker) for marker in (
        "I2CDeviceManager::I2CDeviceManager()", "I2CDevice::I2CDevice()",
        "I2CDeviceManager* I2CDeviceManager::getInstance()",
        "bool I2CDeviceManager::enqueueDeviceStart(", "bool I2CDeviceManager::enqueueDeviceStop(",
        "bool I2CDeviceManager::dequeueDeviceRequest(", "bool I2CDeviceManager::hasPendingDeviceStop(",
        "bool I2CDeviceManager::isInQueue(", "int I2CDeviceManager::getQueuePosition(",
        "int I2CDeviceManager::getQueueDepth(",
    )]
    controller_definitions = re.findall(r"^#define I2C_ADDR_[^\n]+", header, re.M)
    controller_definitions += [extract_block(header, marker) for marker in (
        "inline bool enqueueDeviceStart(", "inline bool isInQueue(",
        "inline int getQueuePosition(", "inline int getQueueDepth(",
    )]
    controller_definitions += [re.search(r"^static bool sSensorStopAwaitingCleanup[^\n]+", controller, re.M)[0]]
    controller_definitions += [extract_block(controller, marker) for marker in (
        "static uint8_t i2cAddressForDeviceType(", "static uint8_t i2cBusForDeviceType(",
        "static bool sensorIsRunning(", "static bool sensorHasTask(",
        "const char* cmd_sensorstart_queued(", "const char* deviceTypeDisplayName(",
        "bool requestSensorStop(", "static void applySensorStop(", "static void finishSensorStops()",
        "static bool startSensorInternal(", "static void broadcastStartedSensor(",
        "static void announceSensorStart(", "void sensorQueueProcessorTask(",
    )]
    harness = (HERE / "sensor_lifecycle_requests_harness.cpp").read_text()
    harness = (harness.replace("// INSERT_MANAGER_DEFINITIONS", "\n\n".join(manager_definitions))
               .replace("// INSERT_CONTROLLER_DEFINITIONS", "\n\n".join(controller_definitions)))
    with tempfile.TemporaryDirectory(prefix="hw1-sensor-lifecycle-") as directory:
        source = Path(directory) / "test.cpp"
        source.write_text(harness)
        for mesh in (0, 1):
            executable = Path(directory) / f"test-mesh-{mesh}"
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-Wno-keyword-macro", "-Wno-unused-parameter", "-Wno-switch",
                       f"-DENABLE_ESPNOW={mesh}", "-I", str(HERE / "i2c_stubs"),
                       "-I", str(COMPONENT), str(source), "-o", str(executable)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=60)
            subprocess.run([str(executable)], check=True, timeout=60)


if __name__ == "__main__":
    main()
