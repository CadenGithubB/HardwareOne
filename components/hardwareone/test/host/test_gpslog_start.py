#!/usr/bin/env python3
"""Execute production GPS-log orchestration and logger startup on the host.

GPS commands use the real argument parser, GPS auto-start handler and queued
start adapter. Sensor-command execution and GPS queue outcomes are boundaries.
The owner unit compiles the real typed start, saved-setting restoration and boot
policy; VFS, sealing, clock, path shaping and persistence are host boundaries.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_mqtt_boolean_commands import extract_macro
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def generated(gps: bool, baseline: Path | None = None) -> str:
    owner = (COMPONENT / "System_SensorLogging.cpp").read_text()
    gps_source = ((baseline or COMPONENT) / "i2csensor_pa1010d.cpp").read_text()
    header = (COMPONENT / "System_SensorLogging.h").read_text()
    command = (COMPONENT / "System_Command.cpp").read_text()
    types = ["\n".join(re.findall(r"(?m)^#define LOG_.*$", header)),
             extract_block(header, "enum SensorLogFormat") + ";",
             extract_block((COMPONENT / "System_Command.h").read_text(), "class CommandArgs") + ";"]
    definitions = [extract_macro((COMPONENT / "System_Utils.h").read_text(), "RETURN_VALID_IF_VALIDATE_CSTR"),
                   extract_block((COMPONENT / "System_Settings.h").read_text(), "template<typename T>\ninline void setSetting("),
                   "const String CommandArgs::empty_;"]
    definitions += [extract_block(command, marker) for marker in (
        "CommandArgs::CommandArgs(", "void CommandArgs::parse()",
        "const String& CommandArgs::arg(", "String CommandArgs::remaining(",
        "int CommandArgs::argInt(")]
    if gps:
        definitions += [extract_block(gps_source, marker) for marker in (
            "const char* cmd_gpsstart(", "const char* cmd_gpsautostart(")]
        if "static const char* gpsLogStepError(" in gps_source:
            definitions.append(extract_block(gps_source, "static const char* gpsLogStepError("))
        else:
            require(baseline is not None, "missing checked-result helper")
            definitions.append("static void sensorLogAutoStart() { (void)sensorLogStartConfiguredSession(); }")
        definitions.append(extract_block(gps_source, "const char* cmd_gpslog("))
        if baseline is None:
            require("gSensorLogIntervalMs" not in extract_block(gps_source, "const char* cmd_gpslog("),
                    "GPS still writes logger interval directly")
    else:
        definitions += [extract_block(owner, marker) for marker in (
            "static void sensorLogPublishStarted(", "static bool sensorLogMarkStopped(",
            "uint32_t sensorLogCurrentSessionId(", "bool sensorLogStopSession(")]
        definitions += [extract_block(owner, marker) for marker in (
            "static uint32_t sensorLogClampInterval(", "static const char* sensorLogStartError(",
            "static const char* sensorLogStart(")]
        # The restore helper is forward-declared near the session helpers; take
        # its definition (brace-less body) rather than that declaration.
        restore_marker = "static void sensorLogRestoreSettings()"
        definitions.append(extract_block(owner[owner.index(restore_marker + " {"):], restore_marker))
        definitions += [extract_block(owner, marker) for marker in (
            "const char* sensorLogStartConfiguredSession(", "void sensorLogAutoStart(")]
        if "const char* sensorLogStartLiveTrack(" in owner:
            definitions.append(extract_block(owner, "const char* sensorLogStartLiveTrack("))
        definitions.insert(0, f"#define HAVE_LIVE_TRACK_START {int('sensorLogStartLiveTrack(' in owner)}")
        start = extract_block(owner, 'if (subCmd == "start")')
        definitions += ['static const char* commandStart(const String& input) {\n'
                        '  const CommandArgs a(input);\n'
                        '  const String subCmd = "start";\n' + start +
                        '\n  return "Error: unreachable";\n}']
        for name in ("gSensorLogIntervalMs", "gSensorLogMaxSize", "gSensorLogMaxRotations"):
            require(re.search(r"\b" + name + r"\b", header) is None,
                    f"private logger parameters remain public: {name}")
    harness = (HERE / "gpslog_start_harness.cpp").read_text()
    for marker, body in (("// INSERT_TYPES", "\n\n".join(types)),
                         ("// INSERT_PRODUCTION", "\n\n".join(definitions))):
        require(harness.count(marker) == 1, f"bad marker: {marker}")
        harness = harness.replace(marker, body)
    return harness


def verify_start_body(baseline: Path) -> None:
    old = extract_block(baseline.read_text(), 'if (subCmd == "start")')
    new = extract_block((COMPONENT / "System_SensorLogging.cpp").read_text(),
                        "static const char* sensorLogStart(")
    marker = "if (filepath.length() == 0"
    normalize = lambda source: "\n".join(line.strip() for line in source[source.index(marker):].splitlines())
    require(normalize(old) == normalize(new),
            "typed start's filesystem/admission/publication body differs from baseline")
    print("  typed start filesystem/admission/publication body matches saved baseline")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path,
                        help="replay GPS assertions against the saved old component directory")
    parser.add_argument("--start-baseline", type=Path,
                        help="saved pre-extraction System_SensorLogging.cpp for body comparison")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    if args.start_baseline:
        verify_start_body(args.start_baseline)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
             "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    results = []
    with tempfile.TemporaryDirectory(prefix="hw1-gpslog-start-") as directory:
        arduino = (HERE / "web_batch_stubs" / "Arduino.h").read_text()
        arduino = arduino.replace("#include <algorithm>", "#include <algorithm>\n#include <cstdlib>")
        arduino = arduino.replace("  char operator[]", """  explicit String(uint32_t value) : text_(std::to_string(value)) {}
  long toInt() const { return std::strtol(text_.c_str(), nullptr, 10); }
  int lastIndexOf(char value) const {
    auto pos = text_.rfind(value);
    return pos == std::string::npos ? -1 : static_cast<int>(pos);
  }
  friend String operator+(const char* left, const String& right) { return String(left) + right; }
  char operator[]""")
        (Path(directory) / "Arduino.h").write_text(arduino)
        for gps in (True, False):
            source = Path(directory) / f"test-{int(gps)}.cpp"
            executable = source.with_suffix("")
            source.write_text(generated(gps, args.baseline))
            subprocess.run([args.cxx, *flags, f"-DTEST_GPS={int(gps)}", "-I", directory,
                            str(source), "-o", str(executable)], check=True, timeout=120)
            results.append(subprocess.run([str(executable)], timeout=60).returncode)
    raise SystemExit(1 if any(results) else 0)


if __name__ == "__main__":
    main()
