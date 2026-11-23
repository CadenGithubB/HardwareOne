#pragma once
// ESP-NOW remote commands, requester side (1.0 step 6, DESIGN 3.4 and 3.5),
// without the radio, the lock, the allocator or the log.
//
// Two pieces, both in caller-owned memory (System_ESPNow.cpp allocates them in
// PSRAM at init and serialises the calls):
//
// RequesterTable: one entry per remote command this device sent (CMD and
// USER_SYNC), keyed by reqId (the request's msgId), 16 entries (MESH_PEER_MAX:
// a room or tag fan-out reaches up to 16 members). Each request ends in exactly
// one terminal result (G8):
//
//   Sending --ACK(0)--> Accepted --CMD_RESP--> Result                (terminal)
//      |                   |---STREAM line---> Accepted (deadline += 30 s)
//      |                   '---deadline------> TimedOut: "No result" (kept 5 min)
//      |--ACK(busy, no slot)--> Busy                                 (terminal)
//      |--ACK(other status)---> Refused                              (terminal)
//      '--no ACK in 10 s------> NoAnswer                             (terminal)
//   TimedOut --CMD_RESP--> Late: "Result (late)"                     (terminal)
//   NoAnswer --CMD_RESP--> Late (the ACK was lost, the command ran)  (terminal)
//
// ResultQueue: the CMD_RESP messages received and not yet shown, 16 entries
// with their texts in one fixed byte ring (24 KB): no allocation per result.
// A full queue (entries or bytes) refuses the message; the receive path answers
// it kBusy and the responder sends it once more under a new msgId.
//
// The host test compiles this file as is (espnow_remote_cmd_core_tests).
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "System_ESPNow_AnswerCore.h"

namespace espnowcmd {

constexpr uint8_t  kRequesters      = 16;            // MESH_PEER_MAX
constexpr uint8_t  kResults         = 16;            // results waiting to be shown
constexpr uint32_t kResultRingBytes = 24u * 1024u;   // their texts (NUL included)
constexpr uint32_t kAckWaitMs       = 10000;         // Sending -> NoAnswer (kSendStatusPendingTimeoutMs)
constexpr uint32_t kResultDeadlineMs = 30000;        // Accepted -> TimedOut, from acceptance (E4)
constexpr uint32_t kStreamExtendMs  = 30000;         // each STREAM line of the command (E4)
constexpr uint32_t kTimedOutKeepMs  = 5u * 60u * 1000u;  // a timed-out entry waits this long for a late result
constexpr uint8_t  kNameCap         = 32;

enum class State : uint8_t {
  Free = 0,
  Sending,   // sent, no ACK yet
  Accepted,  // ACK(0): the peer took it; waiting for the result
  Result,    // the result came in time (terminal)
  Busy,      // ACK busy / no slot (terminal)
  Refused,   // ACK with another status (terminal)
  NoAnswer,  // no ACK within kAckWaitMs (terminal; a result may still come: Late)
  TimedOut,  // accepted, no result by the deadline: "No result" was shown
  Late,      // the result came after "No result" / "No answer" was shown (terminal)
};

// The outcome a received history row carries (3 bits of the record's state
// field; System_ESPNow_HistoryCore.h). None: an ordinary result row.
enum class Outcome : uint8_t { None = 0, Busy = 1, Refused = 2, NoAnswer = 3, NoResult = 4, Late = 5 };

// JSON value of the `result` key of a received result row; nullptr for None.
inline const char* outcomeKey(uint8_t outcome) {
  switch (static_cast<Outcome>(outcome)) {
    case Outcome::Busy:     return "busy";
    case Outcome::Refused:  return "refused";
    case Outcome::NoAnswer: return "noAnswer";
    case Outcome::NoResult: return "noResult";
    case Outcome::Late:     return "late";
    default:                return nullptr;
  }
}

// The one word of each outcome (DESIGN 1.3), as shown on every surface.
inline const char* outcomeWord(uint8_t outcome) {
  switch (static_cast<Outcome>(outcome)) {
    case Outcome::Busy:     return "Busy";
    case Outcome::Refused:  return "Refused";
    case Outcome::NoAnswer: return "No answer";
    case Outcome::NoResult: return "No result";
    case Outcome::Late:     return "Result (late)";
    default:                return nullptr;
  }
}

// An ACK status that means "busy, send it again later" (DESIGN 1.3: full inbox,
// slot busy, no reassembly room); every other nonzero status is a refusal.
inline bool isBusyStatus(uint8_t status) {
  return status == espnowanswer::kBusy || status == espnowanswer::kNoSlot;
}

inline bool due(uint32_t now, uint32_t deadline) { return static_cast<int32_t>(now - deadline) >= 0; }

// ---- requester table --------------------------------------------------------

struct Requester {
  uint8_t  mac[6];
  uint8_t  state;       // State
  uint8_t  status;      // the ACK's status for Busy / Refused
  uint32_t reqId;
  uint32_t deadlineMs;  // Sending: no-answer time; Accepted: result deadline; TimedOut: forget time;
                        // terminal: when it ended (eviction order)
};
static_assert(sizeof(Requester) == 16, "16 bytes per requester entry (DESIGN 3.5)");

struct RequesterTable {
  Requester e[kRequesters];
};

inline bool isTerminal(uint8_t s) {
  return s == static_cast<uint8_t>(State::Result) || s == static_cast<uint8_t>(State::Busy) ||
         s == static_cast<uint8_t>(State::Refused) || s == static_cast<uint8_t>(State::NoAnswer) ||
         s == static_cast<uint8_t>(State::Late);
}

inline Requester* find(RequesterTable& t, const uint8_t mac[6], uint32_t reqId) {
  if (reqId == 0 || !mac) return nullptr;
  for (auto& r : t.e) {
    if (r.state != static_cast<uint8_t>(State::Free) && r.reqId == reqId && memcmp(r.mac, mac, 6) == 0) return &r;
  }
  return nullptr;
}

inline const Requester* findConst(const RequesterTable& t, uint32_t reqId) {
  if (reqId == 0) return nullptr;
  for (const auto& r : t.e) {
    if (r.state != static_cast<uint8_t>(State::Free) && r.reqId == reqId) return &r;
  }
  return nullptr;
}

// Room for a new request: a free entry; else the terminal one that ended first;
// else the oldest timed-out one; else the open request closest to its deadline.
// `*evictedOpen` is set when an open request (Sending / Accepted) had to go:
// its result will then be logged only.
inline Requester* pick(RequesterTable& t, uint32_t now, bool* evictedOpen) {
  if (evictedOpen) *evictedOpen = false;
  Requester* best = nullptr;
  int bestRank = 99;
  int32_t bestAge = 0;
  for (auto& r : t.e) {
    if (r.state == static_cast<uint8_t>(State::Free)) return &r;
    const int rank = isTerminal(r.state) ? 0 : r.state == static_cast<uint8_t>(State::TimedOut) ? 1 : 2;
    // Higher age = a better candidate: ended longest ago / nearest its deadline.
    const int32_t age = static_cast<int32_t>(now - r.deadlineMs);
    if (!best || rank < bestRank || (rank == bestRank && age > bestAge)) {
      best = &r;
      bestRank = rank;
      bestAge = age;
    }
  }
  if (evictedOpen) *evictedOpen = bestRank == 2;
  return best;
}

// A request goes out (before the send, so a fast ACK finds it). `waitMs`: how
// long it may wait for its ACK (longer when the frame waits for a session).
// Returns false only for reqId 0 or no MAC. The same reqId again restarts it.
inline bool begin(RequesterTable& t, const uint8_t mac[6], uint32_t reqId, uint32_t now,
                  uint32_t waitMs = kAckWaitMs, bool* evictedOpen = nullptr) {
  if (evictedOpen) *evictedOpen = false;
  if (reqId == 0 || !mac) return false;
  Requester* r = find(t, mac, reqId);
  if (!r) r = pick(t, now, evictedOpen);
  memcpy(r->mac, mac, 6);
  r->reqId = reqId;
  r->state = static_cast<uint8_t>(State::Sending);
  r->status = 0;
  r->deadlineMs = now + waitMs;
  return true;
}

// The frame waits for a session to open first: it may wait longer for its ACK.
// Changes only a request still Sending.
inline void extendSending(RequesterTable& t, const uint8_t mac[6], uint32_t reqId, uint32_t now,
                          uint32_t waitMs) {
  Requester* r = find(t, mac, reqId);
  if (r && r->state == static_cast<uint8_t>(State::Sending)) r->deadlineMs = now + waitMs;
}

// The sender sends the whole message once more under a new msgId (the
// receiver answered busy, or a rekey interrupted it): the request is now
// `newId`, Sending again. False when `oldId` is not a request of this table.
inline bool rename(RequesterTable& t, const uint8_t mac[6], uint32_t oldId, uint32_t newId, uint32_t now) {
  if (newId == 0) return false;
  Requester* r = find(t, mac, oldId);
  if (!r) return false;
  r->reqId = newId;
  r->state = static_cast<uint8_t>(State::Sending);
  r->status = 0;
  r->deadlineMs = now + kAckWaitMs;
  return true;
}

// The send failed on this device (nothing reached the air): forget it.
inline void cancel(RequesterTable& t, const uint8_t mac[6], uint32_t reqId) {
  Requester* r = find(t, mac, reqId);
  if (r) memset(r, 0, sizeof(*r));
}

// A sealed ACK from `mac` for `reqId`. Only a Sending entry changes (a
// duplicate ACK of an accepted request changes nothing). Returns the new state,
// or Free when nothing changed.
inline State onAck(RequesterTable& t, const uint8_t mac[6], uint32_t reqId, uint8_t status, uint32_t now) {
  Requester* r = find(t, mac, reqId);
  if (!r || r->state != static_cast<uint8_t>(State::Sending)) return State::Free;
  if (status == 0) {
    r->state = static_cast<uint8_t>(State::Accepted);
    r->deadlineMs = now + kResultDeadlineMs;
  } else {
    r->state = static_cast<uint8_t>(isBusyStatus(status) ? State::Busy : State::Refused);
    r->status = status;
    r->deadlineMs = now;
  }
  return static_cast<State>(r->state);
}

// A STREAM line of the command `reqId` from `mac`: the command runs, so its
// deadline moves to 30 s from now (a line also proves acceptance when its ACK
// was lost). True when a deadline moved.
inline bool onStream(RequesterTable& t, const uint8_t mac[6], uint32_t reqId, uint32_t now) {
  Requester* r = find(t, mac, reqId);
  if (!r) return false;
  if (r->state != static_cast<uint8_t>(State::Sending) && r->state != static_cast<uint8_t>(State::Accepted)) {
    return false;
  }
  r->state = static_cast<uint8_t>(State::Accepted);
  r->deadlineMs = now + kStreamExtendMs;
  return true;
}

enum class ResultKind : uint8_t {
  OnTime,     // Sending / Accepted -> Result
  Late,       // TimedOut / NoAnswer -> Late: shown as "Result (late)"
  Duplicate,  // a second result for a request that already has one: logged only
  Unknown,    // not a request of this device (or forgotten): logged only
};

inline ResultKind onResult(RequesterTable& t, const uint8_t mac[6], uint32_t reqId, uint32_t now) {
  Requester* r = find(t, mac, reqId);
  if (!r) return ResultKind::Unknown;
  switch (static_cast<State>(r->state)) {
    case State::Sending:
    case State::Accepted:
      r->state = static_cast<uint8_t>(State::Result);
      r->deadlineMs = now;
      return ResultKind::OnTime;
    case State::TimedOut:
    case State::NoAnswer:
      r->state = static_cast<uint8_t>(State::Late);
      r->deadlineMs = now;
      return ResultKind::Late;
    default:
      // Result / Late: already has one. Busy / Refused: the peer said it did
      // not take it, so a result for it is not this request's.
      return ResultKind::Duplicate;
  }
}

// One request that ended without a result (tick: NoAnswer / NoResult; an ACK:
// Busy / Refused with the ACK's status), to be shown.
struct Expired {
  uint8_t  mac[6];
  uint8_t  outcome;  // Outcome
  uint8_t  status;   // the ACK's status for Busy / Refused; 0 otherwise
  uint32_t reqId;
};

// Moves every entry whose time is up: Sending -> NoAnswer, Accepted ->
// TimedOut (kept kTimedOutKeepMs for a late result), TimedOut -> Free. Reports
// up to `max` of the first two kinds in `out` (an entry not reported stays as it
// is and is reported by a later call). Returns the count reported.
inline int tick(RequesterTable& t, uint32_t now, Expired* out, int max) {
  int n = 0;
  for (auto& r : t.e) {
    if (r.state == static_cast<uint8_t>(State::TimedOut)) {
      if (due(now, r.deadlineMs)) memset(&r, 0, sizeof(r));
      continue;
    }
    const bool sending = r.state == static_cast<uint8_t>(State::Sending);
    const bool accepted = r.state == static_cast<uint8_t>(State::Accepted);
    if ((!sending && !accepted) || !due(now, r.deadlineMs)) continue;
    if (!out || n >= max) continue;
    memcpy(out[n].mac, r.mac, 6);
    out[n].reqId = r.reqId;
    out[n].outcome = static_cast<uint8_t>(sending ? Outcome::NoAnswer : Outcome::NoResult);
    out[n].status = 0;
    ++n;
    if (sending) {
      r.state = static_cast<uint8_t>(State::NoAnswer);
      r.deadlineMs = now;
    } else {
      r.state = static_cast<uint8_t>(State::TimedOut);
      r.deadlineMs = now + kTimedOutKeepMs;
    }
  }
  return n;
}

// ---- result queue -----------------------------------------------------------

struct ResultEntry {
  uint8_t  mac[6];
  uint8_t  status;      // the CMD_RESP's status
  uint8_t  reserved;
  uint32_t reqId;
  uint32_t readyAtMs;   // shown from this time on (the lab hold; else the arrival time)
  uint16_t off;         // text position in the byte ring
  uint16_t len;         // text bytes including its NUL (>= 1)
  char     name[kNameCap];
};
static_assert(kResultRingBytes <= 0xFFFFu, "ring offsets fit 16 bits");

struct ResultQueue {
  ResultEntry e[kResults];
  uint8_t  head;    // index of the oldest entry
  uint8_t  count;
  uint16_t reserved;
  uint32_t cap;     // bytes of `bytes`
  uint8_t* bytes;
};

inline void queueInit(ResultQueue& q, uint8_t* bytes, uint32_t cap) {
  memset(&q, 0, sizeof(q));
  q.bytes = bytes;
  q.cap = bytes ? (cap > kResultRingBytes ? kResultRingBytes : cap) : 0;
}

inline void queueClear(ResultQueue& q) {
  q.head = 0;
  q.count = 0;
}

// Where `need` bytes fit after the newest text, or -1. Texts are stored whole
// (never split at the end): when the space after the newest one is too small,
// the text goes to offset 0 if it ends before the oldest one.
inline int32_t queuePlace(const ResultQueue& q, uint32_t need) {
  if (!q.bytes || need == 0 || need > q.cap) return -1;
  if (q.count == 0) return 0;
  const ResultEntry& first = q.e[q.head];
  const ResultEntry& last = q.e[(q.head + q.count - 1) % kResults];
  const uint32_t lastEnd = static_cast<uint32_t>(last.off) + last.len;
  if (last.off >= first.off) {           // not wrapped: [first.off, lastEnd) in use
    if (lastEnd + need <= q.cap) return static_cast<int32_t>(lastEnd);
    if (need <= first.off) return 0;
    return -1;
  }
  // Wrapped: [first.off, end) and [0, lastEnd) in use; free between them.
  if (lastEnd + need <= first.off) return static_cast<int32_t>(lastEnd);
  return -1;
}

// Stores one result (its text is copied with a NUL added; a text with NULs is
// cut at the first). False when the queue has no entry or no room left: the
// caller answers the message busy.
inline bool push(ResultQueue& q, const uint8_t mac[6], const char* name, uint32_t reqId, uint8_t status,
                 const char* text, size_t textLen, uint32_t readyAtMs) {
  if (!mac || q.count >= kResults) return false;
  if (!text) textLen = 0;
  const uint32_t need = static_cast<uint32_t>(textLen) + 1;
  const int32_t at = queuePlace(q, need);
  if (at < 0) return false;
  ResultEntry& r = q.e[(q.head + q.count) % kResults];
  memset(&r, 0, sizeof(r));
  memcpy(r.mac, mac, 6);
  r.status = status;
  r.reqId = reqId;
  r.readyAtMs = readyAtMs;
  r.off = static_cast<uint16_t>(at);
  r.len = static_cast<uint16_t>(need);
  if (name) {
    size_t n = strlen(name);
    if (n > kNameCap - 1) n = kNameCap - 1;
    memcpy(r.name, name, n);
  }
  if (textLen) memcpy(q.bytes + at, text, textLen);
  q.bytes[at + textLen] = '\0';
  ++q.count;
  return true;
}

// The oldest stored result, when it may be shown at `now`; nullptr otherwise.
inline const ResultEntry* front(const ResultQueue& q, uint32_t now) {
  if (q.count == 0) return nullptr;
  const ResultEntry& r = q.e[q.head];
  return due(now, r.readyAtMs) ? &r : nullptr;
}

inline const char* textOf(const ResultQueue& q, const ResultEntry& r) {
  return reinterpret_cast<const char*>(q.bytes + r.off);
}

inline void pop(ResultQueue& q) {
  if (q.count == 0) return;
  q.head = static_cast<uint8_t>((q.head + 1) % kResults);
  --q.count;
  if (q.count == 0) q.head = 0;
}

inline uint32_t bytesUsed(const ResultQueue& q) {
  uint32_t used = 0;
  for (uint8_t i = 0; i < q.count; ++i) used += q.e[(q.head + i) % kResults].len;
  return used;
}

}  // namespace espnowcmd
