#!/usr/bin/env python3
"""Exercise production sensor task creation/exit across RTOS TCB retirement.

Task creation and the shared exit macro are extracted from firmware on each
run. The host models only the RTOS boundary: creation can fail, self-deletion
does not return, and idle later frees the TCB. This is a lifetime regression,
not a simulation of scheduling, sensor cleanup, or physical I2C behavior.
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
    header = (COMPONENT / "System_TaskUtils.h").read_text()
    source = (COMPONENT / "System_TaskUtils.cpp").read_text()
    macro = re.search(r"^#define SENSOR_TASK_EXIT\([^\n]+\n(?:[^\n]*\\\n)*[^\n]*", header, re.M)
    require(macro is not None, "missing production sensor task exit macro")
    # Once restart is published, only self-deletion may follow. Logging and
    # sensor/cache cleanup can yield, so they must finish while the slot is live.
    expanded = macro.group().replace("\\\n", " ")
    require(re.search(r"\(handle\)\s*=\s*nullptr;\s*vTaskDelete\(nullptr\);\s*}\s*while\s*\(0\)\s*$",
                      expanded) is not None,
            "sensor exit must publish null immediately before self-deletion")

    families = ("Input", "Thermal", "IMU", "ToF", "FMRadio", "APDS", "Presence", "GPS", "RTC")
    definitions = [extract_block(source, f"bool create{family}Task()") for family in families]
    require(all("eTaskGetState" not in definition for definition in definitions),
            "sensor creation must not inspect potentially retired task handles")

    for file, subsystem, handle in (
        ("seesaw", "INPUT", "gInputTaskHandle"),
        ("ano_encoder", "INPUT", "gInputTaskHandle"),
        ("mlx90640", "THERMAL", "gThermalTaskHandle"),
        ("bno055", "IMU", "gImuTaskHandle"),
        ("vl53l4cx", "TOF", "gTofTaskHandle"),
        ("rda5807", "FMRADIO", "gFmRadioTaskHandle"),
        ("apds9960", "APDS", "gApdsTaskHandle"),
        ("sths34pf80", "PRESENCE", "gPresenceTaskHandle"),
        ("pa1010d", "GPS", "gGpsTaskHandle"),
    ):
        driver = (COMPONENT / f"i2csensor_{file}.cpp").read_text()
        code = re.sub(r"//[^\n]*|/\*.*?\*/", "", driver, flags=re.S)
        calls = re.findall(r"SENSOR_TASK_EXIT\s*\(([^)]+)\)\s*;", code)
        require(calls == [f"{subsystem}, {handle}"],
                f"{file} must release its own task slot through the shared exit")
        require(re.search(rf"SENSOR_TASK_EXIT\s*\({subsystem},\s*{handle}\);\s*}}", code) is not None,
                f"{file} must complete shutdown cleanup before publishing restart")

    rtc = (COMPONENT / "i2csensor_ds3231.cpp").read_text()
    rtc_task = extract_block(rtc, "void rtcTask(void* pvParameters)")
    require(re.search(r"while\s*\(!rtcInvalidateCache\(\)\)\s*vTaskDelay\(pdMS_TO_TICKS\(10\)\);\s*"
                      r"gRtcTaskHandle\s*=\s*nullptr;\s*vTaskDelete\(nullptr\);\s*}$", rtc_task) is not None,
            "RTC must invalidate before publishing null and immediately self-delete")
    require("eTaskGetState" not in extract_block(rtc, "bool rtcStartInternal()"),
            "RTC start must not inspect a retired task handle")
    rtc_exit = rtc_task[rtc_task.rindex("gRtcTaskHandle = nullptr;"):].rstrip()[:-1]

    constants = []
    for family in ("INPUT", "THERMAL", "IMU", "TOF", "FMRADIO", "APDS", "PRESENCE", "GPS", "RTC"):
        declaration = re.search(rf"^constexpr uint32_t {family}_STACK_WORDS\s*=\s*[^;]+;", header, re.M)
        require(declaration is not None, f"missing production {family} stack constant")
        constants.append(declaration.group())

    harness = (HERE / "sensor_task_restart_harness.cpp").read_text()
    for marker, implementation in {
        "// INSERT_PRODUCTION_CONSTANTS_HERE": "\n".join(constants),
        "// INSERT_PRODUCTION_EXIT_HERE": macro.group(),
        "// INSERT_PRODUCTION_RTC_EXIT_HERE": rtc_exit,
        "// INSERT_PRODUCTION_CREATE_HERE": "\n\n".join(definitions),
    }.items():
        require(harness.count(marker) == 1, f"expected one marker: {marker}")
        harness = harness.replace(marker, implementation)
    with tempfile.TemporaryDirectory(prefix="hw1-sensor-restart-host-") as temp:
        generated = Path(temp) / "sensor_task_restart.cpp"
        executable = Path(temp) / "sensor_task_restart"
        generated.write_text(harness)
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   str(generated), "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-g"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    main()
