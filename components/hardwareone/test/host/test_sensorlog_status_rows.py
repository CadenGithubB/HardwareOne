#!/usr/bin/env python3
"""Execute the sensor logger's status forms and row builders on the host.

Covers what the Logging page and the log files depend on:

- `sensorlog status` and `sensorlog status json` print from one SensorLogStatus
  reading: the text keeps its wording, the json is one whole object (or the
  error object when it does not fit), and the two forms agree for every
  selection, format and running state.
- A CSV row always carries the columns its header lists: a selected sensor
  without a reading writes empty fields, so later columns keep their names.
- The log snapshot keeps the GPS satellite count and fix quality without a fix
  (the no-fix text row and the CSV row print them); position, speed and time
  stay fix-only.

The status struct, its reader and formatters, the `status` command branch, the
CSV header and row builders, the text row builder and the GPS snapshot branch
are extracted unchanged from System_SensorLogging.cpp / .h on every run;
CommandArgs is the shipping parser. Globals, the debug buffer, the clock and
the GPS cache read are host doubles.

Source contracts (no compiler needed) pin the upload admission arithmetic in
WebServer_Server.cpp to 64 bits: size_t is 32 bits on the device, and the host
cannot reproduce that wrap by running the handler.
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


def upload_contracts(component: Path) -> None:
    web = (component / "WebServer_Server.cpp").read_text()
    handler = extract_block(web, "esp_err_t handleFileUpload(")
    require("(size_t)freeBytes64" not in handler, "upload admission narrows the free space to size_t")
    require(not re.search(r"\bsize_t\s+(?:freeBytes|maxFileSize|estimatedFileSize|freeLimit)\b", handler),
            "upload admission keeps a size in size_t (32 bits on the device)")
    for line in ("const uint64_t estimatedFileSize = rawBody ? (uint64_t)contentLen : ((uint64_t)contentLen * 3) / 4;",
                 "const uint64_t maxFileSize = (freeBytes64 * 9) / 10;",
                 "const uint64_t freeLimit = freeBytes64;",
                 "if (estimatedFileSize > maxFileSize) {"):
        require(line in handler, f"upload admission lost: {line}")
    stats = handler.index("VFS::getStats(uploadStorage, totalBytes64, usedBytes64, freeBytes64);")
    lock = handler.rindex('FsLockGuard fsGuard("handleFileUpload.stats");', 0, stats)
    require(stats - lock < 80, "upload admission reads fresh stats under the filesystem lock")
    for text in ('File too large (~%.1fMB). Free: %.1fMB', '"Insufficient storage space"'):
        require(text in handler, f"upload reply text changed: {text}")
    # A 64-bit value passed to %d shifts every argument after it.
    for call in re.findall(r"DEBUG_STORAGEF\(([^;]*?(?:freeBytes64|freeLimit|maxFileSize|estimatedFileSize)[^;]*)\);", handler):
        require("%d" not in call.split('",')[0], f"a debug line prints a 64-bit size with %d: {call[:80]}")


def generated(component: Path) -> str:
    header = (component / "System_SensorLogging.h").read_text()
    owner = (component / "System_SensorLogging.cpp").read_text()
    command = (COMPONENT / "System_Command.cpp").read_text()
    types = ["\n".join(re.findall(r"(?m)^#define LOG_.*$", header)),
             (COMPONENT / "System_JsonText.h").read_text().replace("#pragma once\n", ""),
             extract_block(header, "struct SensorLogSensor") + ";",
             extract_block(header, "inline constexpr SensorLogSensor kSensorLogSensors[]") + ";",
             extract_block(header, "enum SensorLogFormat") + ";",
             extract_block(header, "struct SensorCacheSnapshot") + ";",
             extract_block(header, "struct SensorLogStatus") + ";",
             extract_block((component / "i2csensor_pa1010d.h").read_text(), "struct GpsCache") + ";",
             extract_block((COMPONENT / "System_Command.h").read_text(), "class CommandArgs") + ";"]
    definitions = ["const String CommandArgs::empty_;"] + [
        extract_block(command, marker) for marker in (
            "CommandArgs::CommandArgs(", "void CommandArgs::parse()",
            "const String& CommandArgs::arg(", "String CommandArgs::remaining(")]
    definitions += [extract_block(owner, marker) for marker in (
        "uint8_t sensorLogSelectedMask(",
        "static String buildCsvHeader(uint8_t mask)",
        "static String sensorLogSensorNames(",
        "static void sensorLogReadStatus(SensorLogStatus& st)",
        "static void sensorLogFormatStatusText(const SensorLogStatus& st, char* buf, size_t cap)",
        "static bool sensorLogFormatStatusJson(const SensorLogStatus& st, char* buf, size_t cap)")]
    status = extract_block(owner, 'if (subCmd == "status")')
    require("sensorLogReadStatus(st);" in status and "sensorLogFormatStatusJson(st, buf, 1024)" in status and
            "sensorLogFormatStatusText(st, buf, 1024)" in status,
            "the status branch reads once and prints either form from that reading")
    definitions.append("static const char* sensorLogStatusCommand(const String& argsInput) {\n"
                       "  CommandArgs a(argsInput);\n  String subCmd = a.arg(0);\n  subCmd.toLowerCase();\n  " +
                       status + '\n  return "Error: unreachable";\n}')
    for name, marker in (("csvRow", "auto buildCsvFromSnap = [](const SensorCacheSnapshot& s) -> const char*"),
                         ("textRow", "auto buildFromSnap = [](const SensorCacheSnapshot& s) -> const char*")):
        builder = extract_block(owner, marker)
        local = marker.split()[1]
        definitions.append(f"static const char* {name}(const SensorCacheSnapshot& snapshot) {{\n  " + builder +
                           f";\n  return {local}(snapshot);\n}}")
    # The row builder must not open with the text the snapshot extractors look
    # for (test_gps_snapshot, test_sensor_reading_consumers take the first one).
    csv = extract_block(owner, "auto buildCsvFromSnap = [](const SensorCacheSnapshot& s) -> const char*")
    require("if (mask & LOG_" not in csv, "the CSV row builder reads gSensorLogMask, not a local mask")
    definitions.append("static SensorCacheSnapshot loggingGps(unsigned mask) { SensorCacheSnapshot snap{};\n" +
                       extract_block(owner, "\n    if (mask & LOG_GPS)") + "\nreturn snap; }")
    harness = (HERE / "sensorlog_status_rows_harness.cpp").read_text()
    for marker, content in (("// INSERT_TYPES", "\n\n".join(types)),
                            ("// INSERT_PRODUCTION", "\n\n".join(definitions))):
        require(harness.count(marker) == 1, f"missing/duplicate marker: {marker}")
        harness = harness.replace(marker, content)
    return harness


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT,
                        help="components/hardwareone directory to read the production sources from")
    parser.add_argument("--contract-only", action="store_true", help="run only the source contracts")
    args = parser.parse_args()
    upload_contracts(args.component)
    print("sensorlog status rows: upload admission source contracts passed", flush=True)
    if args.contract_only:
        return
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-sensorlog-status-rows-") as directory:
        source, executable = Path(directory) / "test.cpp", Path(directory) / "test"
        source.write_text(generated(args.component))
        subprocess.run([args.cxx, *flags, "-I", str(HERE / "web_batch_stubs"),
                        str(source), "-o", str(executable)], check=True, timeout=120)
        subprocess.run([str(executable)], check=True, timeout=60)


if __name__ == "__main__":
    main()
