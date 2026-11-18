#!/usr/bin/env python3
"""The sensor reading path over ESP-NOW, sender to receiver, as production code.

A reading over 200 bytes used to be refused in the sender with a debug line
(ToF and FM radio readings never arrived), a receiver path cut at 255 bytes,
nothing counted a lost reading, the receiver kept only its uptime, a sensor
type from the frame indexed the name table unchecked, and the readers of the
receiver's table freed its slots.

The production code of System_ESPNow_Sensors.cpp between its
SENSOR_RX_STATE / SENSOR_TX_STATE / SENSOR_RX / SENSOR_TX _HOST_BEGIN and _END
marks (the slot and per-sender record, the sender's counters, the table and
its accessors, the ingest of a reading part and of a status frame, the sender,
its task step and the stream command's answer), remoteSensorTableEnsure and
getRemoteSensorDataJson are
compiled with the production rules header System_ESPNow_SensorCore.h. The
radio queue of the harness hands every part the sender produces to the
receiver. Checked:

  * a reading of up to 472 bytes arrives whole, in 1 to 3 frames direct and up
    to 4 through a relay, every frame within its budget; 473 bytes are not
    sent, with the reason counted, and the receiver sees the gap;
  * the receiver stamps a reading with its own UTC epoch (0, null in JSON,
    while its clock is not set) and its uptime, and says how its clock was set;
  * lost, late, repeated, restarted and resumed readings are counted per
    sender and sensor; a repeat or a late reading changes neither the stored
    text nor its stamp; an outage longer than the presence time is still
    counted (the counter state is not in the slot, and readers free nothing);
  * a part that is lost or refused by the queue gives no stored reading,
    counted incomplete and lost; parts of two senders in turn give both;
  * the bond frame and the mesh frame give the same slot;
  * types 11 to 255, short payloads, hostile part counts and a text that is
    not one JSON object are refused and counted, and nothing is written;
  * who keeps: a worker that is sent readings refuses them, counts them, says
    so once a minute and allocates nothing; a backup master and the master
    keep them; the bond partner's reading is kept whatever the mesh role;
  * the table is made by the first reading or status frame a receiver keeps,
    a sender's slots by its first status frame or its first reading that is
    whole and well formed; a status frame lists the sender's sensors before
    any reading, never makes data, never counts a reading or makes an old one
    fresh, ignores unknown bits, and is refused as a reading is when the table
    is full (also when filled by status frames alone) or memory is out;
  * every sender has its own slots: sixteen senders with eleven sensors each
    are all kept; the seventeenth sender is refused, counted and said once a
    minute; a sender that is no longer paired gives its record and its slots,
    cleared, to the next; no memory is refused, counted and said;
  * what espnowsensorstream answers after a stream was switched on;
  * the 4 KB reply of espnowsensorstatus json (sensorStatusJsonFit, with the
    vendored ArduinoJson, espnow_sensor_status_json_harness.cpp): at every
    document size around the buffer's it is whole JSON inside the buffer, and
    devicesMore says truly whether devices were left out;
  * no session: nothing is sent or parked, no counter value is taken, and the
    handshake is started by the task step; the stream-start flag on every
    change of target; every reason not to send is counted and said on the
    console once a minute, by the task step;
  * nothing that may block, print or allocate runs under a lock.

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

WIRE_DEFINES = ("ESPNOW_V4_HEADER_LEN", "ESPNOW_V4_MAX_PAYLOAD", "ESPNOW_V4_AEAD_TAG_LEN", "ESPNOW_V4_MAX_PLAINTEXT",
                "ESPNOW_V4_BROADCAST_AUTH_TAG_LEN", "ESPNOW_V4_RELAY_MAX_INNER_FRAME",
                "ESPNOW_V4_RELAY_MAX_INNER_PAYLOAD", "ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT",
                "SENSOR_READING_HEADER_LEN", "SENSOR_READING_MAX_PARTS", "SENSOR_READING_PART_MAX_DIRECT",
                "SENSOR_READING_PART_MAX_ROUTED", "SENSOR_READING_MAX_BYTES")
SENSORS_H_DEFINES = ("REMOTE_SENSOR_PEERS_MAX", "REMOTE_SENSOR_SLOTS_MAX", "REMOTE_SENSOR_TTL_MS",
                     "REMOTE_SENSOR_PRESENCE_TTL_MS")


def define(source: str, name: str) -> str:
    """The #define of `name`, with its continuation lines."""
    match = re.search(rf"^#define {name}\b(?:[^\n]*\\\n)*[^\n]*", source, re.M)
    require(match is not None, f"#define {name} not found")
    return match.group(0)


def between(source: str, name: str) -> str:
    begin, end = f"// {name}_HOST_BEGIN", f"// {name}_HOST_END"
    require(source.count(begin) == 1 and source.count(end) == 1, f"one {name} host block in System_ESPNow_Sensors.cpp")
    return source[source.index(begin):source.index(end)]


def pieces(component: Path) -> dict[str, str]:
    wire = (component / "System_ESPNow_Wire.h").read_text()
    sensors_h = (component / "System_ESPNow_Sensors.h").read_text()
    sensors = (component / "System_ESPNow_Sensors.cpp").read_text()
    json_at = sensors.index("static const char* arrivalClockName(")
    return {
        "// INSERT_WIRE": "\n".join(define(wire, name) for name in WIRE_DEFINES) + "\n" +
                          extract_block(wire, "struct __attribute__((packed)) V4PayloadSensorStatus") + ";",
        "// INSERT_SENSORS_H": "\n".join(define(sensors_h, name) for name in SENSORS_H_DEFINES) + "\n" +
                               extract_block(sensors_h, "struct RemoteSensorInfo") + ";",
        "// INSERT_RX_STATE": between(sensors, "SENSOR_RX_STATE"),
        "// INSERT_TX_STATE": between(sensors, "SENSOR_TX_STATE"),
        "// INSERT_HELPERS": extract_block(sensors, "const char* sensorTypeToString(RemoteSensorType type)") + "\n" +
                             extract_block(sensors, "bool remoteSensorTableEnsure()"),
        "// INSERT_RX": between(sensors, "SENSOR_RX"),
        "// INSERT_TX": between(sensors, "SENSOR_TX"),
        "// INSERT_JSON": extract_block(sensors[json_at:], "static const char* arrivalClockName(") + "\n" +
                          extract_block(sensors, "String getRemoteSensorDataJson(const uint8_t* deviceMac, RemoteSensorType sensorType)"),
    }


def strip_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", lambda match: "\n" * match.group(0).count("\n"), source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def check_sources(component: Path) -> None:
    """Rules for the parts no host test runs."""
    sensors = (component / "System_ESPNow_Sensors.cpp").read_text()
    code = strip_comments(sensors)
    # gRemoteSensorMux is a spinlock (interrupts are masked while it is held)
    # over records and slots in PSRAM: one sender per hold, nothing cleared in bulk.
    spans = re.findall(r"taskENTER_CRITICAL\(&gRemoteSensorMux\);(.*?)taskEXIT_CRITICAL\(&gRemoteSensorMux\);", code, re.S)
    require(len(spans) >= 12, f"the holds of gRemoteSensorMux were found ({len(spans)})")
    for span in spans:
        require("memset(" not in span, "nothing is cleared in bulk under gRemoteSensorMux")
        require(not ("REMOTE_SENSOR_PEERS_MAX" in span and "slots" in span),
                "no hold of gRemoteSensorMux walks the slots of every sender")
    announce = strip_comments(extract_block(sensors, "static void announceSensorStatus(const uint8_t* alsoTo = nullptr)"))
    require("if (gSensorStreamingEnabled[it.type]) streamingMask |= espnowsensor::sensorBit((uint8_t)it.type);" in announce,
            "the status frame's streaming bit is that sensor's own")
    require("haveTarget && espnowSessionSettled(target.mac, nowMs)" in announce and
            "espnowSessionSettled(alsoTo, nowMs)" in announce, "the status frame waits until the session has settled")
    espnow = (component / "System_ESPNow.cpp").read_text()
    status_tx = strip_comments(extract_block(espnow, "bool v4_send_sensor_status(const uint8_t* dstMac, uint32_t presentMask, "
                                                     "uint32_t streamingMask,"))
    require("payload.bootTag = bootTag;" in status_tx, "the status frame carries the boot tag")
    part_tx = strip_comments(extract_block(espnow, "bool v4_send_sensor_part(const uint8_t* dstMac, bool viaBond, const uint8_t* part, uint16_t len)"))
    require("len <= SENSOR_READING_HEADER_LEN || len > ESPNOW_V4_MAX_PLAINTEXT" in part_tx,
            "a part with no text or over one frame is not sent")
    target = strip_comments(extract_block(sensors, "static espnowsensor::TargetChoice sensorReadingTarget(int sensorType)"))
    require("sensorTargetInputsLocked(sensorType, in);" in target,
            "the target rule takes its lease inputs from sensorTargetInputsLocked (espnow_deadline_wrap runs it)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    harness = (HERE / "espnow_sensor_ingest_harness.cpp").read_text()
    for marker, text in pieces(component).items():
        require(harness.count(marker) == 1, f"one {marker}")
        harness = harness.replace(marker, text)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function", f"-I{component}"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-espnow-sensor-ingest-") as directory:
        source = Path(directory) / "sensor_ingest.cpp"
        binary = Path(directory) / "sensor_ingest"
        source.write_text(harness)
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=180)
        subprocess.run([str(binary)], check=True, timeout=120)
        status = (HERE / "espnow_sensor_status_json_harness.cpp").read_text()
        require(status.count("// INSERT_PRODUCTION") == 1, "one // INSERT_PRODUCTION in the status reply harness")
        sensors = (component / "System_ESPNow_Sensors.cpp").read_text()
        status = status.replace("// INSERT_PRODUCTION", between(sensors, "SENSOR_STATUS_JSON"))
        status_source = Path(directory) / "sensor_status_json.cpp"
        status_binary = Path(directory) / "sensor_status_json"
        status_source.write_text(status)
        json_include = component.parent / "hardwareone_libs" / "ArduinoJson" / "src"
        subprocess.run([args.cxx, *flags, "-I", str(json_include), str(status_source), "-o", str(status_binary)],
                       check=True, timeout=180)
        subprocess.run([str(status_binary)], check=True, timeout=120)
    check_sources(component)
    print("sensor readings: sent in parts, put together, stamped and counted by the receiver", flush=True)


if __name__ == "__main__":
    main()
