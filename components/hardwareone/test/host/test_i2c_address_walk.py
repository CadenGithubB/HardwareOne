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
