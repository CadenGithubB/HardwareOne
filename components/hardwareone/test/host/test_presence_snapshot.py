#!/usr/bin/env python3
"""Run production presence polling, snapshots and presentation with host boundaries.

The cache guard, poll/detection logic and consumers are extracted unchanged.
Register I/O, RTOS admission, HTTP and display printing are substitutes. Optional
--baseline executes pre-change production code against the same input cases.
"""
from __future__ import annotations

import argparse
import json
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
    owner = (component / "i2csensor_sths34pf80.cpp").read_text()
    header = (component / "i2csensor_sths34pf80.h").read_text()
    mutex = (COMPONENT / "System_Mutex.cpp").read_text()
    types = "\n".join(extract_block(source.read_text(), marker) + ";" for source, marker in [
        (component / "i2csensor_sths34pf80.h", "struct PresenceCache"),
        (COMPONENT / "System_SensorLogging.h", "struct SensorCacheSnapshot"),
        (COMPONENT / "System_Mutex.h", "struct SensorCacheGuard"),
    ])
    state = "\n".join(re.search(r"(?m)^static[^\n]*\b" + name + r"\b[^\n]*;", owner)[0]
                      for name in ["sPresenceEvtPresent", "sPresenceEvtClearPolls", "gMotionActive"])
    state += "\n" + "\n".join(re.findall(r"(?m)^#define STHS34PF80_.*$", owner))
    if baseline:
        state += "\nPresenceCache gPresenceCache{};\n#define DATA gPresenceCache\n#define MUTEX gPresenceCache.mutex\n"
    else:
        state += "\n" + "\n".join(re.search(r"(?m)^static[^\n]*\b" + name + r"\b[^\n]*;", owner)[0]
                                    for name in ["sPresenceCache", "sPresenceCacheMutex"])
        state += "\n#define DATA sPresenceCache\n#define MUTEX sPresenceCacheMutex\n"
        state += re.search(r"(?m)^bool presenceCacheSnapshot\([^\n]*;", header)[0] + "\n"
    functions = [extract_block(mutex, "SensorCacheGuard::SensorCacheGuard("),
                 extract_block(mutex, "SensorCacheGuard::~SensorCacheGuard("),
                 extract_block((COMPONENT / "System_I2C.cpp").read_text(), "int sensorEnvelopeBegin(")]
    if not baseline:
        functions.append(extract_block(owner, "bool presenceCacheSnapshot("))
    functions += [extract_block(owner, marker) for marker in [
        "int presenceBuildDataJSON(", "const char* cmd_presenceread(",
        "const char* cmd_presencestatus(", "bool presencePoll()",
    ]]
    g2 = (component / "G2_Page_Sensors.cpp").read_text()
    functions += [extract_block(g2, marker) for marker in [
        "static void presenceG2FormatValue(", "static void g2BuildPresenceReadout(",
    ]]
    functions.append(extract_block((component / "i2csensor_sths34pf80_oled.h").read_text(),
                                   "void displayPresenceData()"))
    mqtt = (component / "System_MQTT.cpp").read_text()
    mqtt_pos = mqtt.index("if (gSettings.mqttPublishPresence && gPresenceRunning")
    mqtt_block = mqtt[mqtt.rindex("#if ENABLE_PRESENCE_SENSOR", 0, mqtt_pos):
                      mqtt.index("#endif", mqtt_pos) + len("#endif")]
    web_block = extract_block((component / "WebPage_Sensors.cpp").read_text(),
                              'if (sensorType == "presence")')
    log_block = extract_block((component / "System_SensorLogging.cpp").read_text(),
                              "\n    if (mask & LOG_PRESENCE)")
    functions += [
        "static int mqttPresence(char* jsonBuf) { int pos = 0;\n" + mqtt_block + "\nreturn pos; }",
        "static int webPresence() { void* req = nullptr; const std::string sensorType = \"presence\";\n" +
        web_block + "\nreturn -1; }",
        "static SensorCacheSnapshot loggingPresence(unsigned mask) { SensorCacheSnapshot snap{};\n" +
        log_block + "\nreturn snap; }",
    ]
    if not baseline:
        functions.append(extract_block(owner, "static bool presenceInvalidateCache("))
        task = extract_block(owner, "void presenceTask(void* parameter)")
        functions.append("static void runStopBranch() { while (true) {\n" +
                         extract_block(task, "if (!gPresenceRunning)") + "\nbreak; } }")
        start = extract_block(owner, "bool presenceStartInternal()")
        prefix = start[:start.index("  // Initialize sensor synchronously")]
        functions.append(prefix.replace("presenceStartInternal", "runStartReset", 1) + "return true;\n}")
        require("SemaphoreHandle_t" not in extract_block(header, "struct PresenceCache"),
                "Presence snapshot must not expose its semaphore")
        require(not re.search(r"\bextern\s+.*gPresenceCache", header), "Presence cache must stay private")
        for file in ["System_MQTT.cpp", "WebPage_Sensors.cpp", "G2_Page_Sensors.cpp",
                     "System_SensorLogging.cpp", "i2csensor_sths34pf80_oled.h",
                     "i2csensor_sths34pf80_web.h", "System_SensorStubs.cpp", "System_SensorStubs.h"]:
            require("gPresenceCache" not in (component / file).read_text(),
                    f"{file} exposes or bypasses the presence cache")
    harness = (HERE / "presence_snapshot_harness.cpp").read_text()
    return (harness.replace("// INSERT_TYPES", types).replace("// INSERT_STATE", state)
            .replace("// INSERT_FUNCTIONS", "\n\n".join(functions)))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path, help="pre-change components/hardwareone directory")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-presence-snapshot-") as directory:
        work = Path(directory)
        def run(name: str, source: str, extra: list[str] | None = None) -> str:
            path = work / (name + ".cpp")
            path.write_text(source)
            subprocess.run([args.cxx, *flags, *(extra or []), str(path), "-o", str(work / name)],
                           check=True, timeout=60)
            return subprocess.check_output([str(work / name)], text=True, timeout=60)
        output = run("presence", generated(COMPONENT, False), ["-DTEST_BASELINE=0"])
        expected = "".join(json.loads((HERE / "fixtures/presence_snapshot_expected.json").read_text()))
        require(output == expected,
                "Presence characterized wire/display output changed")
        if args.baseline:
            old = run("baseline", generated(args.baseline, True), ["-DTEST_BASELINE=1"])
            require(output == old, "Presence production output differs from baseline")
            print("Presence baseline: production JSON, CLI, G2, web, MQTT, OLED and polling match")
        shutil.copyfile(COMPONENT / "i2csensor_sths34pf80.h", work / "i2csensor_sths34pf80.h")
        (work / "Arduino.h").write_text("#include <cstddef>\n#include <cstdint>\nclass String;\n")
        (work / "System_BuildConfig.h").write_text("#define ENABLE_PRESENCE_SENSOR 0\n")
        (work / "freertos").mkdir()
        (work / "freertos/FreeRTOS.h").write_text("#pragma once\nusing TaskHandle_t = void*;\n")
        (work / "freertos/task.h").write_text('#include "FreeRTOS.h"\n')
        run("disabled", '''#include <cassert>
#include "i2csensor_sths34pf80.h"
int main() {
  PresenceCache out{};
  out.presenceDetected = true; out.ambientTemp = 12;
  assert(!presenceCacheSnapshot(out));
  assert(!out.presenceDetected && out.ambientTemp == 0 && !out.dataValid);
  out.dataValid = true;
  assert(!presenceCacheSnapshot(out, 100) && !out.dataValid);
  char buffer[16] = "unchanged";
  assert(presenceBuildDataJSON(buffer, sizeof(buffer)) == 0 && buffer[0] == '\\0');
  assert(presenceBuildDataJSON(nullptr, 12) == 0);
  buffer[0] = 'Z';
  assert(presenceBuildDataJSON(buffer, 0) == 0 && buffer[0] == 'Z');
}
''', ["-I", str(work)])
    print("Presence snapshot: owner, budgets, polling/debounce, consumers and disabled API passed")


if __name__ == "__main__":
    main()
