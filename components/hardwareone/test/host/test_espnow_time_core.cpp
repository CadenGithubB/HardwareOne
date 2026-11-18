// Rules of mesh time (System_ESPNow_TimeCore.h, compiled as is).
//
// The mesh master sends its UTC clock to each paired device; a device with no
// better source of its own takes it. These cases check:
//   * the 16-byte frame: every field at its stated offset, round trip, a
//     payload that is too short;
//   * who sends: only the acting master, with mesh time and mesh mode on, a
//     set clock from rtc, ntp, manual or cm5, and no unconfirmed NTP jump;
//   * when: the first round at once, again after 10 minutes, at once after a
//     clock step and once more 15 s later, by hand; one frame to a peer whose
//     session came up, only after the session has settled; the uptime wrap;
//   * what a receiver does with a frame: every row of the rule, for every own
//     source; the frame's wait is added; a change of more than an hour is
//     held and applied when the same sender confirms it; a second sender
//     cannot move the clock back and forth; the 10 s gap after a step.
#include "System_ESPNow_TimeCore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

static unsigned gChecks = 0;
#define CHECK(c)                                                                 \
  do {                                                                           \
    ++gChecks;                                                                   \
    if (!(c)) {                                                                  \
      std::fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
      std::exit(1);                                                              \
    }                                                                            \
  } while (0)

using namespace meshtime;

static bool is(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

static const int64_t kNow = 1763388202417LL;  // 2026-11-17, UTC ms
static const uint8_t kHub[6] = { 2, 0, 0, 0, 0, 1 };
static const uint8_t kBackup[6] = { 2, 0, 0, 0, 0, 2 };
static const uint8_t kOther[6] = { 2, 0, 0, 0, 0, 3 };

static void testFrame() {
  CHECK(kFrameLen == 16);
  Frame f = {};
  f.epochMs = 0x0102030405060708LL;
  f.source = kSrcNtp;
  f.flags = kFlagActingBackup;
  f.sinceSetMin = 0xA1B2;
  uint8_t raw[kFrameLen];
  std::memset(raw, 0xEE, sizeof(raw));
  packFrame(raw, f);
  const uint8_t expect[kFrameLen] = { 8, 7, 6, 5, 4, 3, 2, 1, 2, 1, 0xB2, 0xA1, 0, 0, 0, 0 };
  CHECK(std::memcmp(raw, expect, sizeof(raw)) == 0);
  Frame back = {};
  CHECK(parseFrame(raw, sizeof(raw), back));
  CHECK(back.epochMs == f.epochMs && back.source == kSrcNtp && back.flags == kFlagActingBackup &&
        back.sinceSetMin == 0xA1B2);
  CHECK(!parseFrame(raw, kFrameLen - 1, back) && !parseFrame(nullptr, kFrameLen, back));
  CHECK(parseFrame(raw, 64, back));  // a longer payload of a later version is read by its first 16 bytes
  f.epochMs = kNow;
  packFrame(raw, f);
  CHECK(parseFrame(raw, sizeof(raw), back) && back.epochMs == kNow);

  CHECK(sinceSetMinutes(0) == 0 && sinceSetMinutes(59) == 0 && sinceSetMinutes(60) == 1 &&
        sinceSetMinutes(3600) == 60);
  CHECK(sinceSetMinutes(65534u * 60u) == 65534 && sinceSetMinutes(65535u * 60u) == 65535 &&
        sinceSetMinutes(0xFFFFFFFFu) == 65535);
  CHECK(kSrcRtc == 1 && kSrcNtp == 2 && kSrcManual == 3 && kSrcCm5 == 4 && kSrcWeakFirst == 128);
  CHECK(is(wireSourceName(kSrcRtc), "rtc") && is(wireSourceName(kSrcNtp), "ntp") &&
        is(wireSourceName(kSrcManual), "manual") && is(wireSourceName(kSrcCm5), "cm5"));
  CHECK(is(wireSourceName(0), "none") && is(wireSourceName(5), "other") && is(wireSourceName(127), "other") &&
        is(wireSourceName(128), "weak") && is(wireSourceName(255), "weak"));
  CHECK(wireSourceFor(OwnSource::Rtc) == kSrcRtc && wireSourceFor(OwnSource::Ntp) == kSrcNtp &&
        wireSourceFor(OwnSource::Manual) == kSrcManual && wireSourceFor(OwnSource::Cm5) == kSrcCm5);
  CHECK(wireSourceFor(OwnSource::None) == 0 && wireSourceFor(OwnSource::Ring) == 0 &&
        wireSourceFor(OwnSource::Mesh) == 0);
  CHECK(epochMsPlausible(kNow) && epochMsPlausible(kEpochMinMs) && !epochMsPlausible(kEpochMinMs - 1) &&
        !epochMsPlausible(kEpochMaxMs) && !epochMsPlausible(0) && !epochMsPlausible(-5));
}

static void testSender() {
  const OwnSource sources[] = { OwnSource::None, OwnSource::Rtc, OwnSource::Ntp, OwnSource::Manual,
                                OwnSource::Ring, OwnSource::Cm5, OwnSource::Mesh };
  unsigned sending = 0;
  // Every combination of the six inputs.
  for (int bits = 0; bits < 32; ++bits) {
    for (OwnSource source : sources) {
      SenderInputs in = {};
      in.settingOn = bits & 1;
      in.meshOn = bits & 2;
      in.actingMaster = bits & 4;
      in.clockSet = bits & 8;
      in.clockHeld = bits & 16;
      in.source = source;
      const bool real = source == OwnSource::Rtc || source == OwnSource::Ntp || source == OwnSource::Manual ||
                        source == OwnSource::Cm5;
      const bool expect = in.settingOn && in.meshOn && in.actingMaster && in.clockSet && real && !in.clockHeld;
      const SendWhyNot why = senderDecide(in);
      CHECK((why == SendWhyNot::Sends) == expect);
      CHECK((sendWhyNotKey(why) == nullptr) == expect);
      if (expect) ++sending;
    }
  }
  CHECK(sending == 4);
  // The reason named is the first that applies.
  SenderInputs in = {};
  CHECK(senderDecide(in) == SendWhyNot::Off);
  in.settingOn = true;
  CHECK(senderDecide(in) == SendWhyNot::MeshOff);
  in.meshOn = true;
  CHECK(senderDecide(in) == SendWhyNot::NotMaster);
  in.actingMaster = true;
  CHECK(senderDecide(in) == SendWhyNot::ClockNotSet);
  in.clockSet = true;
  CHECK(senderDecide(in) == SendWhyNot::SourceNotReal);  // None while set: carried over a restart
  in.source = OwnSource::Ring;
  CHECK(senderDecide(in) == SendWhyNot::SourceNotReal);
  in.source = OwnSource::Mesh;
  CHECK(senderDecide(in) == SendWhyNot::SourceNotReal);  // a backup does not pass mesh time on
  in.source = OwnSource::Ntp;
  in.clockHeld = true;
  CHECK(senderDecide(in) == SendWhyNot::ClockUnconfirmed);
  in.clockHeld = false;
  CHECK(senderDecide(in) == SendWhyNot::Sends);
  CHECK(is(sendWhyNotKey(SendWhyNot::Off), "off") && is(sendWhyNotKey(SendWhyNot::MeshOff), "mesh_off") &&
        is(sendWhyNotKey(SendWhyNot::NotMaster), "not_master") &&
        is(sendWhyNotKey(SendWhyNot::ClockNotSet), "clock_not_set") &&
        is(sendWhyNotKey(SendWhyNot::SourceNotReal), "source_not_real") &&
        is(sendWhyNotKey(SendWhyNot::ClockUnconfirmed), "clock_unconfirmed"));
}

static void testRounds() {
  // The first round starts as soon as the device sends.
  RoundState r = {};
  CHECK(roundBegin(r, 5000, 3, false) && r.serial == 1 && r.everRound);
  CHECK(!roundBegin(r, 5500, 3, false));
  CHECK(!roundBegin(r, 5000 + kRoundIntervalMs - 1, 3, false));
  CHECK(roundBegin(r, 5000 + kRoundIntervalMs, 3, false) && r.serial == 2);
  // By hand.
  uint32_t t = 5000 + kRoundIntervalMs + 100;
  CHECK(roundBegin(r, t, 3, true) && r.serial == 3);
  CHECK(!roundBegin(r, t + 1, 3, false));
  // The master's clock was set: a round at once, and one repeat 15 s later.
  t += 20000;
  CHECK(roundBegin(r, t, 4, false) && r.serial == 4 && r.repeatPending);
  CHECK(!roundBegin(r, t + kStepRepeatMs - 1, 4, false));
  CHECK(roundBegin(r, t + kStepRepeatMs, 4, false) && r.serial == 5 && !r.repeatPending);
  CHECK(!roundBegin(r, t + kStepRepeatMs + 1000, 4, false));
  CHECK(roundBegin(r, t + kStepRepeatMs + kRoundIntervalMs, 4, false) && r.serial == 6);
  // Across the uptime wrap.
  RoundState w = {};
  CHECK(roundBegin(w, 0xFFFFF000u, 1, false));
  CHECK(!roundBegin(w, 0x00000100u, 1, false));
  CHECK(roundBegin(w, 0xFFFFF000u + kRoundIntervalMs, 1, false));
  RoundState ws = {};
  CHECK(roundBegin(ws, 0xFFFFFFF0u, 1, false));
  CHECK(roundBegin(ws, 0xFFFFFFF8u, 2, false) && ws.repeatPending);  // the repeat falls after the wrap
  CHECK(!roundBegin(ws, 100, 2, false));
  CHECK(roundBegin(ws, 0xFFFFFFF8u + kStepRepeatMs, 2, false));
  // The round number never becomes 0 ("no round yet").
  RoundState big = {};
  big.everRound = true;
  big.serial = 0xFFFFFFFFu;
  CHECK(roundBegin(big, 1000, 0, true) && big.serial == 1);

  // Peers.
  RoundState round = {};
  PeerSendState peer = {};
  CHECK(!peerDue(peer, round, false, 0, 10000));                // no session
  CHECK(!peerDue(peer, round, true, 9000, 10000));              // the session is 1 s old: not settled
  CHECK(!peerDue(peer, round, true, 9000, 9000 + kSettleMs - 1));
  CHECK(peerDue(peer, round, true, 9000, 9000 + kSettleMs));    // a new session gets a frame, round or not
  peerSent(peer, round, 9000, 11000);
  CHECK(!peerDue(peer, round, true, 9000, 12000));
  CHECK(roundBegin(round, 12000, 1, false));
  CHECK(peerDue(peer, round, true, 9000, 12000));               // the round has not reached it
  peerSent(peer, round, 9000, 12000);
  CHECK(!peerDue(peer, round, true, 9000, 13000) && peer.lastSentMs == 12000);
  // The session came up again (or was rekeyed): one more frame, after it settled.
  CHECK(!peerDue(peer, round, true, 50000, 50500));
  CHECK(peerDue(peer, round, true, 50000, 50000 + kSettleMs));
  peerSent(peer, round, 50000, 52000);
  CHECK(!peerDue(peer, round, true, 50000, 53000));
  // A session stamped just before the wrap.
  PeerSendState late = {};
  CHECK(!peerDue(late, round, true, 0xFFFFFF00u, 0xFFFFFF00u + 1999));
  CHECK(peerDue(late, round, true, 0xFFFFFF00u, 0xFFFFFF00u + 2000));
  CHECK(sessionSettled(0xFFFFFF00u, 0x00000700u) && !sessionSettled(100, 2099) && sessionSettled(100, 2100));
  // A session another task set up after this pass read the uptime: its stamp
  // is a little ahead. It is not settled (it was taken for 49 days old).
  CHECK(!sessionSettled(50100, 50000) && !sessionSettled(50000 + 60000, 50000) && !sessionSettled(50000, 50000));
  CHECK(!sessionSettled(0x00000050u, 0xFFFFFFF0u));  // ahead across the wrap
  CHECK(!peerDue(PeerSendState{}, round, true, 50300, 50000));
}

// A follower with settable state.
struct Follower {
  FollowerState st;
  FollowerInputs in;
  Follower() : st{}, in{} {
    in.settingOn = true;
    in.meshOn = true;
    in.clockSet = true;
    in.ownSource = OwnSource::Mesh;
    in.nowEpochMs = kNow;
    in.nowMonoMs = 1000000;
  }
  // A frame that claims `claimEpochMs`, received `waitedMs` before now.
  Decision frame(const uint8_t sender[6], int64_t claimEpochMs, int64_t waitedMs = 0, uint8_t source = kSrcNtp,
                 uint8_t flags = 0) {
    Received rx = {};
    rx.frame.epochMs = claimEpochMs;
    rx.frame.source = source;
    rx.frame.flags = flags;
    std::memcpy(rx.sender, sender, 6);
    rx.receiptMonoMs = in.nowMonoMs - waitedMs;
    return followerDecide(st, in, rx);
  }
  void pass(int64_t ms) {
    in.nowMonoMs += ms;
    in.nowEpochMs += ms;
  }
};

static void testFollowerRows() {
  // 1 to 4: off, not the named master, this device is the master, a bad time.
  {
    Follower f;
    f.in.settingOn = false;
    CHECK(f.frame(kHub, kNow + 60000).verdict == Verdict::IgnoredOff);
    f.in.settingOn = true;
    f.in.hasNamedMaster = true;
    CHECK(f.frame(kHub, kNow + 60000).verdict == Verdict::IgnoredNotNamed);
    f.in.senderIsNamed = true;
    CHECK(f.frame(kHub, kNow + 60000).verdict == Verdict::Applied);
    Follower m;
    m.in.actingMaster = true;
    CHECK(m.frame(kHub, kNow + 60000).verdict == Verdict::IgnoredMaster);
    // 1b: mesh mode off: no time is taken (the status says so), whatever else.
    Follower off;
    off.in.meshOn = false;
    CHECK(off.frame(kHub, kNow + 60000).verdict == Verdict::IgnoredMeshOff);
    off.in.clockSet = false;
    off.in.hasNamedMaster = off.in.senderIsNamed = off.in.senderIsNamedPrimary = true;
    CHECK(off.frame(kHub, kNow).verdict == Verdict::IgnoredMeshOff && !off.st.stickyHave && !off.st.appliedHave);
    off.in.settingOn = false;
    CHECK(off.frame(kHub, kNow).verdict == Verdict::IgnoredOff);  // the setting is named first
    Follower b;
    CHECK(b.frame(kHub, kNow + 60000, 0, /*source=*/0).verdict == Verdict::RefusedBadTime);
    CHECK(b.frame(kHub, 1000).verdict == Verdict::RefusedBadTime);                   // 1970
    CHECK(b.frame(kHub, kEpochMinMs - 1).verdict == Verdict::RefusedBadTime);        // 2019
    CHECK(b.frame(kHub, kEpochMaxMs).verdict == Verdict::RefusedBadTime);            // 2100
    CHECK(b.frame(kHub, kEpochMaxMs - 10, 100).verdict == Verdict::RefusedBadTime);  // 2100 once its wait is added
    CHECK(b.frame(kHub, -1).verdict == Verdict::RefusedBadTime);
    // Nothing is computed from a time that is not a real date: a hostile
    // INT64_MAX or INT64_MIN gives no overflow (UBSan) and no difference.
    for (int64_t hostile : { INT64_MAX, INT64_MIN, INT64_MAX - 5, (int64_t)-1 }) {
      Decision bad = b.frame(kHub, hostile, 100);
      CHECK(bad.verdict == Verdict::RefusedBadTime && bad.candidateEpochMs == 0 && bad.differenceMs == 0);
      Follower named;
      named.in.hasNamedMaster = true;
      bad = named.frame(kHub, hostile, 100);
      CHECK(bad.verdict == Verdict::IgnoredNotNamed && bad.candidateEpochMs == 0 && bad.differenceMs == 0);
    }
    // A bad frame is refused also by a device whose clock is not set.
    Follower dark;
    dark.in.clockSet = false;
    dark.in.ownSource = OwnSource::None;
    CHECK(dark.frame(kHub, 1000).verdict == Verdict::RefusedBadTime);
    CHECK(dark.frame(kHub, kNow, 0, 0).verdict == Verdict::RefusedBadTime);
  }
  // 5 and 8 to 11 for every own source.
  {
    const OwnSource sources[] = { OwnSource::None, OwnSource::Rtc, OwnSource::Ntp, OwnSource::Manual,
                                  OwnSource::Ring, OwnSource::Cm5, OwnSource::Mesh };
    for (OwnSource source : sources) {
      const bool better = ownSourceIsReal(source);
      CHECK(better == (source == OwnSource::Rtc || source == OwnSource::Ntp || source == OwnSource::Manual ||
                       source == OwnSource::Cm5));
      Follower f;
      f.in.ownSource = source;
      const Decision d = f.frame(kHub, kNow + 60000);
      CHECK(d.verdict == (better ? Verdict::IgnoredBetterSource : Verdict::Applied));
      CHECK(d.candidateEpochMs == kNow + 60000 && d.differenceMs == 60000);
      // A clock that is not set takes the time whatever its source field says.
      Follower dark;
      dark.in.clockSet = false;
      dark.in.ownSource = source;
      const Decision first = dark.frame(kHub, kNow);
      CHECK(first.verdict == Verdict::Applied && first.differenceMs == 0);
    }
  }
  // 10 and 11: the bands.
  {
    Follower f;
    CHECK(f.frame(kHub, kNow + kAgreeBandMs - 1).verdict == Verdict::Agreed);
    CHECK(f.frame(kHub, kNow - kAgreeBandMs + 1).verdict == Verdict::Agreed);
    Follower g;
    CHECK(g.frame(kHub, kNow + kAgreeBandMs).verdict == Verdict::Applied);
    Follower h;
    CHECK(h.frame(kHub, kNow - kAgreeBandMs).verdict == Verdict::Applied);
    Follower i;
    CHECK(i.frame(kHub, kNow + kLargeChangeMs).verdict == Verdict::Applied);
    Follower j;
    CHECK(j.frame(kHub, kNow + kLargeChangeMs + 1).verdict == Verdict::HeldLargeChange);
    Follower k;
    CHECK(k.frame(kHub, kNow - kLargeChangeMs - 1).verdict == Verdict::HeldLargeChange);
  }
  // The frame's wait is added to its time.
  {
    Follower f;
    // It claimed "now - 3 s" three seconds ago: that is now, so it agrees.
    Decision d = f.frame(kHub, kNow - 3000, 3000);
    CHECK(d.verdict == Verdict::Agreed && d.candidateEpochMs == kNow && d.differenceMs == 0);
    Follower g;
    d = g.frame(kHub, kNow + 60000, 250);
    CHECK(d.verdict == Verdict::Applied && d.candidateEpochMs == kNow + 60250);
    Follower h;  // a receipt stamp in the future cannot pull the time back
    d = h.frame(kHub, kNow + 60000, -500);
    CHECK(d.candidateEpochMs == kNow + 60000);
  }
  // 6: a weak source is taken only while the own clock is not set.
  {
    Follower f;
    CHECK(f.frame(kHub, kNow + 60000, 0, 128).verdict == Verdict::IgnoredWeakSource);
    CHECK(f.frame(kHub, kNow + 60000, 0, 255).verdict == Verdict::IgnoredWeakSource);
    CHECK(f.frame(kHub, kNow + 60000, 0, 127).verdict == Verdict::Applied);  // an unknown real source
    Follower dark;
    dark.in.clockSet = false;
    CHECK(dark.frame(kHub, kNow, 0, 200).verdict == Verdict::Applied);
  }
  // Every verdict has a name of its own.
  for (size_t i = 0; i < kVerdictCount; ++i) {
    CHECK(!is(verdictKey((Verdict)i), "?"));
    for (size_t k = 0; k < i; ++k) CHECK(!is(verdictKey((Verdict)i), verdictKey((Verdict)k)));
  }
  CHECK(is(verdictKey(Verdict::Applied), "applied") && is(verdictKey(Verdict::HeldLargeChange), "held_large_change") &&
        is(verdictKey(Verdict::IgnoredOtherMaster), "ignored_other_master"));
  CHECK(is(verdictKey(Verdict::IgnoredMeshOff), "ignored_mesh_off") && verdictIsIgnored(Verdict::IgnoredMeshOff));
  CHECK(verdictIsIgnored(Verdict::IgnoredTooSoon) && verdictIsIgnored(Verdict::IgnoredWeakSource) &&
        !verdictIsIgnored(Verdict::Applied) && !verdictIsIgnored(Verdict::Agreed) &&
        !verdictIsIgnored(Verdict::RefusedBadTime) && !verdictIsIgnored(Verdict::HeldLargeChange));
}

static void testLargeChange() {
  const int64_t wrong = kNow + 7200000;  // the master says two hours later
  // Held, then applied when the same sender says it again after 10 s.
  {
    Follower f;
    Decision d = f.frame(kHub, wrong);
    CHECK(d.verdict == Verdict::HeldLargeChange && d.differenceMs == 7200000 && f.st.holdHave);
    f.pass(15000);
    d = f.frame(kHub, wrong + 15000);
    CHECK(d.verdict == Verdict::Applied && !f.st.holdHave);
  }
  // Not confirmed: said again within 10 s (the first claim stands and is confirmed later).
  {
    Follower f;
    f.frame(kHub, wrong);
    f.pass(9000);
    CHECK(f.frame(kHub, wrong + 9000).verdict == Verdict::HeldLargeChange);
    f.pass(2000);  // 11 s after the first
    CHECK(f.frame(kHub, wrong + 11000).verdict == Verdict::Applied);
  }
  // Not confirmed: by another sender. Its frames do not clear the memory either.
  {
    Follower f;
    f.frame(kHub, wrong);
    f.pass(15000);
    CHECK(f.frame(kOther, wrong + 15000).verdict == Verdict::HeldLargeChange);
    CHECK(std::memcmp(f.st.holdMac, kHub, 6) == 0);
    CHECK(f.frame(kOther, kNow + 15000 + 100, 0, 0).verdict == Verdict::RefusedBadTime);
    CHECK(f.st.holdHave && std::memcmp(f.st.holdMac, kHub, 6) == 0);
    f.pass(1000);
    CHECK(f.frame(kHub, wrong + 16000).verdict == Verdict::Applied);
  }
  // Not confirmed: the second claim disagrees with the first. It becomes the claim.
  {
    Follower f;
    f.frame(kHub, wrong);
    f.pass(15000);
    CHECK(f.frame(kHub, wrong + 15000 + 6000).verdict == Verdict::HeldLargeChange);
    f.pass(15000);
    CHECK(f.frame(kHub, wrong + 30000 + 6000).verdict == Verdict::Applied);
  }
  // The claim may drift by 5 s between the two frames, not more.
  {
    Follower f;
    f.frame(kHub, wrong);
    f.pass(20000);
    CHECK(f.frame(kHub, wrong + 20000 + kHoldAgreeMs).verdict == Verdict::Applied);
    Follower g;
    g.frame(kHub, wrong);
    g.pass(20000);
    CHECK(g.frame(kHub, wrong + 20000 - kHoldAgreeMs - 1).verdict == Verdict::HeldLargeChange);
  }
  // Not confirmed after 30 minutes: the old claim has run out, the new one starts again.
  {
    Follower f;
    f.frame(kHub, wrong);
    f.pass(kHoldMaxMs + 1000);
    CHECK(f.frame(kHub, wrong + kHoldMaxMs + 1000).verdict == Verdict::HeldLargeChange);
    f.pass(15000);
    CHECK(f.frame(kHub, wrong + kHoldMaxMs + 16000).verdict == Verdict::Applied);
    // Another sender may take a claim that has run out.
    Follower g;
    g.frame(kHub, wrong);
    g.pass(kHoldMaxMs + 1);
    CHECK(g.frame(kOther, wrong).verdict == Verdict::HeldLargeChange);
    CHECK(std::memcmp(g.st.holdMac, kOther, 6) == 0);
  }
  // A frame from the same sender that is no large change settles it.
  {
    Follower f;
    f.frame(kHub, wrong);
    f.pass(15000);
    CHECK(f.frame(kHub, kNow + 15000 + 60000).verdict == Verdict::Applied && !f.st.holdHave);
  }
  // The receipts are what is compared, not when the frames were judged.
  {
    Follower f;
    f.frame(kHub, wrong, 4000);  // received 4 s before it was judged
    f.pass(8000);                // judged 8 s later, received 12 s after the first
    CHECK(f.frame(kHub, wrong + 12000, 0).verdict == Verdict::Applied);
  }
}

static void testTwoSenders() {
  // Two devices both send time, 20 s apart. The follower takes the first and
  // does not step back and forth.
  {
    Follower f;
    CHECK(f.frame(kHub, kNow + 60000).verdict == Verdict::Applied);
    f.pass(30000);
    f.in.nowEpochMs = kNow + 60000 + 30000;  // the clock was set
    CHECK(f.frame(kOther, f.in.nowEpochMs + 20000).verdict == Verdict::IgnoredOtherMaster);
    f.pass(30000);
    CHECK(f.frame(kHub, f.in.nowEpochMs + 100).verdict == Verdict::Agreed);
    f.pass(30000);
    CHECK(f.frame(kOther, f.in.nowEpochMs + 20000).verdict == Verdict::IgnoredOtherMaster);
    // When the first sender falls silent for 25 minutes, the other is taken.
    f.pass(kStickyMs);
    CHECK(f.frame(kOther, f.in.nowEpochMs + 20000).verdict == Verdict::Applied);
    CHECK(std::memcmp(f.st.stickyMac, kOther, 6) == 0);
  }
  // Rank: the primary named here beats a master by its own word, which beats
  // a backup acting as master.
  {
    Follower f;
    CHECK(f.frame(kBackup, kNow + 60000, 0, kSrcNtp, kFlagActingBackup).verdict == Verdict::Applied);
    CHECK(f.st.stickyRank == 0);
    f.pass(kUnnamedApplyGapMs);  // (no master is named: one step in 10 minutes)
    f.in.nowEpochMs = kNow + 60000 + kUnnamedApplyGapMs;
    // The master is back and says another time: it outranks the acting backup.
    CHECK(f.frame(kHub, f.in.nowEpochMs + 30000).verdict == Verdict::Applied && f.st.stickyRank == 1);
    f.pass(60000);
    f.in.nowEpochMs += 30000;
    // The backup, still acting, is now ignored.
    CHECK(f.frame(kBackup, f.in.nowEpochMs + 60000, 0, kSrcNtp, kFlagActingBackup).verdict ==
          Verdict::IgnoredOtherMaster);
    // Equal rank does not replace.
    CHECK(f.frame(kOther, f.in.nowEpochMs + 60000).verdict == Verdict::IgnoredOtherMaster);
    // The primary named on this device outranks both.
    f.in.hasNamedMaster = true;
    f.in.senderIsNamed = true;
    f.in.senderIsNamedPrimary = true;
    CHECK(f.frame(kOther, f.in.nowEpochMs + 60000).verdict == Verdict::Applied && f.st.stickyRank == 2);
  }
  // A frame that agrees also makes its sender the one followed.
  {
    Follower f;
    CHECK(f.frame(kHub, kNow + 100).verdict == Verdict::Agreed && f.st.stickyHave);
    f.pass(1000);
    CHECK(f.frame(kOther, f.in.nowEpochMs + 60000).verdict == Verdict::IgnoredOtherMaster);
  }
  // After a step, another step waits 10 s (a peer cannot step the clock at radio rate).
  {
    Follower f;
    f.in.hasNamedMaster = f.in.senderIsNamed = true;
    CHECK(f.frame(kHub, kNow + 121000).verdict == Verdict::Applied);
    f.pass(1000);
    f.in.nowEpochMs = kNow + 122000;
    CHECK(f.frame(kHub, f.in.nowEpochMs - 121000).verdict == Verdict::IgnoredTooSoon);
    f.pass(kApplyGapMs - 1001);
    CHECK(f.frame(kHub, f.in.nowEpochMs - 121000).verdict == Verdict::IgnoredTooSoon);
    f.pass(1);
    CHECK(f.frame(kHub, f.in.nowEpochMs - 121000).verdict == Verdict::Applied);
    // A clock that is not set is never kept waiting.
    Follower dark;
    dark.in.clockSet = false;
    CHECK(dark.frame(kHub, kNow).verdict == Verdict::Applied);
    CHECK(dark.frame(kHub, kNow).verdict == Verdict::Applied);
  }
  // With no master named, any paired peer that claims to be the master can
  // change the clock at most once in 10 minutes (each change of more than
  // 2 minutes writes the boot anchor and the events log). A frame every 10 s,
  // 59 minutes off each time, moved such a clock once every 10 s before.
  {
    Follower f;
    int steps = 0;
    for (int i = 0; i < 360; ++i) {  // an hour of frames, one every 10 s
      const Decision d = f.frame(kOther, f.in.nowEpochMs + 59LL * 60 * 1000);
      if (d.verdict == Verdict::Applied) {
        ++steps;
        f.in.nowEpochMs = d.candidateEpochMs;
      } else {
        CHECK(d.verdict == Verdict::IgnoredTooSoon);
      }
      f.pass(10000);
    }
    CHECK(steps == 6);
    // Agreeing frames are still agreed meanwhile, and keep the sender followed.
    Follower g;
    CHECK(g.frame(kHub, kNow + 60000).verdict == Verdict::Applied);
    g.in.nowEpochMs = kNow + 60000;
    g.pass(60000);
    CHECK(g.frame(kHub, g.in.nowEpochMs + 100).verdict == Verdict::Agreed);
    CHECK(g.frame(kHub, g.in.nowEpochMs + 120000).verdict == Verdict::IgnoredTooSoon);
    g.pass(kUnnamedApplyGapMs - 60000 - 1);
    CHECK(g.frame(kHub, g.in.nowEpochMs + 120000).verdict == Verdict::IgnoredTooSoon);
    g.pass(1);
    CHECK(g.frame(kHub, g.in.nowEpochMs + 120000).verdict == Verdict::Applied);
    // A large change confirmed too soon stays held and is applied when the 10
    // minutes are over and the sender says it again.
    Follower h;
    CHECK(h.frame(kHub, kNow + 60000).verdict == Verdict::Applied);
    h.in.nowEpochMs = kNow + 60000;
    const int64_t wrong = h.in.nowEpochMs + 7200000;
    h.pass(30000);
    CHECK(h.frame(kHub, wrong + 30000).verdict == Verdict::HeldLargeChange);
    h.pass(15000);
    CHECK(h.frame(kHub, wrong + 45000).verdict == Verdict::IgnoredTooSoon && h.st.holdHave);
    h.pass(kUnnamedApplyGapMs);
    CHECK(h.frame(kHub, wrong + 45000 + kUnnamedApplyGapMs).verdict == Verdict::Applied && !h.st.holdHave);
    // With a master named, its frames step the clock after 10 s as before.
    Follower n;
    n.in.hasNamedMaster = n.in.senderIsNamed = true;
    CHECK(n.frame(kHub, kNow + 60000).verdict == Verdict::Applied);
    n.in.nowEpochMs = kNow + 60000;
    n.pass(kApplyGapMs);
    CHECK(n.frame(kHub, n.in.nowEpochMs + 120000).verdict == Verdict::Applied);
  }
}

int main() {
  testFrame();
  testSender();
  testRounds();
  testFollowerRows();
  testLargeChange();
  testTwoSenders();
  std::printf("espnow_time_core_tests: %u checks passed\n", gChecks);
  return 0;
}
