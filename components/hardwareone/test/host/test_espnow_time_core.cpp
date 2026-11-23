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
//     source; time only from the master named here (primary or backup), none
//     at all with no master named; the frame's wait is added; a change of
//     more than an hour is held and applied when the same sender confirms it;
//     the primary and the backup cannot move the clock back and forth; the
//     10 s gap after a step;
//   * a fingerprint set, changed or cleared drops the sender followed and the
//     claim held at once: the newly named master's next frame applies.
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
        wireSourceFor(OwnSource::Mesh) == 0 && wireSourceFor(OwnSource::MeshNtp) == 0);
  // A frame is NTP-backed mesh time when the master's clock is from NTP.
  CHECK(frameClockSource(kSrcNtp) == OwnSource::MeshNtp && frameClockSource(kSrcRtc) == OwnSource::Mesh &&
        frameClockSource(kSrcManual) == OwnSource::Mesh && frameClockSource(kSrcCm5) == OwnSource::Mesh &&
        frameClockSource(200) == OwnSource::Mesh);
  CHECK(!ownSourceIsReal(OwnSource::Mesh) && !ownSourceIsReal(OwnSource::MeshNtp) && !ownSourceIsReal(OwnSource::Ring) &&
        !ownSourceIsReal(OwnSource::None) && ownSourceIsReal(OwnSource::Rtc) && ownSourceIsReal(OwnSource::Cm5));
  CHECK(epochMsPlausible(kNow) && epochMsPlausible(kEpochMinMs) && !epochMsPlausible(kEpochMinMs - 1) &&
        !epochMsPlausible(kEpochMaxMs) && !epochMsPlausible(0) && !epochMsPlausible(-5));
}

static void testSender() {
  const OwnSource sources[] = { OwnSource::None, OwnSource::Rtc, OwnSource::Ntp, OwnSource::Manual,
                                OwnSource::Ring, OwnSource::Cm5, OwnSource::Mesh, OwnSource::MeshNtp };
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

// A follower with settable state. kHub is named on it as the primary and
// kBackup as the backup master; either can be unnamed (nullptr). kOther is
// paired and named as neither.
struct Follower {
  FollowerState st;
  FollowerInputs in;
  const uint8_t* primary = kHub;
  const uint8_t* backup = kBackup;
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
    const bool isPrimary = primary && std::memcmp(sender, primary, 6) == 0;
    const bool isBackup = backup && std::memcmp(sender, backup, 6) == 0;
    // The fingerprints are the addresses as text here.
    char primaryText[16] = "", backupText[16] = "";
    if (primary) std::snprintf(primaryText, sizeof(primaryText), "%02x%02x%02x%02x%02x%02x", primary[0], primary[1],
                               primary[2], primary[3], primary[4], primary[5]);
    if (backup) std::snprintf(backupText, sizeof(backupText), "%02x%02x%02x%02x%02x%02x", backup[0], backup[1],
                              backup[2], backup[3], backup[4], backup[5]);
    in.namedTag = namedMastersTag(primaryText, backupText);
    in.hasNamedMaster = primary || backup;
    in.senderIsNamed = isPrimary || isBackup;
    in.senderIsNamedPrimary = isPrimary;
    return followerDecide(st, in, rx);
  }
  void pass(int64_t ms) {
    in.nowMonoMs += ms;
    in.nowEpochMs += ms;
  }
};

static void testFollowerRows() {
  // 1 to 4: off, this device is the master, no master named, not the named
  // master, a bad time.
  {
    Follower f;
    f.in.settingOn = false;
    CHECK(f.frame(kHub, kNow + 60000).verdict == Verdict::IgnoredOff);
    f.in.settingOn = true;
    CHECK(f.frame(kOther, kNow + 60000).verdict == Verdict::IgnoredNotNamed);
    // The acting-backup bit does not make a device that is not named a master.
    CHECK(f.frame(kOther, kNow + 60000, 0, kSrcNtp, kFlagActingBackup).verdict == Verdict::IgnoredNotNamed);
    CHECK(!f.st.stickyHave && !f.st.appliedHave);
    CHECK(f.frame(kHub, kNow + 60000).verdict == Verdict::Applied);
    Follower m;
    m.in.actingMaster = true;
    CHECK(m.frame(kHub, kNow + 60000).verdict == Verdict::IgnoredMaster);
    m.primary = m.backup = nullptr;
    CHECK(m.frame(kHub, kNow + 60000).verdict == Verdict::IgnoredMaster);  // the master is named first
    // 3: no master named on this device: no frame is taken, from anyone, also
    // while the clock is not set; nothing is followed or held.
    for (bool clockSet : { true, false }) {
      Follower none;
      none.primary = none.backup = nullptr;
      none.in.clockSet = clockSet;
      for (const uint8_t* sender : { kHub, kBackup, kOther }) {
        for (uint8_t flags : { (uint8_t)0, kFlagActingBackup }) {
          for (int64_t claim : { kNow + 60000, kNow + 7200000, kNow + 100 }) {
            CHECK(none.frame(sender, claim, 0, kSrcNtp, flags).verdict == Verdict::IgnoredNoMaster);
          }
        }
      }
      CHECK(!none.st.stickyHave && !none.st.appliedHave && !none.st.holdHave);
    }
    // Only a backup named: its frames are taken, the primary's are not.
    Follower onlyBackup;
    onlyBackup.primary = nullptr;
    CHECK(onlyBackup.frame(kHub, kNow + 60000).verdict == Verdict::IgnoredNotNamed);
    CHECK(onlyBackup.frame(kBackup, kNow + 60000, 0, kSrcNtp, kFlagActingBackup).verdict == Verdict::Applied);
    // The named backup is taken by its fingerprint, as is the primary.
    Follower bk;
    CHECK(bk.frame(kBackup, kNow + 60000, 0, kSrcNtp, kFlagActingBackup).verdict == Verdict::Applied);
    // 1b: mesh mode off: no time is taken (the status says so), whatever else.
    Follower off;
    off.in.meshOn = false;
    CHECK(off.frame(kHub, kNow + 60000).verdict == Verdict::IgnoredMeshOff);
    off.in.clockSet = false;
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
      bad = named.frame(kOther, hostile, 100);
      CHECK(bad.verdict == Verdict::IgnoredNotNamed && bad.candidateEpochMs == 0 && bad.differenceMs == 0);
      Follower none;
      none.primary = none.backup = nullptr;
      bad = none.frame(kHub, hostile, 100);
      CHECK(bad.verdict == Verdict::IgnoredNoMaster && bad.candidateEpochMs == 0 && bad.differenceMs == 0);
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
                                  OwnSource::Ring, OwnSource::Cm5, OwnSource::Mesh, OwnSource::MeshNtp };
    for (OwnSource source : sources) {
      // The clock's rank table (System_ClockCore.h): a master whose time is
      // from NTP outranks the CM5, the chip and the ring; this device's own
      // NTP and a manual time outrank it.
      const bool ownOutranksNtpMaster = source == OwnSource::Ntp || source == OwnSource::Manual;
      // Any other master's time is last: only a clock that mesh time set, or
      // one carried over a restart, takes it.
      const bool ownOutranksPlainMaster = !(source == OwnSource::None || source == OwnSource::Mesh ||
                                            source == OwnSource::MeshNtp);
      CHECK(frameMayCorrect(kSrcNtp, source) == !ownOutranksNtpMaster);
      for (uint8_t plain : { kSrcRtc, kSrcManual, kSrcCm5, (uint8_t)5, (uint8_t)127 }) {
        CHECK(frameMayCorrect(plain, source) == !ownOutranksPlainMaster);
        Follower other;
        other.in.ownSource = source;
        CHECK(other.frame(kHub, kNow + 60000, 0, plain).verdict ==
              (ownOutranksPlainMaster ? Verdict::IgnoredBetterSource : Verdict::Applied));
      }
      Follower f;
      f.in.ownSource = source;
      const Decision d = f.frame(kHub, kNow + 60000);
      CHECK(d.verdict == (ownOutranksNtpMaster ? Verdict::IgnoredBetterSource : Verdict::Applied));
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
  CHECK(is(verdictKey(Verdict::IgnoredNoMaster), "ignored_no_master") && verdictIsIgnored(Verdict::IgnoredNoMaster));
  CHECK(is(verdictKey(Verdict::IgnoredNotNamed), "ignored_not_named") && verdictIsIgnored(Verdict::IgnoredNotNamed));
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
  // Not confirmed: by the other named master. Its frames do not clear the memory either.
  {
    Follower f;
    f.frame(kHub, wrong);
    f.pass(15000);
    CHECK(f.frame(kBackup, wrong + 15000).verdict == Verdict::HeldLargeChange);
    CHECK(std::memcmp(f.st.holdMac, kHub, 6) == 0);
    CHECK(f.frame(kBackup, kNow + 15000 + 100, 0, 0).verdict == Verdict::RefusedBadTime);
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
    // The other named master may take a claim that has run out.
    Follower g;
    g.frame(kHub, wrong);
    g.pass(kHoldMaxMs + 1);
    CHECK(g.frame(kBackup, wrong).verdict == Verdict::HeldLargeChange);
    CHECK(std::memcmp(g.st.holdMac, kBackup, 6) == 0);
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
  // The primary and the backup both send time (both act as master), 20 s
  // apart. The follower takes the first and does not step back and forth.
  {
    Follower f;
    CHECK(f.frame(kHub, kNow + 60000).verdict == Verdict::Applied);
    f.pass(30000);
    f.in.nowEpochMs = kNow + 60000 + 30000;  // the clock was set
    CHECK(f.frame(kBackup, f.in.nowEpochMs + 20000, 0, kSrcNtp, kFlagActingBackup).verdict ==
          Verdict::IgnoredOtherMaster);
    f.pass(30000);
    CHECK(f.frame(kHub, f.in.nowEpochMs + 100).verdict == Verdict::Agreed);
    f.pass(30000);
    CHECK(f.frame(kBackup, f.in.nowEpochMs + 20000, 0, kSrcNtp, kFlagActingBackup).verdict ==
          Verdict::IgnoredOtherMaster);
    // When the primary falls silent for 25 minutes, the backup is taken.
    f.pass(kStickyMs);
    CHECK(f.frame(kBackup, f.in.nowEpochMs + 20000, 0, kSrcNtp, kFlagActingBackup).verdict == Verdict::Applied);
    CHECK(std::memcmp(f.st.stickyMac, kBackup, 6) == 0);
  }
  // Rank: the primary named here beats the backup.
  {
    Follower f;
    CHECK(f.frame(kBackup, kNow + 60000, 0, kSrcNtp, kFlagActingBackup).verdict == Verdict::Applied);
    CHECK(f.st.stickyRank == 0);
    f.pass(60000);
    f.in.nowEpochMs = kNow + 60000 + 60000;
    // The primary is back and says another time: it outranks the backup at once.
    CHECK(f.frame(kHub, f.in.nowEpochMs + 30000).verdict == Verdict::Applied && f.st.stickyRank == 1);
    f.pass(60000);
    f.in.nowEpochMs += 30000;
    // The backup, still acting, is now ignored, with its acting bit or without.
    CHECK(f.frame(kBackup, f.in.nowEpochMs + 60000, 0, kSrcNtp, kFlagActingBackup).verdict ==
          Verdict::IgnoredOtherMaster);
    CHECK(f.frame(kBackup, f.in.nowEpochMs + 60000).verdict == Verdict::IgnoredOtherMaster);
    // The bit does not rank the backup up either.
    Follower g;
    CHECK(g.frame(kBackup, kNow + 60000).verdict == Verdict::Applied && g.st.stickyRank == 0);
  }
  // A frame that agrees also makes its sender the one followed.
  {
    Follower f;
    CHECK(f.frame(kHub, kNow + 100).verdict == Verdict::Agreed && f.st.stickyHave);
    f.pass(1000);
    CHECK(f.frame(kBackup, f.in.nowEpochMs + 60000).verdict == Verdict::IgnoredOtherMaster);
  }
  // After a step, another step waits 10 s (a peer cannot step the clock at radio rate).
  {
    Follower f;
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
  // The named master is the only gate: its frames step the clock after 10 s
  // every time (no longer wait), and a held change is applied as soon as it
  // is confirmed. With no master named, an hour of frames that are each 59
  // minutes off, one every 10 s, changes nothing.
  {
    Follower n;
    CHECK(n.frame(kHub, kNow + 60000).verdict == Verdict::Applied);
    n.in.nowEpochMs = kNow + 60000;
    n.pass(kApplyGapMs);
    CHECK(n.frame(kHub, n.in.nowEpochMs + 120000).verdict == Verdict::Applied);
    n.in.nowEpochMs += 120000;
    const int64_t wrong = n.in.nowEpochMs + 7200000;
    n.pass(30000);
    CHECK(n.frame(kHub, wrong + 30000).verdict == Verdict::HeldLargeChange);
    n.pass(15000);
    CHECK(n.frame(kHub, wrong + 45000).verdict == Verdict::Applied && !n.st.holdHave);
    for (bool clockSet : { true, false }) {
      Follower none;
      none.primary = none.backup = nullptr;
      none.in.clockSet = clockSet;
      int steps = 0;
      for (int i = 0; i < 360; ++i) {
        const Decision d = none.frame(kOther, none.in.nowEpochMs + 59LL * 60 * 1000);
        if (d.verdict == Verdict::Applied) ++steps;
        CHECK(d.verdict == Verdict::IgnoredNoMaster);
        none.pass(10000);
      }
      CHECK(steps == 0);
    }
  }
}

// Row 0: the masters named here changed.
static void testNamedChange() {
  // The tag: one for each pair of fingerprints, the same in either case.
  const char* a = "6e9c0f7a1b2c3d4e5f60718293a4b5c6d7e8f90a1b2c3d4e5f60718293a4b5c6";
  const char* aUpper = "6E9C0F7A1B2C3D4E5F60718293A4B5C6D7E8F90A1B2C3D4E5F60718293A4B5C6";
  const char* b = "0f1e2d3c4b5a69788796a5b4c3d2e1f00f1e2d3c4b5a69788796a5b4c3d2e1f0";
  CHECK(namedMastersTag(a, "") == namedMastersTag(aUpper, ""));
  CHECK(namedMastersTag(a, "") == namedMastersTag(a, nullptr) && namedMastersTag("", "") == namedMastersTag(nullptr, nullptr));
  CHECK(namedMastersTag(a, "") != namedMastersTag(b, ""));
  CHECK(namedMastersTag(a, "") != namedMastersTag("", ""));    // cleared
  CHECK(namedMastersTag(a, b) != namedMastersTag(b, a));       // swapped
  CHECK(namedMastersTag(a, "") != namedMastersTag("", a));     // primary made backup
  CHECK(namedMastersTag("ab", "") != namedMastersTag("a", "b"));

  // A followed (sticky); the primary renamed to B: B's next frame applies.
  for (const uint8_t* renamed : { kOther, kBackup }) {
    Follower f;
    CHECK(f.frame(kHub, kNow + 60000).verdict == Verdict::Applied && std::memcmp(f.st.stickyMac, kHub, 6) == 0);
    f.pass(60000);
    f.in.nowEpochMs = kNow + 60000 + 60000;
    f.primary = renamed;           // espnowmasterfingerprint <B>
    if (renamed == kBackup) f.backup = kHub;  // ... or the two swapped
    const Decision d = f.frame(renamed, f.in.nowEpochMs + 120000);
    CHECK(d.verdict == Verdict::Applied);
    CHECK(f.st.stickyHave && std::memcmp(f.st.stickyMac, renamed, 6) == 0 && f.st.stickyRank == 1);
  }
  // The same without a change: the sender followed keeps the other one out
  // (control for the case above).
  {
    Follower f;
    CHECK(f.frame(kHub, kNow + 60000).verdict == Verdict::Applied);
    f.pass(60000);
    f.in.nowEpochMs = kNow + 60000 + 60000;
    CHECK(f.frame(kBackup, f.in.nowEpochMs + 120000).verdict == Verdict::IgnoredOtherMaster);
  }
  // A claim held from A is discarded when the names change, even while A
  // stays named (as the backup): A's repeat is a new claim, not a confirmation.
  {
    const int64_t wrong = kNow + 7200000;
    Follower f;
    CHECK(f.frame(kHub, wrong).verdict == Verdict::HeldLargeChange && f.st.holdHave);
    f.primary = kOther;
    f.backup = kHub;
    f.pass(15000);
    CHECK(f.frame(kHub, wrong + 15000).verdict == Verdict::HeldLargeChange);
    CHECK(f.st.holdHave && f.st.holdReceiptMonoMs == f.in.nowMonoMs);
    f.pass(15000);
    CHECK(f.frame(kHub, wrong + 30000).verdict == Verdict::Applied);  // confirmed under the new names
  }
  // Cleared: both go at once, with the frame that finds no master named.
  {
    const int64_t wrong = kNow + 7200000;
    Follower f;
    CHECK(f.frame(kHub, kNow + 100).verdict == Verdict::Agreed && f.st.stickyHave);
    f.pass(1000);
    CHECK(f.frame(kHub, wrong).verdict == Verdict::HeldLargeChange && f.st.holdHave);
    f.primary = f.backup = nullptr;  // espnowmasterfingerprint clear, espnowbackupfingerprint clear
    f.pass(15000);
    CHECK(f.frame(kHub, wrong + 15000).verdict == Verdict::IgnoredNoMaster);
    CHECK(!f.st.stickyHave && !f.st.holdHave);
    f.primary = kHub;                // named again: its claim starts over
    f.pass(1000);
    CHECK(f.frame(kHub, wrong + 16000).verdict == Verdict::HeldLargeChange);
  }
  // The 10 s gap after a step is about this device's clock: it stays.
  {
    Follower f;
    CHECK(f.frame(kHub, kNow + 121000).verdict == Verdict::Applied);
    f.pass(1000);
    f.primary = kOther;
    CHECK(f.frame(kOther, f.in.nowEpochMs + 60000).verdict == Verdict::IgnoredTooSoon);
    f.pass(kApplyGapMs);
    CHECK(f.frame(kOther, f.in.nowEpochMs + 60000).verdict == Verdict::Applied);
  }
}

int main() {
  testFrame();
  testSender();
  testRounds();
  testFollowerRows();
  testLargeChange();
  testTwoSenders();
  testNamedChange();
  std::printf("espnow_time_core_tests: %u checks passed\n", gChecks);
  return 0;
}
