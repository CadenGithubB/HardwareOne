#!/usr/bin/env python3
"""Exercise the production CPU owner, mode application and command off-device.

Only the RTOS, clock HAL, settings and presentation boundaries are mocked. The
enabled variant links the actual polling-pause implementation; the disabled
variant has no I2C manager or pause definitions to hide an accidental dependency.
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
# System_Power.h only tells the ESP32-P4 apart from every other target, so the
# S3 compiles the same code as the classic ESP32; a separate S3 profile would
# only repeat the esp32 runs.
PROFILES = (
    ("esp32", []),
    ("p4", ["-DCONFIG_IDF_TARGET_ESP32P4=1"]),
    ("p4-old", ["-DCONFIG_IDF_TARGET_ESP32P4=1", "-DCONFIG_ESP32P4_SELECTS_REV_LESS_V3=1"]),
)


def generate() -> str:
    power = (COMPONENT / "System_Power.cpp").read_text()
    utils = (COMPONENT / "System_Utils.cpp").read_text()
    hardware = (COMPONENT / "HardwareOne.cpp").read_text()
    header = (COMPONENT / "System_Power.h").read_text()
    manager = (COMPONENT / "System_I2C_Manager.h").read_text()
    require("bool powerSetCpuFrequencyMhz(uint32_t mhz, bool* outClockChanged = nullptr);" in header,
            "CPU owner must expose optional actual-change output")
    accessor = extract_block(manager, "static I2CDeviceManager* existingInstance(")
    require(re.sub(r"\s+", " ", accessor).endswith("{ return instance; }"),
            "existing manager lookup must be nonallocating")
    command = extract_block(utils, "const char* cmd_cpufreq(")
    require("RETURN_VALID_IF_VALIDATE_CSTR();" in command, "validation-only command guard missing")
    require(re.search(r'\{\s*"cpufreq"\s*,[^\n]+,\s*true\s*,\s*cmd_cpufreq\s*,', utils),
            "cpufreq must retain its admin authorization")
    for name, body in (("mode", extract_block(power, "bool applyPowerMode(")),
                       ("command", command),
                       ("policy", extract_block(power, "void powerPolicyTick("))):
        require("powerSetCpuFrequencyMhz(" in body, f"{name} must use the shared owner")
        require(not re.search(r"\bsetCpuFrequencyMhz\s*\(", body), f"{name} bypasses owner")
    require("setCpuFrequencyDrained" not in hardware, "old clock helper must be removed")
    # The local panel's tick no longer touches the clock at all.
    require("powerSetCpuFrequencyMhz(" not in extract_block(hardware, "static void powerDisplayTick("),
            "the display tick must leave the clock to powerPolicyTick")
    # The owner region ends where the telemetry JSON (ArduinoJson) begins. An
    # older source still has the inert auto-mode stub there; stop before it.
    start = power.index("static StaticSemaphore_t sCpuFrequencyMutexStorage;")
    stub = power.find("void checkAutoPowerMode()")
    owner = power[start:stub if stub >= 0 else power.index("void buildPowerJson(JsonDocument& doc)")]
    # applySettings re-applies the saved mode at boot and after setup; run its
    # actual applyPowerMode statement against the owner.
    settings = (COMPONENT / "System_Settings.cpp").read_text()
    apply_settings = extract_block(settings, "void applySettings(")
    steps = re.findall(r"^\s*applyPowerMode\([^;]*\);", apply_settings, re.M)
    require(len(steps) == 1, "applySettings must re-apply the saved power mode once")
    step = "static void applySettingsPowerStep() {\n" + steps[0] + "\n}\n"
    keeps = int("bool applyPowerMode(uint8_t mode, bool setDisplayBrightness" in header)
    harness = (HERE / "power_cpu_frequency_harness.cpp").read_text()
    return (f"#define POWER_MODE_HAS_BRIGHTNESS_OPTION {keeps}\n" +
            harness.replace("// INSERT_PRODUCTION", owner + "\n" + command + "\n" + step))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    with tempfile.TemporaryDirectory(prefix="hw1-power-cpu-") as directory:
        work = Path(directory)
        (work / "Arduino.h").write_text("#pragma once\n#include <cstdint>\n#include <cstddef>\n")
        (work / "freertos").mkdir()
        (work / "freertos/FreeRTOS.h").write_text(
            "#pragma once\nusing portMUX_TYPE = int;\n"
            "#define portMUX_INITIALIZER_UNLOCKED 0\n"
            "#define portENTER_CRITICAL(mux) ((void)(mux))\n"
            "#define portEXIT_CRITICAL(mux) ((void)(mux))\n")
        (work / "freertos/task.h").write_text("#pragma once\n")
        source = work / "test.cpp"
        source.write_text(generate())
        for profile, flags in PROFILES:
            for i2c in (0, 1):
                executable = work / f"test-{profile}-i2c-{i2c}"
                command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                           "-Wno-unused-function", "-Wno-unused-parameter", f"-DENABLE_I2C_SYSTEM={i2c}",
                           "-DENABLE_OLED_DISPLAY=1", "-I" + str(work), "-I" + str(COMPONENT),
                           *flags, str(source), "-o", str(executable)]
                if i2c:
                    command.append(str(COMPONENT / "System_PollPause.cpp"))
                if args.sanitize:
                    command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
                subprocess.run(command, check=True, timeout=60)
                subprocess.run([str(executable)], check=True, timeout=60)


if __name__ == "__main__":
    main()
