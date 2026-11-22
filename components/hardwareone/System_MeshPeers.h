#ifndef SYSTEM_MESH_PEERS_H
#define SYSTEM_MESH_PEERS_H

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <Arduino.h>
#include "System_ESPNow.h"  // MeshPeerHealth, MeshPeerMeta, primitive helpers

// =============================================================================
// MeshPeers — compound accessors over the mesh peer tables
// =============================================================================
//
// The primitives (getMeshPeerHealth, getMeshPeerMeta, isMeshPeerAlive,
// isMeshPeerRecentlyActive) live in System_ESPNow.h. They expose raw structs
// so callers that need fine-grained field access can still reach them.
//
// This namespace exists for the *compound* queries — the ones currently
// inlined across OLED / MQTT / Web modules:
//   - "what's the human name for this MAC?" (friendlyName > name > "Unknown")
//   - "how many paired peers are in this room?" (loop + filter)
// Whether a peer is online, connecting or offline, and how many are, is not
// asked here: espnowPeerState / espnowPeerCounts (System_ESPNow.h) are the
// one rule every surface uses.
//
// Same rationale as BondedPeer: each UI (OLED status, MQTT publish, web JSON
// builder) was open-coding the same 4-6 line pattern. Consolidating means
// (a) one place to fix bugs, (b) future UIs (G2 menu, automations) get the
// right behavior for free.
//
// Threading: all reads. Underlying tables are written from the ESP-NOW RX
// task without locks — same risk profile as every other reader of gMeshPeers.
// =============================================================================

namespace MeshPeers {

// ----- Compound state queries ---------------------------------------------

// True if the peer is active AND any RX activity (not just heartbeat) is
// recent — useful for "have we heard anything from this MAC lately" checks
// (e.g., relay candidate selection). Not the peer's online state.
bool isReachable(const uint8_t mac[6]);

// True if the peer has an active metadata slot. Independent of health —
// metadata persists even when heartbeats time out, so a peer can be known
// (isKnown=true) while it is offline.
bool isKnown(const uint8_t mac[6]);

// ----- Display / identity -------------------------------------------------

// Best human-readable name for this MAC. Resolution order:
//   1. meta.friendlyName  (the user-set label, "Living Room TV")
//   2. meta.name          (the auto-generated name, "Worker-A1B2")
//   3. paired registry name (gEspNow->devices[].name)
//   4. "Unknown"
// Returns "Unknown" if the MAC isn't known at all. Always returns a
// non-empty string so callers can safely use it in printf-style formatting
// without NULL-checking.
String displayName(const uint8_t mac[6]);

// ----- Aggregations -------------------------------------------------------

// Count of paired peers that are members of `room` for a room command: the
// room was accepted on this device, not merely announced by the peer. Forwards
// to countMeshPeerMetaByRoom. NULL/empty room → 0.
int countByRoom(const char* room);

} // namespace MeshPeers

#endif // ENABLE_ESPNOW
#endif // SYSTEM_MESH_PEERS_H
