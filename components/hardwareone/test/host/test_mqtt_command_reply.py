#!/usr/bin/env python3
"""Execute the MQTT command reply path (cmd_exec completion) on a host.

MqttCommandReply, enqueueMqttCommandReply and mqttCommandCompleted are
extracted intact from System_MQTT.cpp on every run and compiled with the
production lifecycle gate and movable admission, vendored ArduinoJson and a
small esp-mqtt model (direct publish leaves at once, an enqueued item waits
for the client task's next loop pass, stopping deletes the outbox). A command
that stops MQTT must still get its reply; other replies go through the outbox.
"""

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]
JSON_INCLUDE = COMPONENT.parent / "hardwareone_libs" / "ArduinoJson" / "src"


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def definitions(mqtt: str) -> str:
    for marker in ("struct MqttCommandReply {",
                   "static void enqueueMqttCommandReply(",
                   "static void mqttCommandCompleted("):
        require(marker in mqtt, f"{marker} not found in System_MQTT.cpp")
    return "\n\n".join([
        extract_block(mqtt, "struct MqttCommandReply") + ";",
        extract_block(mqtt, "static void enqueueMqttCommandReply("),
        extract_block(mqtt, "static void mqttCommandCompleted("),
    ])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path,
                        help="run the same cases against another components/hardwareone directory")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    source_dir = args.baseline or COMPONENT
    mqtt = (source_dir / "System_MQTT.cpp").read_text()
    harness = (HERE / "mqtt_command_reply_harness.cpp").read_text()
    marker = "// INSERT_PRODUCTION_DEFINITIONS"
    require(harness.count(marker) == 1, "harness insertion marker")
    with tempfile.TemporaryDirectory(prefix="hw1-mqtt-command-reply-") as temp:
        directory = Path(temp)
        (directory / "Arduino.h").write_text(
            (HERE / "web_batch_stubs" / "Arduino.h").read_text())
        unit = directory / "mqtt_command_reply.cpp"
        unit.write_text(harness.replace(marker, definitions(mqtt)))
        executable = directory / "mqtt_command_reply"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   "-DARDUINOJSON_ENABLE_ARDUINO_STRING=1",
                   "-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0",
                   "-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0",
                   "-DARDUINOJSON_ENABLE_PROGMEM=0",
                   "-I", str(directory), "-I", str(source_dir), "-I", str(JSON_INCLUDE),
                   str(unit), "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(executable)], timeout=60)
        if result.returncode != 0:
            raise SystemExit(result.returncode)
    print("MQTT command reply: stop-command reply, queued reply and routed document pass")


if __name__ == "__main__":
    main()
