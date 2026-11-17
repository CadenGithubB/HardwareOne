// System_ESPNow_HandshakeBackoff.h - back-off for automatic handshake starts
//
// A bond partner that is not there used to cost a key exchange every 5 s, for
// good, with two warnings each time (bench 2026-11-17, step d2). Automatic
// starts (KEY_EX or SESSION_OPEN, kicked by a periodic sender such as the bond
// heartbeat) to a peer that has not answered now wait 5 s, 10 s, 20 s ... up
// to 5 minutes between tries. The wait ends at once when the peer shows it is
// there again (a finished key exchange, a session that came up, an
// authenticated frame after a quiet spell, the mesh peer table seeing it come
// online) or when the user runs a pairing or bond command.
//
// Pure logic on a small fixed table (no heap); the owner (Handlers_Crypto.cpp)
// holds the instance and the lock. Times are millis() values; every compare
// is a wrap-safe difference.
#pragma once

#include <stdint.h>
#include <string.h>

#include "System_Deadline.h"

constexpr uint8_t  kHandshakeBackoffSlots   = 8;
constexpr uint32_t kHandshakeBackoffFirstMs = 5000;     // after the first unanswered start
constexpr uint32_t kHandshakeBackoffMaxMs   = 300000;   // 5 minutes
// Frames from a peer that is backing off end the wait only when the peer had
// been quiet this long: a peer that is heard all the time but never finishes
// the key exchange (another passphrase, a lost identity) must not end it with
// every heartbeat it sends.
constexpr uint32_t kHandshakeBackoffQuietMs = 60000;

struct HandshakeBackoffEntry {
  bool     inUse;
  uint8_t  mac[6];
  uint8_t  starts;        // automatic starts since the peer last answered
  uint32_t nextStartMs;   // deadline (0 = a start is due now)
  uint32_t lastStartMs;
  uint32_t lastHeardMs;   // 0 = not heard since the entry was made
};

struct HandshakeBackoffTable {
  HandshakeBackoffEntry e[kHandshakeBackoffSlots];
};

// The wait after the n-th unanswered start (n >= 1): 5 s doubling to 5 min.
inline uint32_t handshakeBackoffDelayMs(uint8_t starts) {
  uint32_t delay = kHandshakeBackoffFirstMs;
  for (uint8_t i = 1; i < starts && delay < kHandshakeBackoffMaxMs; ++i) delay *= 2;
  return delay < kHandshakeBackoffMaxMs ? delay : kHandshakeBackoffMaxMs;
}

inline HandshakeBackoffEntry* handshakeBackoffFind(HandshakeBackoffTable& t, const uint8_t mac[6]) {
  for (auto& e : t.e) {
    if (e.inUse && memcmp(e.mac, mac, 6) == 0) return &e;
  }
  return nullptr;
}

// True when an automatic start to `mac` is due now; the start is then counted
// and the next one is pushed out. A full table gives up the entry whose last
// start is oldest (that peer just gets a start sooner).
inline bool handshakeBackoffTake(HandshakeBackoffTable& t, const uint8_t mac[6], uint32_t nowMs) {
  HandshakeBackoffEntry* e = handshakeBackoffFind(t, mac);
  if (e && deadlinePending(e->nextStartMs, nowMs)) return false;
  if (!e) {
    for (auto& c : t.e) {
      if (!c.inUse) { e = &c; break; }
    }
    if (!e) {
      e = &t.e[0];
      for (auto& c : t.e) {
        if ((uint32_t)(nowMs - c.lastStartMs) > (uint32_t)(nowMs - e->lastStartMs)) e = &c;
      }
    }
    memset(e, 0, sizeof(*e));
    memcpy(e->mac, mac, 6);
    e->inUse = true;
  }
  if (e->starts < 0xFF) e->starts++;
  e->lastStartMs = nowMs;
  e->nextStartMs = deadlineAfter(nowMs, handshakeBackoffDelayMs(e->starts));
  return true;
}

// The peer answered, the user asked, or the peer was unpaired: forget it, so
// the next automatic start is due at once.
inline void handshakeBackoffClear(HandshakeBackoffTable& t, const uint8_t mac[6]) {
  HandshakeBackoffEntry* e = handshakeBackoffFind(t, mac);
  if (e) memset(e, 0, sizeof(*e));
}

// A frame proved `mac` is on the air. Ends the wait only when the peer had
// been quiet for kHandshakeBackoffQuietMs (or not heard since its first
// unanswered start); a peer heard all along keeps its back-off. Returns true
// when the wait was ended.
inline bool handshakeBackoffHeard(HandshakeBackoffTable& t, const uint8_t mac[6], uint32_t nowMs) {
  HandshakeBackoffEntry* e = handshakeBackoffFind(t, mac);
  if (!e) return false;
  const bool cameBack = e->lastHeardMs == 0 ||
                        (uint32_t)(nowMs - e->lastHeardMs) >= kHandshakeBackoffQuietMs;
  e->lastHeardMs = nowMs ? nowMs : 1;
  if (!cameBack) return false;
  e->starts = 0;
  e->nextStartMs = 0;
  return true;
}

// One warning per window, carrying the number held back since the last one.
struct WarnLimiter {
  uint32_t lastMs;
  uint32_t held;
};

// True when a warning may be printed now; *heldOut is then the number of
// warnings held back since the previous one. False counts this one as held.
inline bool warnLimiterTake(WarnLimiter& w, uint32_t nowMs, uint32_t windowMs, uint32_t* heldOut) {
  if (w.lastMs != 0 && (uint32_t)(nowMs - w.lastMs) < windowMs) {
    ++w.held;
    return false;
  }
  if (heldOut) *heldOut = w.held;
  w.held = 0;
  w.lastMs = nowMs ? nowMs : 1;
  return true;
}
