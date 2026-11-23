// Host test for System_ESPNow_RemoteCmdCore.h (ESP-NOW 1.0 step 6, DESIGN 3.4-3.5):
// the remote-command requester table and the CMD_RESP result queue, compiled as is.
//
// Cases: a room fan-out to 16 responders (every request accepted and answered
// once, a 17th request pushes out the oldest open one); ACK busy / no slot /
// other status; the 30 s result deadline, "No result", then "Result (late)";
// STREAM lines move the deadline; no ACK -> "No answer" and a result after it;
// a whole-message retry under a new msgId; deadlines across the 32-bit wrap;
// tick's report limit; the result queue's byte ring against a model (random
// sizes, wraps, texts intact, refusals only when it is really full).
#include "System_ESPNow_RemoteCmdCore.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <vector>

static unsigned checks = 0, failures = 0;
#define CHECK(cond, what)                                   \
  do {                                                      \
    ++checks;                                               \
    if (!(cond)) {                                          \
      ++failures;                                           \
      std::fprintf(stderr, "FAIL %s (line %d)\n", what, __LINE__); \
    }                                                       \
  } while (0)

using namespace espnowcmd;

static void macOf(uint8_t out[6], uint8_t n) {
  const uint8_t m[6] = {0x02, 0, 0, 0, 0x10, n};
  memcpy(out, m, 6);
}
static uint8_t stateOf(const RequesterTable& t, uint32_t reqId) {
  const Requester* r = findConst(t, reqId);
  return r ? r->state : static_cast<uint8_t>(State::Free);
}
static bool is(const RequesterTable& t, uint32_t reqId, State s) { return stateOf(t, reqId) == static_cast<uint8_t>(s); }

// A room command to 16 members: 16 requests open at once, each accepted and
// answered exactly once; a second answer and an answer to nobody's request are
// logged only; a 17th request pushes the oldest open one out (its answer is then
// unknown) and says so.
static void fanOutTo16() {
  RequesterTable t{};
  const uint32_t now = 5000;
  uint8_t mac[6];
  for (uint8_t i = 0; i < 16; ++i) {
    macOf(mac, i);
    bool evicted = true;
    CHECK(begin(t, mac, 1000 + i, now + i, kAckWaitMs, &evicted) && !evicted, "fan-out: 16 requests fit");
  }
  for (uint8_t i = 0; i < 16; ++i) {
    macOf(mac, i);
    CHECK(onAck(t, mac, 1000 + i, 0, now + 50) == State::Accepted, "fan-out: each member's ACK(0) accepts its own");
  }
  // An ACK from another member for this request changes nothing.
  macOf(mac, 3);
  CHECK(onAck(t, mac, 1004, espnowanswer::kBusy, now + 60) == State::Free && is(t, 1004, State::Accepted),
        "fan-out: an ACK from the wrong member is not this request's");
  for (uint8_t i = 0; i < 16; ++i) {
    macOf(mac, i);
    CHECK(onResult(t, mac, 1000 + i, now + 100 + i) == ResultKind::OnTime, "fan-out: each result is on time");
  }
  for (uint8_t i = 0; i < 16; ++i) CHECK(is(t, 1000 + i, State::Result), "fan-out: every request ended Result");
  macOf(mac, 0);
  CHECK(onResult(t, mac, 1000, now + 200) == ResultKind::Duplicate, "fan-out: a second result is a duplicate");
  CHECK(onResult(t, mac, 4242, now + 200) == ResultKind::Unknown, "fan-out: a result for no request is unknown");
  Expired out[16];
  CHECK(tick(t, now + 10 * kResultDeadlineMs, out, 16) == 0, "fan-out: nothing expires after the results");

  // 16 open at once, then a 17th: a terminal entry would go first, but none
  // is; the open one nearest its deadline (the first sent) goes, said aloud.
  RequesterTable u{};
  for (uint8_t i = 0; i < 16; ++i) {
    macOf(mac, i);
    begin(u, mac, 2000 + i, now + i);
  }
  macOf(mac, 16);
  bool evicted = false;
  CHECK(begin(u, mac, 3000, now + 100, kAckWaitMs, &evicted) && evicted, "17th request: an open one is pushed out");
  CHECK(!findConst(u, 2000) && is(u, 2001, State::Sending) && is(u, 3000, State::Sending),
        "17th request: the oldest (2000) went, the rest stay");
  macOf(mac, 0);
  CHECK(onResult(u, mac, 2000, now + 200) == ResultKind::Unknown, "17th request: the pushed-out answer is unknown");

  // With a terminal entry present, it is reused first.
  macOf(mac, 5);
  onAck(u, mac, 2005, espnowanswer::kBusy, now + 150);
  Expired shown[16];
  CHECK(tick(u, now + 150, shown, 16) == 1 && shown[0].reqId == 2005 && is(u, 2005, State::Busy),
        "the busy one is reported");
  macOf(mac, 17);
  CHECK(begin(u, mac, 3001, now + 160, kAckWaitMs, &evicted) && !evicted && !findConst(u, 2005) &&
            is(u, 2001, State::Sending),
        "a full table reuses an ended request before an open one");
}

static void ackOutcomes() {
  RequesterTable t{};
  uint8_t mac[6];
  macOf(mac, 1);
  begin(t, mac, 10, 0);
  begin(t, mac, 11, 0);
  begin(t, mac, 12, 0);
  CHECK(onAck(t, mac, 10, espnowanswer::kBusy, 5) == State::BusyNew, "ACK busy: Busy (to report)");
  CHECK(onAck(t, mac, 11, espnowanswer::kNoSlot, 5) == State::BusyNew, "ACK no slot: Busy (to report)");
  CHECK(onAck(t, mac, 12, espnowanswer::kTooLarge, 5) == State::RefusedNew &&
            findConst(t, 12)->status == espnowanswer::kTooLarge,
        "ACK too large: Refused with its status (to report)");
  CHECK(onAck(t, mac, 10, 0, 6) == State::Free && is(t, 10, State::BusyNew), "a later ACK does not reopen it");
  CHECK(onResult(t, mac, 10, 7) == ResultKind::Duplicate, "a result for a Busy request is not shown");
  Expired out[4];
  CHECK(tick(t, 8, out, 4) == 3 && out[0].outcome == (uint8_t)Outcome::Busy && out[0].reqId == 10 &&
            out[1].outcome == (uint8_t)Outcome::Busy && out[1].status == espnowanswer::kNoSlot &&
            out[2].outcome == (uint8_t)Outcome::Refused && out[2].status == espnowanswer::kTooLarge &&
            is(t, 10, State::Busy) && is(t, 12, State::Refused),
        "tick reports Busy (with busy / no slot) and Refused (with its status) once");
  CHECK(tick(t, 9, out, 4) == 0, "each is reported once");
  begin(t, mac, 13, 0);
  onAck(t, mac, 13, 0, 1);
  CHECK(onAck(t, mac, 13, 0, 2) == State::Free && is(t, 13, State::Accepted),
        "a duplicate ACK of an accepted request changes nothing");
  CHECK(tick(t, 100000, out, 4) == 1 && out[0].reqId == 13 && out[0].outcome == (uint8_t)Outcome::NoResult,
        "Busy and Refused never expire again; the accepted one does");
  CHECK(isBusyStatus(espnowanswer::kBusy) && isBusyStatus(espnowanswer::kNoSlot) &&
            !isBusyStatus(espnowanswer::kRefused) && !isBusyStatus(0),
        "busy statuses: busy and no slot only");
}

// Accepted at t0: "No result" at t0 + 30 s exactly, kept 5 min; a result in
// that time is "Result (late)"; after it the entry is gone and an answer is unknown.
static void deadlineAndLate() {
  RequesterTable t{};
  uint8_t mac[6];
  macOf(mac, 2);
  const uint32_t t0 = 100000;
  begin(t, mac, 20, t0);
  onAck(t, mac, 20, 0, t0);
  Expired out[4];
  CHECK(tick(t, t0 + kResultDeadlineMs - 1, out, 4) == 0 && is(t, 20, State::Accepted), "29.999 s: still waiting");
  CHECK(tick(t, t0 + kResultDeadlineMs, out, 4) == 1 && out[0].outcome == (uint8_t)Outcome::NoResult &&
            out[0].reqId == 20 && memcmp(out[0].mac, mac, 6) == 0 && is(t, 20, State::TimedOut),
        "30 s: No result, reported once, entry kept");
  CHECK(tick(t, t0 + kResultDeadlineMs + 1000, out, 4) == 0, "No result is reported once");
  CHECK(onResult(t, mac, 20, t0 + 40000) == ResultKind::Late && is(t, 20, State::Late), "a result after it: late");
  CHECK(onResult(t, mac, 20, t0 + 41000) == ResultKind::Duplicate, "a second late result: duplicate");

  begin(t, mac, 21, t0);
  onAck(t, mac, 21, 0, t0);
  tick(t, t0 + kResultDeadlineMs, out, 4);
  CHECK(tick(t, t0 + kResultDeadlineMs + kTimedOutKeepMs - 1, out, 4) == 0 && is(t, 21, State::TimedOut),
        "a timed-out entry waits 5 min for a late result");
  tick(t, t0 + kResultDeadlineMs + kTimedOutKeepMs, out, 4);
  CHECK(!findConst(t, 21) && onResult(t, mac, 21, t0 + 400000) == ResultKind::Unknown,
        "after 5 min it is forgotten: a result then is logged only");
}

static void streamExtends() {
  RequesterTable t{};
  uint8_t mac[6];
  macOf(mac, 3);
  const uint32_t t0 = 7000;
  begin(t, mac, 30, t0);
  onAck(t, mac, 30, 0, t0);
  CHECK(onStream(t, mac, 30, t0 + 20000), "a STREAM line moves the deadline");
  Expired out[2];
  CHECK(tick(t, t0 + kResultDeadlineMs, out, 2) == 0, "30 s after acceptance: still running (a line came at 20 s)");
  CHECK(tick(t, t0 + 20000 + kStreamExtendMs, out, 2) == 1 && out[0].outcome == (uint8_t)Outcome::NoResult,
        "30 s after the last line: No result");
  CHECK(!onStream(t, mac, 30, t0 + 60000) && is(t, 30, State::TimedOut), "a line after No result changes nothing");
  // A line for a request whose ACK was lost proves it was accepted.
  begin(t, mac, 31, t0);
  CHECK(onStream(t, mac, 31, t0 + 100) && is(t, 31, State::Accepted), "a line while Sending: Accepted");
  uint8_t other[6];
  macOf(other, 4);
  CHECK(!onStream(t, other, 31, t0 + 200), "a line from another device is not this request's");
}

static void noAnswer() {
  RequesterTable t{};
  uint8_t mac[6];
  macOf(mac, 5);
  begin(t, mac, 40, 0);
  Expired out[2];
  CHECK(tick(t, kAckWaitMs - 1, out, 2) == 0, "no ACK for 9.999 s: still Sending");
  CHECK(tick(t, kAckWaitMs, out, 2) == 1 && out[0].outcome == (uint8_t)Outcome::NoAnswer && is(t, 40, State::NoAnswer),
        "no ACK in 10 s: No answer");
  CHECK(onAck(t, mac, 40, 0, kAckWaitMs + 5) == State::Free, "an ACK after No answer changes nothing");
  CHECK(onResult(t, mac, 40, kAckWaitMs + 900) == ResultKind::Late, "a result after No answer (ACK lost): late");
  // A frame waiting for its session may wait longer.
  begin(t, mac, 41, 0);
  extendSending(t, mac, 41, 0, kAckWaitMs + 5000);
  CHECK(tick(t, kAckWaitMs, out, 2) == 0 && tick(t, kAckWaitMs + 5000, out, 2) == 1,
        "a queued request waits its session's time longer for the ACK");
  begin(t, mac, 42, 0);
  onAck(t, mac, 42, 0, 1);
  extendSending(t, mac, 42, 0, 999999);
  CHECK(findConst(t, 42)->deadlineMs == 1 + kResultDeadlineMs, "extendSending leaves an accepted request alone");
  cancel(t, mac, 42);
  CHECK(is(t, 42, State::Accepted), "cancel leaves an answered request alone");
  begin(t, mac, 43, 0);
  cancel(t, mac, 43);
  CHECK(!findConst(t, 43), "cancel: a request still Sending is gone");
}

// Busy, then the multi-frame sender's whole-message retry under a new msgId
// (review of step 6, item 1): a held request ignores the busy ACK, follows the
// message to the new msgId and ends exactly once, by the final outcome; a
// caller's cancel of the old msgId after a failed send finds nothing to drop.
static void retryRename() {
  RequesterTable t{};
  uint8_t mac[6];
  macOf(mac, 6);
  Expired out[4];
  begin(t, mac, 50, 0);
  CHECK(hold(t, mac, 50), "hold: the multi-frame sender takes the request");
  CHECK(onAck(t, mac, 50, espnowanswer::kBusy, 10) == State::Free && is(t, 50, State::SendingRetry),
        "a held request ignores the busy ACK (its sender may send it again)");
  CHECK(tick(t, 20, out, 4) == 0, "nothing is reported for the first busy");
  CHECK(rename(t, mac, 50, 51, 500) && is(t, 51, State::SendingRetry) && !findConst(t, 50),
        "retry: the request follows its message to the new msgId");
  CHECK(onAck(t, mac, 51, 0, 520) == State::Accepted, "retry accepted under the new msgId");
  settle(t, mac, 51, Settle::Delivered, 0, 530);
  CHECK(is(t, 51, State::Accepted) && onResult(t, mac, 51, 600) == ResultKind::OnTime,
        "Delivered changes nothing after the ACK; answered under the new msgId");
  CHECK(onResult(t, mac, 50, 700) == ResultKind::Unknown, "retry: the old msgId is no request any more");
  CHECK(!rename(t, mac, 77, 78, 0), "rename of a message that is no request: nothing");

  // Busy twice: reported once, as Busy, under the final msgId.
  begin(t, mac, 60, 0);
  hold(t, mac, 60);
  onAck(t, mac, 60, espnowanswer::kBusy, 10);
  rename(t, mac, 60, 61, 500);
  onAck(t, mac, 61, espnowanswer::kBusy, 510);
  settle(t, mac, 61, Settle::Refused, espnowanswer::kBusy, 520);
  cancel(t, mac, 60);  // v4_send_user_sync after a failed send (old msgId)
  cancel(t, mac, 61);
  CHECK(tick(t, 530, out, 4) == 1 && out[0].reqId == 61 && out[0].outcome == (uint8_t)Outcome::Busy &&
            tick(t, 540, out, 4) == 0,
        "busy twice: one Busy, under the final msgId; the caller's cancel leaves it");
  // A refusal (not busy) ends a held request at once; no answer ends at the next tick; not sent is forgotten.
  begin(t, mac, 70, 0);
  hold(t, mac, 70);
  CHECK(onAck(t, mac, 70, espnowanswer::kTooLarge, 5) == State::RefusedNew, "a held request: too large is final");
  begin(t, mac, 71, 0);
  hold(t, mac, 71);
  settle(t, mac, 71, Settle::NoAnswer, 0, 600);
  cancel(t, mac, 71);
  begin(t, mac, 72, 0);
  hold(t, mac, 72);
  settle(t, mac, 72, Settle::NotSent, 0, 600);
  const int n = tick(t, 600, out, 4);
  CHECK(n == 2 && out[0].reqId == 70 && out[0].outcome == (uint8_t)Outcome::Refused && out[1].reqId == 71 &&
            out[1].outcome == (uint8_t)Outcome::NoAnswer && !findConst(t, 72),
        "settled: Refused at once, No answer at the next tick (cancel leaves it), Not sent forgotten");
}

// 16 members refuse in one pass (review of step 6, item 2): every one ends
// reported, in one tick with room for 16.
static void sixteenEndingsOnePass() {
  RequesterTable t{};
  uint8_t mac[6];
  for (uint8_t i = 0; i < 16; ++i) {
    macOf(mac, i);
    begin(t, mac, 900 + i, 0);
    onAck(t, mac, 900 + i, (i & 1) ? espnowanswer::kRefused : espnowanswer::kBusy, 5);
  }
  Expired out[16];
  const int n = tick(t, 6, out, 16);
  bool all = n == 16;
  for (int i = 0; i < n; ++i) {
    all = all && out[i].reqId == 900u + (uint32_t)i &&
          out[i].outcome == (uint8_t)((i & 1) ? Outcome::Refused : Outcome::Busy);
  }
  CHECK(all, "16 refusals in one pass: all 16 reported, each with its word");
}

static void wrapSafe() {
  RequesterTable t{};
  uint8_t mac[6];
  macOf(mac, 7);
  const uint32_t t0 = 0xFFFFF000u;  // millis() wraps 4 s later
  begin(t, mac, 60, t0);
  onAck(t, mac, 60, 0, t0);
  Expired out[2];
  CHECK(tick(t, t0 + 10000, out, 2) == 0, "across the wrap: 10 s is not 30 s");
  CHECK(tick(t, t0 + kResultDeadlineMs, out, 2) == 1, "across the wrap: 30 s is the deadline");
}

static void tickLimit() {
  RequesterTable t{};
  uint8_t mac[6];
  for (uint8_t i = 0; i < 5; ++i) {
    macOf(mac, i);
    begin(t, mac, 70 + i, 0);
  }
  Expired out[2];
  CHECK(tick(t, kAckWaitMs, out, 2) == 2, "tick reports at most its limit");
  CHECK(tick(t, kAckWaitMs, out, 2) == 2 && tick(t, kAckWaitMs, out, 2) == 1 && tick(t, kAckWaitMs, out, 2) == 0,
        "the rest are reported by later ticks, each once");
}

static void words() {
  CHECK(std::string(outcomeKey((uint8_t)Outcome::Busy)) == "busy" &&
            std::string(outcomeKey((uint8_t)Outcome::NoResult)) == "noResult" &&
            std::string(outcomeKey((uint8_t)Outcome::Late)) == "late" && !outcomeKey(0) && !outcomeKey(7),
        "outcome keys");
  CHECK(std::string(outcomeWord((uint8_t)Outcome::Late)) == "Result (late)" &&
            std::string(outcomeWord((uint8_t)Outcome::NoAnswer)) == "No answer" &&
            std::string(outcomeWord((uint8_t)Outcome::NoResult)) == "No result",
        "outcome words (DESIGN 1.3)");
  CHECK((uint8_t)Outcome::Late <= 7, "every outcome fits the history record's three state bits");
}

// The result queue against a model: random text sizes (up to the wire's
// 4200 B), random shows; a push is refused only when the entries or the
// contiguous bytes are really used up; every text comes back whole.
static void queueModel() {
  static uint8_t ring[kResultRingBytes];
  ResultQueue q;
  queueInit(q, ring, sizeof(ring));
  std::deque<std::pair<uint32_t, std::string>> model;
  srand(12345);
  uint32_t nextId = 1;
  unsigned refusedFull = 0, wraps = 0;
  uint8_t mac[6];
  macOf(mac, 9);
  for (int step = 0; step < 200000; ++step) {
    const bool doPush = (rand() % 3) != 0;
    if (doPush) {
      const size_t len = (rand() % 8 == 0) ? 4200 : static_cast<size_t>(rand() % 600);
      std::string text(len, static_cast<char>('a' + nextId % 26));
      const int32_t at = queuePlace(q, static_cast<uint32_t>(len + 1));
      const bool ok = push(q, mac, "peer", nextId, 0, text.c_str(), len, 0);
      if (ok) {
        if (at == 0 && !model.empty()) ++wraps;
        model.emplace_back(nextId, text);
      } else {
        // Refused: no entry left, or no contiguous room (checked independently).
        const bool noEntry = model.size() >= kResults;
        CHECK(noEntry || at < 0, "queue: refused only when full");
        ++refusedFull;
      }
      ++nextId;
    } else if (!model.empty()) {
      const ResultEntry* r = front(q, 0);
      CHECK(r && r->reqId == model.front().first && textOf(q, *r) == model.front().second,
            "queue: the oldest comes back whole");
      pop(q);
      model.pop_front();
    }
    CHECK(q.count == model.size(), "queue: count matches the model");
    if (failures > 20) break;
  }
  CHECK(wraps > 100 && refusedFull > 100, "queue model: wraps and refusals were exercised");
  // Never more bytes than the ring.
  CHECK(bytesUsed(q) <= kResultRingBytes, "queue: bytes in use within the ring");
  // A text with no NUL room at the end goes to the start whole.
  queueInit(q, ring, sizeof(ring));
  CHECK(!push(q, mac, "p", 1, 0, nullptr, 0, 0) || q.e[0].len == 1, "an empty result is one NUL byte");
  ResultQueue none;
  queueInit(none, nullptr, 0);
  CHECK(!push(none, mac, "p", 1, 0, "x", 1, 0), "no ring: every push refused");
  // A long name is cut to 31 bytes.
  queueInit(q, ring, sizeof(ring));
  push(q, mac, "a-very-long-device-name-of-more-than-31-bytes", 2, 0, "t", 1, 0);
  CHECK(strlen(q.e[0].name) == kNameCap - 1, "the name is cut to fit");
  // Held results: the queue is in arrival order, so a held head holds the rest.
  queueInit(q, ring, sizeof(ring));
  push(q, mac, "p", 3, 0, "held", 4, 50000);
  push(q, mac, "p", 4, 0, "next", 4, 0);
  CHECK(!front(q, 1000) && front(q, 50000) && front(q, 50000)->reqId == 3, "a held result waits; order is kept");
}

int main() {
  fanOutTo16();
  ackOutcomes();
  deadlineAndLate();
  streamExtends();
  noAnswer();
  retryRename();
  sixteenEndingsOnePass();
  wrapSafe();
  tickLimit();
  words();
  queueModel();
  std::printf("espnow_remote_cmd_core: %u checks, %u failures\n", checks, failures);
  return failures ? 1 : 0;
}
