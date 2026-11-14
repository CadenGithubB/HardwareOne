#!/usr/bin/env python3
"""Execute the MQTT discovery builders and external-sensor copy on a host.

publishDiscoveryConfig, publishPeerDiscoveryConfig (with their shared PSRAM
buffer and bounded writer), updateExternalSensor (with its table state) and
macToPathToken are taken intact from the production sources on every run.
A 400-character base topic must skip the discovery publish instead of writing
past the 1 KB buffer, a sweep across the capacity checks that every document
that fits is sent byte-exact, and esp-mqtt's length-delimited topic/payload
must be copied with exactly topic_len/data_len bytes. The client's own command
and response topics (System_MQTTControlTopic.h) never reach the sensor table:
the command payload is the {user, pass, cmd} envelope. Run with --sanitize to
make any write or read past a buffer an ASan failure.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

from test_web_batch_handlers import extract_block

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def definitions(mqtt: str, mac_helpers: str, control_topic: str) -> str:
    buffer_lines = re.search(
        r"(?m)^static constexpr size_t kMqttDiscoveryJsonCap = \d+;\n"
        r"EXT_RAM_BSS_ATTR static char sMqttDiscoveryConfigJson\[kMqttDiscoveryJsonCap\];",
        mqtt)
    require(buffer_lines is not None, "discovery buffer declaration not found")
    sensor_state = mqtt[mqtt.index("struct ExternalSensor {"):
                        mqtt.index("static bool initExternalSensorStorage()")]
    # A tree from before the control-topic filter still compiles; the harness
    # then reports the stored command envelope as its failure.
    parts = [f"#define HW1_MQTT_CONTROL_TOPIC_PRESENT {1 if control_topic else 0}",
             mac_helpers, control_topic,
             buffer_lines[0], sensor_state]
    # The bounded writer is part of the fix; the pre-fix builders do not use it.
    if "class MqttJsonWriter final" in mqtt:
        parts.append(extract_block(mqtt, "class MqttJsonWriter final") + ";")
    parts += [extract_block(mqtt, "static void publishDiscoveryConfig("),
              extract_block(mqtt, "static void publishPeerDiscoveryConfig("),
              extract_block(mqtt, "static void updateExternalSensor(")]
    return "\n\n".join(parts)


def source_guards(mqtt: str) -> None:
    # One spelling of the two topic words, and the filter sits in the function
    # that stores, so no caller can reach the table around it.
    require('"/command"' not in mqtt and '"/response"' not in mqtt,
            "System_MQTT.cpp spells the command/response topics through "
            "System_MQTTControlTopic.h")
    sensor = extract_block(mqtt, "static void updateExternalSensor(")
    require(sensor.index("mqttIsOwnControlTopic(topic") < sensor.index("xSemaphoreTake("),
            "updateExternalSensor drops the client's control topics before it stores")
    subscribe = extract_block(mqtt, "static void subscribeToCommandTopic(")
    require(subscribe.index("sMqttSubscribedBaseTopic = gSettings.mqttBaseTopic;")
            < subscribe.index("esp_mqtt_client_subscribe("),
            "the subscribed base topic is recorded before the command subscription")
    require("sizeof(configJson) - pos" not in mqtt,
            "discovery builders still advance an unchecked snprintf position")
    state = extract_block(mqtt, "void publishMqttSensorData(")
    guard = state.index("if (pos <= 0 || pos >= 16384)")
    require(guard < state.index("esp_mqtt_client_publish(mqttClient, stateTopic"),
            "the state document is checked before it is published")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--baseline", type=Path,
                        help="run the same cases against another components/hardwareone directory")
    parser.add_argument("--case", choices=("discovery", "peer", "sensor"),
                        help="run one group of cases only")
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    source_dir = args.baseline or COMPONENT
    mqtt = (source_dir / "System_MQTT.cpp").read_text()
    mac_header = source_dir / "System_MacText.h"
    if mac_header.exists():
        mac_helpers = mac_header.read_text().replace("#pragma once\n", "", 1)
    else:  # a baseline tree from before the header existed
        mac_helpers = extract_block((source_dir / "System_Utils.h").read_text(),
                                    "inline void macToPathToken(")
    control_header = source_dir / "System_MQTTControlTopic.h"
    control_topic = (control_header.read_text().replace("#pragma once\n", "", 1)
                     if control_header.exists() else "")
    harness = (HERE / "mqtt_json_bounds_harness.cpp").read_text()
    marker = "// INSERT_PRODUCTION_DEFINITIONS"
    require(harness.count(marker) == 1, "harness insertion marker")
    with tempfile.TemporaryDirectory(prefix="hw1-mqtt-json-bounds-") as temp:
        unit = Path(temp) / "mqtt_json_bounds.cpp"
        unit.write_text(harness.replace(
            marker, definitions(mqtt, mac_helpers, control_topic)))
        executable = Path(temp) / "mqtt_json_bounds"
        command = [args.cxx, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
                   str(unit), "-o", str(executable)]
        if args.sanitize:
            command[1:1] = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(executable), *([args.case] if args.case else [])],
                                timeout=60)
        if result.returncode != 0:
            raise SystemExit(result.returncode)
    source_guards(mqtt)
    print("MQTT JSON bounds: discovery, peer discovery, external-sensor copy "
          "and control-topic filter pass")


if __name__ == "__main__":
    main()
