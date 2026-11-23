// System_ESPNow_PeerStateCore.h - the three states of a mesh peer
//
// One meaning on every surface that shows or counts ESP-NOW peers (owner
// decision 2026-11-19, built for the OLED and the automation PEERS value on
// 2026-11-21; sharpened by ESP-NOW 1.0 step 8, RC3):
//   online     - heard within 30 s, the link proven both ways (MeshLink::TwoWay,
//                System_ESPNow_LinkCore.h) AND an ACTIVE encrypted session:
//                commands to the peer work;
//   connecting - heard within 30 s, but not proven both ways yet (one-way: it
//                may not hear this device) or no session yet (a few seconds
//                after a restart, or while a session is rebuilt);
//   offline    - not heard within 30 s (a session that outlived the peer does
//                not make a silent peer online).
// The peers counted are the paired ones: gEspNow->devices[] without this
// device's own entry, the set the lens "up/paired" count already walks.
//
// The rule is pure (no radio, no tables) so host suites can run it as is. The
// accessors that feed it from the link table and the session table,
// espnowPeerState(mac) and espnowPeerCounts(), sit in System_ESPNow.h.
#pragma once

#include <cstdint>

enum class EspNowPeerState : uint8_t { Offline, Connecting, Online };

// A mesh peer's link (DESIGN 4.3): not heard within 30 s, heard only, or
// proven both ways.
enum class MeshLink : uint8_t { None, OneWay, TwoWay };

constexpr EspNowPeerState espnowPeerStateOf(MeshLink link, bool sessionUp) {
  return link == MeshLink::None                    ? EspNowPeerState::Offline
         : (link == MeshLink::TwoWay && sessionUp) ? EspNowPeerState::Online
                                                   : EspNowPeerState::Connecting;
}

// The link's word for status output and JSON (`link`).
constexpr const char* meshLinkName(MeshLink link) {
  return link == MeshLink::TwoWay   ? "twoWay"
         : link == MeshLink::OneWay ? "oneWay"
                                    : "none";
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
