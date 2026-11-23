#!/usr/bin/env python3
"""The web pages find the IMU by the device table's names, whichever chip the build has.

The Sensors page, the dashboard and the IMU card used to look for a device
named "BNO055", so a build with the Adafruit 4517 (LSM6DSOX + LIS3MDL) showed
no IMU card. They now read window.__hwImu, one statement the firmware writes
from the "imu" rows of its device table (WebServer_Utils.cpp,
webStreamImuIdentityJs; the name list itself is test_i2c_instance_core's).

The shipping scripts are extracted from WebPage_Sensors.h, WebPage_Dashboard.h
and HAL_Motion_Web.h and run in Node for both builds (imu_web_surfaces_browser_test.js):
the Sensors page's device detection and its already-running check, the
dashboard's sensor list, its card visibility and its "detected but not
compiled" notice, and the IMU card's reading (yaw is labelled relative when the reading has no heading).
Only the DOM, the hw helpers and the fetches are fake.

Source contracts (no Node needed): no page or lens source names an IMU chip
to find the IMU, and the identity statement has the shape the pages read.
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

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
CHUNK = re.compile(r"httpd_resp_send_chunk\(req,(.*?),\s*HTTPD_RESP_USE_STRLEN\);", re.DOTALL)
LITERAL = re.compile(r'"(?:\\.|[^"\\])*"')
# The two IMU builds: the device table's "imu" rows (System_I2C.cpp).
BUILDS = {
    "bno055": {"names": "{'BNO055':1}", "part": "BNO055"},
    "lsm6dsox": {"names": "{'LSM6DSOX':1,'LIS3MDL':1}", "part": "LSM6DSOX"},
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def literals(chunk: str) -> str:
    """Concatenated C string literals of one chunk; an #if/#else takes its #else lines
    (the pages' only guard here is the ANO encoder, off in these builds)."""
    active = [True]
    parts = []
    for line in chunk.splitlines():
        directive = line.strip()
        if directive.startswith("#if "):
            require(directive == "#if ENABLE_ANO_ENCODER", f"unhandled guard in a page script: {directive}")
            active.append(False)
        elif directive.startswith("#else"):
            active[-1] = active[-2]
        elif directive.startswith("#endif"):
            active.pop()
        elif active[-1] and not directive.startswith("//"):
            parts += [json.loads(match) for match in LITERAL.findall(line)]
    return "".join(parts)


def chunks(source: str) -> list[str]:
    return [match.group(1) for match in CHUNK.finditer(source)]


def one(source: str, marker: str) -> str:
    found = [chunk for chunk in chunks(source) if marker in chunk]
    require(len(found) == 1, f"expected one chunk with {marker!r}, found {len(found)}")
    return literals(found[0])


def between(source: str, first: str, last: str) -> str:
    """The chunks from the one with `first` to the one with `last`, joined."""
    all_chunks = chunks(source)
    start = [i for i, chunk in enumerate(all_chunks) if first in chunk]
    end = [i for i, chunk in enumerate(all_chunks) if last in chunk]
    require(len(start) == 1 and len(end) == 1 and start[0] <= end[0], f"script from {first!r} to {last!r}")
    return "".join(literals(chunk) for chunk in all_chunks[start[0]:end[0] + 1])


def script(text: str) -> str:
    require(text.startswith("<script>") and text.endswith("</script>"), "the chunk is one script")
    return text[len("<script>"):-len("</script>")]


def identity_format(component: Path) -> str:
    utils = (component / "WebServer_Utils.cpp").read_text()
    found = re.search(r'"(window\.__hwImu=\{names:%s,part:\'%s\',label:\'IMU \(%s\)\'\};)"', utils)
    require(found is not None, "webStreamImuIdentityJs writes window.__hwImu={names,part,label}")
    require('i2cModuleDeviceNamesJs("imu"' in utils and 'i2cModuleFirstDeviceName("imu")' in utils,
            "the identity comes from the device table's imu rows")
    return found.group(1)


def table_rows(component: Path) -> None:
    table = (component / "System_I2C.cpp").read_text()
    for name in ("LSM6DSOX", "LIS3MDL", "BNO055"):
        row = re.search(r'\{ 0x[0-9A-Fa-f]+, "' + name + r'", [^\n]*"imu", ', table)
        require(row is not None, f"the {name} row of the device table belongs to the imu module")


def no_chip_names(component: Path) -> None:
    # A chip's name may label the card or the table; it never finds the IMU.
    for file in ("WebPage_Sensors.h", "WebPage_Dashboard.h", "G2_Page_Sensors.cpp"):
        text = (component / file).read_text()
        for chip in ("'BNO055'", '"BNO055"', "'LSM6DSOX'", '"LSM6DSOX"', "IMU (BNO055)"):
            require(chip not in text, f"{file} names the IMU chip {chip}; read window.__hwImu / imuBackendName()")
    card = (component / "HAL_Motion_Web.h").read_text()
    require("device:'BNO055'" not in card and "window.__hwImu.names" in card,
            "the dashboard entry of the IMU is found by window.__hwImu.names")
    lens = (component / "G2_Page_Sensors.cpp").read_text()
    require('add(G2SensorKind::Imu, "IMU",   imuBackendName(), "imu", true,' in lens,
            "the lens names the build's chip (imuBackendName)")


def pages(component: Path) -> dict:
    sensors = (component / "WebPage_Sensors.h").read_text()
    dashboard = (component / "WebPage_Dashboard.h").read_text()
    card = (component / "HAL_Motion_Web.h").read_text()
    # The detection script opens "(function(){try{" and its Promise chain ends
    # with .finally(...); the rest of that function is other page code.
    detection = between(sensors, "[SENSORS] Device detection starting", ".finally(function(){hw.hide(loading);hw.show(grid);});")
    require(detection.startswith("console.log('[SENSORS] Device detection starting...');(function(){try{"),
            "the detection script starts as expected")
    detection += "}catch(e){throw e;}})();"
    return {
        "identity": identity_format(component),
        "builds": BUILDS,
        "detection": detection,
        "active": one(sensors, "function checkAlreadyActiveSensors()"),
        "reader": one(card, "window._sensorReaders.imu = function()"),
        "dashDef": one(card, "window.__dashSensorDefs.push("),
        "dashList": script(one(dashboard, "Section 7c: Adding helper functions")),
        "dashNotice": script(one(dashboard, "Detected but not compiled")),
        "dashVisibility": script(one(dashboard, "window.Dash.updateDeviceVisibility=function(registry)")),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node", default=os.environ.get("NODE") or shutil.which("node"))
    args = parser.parse_args()
    table_rows(COMPONENT)
    no_chip_names(COMPONENT)
    page = pages(COMPONENT)
    print("IMU web surfaces: no page names an IMU chip; the identity statement has its shape")
    if not args.node:
        raise SystemExit("Pass --node /path/to/node for JavaScript behavior tests")
    with tempfile.TemporaryDirectory(prefix="hw1-imu-web-") as tmp:
        data = Path(tmp) / "pages.json"
        data.write_text(json.dumps(page))
        for name in ("detection", "active", "reader", "dashDef", "dashList", "dashNotice", "dashVisibility"):
            js = Path(tmp) / f"{name}.js"
            js.write_text(page[name])
            subprocess.run([args.node, "--check", str(js)], check=True)
        subprocess.run([args.node, str(HERE / "imu_web_surfaces_browser_test.js"), str(data)], check=True)


if __name__ == "__main__":
    main()
