// System_ESPNow_LinkCore.h - two-way liveness of a mesh peer (ESP-NOW 1.0 step 8, RC3)
//
// "Heard" is not "reachable": a board that hears a peer whose radio cannot hear
// it back used to count the peer as online and sent into the void. 1.0 keeps
// two clocks per peer (DESIGN 4.1):
//   - lastHeardMs: any sealed or group-tagged frame received DIRECTLY from it
//     (not relayed); one-way evidence;
//   - two-way evidence: (a) a SEALED ACK from it, received direct, for one of
//     our frames that went out direct (it heard us, we heard it); or (b) a
//     SEALED heartbeat from it, received direct, whose echoMsgId is one of the
//     last two heartbeat msgIds we sent it within 30 s.
// Tagged ACKs and tagged heartbeats are one-way only: any holder of the group
// key can forge origin = B. Sealed frames sit behind the session's replay
// window, so a replay cannot fake two-way.
//
// meshLinkOf: None (not heard in 30 s), TwoWay (two-way evidence within 30 s;
// entering needs two distinct evidences, staying needs one), else OneWay.
// Each evidence id (our msgId the ACK or echo names) counts once.
//
// Limits (threat model):
//   - Wormhole: an attacker without any key who records B's sealed frames
//     near B and replays them near A (and A's near B) in real time can make a
//     link that is one-way on the air look two-way: the frames are genuine
//     and inside the replay window. It cannot read, change or forge them, so
//     the worst case is a direct send that is lost (an unanswered send then
//     counts toward the session re-open as before).
//   - A replayed group-tagged frame of a paired peer refreshes its heard
//     clock (tags carry no freshness beyond the 5 s dedup), so someone in
//     range can keep a powered-off peer "connecting" (never online: that
//     needs fresh sealed evidence). Counting only sealed frames as heard once
//     a session existed would close this; not done in 1.0 (a restarted peer
//     sends tagged heartbeats until its session reopens).
//
// Also here (pure, host-tested): the heartbeat echo memory, the record of
// direct sends an ACK can prove, the heartbeat back-off (ceiling 10 s, E8) and
// the online/offline event rate limit (one per peer per 30 s, the latest state
// posted when the window ends).
//
// No heap, no lock, no clock: the owner (System_ESPNow.cpp) keeps one
// MeshLinkState per MeshPeerHealth slot (PSRAM) and calls these on espnow_task;
// readers on other tasks only call the const meshLinkOf. Times are millis()
// values; every compare is a wrap-safe difference.
#pragma once

#include <stdint.h>
#include <string.h>

#include "System_ESPNow_PeerStateCore.h"  // MeshLink

constexpr uint32_t kMeshLinkWindowMs       = 30000;  // heard / two-way window (MESH_PEER_TIMEOUT_MS)
constexpr uint8_t  kMeshLinkEvidenceSlots  = 3;      // twoWayHits is capped at 3
constexpr uint8_t  kMeshLinkDirectTxSlots  = 8;      // our recent direct ACK_REQ sends to the peer
constexpr uint32_t kMeshHeartbeatCeilingMs = 10000;  // back-off ceiling toward a silent peer (E8)
constexpr uint32_t kMeshEventMinIntervalMs = 30000;  // online/offline events per peer

inline bool meshLinkWithin(uint32_t thenMs, uint32_t nowMs, uint32_t windowMs) {
  return thenMs != 0 && (uint32_t)(nowMs - thenMs) < windowMs;
}

// The heard clock itself is MeshPeerHealth::lastRxActivityMs (its readers
// predate this file); it is passed in.
struct MeshLinkState {
  uint32_t lastTwoWayMs;                        // 0 = never
  uint32_t evidenceId[kMeshLinkEvidenceSlots];  // our msgIds the evidences named
  uint32_t evidenceMs[kMeshLinkEvidenceSlots];  // 0 = empty
  bool     wasTwoWay;                           // settled state, for the hysteresis
  bool     expired;                             // went silent: nothing here counts until heard again
  // Heartbeat echo (DESIGN 4.2): the msgIds of the last two heartbeats we sent
  // the peer, and the last heartbeat we heard from it (echoed back to it).
  uint32_t hbTxId[2];
  uint32_t hbTxMs[2];
  uint8_t  hbTxNext;
  int8_t   hbRxRssi;                            // our RSSI of its last heartbeat (0 = none)
  int8_t   peerHeardRssi;                       // its RSSI of our heartbeats (its linkRssi; 0 = none)
  uint8_t  directTxNext;
  uint32_t hbRxId;
  uint32_t hbRxMs;
  // Our recent ACK_REQ frames to the peer that went out direct (not wrapped
  // in RELAY_DATA): only their ACKs prove the direct link both ways.
  uint32_t directTxId[kMeshLinkDirectTxSlots];
};

// Distinct two-way evidences within the window (0..3).
inline uint8_t meshLinkHits(const MeshLinkState& s, uint32_t nowMs) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < kMeshLinkEvidenceSlots; i++) {
    if (meshLinkWithin(s.evidenceMs[i], nowMs, kMeshLinkWindowMs)) n++;
  }
  return n;
}

// The one definition (DESIGN 4.3).
inline MeshLink meshLinkOf(const MeshLinkState& s, uint32_t lastHeardMs, uint32_t nowMs) {
  if (s.expired || !meshLinkWithin(lastHeardMs, nowMs, kMeshLinkWindowMs)) return MeshLink::None;
  if (meshLinkWithin(s.lastTwoWayMs, nowMs, kMeshLinkWindowMs)) {
    const uint8_t hits = meshLinkHits(s, nowMs);
    if (s.wasTwoWay ? hits >= 1 : hits >= 2) return MeshLink::TwoWay;
  }
  return MeshLink::OneWay;
}

// The peer went silent (heard -> None, seen on the owner's tick): its link
// memory is cleared and the old stamps no longer count. A paired peer's slot
// is kept for good, and every compare here is a wrap-safe difference, so
// without this a stamp from about 49.7 days ago would read as "just now" when
// millis() comes round again. meshLinkHeard ends it.
inline void meshLinkExpire(MeshLinkState& s) {
  memset(&s, 0, sizeof(s));
  s.expired = true;
}

// The peer was heard (the owner sets its heard clock at the same time).
inline void meshLinkHeard(MeshLinkState& s) { s.expired = false; }

// The owner settles the hysteresis after each change and on its tick.
inline MeshLink meshLinkSettle(MeshLinkState& s, uint32_t lastHeardMs, uint32_t nowMs) {
  const MeshLink link = meshLinkOf(s, lastHeardMs, nowMs);
  s.wasTwoWay = (link == MeshLink::TwoWay);
  return link;
}

// One two-way evidence naming our msgId `id` (the caller has recorded the
// frame as heard first). A repeat of an id already counted (the ACK of another
// fragment of the same message, a copy) refreshes nothing; id 0 is never
// evidence. True when it counted.
inline bool meshLinkNoteTwoWay(MeshLinkState& s, uint32_t id, uint32_t lastHeardMs, uint32_t nowMs) {
  if (id == 0) return false;
  const uint32_t t = nowMs ? nowMs : 1;
  for (uint8_t i = 0; i < kMeshLinkEvidenceSlots; i++) {
    if (s.evidenceId[i] == id && s.evidenceMs[i] != 0) return false;  // counted once
  }
  // An empty slot, else the oldest evidence.
  uint8_t slot = 0;
  for (uint8_t i = 0; i < kMeshLinkEvidenceSlots; i++) {
    if (s.evidenceMs[i] == 0) { slot = i; break; }
    if ((uint32_t)(t - s.evidenceMs[i]) > (uint32_t)(t - s.evidenceMs[slot])) slot = i;
  }
  s.evidenceId[slot] = id;
  s.evidenceMs[slot] = t;
  s.lastTwoWayMs = t;
  meshLinkSettle(s, lastHeardMs, t);
  return true;
}

// ---- Heartbeat echo --------------------------------------------------------

// We are about to send heartbeat `id` to the peer.
inline void meshLinkNoteHeartbeatSent(MeshLinkState& s, uint32_t id, uint32_t nowMs) {
  const uint8_t i = (uint8_t)(s.hbTxNext & 1);
  s.hbTxId[i] = id;
  s.hbTxMs[i] = nowMs ? nowMs : 1;
  s.hbTxNext = (uint8_t)(i ^ 1);
}

// A sealed heartbeat from the peer echoes `echo`: true when it names one of
// the last two heartbeats we sent it within 30 s.
inline bool meshLinkEchoMatches(const MeshLinkState& s, uint32_t echo, uint32_t nowMs) {
  if (echo == 0) return false;
  for (uint8_t i = 0; i < 2; i++) {
    if (s.hbTxId[i] == echo && meshLinkWithin(s.hbTxMs[i], nowMs, kMeshLinkWindowMs)) return true;
  }
  return false;
}

// We heard heartbeat `id` from the peer at `rssi` (our radio), and it reports
// hearing ours at `itsLinkRssi` (0 = it heard none).
inline void meshLinkNoteHeartbeatHeard(MeshLinkState& s, uint32_t id, int8_t rssi, int8_t itsLinkRssi,
                                       uint32_t nowMs) {
  s.hbRxId = id;
  s.hbRxMs = nowMs ? nowMs : 1;
  s.hbRxRssi = rssi;
  s.peerHeardRssi = itsLinkRssi;
}

// The echo our next heartbeat to the peer carries: the msgId of its last
// heartbeat we heard within 30 s, else 0.
inline uint32_t meshLinkEchoFor(const MeshLinkState& s, uint32_t nowMs) {
  return meshLinkWithin(s.hbRxMs, nowMs, kMeshLinkWindowMs) ? s.hbRxId : 0;
}

// The linkRssi our next heartbeat carries (0 = heard none within 30 s).
inline int8_t meshLinkRssiFor(const MeshLinkState& s, uint32_t nowMs) {
  return meshLinkWithin(s.hbRxMs, nowMs, kMeshLinkWindowMs) ? s.hbRxRssi : (int8_t)0;
}

// ---- Direct sends an ACK can prove -----------------------------------------

inline void meshLinkNoteDirectSend(MeshLinkState& s, uint32_t msgId) {
  if (msgId == 0) return;
  const uint8_t prev = (uint8_t)((s.directTxNext + kMeshLinkDirectTxSlots - 1) % kMeshLinkDirectTxSlots);
  if (s.directTxId[prev] == msgId) return;   // fragments of one message: one entry
  s.directTxId[s.directTxNext % kMeshLinkDirectTxSlots] = msgId;
  s.directTxNext = (uint8_t)((s.directTxNext + 1) % kMeshLinkDirectTxSlots);
}

inline bool meshLinkSentDirect(const MeshLinkState& s, uint32_t msgId) {
  if (msgId == 0) return false;
  for (uint8_t i = 0; i < kMeshLinkDirectTxSlots; i++) {
    if (s.directTxId[i] == msgId) return true;
  }
  return false;
}

// ---- Heartbeat back-off (DESIGN 4.4, E8) -----------------------------------

// Minimum ms between heartbeats to a peer we last heard `silentMs` ago:
// 0 (every 5 s tick) below 30 s, then the 10 s ceiling. A board that does not
// hear us still sends at least every 10 s, so one lost heartbeat cannot push
// our view of it past the 30 s window (and a hunting worker's 30 s silence
// clock never runs out on a master that only cannot hear it: CF bench B8).
inline uint32_t meshHeartbeatMinInterval(uint32_t silentMs) {
  return silentMs < kMeshLinkWindowMs ? 0 : kMeshHeartbeatCeilingMs;
}

// ---- Online/offline events (DESIGN 4.4) ------------------------------------

enum class MeshPeerEvent : uint8_t { None = 0, Online = 1, Offline = 2 };

struct MeshEventLatch {
  uint8_t  posted;   // MeshPeerEvent last posted (None: nothing yet)
  uint32_t lastMs;   // when it was posted (0 = never)
};

// One step per tick with the peer's current state. Online is posted when the
// peer becomes online, Offline when it stops being online (it went offline, or
// is only connecting: one-way or no session). An Offline is never posted for a
// peer that was never posted Online. At most one event per peer per 30 s; a
// change inside the window waits and the state at the window's end is posted
// (nothing when it went back to what was posted).
inline MeshPeerEvent meshEventStep(MeshEventLatch& l, bool online, uint32_t nowMs) {
  MeshPeerEvent want = online ? MeshPeerEvent::Online
                              : (l.posted == (uint8_t)MeshPeerEvent::Online ? MeshPeerEvent::Offline
                                                                             : MeshPeerEvent::None);
  if (want == MeshPeerEvent::None || (uint8_t)want == l.posted) return MeshPeerEvent::None;
  if (l.lastMs != 0 && (uint32_t)(nowMs - l.lastMs) < kMeshEventMinIntervalMs) return MeshPeerEvent::None;
  l.posted = (uint8_t)want;
  l.lastMs = nowMs ? nowMs : 1;
  return want;
}
