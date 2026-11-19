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
           "V4PayloadPowerStatus", "V4PayloadEventPush", "V4PayloadForwardLink")

HARNESS = r"""
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "System_ESPNow_ForwardCore.h"
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
  check(ESPNOW_V4_BROADCAST_AUTH_TAG_LEN == 16 && ESPNOW_V4_MAX_PLAINTEXT == 202 &&
        ESPNOW_V4_RELAY_MAX_INNER_FRAME == 194 && ESPNOW_V4_RELAY_MAX_INNER_PAYLOAD == 162 &&
        ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT == 146, "202 bytes direct, 146 through a relay (16-byte group tag)");
  check(SENSOR_READING_HEADER_LEN == 12 && SENSOR_READING_MAX_PARTS == 4 && SENSOR_READING_PART_MAX_DIRECT == 190 &&
        SENSOR_READING_PART_MAX_ROUTED == 134 && SENSOR_READING_MAX_BYTES == 536, "the reading's sizes");
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
        offsetof(V4PayloadSensorReading, parts) == 6 && offsetof(V4PayloadSensorReading, fwdLink) == 7 &&
        offsetof(V4PayloadSensorReading, ageMs) == 8, "its offsets: 0 type, 1 flags, 2 boot tag, 4 counter, 6 parts, 7 link number, 8 age");
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
    // A forwarded reading: flag bit 2 and the link number in byte 7, nothing
    // else moves (the 12-byte header and the 536-byte reading are unchanged).
    wire.flags = (uint8_t)(wire.flags | espnowsensor::kReadingFlagForwarded);
    wire.fwdLink = 0x5C;
    h.flags = (uint8_t)(h.flags | espnowsensor::kReadingFlagForwarded);
    h.fwdLink = 0x5C;
    espnowsensor::packReadingHeader(packed, h);
    check(std::memcmp(packed, &wire, sizeof(wire)) == 0 && packed[1] == 0x07 && packed[7] == 0x5C,
          "a forwarded reading's header: bit 2 of the flags, the link number in byte 7");
  }

  // The status frame: 16 bytes, two masks in the one numbering, the link
  // byte of a forwarded one where a reserved byte was (no growth).
  check(sizeof(V4PayloadSensorStatus) == 16 && offsetof(V4PayloadSensorStatus, presentMask) == 0 &&
        offsetof(V4PayloadSensorStatus, streamingMask) == 4 && offsetof(V4PayloadSensorStatus, bootTag) == 8 &&
        offsetof(V4PayloadSensorStatus, fwdLink) == 10 && offsetof(V4PayloadSensorStatus, reserved) == 11 &&
        sizeof(((V4PayloadSensorStatus*)nullptr)->reserved) == 5,
        "V4PayloadSensorStatus: 0 present, 4 streaming, 8 boot tag, 10 link byte, 11 reserved[5]");
  {
    V4PayloadSensorStatus wire{};
    wire.presentMask = 0x0429;
    wire.streamingMask = 0x0009;
    wire.bootTag = 0xBEEF;
    wire.fwdLink = 7;
    espnowfwd::StatusFrame f{0x0429, 0x0009, 0xBEEF, 7};
    uint8_t packed[16];
    espnowfwd::packStatusFrame(packed, f);
    check(std::memcmp(packed, &wire, sizeof(wire)) == 0, "espnowfwd::packStatusFrame writes the struct's layout");
  }

  // The request: 16 bytes, the link byte of a request for a bond partner
  // where a reserved byte was (no growth).
  check(sizeof(V4PayloadSensorReq) == 16 && offsetof(V4PayloadSensorReq, reqId) == 0 &&
        offsetof(V4PayloadSensorReq, mode) == 1 && offsetof(V4PayloadSensorReq, sensorMask) == 2 &&
        offsetof(V4PayloadSensorReq, intervalMs) == 6 && offsetof(V4PayloadSensorReq, leaseMs) == 10 &&
        offsetof(V4PayloadSensorReq, fwdLink) == 14 && offsetof(V4PayloadSensorReq, reserved) == 15,
        "V4PayloadSensorReq: 0 id, 1 mode, 2 mask, 6 interval, 10 lease, 14 link byte, 15 reserved");

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
        offsetof(V4PayloadPowerStatus, fwdLink) == 16 && offsetof(V4PayloadPowerStatus, reserved) == 17 &&
        sizeof(((V4PayloadPowerStatus*)nullptr)->reserved) == 7,
        "V4PayloadPowerStatus: 0 counter, 2 boot tag, 4 status, 5 backend, 6 known, 8 mV, 10 percent x10, 12 rate x10, 14 sample age, 16 link number, 17 reserved[7]");
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
    wire.fwdLink = 0x21;
    f.fwdLink = 0x21;
    espnowsensor::packPowerFrame(packed, f);
    check(std::memcmp(packed, &wire, sizeof(wire)) == 0 && packed[16] == 0x21,
          "a forwarded battery frame: the link number in byte 16, known untouched");
  }

  // The event frame: 128 bytes.
  check(sizeof(V4PayloadEventPush) == 128 && offsetof(V4PayloadEventPush, counter) == 0 &&
        offsetof(V4PayloadEventPush, bootTag) == 2 && offsetof(V4PayloadEventPush, flags) == 4 &&
        offsetof(V4PayloadEventPush, tier) == 5 && offsetof(V4PayloadEventPush, source) == 6 &&
        offsetof(V4PayloadEventPush, fwdLink) == 7 && offsetof(V4PayloadEventPush, skipped) == 8 &&
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
    wire.flags = (uint8_t)(wire.flags | espnowsensor::kEventFlagForwarded);
    wire.fwdLink = 0x33;
    f.flags = (uint8_t)(f.flags | espnowsensor::kEventFlagForwarded);
    f.fwdLink = 0x33;
    espnowsensor::packEventFrame(packed, f);
    check(std::memcmp(packed, &wire, sizeof(wire)) == 0 && packed[4] == 0x12 && packed[7] == 0x33,
          "a forwarded event: bit 4 of the flags, the link number in byte 7");
  }

  // The forward-link frame: 96 bytes.
  check(sizeof(V4PayloadForwardLink) == 96 && offsetof(V4PayloadForwardLink, op) == 0 &&
        offsetof(V4PayloadForwardLink, linkGen) == 1 && offsetof(V4PayloadForwardLink, viaBootTag) == 2 &&
        offsetof(V4PayloadForwardLink, streams) == 4 && offsetof(V4PayloadForwardLink, result) == 5 &&
        offsetof(V4PayloadForwardLink, answers) == 6 && offsetof(V4PayloadForwardLink, reserved0) == 7 &&
        offsetof(V4PayloadForwardLink, partnerMac) == 8 && offsetof(V4PayloadForwardLink, reserved1) == 14 &&
        offsetof(V4PayloadForwardLink, sensorMask) == 16 && sizeof(((V4PayloadForwardLink*)nullptr)->sensorMask) == 4 &&
        offsetof(V4PayloadForwardLink, partnerKey) == 20 && offsetof(V4PayloadForwardLink, partnerName) == 52 &&
        offsetof(V4PayloadForwardLink, reserved2) == 84 && sizeof(((V4PayloadForwardLink*)nullptr)->reserved2) == 12,
        "V4PayloadForwardLink: 0 op, 1 link number, 2 boot tag, 4 streams, 5 result, 6 answers, 8 partner, "
        "14 reserved, 16 sensors (32 bits), 20 key[32], 52 name[32], 84 reserved[12]");
  {
    V4PayloadForwardLink wire{};
    wire.op = espnowfwd::kOpAnnounce;
    wire.linkGen = 9;
    wire.viaBootTag = 0xA1B2;
    wire.streams = espnowfwd::kStreamsAll;
    const uint8_t partner[6] = { 0x02, 0xA0, 0, 0, 0, 0x01 };
    std::memcpy(wire.partnerMac, partner, 6);
    wire.sensorMask = espnowfwd::kSensorMaskDefault;
    std::memset(wire.partnerKey, 0x4B, sizeof(wire.partnerKey));
    std::memcpy(wire.partnerName, "porch", 5);
    espnowfwd::LinkFrame f{};
    f.op = espnowfwd::kOpAnnounce;
    f.linkGen = 9;
    f.viaBootTag = 0xA1B2;
    f.streams = espnowfwd::kStreamsAll;
    std::memcpy(f.partnerMac, partner, 6);
    f.sensorMask = espnowfwd::kSensorMaskDefault;
    std::memset(f.partnerKey, 0x4B, sizeof(f.partnerKey));
    std::memcpy(f.partnerName, "porch", 5);
    uint8_t packed[96];
    std::memset(packed, 0xEE, sizeof(packed));
    espnowfwd::packLinkFrame(packed, f);
    check(std::memcmp(packed, &wire, sizeof(wire)) == 0, "packLinkFrame writes the bytes the struct states, reserved 0");
    espnowfwd::LinkFrame back{};
    check(espnowfwd::parseLinkFrame((const uint8_t*)&wire, sizeof(wire), back) && back.linkGen == 9 &&
          back.viaBootTag == 0xA1B2 && back.sensorMask == espnowfwd::kSensorMaskDefault, "parseLinkFrame reads them back");
  }

  // Every one of them travels inside a relay envelope in one frame.
  check(sizeof(V4PayloadPowerStatus) <= ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT &&
        sizeof(V4PayloadEventPush) <= ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT &&
        sizeof(V4PayloadForwardLink) <= ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT,
        "the battery, the event and the forward-link frame fit one routed frame");
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
            for name, flags in re.findall(r"\{\s*ESPNOW_V4_TYPE_(\w+)\s*,([^,]*),\s*ESPNOW_V4_MINLEN_\w+\s*,\s*\w+\s*\}", table)}
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

    # Forwarding for a bond partner (System_ESPNow_ForwardCore.h): FORWARD_LINK
    # is a paired + session mesh frame, outside every bond block, routed and not
    # flooded; the bond worker's battery and events are bond frames, one hop,
    # dropped (not parked) without a session.
    expect(rows.get("FORWARD_LINK") == paired_session,
           f"the FORWARD_LINK row asks for a paired sender and a session-encrypted frame, nothing else: {rows.get('FORWARD_LINK')}")
    expect(depth_in(espnow, "  { ESPNOW_V4_TYPE_FORWARD_LINK,", "ENABLE_BONDED_MODE") == 0,
           "the FORWARD_LINK row is outside every ENABLE_BONDED_MODE block (a master built without bond mode takes it)")
    for name in ("BOND_POWER_STATUS", "BOND_EVENT_PUSH"):
        expect(rows.get(name) == paired_session | {"V4_OPC_FLAG_REQ_BOND_PEER"},
               f"the {name} row is gated to the bond partner: {rows.get(name)}")
        expect(name not in routed and name not in flood, f"{name} stays one hop")
    expect("case ESPNOW_V4_TYPE_FORWARD_LINK:" in routed and "FORWARD_LINK" not in flood,
           "FORWARD_LINK may travel inside a relay envelope and is not flooded")
    periodic = strip_comments(extract_block(espnow, "static bool bondFrameIsPeriodic(uint8_t type)"))
    expect("ESPNOW_V4_TYPE_BOND_POWER_STATUS" in periodic and "ESPNOW_V4_TYPE_BOND_EVENT_PUSH" in periodic,
           "a bond battery or event frame without a session is dropped, not parked (bondFrameIsPeriodic)")
    wire_text = files["System_ESPNow_Wire.h"]
    expect("ESPNOW_V4_TYPE_FORWARD_LINK    = 155," in wire_text and "ESPNOW_V4_TYPE_BOND_POWER_STATUS = 180," in wire_text and
           "ESPNOW_V4_TYPE_BOND_EVENT_PUSH = 182," in wire_text, "the opcodes 155, 180 and 182 have their numbers")
    expect("FORWARD_LINK=155" in wire_text and "BOND_POWER_STATUS=180" in wire_text and "BOND_EVENT_PUSH=182" in wire_text
           and "reverses" in wire_text, "the opcode map names 155, 180, 182 and says the earlier decision was reversed")
    expect(depth_in(espnow, "  espnowForwardRxTick(now);", "ENABLE_BONDED_MODE") == 0 and
           depth_in(espnow, "  bondForwardTick(now);", "ENABLE_BONDED_MODE") == 1 and
           espnow.index("  espnowPowerEventsTick(now);") < espnow.index("  espnowForwardRxTick(now);") <
           espnow.index("  bondForwardTick(now);"),
           "the mesh master's forward step runs in every build, the bond master's in a bond build, after the battery step")
    # One rule keeps a bond worker's data off the mesh.
    power = strip_comments(files["System_ESPNow_PowerEvents.cpp"])
    expect("espnowfwd::meshDataAllowed(peBondWorker())" in strip_comments(extract_block(files["System_ESPNow_PowerEvents.cpp"], "static void pePlan(uint32_t nowMs, PeSendPlan* plan)")),
           "pePlan asks meshDataAllowed: a bond worker sends to its partner only")
    announce = strip_comments(extract_block(sensors, "static void announceSensorStatus(const uint8_t* alsoTo = nullptr)"))
    # Nothing ends the function early for a bond worker: its one early return
    # is the one for "no receiver and no controller" (the worker's receiver is
    # its bond master, target.viaBond).
    before_send = announce[:announce.index("if (haveTarget && target.viaBond)")]
    expect(before_send.count("return") == 1 and "if (!haveTarget && !alsoTo) return;" in before_send,
           "announceSensorStatus returns early only when nobody is to be told")
    expect("if (!espnowfwd::meshDataAllowed(bondWorker)) alsoTo = nullptr;" in announce and
           "target.viaBond == bondWorker" in announce and
           "v4_send_bond_sensor_status(target.mac, presentMask, streamingMask, sensorBootTag())" in announce and
           announce.index("if (haveTarget && target.viaBond)") < announce.index("v4_send_sensor_status(target.mac"),
           "announceSensorStatus: a bond worker tells its bond master only (185), never a mesh master or a controller")
    bond_status = strip_comments(extract_block(espnow, "bool v4_send_bond_sensor_status(const uint8_t* dstMac, uint32_t presentMask, uint32_t streamingMask,"))
    expect("bondSendEncryptedAsync(dstMac, ESPNOW_V4_TYPE_BOND_SENSOR_STATUS" in bond_status,
           "the bond worker's status goes over the bond link")
    expect("ESPNOW_V4_TYPE_BOND_SENSOR_STATUS" in periodic, "a bond status frame without a session is dropped, not parked")
    tick_src = strip_comments(extract_block(sensors, "void sensorLinkTick(uint32_t nowMs)"))
    bond_tick = strip_comments(extract_block(sensors, "static void sensorBondStatusTick(uint32_t nowMs)"))
    expect("sensorBondStatusTick(nowMs);" in tick_src and "espnowSessionIdOf(target.mac)" in bond_tick and
           "announceSensorStatus();" in bond_tick, "a bond worker tells its bond master its sensors once per bond session")
    expect("ESPNOW_V4_TYPE_BOND_SENSOR_STATUS = 185," in wire_text and "BOND_SENSOR_STATUS=185" in wire_text,
           "the opcode 185 has its number and is in the opcode map")
    expect(re.search(r"\{ ESPNOW_V4_TYPE_BOND_SENSOR_STATUS, V4_OPC_FLAG_REQ_PAIRED \| V4_OPC_FLAG_REQ_BOND_PEER \| "
                     r"V4_OPC_FLAG_REQ_SESSION_ENC, ESPNOW_V4_MINLEN_BOND_SENSOR_STATUS, v4hBondSensorStatus \}", espnow) is not None and
           depth_in(espnow, "  { ESPNOW_V4_TYPE_BOND_SENSOR_STATUS,", "ENABLE_BONDED_MODE") == 1,
           "185 is a bond row: only from the bond partner, over its session, in a bond build")
    sensor_req = strip_comments(extract_block(espnow, "static void v4h_sensor_req(const V4RxCtx& ctx)"))
    refusal = re.search(r"\{\s*bool bondWorker = false;\s*#if ENABLE_BONDED_MODE\s*"
                        r"bondWorker = gSettings\.bondModeEnabled && isBondWorker\(\);\s*#endif\s*"
                        r"if \(!espnowfwd::meshDataAllowed\(bondWorker\)\) \{(.*?)\n  \}\n", sensor_req, re.S)
    expect(refusal is not None and "return;" in refusal.group(1) and "gEspNow->sensorReqRefusedBond" in refusal.group(1),
           "v4h_sensor_req first asks meshDataAllowed for a bond worker, counts the refusal and returns: a bond worker "
           "takes no sensor request from a mesh master")
    expect(refusal is not None and sensor_req.index("if (!espnowfwd::meshDataAllowed(bondWorker)) {") <
           sensor_req.index("gSettings.espnowAcceptSensorControl"),
           "the bond worker's refusal comes before anything else is looked at")
    # A request with the link byte: for this device's bond partner, handed to
    # the bond master's side and never applied to this device's own sensors.
    expect(re.search(r"V4PayloadSensorReq rq;\s*ctx\.read\(rq\);\s*if \(rq\.fwdLink != 0\) \{", sensor_req) is not None,
           "v4h_sensor_req takes every request with the link byte set as one for the bond partner")
    fwd_at = sensor_req.index("rq.fwdLink != 0")
    fwd_block = extract_block(sensor_req, "rq.fwdLink != 0)")
    # The block ends with its return, after the #if/#else/#endif: whatever the
    # build, the request never falls through to the own-sensor path.
    expect(re.search(r"#endif\s*return;\s*\}$", fwd_block) is not None,
           "the request for the bond partner returns at the end of its block, in every build")
    expect(sensor_req.index("if (!espnowfwd::meshDataAllowed(bondWorker)) {") < fwd_at <
           sensor_req.index("gSettings.espnowAcceptSensorControl") and
           "bondForwardSensorRequest(ctx.recv_info->src_addr, rq.mode, rq.sensorMask, rq.intervalMs, rq.leaseMs," in fwd_block
           and "meshSensorReqPending" not in fwd_block,
           "a request for the bond partner goes to the bond master's side and returns before the own-sensor path")
    expect("if (wants && !plan.viaBond)" in power, "the battery step starts no handshake with the bond partner")
    # A bond is 1:1: the forward code reads only the single-bond settings.
    for name in ("System_ESPNow_BondForward.cpp", "System_ESPNow_ForwardRx.cpp", "System_ESPNow_PowerEvents.cpp",
                 "System_ESPNow_Sensors.cpp"):
        expect(re.search(r"\bbond(?:ModeEnabled|Role|PeerMac)Mesh\b", code[name]) is None,
               f"{name}: reads only the single-bond settings (bondModeEnabled, bondRole, bondPeerMac)")
    # Every reader outside the store names the record it means: the origin's
    # own (nullptr) or the bond master that forwards for it.
    for name in ("System_MQTT.cpp", "G2_Page_ESPNow.cpp", "OLED_Mode_Network.cpp", "WebPage_Sensors.cpp",
                 "System_ESPNow_Sensors.cpp"):
        for call in re.findall(r"remoteSensorCopyText\(([^;]*)\);", code[name]):
            expect("remoteSensorInfoVia(" in call or name == "System_ESPNow_Sensors.cpp" and "via" in call,
                   f"{name}: remoteSensorCopyText({call.strip()}) passes the via of the slot it listed")
    expect("if (remoteSensorInfoVia(slot) != nullptr) continue;" in code["System_MQTT.cpp"],
           "MQTT publishes a device's own readings only, never what a bond master forwards for it")
    expect("gSelSensorForwarded ? gSelSensorVia : nullptr" in code["G2_Page_ESPNow.cpp"],
           "the G2 detail view keeps the via of the row it opened")
    expect("getRemoteSensorDataJson(mac, via, type)" in code["WebPage_Sensors.cpp"] and
           'httpd_query_key_value(query, "via"' in code["WebPage_Sensors.cpp"],
           "GET /api/sensors/remote takes &via=")

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
    status_rx = strip_comments(extract_block(sensors, "static void sensorStatusIngest(const uint8_t* senderMac, const char* senderName, const uint8_t* payload,"))
    expect("!fromBond && !espnowsensor::roleReceives(gSettings.meshRole)" in status_rx and "remoteSensorTableEnsure()" in status_rx and
           status_rx.index("espnowsensor::roleReceives(gSettings.meshRole)") < status_rx.index("remoteSensorTableEnsure()"),
           "the status frame may make the table, only after the role rule (a bond partner's: whatever the role)")
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
    expect("if (e.valid) out->readMask |= espnowsensor::sensorBit((uint8_t)k);" in summary_at and
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
           '"Error: This device is the mesh master. It receives readings and does not send its own."' in stream_cmd,
           "espnowsensorstream answers with the target of that sensor, or why nothing is sent")
    expect("Master devices receive sensor data, they don't stream it" not in sensors and
           "Sensor streaming started" not in sensors, "the old stream command texts are gone")
    expect("gSensorByHand[sensorType] = true;" in strip_comments(definition(sensors, "bool tryStartSensorDataStreaming(RemoteSensorType sensorType)")),
           "a stream switched on by hand is marked so")
    task_code = strip_comments(extract_block(sensors, "static void sensorBroadcasterTask(void* param)"))
    expect("espnowsensor::streamSurvivesLeaseEnd(gSensorByHand[i])" in task_code and
           "sensorControllerClearIfNoLeaseLocked(oldController)" in task_code,
           "a lease that lapses does not stop a stream switched on by hand, and the controller goes with its last lease")
    for name in ("OLED_Mode_Network.cpp", "G2_Page_ESPNow.cpp"):
        expect("uint8_t" in code[name] and re.search(r"\[REMOTE_SENSOR_SLOTS_MAX\]", code[name]) is not None,
               f"{name}: its slot index arrays are sized by REMOTE_SENSOR_SLOTS_MAX")
    expect("static_assert(REMOTE_SENSOR_MAX <= 30" in files["System_ESPNow_SensorCore.h"] and
           "using SensorMask = uint32_t;" in files["System_ESPNow_SensorCore.h"],
           "System_ESPNow_SensorCore.h: sensor masks are 32 bits (SensorMask), with room for the forward resume bits")

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

    # A master's request leases only the sensors this device has running (bench
    # 2026-11-18): the same list its SENSOR_STATUS sends, before the apply, the
    # kept mask staged back for a retried apply.
    keep = strip_comments(definition(sensors, "uint32_t espnowSensorReqKeepPresent(uint8_t mode, uint32_t sensorMask, "
                                              "const uint8_t* controllerMac)"))
    expect("espnowsensor::sensorReqSplit(mode, sensorMask, sensorPresentMask())" in keep and
           "gSensorTxStats[i].reqLacking++" in keep and "warnLimiterTake(" in keep,
           "espnowSensorReqKeepPresent splits the request by the sensors present, counts and says what it left out")
    expect(keep.count("return ") == 2 and keep.count("return split.apply;") == 2 and
           "if (split.lacking & espnowsensor::sensorBit((uint8_t)i))" in keep,
           "espnowSensorReqKeepPresent returns only the kept sensors (split.apply), on both ways out, and counts "
           "only those left out")
    announce = strip_comments(extract_block(sensors, "static void announceSensorStatus(const uint8_t* alsoTo = nullptr)"))
    expect("sensorPresentMask(&streamingMask)" in announce and "gTofConnected" not in announce,
           "the status frame lists the sensors from the same sensorPresentMask")
    pending = espnow.index("if (gEspNow->meshSensorReqPending) {")
    step = strip_comments(extract_block(espnow[pending:], "if (gEspNow->meshSensorReqPending)"))
    expect("gEspNow->meshSensorReqMask = espnowSensorReqKeepPresent(" in step and
           0 <= step.find("espnowSensorReqKeepPresent(") < step.find("espnowApplySensorSubscription(") and
           "if (gEspNow->meshSensorReqMask || gEspNow->meshSensorReqMode == SENSOR_REQ_UNSUBSCRIBE)" in step,
           "step 9f3 keeps only the present sensors before the apply, and applies nothing when none is left")
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
