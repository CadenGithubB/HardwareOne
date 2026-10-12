#!/usr/bin/env python3
"""Exercise complete production sensor starters across resource/RTOS failures.

The cache value types and all nine sensor slots (both input backends) are extracted fresh.
Sensor libraries, allocation, bus admission and task handshakes are controlled
boundaries; this does not simulate hardware or scheduler timing.
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
    owners = (
        ("vl53l4cx", "tof", "tof"), ("bno055", "imu", "imu"),
        ("mlx90640", "thermal", "thermal"), ("pa1010d", "gps", "gps"),
        ("apds9960", "apds", "apds"), ("sths34pf80", "presence", "presence"),
        ("ds3231", "rtc", "rtc"), ("rda5807", "fmRadio", "fmRadio"),
        ("seesaw", "input", "gamepad"), ("ano_encoder", "input", "ano"),
    )
    definitions = []
    logs = set()
    for file, function, unique in owners:
        source = (COMPONENT / f"i2csensor_{file}.cpp").read_text()
        code = extract_block(source, f"bool {function}StartInternal()")
        # The two alternative input backends export the same production name;
        # only their declaration names change to exercise both in one binary.
        code = code.replace(f"bool {function}StartInternal()", f"bool {unique}StartInternal()", 1)
        # The APDS rollback deletes its driver through the owner's lock helper.
        if file == "apds9960" and "static bool apdsReleaseDriver(" in source:
            code = extract_block(source, "static bool apdsReleaseDriver(") + "\n\n" + code
        logs.update(re.findall(r"\b((?:DEBUG|INFO|ERROR)[A-Z_]*F|DEBUGF_QUEUE)\s*\(", code))
        definitions.append(code)
    types = []
    for file, names in (
        ("vl53l4cx", ("TofCache",)), ("bno055", ("ImuCache",)),
        ("pa1010d", ("GPSCache",)), ("apds9960", ("APDSCache",)),
        ("sths34pf80", ("PresenceCache",)), ("ds3231", ("RTCDateTime", "RTCCache")),
        ("ano_encoder", ("AnoEncoderCache",)),
    ):
        source = (COMPONENT / f"i2csensor_{file}.h").read_text()
        types.extend(extract_block(source, f"struct {name}") + ";" for name in names)
    harness = (HERE / "sensor_start_outcomes_harness.cpp").read_text()
    for marker, text in {
        "// INSERT_PRODUCTION_TYPES": "\n\n".join(types),
        "// INSERT_PRODUCTION_STARTERS": "\n\n".join(definitions),
        "// INSERT_LOG_BOUNDARIES": "\n".join(f"#define {name}(...) logBoundary(__VA_ARGS__)" for name in sorted(logs)),
    }.items():
        require(harness.count(marker) == 1, f"expected one insertion point: {marker}")
        harness = harness.replace(marker, text)
    with tempfile.TemporaryDirectory(prefix="hw1-sensor-start-outcomes-") as directory:
        source = Path(directory) / "test.cpp"
        binary = Path(directory) / "test"
        source.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   str(source), "-o", str(binary)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
