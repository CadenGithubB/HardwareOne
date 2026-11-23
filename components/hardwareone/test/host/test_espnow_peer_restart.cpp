// The rules that end a session with a peer that restarted
// (System_ESPNow_PeerRestartCore.h), on the production header. Bench
// 2026-11-18: the P4 restarted, the XIAO still held its session with it and
// sent three messages into it; all three were lost, and the XIAO noticed only
// after two send timeouts (~10 s).
//
// Cases: a group-tagged heartbeat whose boot counter is newer (or a first one)
// closes a live session (1.0 step 8; this was the BOOT notice 31); the same
// counter, an older one (never recorded: copies of old heartbeats played in
// turn close nothing), a missing session or a session opened after the restart
// close nothing; a counter from a sealed frame becomes the reference outright
// (an erased peer is not locked out); counters compare across the 32-bit wrap;
// a counter of 0 closes nothing; only a group tag of the peer's own mesh
// counts; only a sealed counter past the last sealed one is reported as a
// restart; a forged heartbeat forces at most one close per peer per
// 10 s, shared with the hint; the device without
// the session answers at most one hint per peer per second and none for its own
// handshake or a replaced session; a hint closes only the live session it
// names, for a sequence this device sealed; the table gives up its oldest entry
// when full; and the times survive the millis() wrap.
#include "System_ESPNow_PeerRestartCore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static unsigned checks = 0;
#define CHECK(condition)                                                          \
  do {                                                                            \
    ++checks;                                                                     \
    if (!(condition)) {                                                           \
      std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);        \
      std::exit(1);                                                               \
    }                                                                             \
  } while (0)

static const uint8_t kP4[6]   = {0xFC, 0x01, 0x2C, 0xE0, 0xB6, 0x18};
static const uint8_t kXiao[6] = {0x68, 0xEE, 0x8F, 0x50, 0xE9, 0xD0};

using V = PeerBootVerdict;
using H = SessionHintVerdict;
using R = UnknownSessionReply;

static const uint32_t kOld = 60000;  // age of a session opened long before the notice

// The bench case: the XIAO restarted earlier (it knows no counter for the P4),
// holds a session with it; the P4 restarts and its notice arrives.
static void benchRestart() {
  PeerRestartTable t = {};
  CHECK(peerRestartOnBootCounter(t, kP4, 133, true, kOld, 50000) == V::DropSession);
  // The notice is recorded: the same start seen again changes nothing.
  CHECK(peerRestartOnBootCounter(t, kP4, 133, true, kOld, 70000) == V::SameBoot);
  // The next restart closes the session opened in between.
  CHECK(peerRestartOnBootCounter(t, kP4, 134, true, kOld, 200000) == V::DropSession);
}

static void nothingToClose() {
  PeerRestartTable t = {};
  CHECK(peerRestartOnBootCounter(t, kP4, 10, false, 0, 1000) == V::NoSession);
  // Recorded anyway: the session opened afterwards is not closed by a copy.
  CHECK(peerRestartOnBootCounter(t, kP4, 10, true, kOld, 90000) == V::SameBoot);
  // A session opened after the restart (younger than the notice can be) stays.
  CHECK(peerRestartOnBootCounter(t, kP4, 11, true, kPeerRestartFreshSessionMs - 1, 200000) ==
        V::FreshSession);
  CHECK(peerRestartOnBootCounter(t, kP4, 12, true, kPeerRestartFreshSessionMs, 300000) ==
        V::DropSession);
}

// An old notice re-sent by anyone in range (no key needed: the signed frame is
// captured off the air) closes nothing, and neither do two captured notices of
// different starts played in turn, for as long as they are played.
static void replayedNotice() {
  PeerRestartTable t = {};
  CHECK(peerRestartOnBootCounter(t, kP4, 40, true, kOld, 1000) == V::DropSession);
  CHECK(peerRestartOnBootCounter(t, kP4, 41, true, kOld, 100000) == V::DropSession);
  CHECK(peerRestartOnBootCounter(t, kP4, 41, true, kOld, 200000) == V::SameBoot);
  CHECK(peerRestartOnBootCounter(t, kP4, 40, true, kOld, 300000) == V::OlderBoot);
  unsigned drops = 0;
  uint32_t now = 300000;
  for (int i = 0; i < 20; ++i) {  // 40, 41, 40, 41 ... one every 11 s (past the 10 s limit)
    now += 11000;
    if (peerRestartOnBootCounter(t, kP4, (i & 1) ? 41 : 40, true, kOld, now) == V::DropSession) ++drops;
  }
  CHECK(drops == 0);  // FAILS ON the re-baselining rule: one forced handshake every 22 s
  CHECK(peerRestartFind(t, kP4)->boot == 41);
}

// A peer whose count was erased (it starts at 1 again) is not locked out. Its
// notices close nothing (a lower counter from a signed notice could be a
// copy), until a sealed frame of the live session shows its count (the bond
// heartbeat): that becomes the reference, and its next restart is recognised.
// Meanwhile the SESSION_UNKNOWN hint ends each stale session (hintRules).
static void counterReset() {
  PeerRestartTable t = {};
  CHECK(peerRestartOnBootCounter(t, kP4, 500, true, kOld, 1000) == V::DropSession);
  CHECK(peerRestartOnBootCounter(t, kP4, 1, true, kOld, 100000) == V::OlderBoot);
  CHECK(peerRestartOnBootCounter(t, kP4, 2, true, kOld, 200000) == V::OlderBoot);
  CHECK(peerRestartFind(t, kP4)->boot == 500);
  peerRestartBootSeen(t, kP4, 2, 210000);  // its heartbeat in the new session
  CHECK(peerRestartFind(t, kP4)->boot == 2);
  CHECK(peerRestartOnBootCounter(t, kP4, 3, true, kOld, 300000) == V::DropSession);
  CHECK(peerRestartOnBootCounter(t, kP4, 4, true, kOld, 400000) == V::DropSession);
  // A captured notice from before the erase reads as newer: it forces one
  // handshake, and then the reference is past it, so no copy of it or of any
  // older one closes anything again (each captured counter at most once).
  CHECK(peerRestartOnBootCounter(t, kP4, 500, true, kOld, 500000) == V::DropSession);
  CHECK(peerRestartOnBootCounter(t, kP4, 499, true, kOld, 600000) == V::OlderBoot);
  CHECK(peerRestartOnBootCounter(t, kP4, 500, true, kOld, 700000) == V::SameBoot);
}

// Counters compare as a signed 32-bit difference: across the wrap, and a jump
// of 2^31 or more reads as older.
static void counterWrap() {
  CHECK(bootCounterNewer(1, 0));
  CHECK(!bootCounterNewer(5, 5));
  CHECK(bootCounterNewer(0x00000002u, 0xFFFFFFFEu));
  CHECK(!bootCounterNewer(0xFFFFFFFEu, 0x00000002u));
  CHECK(bootCounterNewer(0x7FFFFFFFu, 0));
  CHECK(!bootCounterNewer(0x80000000u, 0));
  PeerRestartTable t = {};
  CHECK(peerRestartOnBootCounter(t, kP4, 0xFFFFFFFFu, true, kOld, 1000) == V::DropSession);
  CHECK(peerRestartOnBootCounter(t, kP4, 1, true, kOld, 100000) == V::DropSession);  // wrapped: newer
  CHECK(peerRestartOnBootCounter(t, kP4, 0xFFFFFFFFu, true, kOld, 200000) == V::OlderBoot);
}

// A peer that could not read its count sends 0 (or a notice without one): no
// comparison is possible, and such a notice could be a copy played again and
// again, so it closes nothing and nothing is recorded (the hint covers that
// peer). An unknown peer included: it takes no table entry.
static void noCounter() {
  PeerRestartTable t = {};
  CHECK(peerRestartOnBootCounter(t, kXiao, 0, true, kOld, 500) == V::NoCounter);
  CHECK(peerRestartOnBootCounter(t, kXiao, 0, true, kOld, 600) == V::NoCounter);
  CHECK(peerRestartFind(t, kXiao) == nullptr);
  CHECK(peerRestartOnBootCounter(t, kP4, 7, true, kOld, 1000) == V::DropSession);
  unsigned drops = 0;
  for (uint32_t now = 100000; now < 400000; now += 11000) {
    if (peerRestartOnBootCounter(t, kP4, 0, true, kOld, now) == V::DropSession) ++drops;
    if (peerRestartOnBootCounter(t, kP4, 0, true, kOld, now + 5) == V::DropSession) ++drops;
  }
  CHECK(drops == 0);  // FAILS ON the counts-as-a-restart rule: one every 11 s
  // 7 is still the reference.
  CHECK(peerRestartOnBootCounter(t, kP4, 7, true, kOld, 500000) == V::SameBoot);
  CHECK(peerRestartFind(t, kP4)->boot == 7);
}

// Only a group tag of the peer's own mesh counts: a member of another mesh
// this device is in holds a group key too, but not this one. A sealed frame
// came over the peer's own session.
static void signalScope() {
  CHECK(peerRestartSignalScoped(false, 0x4A21, 0x4A21));
  CHECK(!peerRestartSignalScoped(false, 0x77C0, 0x4A21));  // another mesh's tag
  CHECK(!peerRestartSignalScoped(false, 0, 0));            // no mesh scope: never a tag
  CHECK(peerRestartSignalScoped(true, 0, 0x4A21));
  CHECK(peerRestartSignalScoped(true, 0x77C0, 0x4A21));
}


// Restart reports (1.0 step 8, review fix 3): only a SEALED counter that moved
// past the last sealed one reports a restart (the "Online (boot #N)" record).
// Tagged counters (any group-key holder can send them) never report; the first
// sealed counter after this device's start, a copy and an older one do not.
static void restartReports() {
  PeerRestartTable t = {};
  CHECK(!peerRestartBootSeen(t, kXiao, 3, 1000));             // first sealed: the reference
  CHECK(peerRestartOnBootCounter(t, kXiao, 4, true, kOld, 2000) == V::DropSession);  // tagged, newer
  for (uint32_t c = 5; c < 50; c++) (void)peerRestartOnBootCounter(t, kXiao, c, false, 0, 2000 + c * 11000);
  CHECK(peerRestartFind(t, kXiao)->boot == 49 && peerRestartFind(t, kXiao)->confirmedBoot == 3);
  CHECK(peerRestartBootSeen(t, kXiao, 4, 900000));             // sealed past the last sealed: one report
  CHECK(!peerRestartBootSeen(t, kXiao, 4, 901000));            // the same again: none
  CHECK(!peerRestartBootSeen(t, kXiao, 1, 902000));            // erased: the reference, not a restart
  CHECK(peerRestartFind(t, kXiao)->boot == 1 && peerRestartFind(t, kXiao)->confirmedBoot == 1);
  CHECK(!peerRestartBootSeen(t, kXiao, 0, 903000) && peerRestartFind(t, kXiao)->boot == 1);
  CHECK(peerRestartBootSeen(t, kXiao, 2, 904000));
}

// A mesh member forging notices with ever newer counters closes the session at
// most once per 10 s; the counters are still recorded, and each peer has its
// own limit.
static void forgedNotices() {
  PeerRestartTable t = {};
  uint32_t now = 1000;
  CHECK(peerRestartOnBootCounter(t, kP4, 100, true, kOld, now) == V::DropSession);
  unsigned drops = 0;
  for (uint32_t c = 101; c < 141; ++c) {  // one every 500 ms for 20 s
    now += 500;
    if (peerRestartOnBootCounter(t, kP4, c, true, kOld, now) == V::DropSession) ++drops;
  }
  CHECK(drops == 2);  // at 10 s and 20 s after the first
  CHECK(peerRestartFind(t, kP4)->boot == 140);
  CHECK(peerRestartOnBootCounter(t, kXiao, 9, true, kOld, now) == V::DropSession);
  CHECK(peerRestartOnBootCounter(t, kP4, 200, true, kOld, now + 1) == V::RateLimited);
}

// The device without the session: one hint per peer per second, none for its
// own SESSION_OPEN awaiting its CONFIRM or a session it replaced.
static void unknownSessionReplies() {
  PeerRestartTable t = {};
  CHECK(peerRestartOnUnknownSession(t, kXiao, true, 1000) == R::KnownId);
  CHECK(peerRestartOnUnknownSession(t, kXiao, false, 1000) == R::SendHint);
  CHECK(peerRestartOnUnknownSession(t, kXiao, false, 1200) == R::RateLimited);  // the burst's 2nd frame
  CHECK(peerRestartOnUnknownSession(t, kXiao, false, 1400) == R::RateLimited);  // and 3rd
  CHECK(peerRestartOnUnknownSession(t, kP4, false, 1400) == R::SendHint);       // another peer
  CHECK(peerRestartOnUnknownSession(t, kXiao, false, 1000 + kSessionUnknownHintMinIntervalMs) == R::SendHint);
}

// The device that sent the frame: it closes only its live session the hint
// names, for a sequence it sealed in it.
static void hintRules() {
  PeerRestartTable t = {};
  CHECK(peerRestartOnHint(t, kP4, false, 0, 0, 16447, 1, 1000) == H::NoSession);
  CHECK(peerRestartOnHint(t, kP4, true, 16447, 3, 9999, 1, 1000) == H::OtherSession);  // old / replaced
  CHECK(peerRestartOnHint(t, kP4, true, 16447, 3, 0, 1, 1000) == H::OtherSession);
  CHECK(peerRestartOnHint(t, kP4, true, 16447, 3, 16447, 4, 1000) == H::UnsentSeq);    // never sealed
  CHECK(peerRestartOnHint(t, kP4, true, 16447, 3, 16447, 0, 1000) == H::UnsentSeq);
  CHECK(peerRestartOnHint(t, kP4, true, 16447, 3, 16447, 1, 1000) == H::DropSession);
  // A second hint for the same burst: the session is gone (the caller closed
  // it), and a forged one within 10 s is refused.
  CHECK(peerRestartOnHint(t, kP4, false, 0, 0, 16447, 2, 1100) == H::NoSession);
  CHECK(peerRestartOnHint(t, kP4, true, 36309, 5, 36309, 5, 5000) == H::RateLimited);
  CHECK(peerRestartOnHint(t, kP4, true, 36309, 5, 36309, 5, 1000 + kPeerRestartDropMinIntervalMs) ==
        H::DropSession);
}

// The limit is shared: a notice and a hint together close at most once per 10 s.
static void sharedLimit() {
  PeerRestartTable t = {};
  CHECK(peerRestartOnBootCounter(t, kP4, 5, true, kOld, 1000) == V::DropSession);
  CHECK(peerRestartOnHint(t, kP4, true, 22, 9, 22, 9, 3000) == H::RateLimited);
  CHECK(peerRestartOnHint(t, kP4, true, 22, 9, 22, 9, 11000) == H::DropSession);
  CHECK(peerRestartOnBootCounter(t, kP4, 6, true, kOld, 12000) == V::RateLimited);
  // The counter was recorded even so: a copy of it later closes nothing.
  CHECK(peerRestartOnBootCounter(t, kP4, 6, true, kOld, 40000) == V::SameBoot);
}

// A counter seen in a sealed frame of the session (bond heartbeat, a sealed
// notice) becomes the reference outright, lower ones included, and never
// closes anything; its notice arriving later is then a copy.
static void bootSeenInSession() {
  PeerRestartTable t = {};
  peerRestartBootSeen(t, kP4, 0, 1000);
  CHECK(peerRestartFind(t, kP4) == nullptr);
  peerRestartBootSeen(t, kP4, 20, 1000);
  CHECK(peerRestartFind(t, kP4)->boot == 20);
  CHECK(peerRestartOnBootCounter(t, kP4, 20, true, kOld, 3000) == V::SameBoot);
  peerRestartBootSeen(t, kP4, 19, 4000);
  CHECK(peerRestartFind(t, kP4)->boot == 19);  // FAILS ON the newer-only rule
  CHECK(peerRestartOnBootCounter(t, kP4, 20, true, kOld, 5000) == V::DropSession);
}

// Sixteen peers fit; a seventeenth takes the entry used longest ago, which then
// starts unknown again (its next notice counts as a restart).
static void tableFull() {
  PeerRestartTable t = {};
  uint8_t mac[6] = {0x02, 0, 0, 0, 0, 0};
  for (uint8_t i = 0; i < kPeerRestartSlots; ++i) {
    mac[5] = i;
    CHECK(peerRestartOnBootCounter(t, mac, 50, false, 0, 1000u + i) == V::NoSession);
  }
  mac[5] = 0;
  CHECK(peerRestartOnBootCounter(t, mac, 50, false, 0, 5000) == V::SameBoot);  // refreshes peer 0
  mac[5] = 99;
  CHECK(peerRestartOnBootCounter(t, mac, 50, false, 0, 6000) == V::NoSession);
  mac[5] = 1;  // the oldest, given up
  CHECK(peerRestartFind(t, mac) == nullptr);
  mac[5] = 0;
  CHECK(peerRestartFind(t, mac) != nullptr && peerRestartFind(t, mac)->boot == 50);
  mac[5] = 1;
  CHECK(peerRestartOnBootCounter(t, mac, 50, true, kOld, 7000) == V::DropSession);
}

// Every time compare is a wrap-safe difference.
static void millisWrap() {
  PeerRestartTable t = {};
  const uint32_t nearWrap = 0xFFFFF000u;
  CHECK(peerRestartOnBootCounter(t, kP4, 3, true, kOld, nearWrap) == V::DropSession);
  CHECK(peerRestartOnBootCounter(t, kP4, 4, true, kOld, nearWrap + 5000) == V::RateLimited);
  CHECK(peerRestartOnBootCounter(t, kP4, 5, true, kOld, nearWrap + kPeerRestartDropMinIntervalMs) ==
        V::DropSession);
  // A drop stamped at 0 is kept as 1, never read as "none".
  PeerRestartTable u = {};
  CHECK(peerRestartOnHint(u, kP4, true, 9, 1, 9, 1, 0) == H::DropSession);
  CHECK(peerRestartOnHint(u, kP4, true, 9, 1, 9, 1, 500) == H::RateLimited);
  CHECK(peerRestartOnUnknownSession(u, kXiao, false, 0) == R::SendHint);
  CHECK(peerRestartOnUnknownSession(u, kXiao, false, 10) == R::RateLimited);
  CHECK(peerRestartOnUnknownSession(u, kXiao, false, nearWrap) == R::SendHint);
  CHECK(peerRestartOnUnknownSession(u, kXiao, false, nearWrap + 999) == R::RateLimited);
  CHECK(peerRestartOnUnknownSession(u, kXiao, false, nearWrap + 1000) == R::SendHint);
}

int main() {
  benchRestart();
  nothingToClose();
  replayedNotice();
  counterReset();
  counterWrap();
  noCounter();
  signalScope();
  restartReports();
  forgedNotices();
  unknownSessionReplies();
  hintRules();
  sharedLimit();
  bootSeenInSession();
  tableFull();
  millisWrap();
  std::printf("espnow_peer_restart_tests: %u checks passed\n", checks);
  return 0;
}
