#ifndef G2_FEED_CORE_H
#define G2_FEED_CORE_H

// Shared policy for feeding a stock G2 dashboard panel from HardwareOne: the
// Health panel (sid 0x0E, G2_HealthPushCore.h) and later the stock widget
// (sid 0x01). Pure, host-testable, no Arduino/IDF, no allocation.
//
// One feed instance = one Policy<Key> + one magic counter, owned by the G2
// control task (g2_owner). Each lap the adapter:
//   1. reads the right-temple link and asks gate(): Down resets the link
//      state (linkDown), Paused skips the lap without touching it (voice,
//      QuickNote and the other exclusive sessions must not cause a burst when
//      they end), Open goes on;
//   2. calls tick() with the current Key; on Action::Send it encodes
//      p.pending, writes it, then calls sent() or sendFailed();
//   3. matches echoes from the same temple with onAck().
// Status counters (Counters) are copied out by the adapter under its own lock.
//
// Key is whatever "the same data" means for the feed: the clamped snapshot
// for Health, a small digest of the batch for a larger feed. It must be
// default-constructible, copyable and comparable with ==. It is copied three
// times per instance (pending, lastAttempted, lastAcked), so keep it small.
//
// A send unit may be several messages (one magic each, consecutive by
// nextMagic). It is acknowledged when every magic in it has been echoed.
// Register the unit with sent() before or right after its first write and
// call extend() after each later write, so the ack timer runs from the last
// one; acks that arrive between laps are matched against the registered unit.
//
// Wrap-safe millisecond arithmetic throughout.

#include <stddef.h>
#include <stdint.h>

namespace hw1_g2_feed {

enum class Action : uint8_t { None, Send };
enum class Gate : uint8_t { Down, Paused, Open };
enum class AckResult : uint8_t { Ignored, Partial, Complete };

// Most messages one send unit can hold (ack bitmask width).
static constexpr uint8_t kMaxUnitMessages = 16;

struct Timing {
  uint32_t settleMs;       // after the link generation first seen ready
  uint32_t minIntervalMs;  // between sends of changed data (may vary per tick)
  uint32_t ackTimeoutMs;   // no complete echo within this -> retry
  uint8_t maxRetries;      // retries per unit, then wait for a change
};

template <typename Key>
struct Policy {
  uint32_t generation = 0;  // right-temple link generation being served
  uint32_t readySinceMs = 0;
  bool sentThisLink = false;
  uint32_t lastSendMs = 0;
  Key lastAcked = Key();
  bool haveAcked = false;
  // Last data that reached the radio (acked or not). Unchanged data is never
  // re-sent after its retries, acked or rejected.
  Key lastAttempted = Key();
  bool haveAttempted = false;
  bool awaitingAck = false;
  uint8_t awaitingMagic = 0;  // first magic of the unit in flight
  uint8_t awaitingCount = 0;  // messages in it (1..kMaxUnitMessages)
  uint16_t ackedMask = 0;     // bit i = message i echoed
  uint8_t retries = 0;
  Key pending = Key();
};

// Status counters for the adapter's `status` command. Plain data: the adapter
// updates them under its own lock with the helpers below.
struct Counters {
  bool linkReady = false;
  bool busy = false;
  uint32_t sends = 0;          // successful writes (units), never reset
  uint32_t sendFailures = 0;   // failed writes and refused one-shot requests
  uint32_t acks = 0;           // units fully echoed
  uint32_t lastSendMs = 0;     // last successful write
  uint32_t lastAckMs = 0;
  uint8_t lastMagic = 0;
  bool awaitingAck = false;
  // The policy's own pair, which a manual send tests: the last write that was
  // sent or tried on this link (lastSendMs above only moves on success).
  uint32_t lastAttemptMs = 0;
  bool attemptedThisLink = false;
};

// Rolling request id, never 0 (0 would be omitted by a proto3 encoder and
// cannot be told apart from "no magic" in an echo).
inline uint8_t nextMagic(uint8_t prev) { return (uint8_t)(prev == 0xFF ? 1 : prev + 1); }

// Link gate for one lap. `ready`/`generation` describe the temple the feed
// writes; `busy` is true while an exclusive glasses session owns it.
inline Gate gate(bool ready, uint32_t generation, bool busy) {
  if (!ready || generation == 0) return Gate::Down;
  return busy ? Gate::Paused : Gate::Open;
}

// Link gone: forget the generation and any echo in flight. The next ready
// generation starts a fresh settle window and connect push.
template <typename Key>
inline void linkDown(Policy<Key>& p) {
  p.generation = 0;
  p.awaitingAck = false;
}

// Decide whether to send now. `ready` = the feed's temple is ready at
// `generation`; `enabled` = the feed is on; `hasData` = `current` is worth
// showing (a feed never blanks its panel automatically).
template <typename Key>
inline Action tick(Policy<Key>& p, const Timing& t, bool enabled, bool hasData,
                   bool ready, uint32_t generation, uint32_t nowMs,
                   const Key& current) {
  if (!ready || generation == 0) {
    linkDown(p);
    return Action::None;
  }
  if (generation != p.generation) {
    p = Policy<Key>{};
    p.generation = generation;
    p.readySinceMs = nowMs;
  }
  // The ack timeout runs whether or not the feed may send now: a unit that
  // went out as a one-shot while the feed is off (or before data was ready)
  // must not wait for its echo for the rest of the link.
  bool timedOut = false;
  if (p.awaitingAck && (uint32_t)(nowMs - p.lastSendMs) >= t.ackTimeoutMs) {
    p.awaitingAck = false;
    timedOut = true;
  }
  if (!enabled || !hasData) return Action::None;
  if ((uint32_t)(nowMs - p.readySinceMs) < t.settleMs) return Action::None;
  if (p.awaitingAck) return Action::None;
  if (timedOut && p.retries < t.maxRetries) {
    p.retries++;
    p.pending = current;
    return Action::Send;
  }
  // A timed-out unit past its retries waits for the data to change.
  if (!p.sentThisLink) {
    p.retries = 0;
    p.pending = current;
    return Action::Send;
  }
  if ((uint32_t)(nowMs - p.lastSendMs) < t.minIntervalMs) return Action::None;
  if (p.haveAttempted && current == p.lastAttempted) return Action::None;
  p.retries = 0;
  p.pending = current;
  return Action::Send;
}

// p.pending went out as `count` messages with magics firstMagic,
// nextMagic(firstMagic), ...; wait for their echoes. A count outside
// 1..kMaxUnitMessages is treated as 1.
template <typename Key>
inline void sent(Policy<Key>& p, uint32_t nowMs, uint8_t firstMagic,
                 uint8_t count = 1) {
  if (count == 0 || count > kMaxUnitMessages) count = 1;
  p.sentThisLink = true;
  p.lastSendMs = nowMs;
  p.awaitingAck = true;
  p.awaitingMagic = firstMagic;
  p.awaitingCount = count;
  p.ackedMask = 0;
  p.lastAttempted = p.pending;
  p.haveAttempted = true;
}

// A later message of a paced unit was written: restart the ack timer (and the
// minimum interval) from it. No effect when nothing is in flight.
template <typename Key>
inline void extend(Policy<Key>& p, uint32_t nowMs) {
  if (p.awaitingAck) p.lastSendMs = nowMs;
}

// The write itself failed (locks/link): nothing reliable reached the glasses,
// so the same data may be tried again once the minimum interval has passed.
template <typename Key>
inline void sendFailed(Policy<Key>& p, uint32_t nowMs) {
  p.sentThisLink = true;
  p.lastSendMs = nowMs;
  p.awaitingAck = false;
}

// A later write of a unit already registered with sent() failed: the glasses
// hold part of it at most. Like sendFailed(), and the same data counts as not
// tried, so it goes again once the minimum interval has passed even when it
// has not changed (sent() had already recorded it as attempted).
template <typename Key>
inline void unitFailed(Policy<Key>& p, uint32_t nowMs) {
  sendFailed(p, nowMs);
  p.haveAttempted = false;
}

// Index of `magic` in the unit in flight, or -1.
template <typename Key>
inline int unitIndex(const Policy<Key>& p, uint8_t magic) {
  uint8_t m = p.awaitingMagic;
  for (uint8_t i = 0; i < p.awaitingCount; i++) {
    if (m == magic) return i;
    m = nextMagic(m);
  }
  return -1;
}

// An echo {cmd, magic} for the unit in flight. Echoes for another command, an
// unknown or repeated magic, or nothing in flight are ignored.
template <typename Key>
inline AckResult ack(Policy<Key>& p, uint8_t expectedCmd, uint8_t cmd,
                     uint8_t magic) {
  if (!p.awaitingAck || cmd != expectedCmd) return AckResult::Ignored;
  const int i = unitIndex(p, magic);
  if (i < 0) return AckResult::Ignored;
  const uint16_t bit = (uint16_t)(1u << i);
  if (p.ackedMask & bit) return AckResult::Ignored;
  p.ackedMask |= bit;
  const uint16_t all = (uint16_t)((1u << p.awaitingCount) - 1u);
  if (p.ackedMask != all) return AckResult::Partial;
  p.awaitingAck = false;
  p.lastAcked = p.pending;
  p.haveAcked = true;
  p.retries = 0;
  return AckResult::Complete;
}

// ack() for an echo that arrived on link generation `rxGeneration`: an echo
// from an older link never matches.
template <typename Key>
inline AckResult onAck(Policy<Key>& p, uint32_t rxGeneration,
                       uint8_t expectedCmd, uint8_t cmd, uint8_t magic) {
  if (rxGeneration == 0 || rxGeneration != p.generation) return AckResult::Ignored;
  return ack(p, expectedCmd, cmd, magic);
}

// Bound for one-shot (manual) sends: true while the last write or attempt on
// this link is younger than `floorMs`.
inline bool tooSoon(bool attemptedThisLink, uint32_t lastAttemptMs,
                    uint32_t nowMs, uint32_t floorMs) {
  return attemptedThisLink && (uint32_t)(nowMs - lastAttemptMs) < floorMs;
}
template <typename Key>
inline bool tooSoon(const Policy<Key>& p, uint32_t nowMs, uint32_t floorMs) {
  return tooSoon(p.sentThisLink, p.lastSendMs, nowMs, floorMs);
}

// ---------------------------------------------------------------- counters

template <typename Key>
inline void countLink(Counters& c, const Policy<Key>& p, bool ready, bool busy) {
  c.linkReady = ready;
  c.busy = busy;
  c.awaitingAck = p.awaitingAck;
  c.lastAttemptMs = p.lastSendMs;
  c.attemptedThisLink = p.sentThisLink;
}

inline void countSent(Counters& c, uint32_t nowMs, uint8_t magic) {
  c.sends++;
  c.lastSendMs = nowMs;
  c.lastMagic = magic;
}

inline void countFailed(Counters& c) { c.sendFailures++; }

inline void countAck(Counters& c, uint32_t nowMs) {
  c.acks++;
  c.lastAckMs = nowMs;
  c.awaitingAck = false;
}

// ---------------------------------------------------------------- echo

// Leading {1 cmd varint, 2 magic varint} of an echo, as the stock dashboard
// packages (HealthDataPackage, DashboardDataPackage) start. Anything after the
// pair is left to the adapter. Both must be single-tag fields <= 0xFF.
inline bool parseCmdMagic(const uint8_t* pb, size_t len, uint8_t& cmd,
                          uint8_t& magic) {
  if (!pb || len < 4) return false;
  size_t pos = 0;
  auto field = [&](uint8_t tag, uint32_t& v) {
    if (pos >= len || pb[pos] != tag) return false;
    pos++;
    v = 0;
    for (int shift = 0; shift < 35; shift += 7) {
      if (pos >= len) return false;
      const uint8_t b = pb[pos++];
      v |= (uint32_t)(b & 0x7F) << shift;
      if (!(b & 0x80)) return true;
    }
    return false;
  };
  uint32_t c = 0, m = 0;
  if (!field(0x08, c) || !field(0x10, m) || c > 0xFF || m > 0xFF) return false;
  cmd = (uint8_t)c;
  magic = (uint8_t)m;
  return true;
}

}  // namespace hw1_g2_feed

#endif  // G2_FEED_CORE_H
