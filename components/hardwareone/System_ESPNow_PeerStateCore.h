// System_ESPNow_PeerStateCore.h - the three states of a mesh peer
//
// One meaning on every surface that shows or counts ESP-NOW peers (owner
// decision 2026-11-19, built for the OLED and the automation PEERS value on
// 2026-11-21):
//   online     - a mesh heartbeat within MESH_PEER_TIMEOUT_MS AND an ACTIVE
//                encrypted session: commands to the peer work;
//   connecting - a recent heartbeat, the session not up yet (a few seconds
//                after a restart, or while a session is rebuilt);
//   offline    - no recent heartbeat (a session that outlived the heartbeats
//                does not make a silent peer online).
// The peers counted are the paired ones: gEspNow->devices[] without this
// device's own entry, the set the lens "up/paired" count already walks.
//
// The rule is pure (no radio, no tables) so host suites can run it as is. The
// accessors that feed it from the heartbeat table and the session table,
// espnowPeerState(mac) and espnowPeerCounts(), sit in System_ESPNow.h.
#pragma once

#include <cstdint>

enum class EspNowPeerState : uint8_t { Offline, Connecting, Online };

constexpr EspNowPeerState espnowPeerStateOf(bool heartbeatRecent, bool sessionUp) {
  return !heartbeatRecent ? EspNowPeerState::Offline
         : sessionUp      ? EspNowPeerState::Online
                          : EspNowPeerState::Connecting;
}

// The state's word, as screens and documents print it.
constexpr const char* espnowPeerStateName(EspNowPeerState state) {
  return state == EspNowPeerState::Online       ? "online"
         : state == EspNowPeerState::Connecting ? "connecting"
                                                : "offline";
}

// The paired peers by state. The automation PEERS value is `online`.
struct EspNowPeerCounts {
  int online = 0;
  int connecting = 0;
  int offline = 0;

  constexpr int paired() const { return online + connecting + offline; }
  constexpr void add(EspNowPeerState state) {
    if (state == EspNowPeerState::Online) ++online;
    else if (state == EspNowPeerState::Connecting) ++connecting;
    else ++offline;
  }
};
