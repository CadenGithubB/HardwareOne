#!/usr/bin/env python3
"""Two recurring reply buffers are made once in PSRAM and reused.

- handleLogs (/api/cli/logs, polled every 500 ms by the CLI page) took an
  8 KB PreferPsram block per poll, copied the web mirror into it, sent it and
  freed it (census-raw-alloc-hot-07). It now keeps one RequirePsram block for
  later polls; every handler runs on the one httpd task. The kept block keeps
  its own size, so a later gWebMirrorCap never makes a poll write past it.
- publishMqttSensorData (every MQTT publish interval, 1 to 10 s and more) did
  the same with a 16 KB state buffer (census-raw-alloc-hot-12). It now keeps
  one RequirePsram block until stopMqttNow releases it.
When PSRAM refuses, the call borrows a PreferPsram block and frees it, as
every call did before; with no memory at all the old error reply or warning
stands.

Runs the production handleLogs (with the production WebMirrorBuf::snapshotTo)
and publishMqttSensorData (sensor sections and ESP-NOW compiled off, with
releaseMqttStateJson) against doubles that record every ps_alloc: replies and
documents are those of before, the block is asked once and kept, refusals
borrow and free. Source contract: stopMqttNow releases the buffer on both of
its finished paths, next to the external sensor storage.

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

from test_web_batch_handlers import extract_block, require

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def production(component: Path) -> dict[str, str]:
    server = (component / "WebServer_Server.cpp").read_text()
    utils = (component / "WebServer_Utils.cpp").read_text()
    mqtt = (component / "System_MQTT.cpp").read_text()
    kept = re.search(r"(?m)^static char\* sMqttStateJson = nullptr;$", mqtt)
    require(kept is not None, "System_MQTT.cpp keeps the state buffer in sMqttStateJson")
    require("static char* sLogsResponseBuf = nullptr;" in extract_block(server, "esp_err_t handleLogs("),
            "handleLogs keeps its response buffer in sLogsResponseBuf")
    return {
        "// INSERT_LOGS": extract_block(utils, "size_t WebMirrorBuf::snapshotTo(") + "\n\n" +
                          extract_block(server, "esp_err_t handleLogs("),
        "// INSERT_MQTT": kept[0] + "\n" + extract_block(mqtt, "static void releaseMqttStateJson(") + "\n\n" +
                          extract_block(mqtt, "void publishMqttSensorData("),
    }


def source_contract(component: Path) -> None:
    mqtt = (component / "System_MQTT.cpp").read_text()
    stop = extract_block(mqtt, "static bool stopMqttNow(")
    require(stop.count("releaseMqttStateJson();") == 2 and
            stop.count("releaseExternalSensorStorage();\n    releaseMqttStateJson();\n    return true;") == 1 and
            stop.count("releaseExternalSensorStorage();\n  releaseMqttStateJson();\n") == 1,
            "stopMqttNow releases the state buffer on both finished paths")
    require(mqtt.count("releaseMqttStateJson()") == 3, "only stopMqttNow releases the state buffer")
    require(mqtt.count("sMqttStateJson") == 7, "sMqttStateJson is named only by its owner and publisher")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    harness = (HERE / "reused_reply_buffers_harness.cpp").read_text()
    for marker, text in production(component).items():
        require(harness.count(marker) == 1, f"one {marker}")
        harness = harness.replace(marker, text)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
             "-Wno-format"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-reused-reply-buffers-") as directory:
        unit = Path(directory) / "test.cpp"
        unit.write_text(harness)
        for case in ("TEST_LOGS", "TEST_MQTT"):
            binary = Path(directory) / case.lower()
            subprocess.run([args.cxx, *flags, f"-D{case}=1", "-I", str(HERE / "web_batch_stubs"),
                            str(unit), "-o", str(binary)], check=True, timeout=180)
            subprocess.run([str(binary)], check=True, timeout=30)
    source_contract(component)
    print("reused reply buffers: handleLogs and the MQTT state document keep one PSRAM block")


if __name__ == "__main__":
    main()
