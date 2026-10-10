#!/usr/bin/env python3
"""Execute shipping Map track ownership and commands with GPS on/off.

The Map owner, parser and logger session identity functions are extracted
unchanged. GPS requests, logger file startup, allocation and VFS are boundaries.
This does not claim concurrent GPS/cache/render buffer safety.
"""
import argparse
import os
from pathlib import Path
import re
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
    types = [re.search(r"(?m)^#define MAX_TRACK_POINTS.*$", header).group()]
    types += [extract_block(header, marker) + ";" for marker in (
        "struct GPSTrackPoint", "struct GPSTrackBounds", "struct GPSTrackStats",
        "enum TrackValidation", "class GPSTrackManager")]
    types += [extract_block((COMPONENT / "System_Command.h").read_text(),
                           "class CommandArgs") + ";"]
    state = maps[maps.index("GPSTrackPoint* GPSTrackManager::_points"):
                 maps.index("// Haversine formula for distance")]
    definitions = [extract_macro((COMPONENT / "System_Utils.h").read_text(),
                                 "RETURN_VALID_IF_VALIDATE_CSTR"),
                   "const String CommandArgs::empty_;", state]
    definitions += [extract_block(command, marker) for marker in (
        "CommandArgs::CommandArgs(", "void CommandArgs::parse()",
        "const String& CommandArgs::arg(")]
    definitions += [extract_block(logger, marker) for marker in (
        "static void sensorLogPublishStarted(", "static bool sensorLogMarkStopped(",
        "uint32_t sensorLogCurrentSessionId(", "bool sensorLogStopSession(")]
    definitions += [extract_block(maps, marker) for marker in (
        "float GPSTrackManager::haversineDistance(",
        "void GPSTrackManager::calculateStats(", "bool GPSTrackManager::parseGPSLine(",
        "void GPSTrackManager::calculateBounds(", "bool GPSTrackManager::loadTrack(",
        "bool GPSTrackManager::clearTrack(", "bool GPSTrackManager::deleteTrackFile(",
        "bool GPSTrackManager::isLiveTracking(",
        "const char* GPSTrackManager::startLiveTracking(",
        "const char* GPSTrackManager::stopLiveTracking(",
        "bool GPSTrackManager::appendPoint(",
        "const char* GPSTrackManager::getValidationMessage(",
        "const char* cmd_gpstrack(")]
    harness = (HERE / "map_live_tracking_harness.cpp").read_text()
    return harness.replace("// INSERT_TYPES", "\n\n".join(types)).replace(
        "// INSERT_PRODUCTION", "\n\n".join(definitions))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
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
