#!/usr/bin/env python3
"""External sensor names and values on the MQTT page are HTML-escaped.

A row of the page's External Sensors table shows the last segment of a
received topic and the first 100 bytes of its payload. Whoever publishes to a
subscribed topic chooses both, and the page is rendered for a signed-in user.
The production streamMqttExternalSensorRow (WebPage_MQTT.cpp) is compiled with
the production streamHtmlEscaped / streamHtmlEscapedChecked
(WebServer_Utils.cpp) against an httpd double: markup in either cell must
arrive as text and read back unchanged, a clean row keeps its bytes, the value
is cut at 100 received bytes and at its first NUL, and no zero-length chunk
(the chunked-response terminator) is sent for an empty cell. Source contract:
the table loop sends every row through that function and formats no received
text with %s, and the "Subscribed topics" line under the table sends the
mqttSubscribeTopics setting through streamHtmlEscaped as well.

Options: --cxx <compiler>, --sanitize (ASan + UBSan), --component <dir> to run
the same cases against another components/hardwareone directory.
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

ROW_MARKER = "static void streamMqttExternalSensorRow("
ESCAPER_MARKER = "esp_err_t streamHtmlEscapedChecked("
WRAPPER_MARKER = "void streamHtmlEscaped(httpd_req_t* req, const char* buf, size_t len)"


def production(page: str, utils: str) -> str:
    for source, marker in ((page, ROW_MARKER), (utils, ESCAPER_MARKER), (utils, WRAPPER_MARKER)):
        require(source.count(marker) == 1, f"one definition: {marker}")
    shown = [line for line in page.splitlines()
             if line.startswith("static constexpr size_t kMqttSensorValueShownMax")]
    require(len(shown) == 1, "one kMqttSensorValueShownMax")
    return "\n\n".join([extract_block(utils, ESCAPER_MARKER),
                        extract_block(utils, WRAPPER_MARKER),
                        shown[0],
                        extract_block(page, ROW_MARKER)])


def check_sources(page: str) -> None:
    inner = extract_block(page, "void streamMqttInner(")
    table = inner[inner.index("// External Sensors section"):inner.index("// Info section")]
    require("streamMqttExternalSensorRow(req, name, value, ageSec);" in table,
            "the External Sensors loop sends each row through streamMqttExternalSensorRow")
    require("name.c_str()" not in table and "value.c_str()" not in table
            and "value.substring" not in table,
            "the External Sensors loop formats no received text itself")
    row = extract_block(page, ROW_MARKER)
    require(row.count("streamHtmlEscaped(req, name.c_str()") == 1
            and row.count("streamHtmlEscaped(req, value.c_str()") == 1,
            "the row escapes the name and the value")
    require("%s" not in row, "the row formats no text with %s")
    require("streamHtmlEscaped(req, gSettings.mqttSubscribeTopics.c_str()," in inner
            and not re.search(r"httpd_resp_send_chunk\([^;]*mqttSubscribeTopics", inner),
            "the subscribed topics line is sent HTML-escaped")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    page = (args.component / "WebPage_MQTT.cpp").read_text()
    utils = (args.component / "WebServer_Utils.cpp").read_text()
    harness = (HERE / "web_mqtt_sensor_rows_harness.cpp").read_text()
    require(harness.count("// INSERT_PRODUCTION") == 1, "one insertion marker")
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-web-mqtt-sensor-rows-") as directory:
        source = Path(directory) / "rows.cpp"
        binary = Path(directory) / "rows"
        source.write_text(harness.replace("// INSERT_PRODUCTION", production(page, utils)))
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=60)
    check_sources(page)
    print("MQTT page external sensor rows are escaped", flush=True)


if __name__ == "__main__":
    main()
