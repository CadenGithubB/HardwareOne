#!/usr/bin/env python3
"""The system metric and the MQTT / Home Assistant peer values use the one peer state.

An ESP-NOW peer is online (a mesh heartbeat within 30 s AND an encrypted
session up), connecting (the heartbeat, no session yet) or offline (no recent
heartbeat): System_ESPNow_PeerStateCore.h, read through espnowPeerState /
espnowPeerCounts in System_ESPNow.h (owner decision 2026-11-20, "three states
on every surface"). The OLED and the automation PEERS value moved first
(test_oled_espnow_remote_actions), the lens rows in test_g2_espnow_peer_rows.
This suite covers the rest:

Part one compiles the production accessors (System_ESPNow.h), readMesh and its
Raw reading (System_Metrics.cpp) and meshPeerAvailability (System_MQTT.cpp)
against doubles of the paired registry, the heartbeat table and the session
table:
- the MESH metric is the online peers out of the paired ones; a connecting
  peer, a session without a heartbeat, this device's own registry entry and an
  unpaired peer are never counted online; it is unavailable while ESP-NOW is
  stopped;
- a peer's Home Assistant availability is "online" for an online peer only;
  connecting and offline are both "offline".

Part two reads sources (comments stripped):
- readMesh counts with espnowPeerCounts and asks neither table itself;
- System_MQTT.cpp asks no heartbeat or session table and no MeshPeers health;
  both peer availability publishes print meshPeerAvailability(espnowPeerState
  ...); the state pass publishes availability before it skips a peer without
  a fresh reading (a silent peer used to keep its retained "online"); the
  peer document's system.online is the online state and system.state its name;
- MeshPeers has no health or count queries left (isHealthy, countHealthy,
  countActive), and no source names them.

--component points the suite at another component directory.
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

HERE = Path(__file__).resolve().parent
COMPONENT = HERE.parents[1]

LEXEMES = re.compile(r'''
    //[^\n]*                          # line comment
  | /\*.*?\*/                         # block comment
  | "(?:\\.|[^"\\\n])*"               # string literal
  | '(?:\\.|[^'\\\n])*'               # character literal
''', re.S | re.X)


def code_of(text: str) -> str:
    """The text without comments (string and character literals kept)."""
    return LEXEMES.sub(lambda m: " " if m[0].startswith(("//", "/*")) else m[0], text)


HARNESS = r'''
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>
#include "System_ESPNow_PeerStateCore.h"

#define ENABLE_ESPNOW 1

static unsigned checks = 0;
#define CHECK(condition) do { ++checks; if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #condition); \
  std::exit(1); } } while (false)

// ---- the tables the peer state reads (System_ESPNow.h), by the last address byte ----
struct EspNowDevice { uint8_t mac[6]; };
struct EspNowState { bool initialized = false; int deviceCount = 0; EspNowDevice devices[16] = {}; };
static EspNowState espnow;
static EspNowState* gEspNow = &espnow;
static const uint8_t selfMac[6] = {0x02, 0, 0, 0, 0, 0xEE};
static bool isSelfMac(const uint8_t* mac) { return std::memcmp(mac, selfMac, 6) == 0; }
struct MeshPeerHealth { uint8_t id; bool recent; };
static std::vector<MeshPeerHealth> heartbeats;
static std::vector<uint8_t> sessions;
static MeshPeerHealth* getMeshPeerHealth(const uint8_t mac[6], bool createIfMissing) {
  CHECK(!createIfMissing);
  for (auto& slot : heartbeats) if (slot.id == mac[5]) return &slot;
  return nullptr;
}
static bool isMeshPeerAlive(const MeshPeerHealth* slot) { return slot && slot->recent; }
static bool espnowSessionUp(const uint8_t* mac, uint32_t* = nullptr) {
  for (uint8_t id : sessions) if (mac[5] == id) return true;
  return false;
}
static void paired(std::initializer_list<uint8_t> ids) {
  espnow = EspNowState();
  espnow.initialized = true;
  for (uint8_t id : ids) {
    uint8_t* mac = espnow.devices[espnow.deviceCount++].mac;
    std::memcpy(mac, selfMac, 6);
    mac[5] = id;
  }
}
static void heard(std::initializer_list<uint8_t> ids) {
  heartbeats.clear();
  for (uint8_t id : ids) heartbeats.push_back({id, true});
}
static const uint8_t* macOf(uint8_t id) {
  static uint8_t mac[6];
  std::memcpy(mac, selfMac, 6);
  mac[5] = id;
  return mac;
}

// INSERT_PEER_STATE

namespace {
// INSERT_METRICS
}  // namespace

// INSERT_MQTT

static bool mesh(float online, float paired) {
  const Raw raw = readMesh();
  return raw.available && raw.value == online && raw.rangeLow == 0.0f && raw.rangeHigh == paired;
}

int main() {
  // MESH: online out of paired.
  espnow = EspNowState();
  heard({1});
  sessions = {1};
  CHECK(!readMesh().available);              // ESP-NOW stopped
  paired({0xEE, 1, 2, 3});
  heard({1, 2, 3});
  sessions = {1, 2, 3};
  CHECK(mesh(3, 3));
  sessions = {1};                            // 2 and 3 connecting: not online
  CHECK(mesh(1, 3));
  sessions.clear();
  CHECK(mesh(0, 3));
  heard({1});                                // sessions that outlived the heartbeats
  sessions = {1, 2, 3};
  CHECK(mesh(1, 3));
  heartbeats = {{1, true}, {2, false}, {3, false}};
  CHECK(mesh(1, 3));
  paired({0xEE, 1});                         // 4 and 5 heard with sessions, not paired
  heard({1, 4, 5, 0xEE});
  sessions = {1, 4, 5, 0xEE};
  CHECK(mesh(1, 1));
  paired({0xEE});                            // only this device: 0 of a scale of 1
  CHECK(mesh(0, 1));
  paired({});
  CHECK(mesh(0, 1));

  // Home Assistant availability: online only for an online peer.
  paired({0xEE, 1, 2, 3});
  heard({1, 2});
  sessions = {1, 3};
  CHECK(std::strcmp(meshPeerAvailability(espnowPeerState(macOf(1))), "online") == 0);
  CHECK(std::strcmp(meshPeerAvailability(espnowPeerState(macOf(2))), "offline") == 0);  // connecting
  CHECK(std::strcmp(meshPeerAvailability(espnowPeerState(macOf(3))), "offline") == 0);  // no heartbeat
  CHECK(std::strcmp(meshPeerAvailability(espnowPeerState(macOf(9))), "offline") == 0);  // never heard
  CHECK(std::strcmp(meshPeerAvailability(EspNowPeerState::Online), "online") == 0);
  CHECK(std::strcmp(meshPeerAvailability(EspNowPeerState::Connecting), "offline") == 0);
  CHECK(std::strcmp(meshPeerAvailability(EspNowPeerState::Offline), "offline") == 0);
  // The document's system.state words.
  CHECK(std::strcmp(espnowPeerStateName(espnowPeerState(macOf(2))), "connecting") == 0);
  CHECK(std::strcmp(espnowPeerStateName(espnowPeerState(macOf(3))), "offline") == 0);
  std::printf("%u\n", checks);
  return 0;
}
'''


def unit(component: Path) -> str:
    espnow = (component / "System_ESPNow.h").read_text(encoding="utf-8")
    metrics = (component / "System_Metrics.cpp").read_text(encoding="utf-8")
    mqtt = (component / "System_MQTT.cpp").read_text(encoding="utf-8")
    peer_state = "\n\n".join(extract_block(espnow, marker) for marker in (
        "inline EspNowPeerState espnowPeerState(", "inline EspNowPeerCounts espnowPeerCounts("))
    metric = "\n\n".join([extract_block(metrics, "struct Raw") + ";",
                          re.search(r"(?m)^Raw unavailable\(\)[^\n]*\}$", metrics)[0],
                          extract_block(metrics, "Raw readMesh()")])
    availability = extract_block(mqtt, "static const char* meshPeerAvailability(")
    text = HARNESS
    for marker, body in (("// INSERT_PEER_STATE", peer_state), ("// INSERT_METRICS", metric),
                         ("// INSERT_MQTT", availability)):
        require(text.count(marker) == 1, f"missing/duplicate marker: {marker}")
        text = text.replace(marker, body)
    return text


def definition(source: str, signature: str) -> str:
    """The body of `signature` where it is defined (forward declarations skipped)."""
    return extract_block(source[source.index(signature + " {"):], signature)


def source_failures(component: Path) -> list[str]:
    failures: list[str] = []
    metrics = code_of((component / "System_Metrics.cpp").read_text(encoding="utf-8"))
    mesh = extract_block(metrics, "Raw readMesh()")
    if "espnowPeerCounts()" not in mesh or re.search(r"espnowSessionUp\(|isMeshPeerAlive\(|gMeshPeers", mesh):
        failures.append("System_Metrics.cpp readMesh does not count with espnowPeerCounts()")

    mqtt = code_of((component / "System_MQTT.cpp").read_text(encoding="utf-8"))
    for pattern, why in ((r"\bMeshPeers::", "MeshPeers health"), (r"\bisMeshPeerAlive\(", "the heartbeat table"),
                         (r"\bespnowSessionUp\(", "the session table"),
                         (r'#include "System_MeshPeers\.h"', "System_MeshPeers.h (no MeshPeers call left)")):
        if re.search(pattern, mqtt):
            failures.append(f"System_MQTT.cpp uses {why}; a peer's state is espnowPeerState")
    discovery = definition(mqtt, "static void publishMeshPeerDiscovery()")
    if "meshPeerAvailability(espnowPeerState(peer.mac))" not in discovery:
        failures.append("publishMeshPeerDiscovery: the availability is not meshPeerAvailability(espnowPeerState)")
    data = definition(mqtt, "static void publishMeshPeerSensorData()")
    state = data.find("const EspNowPeerState state = espnowPeerState(peer.mac);")
    publish = data.find("meshPeerAvailability(state)")
    skip = data.find("if (!hasData) continue;")
    if not (0 <= state < publish < skip):
        failures.append("publishMeshPeerSensorData: availability is not published from espnowPeerState "
                        "before a peer without a fresh reading is skipped")
    if data.count("meshPeerAvailability(") != 1 or '"online" : "offline"' in data:
        failures.append("publishMeshPeerSensorData: one availability publish, through meshPeerAvailability")
    for line in ('sys["online"] = state == EspNowPeerState::Online;',
                 'sys["state"] = espnowPeerStateName(state);'):
        if line not in data:
            failures.append(f"publishMeshPeerSensorData: missing `{line}`")

    gone = ("isHealthy", "countHealthy", "countActive")
    for name in ("System_MeshPeers.h", "System_MeshPeers.cpp"):
        code = code_of((component / name).read_text(encoding="utf-8"))
        for function in gone:
            if re.search(r"\b" + function + r"\(", code):
                failures.append(f"{name} still has MeshPeers::{function} (dead since the peer state moved)")
    for path in sorted(component.rglob("*.cpp")) + sorted(component.rglob("*.h")):
        if "test" in path.relative_to(component).parts:
            continue
        code = code_of(path.read_text(encoding="utf-8", errors="replace"))
        for function in gone:
            if f"MeshPeers::{function}" in code:
                failures.append(f"{path.relative_to(component)} calls MeshPeers::{function}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cxx", default=os.environ.get("CXX") or shutil.which("c++"))
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--component", type=Path, default=COMPONENT)
    args = parser.parse_args()
    require(bool(args.cxx), "a host C++17 compiler is required")

    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic", "-Wno-unused-function",
             "-I", str(args.component)]
    if args.sanitize:
        flags += ["-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                  "-fno-omit-frame-pointer", "-g"]
    with tempfile.TemporaryDirectory(prefix="hw1-peer-state-surfaces-") as directory:
        work = Path(directory)
        source, binary = work / "surfaces.cpp", work / "surfaces"
        source.write_text(unit(args.component))
        subprocess.run([args.cxx, *flags, str(source), "-o", str(binary)], check=True, timeout=120)
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        if result.returncode != 0:
            sys.stdout.write(result.stderr)
            print("espnow_peer_state_surfaces: FAILED")
            return 1
        total = int(result.stdout.strip())

    failures = source_failures(args.component)
    for failure in failures:
        print(f"FAIL: {failure}")
    if failures:
        print(f"espnow_peer_state_surfaces: {len(failures)} source failure(s)")
        return 1
    print(f"espnow_peer_state_surfaces: MESH metric and MQTT availability follow the peer state "
          f"({total} checks); source rules hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
