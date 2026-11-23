// Two-way liveness of a mesh peer (ESP-NOW 1.0 step 8, RC3):
// System_ESPNow_LinkCore.h and the peer-state rule of
// System_ESPNow_PeerStateCore.h, the production dependency-free headers.
//
// L1-L4: the one link definition (heard / two-way, hysteresis, distinct
// evidence, the 30 s window, wrap-safe); L5: the heartbeat echo memory; L6:
// the record of direct sends an ACK can prove; L7: the back-off ceiling; L8:
// the event rate limit; L9: the peer state from the link; L10-L12: a two-board
// simulation at 1 s steps (heartbeat tick 5 s, sealed when a session is up):
// both ways -> online within two heartbeat rounds; the one-way case (B cannot
// hear A) for 10 simulated minutes: A shows B connecting (one-way) and B shows A
// offline the whole time, no flap and no event; one lost heartbeat of the
// deaf board's 10 s stream keeps it heard; a link flapping every 5 s for 10
// minutes posts at most one event per 30 s.
#include "System_ESPNow_LinkCore.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int gFailures = 0;
static int gChecks = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    gChecks++;                                         \
    if (!(cond)) {                                     \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
      gFailures++;                                     \
    }                                                  \
  } while (0)

namespace {

MeshLinkState fresh() {
  MeshLinkState s;
  std::memset(&s, 0, sizeof(s));
  return s;
}

// L1: heard only -> OneWay; not heard -> None.
void l1Heard(uint32_t t0) {
  MeshLinkState s = fresh();
  CHECK(meshLinkOf(s, 0, t0) == MeshLink::None, "L1 [%08x] never heard: none", t0);
  CHECK(meshLinkOf(s, t0, t0 + 29999) == MeshLink::OneWay, "L1 [%08x] heard 29.999 s ago: one-way", t0);
  CHECK(meshLinkOf(s, t0, t0 + 30000) == MeshLink::None, "L1 [%08x] heard 30 s ago: none", t0);
}

// L2: entering two-way needs two distinct evidences within 30 s; staying needs one.
void l2Hysteresis(uint32_t t0) {
  MeshLinkState s = fresh();
  uint32_t t = t0;
  CHECK(meshLinkNoteTwoWay(s, 101, t, t), "L2 [%08x] first evidence counts", t0);
  CHECK(meshLinkOf(s, t, t) == MeshLink::OneWay, "L2 [%08x] one evidence is not enough to enter", t0);
  CHECK(!meshLinkNoteTwoWay(s, 101, t + 1000, t + 1000), "L2 [%08x] the same id counts once", t0);
  CHECK(meshLinkOf(s, t + 1000, t + 1000) == MeshLink::OneWay, "L2 [%08x] a repeat does not enter", t0);
  CHECK(!meshLinkNoteTwoWay(s, 0, t + 1000, t + 1000), "L2 [%08x] id 0 is never evidence", t0);
  t += 5000;
  CHECK(meshLinkNoteTwoWay(s, 102, t, t), "L2 [%08x] second distinct evidence", t0);
  CHECK(meshLinkOf(s, t, t) == MeshLink::TwoWay && s.wasTwoWay, "L2 [%08x] two distinct: two-way", t0);
  // 25 s later the first evidence ages out, one remains: staying needs one.
  CHECK(meshLinkOf(s, t + 25000, t + 25000) == MeshLink::TwoWay, "L2 [%08x] staying needs one", t0);
  // Settled as one-way (all evidence old), entering again needs two.
  CHECK(meshLinkSettle(s, t + 31000, t + 31000) == MeshLink::OneWay && !s.wasTwoWay,
        "L2 [%08x] no evidence for 30 s: one-way", t0);
  CHECK(meshLinkNoteTwoWay(s, 103, t + 31000, t + 31000) &&
            meshLinkOf(s, t + 31000, t + 31000) == MeshLink::OneWay,
        "L2 [%08x] after falling back one evidence does not re-enter", t0);
  CHECK(meshLinkNoteTwoWay(s, 104, t + 32000, t + 32000) &&
            meshLinkOf(s, t + 32000, t + 32000) == MeshLink::TwoWay,
        "L2 [%08x] two again re-enter", t0);
  // Not heard for 30 s wins over any evidence.
  CHECK(meshLinkOf(s, t + 1000, t + 32000) == MeshLink::None, "L2 [%08x] silence is none", t0);
}

// L3: hits are capped at 3; the oldest slot is reused.
void l3Cap() {
  MeshLinkState s = fresh();
  for (uint32_t i = 1; i <= 5; i++) meshLinkNoteTwoWay(s, i, 1000 * i, 1000 * i);
  CHECK(meshLinkHits(s, 5000) == 3, "L3 three slots");
  bool has3 = false, has4 = false, has5 = false, has1 = false;
  for (uint8_t i = 0; i < kMeshLinkEvidenceSlots; i++) {
    has1 |= s.evidenceId[i] == 1;
    has3 |= s.evidenceId[i] == 3;
    has4 |= s.evidenceId[i] == 4;
    has5 |= s.evidenceId[i] == 5;
  }
  CHECK(has3 && has4 && has5 && !has1, "L3 the oldest evidence gave way");
  // An evicted id may count again (it is a new evidence by then).
  CHECK(meshLinkNoteTwoWay(s, 1, 6000, 6000), "L3 an evicted id counts again");
}

// L4: the 30 s windows across the millis() wrap.
void l4Wrap() {
  const uint32_t t0 = 0xFFFFF000u;
  MeshLinkState s = fresh();
  meshLinkNoteTwoWay(s, 7, t0, t0);
  meshLinkNoteTwoWay(s, 8, t0 + 3000, t0 + 3000);  // crosses 0
  CHECK(meshLinkOf(s, t0 + 3000, t0 + 3000) == MeshLink::TwoWay, "L4 two-way across the wrap");
  CHECK(meshLinkOf(s, t0 + 3000, t0 + 33000) == MeshLink::None, "L4 silence across the wrap");
}

// L5: heartbeat echo memory.
void l5Echo() {
  MeshLinkState s = fresh();
  CHECK(meshLinkEchoFor(s, 1000) == 0 && meshLinkRssiFor(s, 1000) == 0, "L5 nothing heard: echo 0, rssi 0");
  meshLinkNoteHeartbeatHeard(s, 555, -61, -70, 1000);
  CHECK(meshLinkEchoFor(s, 30999) == 555 && meshLinkRssiFor(s, 30999) == -61, "L5 echo within 30 s");
  CHECK(meshLinkEchoFor(s, 31000) == 0 && meshLinkRssiFor(s, 31000) == 0, "L5 no echo after 30 s");
  CHECK(s.peerHeardRssi == -70, "L5 the peer's report of our heartbeats");
  meshLinkNoteHeartbeatSent(s, 11, 1000);
  meshLinkNoteHeartbeatSent(s, 12, 6000);
  CHECK(meshLinkEchoMatches(s, 11, 7000) && meshLinkEchoMatches(s, 12, 7000), "L5 last two match");
  meshLinkNoteHeartbeatSent(s, 13, 11000);
  CHECK(!meshLinkEchoMatches(s, 11, 12000) && meshLinkEchoMatches(s, 12, 12000) &&
            meshLinkEchoMatches(s, 13, 12000),
        "L5 a third send evicts the oldest");
  CHECK(!meshLinkEchoMatches(s, 12, 36000) && meshLinkEchoMatches(s, 13, 36000), "L5 30 s window per send");
  CHECK(!meshLinkEchoMatches(s, 0, 12000), "L5 echo 0 never matches");
  CHECK(!meshLinkEchoMatches(s, 99, 12000), "L5 an id never sent does not match");
}

// L6: direct sends an ACK can prove.
void l6Direct() {
  MeshLinkState s = fresh();
  CHECK(!meshLinkSentDirect(s, 5), "L6 nothing sent");
  meshLinkNoteDirectSend(s, 5);
  meshLinkNoteDirectSend(s, 5);  // fragments of one message
  CHECK(meshLinkSentDirect(s, 5) && s.directTxNext == 1, "L6 one entry per message");
  for (uint32_t id = 6; id < 6 + kMeshLinkDirectTxSlots; id++) meshLinkNoteDirectSend(s, id);
  CHECK(!meshLinkSentDirect(s, 5) && meshLinkSentDirect(s, 6 + kMeshLinkDirectTxSlots - 1), "L6 ring of 8");
  meshLinkNoteDirectSend(s, 0);
  CHECK(!meshLinkSentDirect(s, 0), "L6 msgId 0 is never recorded");
}

// L7: back-off ceiling 10 s (E8).
void l7Backoff() {
  CHECK(meshHeartbeatMinInterval(0) == 0 && meshHeartbeatMinInterval(29999) == 0, "L7 full rate below 30 s");
  CHECK(meshHeartbeatMinInterval(30000) == 10000, "L7 10 s at 30 s");
  CHECK(meshHeartbeatMinInterval(3600000) == 10000, "L7 10 s after an hour (was 30 s after 5 min)");
}

// L8: one event per peer per 30 s; the state at the end of the window.
void l8Events() {
  MeshEventLatch l{};
  CHECK(meshEventStep(l, false, 1000) == MeshPeerEvent::None, "L8 never online: no offline event");
  CHECK(meshEventStep(l, true, 2000) == MeshPeerEvent::Online, "L8 online");
  CHECK(meshEventStep(l, true, 3000) == MeshPeerEvent::None, "L8 once");
  CHECK(meshEventStep(l, false, 10000) == MeshPeerEvent::None, "L8 offline inside the window waits");
  CHECK(meshEventStep(l, true, 20000) == MeshPeerEvent::None, "L8 back online inside the window: nothing");
  CHECK(meshEventStep(l, true, 40000) == MeshPeerEvent::None, "L8 still online after the window: nothing");
  CHECK(meshEventStep(l, false, 45000) == MeshPeerEvent::Offline, "L8 offline after the window");
  CHECK(meshEventStep(l, true, 50000) == MeshPeerEvent::None, "L8 online 5 s later waits");
  CHECK(meshEventStep(l, true, 75000) == MeshPeerEvent::Online, "L8 posted when the window ends");
}

// L9: the peer state from the link and the session.
void l9State() {
  CHECK(espnowPeerStateOf(MeshLink::TwoWay, true) == EspNowPeerState::Online, "L9 two-way + session = online");
  CHECK(espnowPeerStateOf(MeshLink::TwoWay, false) == EspNowPeerState::Connecting, "L9 no session = connecting");
  CHECK(espnowPeerStateOf(MeshLink::OneWay, true) == EspNowPeerState::Connecting,
        "L9 one-way with a session = connecting");
  CHECK(espnowPeerStateOf(MeshLink::None, true) == EspNowPeerState::Offline, "L9 not heard = offline");
  CHECK(std::strcmp(meshLinkName(MeshLink::OneWay), "oneWay") == 0 &&
            std::strcmp(meshLinkName(MeshLink::TwoWay), "twoWay") == 0 &&
            std::strcmp(meshLinkName(MeshLink::None), "none") == 0,
        "L9 link words");
}

// ---- Two boards ------------------------------------------------------------
//
// Each board keeps, for the other: the link state, the heard clock, the
// heartbeat back-off clock (hbLastHeardMs, hbLastSentMs; the meta slot) and an
// event latch. A heartbeat tick runs every 5 s; a heartbeat is sealed (both
// hold a session), so its echo is two-way evidence. `hears[x]` says whether
// board x's radio receives the other's frames.
struct Board {
  MeshLinkState link;
  uint32_t lastHeardMs = 0;
  uint32_t hbLastHeardMs = 0;  // seeded at slot birth
  uint32_t hbLastSentMs = 0;
  MeshEventLatch events{};
  uint32_t nextId = 1;
  int linkChanges = 0;
  MeshLink lastLink = MeshLink::None;
  std::vector<int> eventsPosted;
  Board() { std::memset(&link, 0, sizeof(link)); }
};

struct Sim {
  Board b[2];
  bool hears[2] = {true, true};
  uint32_t now;
  int heartbeatsSent[2] = {0, 0};
  std::vector<uint32_t> heardAt[2];  // when board x heard a heartbeat from the other
  int dropNextFrom = -1;             // drop the next heartbeat sent by this board (one loss)
  uint32_t seconds = 0;              // simulated seconds (the heartbeat tick is every 5th)

  explicit Sim(uint32_t start) : now(start) {
    for (auto& x : b) x.hbLastHeardMs = start;
  }

  void sendHeartbeat(int from) {
    Board& s = b[from];
    Board& r = b[1 - from];
    const uint32_t id = (uint32_t)(from + 1) * 1000000u + s.nextId++;
    const uint32_t echo = meshLinkEchoFor(s.link, now);
    meshLinkNoteHeartbeatSent(s.link, id, now);
    heartbeatsSent[from]++;
    if (dropNextFrom == from) {
      dropNextFrom = -1;
      return;
    }
    if (!hears[1 - from]) return;
    // Received by the other board: heard clock, echo memory, evidence.
    r.lastHeardMs = now;
    r.hbLastHeardMs = now;
    heardAt[1 - from].push_back(now);
    meshLinkNoteHeartbeatHeard(r.link, id, -60, 0, now);
    if (meshLinkEchoMatches(r.link, echo, now)) meshLinkNoteTwoWay(r.link, echo, r.lastHeardMs, now);
  }

  void tick() {
    if ((++seconds % 5) == 0) {
      for (int x = 0; x < 2; x++) {
        Board& s = b[x];
        const uint32_t silent = s.hbLastHeardMs ? now - s.hbLastHeardMs : 0;
        const uint32_t iv = meshHeartbeatMinInterval(silent);
        if (iv != 0 && now - s.hbLastSentMs < iv) continue;
        s.hbLastSentMs = now;
        sendHeartbeat(x);
      }
    }
    for (int x = 0; x < 2; x++) {
      Board& s = b[x];
      const MeshLink link = meshLinkSettle(s.link, s.lastHeardMs, now);
      if (link != s.lastLink) {
        s.linkChanges++;
        s.lastLink = link;
      }
      const bool online = espnowPeerStateOf(link, true) == EspNowPeerState::Online;
      const MeshPeerEvent ev = meshEventStep(s.events, online, now);
      if (ev != MeshPeerEvent::None) s.eventsPosted.push_back((int)ev);
    }
  }
  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 1000) {
      now += 1000;
      tick();
    }
  }
};

// L10: both ways: online within two heartbeat rounds (plus the first, which
// has nothing to echo yet), one online event each, and stays.
void l10BothWays(uint32_t start) {
  Sim s(start);
  s.run(16000);
  CHECK(s.b[0].lastLink == MeshLink::TwoWay && s.b[1].lastLink == MeshLink::TwoWay,
        "L10 [%08x] both two-way within 16 s", start);
  s.run(600000);
  CHECK(s.b[0].linkChanges == 2 && s.b[1].linkChanges == 2, "L10 [%08x] none -> one-way -> two-way, then steady",
        start);
  CHECK(s.b[0].eventsPosted == std::vector<int>{1} && s.b[1].eventsPosted == std::vector<int>{1},
        "L10 [%08x] one online event each", start);
  CHECK(s.heartbeatsSent[0] >= 120 && s.heartbeatsSent[1] >= 120, "L10 [%08x] full 5 s rate", start);
}

// L11: the one-way case. B (board 1) cannot hear A (board 0). For 10 minutes A
// shows B connecting (one-way), B shows A offline; neither flaps, no event.
// B never hears A, so it backs off to the 10 s ceiling; A hears B every 10 s
// (inside its 30 s window) and stays at the full rate toward B.
void l11OneWay(uint32_t start) {
  Sim s(start);
  s.hears[1] = false;  // B's radio does not receive A
  s.run(600000);
  CHECK(s.b[0].lastLink == MeshLink::OneWay && s.b[0].linkChanges == 1,
        "L11 [%08x] A: one-way after the first heartbeat, never changes (%d changes)", start, s.b[0].linkChanges);
  CHECK(s.b[1].lastLink == MeshLink::None && s.b[1].linkChanges == 0, "L11 [%08x] B: none throughout", start);
  CHECK(espnowPeerStateOf(s.b[0].lastLink, true) == EspNowPeerState::Connecting &&
            espnowPeerStateOf(s.b[1].lastLink, true) == EspNowPeerState::Offline,
        "L11 [%08x] A shows B connecting, B shows A offline", start);
  CHECK(s.b[0].eventsPosted.empty() && s.b[1].eventsPosted.empty(), "L11 [%08x] no event in 10 min", start);
  uint32_t maxGap = 0;
  for (size_t i = 1; i < s.heardAt[0].size(); i++) {
    const uint32_t g = s.heardAt[0][i] - s.heardAt[0][i - 1];
    if (g > maxGap) maxGap = g;
  }
  CHECK(maxGap <= 10000, "L11 [%08x] A hears B at least every 10 s (max gap %u)", start, (unsigned)maxGap);
  CHECK(s.heartbeatsSent[1] <= 600000 / 10000 + 6, "L11 [%08x] B backs off to 10 s (%d sent)", start,
        s.heartbeatsSent[1]);
  CHECK(s.heartbeatsSent[0] >= 600000 / 5000 - 1, "L11 [%08x] A keeps the 5 s rate (%d sent)", start,
        s.heartbeatsSent[0]);
  // One lost heartbeat of B's 10 s stream: a 20 s gap, still inside A's window.
  s.dropNextFrom = 1;
  s.run(60000);
  CHECK(s.b[0].lastLink == MeshLink::OneWay && s.b[0].linkChanges == 1,
        "L11 [%08x] one lost heartbeat does not drop A's view of B", start);
  // The mutant: the pre-1.0 back-off (30 s after 5 min) let A's view of B fall
  // to none between heartbeats: this suite would see changes.
}

// L12: a link flapping every 5 s for 10 minutes: events at most one per 30 s.
void l12Flap() {
  MeshEventLatch l{};
  int posted = 0;
  uint32_t lastPost = 0;
  bool spacingOk = true;
  for (uint32_t t = 1000; t <= 601000; t += 1000) {
    const bool online = ((t / 5000) % 2) == 0;
    if (meshEventStep(l, online, t) != MeshPeerEvent::None) {
      if (posted && t - lastPost < kMeshEventMinIntervalMs) spacingOk = false;
      lastPost = t;
      posted++;
    }
  }
  CHECK(spacingOk && posted <= 600 / 30 + 1, "L12 flapping link: %d events in 10 min, >= 30 s apart", posted);
}

}  // namespace

int main() {
  for (uint32_t t0 : {1000u, 0xFFFFF000u}) {
    l1Heard(t0);
    l2Hysteresis(t0);
  }
  l3Cap();
  l4Wrap();
  l5Echo();
  l6Direct();
  l7Backoff();
  l8Events();
  l9State();
  for (uint32_t start : {1000u, 0xFFFF0000u}) {
    l10BothWays(start);
    l11OneWay(start);
  }
  l12Flap();
  if (gFailures) {
    std::printf("espnow_link_core_tests: %d failure(s) of %d checks\n", gFailures, gChecks);
    return 1;
  }
  std::printf("espnow_link_core_tests: all %d checks passed\n", gChecks);
  return 0;
}
