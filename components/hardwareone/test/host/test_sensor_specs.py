#!/usr/bin/env python3
"""kSensorSpecs: each sensor type's mesh masks, mesh reading and MQTT section.

The table (System_SensorRegistry.cpp) replaced these lists: the ESP-NOW
broadcast table gSensorSpecs of System_ESPNow_Sensors.cpp, the #if ladder
of state sections in publishMqttSensorData (System_MQTT.cpp), the list of
sensorPresentMask (System_ESPNow_Sensors.cpp) and the #if ladders of the bond
status enabled and connected masks and the capability sensorMask
(buildLocalBondStatus, buildCapabilitySummary in System_ESPNow.cpp). This
suite pins the rows to those lists as they were.

The production text between the SENSOR_SPECS_HOST_BEGIN and _END marks and
the row order static_assert after it are compiled with the production
System_SensorRegistry.h and System_ESPNow_SensorCore.h against stand-in
builders, flags and gSettings (sensor_specs_harness.cpp), once per build
profile, and linked with a second unit that prints every row by name.
Checked, in every profile:

  * the row order static_assert compiles, and row i has id i;
  * {built, masks, running, connected} is the old mask lists: the 8 types of
    sensorPresentMask (status, its connected flag) and the 9 with a
    CAP_SENSOR_ bit (capability, each ladder's build flag, running and
    connected flag): input built by ENABLE_OLED_INPUT, apds in the
    capability masks only with gApdsRunning; camera and microphone in no
    mask, built by their own flags;
  * mesh {builder, minIntervalMs, bufBytes} is the old gSensorSpecs row:
    tof 500/1024, imu 500/512, gps 1000/256, fmradio 1000/512, rtc 1000/256,
    presence 500/256, input 100 ms with the build's one input device
    (gamepad 128, ANO 128, GPIO encoder 256, IMU input 128 bytes), each only
    with its flag; thermal, camera, microphone and apds never have one;
  * mqtt {builder, publish setting, running flag, connected flag} is the old
    ladder's section: the builder only with its flag (thermal the summary,
    input the build's input builder), gps, apds, rtc and input also ask
    their connected flag, apds its colour mode; fmradio, camera and
    microphone have no section at all.

Profiles: everything on with each of the four input devices with a reading,
with GT911 touch, with no input device; each sensor and each input device
alone; everything off. The expected rows are written out below.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --component <dir>.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

SENSORS = ("ENABLE_THERMAL_SENSOR", "ENABLE_TOF_SENSOR", "ENABLE_IMU_SENSOR", "ENABLE_GPS_SENSOR",
           "ENABLE_FM_RADIO", "ENABLE_RTC_SENSOR", "ENABLE_PRESENCE_SENSOR", "ENABLE_APDS_SENSOR",
           "ENABLE_CAMERA_SENSOR", "ENABLE_MICROPHONE")
INPUTS = ("ENABLE_GAMEPAD_SENSOR", "ENABLE_ANO_ENCODER", "ENABLE_GPIO_ENCODER", "ENABLE_IMU_INPUT",
          "ENABLE_GT911_TOUCH")
TYPES = ("thermal", "tof", "imu", "gps", "input", "fmradio", "camera", "microphone", "rtc", "presence", "apds")

# The old gSensorSpecs rows (System_ESPNow_Sensors.cpp before kSensorSpecs):
# type: (flag, builder, minIntervalMs, bufBytes), or None for { nullptr, 0, 0 }
# in every build.
OLD_MESH = {
    "thermal": None,
    "tof": ("ENABLE_TOF_SENSOR", "tofBuildDataJson", 500, 1024),
    "imu": ("ENABLE_IMU_SENSOR", "imuBuildDataJson", 500, 512),
    "gps": ("ENABLE_GPS_SENSOR", "gpsBuildDataJson", 1000, 256),
    "fmradio": ("ENABLE_FM_RADIO", "fmRadioBuildDataJson", 1000, 512),
    "camera": None,
    "microphone": None,
    "rtc": ("ENABLE_RTC_SENSOR", "rtcBuildDataJson", 1000, 256),
    "presence": ("ENABLE_PRESENCE_SENSOR", "presenceBuildDataJson", 500, 256),
    "apds": None,
}
# Its input row, an #if/#elif chain: the first flag set wins; none gives
# { nullptr, 0, 0 } (GT911 touch has no reading).
OLD_INPUT_MESH = (("ENABLE_GAMEPAD_SENSOR", "gamepadBuildDataJson", 100, 128),
                  ("ENABLE_ANO_ENCODER", "anoEncoderBuildDataJson", 100, 128),
                  ("ENABLE_GPIO_ENCODER", "gpioEncoderBuildDataJson", 100, 256),
                  ("ENABLE_IMU_INPUT", "imuInputBuildDataJson", 100, 128))
# The old ladder of publishMqttSensorData: type: (flag, builder, publish
# setting, running flag, connected flag or None). The input section used the
# input chain's builder above, behind any of its four flags.
OLD_MQTT = {
    "thermal": ("ENABLE_THERMAL_SENSOR", "thermalBuildSummaryJson", "mqttPublishThermal", "gThermalRunning", None),
    "tof": ("ENABLE_TOF_SENSOR", "tofBuildDataJson", "mqttPublishTof", "gTofRunning", None),
    "imu": ("ENABLE_IMU_SENSOR", "imuBuildDataJson", "mqttPublishImu", "gImuRunning", None),
    "presence": ("ENABLE_PRESENCE_SENSOR", "presenceBuildDataJson", "mqttPublishPresence", "gPresenceRunning",
                 None),
    "gps": ("ENABLE_GPS_SENSOR", "gpsBuildDataJson", "mqttPublishGps", "gGpsRunning", "gGpsConnected"),
    "apds": ("ENABLE_APDS_SENSOR", "apdsBuildDataJson", "mqttPublishApds", "gApdsColorRunning", "gApdsConnected"),
    "rtc": ("ENABLE_RTC_SENSOR", "rtcBuildDataJson", "mqttPublishRtc", "gRtcRunning", "gRtcConnected"),
    "input": (None, None, "mqttPublishInput", "gInputRunning", "gInputConnected"),
}
NO_MESH = ("thermal", "camera", "microphone", "apds")
NO_MQTT = ("fmradio", "camera", "microphone")
# The list of sensorPresentMask (the SENSOR_STATUS masks): type: connected
# flag, read with no build condition.
OLD_STATUS = {"thermal": "gThermalConnected", "tof": "gTofConnected", "imu": "gImuConnected",
              "gps": "gGpsConnected", "input": "gInputConnected", "fmradio": "gFmRadioConnected",
              "rtc": "gRtcConnected", "presence": "gPresenceConnected"}
# The ladders of buildLocalBondStatus: type: (flag, running flag of the enabled
# mask, connected flag of the connected mask).
OLD_BOND = {
    "thermal": ("ENABLE_THERMAL_SENSOR", "gThermalRunning", "gThermalConnected"),
    "tof": ("ENABLE_TOF_SENSOR", "gTofRunning", "gTofConnected"),
    "imu": ("ENABLE_IMU_SENSOR", "gImuRunning", "gImuConnected"),
    "input": ("ENABLE_OLED_INPUT", "gInputRunning", "gInputConnected"),
    "gps": ("ENABLE_GPS_SENSOR", "gGpsRunning", "gGpsConnected"),
    "presence": ("ENABLE_PRESENCE_SENSOR", "gPresenceRunning", "gPresenceConnected"),
    "rtc": ("ENABLE_RTC_SENSOR", "gRtcRunning", "gRtcConnected"),
    "apds": ("ENABLE_APDS_SENSOR", "gApdsRunning", "gApdsConnected"),
    "fmradio": ("ENABLE_FM_RADIO", "gFmRadioRunning", "gFmRadioConnected"),
}
# The ladder of buildCapabilitySummary's sensorMask: type: flag.
OLD_CAPABILITY = {"thermal": "ENABLE_THERMAL_SENSOR", "tof": "ENABLE_TOF_SENSOR", "imu": "ENABLE_IMU_SENSOR",
                  "input": "ENABLE_OLED_INPUT", "apds": "ENABLE_APDS_SENSOR", "gps": "ENABLE_GPS_SENSOR",
                  "rtc": "ENABLE_RTC_SENSOR", "presence": "ENABLE_PRESENCE_SENSOR", "fmradio": "ENABLE_FM_RADIO"}
# In no sensor mask (feature bits CAP_FEATURE_CAMERA/_MICROPHONE): type: flag.
NO_MASK = {"camera": "ENABLE_CAMERA_SENSOR", "microphone": "ENABLE_MICROPHONE"}


def profiles() -> dict[str, dict[str, int]]:
    off = {flag: 0 for flag in SENSORS + INPUTS}
    every = {flag: 1 for flag in SENSORS} | {flag: 0 for flag in INPUTS}
    out = {}
    for device in INPUTS:
        out[f"all+{device}"] = every | {device: 1}
    out["all+no-input"] = every
    for flag in SENSORS:
        out[f"only-{flag}"] = off | {flag: 1}
    for device in INPUTS:
        # IMU input needs the IMU in the build (System_BuildConfig.h).
        out[f"only-{device}"] = off | {device: 1} | ({"ENABLE_IMU_SENSOR": 1} if device == "ENABLE_IMU_INPUT" else {})
    out["none"] = off
    for flags in out.values():
        flags["ENABLE_OLED_INPUT"] = int(any(flags[device] for device in INPUTS))
    return out


def expected(flags: dict[str, int]) -> list[str]:
    """Every row as the harness prints it, from the old lists."""
    input_mesh = next(((b, i, n) for f, b, i, n in OLD_INPUT_MESH if flags[f]), None)
    lines = []
    for number, name in enumerate(TYPES):
        if name == "input":
            mesh = input_mesh
        else:
            old = OLD_MESH[name]
            mesh = old[1:] if old and flags[old[0]] else None
        mesh_text = "%s,%d,%d" % mesh if mesh else "null,0,0"
        if name in OLD_MQTT:
            flag, builder, publish, running, connected = OLD_MQTT[name]
            if name == "input":
                builder = input_mesh[0] if input_mesh else None
            elif not flags[flag]:
                builder = None
            # A section that is not built has no builder; the loop then reads
            # nothing else of the row.
            mqtt_text = ",".join(x or "null" for x in (builder, publish, running, connected))
        else:
            mqtt_text = "null,null,null,null"
        if name in OLD_CAPABILITY:
            # One built column serves the three ladders: they asked the same flag.
            flag, running, connected = OLD_BOND[name]
            require(OLD_CAPABILITY[name] == flag, f"{name}: the capability and bond status ladders ask one flag")
            require(name not in OLD_STATUS or OLD_STATUS[name] == connected,
                    f"{name}: SENSOR_STATUS and the bond status read one connected flag")
            built = flags[flag]
            masks = "status+capability" if name in OLD_STATUS else "capability"
        else:
            require(name in NO_MASK and name not in OLD_STATUS, f"{name}: in no sensor mask")
            built, masks, running, connected = flags[NO_MASK[name]], "none", None, None
        mask_text = f"built={built} masks={masks} running={running or 'null'} connected={connected or 'null'}"
        lines.append(f"{name} id={number} {mask_text} mesh={mesh_text} mqtt={mqtt_text}")
    return lines


def specs_text(component: Path) -> str:
    registry = (component / "System_SensorRegistry.cpp").read_text()
    begin, end = "// SENSOR_SPECS_HOST_BEGIN", "// SENSOR_SPECS_HOST_END"
    require(registry.count(begin) == 1 and registry.count(end) == 1,
            "one SENSOR_SPECS host block in System_SensorRegistry.cpp")
    order = re.compile(r"\s*constexpr bool sensorSpecsInOrder\(\) \{.*?\n\}\n"
                       r"static_assert\(sensorSpecsInOrder\(\), [^\n]*\);\n", re.S)
    after = order.match(registry, registry.index(end) + len(end))
    require(after is not None, "the row order static_assert follows the host block")
    return registry[registry.index(begin):registry.index(end)] + after[0]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    harness = (HERE / "sensor_specs_harness.cpp").read_text()
    require(harness.count("// INSERT_SPECS") == 1, "one // INSERT_SPECS")
    harness = harness.replace("// INSERT_SPECS", specs_text(component))
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-sensor-specs-") as directory:
        unit = Path(directory) / "test.cpp"
        unit.write_text(harness)
        for name, profile in profiles().items():
            defines = [f"-D{flag}={value}" for flag, value in sorted(profile.items())]
            binary, table = Path(directory) / "specs", Path(directory) / "specs_table.o"
            subprocess.run([args.cxx, *flags, *defines, "-DSPECS_UNIT=1", "-I", str(component), "-c", str(unit),
                            "-o", str(table)], check=True, timeout=180)
            subprocess.run([args.cxx, *flags, *defines, "-I", str(component), str(unit), str(table),
                            "-o", str(binary)], check=True, timeout=180)
            rows = subprocess.run([str(binary)], check=True, timeout=30, capture_output=True,
                                  text=True).stdout.splitlines()
            want = expected(profile)
            for got_row, want_row in zip(rows, want):
                require(got_row == want_row, f"{name}: row\n  got  {got_row}\n  want {want_row}")
            require(len(rows) == len(want), f"{name}: {len(rows)} rows, want {len(want)}")
            for row in rows:
                type_name = row.split()[0]
                if type_name in NO_MESH:
                    require(" mesh=null,0,0 " in row, f"{name}: {type_name} has no mesh reading")
                if type_name in NO_MQTT:
                    require(row.endswith(" mqtt=null,null,null,null"), f"{name}: {type_name} has no MQTT section")
                if type_name in NO_MASK:
                    require(" masks=none running=null connected=null " in row, f"{name}: {type_name} is in no mask")
                if type_name == "apds":
                    require(" masks=capability running=gApdsRunning " in row,
                            f"{name}: apds is in the capability masks only, by gApdsRunning")
                require("unknown" not in row, f"{name}: {type_name} names a builder or flag of no sensor")
    print(f"sensor specs: {len(profiles())} build profiles, every row as the old mesh table, MQTT ladder "
          f"and mesh mask lists")


if __name__ == "__main__":
    main()
