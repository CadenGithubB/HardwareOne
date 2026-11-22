#!/usr/bin/env python3
"""A sensor is known by a value, not by comparing its name.

The rule (CONTRIBUTING.md, "Display text and reply text"):

1. An i2cSensors[] row (System_I2C.h) states what it is: `compiled`,
   `configuredBus`, `configuredAddress` and `deviceType`. Code reads those
   fields. `moduleName` is the word for commands, logs and the feature
   registry; nothing compares it with a literal.
2. A queue-managed I2C device is an I2cDeviceType. ramFlushIdForDevice() maps
   every value in a switch without a default, so a new device does not
   compile clean until it has its id.
3. A row of the G2 sensors page has a G2SensorKind; the page does not compare
   `featureId` with a literal.
4. The wire names of the remote sensor types are one table in enum order
   (System_ESPNow_Sensors.cpp), and bondStreamSetting() is the one place that
   maps a sensor type to its saved stream setting.

Reads the working tree. --component points the test at another component
directory.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

from test_shared_text_helpers import blank_comments, sources
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

NAME_COMPARE = re.compile(
    r'(?:\bstr(?:n|case|ncase)?cmp\s*\(\s*[\w.\->\[\]]*(?:moduleName|featureId)\s*,\s*"'
    r'|\b[\w.\->\[\]]*(?:moduleName|featureId)(?:\.c_str\(\))?\s*(?:==|!=)\s*")')
# Files whose `moduleName` is not a sensor: a settings module named in a remote
# edit request (remoteEditAllowed).
OTHER_MODULE_NAMES = {"System_BondedPeer.cpp"}
ROW_FIELDS = ("bool compiled;", "int (*configuredBus)();", "uint8_t (*configuredAddress)();",
              "I2cDeviceType deviceType;")
STREAM_SETTING = re.compile(r"gSettings\.bondStream\w+")
# Files that may name a bondStream setting: its owner, its settings rows and
# reset, the page that prints all of them, and the OLED table of its rows.
STREAM_SETTING_FILES = {"System_ESPNow_Sensors.cpp", "System_ESPNow.cpp", "System_Settings.h",
                        "WebPage_Bond.cpp", "OLED_Mode_Remote.cpp"}


def measure(component: Path) -> tuple[int, list[str]]:
    failures: list[str] = []
    texts = {relative: blank_comments(text, keep_strings=True)
             for relative, text in sources(component, (".h", ".cpp", ".c", ".inc"))}

    for relative, code in sorted(texts.items()):
        if relative in OTHER_MODULE_NAMES:
            continue
        for match in NAME_COMPARE.finditer(code):
            number = code.count("\n", 0, match.start()) + 1
            failures.append(f"{relative}:{number} compares a module name or feature id with a literal; "
                            f"branch on the row's fields or its kind\n    "
                            f"{code.splitlines()[number - 1].strip()[:120]}")

    header = texts.get("System_I2C.h", "")
    entry = extract_block(header, "struct I2cSensorEntry") if "struct I2cSensorEntry" in header else ""
    for field in ROW_FIELDS:
        if field not in entry:
            failures.append(f"System_I2C.h: I2cSensorEntry must declare `{field}`")
    source = texts.get("System_I2C.cpp", "")
    start = source.find("const I2cSensorEntry i2cSensors[] = {")
    table = source[start:source.find("\n};", start)] if start >= 0 else ""
    rows = len(re.findall(r"^\s*\{ 0x", table, re.M))
    # A row ends with its I2cDeviceType, a seesaw row with its product id after it.
    typed = len(re.findall(r"I2C_DEVICE_\w+(?:, kSeesawProduct\w+)? \}", table))
    if rows == 0 or rows != typed:
        failures.append(f"System_I2C.cpp: {rows} i2cSensors[] rows, {typed} end with their I2cDeviceType")

    # The device types live in the instance core (System_I2CInstanceCore.h).
    manager = texts.get("System_I2CInstanceCore.h", "")
    devices = re.findall(r"^\s*(I2C_DEVICE_\w+)\b", extract_block(manager, "enum I2cDeviceType"), re.M) \
        if "enum I2cDeviceType" in manager else []
    ramflush = texts.get("System_RamFlush.cpp", "")
    marker = "RamFlushFeatureId ramFlushIdForDevice(I2cDeviceType device)"
    mapping = extract_block(ramflush, marker) if marker in ramflush else ""
    if not mapping:
        failures.append("System_RamFlush.cpp: ramFlushIdForDevice(I2cDeviceType) is missing")
    else:
        if re.search(r"\bdefault\s*:", mapping):
            failures.append("System_RamFlush.cpp: ramFlushIdForDevice must not have a default case")
        for device in devices:
            if f"case {device}:" not in mapping:
                failures.append(f"System_RamFlush.cpp: ramFlushIdForDevice has no case for {device}")
    if len(devices) < 9:
        failures.append(f"System_I2CInstanceCore.h: found only {len(devices)} I2cDeviceType values")

    remote = texts.get("System_ESPNow_Sensors.cpp", "")
    # The remote sensor names have one owner: the table of
    # System_ESPNow_SensorCore.h, sized by REMOTE_SENSOR_MAX so a type without
    # a row does not compile (espnow_sensor_core_tests checks every row).
    core = texts.get("System_ESPNow_SensorCore.h", "")
    if "inline constexpr SensorRow kSensorTable[REMOTE_SENSOR_MAX] = {" not in core:
        failures.append("System_ESPNow_SensorCore.h: kSensorTable[REMOTE_SENSOR_MAX] is missing")
    if "return espnowsensor::sensorName((int)type);" not in remote or "kRemoteSensorNames" in remote:
        failures.append("System_ESPNow_Sensors.cpp: sensorTypeToString must read the one sensor table")
    if remote.count("bool* bondStreamSetting(RemoteSensorType type) {") != 1:
        failures.append("System_ESPNow_Sensors.cpp must define bondStreamSetting(RemoteSensorType) once")
    for relative, code in sorted(texts.items()):
        if relative in STREAM_SETTING_FILES:
            continue
        match = STREAM_SETTING.search(code)
        if match:
            number = code.count("\n", 0, match.start()) + 1
            failures.append(f"{relative}:{number} names {match.group(0)}; ask bondStreamSetting()")
    return len(texts), failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    component = args.component.resolve()

    checked, failures = measure(component)
    if not checked:
        raise SystemExit(f"sensor identity: no sources under {component}")
    for failure in failures:
        print(f"FAIL {failure}", file=sys.stderr)
    if failures:
        raise SystemExit(f"sensor identity: {len(failures)} violation(s)")
    print(f"sensor identity: {checked} source files, no module name or feature id compared with a literal")


if __name__ == "__main__":
    main()
