#!/usr/bin/env python3
"""Polled JSON replies are written into one measured PSRAM block.

serializeJson(doc, String&) frees the String and grows it again for every 31
characters it writes (ArduinoJson's String writer flushes a 32-byte buffer
through String::concat), each time in internal RAM below 16 KB. PsramJsonText
(System_PsramJson.h) measures the document, takes one PreferPsram block of
measureJson + 1 bytes and writes the text into it.

1. Runs the production PsramJsonText (psram_json_text_harness.cpp) against
   the old path on the real Arduino core String with its heap calls counted:
   for documents from `null` to 20 KB (escapes, embedded NUL, UTF-8, every
   number form, the 31-character flush boundary) the bytes are equal, the new
   path makes one block of length + 1 with the caller's tag and no String
   block, a refused block gives no partial text, wipe() clears the text.
2. Source contract for the eight polled sites of census-json-docs-03: each
   writes its reply through PsramJsonText and no longer serializes into a
   String; each file includes System_PsramJson.h. buildEspSrStatusJson keeps
   its String& interface (srstatus returns a static String): it assigns the
   measured text once and falls back to the old call only when the block
   cannot be had.

The handlers themselves run in ble_info_status (/api/ble/status: one block of
the reply's size, a refused block answers 500) and edge_impulse_jpeg (web
detect: one block of the reply's size).

--component points the test at another copy of components/hardwareone.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from arduino_string_host import arduino_string_counted
from psram_json_source import psram_json_text
from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"

# file, function opening, the PsramJsonText line the reply is written with
SITES = (
    ("WebPage_LLM.cpp", "static esp_err_t handleLlmPoll(",
     'const PsramJsonText resp(jdoc, "web.llm.poll");'),
    ("WebPage_Sensors.cpp", "static esp_err_t transcriptionWebReply(",
     'PsramJsonText body(doc, "web.transcription.json");'),
    ("System_EdgeImpulse.cpp", "esp_err_t handleEdgeImpulseDetect(",
     'const PsramJsonText jsonOutput(doc, "ei.detect.json");'),
    ("WebPage_Bluetooth.cpp", "static esp_err_t handleBleStatus(",
     'const PsramJsonText out(doc, "web.ble.status");'),
    ("System_ESPSR.cpp", "void buildEspSrStatusJson(String& output)",
     'const PsramJsonText text(doc, "sr.status.json");'),
    ("System_MQTT.cpp", "static void publishMeshPeerSensorData(",
     'const PsramJsonText output(doc, "mqtt.peer.json");'),
    ("WebPage_Maps.cpp", "esp_err_t handleWaypointsApi(",
     'const PsramJsonText response(doc, "web.waypoints.json");'),
    ("WebServer_Server.cpp", "esp_err_t handleSessionsList(",
     'const PsramJsonText response(doc, "web.sessions.json");'),
)
STRING_SERIALIZE = re.compile(r"\bserializeJson\(\s*\w+\s*,\s*\w+\s*\)")


def code_only(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def source_contract(component: Path) -> None:
    for name, opening, line in SITES:
        source = (component / name).read_text()
        require('#include "System_PsramJson.h"' in source, f"{name} includes System_PsramJson.h")
        require(source.count(opening) == 1, f"{name}: one {opening}")
        body = code_only(extract_block(source, opening))
        what = opening.split("(")[0].split()[-1]
        if name == "WebPage_Maps.cpp":  # the polled GET branch; the POST answer is not polled
            body = body[body.index("if (req->method == HTTP_GET)"):body.index("} else if (req->method == HTTP_POST)")]
        require(line in body, f"{what} writes its reply with `{line}`")
        if name == "System_ESPSR.cpp":
            require("if (text.ok()) output = text.c_str();\n  else serializeJson(doc, output);" in body,
                    "buildEspSrStatusJson assigns the measured text once, the old call only as the fallback")
            continue
        require(STRING_SERIALIZE.search(body) is None and "String jsonOutput" not in body and
                "String response;" not in body and "String resp;" not in body and "String out;" not in body and
                "String body;" not in body and "String output;" not in body,
                f"{what} no longer serializes its reply into a String")
    sensors = code_only(extract_block((component / "WebPage_Sensors.cpp").read_text(),
                                      "static esp_err_t transcriptionWebReply("))
    require(sensors.index("sendJsonResponse(req, body.c_str(), body.length())") < sensors.index("body.wipe();"),
            "the transcription reply's private text is wiped after the send")
    mqtt = code_only(extract_block((component / "System_MQTT.cpp").read_text(),
                                   "static void publishMeshPeerSensorData("))
    require(re.search(r"if \(output\.ok\(\)\) \{\s*esp_mqtt_client_publish\(mqttClient, peerStateTopic\.c_str\(\), "
                      r"output\.c_str\(\), 0, 0, false\);", mqtt) is not None,
            "a mesh peer state document is published only whole")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    production = psram_json_text(component)
    require("class PsramJsonText final" in production, "System_PsramJson.h defines PsramJsonText")
    harness = (HERE / "psram_json_text_harness.cpp").read_text()
    marker = "// INSERT_PRODUCTION"
    require(harness.count(marker) == 1, "one insertion marker")
    sanitize = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-omit-frame-pointer", "-g"] if args.sanitize else []
    with tempfile.TemporaryDirectory(prefix="hw1-psram-json-text-") as directory:
        d = Path(directory)
        includes, sources = arduino_string_counted(d, sanitize)
        unit, binary = d / "test.cpp", d / "test"
        unit.write_text(harness.replace(marker, production))
        subprocess.run([args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                        "-Wno-sign-compare", *sanitize, *includes, "-I", str(JSON_INCLUDE),
                        str(unit), *sources, "-o", str(binary)], check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=60)
    source_contract(component)
    print("psram json text: the eight polled replies are written into one measured PSRAM block")


if __name__ == "__main__":
    main()
