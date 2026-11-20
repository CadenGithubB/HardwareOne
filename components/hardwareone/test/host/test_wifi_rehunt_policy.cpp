// Host-side tests for the production Wi-Fi rehunt policy header: when a lost
// connection, or a failed connect the device made by itself, is followed by a
// hunt across every saved network. Also the services policy header: whether a
// later join starts the web server.
#include <cstdio>

#include "../../System_WiFiRehuntPolicy.h"
#include "../../System_WiFiServicesPolicy.h"

static int failures = 0;

#define CHECK(condition, message)                                            \
  do {                                                                       \
    if (!(condition)) {                                                      \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

using Policy = WifiRehuntPolicy;

static Policy::Inputs offline(int saved, bool armed = true, bool held = false) {
  Policy::Inputs in;
  in.connected = false;
  in.armed = armed;
  in.held = held;
  in.savedCount = saved;
  return in;
}

static void idleUntilADrop() {
  Policy p;
  CHECK(!p.poll(offline(3), 0), "offline without a drop starts nothing");
  CHECK(!p.poll(offline(3), 10 * 60 * 1000), "a boot that never connected starts nothing, however long");
  CHECK(!p.wanted && !p.hunting, "state stays clear");
}

static void firstHuntAfterTheDriverHadItsTurn() {
  Policy p;
  p.noteDropped(1000);
  CHECK(!p.poll(offline(2), 1000), "no hunt at the drop");
  CHECK(!p.poll(offline(2), 1000 + Policy::kFirstDelayMs - 1), "no hunt before the first delay");
  CHECK(p.poll(offline(2), 1000 + Policy::kFirstDelayMs), "hunt once the first delay has passed");
  CHECK(p.hunting, "the hunt is marked running");
  CHECK(!p.poll(offline(2), 1000 + 10 * Policy::kFirstDelayMs), "no second hunt while one runs");
}

static void driverRejoinEndsIt() {
  Policy p;
  p.noteDropped(0);
  Policy::Inputs in = offline(2);
  in.connected = true;
  CHECK(!p.poll(in, 5000), "connected: no hunt");
  CHECK(!p.wanted, "the driver's own rejoin clears the request");
  CHECK(!p.poll(offline(2), 10 * Policy::kFirstDelayMs), "and nothing is left to fire later");
}

static void disarmEndsIt() {
  Policy p;
  p.noteDropped(0);
  CHECK(!p.poll(offline(2, false), Policy::kFirstDelayMs), "auto-reconnect off: no hunt");
  CHECK(!p.wanted, "disarming clears the request");
  CHECK(!p.poll(offline(2), 2 * Policy::kFirstDelayMs), "re-arming alone does not bring it back");
}

static void heldRadioWaits() {
  Policy p;
  p.noteDropped(0);
  CHECK(!p.poll(offline(2, true, true), Policy::kFirstDelayMs), "radio held: no hunt");
  CHECK(p.wanted, "but the request stands");
  CHECK(p.poll(offline(2, true, false), Policy::kFirstDelayMs + 1000), "it runs once the radio is free");
}

static void singleNetworkIsLeftToTheDriver() {
  Policy p;
  p.noteDropped(0);
  CHECK(!p.poll(offline(1), 10 * Policy::kFirstDelayMs), "one saved network: the driver's retry is enough");
  CHECK(!p.poll(offline(0), 10 * Policy::kFirstDelayMs), "no saved network: nothing to hunt");
  CHECK(p.wanted, "the request stands in case a network is added");
  CHECK(p.poll(offline(2), 10 * Policy::kFirstDelayMs), "a second saved network starts the hunt");
}

static void backOff() {
  Policy p;
  uint32_t now = 0;
  p.noteDropped(now);
  now += Policy::kFirstDelayMs;
  CHECK(p.poll(offline(2), now), "first hunt");
  const uint32_t expected[] = {Policy::kGapMs[0], Policy::kGapMs[1], Policy::kGapMs[2],
                               Policy::kGapMs[2], Policy::kGapMs[2]};
  for (uint32_t gap : expected) {
    now += 20000;  // the hunt ran for 20 s
    p.huntEnded(false, false, now);
    CHECK(!p.hunting && p.wanted && p.driverIdle, "a failed hunt leaves the request and an idle driver");
    CHECK(!p.poll(offline(2), now + gap - 1), "no hunt before the gap");
    now += gap;
    CHECK(p.poll(offline(2), now), "hunt after the gap");
  }
  // Once a hunt has failed, the hunt is the only retry: one network is enough.
  now += 20000;
  p.huntEnded(false, false, now);
  now += Policy::kGapMs[2];
  CHECK(p.poll(offline(1), now), "after a failed hunt a single saved network is hunted too");
  p.huntEnded(false, false, now);
  CHECK(!p.poll(offline(0), now + Policy::kGapMs[2]), "but never with none saved");
}

static void failureCountSaturates() {
  Policy p;
  p.noteDropped(0);
  uint32_t now = Policy::kFirstDelayMs;
  for (int i = 0; i < 600; ++i) {
    CHECK(p.poll(offline(2), now), "hunt");
    p.huntEnded(false, false, now);
    now += Policy::kGapMs[2];
  }
  CHECK(p.failures == 255, "the failure count stops at 255");
  CHECK(p.waitMs() == Policy::kGapMs[2], "and the gap stays at the cap");
}

static void successEndsIt() {
  Policy p;
  p.noteDropped(0);
  CHECK(p.poll(offline(2), Policy::kFirstDelayMs), "hunt");
  p.huntEnded(true, false, Policy::kFirstDelayMs + 4000);
  CHECK(!p.wanted && !p.hunting && !p.driverIdle && p.failures == 0, "a hunt that connects clears everything");
  // A later drop starts from the first delay again.
  p.noteDropped(500000);
  CHECK(!p.poll(offline(2), 500000 + Policy::kFirstDelayMs - 1), "fresh drop: first delay again");
  CHECK(p.poll(offline(2), 500000 + Policy::kFirstDelayMs), "fresh drop: hunt");
}

static void supersededHuntWaitsAgainWithoutAPenalty() {
  Policy p;
  p.noteDropped(0);
  CHECK(p.poll(offline(2), Policy::kFirstDelayMs), "hunt");
  p.huntEnded(false, true, Policy::kFirstDelayMs + 3000);
  CHECK(p.wanted && p.failures == 0, "a superseded hunt is not a failure");
  CHECK(!p.poll(offline(2), Policy::kFirstDelayMs + 3000 + Policy::kFirstDelayMs - 1),
        "the wait starts again");
  CHECK(p.poll(offline(2), Policy::kFirstDelayMs + 3000 + Policy::kFirstDelayMs),
        "and the next hunt follows it");
}

static void cancelDuringAHunt() {
  Policy p;
  p.noteDropped(0);
  CHECK(p.poll(offline(2), Policy::kFirstDelayMs), "hunt");
  p.clear();  // wifidisconnect while the hunt runs
  CHECK(p.hunting, "the running hunt is still accounted for");
  CHECK(!p.poll(offline(2), 10 * Policy::kFirstDelayMs), "nothing starts beside it");
  p.huntEnded(false, true, Policy::kFirstDelayMs + 2000);
  CHECK(!p.hunting && !p.wanted && p.failures == 0, "its end leaves the cancelled state clear");
  CHECK(!p.poll(offline(2), 20 * Policy::kFirstDelayMs), "and nothing fires later");
}

static void repeatedDropsKeepTheClock() {
  Policy p;
  p.noteDropped(0);
  p.noteDropped(50000);  // rejoined and dropped again between two polls
  CHECK(p.poll(offline(2), Policy::kFirstDelayMs), "the first drop's clock stands");
}

static void millisWrap() {
  Policy p;
  const uint32_t start = 0xFFFFFFFFu - 10000;
  p.noteDropped(start);
  CHECK(!p.poll(offline(2), start + 30000), "before the delay, across the wrap");
  CHECK(p.poll(offline(2), start + Policy::kFirstDelayMs), "after the delay, across the wrap");
}

// A boot connect (or the companion's restore, radiopower on) that failed: the
// driver is not retrying, so the hunt is armed with one saved network too.
static void failedConnectArmsTheHunt() {
  Policy p;
  p.noteConnectFailed(0);
  CHECK(p.wanted && p.driverIdle && p.failures == 0, "a failed connect arms the hunt with an idle driver");
  CHECK(!p.poll(offline(1), 0), "no hunt at the failure");
  CHECK(!p.poll(offline(1), 59999), "no hunt before the first delay");
  CHECK(p.poll(offline(1), 60000), "one saved network is hunted 60 s after the failure");
}

static void failedConnectBacksOff() {
  Policy p;
  uint32_t now = 5000;
  p.noteConnectFailed(now);
  now += Policy::kFirstDelayMs;
  CHECK(p.poll(offline(1), now), "first hunt after the first delay");
  const uint32_t expected[] = {60000, 120000, 300000, 300000};
  for (uint32_t gap : expected) {
    now += 15000;
    p.huntEnded(false, false, now);
    CHECK(!p.poll(offline(1), now + gap - 1), "failed connect: no hunt before the gap");
    now += gap;
    CHECK(p.poll(offline(1), now), "failed connect: hunt after the gap (60/120/300 s)");
  }
  p.huntEnded(true, false, now + 3000);
  CHECK(!p.wanted && !p.driverIdle && p.failures == 0, "a hunt that joins clears it");
}

static void failedConnectEndsOnJoinOrDisarm() {
  Policy p;
  p.noteConnectFailed(0);
  Policy::Inputs in = offline(1);
  in.connected = true;
  CHECK(!p.poll(in, 1000), "connected: no hunt");
  CHECK(!p.wanted && !p.driverIdle, "a join (openwifi, the companion) clears it");
  CHECK(!p.poll(offline(1), 10 * Policy::kFirstDelayMs), "and nothing fires later");

  Policy q;
  q.noteConnectFailed(0);
  CHECK(!q.poll(offline(1, false), Policy::kFirstDelayMs), "auto-reconnect off: no hunt");
  CHECK(!q.wanted, "disarming clears it");
  CHECK(!q.poll(offline(1), 10 * Policy::kFirstDelayMs), "re-arming alone does not bring it back");
}

static void failedConnectKeepsTheClock() {
  Policy p;
  p.noteConnectFailed(0);
  p.noteConnectFailed(50000);  // the companion's restore failed again
  CHECK(p.poll(offline(1), Policy::kFirstDelayMs), "a second failure does not restart the clock");

  // A failure while a drop's hunt is wanted keeps that clock, and leaves the
  // driver idle: one saved network now qualifies.
  Policy q;
  q.noteDropped(0);
  q.noteConnectFailed(30000);
  CHECK(q.driverIdle, "a failed connect leaves the driver idle");
  CHECK(!q.poll(offline(1), Policy::kFirstDelayMs - 1), "the drop's clock stands");
  CHECK(q.poll(offline(1), Policy::kFirstDelayMs), "and one saved network is hunted");
}

static void failedConnectWaitsForTheRadio() {
  Policy p;
  p.noteConnectFailed(0);
  CHECK(!p.poll(offline(1, true, true), Policy::kFirstDelayMs), "companion offline / radio held: no hunt");
  CHECK(p.wanted, "the request stands");
  CHECK(!p.poll(offline(0), Policy::kFirstDelayMs), "no saved network: nothing to hunt");
  CHECK(p.poll(offline(1), Policy::kFirstDelayMs + 1000), "it runs once the radio is free");
}

// Every combination of the five inputs: only enabled + autostart + stopped +
// not user-closed + not the boot connect's join starts the server.
static void servicesTruthTable() {
  int starts = 0;
  for (int bits = 0; bits < 32; ++bits) {
    WifiServicesInputs in;
    in.httpEnabled = bits & 1;
    in.autostart = bits & 2;
    in.running = bits & 4;
    in.userClosed = bits & 8;
    in.bootConnectActive = bits & 16;
    const bool expected = in.httpEnabled && in.autostart && !in.running && !in.userClosed &&
                          !in.bootConnectActive;
    const bool got = wifiServicesShouldStartHttp(in);
    CHECK(got == expected, "services decision matches the rule");
    starts += got;
  }
  CHECK(starts == 1, "exactly one combination starts the server");
  WifiServicesInputs none;
  CHECK(!wifiServicesShouldStartHttp(none), "defaults start nothing");
}

int main() {
  idleUntilADrop();
  firstHuntAfterTheDriverHadItsTurn();
  driverRejoinEndsIt();
  disarmEndsIt();
  heldRadioWaits();
  singleNetworkIsLeftToTheDriver();
  backOff();
  failureCountSaturates();
  successEndsIt();
  supersededHuntWaitsAgainWithoutAPenalty();
  cancelDuringAHunt();
  repeatedDropsKeepTheClock();
  millisWrap();
  failedConnectArmsTheHunt();
  failedConnectBacksOff();
  failedConnectEndsOnJoinOrDisarm();
  failedConnectKeepsTheClock();
  failedConnectWaitsForTheRadio();
  servicesTruthTable();
  if (failures) {
    std::fprintf(stderr, "%d wifi rehunt policy check(s) failed\n", failures);
    return 1;
  }
  std::puts("wifi rehunt policy: all checks passed");
  return 0;
}
