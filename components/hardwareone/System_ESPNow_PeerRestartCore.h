// System_ESPNow_PeerRestartCore.h - when a peer restarted, the session with it ends
//
// Sessions live only in RAM. A peer that restarts has none, while this device
// still holds the session it had with it and keeps sealing frames into it,
// which the peer can no longer open. Bench 2026-11-18: the P4 restarted, the
// XIAO sent three messages "via SESSION_FRAME" and all three were lost; the
// XIAO noticed only after two send timeouts (~10 s), then lost another 6 s to
// a SESSION_OPEN glare.
//
// Two independent signals end such a session at once:
//   1. The boot counter in every mesh heartbeat (ESP-NOW 1.0 step 8; it
//      replaced the BOOT frame 31). A device that holds no ACTIVE session with
//      a peer sends its heartbeats group-tagged, so they arrive without a
//      session; a counter newer than the last one seen from that peer closes
//      the session this device holds with it. A device that still holds a live
//      session with the peer (ESP-NOW was closed and opened again: sessions
//      stay in RAM) seals its heartbeats in it; a sealed heartbeat proves the
//      session works, so it closes nothing and only records the counter.
//   2. The SESSION_UNKNOWN hint: a device that receives a sealed frame for a
//      session it does not have answers once (group-signed, rate-limited) with
//      the session id and frame sequence it could not open. The sender closes
//      that session if it is the live one and the sequence is one it sealed,
//      and opens a new one at once.
// Either way nothing is accepted or sent on the strength of the signal: the
// only effect is one fresh SESSION_OPEN/CONFIRM, whose signatures are checked
// as always.
//
// Both signals are only group-signed (any member of the peer's mesh could
// forge one; a captured heartbeat can be re-sent by anyone in range, the dedup
// ring forgets it after 5 s), so:
//   - only a group tag of the peer's own mesh counts (peerRestartSignalScoped):
//     a member of another mesh this device is in cannot end the session;
//   - a forced close happens at most once per peer per
//     kPeerRestartDropMinIntervalMs, shared by both signals;
//   - a heartbeat with the counter already seen, an older one, or 0 closes
//     nothing and is not recorded: re-sent copies of old heartbeats (two from
//     different starts played in turn included) can never close a session
//     again, and the counter a heartbeat records only ever moves forward;
//   - a session younger than kPeerRestartFreshSessionMs is kept: a restarted
//     device heartbeats before it opens a session, so a session that young
//     was opened after the restart;
//   - a hint closes only the live session it names, and only when the
//     sequence is one this device sealed in it (1..last sent).
//
// Boot counters are compared wrap-safe: newer means 0 < (notice - known) as a
// signed 32-bit difference. A counter seen in a sealed frame of the live
// session (a sealed mesh heartbeat, the bond heartbeat) cannot be a copy, so
// it becomes the known counter outright, lower ones included. A peer whose
// count was erased (it starts at 1 again) is therefore not locked out: its
// tagged heartbeats close nothing until a sealed one shows the new count or
// this device restarts, and meanwhile the SESSION_UNKNOWN hint ends each stale
// session on the first message sent into it. A counter of 0 (the peer could
// not read its count) closes nothing either (the hint covers that peer). The
// table lives in RAM only: after this device restarts every peer starts
// unknown, and its first heartbeat with a counter counts as a restart for the
// session rule (a copy of an old one can thus close a session once per peer
// per start of this device). A restart is reported only from sealed counters
// (peerRestartBootSeen): the first sealed counter after this device's start
// reports nothing, a later sealed one that moved forward reports once.
//
// Pure logic on a small fixed table (no heap, no lock, no clock); the owner
// (System_ESPNow_Sessions.cpp) holds the instance in PSRAM and the lock.
// Times are millis() values; every compare is a wrap-safe difference.
#pragma once

#include <stdint.h>
#include <string.h>

// One entry per paired device (MESH_PEER_MAX = 16).
constexpr uint8_t  kPeerRestartSlots = 16;
// A forced close (boot counter or hint) per peer at most this often. A forged
// signal costs at most one fresh handshake per peer per 10 s.
constexpr uint32_t kPeerRestartDropMinIntervalMs = 10000;
// A session this young when a newer counter arrives was opened after the restart.
constexpr uint32_t kPeerRestartFreshSessionMs = 2000;
// A device answers sealed frames for a session it does not have with at most
// one hint per peer this often (a burst of frames gets one).
constexpr uint32_t kSessionUnknownHintMinIntervalMs = 1000;

struct PeerRestartEntry {
  bool     inUse;
  bool     haveBoot;      // `boot` holds the last counter seen from this peer
  uint8_t  mac[6];
  uint32_t boot;
  bool     haveConfirmed; // `confirmedBoot` holds the last counter seen SEALED
  uint32_t confirmedBoot; // (only the peer can send it: restart reports go by it)
  uint32_t lastDropMs;    // 0 = no forced close yet
  uint32_t lastHintTxMs;  // 0 = no hint sent yet
  uint32_t lastUseMs;
};

struct PeerRestartTable {
  PeerRestartEntry e[kPeerRestartSlots];
};

// Wrap-safe: true when `notice` comes after `known`.
inline bool bootCounterNewer(uint32_t notice, uint32_t known) {
  return (int32_t)(notice - known) > 0;
}

inline PeerRestartEntry* peerRestartFind(PeerRestartTable& t, const uint8_t mac[6]) {
  for (auto& e : t.e) {
    if (e.inUse && memcmp(e.mac, mac, 6) == 0) return &e;
  }
  return nullptr;
}

// The entry for `mac`, made when missing. A full table gives up the entry used
// longest ago (that peer just starts unknown again).
inline PeerRestartEntry* peerRestartClaim(PeerRestartTable& t, const uint8_t mac[6], uint32_t nowMs) {
  PeerRestartEntry* e = peerRestartFind(t, mac);
  if (!e) {
    for (auto& c : t.e) {
      if (!c.inUse) { e = &c; break; }
    }
    if (!e) {
      e = &t.e[0];
      for (auto& c : t.e) {
        if ((uint32_t)(nowMs - c.lastUseMs) > (uint32_t)(nowMs - e->lastUseMs)) e = &c;
      }
    }
    memset(e, 0, sizeof(*e));
    memcpy(e->mac, mac, 6);
    e->inUse = true;
  }
  e->lastUseMs = nowMs;
  return e;
}

inline bool peerRestartDropAllowed(const PeerRestartEntry& e, uint32_t nowMs) {
  return e.lastDropMs == 0 || (uint32_t)(nowMs - e.lastDropMs) >= kPeerRestartDropMinIntervalMs;
}

inline void peerRestartNoteDrop(PeerRestartEntry& e, uint32_t nowMs) {
  e.lastDropMs = nowMs ? nowMs : 1;
}

// A counter seen in a sealed frame of the live session (a sealed mesh
// heartbeat, the bond heartbeat): only the peer can send it and it cannot be
// replayed, so it becomes the reference outright, also when lower (the peer's
// count was erased). It never closes anything. True when it moved the last
// SEALED counter forward: the peer restarted since this device last saw its
// count in a session. Only this reports a restart (the "Online (boot #N)"
// record): a group-tagged counter can be sent by any member of the mesh, so
// reporting on it would let one file a record per forged counter.
inline bool peerRestartBootSeen(PeerRestartTable& t, const uint8_t mac[6], uint32_t boot, uint32_t nowMs) {
  if (boot == 0) return false;
  PeerRestartEntry* e = peerRestartClaim(t, mac, nowMs);
  const bool restarted = e->haveConfirmed && bootCounterNewer(boot, e->confirmedBoot);
  e->boot = boot;
  e->haveBoot = true;
  e->confirmedBoot = boot;
  e->haveConfirmed = true;
  return restarted;
}

// A heartbeat or hint that is only group-signed counts when its tag is of the
// peer's own mesh (the header fingerprint the tag was checked under equals the
// fingerprint of the mesh the peer is paired in). A sealed one came over the
// peer's own session.
inline bool peerRestartSignalScoped(bool sessionSealed, uint16_t frameFingerprint, uint16_t peerFingerprint) {
  return sessionSealed || (frameFingerprint != 0 && frameFingerprint == peerFingerprint);
}

enum class PeerBootVerdict : uint8_t {
  DropSession  = 0,  // close the session held with the peer now
  NoSession    = 1,  // a restart, but no live session with the peer
  FreshSession = 2,  // a restart, but the session was opened after it
  RateLimited  = 3,  // a restart, but a forced close happened less than 10 s ago
  SameBoot     = 4,  // the counter already seen: this start was handled
  OlderBoot    = 5,  // the counter went back (a copy, or an erased peer): nothing closed or recorded
  NoCounter    = 6,  // no usable counter (0): nothing closed or recorded
};

// The boot counter of a group-tagged heartbeat from `mac` (tag of the peer's
// mesh checked, the peer is paired). `sessionLive` says whether this device
// holds an ACTIVE (or REKEYING) session with the peer, opened or rekeyed
// `sessionAgeMs` ago. Never reports a restart (peerRestartBootSeen does, on
// the sealed counter that follows).
inline PeerBootVerdict peerRestartOnBootCounter(PeerRestartTable& t, const uint8_t mac[6], uint32_t boot,
                                                bool sessionLive, uint32_t sessionAgeMs, uint32_t nowMs) {
  if (boot == 0) return PeerBootVerdict::NoCounter;
  PeerRestartEntry* e = peerRestartClaim(t, mac, nowMs);
  if (e->haveBoot) {
    if (boot == e->boot) return PeerBootVerdict::SameBoot;
    if (!bootCounterNewer(boot, e->boot)) return PeerBootVerdict::OlderBoot;
  }
  e->boot = boot;
  e->haveBoot = true;
  if (!sessionLive) return PeerBootVerdict::NoSession;
  if (sessionAgeMs < kPeerRestartFreshSessionMs) return PeerBootVerdict::FreshSession;
  if (!peerRestartDropAllowed(*e, nowMs)) return PeerBootVerdict::RateLimited;
  peerRestartNoteDrop(*e, nowMs);
  return PeerBootVerdict::DropSession;
}

enum class UnknownSessionReply : uint8_t {
  SendHint    = 0,  // answer with a SESSION_UNKNOWN hint
  KnownId     = 1,  // the id is this device's handshake in flight or a session it replaced: no hint
  RateLimited = 2,  // a hint went to this peer less than 1 s ago
};

// A sealed frame from `mac` (a peer with a stored identity) for a session this
// device does not hold ACTIVE. `idKnown` is true when the id is the one of this
// device's slot for the peer (a SESSION_OPEN of ours awaiting its CONFIRM: the
// peer's frames can overtake the CONFIRM) or one the slot held before (a late
// frame of a replaced session).
inline UnknownSessionReply peerRestartOnUnknownSession(PeerRestartTable& t, const uint8_t mac[6],
                                                       bool idKnown, uint32_t nowMs) {
  if (idKnown) return UnknownSessionReply::KnownId;
  PeerRestartEntry* e = peerRestartClaim(t, mac, nowMs);
  if (e->lastHintTxMs != 0 && (uint32_t)(nowMs - e->lastHintTxMs) < kSessionUnknownHintMinIntervalMs) {
    return UnknownSessionReply::RateLimited;
  }
  e->lastHintTxMs = nowMs ? nowMs : 1;
  return UnknownSessionReply::SendHint;
}

enum class SessionHintVerdict : uint8_t {
  DropSession  = 0,  // close the named session and open a new one now
  NoSession    = 1,  // no live session with the peer: nothing to close
  OtherSession = 2,  // the hint names another session (old, replaced, forged)
  UnsentSeq    = 3,  // this device never sealed that sequence in the session
  RateLimited  = 4,  // a forced close happened less than 10 s ago
};

// A SESSION_UNKNOWN hint from `mac` naming (hintSessionId, hintFrameSeq).
// `liveSessionId` is the id of this device's ACTIVE (or REKEYING) session with
// the peer and `lastSentSeq` the last sequence sealed in it (txSeqNext);
// sessionLive false when there is none.
inline SessionHintVerdict peerRestartOnHint(PeerRestartTable& t, const uint8_t mac[6],
                                            bool sessionLive, uint16_t liveSessionId,
                                            uint32_t lastSentSeq,
                                            uint16_t hintSessionId, uint32_t hintFrameSeq,
                                            uint32_t nowMs) {
  if (!sessionLive) return SessionHintVerdict::NoSession;
  if (hintSessionId == 0 || hintSessionId != liveSessionId) return SessionHintVerdict::OtherSession;
  if (hintFrameSeq == 0 || hintFrameSeq > lastSentSeq) return SessionHintVerdict::UnsentSeq;
  PeerRestartEntry* e = peerRestartClaim(t, mac, nowMs);
  if (!peerRestartDropAllowed(*e, nowMs)) return SessionHintVerdict::RateLimited;
  peerRestartNoteDrop(*e, nowMs);
  return SessionHintVerdict::DropSession;
}
