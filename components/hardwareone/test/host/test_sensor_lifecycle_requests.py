#!/usr/bin/env python3
"""Run production sensor commands, lifecycle worker and manager request queue.

Only owner startup, RTOS scheduling, hardware probes and output are controlled.
The complete worker and manager methods are extracted without rewriting their
logic. One-shot boundary callbacks inject close/cleanup races deterministically.

Also covered here, on the same production code: the <x>enabled gate of every
open (requestSensorStart) and the automatic restart of a sensor that switched
itself off (the worker's tick with System_SensorRestartCore.h).
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
        "I2cDeviceManager::I2cDeviceManager()", "I2cDevice::I2cDevice()",
        "I2cDeviceManager* I2cDeviceManager::getInstance()",
        "bool I2cDeviceManager::enqueueDeviceStart(", "bool I2cDeviceManager::enqueueDeviceStop(",
        "bool I2cDeviceManager::dequeueDeviceRequest(", "bool I2cDeviceManager::hasPendingDeviceStop(",
        "bool I2cDeviceManager::isInQueue(", "int I2cDeviceManager::getQueuePosition(",
        "int I2cDeviceManager::getQueueDepth(",
    )]
    controller_definitions = re.findall(r"^#define I2C_ADDR_[^\n]+", header, re.M)
    # The start helper marks admitted starts outstanding (out of line); older
    # sources had it inline in the header.
    outstanding = re.search(r"^static std::atomic<bool> sSensorStartOutstanding[^\n]+", controller, re.M)
    if outstanding:
        # Indexed by instance slot (System_I2CInstanceCore.h): instance 1 in the
        # array above, the extra instances in a PSRAM companion.
        controller_definitions += ["#define HAS_START_OUTSTANDING 1", outstanding[0],
                                   re.search(r"^EXT_RAM_BSS_ATTR static std::atomic<bool> "
                                             r"sSensorStartOutstandingExtra[^\n]+", controller, re.M)[0],
                                   extract_block(controller, "static std::atomic<bool>& sensorStartOutstanding("),
                                   extract_block(controller, "bool enqueueDeviceStart(I2cDeviceType device")]
    else:
        controller_definitions += [extract_block(header, "inline bool enqueueDeviceStart(")]
    if re.search(r"const char\* eventTag,\s*const char\* hint", header):
        controller_definitions += ["#define HAS_START_HINT 1"]
    controller_definitions += [extract_block(header, marker) for marker in (
        "inline bool isInQueue(", "inline int getQueuePosition(", "inline int getQueueDepth(",
    )]
    controller_definitions += [extract_block(header, "enum class SensorStartRefusal") + ";"]
    # The production declarations, with their trailing instance argument (n = 1).
    for name in ("enqueueDeviceStart", "requestSensorStop", "requestSensorStart", "noteSensorAutoDisabled",
                 "cmd_sensorstart_queued", "cmd_sensorstop_queued", "deviceTypeDisplayName"):
        declaration = re.search(rf"^(?:bool|void|const char\*) {name}\([^;]*\);", header, re.M | re.S)
        require(declaration is not None, f"missing production declaration of {name}")
        controller_definitions.append(declaration[0])
    controller_definitions += [re.search(r"^static bool sSensorStopAwaitingCleanup[^\n]+", controller, re.M)[0],
                               re.search(r"^EXT_RAM_BSS_ATTR static bool sSensorStopAwaitingCleanupExtra[^\n]+",
                                         controller, re.M)[0],
                               extract_block(controller, "static bool& sensorStopAwaitingCleanup(")]
    # The start-at-boot switch table and the automatic restart (state, flags, tick).
    restart_state = re.findall(r"^EXT_RAM_BSS_ATTR static [^\n]*sSensorRestart[^\n]+", controller, re.M)
    require(len(restart_state) == 5, "expected the five automatic-restart state lines in PSRAM")
    controller_definitions += [
        extract_block(controller, "struct SensorSwitchRow") + ";",
        extract_block(controller, "static const SensorSwitchRow kSensorSwitches[]") + ";",
        re.search(r"^enum : uint8_t \{ kRestartHandNone[^\n]+", controller, re.M)[0],
        *restart_state,
    ]
    controller_definitions += [extract_block(controller, marker) for marker in (
        "static const SensorSwitchRow* sensorSwitchRow(", "static bool sensorAutoStartFor(",
        "void noteSensorAutoDisabled(", "static bool sensorRestartClosedByHand(",
        "static void sensorRestartOpenedByHand(",
        "static bool driverServesInstance(",
        "static uint8_t i2cAddressForDeviceType(", "static uint8_t i2cBusForDeviceType(",
        "static bool sensorIsRunning(", "static bool sensorHasTask(",
        "bool requestSensorStart(", "const char* sensorStartRefusalText(",
        "const char* cmd_sensorstart_queued(", "static const char* sensorOpenCommand(",
        "static bool sensorStopHasWork(", "const char* cmd_sensorstop_queued(",
        "const char* deviceTypeDisplayName(",
        "bool requestSensorStop(", "static void applySensorStop(", "static void finishSensorStops()",
        "static bool startSensorInternal(", "static void broadcastStartedSensor(",
        "static void announceSensorStart(",
        "static bool sensorRestartGateOpen(", "static bool sensorRestartStartSettled(",
        "static bool sensorRestartStartWithdrawn(",
        "static void sensorRestartAttempt(", "static void sensorRestartTick(",
        "void sensorQueueProcessorTask(",
    )]
    # A real start command, for the hint-only-after-acceptance contract.
    controller_definitions.append(extract_block((COMPONENT / "i2csensor_pa1010d.cpp").read_text(),
                                                "const char* cmd_gpsstart("))
    harness = (HERE / "sensor_lifecycle_requests_harness.cpp").read_text()
    harness = (harness.replace("// INSERT_MANAGER_DEFINITIONS", "\n\n".join(manager_definitions))
               .replace("// INSERT_CONTROLLER_DEFINITIONS", "\n\n".join(controller_definitions)))
    with tempfile.TemporaryDirectory(prefix="hw1-sensor-lifecycle-") as directory:
        source = Path(directory) / "test.cpp"
        source.write_text(harness)
        # The third build gives the encoder two instances (System_I2CInstanceCore.h)
        # and adds the per-instance cases.
        for mesh, instances in ((0, 1), (1, 1), (0, 2)):
            executable = Path(directory) / f"test-mesh-{mesh}-instances-{instances}"
            extra = [f"-DHW1_HOST_I2C_TEST_ENCODER_MAX={instances}"] if instances > 1 else []
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-Wno-keyword-macro", "-Wno-unused-parameter", "-Wno-switch", *extra,
                       f"-DENABLE_ESPNOW={mesh}", "-I", str(HERE / "i2c_stubs"),
                       "-I", str(COMPONENT), str(source), "-o", str(executable)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=60)
            subprocess.run([str(executable)], check=True, timeout=60)


if __name__ == "__main__":
    main()
