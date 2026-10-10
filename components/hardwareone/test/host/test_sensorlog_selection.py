#!/usr/bin/env python3
"""Execute the logger's shipping selection/parser/catalog with host boundaries.

The real CommandArgs parser and settings mutator are compiled alongside the
selection helpers. String storage, persistence, notifications and debug-buffer
allocation are controlled boundaries. --baseline replays the same behavioral
assertions against the old sensors command branch in a saved component dir;
the current immutable catalog/parser/mutator remain the shared fixture inputs.
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


def generated(baseline: Path | None) -> str:
    header = (COMPONENT / "System_SensorLogging.h").read_text()
    owner = ((baseline or COMPONENT) / "System_SensorLogging.cpp").read_text()
    command = (COMPONENT / "System_Command.cpp").read_text()
    settings = (COMPONENT / "System_Settings.h").read_text()
    types = ["\n".join(re.findall(r"(?m)^#define LOG_.*$", header)),
             extract_block(header, "struct SensorLogSensor") + ";",
             extract_block(header, "inline constexpr SensorLogSensor kSensorLogSensors[]") + ";",
             extract_block(header, "enum SensorLogFormat") + ";",
             extract_block((COMPONENT / "System_Command.h").read_text(), "class CommandArgs") + ";"]
    count_start = header.index("inline constexpr size_t kSensorLogSensorCount")
    types.append(header[count_start:header.index(";", count_start) + 1])
    definitions = ["const String CommandArgs::empty_;"] + [
        extract_block(command, marker) for marker in (
            "CommandArgs::CommandArgs(", "void CommandArgs::parse()",
            "const String& CommandArgs::arg(", "String CommandArgs::remaining(")]
    definitions.append(extract_block(settings, "template<typename T>\ninline void setSetting("))
    if "static const char* sensorLogSelectionChangeError(" in owner:
        definitions.append(extract_block(owner, "static const char* sensorLogSelectionChangeError("))
    if "static const char* sensorLogSelectSensors(" in owner:
        definitions += [extract_block(owner, marker) for marker in (
            "uint8_t sensorLogSelectedMask(", "static uint8_t sensorLogSensorBit(",
            "static String sensorLogSensorNames(", "static const char* sensorLogSelectSensors(")]
    else:
        require(baseline is not None, "production selection helper missing")
        # Only adapts the old handler's input boundary; its branch is unchanged.
        definitions += [
            "static uint8_t sensorLogSelectedMask() { return gSensorLogMask; }",
            "static const char* sensorLogSelectSensors(const CommandArgs& a) {\n"
            '  const String subCmd = "sensors";\n' +
            extract_block(owner, 'if (subCmd == "sensors")') +
            '\n  return "Error: unreachable";\n}']
    definitions.append('static const char* sensorLogStatusForTest() {\n'
                       '  const String subCmd = "status";\n' +
                       extract_block(owner, 'if (subCmd == "status")') +
                       '\n  return "Error: unreachable";\n}')
    harness = (HERE / "sensorlog_selection_harness.cpp").read_text()
    for marker, content in (("// INSERT_TYPES", "\n\n".join(types)),
                            ("// INSERT_PRODUCTION", "\n\n".join(definitions))):
        require(harness.count(marker) == 1, f"missing/duplicate marker: {marker}")
        harness = harness.replace(marker, content)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-sensorlog-selection-") as directory:
        source, executable = Path(directory) / "test.cpp", Path(directory) / "test"
        source.write_text(generated(args.baseline))
        subprocess.run([args.cxx, *flags, "-I", str(HERE / "web_batch_stubs"),
                        str(source), "-o", str(executable)], check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=60)


if __name__ == "__main__":
    main()
