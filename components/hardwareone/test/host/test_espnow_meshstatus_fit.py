#!/usr/bin/env python3
"""espnowmeshstatus always fits one command result (ESP-NOW 1.0 step 8 review).

Extracts fillMeshStatusPeerJsonObject, meshStatusKeepFit, buildMeshStatusPeersJson
and cmd_espnow_meshstatus from System_ESPNow.cpp and runs them with the vendored
ArduinoJson against doubles of the peer tables (the production
System_ESPNow_LinkCore.h and System_ESPNow_PeerStateCore.h are included).

Cases: the worst case - 16 peer slots of unpaired mesh members plus 16 paired
devices not in a slot, every name 31 characters, the largest numbers, and 16
discovered (unpaired) devices with long names - gives a reply under
CMD_RESULT_MAX that parses, says how many peers and discovered devices it left
out (peersNotShown / unpairedNotShown, which add up with the rows shown), and
shows paired peers before discovered ones; never "response too large". A
small mesh (3 peers) is shown whole with no not-shown keys. Each row carries
state, link and lastTwoWayAgeMs, and no lastAck / ackCount / activityAlive /
lastHeartbeat / lastRxActivity.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from test_web_batch_handlers import extract_block  # noqa: E402

HARNESS = r'''
#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "System_MacText.h"
#include "System_ESPNow_LinkCore.h"

void webBatchTestStringAppend() {}

static constexpr size_t CMD_RESULT_MAX = 4096;
static constexpr size_t GLOBAL_DEBUG_BUFFER_SIZE = 4096;
static char gDebug[GLOBAL_DEBUG_BUFFER_SIZE];
static bool ensureDebugBuffer() { return true; }
static char* getDebugBuffer() { return gDebug; }
#define RETURN_VALID_IF_VALIDATE_CSTR() do {} while (0)
#define PSRAM_JSON_DOC(name) JsonDocument name
static uint32_t gNow = 5000000;
static uint32_t millis() { return gNow; }

struct NameText {
  std::string s;
  size_t length() const { return s.size(); }
  const char* c_str() const { return s.c_str(); }
};
struct EspNowDevice { uint8_t mac[6]; NameText name; };
struct UnpairedDevice { uint8_t mac[6]; NameText name; int rssi; uint32_t lastSeenMs; uint32_t heartbeatCount; };
struct EspNowState {
  bool initialized = true;
  int deviceCount = 0;
  EspNowDevice devices[16];
  int unpairedDeviceCount = 0;
  UnpairedDevice unpairedDevices[16];
};
static EspNowState gState;
static EspNowState* gEspNow = &gState;
struct MeshPeerHealth {
  uint8_t mac[6];
  bool isActive;
  uint32_t lastMeshHeartbeatMs;
  uint32_t lastRxActivityMs;
  uint32_t heartbeatCount;
  int16_t linkRssiEwma;
  MeshLinkState link;
};
static MeshPeerHealth gSlots[16];
static MeshPeerHealth* gMeshPeers = gSlots;
static int gMeshPeerSlots = 16;
static const uint8_t kSelf[6] = {0x02, 0xEE, 0, 0, 0, 0};
static bool isSelfMac(const uint8_t* mac) { return std::memcmp(mac, kSelf, 6) == 0; }
static bool meshEnabled() { return true; }
static bool isPairedDevice(const uint8_t* mac) {
  for (int i = 0; i < gState.deviceCount; i++) if (std::memcmp(gState.devices[i].mac, mac, 6) == 0) return true;
  return false;
}
static MeshPeerHealth* getMeshPeerHealth(const uint8_t* mac, bool) {
  for (auto& s : gSlots) if (s.isActive && std::memcmp(s.mac, mac, 6) == 0) return &s;
  return nullptr;
}
static std::string gLongName(31, 'N');
static String getEspNowDeviceName(const uint8_t*) { return String(gLongName.c_str()); }
static MeshLink meshPeerLinkOf(const MeshPeerHealth* p) {
  return p ? meshLinkOf(p->link, p->lastRxActivityMs, gNow) : MeshLink::None;
}
static EspNowPeerState espnowPeerState(const uint8_t* mac) {
  return espnowPeerStateOf(meshPeerLinkOf(getMeshPeerHealth(mac, false)), isPairedDevice(mac));
}

// INSERT_PRODUCTION

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { ++fails; std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); } } while (0)

static void macOf(uint8_t out[6], uint8_t a, uint8_t b) {
  const uint8_t m[6] = {0x02, a, 0x33, 0x44, 0x55, b};
  std::memcpy(out, m, 6);
}

static void worstCase() {
  gState = EspNowState();
  for (int i = 0; i < 16; i++) {
    MeshPeerHealth& s = gSlots[i];
    std::memset(&s, 0, sizeof s);
    macOf(s.mac, 0xA0, (uint8_t)i);   // unpaired mesh members (relays)
    s.isActive = true;
    s.lastMeshHeartbeatMs = s.lastRxActivityMs = gNow - 1;
    s.heartbeatCount = 4294967295u;
    s.linkRssiEwma = -127 * 4;
    s.link.lastTwoWayMs = gNow - 29999;
    s.link.peerHeardRssi = -127;
  }
  gState.deviceCount = 16;
  for (int i = 0; i < 16; i++) {
    macOf(gState.devices[i].mac, 0xB0, (uint8_t)i);  // paired, not in a slot (the slots are full)
    gState.devices[i].name.s = std::string(31, 'P');
  }
  gState.unpairedDeviceCount = 16;
  for (int i = 0; i < 16; i++) {
    macOf(gState.unpairedDevices[i].mac, 0xC0, (uint8_t)i);
    gState.unpairedDevices[i].name.s = std::string(31, 'U');
    gState.unpairedDevices[i].rssi = -127;
    gState.unpairedDevices[i].lastSeenMs = 1;
    gState.unpairedDevices[i].heartbeatCount = 4294967295u;
  }
  const char* reply = cmd_espnow_meshstatus(String(""));
  const size_t len = std::strlen(reply);
  JsonDocument doc;
  const bool parsed = !deserializeJson(doc, reply);
  CHECK(len < CMD_RESULT_MAX && parsed && doc["ok"].isNull(), "worst case: %zu bytes, parsed %d: %.80s", len,
        (int)parsed, reply);
  const size_t shown = doc["peers"].size();
  const int notShown = doc["peersNotShown"] | 0;
  CHECK(doc["totalPeers"] == 32 && shown + (size_t)notShown == 32 && shown >= 8 && notShown > 0,
        "worst case: %zu peers shown, %d not shown, totalPeers %d", shown, notShown, doc["totalPeers"].as<int>());
  const size_t ushown = doc["unpaired"].size();
  const int unotShown = doc["unpairedNotShown"] | 0;
  CHECK(doc["totalUnpaired"].as<size_t>() == 16 && ushown + (size_t)unotShown == 16,
        "worst case: %zu discovered shown, %d not shown", ushown, unotShown);
  JsonObject row = doc["peers"][0];
  CHECK(row["state"].is<const char*>() && row["link"].is<const char*>() && !row["lastTwoWayAgeMs"].isNull() &&
            row["lastAck"].isNull() && row["ackCount"].isNull() && row["activityAlive"].isNull() &&
            row["lastHeartbeat"].isNull() && row["lastRxActivity"].isNull(),
        "row keys");
  std::printf("worst case: %zu bytes, %zu of 32 peers and %zu of 16 discovered shown\n", len, shown, ushown);
}

static void smallMesh() {
  gState = EspNowState();
  for (auto& s : gSlots) std::memset(&s, 0, sizeof s);
  gState.deviceCount = 3;
  for (int i = 0; i < 3; i++) {
    macOf(gState.devices[i].mac, 0xB0, (uint8_t)i);
    gState.devices[i].name.s = "peer";
    MeshPeerHealth& s = gSlots[i];
    std::memcpy(s.mac, gState.devices[i].mac, 6);
    s.isActive = true;
    s.lastRxActivityMs = gNow - 100;
  }
  const char* reply = cmd_espnow_meshstatus(String(""));
  JsonDocument doc;
  CHECK(!deserializeJson(doc, reply) && doc["peers"].size() == 3 && doc["peersNotShown"].isNull() &&
            doc["unpairedNotShown"].isNull() && doc["peers"][0]["state"] == "connecting" &&
            doc["peers"][0]["link"] == "oneWay" && doc["peers"][0]["lastTwoWayAgeMs"].isNull(),
        "small mesh shown whole: %s", reply);
}

int main() {
  worstCase();
  smallMesh();
  if (fails) { std::printf("espnow_meshstatus_fit: %d failure(s)\n", fails); return 1; }
  std::printf("espnow_meshstatus_fit: ok\n");
  return 0;
}
'''


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--cxx', default=os.environ.get('CXX') or shutil.which('c++'))
    p.add_argument('--sanitize', action='store_true')
    p.add_argument('--component', type=Path, default=HERE.parents[1])
    a = p.parse_args()
    if not a.cxx:
        p.error('a host C++17 compiler is required')
    source = (a.component / 'System_ESPNow.cpp').read_text()
    pieces = []
    for marker in ('static void fillMeshStatusPeerJsonObject(', 'static bool meshStatusKeepFit(',
                   'void buildMeshStatusPeersJson(JsonDocument& doc,', 'const char* cmd_espnow_meshstatus('):
        if source.count(marker) != 1:
            sys.exit(f'FAIL extraction: {marker!r} found {source.count(marker)} times')
        pieces.append(extract_block(source, marker))
    unit_text = HARNESS.replace('// INSERT_PRODUCTION', '\n\n'.join(pieces))
    json_include = a.component.parent / 'hardwareone_libs' / 'ArduinoJson' / 'src'
    with tempfile.TemporaryDirectory(prefix='hw1-meshstatus-fit-') as tmp:
        unit, exe = Path(tmp) / 'fit.cpp', Path(tmp) / 'fit'
        unit.write_text(unit_text)
        for name in ('System_MacText.h', 'System_ESPNow_LinkCore.h', 'System_ESPNow_PeerStateCore.h'):
            shutil.copy(a.component / name, Path(tmp) / name)
        cmd = [a.cxx, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-Wno-unused-parameter',
               '-DARDUINOJSON_ENABLE_ARDUINO_STRING=1', '-DARDUINOJSON_ENABLE_ARDUINO_STREAM=0',
               '-DARDUINOJSON_ENABLE_ARDUINO_PRINT=0', '-DARDUINOJSON_ENABLE_PROGMEM=0',
               '-I', tmp, '-I', str(a.component / 'test/host/web_batch_stubs'),
               '-isystem', str(json_include), str(unit), '-o', str(exe)]
        if a.sanitize:
            cmd[1:1] = ['-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                        '-fno-omit-frame-pointer', '-g']
        subprocess.run(cmd, check=True, timeout=240)
        rc = subprocess.run([str(exe)], timeout=60).returncode
    if rc:
        print('FAIL espnow_meshstatus_fit')
        sys.exit(1)
    print('PASS espnow_meshstatus_fit')


if __name__ == '__main__':
    main()
