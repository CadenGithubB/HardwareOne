// Mesh workers follow their master's channel (System_ESPNow_ChannelFollowCore.h,
// the production dependency-free header; spec
// hw1-work/design/espnow-channel-follow-2026-11-19/SPEC.md section 1.4).
//
// A rig plays espnow_task: it ticks the core every lap, performs the sweep
// switches it asks for (reporting them back through applied()), counts the
// probes and plays the master's answers. F1-F23 follow the spec's case list;
// F21-F23 are extra (stale stamps, a resumed sweep after a leak, the zero
// default state); F24-F33 cover the 2026-11-19 review fixes and the mutants
// the first suite let through. F30 is bench check B8 (ESP-NOW 1.0 step 8): a
// master that cannot hear its worker backs off to the 10 s ceiling, so the
// worker's 30 s silence clock never runs out.
#include "System_ESPNow_ChannelFollowCore.h"
#include "System_ESPNow_LinkCore.h"  // meshHeartbeatMinInterval: the master's back-off (B8)

#include <cstdio>
#include <cstring>
#include <functional>
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

using namespace espnowfollow;

static_assert(sizeof(Follow) <= 48, "Follow must stay small fixed-size state (PSRAM, no heap)");

namespace {

constexpr uint32_t kLap = 10;  // espnow_task laps

struct Rig {
  Follow f;
  Inputs in;
  uint32_t now;
  uint8_t radio = 0;  // the channel the radio is on (what the caller's applies did)
  Out last;
  std::vector<uint8_t> switches;      // sweep switches, in order
  std::vector<uint32_t> switchAt;     // when applied() reported each one
  std::vector<uint8_t> probeCh;       // radio channel at each probe
  std::vector<uint32_t> probeAt;
  std::vector<uint8_t> persisted;
  bool refuse[16] = {};    // applied(ok=false) for these channels
  bool noReport[16] = {};  // never call applied() for these channels
  bool rangeViolation = false;
  uint32_t switchCost = 0;            // ms a sweep switch takes (applied() is told now + this)
  int settles = 0;                    // Out.settle seen
  std::function<void(Rig&)> onProbe;  // the master's reaction to a probe

  Rig(uint8_t saved, uint32_t start, Role role = Role::Worker) : now(start) {
    in.role = role;
    in.nowMs = now;
    f.begin(saved, now);
  }

  Out step() {
    in.nowMs = now;
    const Out o = f.tick(in);
    last = o;
    if (o.persistMasterChannel) persisted.push_back(o.persistMasterChannel);
    if (o.channel != 0) {
      const bool pinnedOut = o.phase == Phase::Pinned;
      if (!validChannel(o.channel) || (!pinnedOut && o.channel > effectiveMaxChannel(in.maxChannel)))
        rangeViolation = true;
    }
    if (o.settle) settles++;
    if (o.sweepSwitch) {
      switches.push_back(o.channel);
      if (!noReport[o.channel]) {
        const bool ok = !refuse[o.channel];
        if (ok) radio = o.channel;
        switchAt.push_back(now + switchCost);
        f.applied(now + switchCost, o.channel, ok);
      }
    } else if (validChannel(o.channel) && !in.joined) {
      radio = o.channel;  // the caller's full apply
    } else if (in.joined && validChannel(in.staChannel)) {
      radio = in.staChannel;
    }
    if (o.sendProbe) {
      probeCh.push_back(radio);
      probeAt.push_back(now);
      if (onProbe) onProbe(*this);
    }
    return o;
  }
  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += kLap) {
      now += kLap;
      step();
    }
  }
  // Runs until the phase is `p`; returns the time it took, or UINT32_MAX.
  uint32_t until(Phase p, uint32_t maxMs) {
    for (uint32_t t = kLap; t <= maxMs; t += kLap) {
      now += kLap;
      if (step().phase == p) return t;
    }
    return UINT32_MAX;
  }
  void hear(uint8_t rank, bool sealed, uint8_t rx, uint8_t chanInfo, int8_t rssi = -50, bool heartbeat = true) {
    Heard h;
    h.rank = rank;
    h.sealed = sealed;
    h.rxChannel = rx;
    h.chanInfo = chanInfo;
    h.rssi = rssi;
    h.heartbeat = heartbeat;
    f.heard(now, h);
  }
};

std::vector<uint8_t> order(uint8_t saved, uint8_t maxCh) {
  std::vector<uint8_t> v;
  for (uint8_t i = 0;; i++) {
    const uint8_t c = sweepChannelAt(i, saved, maxCh);
    if (!c) break;
    v.push_back(c);
  }
  return v;
}

// ---------------------------------------------------------------------------
void f1ChanInfo() {
  for (uint8_t ch = 1; ch <= 13; ch++) {
    const uint8_t p = packChanInfo(ch, false, false);
    CHECK(chanOf(p) == ch && !isProbe(p) && !isAnswer(p), "F1 channel %u round-trips", ch);
    CHECK(chanOf(packChanInfo(ch, true, false)) == ch && isProbe(packChanInfo(ch, true, false)) &&
              !isAnswer(packChanInfo(ch, true, false)),
          "F1 probe bit on %u", ch);
    CHECK(chanOf(packChanInfo(ch, false, true)) == ch && isAnswer(packChanInfo(ch, false, true)) &&
              !isProbe(packChanInfo(ch, false, true)),
          "F1 answer bit on %u", ch);
  }
  CHECK(packChanInfo(0, false, false) == 0 && packChanInfo(14, false, false) == 0 && packChanInfo(255, false, false) == 0,
        "F1 channels outside 1..13 pack as not stated");
  CHECK(chanOf(0) == 0 && chanOf(14) == 0 && chanOf(15) == 0, "F1 0, 14, 15 read as not stated");
  CHECK(chanOf(uint8_t(14 | kProbeBit)) == 0 && isProbe(uint8_t(14 | kProbeBit)), "F1 bits independent of the channel");
  CHECK(chanOf(0xC6) == 6 && !isProbe(0xC6) && !isAnswer(0xC6), "F1 reserved bits 0xC0 are ignored");
  CHECK((packChanInfo(13, true, true) & 0xC0) == 0, "F1 reserved bits are sent 0");
}

void f2SweepOrder() {
  const std::vector<uint8_t> a = order(6, 13);
  const std::vector<uint8_t> wantA = {6, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13};
  CHECK(a == wantA && sweepLength(6, 13) == 13, "F2 saved 6, max 13: 6 first, then 1..13 without 6");
  std::vector<uint8_t> all13;
  for (uint8_t c = 1; c <= 13; c++) all13.push_back(c);
  CHECK(order(0, 13) == all13, "F2 saved 0: 1..13");
  std::vector<uint8_t> all11(all13.begin(), all13.begin() + 11);
  CHECK(order(12, 11) == all11 && sweepLength(12, 11) == 11, "F2 saved 12 with max 11: 1..11");
  const std::vector<uint8_t> wantB = {11, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  CHECK(order(11, 11) == wantB, "F2 saved 11, max 11: 11 first, no duplicate");
  const std::vector<uint8_t> wantC = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
  CHECK(order(1, 11) == wantC, "F2 saved 1: no duplicate of 1");
  CHECK(order(0, 0) == all11 && order(0, 14) == all11, "F2 a max outside 1..13 reads as 11");
}

void f3Gaps() {
  const uint32_t want[6] = {30000, 60000, 120000, 300000, 300000, 300000};
  for (uint8_t n = 1; n <= 6; n++) CHECK(gapAfter(n) == want[n - 1], "F3 gap after %u failures", n);
  CHECK(gapAfter(0) == 30000 && gapAfter(255) == 300000, "F3 gap edges");
}

// F4: a fresh worker that never heard its master.
void f4FirstSweep(uint32_t start) {
  Rig r(6, start);
  r.in.maxChannel = 13;
  r.run(29990);
  CHECK(r.switches.empty() && r.last.phase == Phase::Following && r.last.channel == 6,
        "F4 [%08x] no sweep before 30 s, on the saved channel", start);
  r.run(kLap);
  CHECK(r.switches.size() == 1 && r.switches[0] == 6 && r.last.phase == Phase::Sweeping,
        "F4 [%08x] first sweep at 30 s, saved channel first", start);
  CHECK(!r.rangeViolation, "F4 [%08x] range", start);
}

// F5: full failed sweeps and the back-off.
void f5Backoff(uint32_t start) {
  Rig r(6, start);
  r.in.maxChannel = 13;
  r.in.dwellMs = 150;
  CHECK(r.until(Phase::Sweeping, 31000) != UINT32_MAX, "F5 [%08x] the sweep starts", start);
  const uint32_t t = r.until(Phase::Waiting, 10000);
  CHECK(t != UINT32_MAX, "F5 [%08x] the sweep ends", start);
  const std::vector<uint8_t> want = {6, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 13};
  CHECK(r.switches == want, "F5 [%08x] 13 dwells in sweep order", start);
  CHECK(r.probeCh == want, "F5 [%08x] one probe per dwell, on the dwell channel", start);
  bool dwellOk = r.switchAt.size() == 13;
  for (size_t i = 1; dwellOk && i < r.switchAt.size(); i++) {
    const uint32_t d = r.switchAt[i] - r.switchAt[i - 1];
    dwellOk = d >= 150 && d <= 150 + 2 * kLap;
  }
  CHECK(dwellOk, "F5 [%08x] each dwell lasts dwellMs after the switch reported", start);
  CHECK(r.f.failures() == 1 && r.last.hold == Hold::Gap && r.last.channel == 6,
        "F5 [%08x] Waiting, back on the saved channel, one failure", start);
  CHECK(r.f.nextSweepInMs(r.now) == 30000, "F5 [%08x] next sweep in 30 s", start);
  CHECK(r.persisted.empty(), "F5 [%08x] a failed sweep saves nothing", start);

  const uint32_t wantGap[5] = {30000, 60000, 120000, 300000, 300000};
  for (int i = 0; i < 5; i++) {
    const uint32_t g = r.until(Phase::Sweeping, 400000);
    CHECK(g == wantGap[i], "F5 [%08x] gap %d is %u ms (got %u)", start, i + 1, (unsigned)wantGap[i], (unsigned)g);
    CHECK(r.until(Phase::Waiting, 10000) != UINT32_MAX, "F5 [%08x] sweep %d ends", start, i + 2);
    CHECK(r.f.failures() == i + 2, "F5 [%08x] failures %d", start, i + 2);
  }
  CHECK(!r.rangeViolation, "F5 [%08x] range", start);
}

// F6: success via a sealed answer that names another channel than the dwell.
void f6SealedSuccess() {
  Rig r(11, 1000);
  r.in.maxChannel = 13;
  r.in.sessionUp = true;
  r.run(30000);
  CHECK(r.until(Phase::Waiting, 10000) != UINT32_MAX && r.f.failures() == 1, "F6 one failed sweep first");
  // The master is on 6; the worker hears its sealed answer while dwelling on 5.
  r.onProbe = [](Rig& g) {
    if (g.radio == 5) g.hear(1, true, 5, packChanInfo(6, false, true));
  };
  CHECK(r.until(Phase::Following, 400000) != UINT32_MAX, "F6 the sealed answer locks");
  CHECK(r.f.masterChannel() == 6 && r.f.failures() == 0, "F6 moves to 6, failures reset");
  r.run(kLap);
  CHECK(r.last.channel == 6 && !r.last.sweepSwitch && r.radio == 6, "F6 the full apply goes to 6");
  r.run(1000);
  CHECK(r.persisted.size() == 1 && r.persisted[0] == 6, "F6 6 is persisted once");
}

// F7: group-signed answers.
void f7Unsealed() {
  {  // announced == rx: locks there
    Rig r(11, 1000);
    r.onProbe = [](Rig& g) {
      if (g.radio == 4) g.hear(1, false, 4, packChanInfo(4, false, true));
    };
    CHECK(r.until(Phase::Sweeping, 31000) != UINT32_MAX, "F7 sweep starts");
    CHECK(r.until(Phase::Following, 10000) != UINT32_MAX && r.f.masterChannel() == 4, "F7 unsealed ann == rx locks");
    r.run(kLap);
    CHECK(r.persisted.size() == 1 && r.persisted[0] == 4 && r.last.channel == 4, "F7 and saves it");
  }
  {  // announced != rx while following: no move, no liveness, recheck probe at most every 10 s
    Rig r(6, 1000);
    r.hear(1, true, 6, packChanInfo(6, false, false));
    for (int s = 0; s < 25; s++) {
      r.hear(1, false, 6, packChanInfo(7, false, false));
      r.run(1000);
    }
    CHECK(r.f.masterChannel() == 6 && r.last.channel == 6 && r.last.phase == Phase::Following,
          "F7 unsealed ann != rx does not move");
    CHECK(r.f.silentMs(r.now) == 25000, "F7 the mismatched frames are not liveness (%u)", (unsigned)r.f.silentMs(r.now));
    bool spaced = r.probeAt.size() == 3;
    for (size_t i = 1; spaced && i < r.probeAt.size(); i++) spaced = r.probeAt[i] - r.probeAt[i - 1] >= 10000;
    CHECK(spaced, "F7 recheck probes at most every 10 s (%zu probes)", r.probeAt.size());
    CHECK(r.persisted.empty(), "F7 nothing saved");
    // The sealed answer to the recheck moves it.
    r.hear(1, true, 6, packChanInfo(7, false, true));
    r.run(kLap);
    CHECK(r.f.masterChannel() == 7 && r.last.channel == 7 && r.persisted.size() == 1, "F7 a sealed answer moves");
  }
  {  // no session: the master moved 6 -> 7 and only its group-signed leaks reach 6. The
     // leaks do not hold off the hunt; the hunt locks on 7, where claim and arrival agree.
    Rig r(6, 1000);
    r.in.maxChannel = 13;
    r.hear(1, true, 6, packChanInfo(6, false, false));
    r.onProbe = [](Rig& g) {
      if (g.radio == 6) g.hear(1, false, 6, packChanInfo(7, false, true));
      if (g.radio == 7) g.hear(1, false, 7, packChanInfo(7, false, true));
    };
    for (int s = 0; s < 6; s++) {
      r.hear(1, false, 6, packChanInfo(7, false, false));
      r.run(s < 5 ? 5000 : 4980);
    }
    CHECK(r.switches.empty() && r.last.phase == Phase::Following, "F7 leaks: no hunt before 30 s");
    r.run(30);
    CHECK(r.last.phase == Phase::Sweeping && r.switches.size() == 1 && r.switches[0] == 6,
          "F7 leaks: the hunt starts 30 s after the last real frame");
    CHECK(r.until(Phase::Following, 10000) != UINT32_MAX && r.f.masterChannel() == 7, "F7 leaks: the hunt locks on 7");
    const std::vector<uint8_t> want = {6, 1, 2, 3, 4, 5, 7};
    CHECK(r.switches == want, "F7 leaks: the leak on 6 did not stop the sweep");
    r.run(kLap);
    CHECK(r.persisted.size() == 1 && r.persisted[0] == 7 && r.last.channel == 7, "F7 leaks: 7 saved and used");
  }
}

// F8: ACK then nothing / ACK then a trusted frame / ACK then a leak.
void f8Ack(uint32_t start) {
  {
    Rig r(6, start);
    r.onProbe = [](Rig& g) {
      if (g.radio == 3) g.f.probeAcked(g.now, 3);
    };
    CHECK(r.until(Phase::Confirming, 40000) != UINT32_MAX && r.f.sweepChannel() == 3,
          "F8 [%08x] ACK on 3 -> Confirming", start);
    const size_t sw = r.switches.size();
    r.run(11980);
    CHECK(r.last.phase == Phase::Confirming && r.last.channel == 3 && r.switches.size() == sw,
          "F8 [%08x] parked on 3 for 12 s", start);
    r.run(kLap);
    CHECK(r.last.phase == Phase::Sweeping && r.f.sweepChannel() == 4 && r.f.failures() == 0 &&
              r.switches.size() == sw + 1 && r.switches.back() == 4,
          "F8 [%08x] no trusted frame within 12 s: the sweep goes on at the next dwell, no failure", start);
    CHECK(r.until(Phase::Waiting, 10000) != UINT32_MAX && r.f.failures() == 1 && r.persisted.empty() &&
              r.last.channel == 6,
          "F8 [%08x] the sweep ends as one failure, nothing saved", start);
  }
  {
    Rig r(6, start);
    r.onProbe = [](Rig& g) {
      if (g.radio == 3) g.f.probeAcked(g.now, 3);
    };
    CHECK(r.until(Phase::Confirming, 40000) != UINT32_MAX, "F8 [%08x] Confirming", start);
    r.run(5000);
    r.hear(1, false, 3, packChanInfo(3, false, false));
    r.run(kLap);
    CHECK(r.last.phase == Phase::Following && r.f.masterChannel() == 3 && r.last.channel == 3 &&
              r.persisted.size() == 1 && r.persisted[0] == 3,
          "F8 [%08x] ACK then a trusted heartbeat: Following on 3", start);
  }
}

// F9: older master (chanInfo 0).
void f9OlderMaster() {
  {
    Rig r(6, 1000);
    r.onProbe = [](Rig& g) {
      if (g.radio == 8) g.hear(1, false, 8, 0);
    };
    CHECK(r.until(Phase::Sweeping, 31000) != UINT32_MAX, "F9 sweep starts");
    CHECK(r.until(Phase::Following, 10000) != UINT32_MAX && r.f.masterChannel() == 8, "F9 ann 0 in a sweep locks on rx");
    r.run(kLap);
    CHECK(r.persisted.size() == 1 && r.persisted[0] == 8, "F9 and saves rx");
  }
  {
    Rig r(6, 1000);
    r.run(5000);
    r.hear(1, false, 6, 0);
    r.hear(1, true, 9, 0);
    r.run(1000);
    CHECK(r.f.masterChannel() == 6 && r.persisted.empty() && r.last.channel == 6 && r.f.everHeard(),
          "F9 ann 0 outside a sweep: liveness only");
    r.run(25000);
    CHECK(r.switches.empty(), "F9 the liveness holds off the hunt");
  }
}

// F10: Wi-Fi wins.
void f10Joined() {
  Rig r(6, 1000);
  r.hear(1, true, 6, packChanInfo(6, false, false));
  r.in.joined = true;
  r.in.staChannel = 6;
  r.run(600000);
  CHECK(r.last.phase == Phase::Joined && r.last.channel == 0 && r.switches.empty() && r.probeAt.empty(),
        "F10 joined on the master's channel: Joined, no hunt however long silent");
  r.in.staChannel = 11;
  r.run(1000);
  CHECK(r.last.phase == Phase::WifiPaused && r.last.channel == 0 && r.switches.empty(), "F10 joined elsewhere: WifiPaused");
  r.hear(1, true, 11, packChanInfo(6, false, false));  // bookkeeping only
  r.run(kLap);
  CHECK(r.last.phase == Phase::WifiPaused && r.f.masterChannel() == 6, "F10 paused stays paused");
  r.run(40000);
  r.in.joined = false;
  r.in.staChannel = 0;
  r.run(kLap);
  CHECK(r.last.phase == Phase::Sweeping && r.switches.size() == 1 && r.switches[0] == 6,
        "F10 unjoin after > 30 s silence: the hunt starts at once");

  Rig q(6, 1000);
  q.in.joined = true;
  q.in.staChannel = 11;
  q.run(5000);
  q.hear(1, true, 11, packChanInfo(6, false, false));
  q.run(10000);
  q.in.joined = false;
  q.in.staChannel = 0;
  q.run(kLap);
  CHECK(q.last.phase == Phase::Following && q.last.channel == 6, "F10 unjoin after 10 s: back on the master's channel");
  q.run(19980);
  CHECK(q.switches.empty(), "F10 the silence clock counts across the join (not before 30 s)");
  q.run(30);
  CHECK(q.switches.size() == 1, "F10 hunt 30 s after the last frame");
}

// F11: pinned.
void f11Pinned() {
  Rig r(6, 1000);
  r.in.pinned = 11;
  r.run(600000);
  CHECK(r.last.phase == Phase::Pinned && r.last.channel == 11 && r.switches.empty() && r.probeAt.empty(),
        "F11 pinned 11: channel 11, never hunts");
  r.hear(1, true, 11, packChanInfo(3, false, false));
  r.run(kLap);
  CHECK(r.f.masterChannel() == 3 && r.last.channel == 11 && r.persisted.size() == 1 && r.persisted[0] == 3,
        "F11 announcement remembered, no move");
  r.in.joined = true;
  r.in.staChannel = 1;
  r.run(kLap);
  CHECK(r.last.phase == Phase::Pinned && r.last.channel == 0, "F11 pinned and joined: the AP decides");
  r.in.joined = false;
  r.in.pinned = 14;
  r.run(kLap);
  CHECK(r.last.phase != Phase::Pinned, "F11 a pin outside 1..13 reads as auto");
}

// F12: busy.
void f12Busy() {
  Rig r(6, 1000);
  r.in.busy = true;
  r.run(30000);
  CHECK(r.switches.empty() && r.last.phase == Phase::Waiting && r.last.hold == Hold::Busy, "F12 busy holds the start");
  r.run(29980);
  CHECK(r.switches.empty(), "F12 still held before 60 s of busy");
  r.run(30);
  CHECK(r.last.phase == Phase::Sweeping, "F12 busy longer than 60 s no longer holds");
  r.in.busy = false;
  CHECK(r.until(Phase::Waiting, 10000) != UINT32_MAX && r.f.failures() == 1, "F12 sweep 1 fails");
  CHECK(r.until(Phase::Sweeping, 40000) != UINT32_MAX, "F12 sweep 2 starts");
  r.run(400);
  r.in.busy = true;  // a new busy spell mid-sweep
  r.run(kLap);
  CHECK(r.last.phase == Phase::Waiting && r.last.hold == Hold::Busy && r.f.failures() == 1 &&
            r.f.nextSweepInMs(r.now) == 30000 && r.last.channel == 6,
        "F12 busy mid-sweep aborts without a failure");
}

// F13: wifiBusy and radioAway hold and abort the same way (no cap).
void f13Holds() {
  for (int k = 0; k < 2; k++) {
    Rig r(6, 1000);
    bool& flag = k == 0 ? r.in.wifiBusy : r.in.radioAway;
    const Hold want = k == 0 ? Hold::WifiBusy : Hold::RadioAway;
    flag = true;
    r.run(200000);
    CHECK(r.switches.empty() && r.last.hold == want, "F13 %s holds the start", k ? "radioAway" : "wifiBusy");
    flag = false;
    r.run(kLap);
    CHECK(r.last.phase == Phase::Sweeping, "F13 released: sweep at once");
    r.run(300);
    flag = true;
    r.run(kLap);
    CHECK(r.last.phase == Phase::Waiting && r.last.hold == want && r.f.failures() == 0,
          "F13 %s mid-sweep aborts without a failure", k ? "radioAway" : "wifiBusy");
  }
}

// F14: role Master / None.
void f14Roles() {
  for (Role role : {Role::Master, Role::None}) {
    Rig r(6, 1000, role);
    r.in.sessionUp = true;
    r.hear(1, true, 6, packChanInfo(3, false, false));
    r.run(600000);
    CHECK(r.last.phase == Phase::Off && r.last.channel == 0 && !r.last.wifiRetryHeld && r.switches.empty() &&
              r.probeAt.empty() && r.persisted.empty(),
          "F14 %s: Off, no channel, no hunt, nothing persisted", role == Role::Master ? "Master" : "None");
  }
}

// F15: Wi-Fi retry hold: session up and a sealed frame from the primary within 45 s.
void f15WifiHold() {
  Rig r(6, 1000);
  r.in.sessionUp = true;
  r.run(kLap);
  CHECK(!r.last.wifiRetryHeld, "F15 session but never heard: not held");
  r.hear(1, false, 6, packChanInfo(6, false, false));
  r.run(kLap);
  CHECK(!r.last.wifiRetryHeld, "F15 group-signed frames alone do not hold (a replay cannot hold Wi-Fi off)");
  CHECK(r.probeAt.size() == 1, "F15 the hold is wanted: a probe asks for a sealed frame at once");
  r.hear(2, true, 6, packChanInfo(6, false, false));
  r.run(kLap);
  CHECK(!r.last.wifiRetryHeld, "F15 the backup's sealed frame does not hold");
  r.in.sessionUp = false;
  r.hear(1, true, 6, packChanInfo(6, false, false));
  const uint32_t t0 = r.now;
  r.run(kLap);
  CHECK(!r.last.wifiRetryHeld, "F15 sealed but no session: not held");
  r.in.sessionUp = true;
  r.run(kLap);
  CHECK(r.last.wifiRetryHeld, "F15 session and sealed: held");
  // Group-signed heartbeats every 5 s keep the primary live; no sealed answer comes.
  for (int i = 0; i < 8; i++) {
    r.run(5000);
    r.hear(1, false, 6, packChanInfo(6, false, false));
  }
  r.run(4960);
  CHECK(r.last.wifiRetryHeld, "F15 still held 44.98 s after the last sealed frame (one probe lost)");
  r.run(20);
  CHECK(!r.last.wifiRetryHeld, "F15 released 45 s after the last sealed frame, session still ACTIVE");
  CHECK(r.probeAt.size() >= 2 && r.probeAt[1] - t0 >= 20000 && r.probeAt[1] - t0 <= 20000 + kLap,
        "F15 a keep-alive probe 20 s after the last sealed frame");
  CHECK(r.probeAt.size() >= 3 && r.probeAt[2] - r.probeAt[1] >= 10000 && r.probeAt[2] - r.probeAt[1] <= 10000 + kLap,
        "F15 no answer: the keep-alive probe repeats 10 s later, inside the hold");

  // The primary silent for 30 s releases the hold at once, sealed frame or not.
  Rig q(6, 1000);
  q.in.sessionUp = true;
  q.hear(1, true, 6, packChanInfo(6, false, false));
  q.run(29990);
  CHECK(q.last.wifiRetryHeld, "F15 held 29.99 s after the last (sealed) frame");
  q.run(20);
  CHECK(!q.last.wifiRetryHeld, "F15 the primary silent for 30 s: released");

  // The master answers the keep-alive sealed: held for minutes over group-signed heartbeats.
  Rig k(6, 1000);
  k.in.sessionUp = true;
  k.onProbe = [](Rig& g) { g.hear(1, true, g.radio, packChanInfo(6, false, true)); };
  k.hear(1, true, 6, packChanInfo(6, false, false));
  bool allHeld = true;
  for (int i = 0; i < 24; i++) {
    k.hear(1, false, 6, packChanInfo(6, false, false));
    for (uint32_t t = 0; t < 5000; t += kLap) {
      k.now += kLap;
      k.step();
      allHeld = allHeld && k.last.wifiRetryHeld;
    }
  }
  CHECK(allHeld && k.probeAt.size() >= 5 && k.probeAt.size() <= 7,
        "F15 keep-alive: held for 120 s, one probe per ~20 s (%zu)", k.probeAt.size());
}

// F16: backup rank 2.
void f16Backup() {
  Rig r(6, 1000);
  r.hear(1, true, 6, packChanInfo(6, false, false));
  r.run(5000);
  r.hear(2, true, 6, packChanInfo(9, false, false));
  r.run(kLap);
  CHECK(r.f.masterChannel() == 6 && r.last.channel == 6, "F16 backup's channel ignored while the primary is heard");
  for (int i = 0; i < 12; i++) {  // the backup keeps the worker alive, the primary is gone
    r.hear(2, false, 6, packChanInfo(6, false, false));
    r.run(5000);
  }
  CHECK(r.switches.empty() && r.last.phase == Phase::Following, "F16 backup frames count as liveness");
  r.hear(2, true, 6, packChanInfo(9, false, false));
  r.run(kLap);
  CHECK(r.f.masterChannel() == 9 && r.last.channel == 9, "F16 adopted once the primary is silent >= 30 s");
  r.hear(1, true, 9, packChanInfo(4, false, false));
  r.run(kLap);
  CHECK(r.f.masterChannel() == 4, "F16 the primary's announcement wins again");
}

// F17: refused and unreported switches.
void f17Applied() {
  Rig r(6, 1000);
  r.refuse[1] = r.refuse[2] = true;
  r.noReport[4] = true;
  CHECK(r.until(Phase::Sweeping, 31000) != UINT32_MAX, "F17 sweep starts");
  CHECK(r.until(Phase::Waiting, 12000) != UINT32_MAX, "F17 sweep ends");
  const std::vector<uint8_t> want = {6, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11};
  CHECK(r.switches == want, "F17 every channel asked for once");
  const std::vector<uint8_t> probes = {6, 3, 5, 7, 8, 9, 10, 11};
  CHECK(r.probeCh == probes, "F17 no probe on refused (1, 2) or unreported (4) channels");
  // 1 and 2 skipped at once (one lap each); 4 skipped after 1 s.
  CHECK(r.switchAt.size() == 10, "F17 ten reports (none for 4)");
  CHECK(r.switchAt[1] - r.switchAt[0] >= 150 && r.switchAt[3] - r.switchAt[1] <= 3 * kLap,
        "F17 refusals skip at once");
  // switchAt: 6, 1, 2, 3, 5, ... (4 never reported): 3's dwell, then 1 s waiting for 4.
  CHECK(r.switchAt.size() == 10 && r.switchAt[4] - r.switchAt[3] >= 150 + 1000 &&
            r.switchAt[4] - r.switchAt[3] <= 150 + 1000 + 3 * kLap,
        "F17 an unreported switch is skipped after 1 s (%u)", (unsigned)(r.switchAt[4] - r.switchAt[3]));
}

// F18: saved channel above maxChannel.
void f18OutOfRange() {
  Rig r(12, 1000);
  r.in.maxChannel = 11;
  r.run(kLap);
  CHECK(r.last.channel == 0 && r.last.hold == Hold::OutOfRange && r.f.masterChannel() == 12,
        "F18 saved 12 under max 11: no target, OutOfRange");
  CHECK(r.until(Phase::Sweeping, 31000) != UINT32_MAX && r.until(Phase::Waiting, 10000) != UINT32_MAX,
        "F18 one sweep");
  std::vector<uint8_t> want;
  for (uint8_t c = 1; c <= 11; c++) want.push_back(c);
  CHECK(r.switches == want, "F18 the sweep skips 12 and 13");
  CHECK(r.last.channel == 0 && r.last.hold == Hold::OutOfRange, "F18 between sweeps still no target, OutOfRange shown");
  r.run(1000);
  CHECK(r.last.phase == Phase::Waiting && r.last.hold == Hold::OutOfRange, "F18 Waiting keeps showing OutOfRange");
  r.hear(1, true, 6, packChanInfo(13, false, false));
  r.run(kLap);
  CHECK(r.f.masterChannel() == 13 && r.last.channel == 0 && r.last.hold == Hold::OutOfRange,
        "F18 a sealed 13 is remembered, not targeted");
  r.in.maxChannel = 13;
  r.run(kLap);
  CHECK(r.last.channel == 13 && r.last.hold == Hold::None, "F18 usable once the country allows it");
  CHECK(!r.rangeViolation, "F18 nothing above maxChannel was ever chosen");
}

// F20: persist is one-shot.
void f20Persist() {
  Rig r(6, 1000);
  r.hear(1, true, 6, packChanInfo(6, false, false));
  r.run(1000);
  CHECK(r.persisted.empty(), "F20 the saved channel again: nothing persisted");
  r.hear(1, true, 6, packChanInfo(7, false, false));
  r.run(1000);
  r.hear(1, true, 7, packChanInfo(7, false, false));
  r.run(1000);
  CHECK(r.persisted.size() == 1 && r.persisted[0] == 7, "F20 7 persisted once");
}

// F21: stale stamps (ticks days apart while joined) and frames stamped after the tick.
void f21Stale() {
  Rig r(6, 1000);
  r.in.sessionUp = true;
  r.hear(1, true, 6, packChanInfo(6, false, false));
  r.in.joined = true;
  r.in.staChannel = 6;
  for (int d = 0; d < 6; d++) {  // 6 x 6 days: past 2^31 ms
    r.now += 6u * 86400u * 1000u;
    r.step();
  }
  r.in.joined = false;
  r.in.staChannel = 0;
  r.run(kLap);
  CHECK(r.last.phase == Phase::Sweeping && !r.last.wifiRetryHeld, "F21 36 days silent still reads as silent");

  Rig q(6, 1000);
  q.run(29000);
  q.f.heard(q.now + 5, Heard{1, true, 6, packChanInfo(6, false, false)});
  q.run(kLap);
  CHECK(q.last.phase == Phase::Following && q.f.silentMs(q.now) == 5, "F21 a frame stamped after the tick reads as fresh");
}

// F22: a leak during a sweep (group-signed, another channel) does not end it.
void f22Leak() {
  Rig r(11, 1000);
  r.in.maxChannel = 13;
  r.onProbe = [](Rig& g) {  // the master is on 6; on 5 its frames leak across
    if (g.radio == 5) g.hear(1, false, 5, packChanInfo(6, false, false));
    if (g.radio == 6) g.hear(1, false, 6, packChanInfo(6, false, true));
  };
  CHECK(r.until(Phase::Sweeping, 31000) != UINT32_MAX, "F22 sweep starts");
  CHECK(r.until(Phase::Following, 10000) != UINT32_MAX && r.f.masterChannel() == 6, "F22 the sweep reaches 6 and locks");
  const std::vector<uint8_t> want = {11, 1, 2, 3, 4, 5, 6};
  CHECK(r.switches == want, "F22 the leak on 5 did not stop the sweep");

  Rig c(11, 1000);  // ACK on 5 (leak), then the leaked heartbeat: resume at 6
  c.onProbe = [](Rig& g) {
    if (g.radio == 5) g.f.probeAcked(g.now, 5);
    if (g.radio == 6) g.hear(1, false, 6, packChanInfo(6, false, true));
  };
  CHECK(c.until(Phase::Sweeping, 31000) != UINT32_MAX, "F22 sweep starts (Confirming case)");
  CHECK(c.until(Phase::Confirming, 10000) != UINT32_MAX && c.f.sweepChannel() == 5, "F22 ACK on 5 parks the sweep");
  c.run(3000);
  c.hear(1, false, 5, packChanInfo(6, false, false));  // the leaked heartbeat names 6
  CHECK(c.until(Phase::Following, 10000) != UINT32_MAX && c.f.masterChannel() == 6 && c.switches.back() == 6,
        "F22 a mismatched frame while Confirming resumes the sweep");
}

// F23: the default state is all zero (EXT_RAM_BSS_ATTR is zero-filled) and
// a never-begun core is harmless.
void f23Zero() {
  static Follow z;  // zero-initialised static storage, as in PSRAM .bss
  static const unsigned char zeros[sizeof(Follow)] = {};
  CHECK(std::memcmp(&z, zeros, sizeof(Follow)) == 0, "F23 a static Follow is all zero bytes");
  Inputs in;
  in.role = Role::Worker;
  in.nowMs = 5;
  const Out o = z.tick(in);
  CHECK(o.phase == Phase::Following && o.channel == 0 && !o.sweepSwitch, "F23 a zero core starts Following, no opinion");
}


// F24: a hunt that used light switches always ends with one full apply (Out.settle),
// even when it locks on the channel the radio is already on (review 1 #1, review 2 #3).
void f24Settle() {
  Rig r(11, 1000);
  r.in.maxChannel = 13;
  r.onProbe = [](Rig& g) {
    if (g.radio == 6) g.hear(1, false, 6, packChanInfo(6, false, true));
  };
  CHECK(r.until(Phase::Sweeping, 31000) != UINT32_MAX && r.until(Phase::Following, 10000) != UINT32_MAX,
        "F24 sweep locks on 6");
  // The lock came from the answer (after the lap's tick); the first Following lap asks for the apply.
  CHECK(r.radio == 6 && r.switches.back() == 6 && r.last.settle && r.last.channel == 6 && !r.last.sweepSwitch,
        "F24 locked on the current dwell: the next lap still asks for the full apply of 6");
  r.run(1000);
  CHECK(r.settles == 1, "F24 settle is one-shot");

  Rig q(6, 1000);
  CHECK(q.until(Phase::Sweeping, 31000) != UINT32_MAX && q.until(Phase::Waiting, 10000) != UINT32_MAX,
        "F24 a failed sweep");
  CHECK(q.last.settle && q.last.channel == 6 && q.settles == 1, "F24 a failed sweep settles on the saved channel");

  Rig j(6, 1000);
  CHECK(j.until(Phase::Sweeping, 31000) != UINT32_MAX, "F24 sweep starts (join case)");
  j.run(300);
  j.in.joined = true;
  j.in.staChannel = 1;
  j.run(kLap);
  CHECK(j.last.settle && j.last.channel == 0 && j.last.phase == Phase::WifiPaused,
        "F24 a hunt cut short by a join settles with no opinion (expected decides)");

  Rig n(6, 1000);
  n.hear(1, true, 6, packChanInfo(6, false, false));
  n.run(20000);
  CHECK(n.settles == 0, "F24 no settle without a sweep");
}

// F25: abortHunt (radio mutex held, Hosted RPC stall): Waiting, no failure, back-off kept
// (review 2 #2, #10, #11).
void f25Abort() {
  Rig r(6, 1000);
  CHECK(r.until(Phase::Sweeping, 31000) != UINT32_MAX, "F25 sweep starts");
  r.run(300);
  r.f.abortHunt(r.now, Hold::RadioAway);
  CHECK(r.f.phase() == Phase::Waiting && r.f.hold() == Hold::RadioAway, "F25 abort shows its reason");
  r.run(kLap);
  CHECK(r.last.phase == Phase::Waiting && r.f.failures() == 0 && r.f.nextSweepInMs(r.now) == 30000 - kLap &&
            r.last.settle && r.last.channel == 6,
        "F25 aborted: Waiting, no failure, 30 s gap, settles on 6");
  r.f.abortHunt(r.now, Hold::WifiBusy);
  CHECK(r.f.phase() == Phase::Waiting && r.f.nextSweepInMs(r.now) == 30000 - kLap, "F25 abort outside a hunt does nothing");

  Rig q(6, 1000);
  q.in.maxChannel = 13;
  for (int i = 0; i < 4; i++) {
    CHECK(q.until(Phase::Sweeping, 400000) != UINT32_MAX && q.until(Phase::Waiting, 10000) != UINT32_MAX,
          "F25 failed sweep %d", i + 1);
  }
  CHECK(q.until(Phase::Sweeping, 400000) != UINT32_MAX, "F25 fifth sweep starts");
  q.run(300);
  q.f.abortHunt(q.now, Hold::WifiBusy);
  CHECK(q.f.failures() == 4 && q.f.nextSweepInMs(q.now) == 300000, "F25 an abort keeps the 300 s back-off (%u)",
        (unsigned)q.f.nextSweepInMs(q.now));
  q.in.wifiBusy = true;  // the same through the input hold
  q.until(Phase::Sweeping, 400000);
  q.run(kLap);
  CHECK(q.last.phase == Phase::Waiting && q.f.nextSweepInMs(q.now) == 0, "F25 wifiBusy holds the due sweep");
  q.in.wifiBusy = false;
  q.run(kLap);
  q.run(300);
  q.in.radioAway = true;
  q.run(kLap);
  CHECK(q.last.phase == Phase::Waiting && q.f.failures() == 4 && q.f.nextSweepInMs(q.now) == 300000,
        "F25 an input hold mid-sweep keeps the 300 s back-off too");
}

// F26: older masters (channel not stated): the strongest of the candidates wins,
// two more dwells after the best; a non-heartbeat sealed frame never locks; a
// backup's stronger frame does not displace the primary's (review 2 #5, mutant M9).
void f26Candidates() {
  Rig r(11, 1000);
  r.in.maxChannel = 13;
  r.onProbe = [](Rig& g) {
    if (g.radio == 5) g.hear(1, false, 5, 0, -80);
    if (g.radio == 6) g.hear(1, false, 6, 0, -40);
    if (g.radio == 7) g.hear(1, false, 7, 0, -75);
  };
  CHECK(r.until(Phase::Sweeping, 31000) != UINT32_MAX, "F26 sweep starts");
  CHECK(r.until(Phase::Following, 10000) != UINT32_MAX && r.f.masterChannel() == 6, "F26 locks on the strongest (6)");
  const std::vector<uint8_t> want = {11, 1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(r.switches == want, "F26 two more dwells after the best candidate");
  r.run(kLap);
  CHECK(r.persisted.size() == 1 && r.persisted[0] == 6 && r.last.channel == 6 && r.settles == 1, "F26 6 saved and settled once");

  Rig e(11, 1000);  // a candidate near the end: the sweep end locks it
  e.in.maxChannel = 13;
  e.onProbe = [](Rig& g) {
    if (g.radio == 13) g.hear(1, false, 13, 0, -60);
  };
  CHECK(e.until(Phase::Sweeping, 31000) != UINT32_MAX && e.until(Phase::Following, 10000) != UINT32_MAX &&
            e.f.masterChannel() == 13 && e.f.failures() == 0,
        "F26 a candidate on the last dwell locks at the sweep end");

  Rig s(11, 1000);
  s.in.maxChannel = 13;
  s.onProbe = [](Rig& g) {
    if (g.radio == 5) g.hear(1, true, 5, 0, -40, false);
  };
  CHECK(s.until(Phase::Sweeping, 31000) != UINT32_MAX && s.until(Phase::Waiting, 10000) != UINT32_MAX &&
            s.f.masterChannel() == 11 && s.f.failures() == 1,
        "F26 a sealed non-heartbeat frame during a hunt never locks");

  Rig b(11, 1000);
  b.in.maxChannel = 13;
  b.onProbe = [](Rig& g) {
    if (g.radio == 3) g.hear(1, false, 3, 0, -70);
    if (g.radio == 4) g.hear(2, false, 4, 0, -30);
  };
  CHECK(b.until(Phase::Sweeping, 31000) != UINT32_MAX && b.until(Phase::Following, 10000) != UINT32_MAX &&
            b.f.masterChannel() == 3,
        "F26 the backup's stronger frame does not replace the heard primary's candidate");

  Rig c(6, 1000);  // ACK, then the older master's heartbeat while Confirming: a candidate, the sweep goes on
  c.in.maxChannel = 13;
  c.onProbe = [](Rig& g) {
    if (g.radio == 3) g.f.probeAcked(g.now, 3);
  };
  CHECK(c.until(Phase::Confirming, 40000) != UINT32_MAX, "F26 Confirming on 3");
  c.run(4000);
  c.hear(1, false, 3, 0, -45);
  c.run(kLap);
  CHECK(c.last.phase == Phase::Sweeping && c.f.sweepChannel() == 4, "F26 an older master's frame while Confirming: on to 4");
  CHECK(c.until(Phase::Following, 10000) != UINT32_MAX && c.f.masterChannel() == 3 && c.switches.back() == 5,
        "F26 then locks on 3 after two more dwells");
}

// F27: Confirming honours the holds (mutant M2).
void f27ConfirmHolds() {
  for (int k = 0; k < 3; k++) {
    Rig r(6, 1000);
    r.onProbe = [](Rig& g) {
      if (g.radio == 3) g.f.probeAcked(g.now, 3);
    };
    CHECK(r.until(Phase::Confirming, 40000) != UINT32_MAX, "F27 Confirming (%d)", k);
    r.run(1000);
    const Hold want = k == 0 ? Hold::RadioAway : k == 1 ? Hold::WifiBusy : Hold::Busy;
    (k == 0 ? r.in.radioAway : k == 1 ? r.in.wifiBusy : r.in.busy) = true;
    r.run(kLap);
    CHECK(r.last.phase == Phase::Waiting && r.last.hold == want && r.f.failures() == 0 &&
              r.f.nextSweepInMs(r.now) == 30000 && r.last.channel == 6,
          "F27 hold %d while Confirming: Waiting, 30 s gap, no failure", k);
  }
}

// F28: the dwell counts from the reported end of the switch (mutant M1), and a slow
// lap after the switch sends the probe without ending the dwell (mutant M5).
void f28Timing() {
  Rig r(6, 1000);
  r.switchCost = 15;
  CHECK(r.until(Phase::Sweeping, 31000) != UINT32_MAX && r.until(Phase::Waiting, 10000) != UINT32_MAX,
        "F28 sweep with 15 ms switches");
  bool dwellOk = r.switchAt.size() == 11;
  for (size_t i = 1; dwellOk && i < r.switchAt.size(); i++) {
    const uint32_t d = r.switchAt[i] - r.switchAt[i - 1];
    dwellOk = d >= 150 + 15 && d <= 150 + 15 + 2 * kLap;
  }
  CHECK(dwellOk, "F28 each dwell lasts dwellMs after the reported switch");

  Rig s(6, 1000);
  CHECK(s.until(Phase::Sweeping, 31000) != UINT32_MAX && s.switches.size() == 1, "F28 first dwell (6)");
  s.now += 250;  // one slow lap
  const Out a = s.step();
  CHECK(a.sendProbe && !a.sweepSwitch && a.channel == 6 && s.probeCh.size() == 1 && s.probeCh[0] == 6,
        "F28 the slow lap sends the probe on 6 and does not end the dwell");
  const Out b = s.step();
  CHECK(b.sweepSwitch && b.channel == 1, "F28 the next lap moves on");
}

// F29: small edges: the first recheck probe goes out at once (M8); an ACK before
// this dwell's probe is ignored (M22); joined with an unknown master channel is
// Joined (M26); Waiting -> Following on a heard frame (M3).
void f29Edges() {
  Rig r(6, 1000);
  r.hear(1, false, 6, packChanInfo(7, false, false));
  r.run(kLap);
  CHECK(r.probeAt.size() == 1 && r.probeAt[0] == 1010, "F29 the first recheck probe goes out at once");

  Rig a(6, 1000);
  CHECK(a.until(Phase::Sweeping, 31000) != UINT32_MAX && a.probeAt.empty(), "F29 switched, no probe yet");
  a.f.probeAcked(a.now, 6);
  a.run(kLap);
  CHECK(a.last.phase == Phase::Sweeping && a.probeAt.size() == 1, "F29 an ACK before the probe is ignored");

  Rig j(0, 1000);
  j.in.joined = true;
  j.in.staChannel = 6;
  j.run(kLap);
  CHECK(j.last.phase == Phase::Joined, "F29 joined, master channel unknown: Joined, not paused");

  for (int k = 0; k < 2; k++) {
    Rig w(6, 1000);
    CHECK(w.until(Phase::Sweeping, 31000) != UINT32_MAX && w.until(Phase::Waiting, 10000) != UINT32_MAX,
          "F29 Waiting after a failed sweep (%d)", k);
    const size_t sw = w.switches.size();
    if (k == 0) w.hear(1, true, 6, packChanInfo(6, false, false));
    else w.hear(1, false, 6, 0);  // older master: liveness only
    w.run(kLap);
    CHECK(w.last.phase == Phase::Following && w.f.failures() == 0 && w.last.channel == 6 && w.switches.size() == sw,
          "F29 Waiting -> Following on a heard frame (%s)", k ? "older master" : "sealed");
  }
}

}  // namespace

// F30 (B8, one-way link): the master never hears the worker, so its heartbeats
// to the worker back off (meshHeartbeatMinInterval, System_ESPNow_LinkCore.h,
// 5 s tick). The worker hears every one of them (group-signed: no session on a
// one-way link) on the master's channel. With the 1.0 ceiling (10 s) the worker
// never starts a sweep in 10 minutes; with the pre-1.0 back-off (30 s after
// 5 min) it hunted once per master heartbeat.
template <typename Interval>
int b8Sweeps(uint32_t start, Interval interval) {
  Rig r(6, start);
  r.in.maxChannel = 13;
  r.in.dwellMs = 150;
  uint32_t lastSent = start;
  uint32_t lastTick = start;
  const uint32_t masterSilentSince = start;  // the master never hears the worker
  int sweeps = 0;
  Phase prev = Phase::Following;
  for (uint32_t t = 0; t < 600000; t += kLap) {
    r.now += kLap;
    // The master's heartbeat tick: "5 s since the last one", seen on a loop
    // lap, so a little over 5 s (the bench saw 30-35 s gaps at the old 30 s step).
    if (r.now - lastTick > 5000) {
      lastTick = r.now;
      const uint32_t iv = interval(r.now - masterSilentSince);
      if (iv == 0 || r.now - lastSent >= iv) {
        lastSent = r.now;
        r.hear(1, /*sealed=*/false, r.radio ? r.radio : 6, packChanInfo(6, false, false));
      }
    }
    const Out o = r.step();
    if (o.phase == Phase::Sweeping && prev != Phase::Sweeping) sweeps++;
    prev = o.phase;
  }
  return sweeps;
}

void f30OneWayB8(uint32_t start) {
  const int now10 = b8Sweeps(start, [](uint32_t silent) { return meshHeartbeatMinInterval(silent); });
  CHECK(now10 == 0, "F30 [%08x] B8: no sweep in 10 min with the 10 s ceiling (%d)", start, now10);
  const int before = b8Sweeps(start, [](uint32_t silent) -> uint32_t {
    if (silent < 30000) return 0;
    if (silent < 120000) return 10000;
    if (silent < 300000) return 20000;
    return 30000;
  });
  CHECK(before > 0, "F30 [%08x] B8: the pre-1.0 back-off made the worker hunt (%d sweeps)", start, before);
}

int main() {
  f1ChanInfo();
  f2SweepOrder();
  f3Gaps();
  f4FirstSweep(1000);
  f5Backoff(1000);
  f6SealedSuccess();
  f7Unsealed();
  f8Ack(1000);
  f9OlderMaster();
  f10Joined();
  f11Pinned();
  f12Busy();
  f13Holds();
  f14Roles();
  f15WifiHold();
  f16Backup();
  f17Applied();
  f18OutOfRange();
  // F19 wraparound: start just before the wrap so the silence, the dwells and
  // the gaps cross it.
  for (uint32_t start : {0xFFFFF000u, 0u - 30500u, 0u - 31800u}) {
    f4FirstSweep(start);
    f5Backoff(start);
    f8Ack(start);
  }
  f20Persist();
  f21Stale();
  f22Leak();
  f23Zero();
  f24Settle();
  f25Abort();
  f26Candidates();
  f27ConfirmHolds();
  f28Timing();
  f29Edges();
  f30OneWayB8(1000);
  f30OneWayB8(0xFFFF0000u);

  if (gFailures) {
    std::printf("espnow_channel_follow_tests: %d failure(s) of %d checks\n", gFailures, gChecks);
    return 1;
  }
  std::printf("espnow_channel_follow_tests: all %d checks passed\n", gChecks);
  return 0;
}
