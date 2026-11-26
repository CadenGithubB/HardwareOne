#!/usr/bin/env python3
"""Exercise actual thermal copies, serializers, OLED rendering and buffer retirement.

Host substitutes cover allocation, FreeRTOS and display output; production
functions are extracted unchanged. Optional --baseline compares every emitted
byte against the saved pre-change component, including raw/interpolated frames.
"""
from __future__ import annotations
import argparse
import hashlib
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
    owner = (component / "i2csensor_mlx90640.cpp").read_text()
    header = (component / "i2csensor_mlx90640.h").read_text()
    types = extract_block(header, "struct ThermalCache") + ";\n"
    types += extract_block((COMPONENT / "System_Mutex.h").read_text(), "struct SensorCacheGuard") + ";\n"
    mutex = (COMPONENT / "System_Mutex.cpp").read_text()
    functions = [extract_block(mutex, "SensorCacheGuard::SensorCacheGuard("),
                 extract_block(mutex, "SensorCacheGuard::~SensorCacheGuard(")]
    if baseline:
        state = "ThermalCache gThermalCache;\n#define DATA gThermalCache\n#define MUTEX gThermalCache.mutex\n"
        i2c = (component / "System_I2C.cpp").read_text()
        functions += [extract_block(i2c, "bool lockThermalCache("),
                      extract_block(i2c, "void unlockThermalCache()")]
        require("System_I2C.cpp" in [p.name for p in component.iterdir()], "baseline needs the I2C source")
    else:
        types += "\n".join(re.findall(r"(?m)^constexpr size_t THERMAL_.*;", header)) + "\n"
        types += extract_block(owner, "struct ThermalCacheState") + ";\n"
        state = "\n".join(re.search(r"(?m)^static[^\n]*\b" + name + r"\b[^\n]*;", owner)[0]
                          for name in ["sThermalCache", "sThermalCacheMutex",
                                       "g_tempFrame", "g_localFrame"])
        state += "\n#define DATA sThermalCache\n#define MUTEX sThermalCacheMutex\n"
        state += re.search(r"bool thermalCacheSnapshot\([^;]+;", header)[0] + "\n"
        state += re.search(r"bool thermalFrameSnapshot\([^;]+;", header)[0] + "\n"
        state += re.search(r"(?m)^using ThermalFrameBuffer.*;", owner)[0] + "\n"
        functions += [extract_block(owner, marker) for marker in [
            "static void thermalCopyMetadata(", "bool thermalCacheSnapshot(",
            "bool thermalFrameSnapshot(", "size_t thermalCacheStorageBytes(",
            "static ThermalFrameBuffer thermalSnapshotBuffer(",
            "static bool thermalResetCacheForStart(", "static bool thermalInvalidateCache(",
            "static bool thermalReadRawSummary(",
        ]]
        cache_type = extract_block(header, "struct ThermalCache")
        require("*" not in cache_type and "Semaphore" not in cache_type, "thermal snapshot exposes storage")
    functions += [extract_block((COMPONENT / "System_I2C.cpp").read_text(), "int sensorEnvelopeBegin("),
                  extract_block(owner, "int thermalBuildSummaryJson("),
                  extract_block(owner, "int thermalBuildDataJson("),
                  extract_block((component / "System_ESPNow_Sensors.cpp").read_text(),
                                "int buildThermalDataJsonInteger("),
                  extract_block((component / "i2csensor_mlx90640_oled.h").read_text(),
                                "static void displayThermalVisual()")]
    if not baseline:
        # Frame buffers are allocated at start. The poll holds the bus only for
        # one subpage transfer; a chip that is still converting is not a failure
        # and does not stay inside the lock. The task does not wrap the whole
        # poll (math, cache, interpolation) in one transaction.
        poll = extract_block(owner, "\nbool thermalPoll() ")
        require('"cache.thermal"' not in poll and '"cache.thermal.interp"' not in poll and
                '"thermal.temp"' not in poll and '"thermal.local"' not in poll and
                "lockThermalCache(pdMS_TO_TICKS(100))" not in poll and
                "getFrame(" not in poll,
                "thermalPoll allocates frame buffers or blocks in getFrame")
        ready = poll.index("MLX90640_FRAME_NOT_READY")
        transfer = poll.index("readRawSubpage")
        publish = poll.index("g_localFrame[i] = (int16_t)(g_tempFrame[i] * 100.0f)")
        require(transfer < ready < publish and "return true" in poll[ready:publish],
                "a converting camera must release the bus before frame processing")
        start = extract_block(owner, "static bool thermalResetCacheForStart(")
        require('"thermal.temp"' in start and '"thermal.local"' in start,
                "frame buffers are allocated when the sensor starts")
        task = extract_block(owner, "void thermalTask(void* parameter)")
        require("i2cTaskWithTimeout" not in task and "thermalPoll()" in task,
                "the task must not hold the bus across thermalPoll")
        functions.append("static void runStopBranch() { while (true) {\n" +
                         extract_block(task, "if (!gThermalRunning)") + "\nbreak; } }")
    harness = (HERE / "thermal_snapshot_harness.cpp").read_text().replace('#include "OLED_Layout.h"', (COMPONENT / "OLED_Layout.h").read_text().replace("#pragma once", ""))
    return (harness.replace("// INSERT_TYPES", types).replace("// INSERT_STATE", state)
            .replace("// INSERT_FUNCTIONS", "\n\n".join(functions)))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path)
    args = parser.parse_args()
    require(bool(args.cxx), "a C++17 host compiler is required")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-thermal-snapshot-") as directory:
        work = Path(directory)
        def run(name: str, code: str, extra: list[str]) -> bytes:
            path = work / (name + ".cpp")
            path.write_text(code)
            subprocess.run([args.cxx, *flags, *extra, str(path), "-o", str(work / name)],
                           check=True, timeout=60)
            return subprocess.check_output([str(work / name)], timeout=60)
        output = run("thermal", generated(COMPONENT, False), ["-DTEST_BASELINE=0"])
        expected = json.loads((HERE / "fixtures/thermal_snapshot_expected.json").read_text())
        require(hashlib.sha256(output).hexdigest() == expected["sha256"],
                "thermal raw/interpolated/rotation/summary/integer/OLED output changed")
        if args.baseline:
            old = run("baseline", generated(args.baseline, True), ["-DTEST_BASELINE=1"])
            require(output == old, "thermal output differs from the actual baseline")
            print("Thermal baseline: all frame, summary, integer-stream and OLED bytes match")
        shutil.copyfile(COMPONENT / "i2csensor_mlx90640.h", work / "i2csensor_mlx90640.h")
        (work / "Arduino.h").write_text("#include <cstddef>\n#include <cstdint>\nclass String;\n")
        (work / "System_BuildConfig.h").write_text("#define ENABLE_THERMAL_SENSOR 0\n")
        (work / "freertos").mkdir()
        (work / "freertos/FreeRTOS.h").write_text("#pragma once\nusing TaskHandle_t=void*;\n")
        (work / "freertos/task.h").write_text('#include "FreeRTOS.h"\n')
        run("disabled", '''#include <cassert>
#include "i2csensor_mlx90640.h"
int main() {
  ThermalCache out; out.thermalDataValid = true;
  assert(!thermalCacheSnapshot(out) && !out.thermalDataValid);
  int16_t value = 7;
  assert(!thermalFrameSnapshot(out, &value, 1, true, 50) && value == 7);
  assert(thermalCacheStorageBytes() == 0);
  char buf[10] = "old";
  assert(thermalBuildDataJson(buf, sizeof(buf)) == 0 && !buf[0]);
  assert(thermalBuildSummaryJson(nullptr, 5) == 0);
  buf[0] = 'Z'; assert(thermalBuildSummaryJson(buf, 0) == 0 && buf[0] == 'Z');
}
''', ["-I", str(work)])
    print("Thermal snapshot: copies, capacity/lifetime, serializers, contention and disabled API passed")


if __name__ == "__main__":
    main()
