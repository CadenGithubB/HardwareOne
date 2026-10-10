#!/usr/bin/env python3
"""Exercise the production GPS snapshot, JSON and consumer boundaries.

FreeRTOS admission, driver destruction and HTTP transport are host substitutes.
The owner functions and consumer blocks are extracted unchanged on every run.
An optional pre-change component directory checks byte-for-byte output parity;
normal runs assert the same wire contracts without a saved checkout.
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


def generated(component: Path, baseline: bool) -> str:
    owner = (component / "i2csensor_pa1010d.cpp").read_text()
    header = (component / "i2csensor_pa1010d.h").read_text()
    g2 = (component / "G2_Page_Sensors.cpp").read_text()
    mqtt = (component / "System_MQTT.cpp").read_text()
    web = (component / "WebPage_Sensors.cpp").read_text()
    logging = (component / "System_SensorLogging.cpp").read_text()
    mutex_header = (COMPONENT / "System_Mutex.h").read_text()
    mutex = (COMPONENT / "System_Mutex.cpp").read_text()
    types = extract_block(header, "struct GPSCache") + ";\n"
    types += extract_block((COMPONENT / "System_SensorLogging.h").read_text(),
                           "struct SensorCacheSnapshot") + ";\n"
    types += extract_block(mutex_header, "struct SensorCacheGuard") + ";\n"
    if baseline:
        state = "GPSCache gGpsCache{};\n#define DATA gGpsCache\n#define MUTEX gGpsCache.mutex\n"
    else:
        state = "\n".join(re.search(r"(?m)^static[^\n]*\b" + name + r"\b[^\n]*;", owner)[0]
                          for name in ["sGpsCache", "sGpsCacheMutex", "sGpsEvtHasFix",
                                       "sGpsNoFixSinceMs"])
        state += "\n#define DATA sGpsCache\n#define MUTEX sGpsCacheMutex\n"
        state += re.search(r"(?m)^bool gpsCacheSnapshot\([^\n]*;", header)[0] + "\n"
    functions = [extract_block(mutex, "SensorCacheGuard::SensorCacheGuard("),
                 extract_block(mutex, "SensorCacheGuard::~SensorCacheGuard("),
                 extract_block(owner, "bool gpsCacheSnapshot("),
                 extract_block((COMPONENT / "System_I2C.cpp").read_text(),
                               "int sensorEnvelopeBegin("),
                 extract_block(owner, "int gpsBuildDataJSON("),
                 extract_block(g2, "static void gpsG2FormatValue("),
                 extract_block(g2, "static void g2BuildGpsReadout(")]
    mqtt_pos = mqtt.index("if (gSettings.mqttPublishGPS && gGpsRunning")
    mqtt_branch = mqtt[mqtt.rindex("#if ENABLE_GPS_SENSOR", 0, mqtt_pos):
                       mqtt.index("#endif", mqtt_pos) + len("#endif")]
    web_branch = extract_block(web, 'if (sensorType == "gps")')
    log_branch = extract_block(logging, "\n    if (mask & LOG_GPS)")
    functions += [
        "static int mqttGps(char* jsonBuf) { int pos = 0;\n" + mqtt_branch + "\nreturn pos; }",
        "static int webGps() { void* req = nullptr; const std::string sensorType = \"gps\";\n" +
        web_branch + "\nreturn -1; }",
        "static SensorCacheSnapshot loggingGps(unsigned mask) { SensorCacheSnapshot snap{};\n" +
        log_branch + "\nreturn snap; }",
    ]
    if not baseline:
        functions.append(extract_block(owner, "static bool gpsInvalidateCache("))
        task = extract_block(owner, "void gpsTask(void* parameter)")
        stop = extract_block(task, "if (!gGpsRunning)")
        functions.append("static void runStopBranch() { while (true) {\n" + stop + "\nbreak; } }")
        publish = extract_block(task, "if (parsedNewSentence)")
        functions.append("static void runPublication(bool parsedNewSentence, unsigned long nowMs) {\n" +
                         publish + "\n}")
        # Execute the exact startup prefix, stopping before hardware allocation.
        start = extract_block(owner, "bool gpsStartInternal()")
        prefix = start[:start.index("  // Initialize GPS module")]
        functions.append(prefix.replace("gpsStartInternal", "runStartReset", 1) + "return true;\n}")
        require(not re.search(r"\bextern\s+.*(?:gGpsCache|gPA1010D)", header),
                "GPS cache/driver must not be exported")
        for file in ["System_MQTT.cpp", "WebPage_Sensors.cpp", "G2_Page_Sensors.cpp",
                     "System_SensorLogging.cpp", "i2csensor_pa1010d_oled.h"]:
            source = (component / file).read_text()
            require(not re.search(r"\bgGpsCache\b|\bgPA1010D\b", source),
                    f"{file} bypasses the GPS snapshot boundary")
        oled = extract_block((component / "i2csensor_pa1010d_oled.h").read_text(),
                             "static void displayGPSData()")
        require(re.search(r"gpsCacheSnapshot\(gps(?:,\s*10)?\)", oled) is not None,
                "OLED must retain its 10 ms read budget")
    harness = (HERE / "gps_snapshot_harness.cpp").read_text()
    return (harness.replace("// INSERT_TYPES", types).replace("// INSERT_STATE", state)
            .replace("// INSERT_FUNCTIONS", "\n\n".join(functions)))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path, help="pre-change components/hardwareone directory")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
             "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-gps-snapshot-") as directory:
        work = Path(directory)
        def run(name: str, source: str, extra: list[str] | None = None) -> str:
            path = work / (name + ".cpp")
            path.write_text(source)
            subprocess.run([args.cxx, *flags, *(extra or []), str(path), "-o", str(work / name)],
                           check=True, timeout=60)
            return subprocess.check_output([str(work / name)], text=True, timeout=60)
        output = run("gps", generated(COMPONENT, False), ["-DTEST_BASELINE=0"])
        expected = (HERE / "fixtures/gps_snapshot_expected.txt").read_text()
        require(output == expected, "GPS characterized wire/display output changed")
        if args.baseline:
            old = run("gps-baseline", generated(args.baseline, True), ["-DTEST_BASELINE=1"])
            require(output == old, "GPS ready/no-fix/invalid/off/timeout wire output changed")
            print("GPS baseline: production JSON, G2, web, MQTT and logging outputs match")
        # Compile the complete shipping public header with GPS disabled. Only
        # unrelated Arduino/SDK/config includes are replaced in this temp tree.
        shutil.copyfile(COMPONENT / "i2csensor_pa1010d.h", work / "i2csensor_pa1010d.h")
        (work / "Arduino.h").write_text("#include <cstddef>\n#include <cstdint>\nclass String;\n")
        (work / "System_BuildConfig.h").write_text("#define ENABLE_GPS_SENSOR 0\n")
        (work / "freertos").mkdir()
        (work / "freertos/FreeRTOS.h").write_text("#pragma once\nusing TaskHandle_t = void*;\n")
        (work / "freertos/task.h").write_text('#include "FreeRTOS.h"\n')
        disabled = '''#include <cassert>
#include "i2csensor_pa1010d.h"
int main() {
  GPSCache out{};
  out.hasFix = true; out.latitude = 12.0f;
  assert(!gpsCacheSnapshot(out));
  assert(!out.hasFix && out.latitude == 0 && !out.dataValid);
  out.hasFix = true;
  assert(!gpsCacheSnapshot(out, 50) && !out.hasFix);
  char buffer[16] = "unchanged";
  assert(gpsBuildDataJSON(buffer, sizeof(buffer)) == 0);
  assert(buffer[0] == '\\0');
  assert(gpsBuildDataJSON(nullptr, sizeof(buffer)) == 0);
  buffer[0] = 'Z';
  assert(gpsBuildDataJSON(buffer, 0) == 0 && buffer[0] == 'Z');
}
'''
        run("gps-disabled", disabled, ["-I", str(work)])
    print("GPS snapshot: owner, timeout budgets, wire formats, consumers and disabled API passed")


if __name__ == "__main__":
    main()
