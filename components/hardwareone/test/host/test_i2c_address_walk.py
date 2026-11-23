#!/usr/bin/env python3
"""The firmware's address walk on a fixture table.

forEachI2cAddressUse (every address in use, each with its row, the extra
rows of a two-chip type included), isSensorConnected (the boot scan matched
by bus and address) and i2cRowOwnerByName are extracted from System_I2C.cpp
unchanged and run against i2c_address_walk_harness.cpp's table and settings.
test_i2c_instance_core covers the same rules in their array forms.
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
    controller = (COMPONENT / "System_I2C.cpp").read_text()
    header = (COMPONENT / "System_I2C.h").read_text()
    types = re.findall(r"^#define I2C_ADDR_\w+ +0x[0-9A-Fa-f]+", header, re.M)
    types += [extract_block(header, "struct I2cSensorEntry") + ";",
              extract_block(header, "struct ConnectedDevice") + ";"]
    require("template <typename Visit>\nstatic void forEachI2cAddressUse(" in controller,
            "forEachI2cAddressUse is the template the walk is")
    production = [extract_block(controller, marker) for marker in (
        "static uint8_t configuredBusForSensor(", "static bool driverServesInstance(",
        "static uint8_t i2cAddressForDeviceType(", "static uint8_t i2cBusForDeviceType(",
    )]
    production.append("template <typename Visit>\n" +
                      extract_block(controller, "static void forEachI2cAddressUse("))
    production += [extract_block(controller, marker) for marker in (
        "bool isSensorConnected(const char* moduleName)", "I2cAddressOwner i2cRowOwnerByName(",
    )]
    # The BNO055's one address is its imuAddress setting (addressAt): its row
    # has no alternate, so detect, the boot scan and the sensor info never
    # count an answer at the other address (or a ToF at 0x29) as the IMU.
    bno055 = re.search(r'\{ 0x28, "BNO055", "9-DOF IMU", "Bosch", (\w+), (0x[0-9A-Fa-f]+),', controller)
    require(bno055 is not None, "the BNO055 row of i2cSensors")
    require(bno055[1] == "false" and int(bno055[2], 16) == 0,
            "the BNO055 row must not name an alternate address (imuAddress replaces it)")
    # The Adafruit 4517's two chips: each row at its own address setting
    # (imuAddress, imuMagAddress) with no alternate, so the walk claims both
    # chips and only the configured addresses.
    for name, address_at in (("LSM6DSOX", "IMU_ADDRESS"), ("LIS3MDL", "IMU_MAG_ADDRESS")):
        row = re.search(r'\{ 0x[0-9A-Fa-f]+, "' + name + r'", "[^"]*", "ST", (\w+), (0x[0-9A-Fa-f]+),[^\n]*\n([^\n]*)',
                        controller)
        require(row is not None, f"the {name} row of i2cSensors")
        require(row[1] == "false" and int(row[2], 16) == 0, f"the {name} row must not name an alternate address")
        require(f", {address_at}, I2C_DEVICE_IMU }}" in row[3] and "ENABLE_IMU_LSM6DSOX" in row[3],
                f"the {name} row is the 4517 build's IMU row at its address setting ({address_at})")
    harness = (HERE / "i2c_address_walk_harness.cpp").read_text()
    harness = (harness.replace("// INSERT_TYPES", "\n\n".join(types))
               .replace("// INSERT_PRODUCTION", "\n\n".join(production)))
    with tempfile.TemporaryDirectory(prefix="hw1-i2c-address-walk-") as directory:
        source = Path(directory) / "test.cpp"
        source.write_text(harness)
        binary = Path(directory) / "test"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                   "-Wno-missing-field-initializers", "-I", str(COMPONENT), str(source), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=60)
        subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
