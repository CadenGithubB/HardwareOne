#!/usr/bin/env python3
"""`inputenabled 0` blocks `openinput` on every input backend.

The production cmd_openinput (HAL_Input.cpp) is extracted and compiled four
times: the onboard GPIO encoder (which has no I2C start queue and so checks
the switch itself) and the three I2C backends (Seesaw gamepad, ANO encoder,
GT911 touch), which hand the open to cmd_sensorstart_queued() where
requestSensorStart() applies the same switch (test_sensor_lifecycle_requests).

GPIO build: with the switch off the reply is the one-sentence refusal, the
driver start is not called, and that holds whether or not the wheel runs.
With it on, every reply is what it was before.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

HARNESS = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "System_CommandReply.h"

struct String {};
#define RETURN_VALID_IF_VALIDATE_CSTR() do {} while (0)
static struct { bool inputEnabled = true; } gSettings;
static bool gInputRunning = false;

#if ENABLE_GPIO_ENCODER
// INSERT_START_ENUM
static unsigned startCalls;
static GpioEncoderStart startResult = GpioEncoderStart::Started;
static GpioEncoderStart gpioEncoderStart() { ++startCalls; return startResult; }
#else
enum I2cDeviceType : int { I2C_DEVICE_INPUT = 3 };
static unsigned queuedOpens;
static std::string openLabel, openTag, openHint;
static const char* cmd_sensorstart_queued(I2cDeviceType sensor, const char* displayName,
                                          const char* eventTag, const char* hint) {
  assert(sensor == I2C_DEVICE_INPUT);
  ++queuedOpens; openLabel = displayName; openTag = eventTag; openHint = hint ? hint : "";
  return "from the shared start";
}
#endif

// INSERT_PRODUCTION

int main() {
#if ENABLE_GPIO_ENCODER
  const char* const disabled = "Error: GPIO encoder is disabled - run 'inputenabled 1' first";
  // Switch off: refused before anything else, running or not; the driver is not asked.
  gSettings.inputEnabled = false;
  for (bool running : {false, true}) {
    gInputRunning = running; startCalls = 0;
    assert(std::strcmp(cmd_openinput(String{}), disabled) == 0 && startCalls == 0);
  }
  // Switch on: the replies are unchanged.
  gSettings.inputEnabled = true; gInputRunning = true; startCalls = 0;
  assert(std::strstr(cmd_openinput(String{}), "already running") && startCalls == 0);
  gInputRunning = false;
  assert(std::strcmp(cmd_openinput(String{}), "[Input] GPIO encoder started") == 0 && startCalls == 1);
  startResult = GpioEncoderStart::PinsInvalid;
  assert(std::strstr(cmd_openinput(String{}), "pins are not valid"));
  startResult = GpioEncoderStart::CacheMissing;
  assert(std::strstr(cmd_openinput(String{}), "Input cache is not available"));
  startResult = GpioEncoderStart::OutOfMemory;
  assert(cmd_openinput(String{}) == kCommandReplyOutOfMemory);
  startResult = GpioEncoderStart::Busy;
  assert(std::strstr(cmd_openinput(String{}), "busy"));
  startResult = GpioEncoderStart::TaskNotCreated;
  assert(std::strstr(cmd_openinput(String{}), "task could not be created"));
  std::puts("openinput enable gate passed (GPIO encoder)");
#else
  // I2C backends: the command itself decides nothing; the shared start (which
  // holds the switch) is always asked, with this backend's name.
  for (bool enabled : {true, false}) {
    gSettings.inputEnabled = enabled; queuedOpens = 0;
    assert(std::strcmp(cmd_openinput(String{}), "from the shared start") == 0 && queuedOpens == 1);
    assert(openLabel == BACKEND_LABEL && openTag == "openinput@enqueue" && !openHint.empty());
  }
  (void)gInputRunning;
  std::printf("openinput enable gate passed (%s via the shared start)\n", BACKEND_LABEL);
#endif
}
'''


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    source = (COMPONENT / "HAL_Input.cpp").read_text()
    production = (extract_block(source, "static const char* inputDeviceLabel()") + "\n#endif\n\n"
                  + extract_block(source, "static const char* cmd_openinput("))
    production = "#if !ENABLE_GPIO_ENCODER\n" + production
    start_enum = extract_block((COMPONENT / "Input_GPIOEncoder.h").read_text(),
                               "enum class GpioEncoderStart") + ";"
    # The GPIO branch holds the switch itself, ahead of the running test.
    gpio_branch = production[production.index("#if ENABLE_GPIO_ENCODER\n", production.index("cmd_openinput(")):]
    gpio_branch = gpio_branch[:gpio_branch.index("#else")]
    require(gpio_branch.find("!gSettings.inputEnabled") != -1, "the GPIO openinput lost its inputenabled check")
    require(gpio_branch.find("!gSettings.inputEnabled") < gpio_branch.find("if (gInputRunning)"),
            "the GPIO openinput must check inputenabled before the already-running reply")
    # Boot, the menu auto-start and first-time setup start the wheel through
    # inputStartInternal(); the switch must not move into the driver start.
    encoder = (COMPONENT / "Input_GPIOEncoder.cpp")
    if encoder.exists():
        text = encoder.read_text()
        for marker in ("GpioEncoderStart gpioEncoderStart()", "bool inputStartInternal()"):
            if marker in text:
                require("inputEnabled" not in extract_block(text, marker),
                        f"{marker} must not read inputEnabled (setup starts the wheel regardless)")
    unit = HARNESS.replace("// INSERT_START_ENUM", start_enum).replace("// INSERT_PRODUCTION", production)
    builds = (
        ("gpio", ["-DENABLE_GPIO_ENCODER=1", "-DENABLE_GT911_TOUCH=0", "-DENABLE_ANO_ENCODER=0"]),
        ("gamepad", ["-DENABLE_GPIO_ENCODER=0", "-DENABLE_GT911_TOUCH=0", "-DENABLE_ANO_ENCODER=0",
                     '-DBACKEND_LABEL="Gamepad"']),
        ("ano", ["-DENABLE_GPIO_ENCODER=0", "-DENABLE_GT911_TOUCH=0", "-DENABLE_ANO_ENCODER=1",
                 '-DBACKEND_LABEL="ANO Encoder"']),
        ("touch", ["-DENABLE_GPIO_ENCODER=0", "-DENABLE_GT911_TOUCH=1", "-DENABLE_ANO_ENCODER=0",
                   '-DBACKEND_LABEL="GT911 Touch"']),
    )
    with tempfile.TemporaryDirectory(prefix="hw1-openinput-gate-") as directory:
        unit_path = Path(directory) / "test.cpp"
        unit_path.write_text(unit)
        for name, flags in builds:
            binary = Path(directory) / f"test-{name}"
            command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                       "-Wno-unused-parameter", *flags, "-I", str(COMPONENT), str(unit_path), "-o", str(binary)]
            if args.sanitize:
                command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
            subprocess.run(command, check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=60)


if __name__ == "__main__":
    main()
