#!/usr/bin/env python3
"""Every sensor auto-disable site reports to the automatic restart.

A sensor task that switches its sensor off after repeated bus errors or
invalid reads posts SYSEVT_SENSOR_FAULT. Each such site must also call
noteSensorAutoDisabled(<its device type>) (System_I2C.h), and must do so
before it clears the running flag or queues its own stop: the sensor queue
worker reads the running flag first and the report second, so it never sees
"stopped" without the report.

The sites are found by their fault event, not by the "@auto_disabled" status
tag: one gamepad site is tagged "@invalid_data_disabled" and the two ANO
encoder sites carry no tag.

Stops that are not bus trouble must not report: the stack bailout
(checkTaskStackSafety), a failed driver init, and a requested close.

Reads source only; nothing is compiled.
"""
from __future__ import annotations

from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

FAULT = "systemEventPost(SYSEVT_SENSOR_FAULT"
NOTE = re.compile(r"\bnoteSensorAutoDisabled\(\s*(\w+)\s*\)")

# file -> (device type, number of auto-disable sites, what the site does to stop)
EXPECTED = {
    "i2csensor_bno055.cpp":       ("I2C_DEVICE_IMU", 1, "gImuRunning = false"),
    "i2csensor_mlx90640.cpp":     ("I2C_DEVICE_THERMAL", 1, "gThermalRunning = false"),
    "i2csensor_vl53l4cx.cpp":     ("I2C_DEVICE_TOF", 1, "gTofRunning = false"),
    "i2csensor_apds9960.cpp":     ("I2C_DEVICE_APDS", 1, "gApdsColorRunning = false"),
    "i2csensor_pa1010d.cpp":      ("I2C_DEVICE_GPS", 1, "gGpsRunning = false"),
    "i2csensor_sths34pf80.cpp":   ("I2C_DEVICE_PRESENCE", 1, "gPresenceRunning = false"),
    "i2csensor_seesaw.cpp":       ("I2C_DEVICE_INPUT", 2, "requestSensorStop(I2C_DEVICE_INPUT)"),
    "i2csensor_ano_encoder.cpp":  ("I2C_DEVICE_INPUT", 2, "requestSensorStop(I2C_DEVICE_INPUT)"),
}
# The site is one small block; the report and the stop sit a few lines above
# the event.
WINDOW_LINES = 14


def strip_comments(text: str) -> str:
    """Blank // comments (keeping line numbers) so prose cannot satisfy a check."""
    return "\n".join(re.sub(r"//.*", "", line) for line in text.split("\n"))


def main() -> int:
    failures: list[str] = []
    drivers = sorted(COMPONENT.glob("i2csensor_*.cpp"))
    if not drivers:
        print("no i2csensor_*.cpp found")
        return 1

    sites = 0
    for path in drivers:
        code = strip_comments(path.read_text())
        lines = code.split("\n")
        faults = [n for n, line in enumerate(lines) if FAULT in line]
        notes = [(n, NOTE.search(line).group(1)) for n, line in enumerate(lines) if NOTE.search(line)]
        expected = EXPECTED.get(path.name)
        if expected is None:
            if faults or notes:
                failures.append(f"{path.name}: has {len(faults)} fault post(s) and {len(notes)} restart "
                                "report(s) but is not listed in EXPECTED; add its row")
            continue
        device, count, stop = expected
        if len(faults) != count:
            failures.append(f"{path.name}: expected {count} auto-disable site(s), found {len(faults)}")
        if len(notes) != len(faults):
            failures.append(f"{path.name}: {len(faults)} fault post(s) but {len(notes)} restart report(s); "
                            "every auto-disable site reports once, and nothing else does")
        previous_fault = -1
        for fault_line in faults:
            sites += 1
            where = f"{path.name}:{fault_line + 1}"
            # This site's own lines: above its event, below the previous site's.
            window = range(max(previous_fault + 1, fault_line - WINDOW_LINES), fault_line)
            previous_fault = fault_line
            near = [(n, d) for n, d in notes if n in window]
            if len(near) != 1:
                failures.append(f"{where}: the auto-disable site needs exactly one "
                                f"noteSensorAutoDisabled() in the {WINDOW_LINES} lines above its fault event")
                continue
            note_line, note_device = near[0]
            if note_device != device:
                failures.append(f"{where}: reports {note_device}, expected {device}")
            stops = [n for n in window if stop in lines[n]]
            if not stops:
                failures.append(f"{where}: expected the site to stop with '{stop}'")
            elif min(stops) < note_line:
                failures.append(f"{where}: noteSensorAutoDisabled() must come before '{stop}' "
                                f"(line {min(stops) + 1})")
    for name in EXPECTED:
        if not (COMPONENT / name).exists():
            failures.append(f"{name}: listed in EXPECTED but missing")
    if sites != sum(count for _, count, _ in EXPECTED.values()):
        failures.append(f"expected {sum(c for _, c, _ in EXPECTED.values())} auto-disable sites in all, found {sites}")

    # No other caller: the stack bailout, init failures, stops and every other
    # file leave the report alone.
    for path in sorted(COMPONENT.glob("*.cpp")) + sorted(COMPONENT.glob("*.h")):
        if path.name in EXPECTED:
            continue
        code = strip_comments(path.read_text())
        calls = [m for m in NOTE.finditer(code)]
        if path.name == "System_I2C.h":
            bailout = code[code.index("inline bool checkTaskStackSafety("):]
            bailout = bailout[:bailout.index("\n}\n")]
            if "noteSensorAutoDisabled" in bailout:
                failures.append("System_I2C.h: the stack bailout must not ask for a restart")
            allowed = 1   # the declaration: noteSensorAutoDisabled(I2cDeviceType sensor)
            calls = [m for m in re.finditer(r"\bnoteSensorAutoDisabled\(", code)]
        elif path.name == "System_I2C.cpp":
            allowed = 1   # the definition
            calls = [m for m in re.finditer(r"\bnoteSensorAutoDisabled\(", code)]
        else:
            allowed = 0
            calls = [m for m in re.finditer(r"\bnoteSensorAutoDisabled\(", code)]
        if len(calls) != allowed:
            failures.append(f"{path.name}: {len(calls)} use(s) of noteSensorAutoDisabled, expected {allowed}")

    # The worker side: requestSensorStop() itself must not cancel a restart
    # (the input drivers call it for their own auto-disable); the close command does.
    controller = strip_comments((COMPONENT / "System_I2C.cpp").read_text())
    stop_fn = controller[controller.index("bool requestSensorStop(I2cDeviceType sensor) {"):]
    stop_fn = stop_fn[:stop_fn.index("\n}\n")]
    if "Restart" in stop_fn:
        failures.append("System_I2C.cpp: requestSensorStop() must not touch the automatic restart")
    close_fn = controller[controller.index("const char* cmd_sensorstop_queued(I2cDeviceType sensor"):]
    close_fn = close_fn[:close_fn.index("\n}\n")]
    cancel_at = close_fn.find("sensorRestartClosedByHand(sensor)")
    idle_at = close_fn.find("sensorStopHasWork(sensor)")
    if cancel_at < 0 or idle_at < 0 or cancel_at > idle_at:
        failures.append("System_I2C.cpp: cmd_sensorstop_queued() must cancel the automatic restart "
                        "before its not-running test")

    if failures:
        print("Sensor auto-disable sites: FAILED")
        for failure in failures:
            print("  " + failure)
        return 1
    print(f"Sensor auto-disable sites passed ({sites} sites in {len(EXPECTED)} drivers report before they stop; "
          "no other caller)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
