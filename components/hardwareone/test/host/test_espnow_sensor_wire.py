#!/usr/bin/env python3
"""The wire layouts of the sensor and time frames, and the one sensor numbering.

1.0 freezes these layouts, so they are pinned here byte by byte.

Compiled: V4PayloadSensorReading, V4PayloadSensorStatus, V4PayloadSensorReq,
V4PayloadTimeSync, V4PayloadPowerStatus and V4PayloadEventPush as
System_ESPNow_Wire.h declares them, with its frame budget constants, against
the pack and parse functions of the three production rules headers
(System_ESPNow_SensorCore.h, System_ESPNow_TimeCore.h,
System_ESPNow_PowerEventCore.h): every size and offset, the bytes the code
writes are the bytes the struct states, reserved bytes are 0, and every frame
fits one frame through a relay.

Source rules:
  * one numbering: every CAP_SENSOR_* is espnowsensor::sensorBit of its
    RemoteSensorType and types no number; SENSOR_NAMES and kRemoteSensorNames
    are gone; the OLED bond page and the bond web page type no sensor mask
    (the web page takes them from the device's sensorBits);
    espnowsensorstatus has no list of sensor names of its own;
  * the old frames and limits are gone (V4PayloadSensorBroadcast,
    V4PayloadSensorData, the 200-byte gate, the 256-byte buffer, the bond
    sequence number, the status broadcast);
  * the receive rows for 151, 153 and 154 ask for a paired sender and a
    session-encrypted frame and nothing else; 179 keeps its bond gate; the
    three mesh frames may travel through a relay and none is flooded;
  * the readers of the receiver's table (OLED, G2, MQTT, web) reach it only
    through the copying accessors;
  * the table is made by the first reading or status frame a device keeps,
    after the role rule, and never by ESP-NOW start; the status frame writes
    no reading's text, freshness, counter state or counts; a worker keeps
    nothing the mesh sends it;
  * the stream command says where the readings go;
  * the task-loop steps for sensor readings are outside the bonded-mode block
    (sending readings to a mesh master needs no bond);
  * the status frame is sent as a session unicast;
  * espnowmasterfingerprint and espnowbackupfingerprint take 'clear' and a
    checked 64-hex value (rules in System_ESPNow_SensorCore.h), and the web
    settings page sends an emptied field as 'clear'.

Options: --cxx <compiler>, --sanitize, --component <dir>.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

from test_web_batch_handlers import extract_block, require
from test_espnow_sensor_ingest import WIRE_DEFINES, define

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

SENSORS = ("THERMAL", "TOF", "IMU", "INPUT", "APDS", "GPS", "RTC", "PRESENCE", "FMRADIO")
GONE = ("V4PayloadSensorBroadcast", "V4PayloadSensorData", "REMOTE_SENSOR_BUFFER_SIZE", "MAX_REMOTE_DEVICES",
        "MAX_SENSORS_PER_DEVICE", "SENSOR_NAMES", "kRemoteSensorNames", "v4_send_sensor_envelope",
        "sendBondedSensorData", "gBondSensorSeqNum", "v4_broadcast_sensor_status", "broadcastSensorStatus",
        "findOrCreateCacheEntry", "updateRemoteSensorStatus")
STRUCTS = ("V4PayloadTimeSync", "V4PayloadSensorStatus", "V4PayloadSensorReading", "V4PayloadSensorReq",
           "V4PayloadPowerStatus", "V4PayloadEventPush")

HARNESS = r"""
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "System_ESPNow_PowerEventCore.h"
#include "System_ESPNow_SensorCore.h"
#include "System_ESPNow_TimeCore.h"

// INSERT_WIRE

static int failures = 0, checks = 0;
static void check(bool ok, const char* what) {
  ++checks;
  if (ok) return;
  ++failures;
  std::fprintf(stderr, "FAIL: %s\n", what);
}

int main() {
  // The frame budgets every layout below is sized against.
  check(ESPNOW_V4_MAX_PLAINTEXT == 202 && ESPNOW_V4_RELAY_MAX_INNER_FRAME == 178 &&
        ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT == 130, "202 bytes direct, 130 through a relay");
  check(SENSOR_READING_HEADER_LEN == 12 && SENSOR_READING_MAX_PARTS == 4 && SENSOR_READING_PART_MAX_DIRECT == 190 &&
        SENSOR_READING_PART_MAX_ROUTED == 118 && SENSOR_READING_MAX_BYTES == 472, "the reading's sizes");
  check(espnowsensor::kFrameCapDirect == ESPNOW_V4_MAX_PLAINTEXT &&
        espnowsensor::kFrameCapRouted == ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT &&
        espnowsensor::kReadingMaxBytes == SENSOR_READING_MAX_BYTES &&
        espnowsensor::kReadingHeaderLen == SENSOR_READING_HEADER_LEN, "the rules header plans with the wire header's numbers");
  check(SENSOR_READING_HEADER_LEN + SENSOR_READING_PART_MAX_ROUTED == ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT &&
        SENSOR_READING_HEADER_LEN + SENSOR_READING_PART_MAX_DIRECT == ESPNOW_V4_MAX_PLAINTEXT,
        "a part fills one frame exactly on either path");

  // The reading header: 12 bytes.
  check(sizeof(V4PayloadSensorReading) == 12, "V4PayloadSensorReading is 12 bytes");
  check(offsetof(V4PayloadSensorReading, sensorType) == 0 && offsetof(V4PayloadSensorReading, flags) == 1 &&
        offsetof(V4PayloadSensorReading, bootTag) == 2 && offsetof(V4PayloadSensorReading, counter) == 4 &&
        offsetof(V4PayloadSensorReading, parts) == 6 && offsetof(V4PayloadSensorReading, reserved) == 7 &&
        offsetof(V4PayloadSensorReading, ageMs) == 8, "its offsets: 0 type, 1 flags, 2 boot tag, 4 counter, 6 parts, 7 reserved, 8 age");
  {
    V4PayloadSensorReading wire{};
    wire.sensorType = REMOTE_SENSOR_RTC;
    wire.flags = espnowsensor::kReadingFlagStreamStart | espnowsensor::kReadingFlagAgeValid;
    wire.bootTag = 0x1234;
    wire.counter = 0xFFFE;
    wire.parts = (uint8_t)((4u << 4) | 3u);  // part 3 of 4
    wire.ageMs = 0x89ABCDEFu;
    espnowsensor::ReadingHeader h{};
    h.sensorType = REMOTE_SENSOR_RTC;
    h.flags = 0x03;
    h.bootTag = 0x1234;
    h.counter = 0xFFFE;
    h.partIndex = 3;
    h.partCount = 4;
    h.ageMs = 0x89ABCDEFu;
    uint8_t packed[12];
    std::memset(packed, 0xEE, sizeof(packed));
    espnowsensor::packReadingHeader(packed, h);
    check(std::memcmp(packed, &wire, sizeof(wire)) == 0, "packReadingHeader writes the bytes the struct states");
    espnowsensor::ReadingHeader back{};
    check(espnowsensor::parseReadingHeader((const uint8_t*)&wire, sizeof(wire), back) && back.partIndex == 3 &&
          back.partCount == 4 && back.counter == 0xFFFE && back.bootTag == 0x1234 && back.ageMs == 0x89ABCDEFu &&
          back.flags == 0x03, "parseReadingHeader reads them back");
  }

  // The status frame: 16 bytes, two masks in the one numbering.
  check(sizeof(V4PayloadSensorStatus) == 16 && offsetof(V4PayloadSensorStatus, presentMask) == 0 &&
        offsetof(V4PayloadSensorStatus, streamingMask) == 4 && offsetof(V4PayloadSensorStatus, bootTag) == 8 &&
        offsetof(V4PayloadSensorStatus, reserved) == 10 && sizeof(((V4PayloadSensorStatus*)nullptr)->reserved) == 6,
        "V4PayloadSensorStatus: 0 present, 4 streaming, 8 boot tag, 10 reserved[6]");

  // The request: 16 bytes with two reserved.
  check(sizeof(V4PayloadSensorReq) == 16 && offsetof(V4PayloadSensorReq, reqId) == 0 &&
        offsetof(V4PayloadSensorReq, mode) == 1 && offsetof(V4PayloadSensorReq, sensorMask) == 2 &&
        offsetof(V4PayloadSensorReq, intervalMs) == 6 && offsetof(V4PayloadSensorReq, leaseMs) == 10 &&
        offsetof(V4PayloadSensorReq, reserved) == 14, "V4PayloadSensorReq: 0 id, 1 mode, 2 mask, 6 interval, 10 lease, 14 reserved[2]");

  // The time frame: 16 bytes.
  check(sizeof(V4PayloadTimeSync) == 16 && offsetof(V4PayloadTimeSync, epochMs) == 0 &&
        offsetof(V4PayloadTimeSync, source) == 8 && offsetof(V4PayloadTimeSync, flags) == 9 &&
        offsetof(V4PayloadTimeSync, sinceSetMin) == 10 && offsetof(V4PayloadTimeSync, reserved) == 12,
        "V4PayloadTimeSync: 0 epoch ms, 8 source, 9 flags, 10 minutes since set, 12 reserved[4]");
  {
    V4PayloadTimeSync wire{};
    wire.epochMs = 1763388202417LL;
    wire.source = meshtime::kSrcNtp;
    wire.flags = meshtime::kFlagActingBackup;
    wire.sinceSetMin = 4242;
    meshtime::Frame f{};
    f.epochMs = 1763388202417LL;
    f.source = meshtime::kSrcNtp;
    f.flags = meshtime::kFlagActingBackup;
    f.sinceSetMin = 4242;
    uint8_t packed[16];
    std::memset(packed, 0xEE, sizeof(packed));
    meshtime::packFrame(packed, f);
    check(std::memcmp(packed, &wire, sizeof(wire)) == 0, "meshtime::packFrame writes the bytes the struct states");
  }

  // The battery frame: 24 bytes.
  check(sizeof(V4PayloadPowerStatus) == 24 && offsetof(V4PayloadPowerStatus, counter) == 0 &&
        offsetof(V4PayloadPowerStatus, bootTag) == 2 && offsetof(V4PayloadPowerStatus, status) == 4 &&
        offsetof(V4PayloadPowerStatus, backend) == 5 && offsetof(V4PayloadPowerStatus, known) == 6 &&
        offsetof(V4PayloadPowerStatus, voltageMv) == 8 && offsetof(V4PayloadPowerStatus, percentX10) == 10 &&
        offsetof(V4PayloadPowerStatus, rateX10) == 12 && offsetof(V4PayloadPowerStatus, sampleAgeMs) == 14 &&
        offsetof(V4PayloadPowerStatus, reserved) == 16 && sizeof(((V4PayloadPowerStatus*)nullptr)->reserved) == 8,
        "V4PayloadPowerStatus: 0 counter, 2 boot tag, 4 status, 5 backend, 6 known, 8 mV, 10 percent x10, 12 rate x10, 14 sample age, 16 reserved[8]");
  {
    V4PayloadPowerStatus wire{};
    wire.counter = 0xFFFE;
    wire.bootTag = 0x1234;
    wire.status = 5;
    wire.backend = espnowsensor::kPowerBackendFuelGauge;
    wire.known = espnowsensor::kPowerKnownAll;
    wire.voltageMv = 3301;
    wire.percentX10 = 42;
    wire.rateX10 = -123;
    wire.sampleAgeMs = 65535;
    espnowsensor::PowerFrame f{};
    f.counter = 0xFFFE;
    f.bootTag = 0x1234;
    f.status = 5;
    f.backend = espnowsensor::kPowerBackendFuelGauge;
    f.known = espnowsensor::kPowerKnownAll;
    f.voltageMv = 3301;
    f.percentX10 = 42;
    f.rateX10 = -123;
    f.sampleAgeMs = 65535;
    uint8_t packed[24];
    std::memset(packed, 0xEE, sizeof(packed));
    espnowsensor::packPowerFrame(packed, f);
    check(std::memcmp(packed, &wire, sizeof(wire)) == 0, "packPowerFrame writes the bytes the struct states, reserved 0");
    espnowsensor::PowerFrame back{};
    check(espnowsensor::parsePowerFrame((const uint8_t*)&wire, sizeof(wire), back) && back.counter == 0xFFFE &&
          back.rateX10 == -123 && back.known == espnowsensor::kPowerKnownAll && back.sampleAgeMs == 65535,
          "parsePowerFrame reads them back");
  }

  // The event frame: 128 bytes.
  check(sizeof(V4PayloadEventPush) == 128 && offsetof(V4PayloadEventPush, counter) == 0 &&
        offsetof(V4PayloadEventPush, bootTag) == 2 && offsetof(V4PayloadEventPush, flags) == 4 &&
        offsetof(V4PayloadEventPush, tier) == 5 && offsetof(V4PayloadEventPush, source) == 6 &&
        offsetof(V4PayloadEventPush, reserved0) == 7 && offsetof(V4PayloadEventPush, skipped) == 8 &&
        offsetof(V4PayloadEventPush, reserved1) == 10 && offsetof(V4PayloadEventPush, eventSeq) == 12 &&
        offsetof(V4PayloadEventPush, ageMs) == 16 && offsetof(V4PayloadEventPush, kind) == 20 &&
        offsetof(V4PayloadEventPush, subject) == 52 && offsetof(V4PayloadEventPush, detail) == 84 &&
        offsetof(V4PayloadEventPush, who) == 116 && sizeof(((V4PayloadEventPush*)nullptr)->who) == 12,
        "V4PayloadEventPush: 0 counter, 2 boot tag, 4 flags, 5 tier, 6 source, 8 skipped, 12 event number, 16 age, "
        "20 kind[32], 52 subject[32], 84 detail[32], 116 who[12]");
  {
    V4PayloadEventPush wire{};
    wire.counter = 7;
    wire.bootTag = 0xA2B3;
    wire.flags = espnowsensor::kEventCutDetail;
    wire.tier = espnowsensor::kEventTierAlert;
    wire.source = espnowsensor::kEventSourceWeb;
    wire.skipped = 300;
    wire.eventSeq = 0x01020304u;
    wire.ageMs = 0x89ABCDEFu;
    std::memcpy(wire.kind, "login_fail", 10);
    std::memcpy(wire.subject, "admin", 5);
    std::memset(wire.detail, 'd', sizeof(wire.detail));
    std::memcpy(wire.who, "1.2.3.4", 7);
    espnowsensor::EventFrame f{};
    f.counter = 7;
    f.bootTag = 0xA2B3;
    f.flags = espnowsensor::kEventCutDetail;
    f.tier = espnowsensor::kEventTierAlert;
    f.source = espnowsensor::kEventSourceWeb;
    f.skipped = 300;
    f.eventSeq = 0x01020304u;
    f.ageMs = 0x89ABCDEFu;
    espnowsensor::eventFieldPut(f.kind, sizeof(f.kind), "login_fail", 11);
    espnowsensor::eventFieldPut(f.subject, sizeof(f.subject), "admin", 6);
    const char longDetail[] = "dddddddddddddddddddddddddddddddddddddddd";
    check(espnowsensor::eventFieldPut(f.detail, sizeof(f.detail), longDetail, sizeof(longDetail)), "a long detail is cut");
    espnowsensor::eventFieldPut(f.who, sizeof(f.who), "1.2.3.4", 8);
    uint8_t packed[128];
    std::memset(packed, 0xEE, sizeof(packed));
    espnowsensor::packEventFrame(packed, f);
    check(std::memcmp(packed, &wire, sizeof(wire)) == 0, "packEventFrame writes the bytes the struct states, reserved 0");
    espnowsensor::EventFrame back{};
    check(espnowsensor::parseEventFrame((const uint8_t*)&wire, sizeof(wire), back) && back.eventSeq == 0x01020304u &&
          back.ageMs == 0x89ABCDEFu && back.skipped == 300 && back.tier == 2 && back.source == 3,
          "parseEventFrame reads them back");
  }

  // Every one of them travels inside a relay envelope in one frame.
  check(sizeof(V4PayloadPowerStatus) <= ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT &&
        sizeof(V4PayloadEventPush) <= ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT, "the battery and the event frame fit one routed frame");
  check(sizeof(V4PayloadTimeSync) <= ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT &&
        sizeof(V4PayloadSensorStatus) <= ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT &&
        sizeof(V4PayloadSensorReq) <= ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT &&
        sizeof(V4PayloadSensorReading) + SENSOR_READING_PART_MAX_ROUTED <= ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT,
        "each frame fits one routed frame");
  if (failures != 0) {
    std::fprintf(stderr, "%d of %d wire layout checks failed\n", failures, checks);
    return 1;
  }
  std::printf("sensor and time wire layouts: %d checks passed\n", checks);
  return 0;
}
"""


def strip_comments(source: str) -> str:
    source = re.sub(r"/\*.*?\*/", lambda match: "\n" * match.group(0).count("\n"), source, flags=re.S)
    return re.sub(r"//[^\n]*", "", source)


def definition(source: str, marker: str) -> str:
    """The block of a definition; a forward declaration of the same name is skipped."""
    return extract_block(source[source.index(marker + " {"):], marker)


def depth_in(source: str, needle: str, macro: str) -> int:
    """How many `#if <macro>` blocks enclose the one line that holds `needle`."""
    require(source.count(needle) == 1, f"one line with {needle!r}")
    stack = []
    for line in source.splitlines():
        text = line.strip()
        if re.match(r"#\s*if", text):
            stack.append(text)
        elif re.match(r"#\s*endif", text) and stack:
            stack.pop()
        if needle in line:
            return sum(macro in entry for entry in stack)
    return -1


def rule_failures(component: Path) -> list[str]:
    failures = []

    def expect(ok: bool, message: str) -> None:
        if not ok:
            failures.append(message)

    files = {path.name: path.read_text() for path in
             sorted(list(component.glob("*.cpp")) + list(component.glob("*.h")))}
    code = {name: strip_comments(text) for name, text in files.items()}

    # One numbering.
    header = code["System_ESPNow.h"]
    for sensor in SENSORS:
        line = re.search(rf"^#define CAP_SENSOR_{sensor}\s+(.*)$", header, re.M)
        expect(line is not None and line.group(1).strip() == f"espnowsensor::sensorBit(REMOTE_SENSOR_{sensor})",
               f"System_ESPNow.h: CAP_SENSOR_{sensor} is espnowsensor::sensorBit(REMOTE_SENSOR_{sensor}), with no number")
    expect(len(re.findall(r"^#define CAP_SENSOR_\w+", header, re.M)) == len(SENSORS),
           "System_ESPNow.h: a CAP_SENSOR_* this test does not know")
    expect("enum RemoteSensorType" in code["System_ESPNow_SensorCore.h"] and
           "enum RemoteSensorType" not in code["System_ESPNow_Sensors.h"],
           "the sensor types have one home, System_ESPNow_SensorCore.h")
    for name, text in code.items():
        for gone in GONE:
            expect(re.search(rf"\b{gone}\b", text) is None, f"{name}: {gone} is gone")

    remote = files["OLED_Mode_Remote.cpp"]
    defs = extract_block(remote, "static const BondSensorDef bondSensorDefs[]")
    expect("0x" not in defs and defs.count("CAP_SENSOR_") >= 7,
           "OLED_Mode_Remote.cpp: bondSensorDefs takes its masks from CAP_SENSOR_*")
    page = files["WebPage_Bond.cpp"]
    expect(re.search(r"\bm(?:ask)?\s*:\s*0x[0-9A-Fa-f]+", page) is None,
           "WebPage_Bond.cpp: the page types no sensor mask number")
    expect(page.count("= data.sensorBits || {};") == 3 and "espnowsensor::kSensorTable[i].name" in page,
           "WebPage_Bond.cpp: the page takes the sensor bits from the device (sensorBits, from the one table)")

    sensors = files["System_ESPNow_Sensors.cpp"]
    status_cmd = strip_comments(definition(sensors, "const char* cmd_espnow_sensorstatus(const String& argsInput)"))
    for name in ("thermal", "tof", "imu", "gps", "fmradio", "microphone"):
        expect(f'"{name}"' not in status_cmd, f'espnowsensorstatus has no sensor name list of its own ("{name}")')
    expect("sensorTypeToString((RemoteSensorType)i)" in status_cmd, "espnowsensorstatus names the sensors from the table")
    expect(re.search(r'"(?:thermal|tof|imu|gps|input|fmradio|camera|microphone|rtc|presence|apds)"',
                     code["System_ESPNow_Sensors.cpp"]) is None,
           "System_ESPNow_Sensors.cpp spells no sensor name itself")

    # The receive table and the relay lists.
    espnow = files["System_ESPNow.cpp"]
    table = strip_comments(extract_block(espnow, "static const V4OpcodeEntry kV4HandlerTable[]"))
    rows = {name: {flag.strip() for flag in flags.split("|")}
            for name, flags in re.findall(r"\{\s*ESPNOW_V4_TYPE_(\w+)\s*,([^,]*),\s*\w+\s*\}", table)}
    paired_session = {"V4_OPC_FLAG_REQ_PAIRED", "V4_OPC_FLAG_REQ_SESSION_ENC"}
    for name in ("SENSOR_STATUS", "SENSOR_REQ", "SENSOR_ENVELOPE"):
        expect(rows.get(name) == paired_session,
               f"the {name} row asks for a paired sender and a session-encrypted frame, nothing else: {rows.get(name)}")
    expect(rows.get("BOND_SENSOR_DATA") == paired_session | {"V4_OPC_FLAG_REQ_BOND_PEER"},
           "the BOND_SENSOR_DATA row keeps its bond gate")
    flood = strip_comments(extract_block(espnow, "static inline bool v4_flood_type_allowed(uint8_t type)"))
    routed = strip_comments(definition(espnow, "static bool v4_routed_type_allowed(uint8_t type)"))
    for name in ("SENSOR_STATUS", "SENSOR_REQ", "SENSOR_ENVELOPE"):
        expect(f"case ESPNOW_V4_TYPE_{name}:" in routed, f"{name} may travel inside a relay envelope")
        expect(name not in flood, f"{name} is not flooded")
    expect("BOND_SENSOR_DATA" not in routed, "the bond reading stays one hop (the bond frames are not routed)")

    # The handlers hand the frame to the one ingest.
    for handler, call in (("static void v4h_sensor_envelope(const V4RxCtx& ctx)", "/*fromBond=*/false"),
                          ("static void v4h_sensor_data(const V4RxCtx& ctx)", "/*fromBond=*/true")):
        body = extract_block(espnow, handler)
        expect("remoteSensorIngestPart(ctx.recv_info->src_addr, ctx.deviceName, ctx.payload, ctx.payloadLen," in body and
               call in body, f"{handler.split('(')[0].split()[-1]} hands the frame to remoteSensorIngestPart")
    expect("remoteSensorIngestStatus(ctx.recv_info->src_addr, ctx.deviceName, ctx.payload, ctx.payloadLen);" in
           extract_block(espnow, "static void v4h_sensor_status(const V4RxCtx& ctx)"),
           "v4h_sensor_status hands the frame to remoteSensorIngestStatus")
    status_tx = strip_comments(extract_block(espnow, "bool v4_send_sensor_status(const uint8_t* dstMac, uint32_t presentMask, uint32_t streamingMask,"))
    expect("espnowtx::sendAead(dstMac, ESPNOW_V4_TYPE_SENSOR_STATUS, 0," in status_tx and "v4_broadcast" not in status_tx,
           "the status frame is a session unicast, not a broadcast")
    part_tx = strip_comments(extract_block(espnow, "bool v4_send_sensor_part(const uint8_t* dstMac, bool viaBond, const uint8_t* part, uint16_t len)"))
    expect("espnowtx::sendAead(dstMac, ESPNOW_V4_TYPE_SENSOR_ENVELOPE, 0," in part_tx and
           "bondSendEncryptedAsync(dstMac, ESPNOW_V4_TYPE_BOND_SENSOR_DATA, 0," in part_tx,
           "a part goes out sealed: SENSOR_ENVELOPE over the session, or BOND_SENSOR_DATA to the bond partner")

    # Sending readings to a mesh master needs no bond: its two task-loop steps
    # are compiled without bonded mode as well.
    for needle in ("  sensorLinkTick(now);", "  if (gEspNow->meshSensorReqPending) {"):
        expect(depth_in(espnow, needle, "ENABLE_BONDED_MODE") == 0,
               f"System_ESPNow.cpp: {needle.strip()!r} is outside every ENABLE_BONDED_MODE block")

    # The table has one owner; every other file reads copies.
    for name, text in code.items():
        if name == "System_ESPNow_Sensors.cpp":
            continue
        expect(re.search(r"\b(?:gRemoteSensorCache|gSensorPeers|SensorPeerRecord|RemoteSensorData)\b", text) is None,
               f"{name}: reads the receiver's table through remoteSensorInfoAt / remoteSensorCopyText only")
    expect("static SensorPeerRecord* gSensorPeers = nullptr;" in sensors and "  RemoteSensorData* slots;" in sensors and
           "gRemoteSensorCache" not in code["System_ESPNow_Sensors.cpp"],
           "the table is private to its file: one record per paired sender, each with its own slots")

    # S14: who has a table. Not ESP-NOW start: the first reading or status
    # frame a device keeps, after the role rule.
    init = strip_comments(definition(sensors, "bool initRemoteSensorSystem()"))
    expect("remoteSensorTableEnsure" not in init and "ps_alloc" not in init,
           "ESP-NOW start allocates nothing for readings received from other devices")
    ingest = strip_comments(extract_block(sensors, "void remoteSensorIngestPart(const uint8_t* senderMac, const char* senderName,"))
    expect("espnowsensor::roleReceives(gSettings.meshRole)" in ingest and "remoteSensorTableEnsure()" in ingest and
           ingest.index("espnowsensor::roleReceives(gSettings.meshRole)") < ingest.index("remoteSensorTableEnsure()"),
           "the ingest makes the table only after the role rule")
    expect(ingest.index("readingTextIsObject") < ingest.index("sensorPeerSlotsEnsure(peer)"),
           "a sender's slots are made by a reading that is whole and well formed")
    status_rx = strip_comments(extract_block(sensors, "void remoteSensorIngestStatus(const uint8_t* senderMac, const char* senderName,"))
    expect("espnowsensor::roleReceives(gSettings.meshRole)" in status_rx and "remoteSensorTableEnsure()" in status_rx and
           status_rx.index("espnowsensor::roleReceives(gSettings.meshRole)") < status_rx.index("remoteSensorTableEnsure()"),
           "the status frame may make the table, only after the role rule")
    expect("statusNotKept" not in sensors, "no status frame is refused for want of a table any more")
    for field in ("valid", "lastUpdate", "jsonLength", "arrivalEpochMs", "counter", "aged"):
        expect(re.search(rf"entry->{field}\b", status_rx) is None,
               f"the status frame never writes a reading's {field}")
    expect("counterJudge" not in status_rx and "->track" not in status_rx and "->counts" not in status_rx,
           "the status frame never touches a reading's counter state or counts")
    # A sensor listed by its sender's status frame before any reading: the
    # master's line says it streams and has no reading yet, not that its
    # reading is 30 s old.
    summary_at = strip_comments(definition(sensors, "static bool remoteSensorPeerSummaryAt(int index, RemoteSensorPeerSummary* out)"))
    status_cmd = strip_comments(definition(sensors, "const char* cmd_espnow_sensorstatus(const String& argsInput)"))
    expect("if (e.valid) out->readMask |= (uint16_t)(1u << k);" in summary_at and
           re.search(r'\(summary\.readMask & \(1u << k\)\) \? "no reading for 30 s" : "streaming, no reading yet"',
                     status_cmd) is not None,
           "a streaming sensor with no reading kept is listed as 'streaming, no reading yet'")
    core = files["System_ESPNow_SensorCore.h"]
    expect("constexpr bool roleReceives(uint8_t role) { return role == kRoleMaster || role == kRoleBackup; }" in core and
           "constexpr bool roleSends(uint8_t role) { return role != kRoleMaster; }" in core,
           "S15: a master and a backup master keep what they are sent; every role but the master sends")
    expect(re.search(r"meshRole\s*[!=]=\s*MESH_ROLE_MASTER", strip_comments(definition(
        sensors, "const char* cmd_espnow_sensorstatus(const String& argsInput)"))) is None,
           "espnowsensorstatus asks the role rules, not 'is it the master'")

    # S21: the stream command says where the readings go.
    stream_cmd = strip_comments(definition(sensors, "const char* cmd_espnow_sensorstream(const String& argsInput)"))
    expect("sensorStreamOnReply(sensorType, target, sessionUp, targetName, reply, sizeof(reply));" in stream_cmd and
           "sensorReadingTarget((int)sensorType)" in stream_cmd and
           '"Error: this device is the mesh master. It receives readings and does not send its own."' in stream_cmd,
           "espnowsensorstream answers with the target of that sensor, or why nothing is sent")
    expect("Master devices receive sensor data, they don't stream it" not in sensors and
           "Sensor streaming started" not in sensors, "the old stream command texts are gone")
    expect("gSensorByHand[sensorType] = true;" in strip_comments(definition(sensors, "void startSensorDataStreaming(RemoteSensorType sensorType)")),
           "a stream switched on by hand is marked so")
    task_code = strip_comments(extract_block(sensors, "static void sensorBroadcasterTask(void* param)"))
    expect("espnowsensor::streamSurvivesLeaseEnd(gSensorByHand[i])" in task_code and
           "sensorControllerClearIfNoLeaseLocked(oldController)" in task_code,
           "a lease that lapses does not stop a stream switched on by hand, and the controller goes with its last lease")
    for name in ("OLED_Mode_Network.cpp", "G2_Page_ESPNow.cpp"):
        expect("uint8_t" in code[name] and re.search(r"\[REMOTE_SENSOR_SLOTS_MAX\]", code[name]) is not None,
               f"{name}: its slot index arrays are sized by REMOTE_SENSOR_SLOTS_MAX")
    expect("static_assert(REMOTE_SENSOR_MAX <= 16" in files["System_ESPNow.h"],
           "System_ESPNow.h: the 16-bit bond status masks are guarded")

    # The sender: a reading is cut by the frame budget of its target and never
    # parked; the broadcaster prints nothing.
    send = strip_comments(extract_block(sensors, "bool sensorSendReadingTo(const uint8_t dst[6], bool viaBond, RemoteSensorType sensorType,"))
    expect("espnowsensor::planParts(len, v4_single_frame_cap(dst), plan)" in send,
           "the sender plans its parts by the frame budget of the target")
    expect("espnowHandshakeAutoStart" not in send and "BROADCAST_PRINTF" not in send,
           "the sender starts no handshake and prints nothing itself")
    task = strip_comments(extract_block(sensors, "static void sensorBroadcasterTask(void* param)"))
    expect("BROADCAST_PRINTF" not in task and "broadcastOutput" not in task,
           "the broadcaster task prints nothing (its stack has no room for a console line)")
    tick = strip_comments(extract_block(sensors, "void sensorLinkTick(uint32_t nowMs)"))
    expect("espnowHandshakeAutoStart(mac)" in tick and "warnLimiterTake(" in tick,
           "sensorLinkTick starts the handshake and limits the console line with WarnLimiter")

    # The masters named on this device: both commands take 'clear', check a
    # value by the rule of the core, and the web page sends an emptied field
    # as 'clear' (an empty argument only shows the value).
    fp_cmd = strip_comments(extract_block(espnow, "static const char* espnowFingerprintCommand(const String& a, const char* jsonKey, bool backup)"))
    expect("espnowsensor::fingerprintArgKind(value.c_str(), value.length())" in fp_cmd and
           "espnowsensor::kFingerprintBadReply" in fp_cmd and
           "setSetting(*((String*)entry->valuePtr), String(), entry);" in fp_cmd and
           "espnowsensor::fingerprintClearedReply(backup," in fp_cmd,
           "espnowmasterfingerprint / espnowbackupfingerprint: show, clear, a checked value, or refused")
    for name, key, backup in (("cmd_espnow_masterfingerprint", "masterFingerprint", "false"),
                              ("cmd_espnow_backupfingerprint", "backupMasterFingerprint", "true")):
        body = strip_comments(extract_block(espnow, f"const char* {name}(const String& a)"))
        expect(f'espnowFingerprintCommand(a, "{key}", {backup})' in body, f"{name} goes through espnowFingerprintCommand")
    expect(re.search(r"ESPNOW_SETTING_CMD\(cmd_espnow_(?:master|backup)fingerprint", espnow) is None,
           "the fingerprint commands are not the generic setting command")
    for name in ("espnowmasterfingerprint", "espnowbackupfingerprint"):
        row = re.search(rf'\{{ "{name}", "[^"]*", true, cmd_espnow_\w+, "Usage: {name} \[<64-hex-pubkey>\|clear\]', espnow)
        expect(row is not None, f"{name} is admin and its usage names 'clear'")
    page = files["WebPage_Settings.h"]
    expect("var clearWhenEmpty = { espnowmasterfingerprint: true, espnowbackupfingerprint: true };" in page and
           "if (clearWhenEmpty[cmd] && String(v).trim() === '') v = 'clear';" in page,
           "the web settings page sends an emptied fingerprint field as 'clear'")
    return failures


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")
    component = args.component.resolve()
    wire = (component / "System_ESPNow_Wire.h").read_text()
    inserted = "\n".join(define(wire, name) for name in WIRE_DEFINES) + "\n" + "\n".join(
        extract_block(wire, f"struct __attribute__((packed)) {name}") + ";" for name in STRUCTS)
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", f"-I{component}"]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-g"]
    failures = []
    with tempfile.TemporaryDirectory(prefix="hw1-espnow-sensor-wire-") as directory:
        source = Path(directory) / "sensor_wire.cpp"
        binary = Path(directory) / "sensor_wire"
        source.write_text(HARNESS.replace("// INSERT_WIRE", inserted))
        if subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], timeout=180).returncode != 0:
            failures.append("the wire layout harness does not compile")
        elif subprocess.run([str(binary)], timeout=60).returncode != 0:
            failures.append("the wire layout harness failed")
    failures += rule_failures(component)
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        print(f"espnow_sensor_wire: {len(failures)} failure(s)", file=sys.stderr)
        sys.exit(1)
    print("sensor and time frames: layouts as stated, one sensor numbering, paired + session gates", flush=True)


if __name__ == "__main__":
    main()
