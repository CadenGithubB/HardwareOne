#!/usr/bin/env python3
"""Run production address-only discovery with Wire and RTOS boundary substitutes."""
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


def check_scanners() -> None:
    for name in ("System_I2C.cpp", "System_I2C.h", "System_I2C_Manager.cpp",
                 "System_I2C_Manager.h", "OLED_Mode_Sensors.cpp"):
        code = (COMPONENT / name).read_text()
        require(re.search(r"\b(?:i2cConfirmPresent|i2cConfirmRead|confirmRead)\s*\(", code) is None,
                f"{name}: discovery must not demand an unsolicited data read")
    for name, marker in (
        ("System_I2C.cpp", "const char* cmd_i2cscan("),
        ("System_I2C.cpp", "void detectHardware("),
        ("System_I2C.cpp", "static void scanBusForDevicesSmart("),
        ("OLED_Mode_Sensors.cpp", "static void i2cDiagRunScan("),
    ):
        source = (COMPONENT / name).read_text()
        # Smart scanning has a forward declaration; select its definition.
        definitions = list(re.finditer(re.escape(marker) + r"[^;{]+\{", source))
        require(len(definitions) == 1, f"expected one definition: {marker}")
        body = extract_block(source[definitions[0].start():], marker)
        require(re.search(r"\b(?:i2cPingAddress|i2cProbeAddress|probeAddress)\s*\(", body) is not None,
                f"{marker}: scanner must reuse the shared address-only probe")
        require("PollPauseGuard" in body,
                f"{marker}: scans must pause polling through the shared guard")
        require(re.search(r"(?:->|\.)\s*(?:begin|setClock|beginTransmission|endTransmission|requestFrom|write|read)\s*\(", body) is None,
                f"{marker}: Wire transport and clock policy belong to the manager")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    manager = (COMPONENT / "System_I2C_Manager.cpp").read_text()
    header = (COMPONENT / "System_I2C.h").read_text()
    constant = re.search(r"^#define I2C_WIRE1_DEFAULT_FREQ[^\n]+", manager, re.M)
    require(constant is not None, "missing production default bus clock")
    definitions = [constant.group()] + [extract_block(manager, marker) for marker in (
        "I2CDeviceManager::I2CDeviceManager()", "I2CDevice::I2CDevice()",
        "void I2CDevice::init(", "uint8_t I2CDeviceManager::probeAddress(",
        "bool I2CDeviceManager::clockStackPush(", "void I2CDeviceManager::clockStackPop(",
        "uint32_t I2CDeviceManager::clockStackTopOrDefault(", "void I2CDeviceManager::setBusClock(",
    )] + [extract_block(header, marker) for marker in (
        "inline uint8_t i2cProbeAddress(", "inline bool i2cPingAddress(",
    )]
    harness = (HERE / "i2c_probe_harness.cpp").read_text()
    harness = harness.replace("// INSERT_PRODUCTION_DEFINITIONS", "\n\n".join(definitions))
    with tempfile.TemporaryDirectory(prefix="hw1-i2c-probe-") as directory:
        source = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        source.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-Wno-keyword-macro", "-I", str(HERE / "i2c_stubs"),
                   "-I", str(COMPONENT), str(source), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)
    check_scanners()
    print("I2C scanner integration guards passed")


if __name__ == "__main__":
    main()
