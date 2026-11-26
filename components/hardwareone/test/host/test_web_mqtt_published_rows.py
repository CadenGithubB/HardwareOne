#!/usr/bin/env python3
"""The sensor rows of the MQTT page's Published Data table.

Each row was an #if ENABLE_<X> block of streamMqttInner (WebPage_MQTT.cpp):
built, the label cell and then the publish setting's Enabled / Disabled span,
two chunks; not built, one chunk with the label and "Not compiled". The
production kMqttPublishedSensorRows and streamMqttPublishedSensorRows now
send them from kSensorSpecs[].built and *mqtt.publish. They are compiled with
the production System_SensorRegistry.h against an httpd double that records
every chunk and stand-in kSensorSpecs rows, and run for every combination of
built and publish over the eight rows: the chunks must be those of the old
blocks, in the old order (thermal, tof, imu, presence, gps, apds, rtc,
input), with the old labels and literals. That built is each old block's flag
(input: ENABLE_OLED_INPUT, GT911 touch too) and publish its setting is pinned
by sensor_specs. Source contract: streamMqttInner sends the rows once,
between the WiFi Info row and the table's close, and the file has no
per-sensor #if.

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

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

# The old blocks, in page order: (RemoteSensorType, label).
OLD_ROWS = (("REMOTE_SENSOR_THERMAL", "Thermal Sensor"), ("REMOTE_SENSOR_TOF", "ToF Sensor"),
            ("REMOTE_SENSOR_IMU", "IMU Sensor"), ("REMOTE_SENSOR_PRESENCE", "Presence Sensor"),
            ("REMOTE_SENSOR_GPS", "GPS Location"), ("REMOTE_SENSOR_APDS", "APDS (Proximity)"),
            ("REMOTE_SENSOR_RTC", "RTC Time"), ("REMOTE_SENSOR_INPUT", "Input Device"))

TABLE = re.compile(r"(?ms)^struct MqttPublishedSensorRow \{.*?^\};\n"
                   r"static constexpr MqttPublishedSensorRow kMqttPublishedSensorRows\[\] = \{.*?^\};\n")
FUNCTION = "static void streamMqttPublishedSensorRows("

DRIVER = r'''
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <sys/types.h>
#include "System_SensorRegistry.h"

#define HTTPD_RESP_USE_STRLEN -1
struct httpd_req_t { std::vector<std::string> chunks; };
static int httpd_resp_send_chunk(httpd_req_t* req, const char* text, ssize_t len) {
  req->chunks.emplace_back(text, len < 0 ? strlen(text) : (size_t)len);
  return 0;
}
static SensorSpec hostSpecs[REMOTE_SENSOR_MAX];
#define kSensorSpecs hostSpecs

// INSERT_PRODUCTION

struct OldRow { uint8_t type; const char* label; };
static const OldRow kOld[] = {
// INSERT_EXPECTED
};
static constexpr unsigned kRows = sizeof(kOld) / sizeof(kOld[0]);

int main() {
  static bool publish[REMOTE_SENSOR_MAX];
  unsigned cases = 0;
  for (unsigned built = 0; built < (1u << kRows); ++built) {
    for (unsigned on = 0; on < (1u << kRows); ++on) {
      for (int t = 0; t < REMOTE_SENSOR_MAX; ++t) {
        // Types with no row are built and switched on: they must not show.
        hostSpecs[t] = SensorSpec{};
        hostSpecs[t].built = true;
        publish[t] = true;
        hostSpecs[t].mqtt.publish = &publish[t];
      }
      std::vector<std::string> want;
      for (unsigned i = 0; i < kRows; ++i) {
        hostSpecs[kOld[i].type].built = (built >> i) & 1;
        publish[kOld[i].type] = (on >> i) & 1;
        const std::string open = std::string("</td></tr><tr><td>") + kOld[i].label + "</td><td>";
        if ((built >> i) & 1) {
          want.push_back(open);
          want.push_back((on >> i) & 1 ? "<span style=\"color:var(--success);\">✓ Enabled</span>"
                                       : "<span style=\"color:var(--panel-fg);\">Disabled</span>");
        } else {
          want.push_back(open + "<span style=\"color:var(--panel-fg);\">Not compiled</span>");
        }
      }
      httpd_req_t req;
      streamMqttPublishedSensorRows(&req);
      if (req.chunks != want) {
        std::printf("built=0x%02x publish=0x%02x: %zu chunks, want %zu\n", built, on, req.chunks.size(),
                    want.size());
        for (size_t k = 0; k < req.chunks.size() || k < want.size(); ++k) {
          std::printf("  got  [%s]\n  want [%s]\n", k < req.chunks.size() ? req.chunks[k].c_str() : "",
                      k < want.size() ? want[k].c_str() : "");
        }
        return 1;
      }
      ++cases;
    }
  }
  std::printf("%u cases", cases);
  return 0;
}
'''


def production(page: str) -> str:
    table = TABLE.search(page)
    require(table is not None and len(TABLE.findall(page)) == 1, "one kMqttPublishedSensorRows table")
    require(page.count(FUNCTION) == 1, "one streamMqttPublishedSensorRows")
    return table[0] + "\n" + extract_block(page, FUNCTION)


def check_sources(page: str) -> None:
    inner = extract_block(page, "void streamMqttInner(")
    require(inner.count("streamMqttPublishedSensorRows(req);") == 1, "streamMqttInner sends the rows once")
    wifi = inner.index("<tr><td>WiFi Info</td><td>")
    rows = inner.index("streamMqttPublishedSensorRows(req);")
    close = inner.index('"</td></tr></table></div>"')
    require(wifi < rows < close, "the rows follow the WiFi Info row and close the table")
    require("mqttPublishSystem" in inner[:wifi] and "mqttPublishWifi" in inner[wifi:rows],
            "only the System and WiFi rows are written out in streamMqttInner")
    require(not re.search(r"mqttPublish(Thermal|Tof|Imu|Presence|Gps|Apds|Rtc|Input)", page),
            "the page reads the sensor publish settings through kSensorSpecs")
    guards = re.findall(r"(?m)^[ \t]*#[ \t]*(?:if|elif|ifdef|ifndef)\b[^\n]*", page)
    require(guards == ["#if ENABLE_WEB_MQTT"], f"the page has no per-sensor condition: {guards}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    page = (component / "WebPage_MQTT.cpp").read_text()
    check_sources(page)
    expected = "\n".join(f'  {{ {t}, "{label}" }},' for t, label in OLD_ROWS)
    unit_text = DRIVER.replace("// INSERT_PRODUCTION", production(page)).replace("// INSERT_EXPECTED", expected)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-web-mqtt-published-rows-") as directory:
        unit = Path(directory) / "rows.cpp"
        binary = Path(directory) / "rows"
        unit.write_text(unit_text)
        subprocess.run([args.cxx, *flags, "-I", str(component), str(unit), "-o", str(binary)],
                       check=True, timeout=180)
        run = subprocess.run([str(binary)], timeout=60, capture_output=True, text=True)
        require(run.returncode == 0, "web_mqtt_published_rows: FAILED\n" + run.stdout[-4000:])
    print(f"MQTT page Published Data rows: {run.stdout.strip()}, the chunks of the old #if blocks", flush=True)


if __name__ == "__main__":
    main()
