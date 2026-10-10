#!/usr/bin/env python3
"""Run real ToF poll/task code through the production I2C manager and health code.

RTOS waits inject stop requests deterministically. --baseline reproduces the old
false NACK from a pre-change checkout; ordinary runs require the corrected poll
and publication behavior. No copied manager or sensor algorithm is used.
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


def generate(component: Path) -> str:
    def read(name: str) -> str:
        # A bounded backup contains the changed owner files only. Unchanged
        # manager/cache/task implementations still come from the live checkout.
        path = component / name
        return (path if path.exists() else COMPONENT / name).read_text()
    owner = read("i2csensor_vl53l4cx.cpp")
    manager = read("System_I2C_Manager.cpp")
    helpers = read("System_I2C.h")
    mutex = read("System_Mutex.h")
    task = read("System_TaskUtils.h")
    definitions = [extract_block(manager, marker) for marker in (
        "I2CDeviceManager::I2CDeviceManager()", "I2CDevice::I2CDevice()",
        "void I2CDevice::init(", "I2CDevice* I2CDeviceManager::getDevice(",
        "I2CDevice* I2CDeviceManager::registerDevice(",
        "uint8_t I2CDeviceManager::probeDeviceStatus(",
        "bool I2CDeviceManager::clockStackPush(", "void I2CDeviceManager::clockStackPop(",
        "uint32_t I2CDeviceManager::clockStackTopOrDefault(", "void I2CDeviceManager::setBusClock(",
        "void I2CDeviceManager::updateMetrics(", "void I2CDeviceManager::updateHistogram(",
        "I2CErrorType classifyI2CError(", "void I2CDevice::recordSuccess(",
        "void I2CDevice::recordError(", "bool I2CDevice::isDegraded(")]
    definitions += [extract_block(helpers, marker) for marker in (
        "template<typename Func>\nauto i2cDeviceTransaction(uint8_t bus",
        "template<typename Func>\nvoid i2cDeviceTransactionVoid(uint8_t bus",
        "template<typename Func>\nauto i2cTaskWithTimeout(uint8_t bus",
        "inline bool i2cShouldAutoDisable(")]
    definitions += [extract_block(read("System_Mutex.cpp"), marker)
                    for marker in ("SensorCacheGuard::SensorCacheGuard(", "SensorCacheGuard::~SensorCacheGuard(")]
    definitions += [extract_block(read("System_MemUtil.h"),
                                  "template <typename T>\ninline void ps_delete("),
                    *[extract_block(owner, marker) for marker in (
                        "static bool tofInvalidateCache(", "bool tofPoll()", "void tofTask(void* parameter)")]]
    types = [extract_block(mutex, "struct SensorCacheGuard") + ";",
             extract_block(read("i2csensor_vl53l4cx.h"), "struct TofCache") + ";"]
    constants = [re.search(r"(?m)^#define I2C_WIRE1_DEFAULT_FREQ[^\n]+", manager)[0],
                 re.search(r"(?m)^static constexpr uint32_t CACHE_MUTEX_TIMEOUT_MS[^\n]+", mutex)[0],
                 re.search(r"(?m)^#define SENSOR_TASK_EXIT\([^\n]+\n(?:[^\n]*\\\n)*[^\n]*", task)[0]]
    harness = (HERE / "tof_poll_outcomes_harness.cpp").read_text()
    for marker, text in {"// INSERT_CONSTANTS": "\n".join(constants),
                         "// INSERT_TYPES": "\n".join(types),
                         "// INSERT_PRODUCTION": "\n\n".join(definitions)}.items():
        require(harness.count(marker) == 1, f"expected one insertion point: {marker}")
        harness = harness.replace(marker, text)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path, help="Pre-change repository snapshot to reproduce the cancelled-poll NACK")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.baseline / "components/hardwareone" if args.baseline else COMPONENT
    with tempfile.TemporaryDirectory(prefix="hw1-tof-poll-") as directory:
        source = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        source.write_text(generate(component))
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-keyword-macro", "-Wno-unused-function", "-Wno-unused-parameter",
                   f"-DTEST_BASELINE={int(bool(args.baseline))}", "-I", str(HERE / "i2c_stubs"),
                   "-I", str(component), "-I", str(COMPONENT), str(source), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
