// Host tests for G2_FeedCore.h: the shared policy behind the stock G2
// dashboard feeds (Health panel now, stock widget next). Keys and magics are
// fixed test values.
#include "../../G2_FeedCore.h"

#include <cstdio>
#include <vector>

using namespace hw1_g2_feed;

static int gFailures = 0;
#define CHECK(cond)                                                         \
  do {                                                                      \
    if (!(cond)) {                                                          \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                                  \
      gFailures++;                                                          \
    }                                                                       \
  } while (0)

// A small digest key, as a larger feed would use.
using P = Policy<uint32_t>;
static constexpr uint8_t kCmd = 7;
static constexpr Timing kT = {2000, 60000, 3000, 1};

static Action run(P& p, uint32_t now, uint32_t key, uint32_t gen = 5,
                  const Timing& t = kT, bool enabled = true, bool hasData = true) {
  return tick(p, t, enabled, hasData, true, gen, now, key);
}

static void testGate() {
  CHECK(gate(false, 5, false) == Gate::Down);
  CHECK(gate(true, 0, false) == Gate::Down);
  CHECK(gate(false, 0, true) == Gate::Down);  // down wins over busy
  CHECK(gate(true, 5, true) == Gate::Paused);
  CHECK(gate(true, 5, false) == Gate::Open);
}

static void testSettleAndConnectPush() {
  P p;
  CHECK(tick(p, kT, true, true, false, 5, 0, 1u) == Action::None);
  CHECK(tick(p, kT, true, true, true, 0, 0, 1u) == Action::None);
  CHECK(run(p, 100, 1) == Action::None);  // generation first seen: settle starts
  CHECK(p.generation == 5 && p.readySinceMs == 100);
  CHECK(run(p, 100 + kT.settleMs - 1, 1) == Action::None);
  CHECK(run(p, 100 + kT.settleMs, 1) == Action::Send);
  CHECK(p.pending == 1);

  // Disabled or no data: nothing, but the settle clock still runs.
  P q;
  CHECK(run(q, 0, 1, 5, kT, false) == Action::None);
  CHECK(run(q, kT.settleMs, 1, 5, kT, false) == Action::None);
  CHECK(run(q, kT.settleMs, 1, 5, kT, true, false) == Action::None);
  CHECK(run(q, kT.settleMs, 1) == Action::Send);
}

static void testSendOnChangeAndRetry() {
  P p;
  uint32_t t = 0;
  CHECK(run(p, t, 1) == Action::None);
  t += kT.settleMs;
  CHECK(run(p, t, 1) == Action::Send);
  sent(p, t, 0x20);
  CHECK(p.awaitingAck && p.awaitingMagic == 0x20 && p.awaitingCount == 1);
  CHECK(p.haveAttempted && p.lastAttempted == 1);
  CHECK(run(p, t + 10, 1) == Action::None);
  CHECK(ack(p, kCmd + 1, kCmd + 1, 0x21) == AckResult::Ignored);  // wrong magic
  CHECK(ack(p, kCmd, kCmd + 1, 0x20) == AckResult::Ignored);      // wrong cmd
  CHECK(ack(p, kCmd, kCmd, 0x20) == AckResult::Complete);
  CHECK(!p.awaitingAck && p.haveAcked && p.lastAcked == 1);
  CHECK(ack(p, kCmd, kCmd, 0x20) == AckResult::Ignored);  // repeat

  // Unchanged: never again. Changed: after the minimum interval only.
  CHECK(run(p, t + 10 * kT.minIntervalMs, 1) == Action::None);
  CHECK(run(p, t + 1000, 2) == Action::None);
  t += kT.minIntervalMs;
  CHECK(run(p, t, 2) == Action::Send);
  sent(p, t, 0x21);

  // No echo: one retry with the current key, then wait for a change.
  CHECK(run(p, t + kT.ackTimeoutMs - 1, 3) == Action::None);
  t += kT.ackTimeoutMs;
  CHECK(run(p, t, 3) == Action::Send);
  CHECK(p.pending == 3 && p.retries == 1);
  sent(p, t, 0x22);
  t += kT.ackTimeoutMs;
  CHECK(run(p, t, 3) == Action::None);
  CHECK(!p.awaitingAck);
  CHECK(run(p, t + 10 * kT.minIntervalMs, 3) == Action::None);
  // A change after giving up goes out with a fresh retry budget.
  CHECK(run(p, t + 10 * kT.minIntervalMs, 4) == Action::Send);
  CHECK(p.retries == 0);
}

static void testRetryBudget() {
  for (uint8_t retries : {0, 2}) {
    const Timing t = {0, 60000, 3000, retries};
    P p;
    uint32_t now = 0;
    CHECK(run(p, now, 9, 1, t) == Action::Send);
    sent(p, now, 1);
    int resent = 0;
    for (int i = 0; i < 5; i++) {
      now += t.ackTimeoutMs;
      if (run(p, now, 9, 1, t) == Action::Send) {
        resent++;
        sent(p, now, 1);
      }
    }
    CHECK(resent == retries);
  }
}

static void testVariableInterval() {
  // The stock feed shortens the interval while its expanded page is open.
  const Timing slow = {0, 60000, 3000, 1};
  const Timing fast = {0, 5000, 3000, 1};
  P p;
  CHECK(run(p, 0, 1, 1, slow) == Action::Send);
  sent(p, 0, 1);
  CHECK(ack(p, kCmd, kCmd, 1) == AckResult::Complete);
  CHECK(run(p, 5000, 2, 1, slow) == Action::None);
  CHECK(run(p, 5000, 2, 1, fast) == Action::Send);
}

static void testFailedWrite() {
  P p;
  CHECK(run(p, 0, 1) == Action::None);
  CHECK(run(p, kT.settleMs, 1) == Action::Send);
  sendFailed(p, kT.settleMs);
  CHECK(!p.awaitingAck && !p.haveAttempted);
  CHECK(run(p, kT.settleMs + 1000, 1) == Action::None);
  CHECK(run(p, kT.settleMs + kT.minIntervalMs, 1) == Action::Send);
}

// A later write of a registered unit fails: the same data goes again after
// the interval, although sent() had recorded it as attempted.
static void testUnitFailed() {
  P p;
  CHECK(run(p, 0, 1) == Action::None);
  CHECK(run(p, kT.settleMs, 1) == Action::Send);
  sent(p, kT.settleMs, 10, 3);
  CHECK(p.haveAttempted && p.lastAttempted == 1u);
  CHECK(ack(p, kCmd, kCmd, 10) == AckResult::Partial);
  unitFailed(p, kT.settleMs + 100);
  CHECK(!p.awaitingAck && !p.haveAttempted);
  CHECK(run(p, kT.settleMs + 1000, 1) == Action::None);  // interval first
  CHECK(run(p, kT.settleMs + 100 + kT.minIntervalMs, 1) == Action::Send);
  // sendFailed alone (Health: nothing registered) keeps its old meaning.
  P q;
  CHECK(run(q, 0, 1) == Action::None);
  CHECK(run(q, kT.settleMs, 1) == Action::Send);
  sent(q, kT.settleMs, 20, 2);
  sendFailed(q, kT.settleMs + 100);
  CHECK(run(q, kT.settleMs + 100 + kT.minIntervalMs, 1) == Action::None);
}

// The ack timeout runs while the feed may not send (a one-shot while it is
// off): the unit stops waiting, and nothing is retried while disabled.
static void testTimeoutWhileDisabled() {
  P p;
  CHECK(run(p, 0, 1, 5, kT, false) == Action::None);
  p.pending = 1;  // a manual send with the feed off, as the adapter registers it
  sent(p, kT.settleMs, 30, 2);
  CHECK(run(p, kT.settleMs + kT.ackTimeoutMs - 1, 1, 5, kT, false) == Action::None);
  CHECK(p.awaitingAck);
  CHECK(run(p, kT.settleMs + kT.ackTimeoutMs, 1, 5, kT, false) == Action::None);
  CHECK(!p.awaitingAck);
  CHECK(ack(p, kCmd, kCmd, 30) == AckResult::Ignored);  // a late echo
  // Enabled later: the same data is not re-sent until it changes.
  CHECK(run(p, kT.settleMs + 2 * kT.minIntervalMs, 1) == Action::None);
  CHECK(run(p, kT.settleMs + 2 * kT.minIntervalMs, 2) == Action::Send);
  // No data yet: the timeout runs too.
  P q;
  CHECK(run(q, 0, 1) == Action::None);
  CHECK(run(q, kT.settleMs, 1) == Action::Send);
  sent(q, kT.settleMs, 40, 1);
  CHECK(run(q, kT.settleMs + kT.ackTimeoutMs, 1, 5, kT, true, false) == Action::None);
  CHECK(!q.awaitingAck);
}

static void testLinkGeneration() {
  P p;
  CHECK(run(p, 0, 1) == Action::None);
  CHECK(run(p, kT.settleMs, 1) == Action::Send);
  sent(p, kT.settleMs, 0x30);

  // Link down: the echo in flight is forgotten; an echo from it is ignored.
  linkDown(p);
  CHECK(p.generation == 0 && !p.awaitingAck);
  CHECK(onAck(p, 5, kCmd, kCmd, 0x30) == AckResult::Ignored);

  // New generation: full reset, settle again, connect push even if unchanged.
  const uint32_t t = 10 * kT.minIntervalMs;
  CHECK(run(p, t, 1, 6) == Action::None);
  CHECK(!p.sentThisLink && !p.haveAttempted && p.readySinceMs == t);
  CHECK(run(p, t + kT.settleMs, 1, 6) == Action::Send);
  sent(p, t + kT.settleMs, 0x31);
  CHECK(onAck(p, 5, kCmd, kCmd, 0x31) == AckResult::Ignored);  // old link
  CHECK(onAck(p, 0, kCmd, kCmd, 0x31) == AckResult::Ignored);
  CHECK(onAck(p, 6, kCmd, kCmd, 0x31) == AckResult::Complete);

  // A changed generation without a down in between resets as well.
  CHECK(run(p, t + 2 * kT.settleMs, 1, 7) == Action::None);
  CHECK(p.generation == 7 && !p.sentThisLink);

  // tick() with ready=false behaves like linkDown().
  sent(p, t, 0x40);
  CHECK(tick(p, kT, true, true, false, 7, t, 1u) == Action::None);
  CHECK(p.generation == 0 && !p.awaitingAck);
}

static void testPausedLapKeepsState() {
  // A Paused lap does not call tick(): the echo timer, interval and link
  // state survive the session, so nothing bursts out when it ends.
  P p;
  CHECK(run(p, 0, 1) == Action::None);
  CHECK(run(p, kT.settleMs, 1) == Action::Send);
  sent(p, kT.settleMs, 0x50);
  const P before = p;
  CHECK(gate(true, 5, true) == Gate::Paused);
  CHECK(p.generation == before.generation && p.awaitingAck == before.awaitingAck);
  // Session ends long after: one retry of the unacked unit, no more.
  const uint32_t t = kT.settleMs + 30 * kT.minIntervalMs;
  CHECK(run(p, t, 1) == Action::Send);
  sent(p, t, 0x51);
  CHECK(run(p, t + kT.ackTimeoutMs, 1) == Action::None);
}

static void testUnitAck() {
  P p;
  CHECK(run(p, 0, 1) == Action::None);
  CHECK(run(p, kT.settleMs, 1) == Action::Send);
  // Three messages, magics wrapping over 0xFF -> 1 (0 is never used).
  uint8_t first = 0xFE;
  sent(p, kT.settleMs, first, 3);
  CHECK(p.awaitingCount == 3);
  CHECK(unitIndex(p, 0xFE) == 0 && unitIndex(p, 0xFF) == 1 && unitIndex(p, 0x01) == 2);
  CHECK(unitIndex(p, 0x00) == -1 && unitIndex(p, 0x02) == -1);
  CHECK(ack(p, kCmd, kCmd, 0x01) == AckResult::Partial);  // out of order
  CHECK(ack(p, kCmd, kCmd, 0x01) == AckResult::Ignored);  // duplicate
  CHECK(ack(p, kCmd, kCmd, 0x02) == AckResult::Ignored);  // not in the unit
  CHECK(ack(p, kCmd, kCmd, 0xFE) == AckResult::Partial);
  CHECK(p.awaitingAck && !p.haveAcked);
  CHECK(ack(p, kCmd, kCmd, 0xFF) == AckResult::Complete);
  CHECK(!p.awaitingAck && p.haveAcked);

  // A partly echoed unit times out from its last write (extend) and is
  // retried whole, with a fresh mask.
  uint32_t t = kT.settleMs + kT.minIntervalMs;
  CHECK(run(p, t, 2) == Action::Send);
  sent(p, t, 0x10, 2);
  t += 2000;
  extend(p, t);
  CHECK(ack(p, kCmd, kCmd, 0x10) == AckResult::Partial);
  CHECK(run(p, t + kT.ackTimeoutMs - 1, 2) == Action::None);
  CHECK(run(p, t + kT.ackTimeoutMs, 2) == Action::Send);
  sent(p, t + kT.ackTimeoutMs, 0x12, 2);
  CHECK(p.ackedMask == 0);
  CHECK(ack(p, kCmd, kCmd, 0x10) == AckResult::Ignored);  // previous unit
  CHECK(ack(p, kCmd, kCmd, 0x13) == AckResult::Partial);
  CHECK(ack(p, kCmd, kCmd, 0x12) == AckResult::Complete);

  // extend() does nothing with nothing in flight.
  const uint32_t last = p.lastSendMs;
  extend(p, last + 5);
  CHECK(p.lastSendMs == last);

  // Count bounds: 0 or too many become 1; the maximum unit works.
  P q;
  sent(q, 0, 9, 0);
  CHECK(q.awaitingCount == 1);
  sent(q, 0, 9, kMaxUnitMessages + 1);
  CHECK(q.awaitingCount == 1);
  sent(q, 0, 0xF8, kMaxUnitMessages);
  uint8_t m = 0xF8;
  for (uint8_t i = 0; i + 1 < kMaxUnitMessages; i++, m = nextMagic(m))
    CHECK(ack(q, kCmd, kCmd, m) == AckResult::Partial);
  CHECK(ack(q, kCmd, kCmd, m) == AckResult::Complete);
}

static void testTwoInstances() {
  // Independent feeds share nothing but the link.
  P a, b;
  const Timing tb = {1000, 30000, 3000, 1};
  CHECK(run(a, 0, 1) == Action::None);
  CHECK(run(b, 0, 7, 5, tb) == Action::None);
  CHECK(run(b, 1000, 7, 5, tb) == Action::Send);
  CHECK(run(a, 1000, 1) == Action::None);
  sent(b, 1000, 0x10);
  CHECK(run(a, 2000, 1) == Action::Send);
  sent(a, 2000, 0x10);  // same magic, different sid: each matches its own
  CHECK(ack(a, 2, 2, 0x10) == AckResult::Complete);
  CHECK(b.awaitingAck);
  CHECK(ack(b, kCmd, kCmd, 0x10) == AckResult::Complete);
}

static void testWrap() {
  P p;
  const uint32_t near = 0xFFFFFF00u;
  CHECK(run(p, near, 1) == Action::None);
  CHECK(run(p, near + kT.settleMs, 1) == Action::Send);
  sent(p, near + kT.settleMs, 1);
  CHECK(run(p, near + kT.settleMs + kT.ackTimeoutMs, 1) == Action::Send);
  CHECK(tooSoon(p, near + kT.settleMs + 10, 1000));
}

static void testTooSoonAndMagic() {
  CHECK(!tooSoon(false, 0, 10, 1000));
  CHECK(tooSoon(true, 0, 999, 1000));
  CHECK(!tooSoon(true, 0, 1000, 1000));
  CHECK(tooSoon(true, 0xFFFFFFF0u, 0x10, 1000));  // across the wrap
  P p;
  CHECK(!tooSoon(p, 0, 1000));
  sendFailed(p, 50);
  CHECK(tooSoon(p, 60, 1000) && !tooSoon(p, 1050, 1000));
  CHECK(nextMagic(0) == 1 && nextMagic(1) == 2 && nextMagic(0xFF) == 1);
}

static void testCounters() {
  Counters c;
  P p;
  CHECK(run(p, 0, 1) == Action::None);
  CHECK(run(p, kT.settleMs, 1) == Action::Send);
  sent(p, kT.settleMs, 4);
  countSent(c, kT.settleMs, 4);
  countLink(c, p, true, false);
  CHECK(c.linkReady && !c.busy && c.awaitingAck && c.attemptedThisLink);
  CHECK(c.sends == 1 && c.lastSendMs == kT.settleMs && c.lastMagic == 4);
  CHECK(c.lastAttemptMs == kT.settleMs);
  countFailed(c);
  countAck(c, kT.settleMs + 100);
  CHECK(c.sendFailures == 1 && c.acks == 1 && c.lastAckMs == kT.settleMs + 100);
  CHECK(!c.awaitingAck);
  linkDown(p);
  countLink(c, p, false, true);
  CHECK(!c.linkReady && c.busy && !c.awaitingAck && c.sends == 1);
}

static std::vector<uint8_t> bytes(std::initializer_list<int> v) {
  std::vector<uint8_t> out;
  for (int b : v) out.push_back((uint8_t)b);
  return out;
}

static void testParseCmdMagic() {
  uint8_t cmd = 0, magic = 0;
  auto ok = bytes({0x08, 0x02, 0x10, 0x2a, 0x22, 0x02, 0x08, 0x01});
  CHECK(parseCmdMagic(ok.data(), ok.size(), cmd, magic));
  CHECK(cmd == 2 && magic == 0x2a);
  // Multi-byte varint magic up to 0xFF.
  auto wide = bytes({0x08, 0x01, 0x10, 0xff, 0x01});
  CHECK(parseCmdMagic(wide.data(), wide.size(), cmd, magic) && magic == 0xFF);
  auto over = bytes({0x08, 0x01, 0x10, 0x80, 0x02});
  CHECK(!parseCmdMagic(over.data(), over.size(), cmd, magic));
  auto noCmd = bytes({0x10, 0x2a, 0x08, 0x02});
  CHECK(!parseCmdMagic(noCmd.data(), noCmd.size(), cmd, magic));
  auto truncated = bytes({0x08, 0x02, 0x10, 0x80});
  CHECK(!parseCmdMagic(truncated.data(), truncated.size(), cmd, magic));
  CHECK(!parseCmdMagic(ok.data(), 3, cmd, magic));
  CHECK(!parseCmdMagic(nullptr, 8, cmd, magic));
}

int main() {
  testGate();
  testSettleAndConnectPush();
  testSendOnChangeAndRetry();
  testRetryBudget();
  testVariableInterval();
  testFailedWrite();
  testUnitFailed();
  testTimeoutWhileDisabled();
  testLinkGeneration();
  testPausedLapKeepsState();
  testUnitAck();
  testTwoInstances();
  testWrap();
  testTooSoonAndMagic();
  testCounters();
  testParseCmdMagic();
  if (gFailures) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("g2_feed_core_tests: OK\n");
  return 0;
}
