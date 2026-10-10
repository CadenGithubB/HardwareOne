#!/usr/bin/env python3
"""Execute shipping Map track ownership and commands with GPS on/off.

The Map owner, parser and logger session identity functions are extracted
unchanged. GPS requests, logger file startup, allocation and VFS are boundaries.
Snapshots, lifecycle publication, serialization and rendering use the real owner.
FreeRTOS mutexes are backed by host mutexes, including deterministic reentrant
boundaries and concurrent reader/writer checks.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_mqtt_boolean_commands import extract_macro
from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generated():
    maps = (COMPONENT / "System_Maps.cpp").read_text()
    header = (COMPONENT / "System_Maps.h").read_text()
    logger = (COMPONENT / "System_SensorLogging.cpp").read_text()
    command = (COMPONENT / "System_Command.cpp").read_text()
    types = [header[header.index("#define MAX_TRACK_POINTS"):
                    header.index("// =============================================================================\n// Waypoint System")]]
    types += [extract_block((COMPONENT / "System_Command.h").read_text(),
                           "class CommandArgs") + ";"]
    owner_start = maps.index("// GPS Track Manager Implementation")
    owner_start = maps.index("\n\n", owner_start) + 2
    owner_end = maps.index("// Stitch several GPS/sensor capture logs", owner_start)
    definitions = [extract_macro((COMPONENT / "System_Utils.h").read_text(),
                                 "RETURN_VALID_IF_VALIDATE_CSTR"),
                   "const String CommandArgs::empty_;"]
    definitions += [extract_block(command, marker) for marker in (
        "CommandArgs::CommandArgs(", "void CommandArgs::parse()",
        "const String& CommandArgs::arg(")]
    definitions += [extract_block(logger, marker) for marker in (
        "static void sensorLogPublishStarted(", "static bool sensorLogMarkStopped(",
        "uint32_t sensorLogCurrentSessionId(", "bool sensorLogStopSession(")]
    definitions += [maps[owner_start:owner_end]]
    harness = (HERE / "map_live_tracking_harness.cpp").read_text()
    return harness.replace("// INSERT_TYPES", "\n\n".join(types)).replace(
        "// INSERT_PRODUCTION", "\n\n".join(definitions))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    sanitizers = parser.add_mutually_exclusive_group()
    sanitizers.add_argument("--sanitize", action="store_true")
    sanitizers.add_argument("--thread-sanitize", action="store_true",
                            help="run reader/writer checks with ThreadSanitizer")
    args = parser.parse_args()
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-pthread"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    if args.thread_sanitize:
        flags += ["-fsanitize=thread", "-fno-omit-frame-pointer"]
    with tempfile.TemporaryDirectory(prefix="hw1-map-live-") as directory:
        root = Path(directory)
        (root / "Arduino.h").write_text((HERE / "web_batch_stubs/Arduino.h").read_text())
        unit = root / "test.cpp"
        unit.write_text(generated())
        for gps in (0, 1):
            exe = root / f"test-{gps}"
            subprocess.run([args.cxx, *flags, f"-DENABLE_GPS_SENSOR={gps}",
                            "-I", directory, str(unit), "-o", str(exe)],
                           check=True, timeout=120)
            subprocess.run([str(exe)], check=True, timeout=60)


if __name__ == "__main__":
    main()
