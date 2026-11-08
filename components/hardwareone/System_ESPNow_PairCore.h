#pragma once
// Discover-then-confirm pairing control state (PAIR_REQUEST / PAIR_ACCEPT /
// PAIR_REJECT) without the radio, the registry or the console.
//
// The control frames are unacknowledged FF broadcasts, so each side repeats
// what it is waiting on:
//   * The initiator re-sends its REQUEST every kResendMs until an answer
//     (ACCEPT or REJECT) arrives or kRequestTtlMs passes. A KEY_EX_HELLO is
//     not an answer: the target also sends one for reasons unrelated to
//     pairing (encrypted sends, espnowkeyex, espnowprobe, the retry sweep).
//     The answer is the REQUEST's acknowledgement; the next re-sent REQUEST is
//     the answer's negative one.
//   * The target answers a repeated REQUEST without asking its user again: the
//     same answer for kAnswerMemoryMs after the user gave it, then ACCEPT while
//     the requester is already in the target's registry (the half pair a lost
//     ACCEPT leaves: the target paired, the initiator did not). A repeat of a
//     request still awaiting the user is the same request (no new prompt, no
//     answer), also after the discovery window closed.
//   * The target also sends kAnswerResends unsolicited copies of an answer,
//     kResendMs apart, for initiators that send their REQUEST only once.
// The wire format is unchanged: a peer running older firmware still pairs
// (it just does not repeat), and its single REQUEST or ACCEPT is answered or
// honoured as before.
//
// No OS dependencies: the caller holds its own lock around every call, passes
// the clock and sends the frames this returns, so the host test runs this file
// as is (espnow_pair_core_tests).
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace espnowpair {

constexpr uint32_t kRequestTtlMs = 45000;          // a pending request's lifetime (both sides)
constexpr uint32_t kResendMs = 1500;               // control-frame retransmit interval
constexpr uint8_t kAnswerResends = 2;              // unsolicited extra copies of an answer
constexpr uint32_t kAnswerMemoryMs = kRequestTtlMs; // a repeated request gets the same answer
constexpr uint32_t kOutcomeShowMs = 30000;         // how long a UI shows a finished request
constexpr size_t kNameCap = 24;

enum class Answer : uint8_t { None, Accept, Reject };
// What the target's REQUEST handler does: nothing, send an answer, or record
// the request and ask its user.
enum class Reply : uint8_t { None, Accept, Reject, Prompt };
// The initiator's last request, for the UI.
enum class Outcome : uint8_t { None, Waiting, Paired, Declined, TimedOut };

struct Request {
  bool active;
  uint8_t mac[6];
  char name[kNameCap];
  uint32_t atMs;      // when it started (the TTL and the UI's identity of a request)
  uint32_t lastTxMs;  // initiator: when the REQUEST was last sent
};

struct Answered {
  bool active;
  Answer answer;
  uint8_t mac[6];
  uint32_t atMs;
  uint32_t lastTxMs;
  uint8_t resendsLeft;
};

struct State {
  Request in;          // target: the incoming request awaiting the user
  Request out;         // initiator: our request awaiting their answer
  Answered answered;   // target: the user's last answer, repeated on demand
  Outcome outcome;     // initiator: the last request's result, for the UI
  char outcomeName[kNameCap];
  uint8_t outcomeMac[6];
  uint32_t outcomeAtMs;
};

inline bool macIs(const uint8_t a[6], const uint8_t b[6]) { return std::memcmp(a, b, 6) == 0; }

inline void copyName(char (&dst)[kNameCap], const char* src) {
  size_t i = 0;
  if (src) {
    for (; i + 1 < kNameCap && src[i] != '\0'; i++) dst[i] = src[i];
  }
  dst[i] = '\0';
}

inline void setOutcome(State& s, Outcome o, const uint8_t mac[6], const char* name, uint32_t now) {
  s.outcome = o;
  std::memcpy(s.outcomeMac, mac, 6);
  copyName(s.outcomeName, name);
  s.outcomeAtMs = now;
}

// Initiator: a request to `mac` begins; the caller sends the first REQUEST now.
// A newer request replaces an older unanswered one.
inline void startRequest(State& s, const uint8_t mac[6], const char* name, uint32_t now) {
  s.out.active = true;
  std::memcpy(s.out.mac, mac, 6);
  copyName(s.out.name, name);
  s.out.atMs = now;
  s.out.lastTxMs = now;
  setOutcome(s, Outcome::Waiting, mac, name, now);
}

// Target: a REQUEST from `mac` arrived. `alreadyPaired` is whether `mac` is in
// this device's registry; `windowOpen` whether its discovery window is open.
inline Reply onRequest(State& s, const uint8_t mac[6], const char* name, uint32_t now,
                       bool alreadyPaired, bool windowOpen) {
  if (s.answered.active && macIs(s.answered.mac, mac) &&
      (uint32_t)(now - s.answered.atMs) <= kAnswerMemoryMs) {
    return s.answered.answer == Answer::Accept ? Reply::Accept : Reply::Reject;
  }
  if (alreadyPaired) return Reply::Accept;   // half pair: the requester lost our ACCEPT
  // The same request, repeated: it stays pending until the user answers or the
  // TTL ends, even if the discovery window closed after it arrived (a REJECT
  // here would end the initiator's side while our prompt can still accept).
  if (s.in.active && macIs(s.in.mac, mac)) return Reply::None;
  if (!windowOpen) return Reply::Reject;
  s.in.active = true;
  std::memcpy(s.in.mac, mac, 6);
  copyName(s.in.name, name);
  s.in.atMs = now;
  s.in.lastTxMs = now;
  return Reply::Prompt;
}

// Target: the user answered the pending request. Copies it to `taken` and
// remembers the answer; the caller sends the first copy now. False when no
// request is pending.
inline bool answerIncoming(State& s, Answer a, uint32_t now, Request* taken) {
  if (!s.in.active || a == Answer::None) return false;
  if (taken) *taken = s.in;
  s.answered.active = true;
  s.answered.answer = a;
  std::memcpy(s.answered.mac, s.in.mac, 6);
  s.answered.atMs = now;
  s.answered.lastTxMs = now;
  s.answered.resendsLeft = kAnswerResends;
  s.in.active = false;
  return true;
}

// Initiator: an answer (ACCEPT or REJECT) arrived from `mac`. True when it
// answers our request: the request ends and, for ACCEPT, the caller completes
// the pairing once. Repeats are ignored.
inline bool onAnswer(State& s, const uint8_t mac[6], Answer a, uint32_t now, Request* ended) {
  if (!s.out.active || !macIs(s.out.mac, mac) || a == Answer::None) return false;
  if (ended) *ended = s.out;
  s.out.active = false;
  setOutcome(s, a == Answer::Accept ? Outcome::Paired : Outcome::Declined, s.out.mac, s.out.name, now);
  return true;
}

// What one sweep asks the caller to send or report.
struct Tick {
  bool resendRequest;     // send REQUEST to requestMac
  uint8_t requestMac[6];
  bool resendAnswer;      // send `answer` to answerMac
  Answer answer;
  uint8_t answerMac[6];
  bool outTimedOut;       // our request expired unanswered: report `timedOut`
  Request timedOut;
};

inline Tick tick(State& s, uint32_t now) {
  Tick t = {};
  if (s.in.active && (uint32_t)(now - s.in.atMs) > kRequestTtlMs) s.in.active = false;
  if (s.out.active) {
    if ((uint32_t)(now - s.out.atMs) > kRequestTtlMs) {
      t.outTimedOut = true;
      t.timedOut = s.out;
      s.out.active = false;
      setOutcome(s, Outcome::TimedOut, s.out.mac, s.out.name, now);
    } else if ((uint32_t)(now - s.out.lastTxMs) >= kResendMs) {
      t.resendRequest = true;
      std::memcpy(t.requestMac, s.out.mac, 6);
      s.out.lastTxMs = now;
    }
  }
  if (s.answered.active) {
    if ((uint32_t)(now - s.answered.atMs) > kAnswerMemoryMs) {
      s.answered.active = false;
    } else if (s.answered.resendsLeft > 0 && (uint32_t)(now - s.answered.lastTxMs) >= kResendMs) {
      t.resendAnswer = true;
      t.answer = s.answered.answer;
      std::memcpy(t.answerMac, s.answered.mac, 6);
      s.answered.lastTxMs = now;
      s.answered.resendsLeft--;
    }
  }
  return t;
}

// The initiator's request state for a UI: Waiting while it runs, a result for
// kOutcomeShowMs after it ends, then None.
inline Outcome outcome(const State& s, uint32_t now, char* name, size_t nameCap) {
  Outcome o = s.outcome;
  if (o != Outcome::None && o != Outcome::Waiting &&
      (uint32_t)(now - s.outcomeAtMs) > kOutcomeShowMs) {
    o = Outcome::None;
  }
  if (o != Outcome::None && name && nameCap) {
    std::strncpy(name, s.outcomeName, nameCap - 1);
    name[nameCap - 1] = '\0';
  }
  return o;
}

}  // namespace espnowpair
