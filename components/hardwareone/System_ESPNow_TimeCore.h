// System_ESPNow_TimeCore.h - rules of mesh time (TIME_SYNC, opcode 35)
//
// The mesh master sends its clock to each paired device over that device's
// session: when the session has come up, every 10 minutes, and when its own
// clock was set. A device that names that master (espnowmasterfingerprint,
// or espnowbackupfingerprint for the backup acting as master) and has no
// better source of its own (RTC, NTP, timeset, CM5) sets its clock from it;
// a device that names no master takes no mesh time. Times are UTC. Pure
// logic on caller-owned state: no heap, no OS, no logging
// (test/host/test_espnow_time_core.cpp). The owner (System_ESPNow.cpp) holds
// the state, reads the clock and sets it.
//
// The frame (16 bytes, little endian), as V4PayloadTimeSync in
// System_ESPNow_Wire.h lays it out:
//
//   0  epochMs      64 bit signed: UTC milliseconds since 1970, the sender's
//                   clock when it built the frame
//   8  source       where the sender's clock came from (kSrc* below); never 0
//   9  flags        bit 0: the sender is a backup master acting as master;
//                   other bits 0. The receiver judges the sender by the
//                   fingerprint named on it, not by this bit
//  10  sinceSetMin  16 bit: minutes since that source last set the sender's
//                   clock; 65535 = that long or longer
//  12  reserved     4 bytes, 0 (room for a later two-way exchange)
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace meshtime {

constexpr size_t kFrameLen = 16;

// `source` on the wire. 1 to 127 are real sources; 128 to 255 are weak ones,
// which a receiver takes only while its own clock is not set. A value this
// firmware does not know is judged by its range alone.
constexpr uint8_t kSrcRtc       = 1;
constexpr uint8_t kSrcNtp       = 2;
constexpr uint8_t kSrcManual    = 3;
constexpr uint8_t kSrcCm5       = 4;
// 5 gps and 6 phone are kept for the day those set a clock.
constexpr uint8_t kSrcWeakFirst = 128;

constexpr uint8_t kFlagActingBackup = 0x01;

inline const char* wireSourceName(uint8_t source) {
  switch (source) {
    case kSrcRtc:    return "rtc";
    case kSrcNtp:    return "ntp";
    case kSrcManual: return "manual";
    case kSrcCm5:    return "cm5";
    default:         return source == 0 ? "none" : (source >= kSrcWeakFirst ? "weak" : "other");
  }
}

// What last set this device's own clock. The values are those of
// Clock::SyncSource (System_Clock.h); System_ESPNow.cpp asserts it.
enum class OwnSource : uint8_t { None = 0, Rtc, Ntp, Manual, Ring, Cm5, Mesh };

// A source a clock can be passed on from, and that mesh time does not
// replace. Not the ring, not mesh time itself, and not a clock that was only
// carried over a soft restart (None while set).
constexpr bool ownSourceIsReal(OwnSource s) {
  return s == OwnSource::Rtc || s == OwnSource::Ntp || s == OwnSource::Manual || s == OwnSource::Cm5;
}

constexpr uint8_t wireSourceFor(OwnSource s) {
  return s == OwnSource::Rtc ? kSrcRtc
       : s == OwnSource::Ntp ? kSrcNtp
       : s == OwnSource::Manual ? kSrcManual
       : s == OwnSource::Cm5 ? kSrcCm5 : (uint8_t)0;
}

struct Frame {
  int64_t  epochMs;
  uint8_t  source;
  uint8_t  flags;
  uint16_t sinceSetMin;
};

inline void packFrame(uint8_t out[kFrameLen], const Frame& f) {
  const uint64_t e = (uint64_t)f.epochMs;
  for (int i = 0; i < 8; ++i) out[i] = (uint8_t)((e >> (8 * i)) & 0xFF);
  out[8] = f.source;
  out[9] = f.flags;
  out[10] = (uint8_t)(f.sinceSetMin & 0xFF);
  out[11] = (uint8_t)(f.sinceSetMin >> 8);
  out[12] = out[13] = out[14] = out[15] = 0;
}

inline bool parseFrame(const uint8_t* in, size_t len, Frame& f) {
  if (!in || len < kFrameLen) return false;
  uint64_t e = 0;
  for (int i = 0; i < 8; ++i) e |= (uint64_t)in[i] << (8 * i);
  f.epochMs = (int64_t)e;
  f.source = in[8];
  f.flags = in[9];
  f.sinceSetMin = (uint16_t)(in[10] | ((uint16_t)in[11] << 8));
  return true;
}

// Minutes since the clock was set, as the frame carries them.
constexpr uint16_t sinceSetMinutes(uint32_t seconds) {
  return (seconds / 60u) >= 65535u ? (uint16_t)65535u : (uint16_t)(seconds / 60u);
}

// 2020-01-01 to 2100-01-01, the bounds of Clock::isPlausibleEpoch.
constexpr int64_t kEpochMinMs = 1577836800LL * 1000;
constexpr int64_t kEpochMaxMs = 4102444800LL * 1000;
constexpr bool epochMsPlausible(int64_t ms) { return ms >= kEpochMinMs && ms < kEpochMaxMs; }

// ---------------------------------------------------------------------------
// Who sends.
// ---------------------------------------------------------------------------
enum class SendWhyNot : uint8_t {
  Sends = 0,
  Off,               // the setting meshTime is off
  MeshOff,
  NotMaster,         // only the acting master sends
  ClockNotSet,
  SourceNotReal,     // ring, mesh, or carried over a restart
  ClockUnconfirmed,  // an NTP reply moved the clock by more than a day and nothing confirmed it
};

struct SenderInputs {
  bool      settingOn;
  bool      meshOn;
  bool      actingMaster;   // mesh role master; a backup has it only while promoted
  bool      clockSet;
  bool      clockHeld;      // Clock::confirmedClockHeld()
  OwnSource source;
};

inline SendWhyNot senderDecide(const SenderInputs& in) {
  if (!in.settingOn) return SendWhyNot::Off;
  if (!in.meshOn) return SendWhyNot::MeshOff;
  if (!in.actingMaster) return SendWhyNot::NotMaster;
  if (!in.clockSet) return SendWhyNot::ClockNotSet;
  if (!ownSourceIsReal(in.source)) return SendWhyNot::SourceNotReal;
  if (in.clockHeld) return SendWhyNot::ClockUnconfirmed;
  return SendWhyNot::Sends;
}

// The reason's name in JSON; nullptr when the device sends.
inline const char* sendWhyNotKey(SendWhyNot w) {
  switch (w) {
    case SendWhyNot::Sends:            return nullptr;
    case SendWhyNot::Off:              return "off";
    case SendWhyNot::MeshOff:          return "mesh_off";
    case SendWhyNot::NotMaster:        return "not_master";
    case SendWhyNot::ClockNotSet:      return "clock_not_set";
    case SendWhyNot::SourceNotReal:    return "source_not_real";
    case SendWhyNot::ClockUnconfirmed: return "clock_unconfirmed";
  }
  return "?";
}

// ---------------------------------------------------------------------------
// When. A "round" sends one frame to every paired peer with a session. A
// round starts 10 minutes after the last one, when this device's clock was
// set since (stepSerial changed), once more 15 s after such a round (so a
// follower that held a large change back can confirm it), and by hand
// (espnowtimesync). Besides the rounds, a peer whose session has come up
// gets one frame. A session is used only once it is kSettleMs old: the side
// that answers a session open is ready before the other side is.
// The owner sends to one peer per kPeerGapMs.
// ---------------------------------------------------------------------------
constexpr uint32_t kRoundIntervalMs = 600000;
constexpr uint32_t kStepRepeatMs    = 15000;
constexpr uint32_t kSettleMs        = 2000;
constexpr uint32_t kPeerGapMs       = 500;

struct RoundState {
  bool     everRound;
  bool     repeatPending;
  uint32_t lastRoundMs;
  uint32_t repeatAtMs;
  uint32_t stepSerialAtRound;
  uint32_t serial;            // number of the current round; 0 = none yet
};

// True when a new round starts now; `serial` is then the new round's number.
inline bool roundBegin(RoundState& r, uint32_t nowMs, uint32_t stepSerial, bool byHand) {
  const bool stepped = r.everRound && stepSerial != r.stepSerialAtRound;
  const bool timed = r.everRound && (uint32_t)(nowMs - r.lastRoundMs) >= kRoundIntervalMs;
  const bool repeat = r.repeatPending && (int32_t)(nowMs - r.repeatAtMs) >= 0;
  if (r.everRound && !stepped && !timed && !repeat && !byHand) return false;
  r.repeatPending = stepped;
  if (stepped) r.repeatAtMs = nowMs + kStepRepeatMs;
  r.everRound = true;
  r.lastRoundMs = nowMs;
  r.stepSerialAtRound = stepSerial;
  if (++r.serial == 0) r.serial = 1;
  return true;
}

// One per paired peer on the sender.
struct PeerSendState {
  bool     inUse;
  bool     sentThisSession;
  uint8_t  mac[6];
  uint32_t sessionEstablishedAtMs;
  uint32_t roundSerialSent;
  uint32_t lastSentMs;
};

// A session is usable for a new sender's first frame. A session that came up
// after `nowMs` was read (its stamp is a little ahead: another task set it
// up during this pass) is not settled yet, rather than 49 days old.
constexpr uint32_t kStampAheadSlackMs = 60000;
constexpr bool sessionSettled(uint32_t establishedAtMs, uint32_t nowMs) {
  return (uint32_t)(establishedAtMs - nowMs) > kStampAheadSlackMs &&
         (uint32_t)(nowMs - establishedAtMs) >= kSettleMs;
}

// True when `p` is owed a frame: its session is settled and either came up
// since the last frame to it, or the current round has not reached it.
inline bool peerDue(const PeerSendState& p, const RoundState& r, bool sessionActive,
                    uint32_t establishedAtMs, uint32_t nowMs) {
  if (!sessionActive || !sessionSettled(establishedAtMs, nowMs)) return false;
  const bool newSession = !p.sentThisSession || p.sessionEstablishedAtMs != establishedAtMs;
  return newSession || (r.serial != 0 && p.roundSerialSent != r.serial);
}

inline void peerSent(PeerSendState& p, const RoundState& r, uint32_t establishedAtMs, uint32_t nowMs) {
  p.sentThisSession = true;
  p.sessionEstablishedAtMs = establishedAtMs;
  p.roundSerialSent = r.serial;
  p.lastSentMs = nowMs;
}

// ---------------------------------------------------------------------------
// What a receiver does with a frame. First match:
//
//    1  the setting meshTime is off                         ignored_off
//   1b  mesh mode is off                                    ignored_mesh_off
//    2  this device is the acting master                    ignored_master
//    3  no master is named on this device                   ignored_no_master
//   3b  the sender is not the master named here             ignored_not_named
//    4  source 0, or the time is not within 2020-2099       refused_bad_time
//    5  own clock set from rtc, ntp, manual or cm5          ignored_better_source
//    6  a weak source while the own clock is set            ignored_weak_source
//    7  another sender's time is being followed             ignored_other_master
//    8  own clock not set                                   applied
//    9  a step was applied under 10 s ago                   ignored_too_soon
//   10  difference under 5 s                                agreed
//   11  difference up to 1 hour                             applied
//   12  difference over 1 hour                              held_large_change,
//       or applied when the same sender says it again
//
// The time judged is the frame's time plus how long the frame waited here.
// A frame whose own time is not a real date is refused before anything is
// computed from it.
//
// 3: mesh time comes only from the master named on this device: the primary
// (espnowmasterfingerprint), or the backup (espnowbackupfingerprint), which
// sends time only while it acts as master. With neither named, no frame is
// taken, whoever sends it.
//
// 7: after a frame from S was applied or agreed, S is followed for
// kStickyMs. A frame from the other named master is ignored meanwhile unless
// it outranks S: the primary (rank 1) over the backup (rank 0). The primary
// and a backup that both act as master cannot step this device back and
// forth, and a returning primary takes over from the backup at once.
//
// 12: the first such frame is remembered with its sender. A later frame from
// the same sender, kHoldConfirmMinMs to kHoldMaxMs after it, that claims the
// same time (its time moved on by as much as the receipts are apart, within
// kHoldAgreeMs) is applied. A master whose clock was wrong and was then
// corrected can so correct its followers, and one stale frame cannot.
// ---------------------------------------------------------------------------
constexpr int64_t kAgreeBandMs      = 5000;
constexpr int64_t kLargeChangeMs    = 3600LL * 1000;
constexpr int64_t kStickyMs         = 25LL * 60 * 1000;
constexpr int64_t kApplyGapMs       = 10000;
constexpr int64_t kHoldConfirmMinMs = 10000;
constexpr int64_t kHoldMaxMs        = 30LL * 60 * 1000;
constexpr int64_t kHoldAgreeMs      = 5000;

enum class Verdict : uint8_t {
  Applied = 0,
  Agreed,
  IgnoredOff,
  IgnoredMeshOff,
  IgnoredNoMaster,
  IgnoredNotNamed,
  IgnoredMaster,
  IgnoredBetterSource,
  IgnoredWeakSource,
  IgnoredOtherMaster,
  IgnoredTooSoon,
  RefusedBadTime,
  HeldLargeChange,
  Count
};
constexpr size_t kVerdictCount = (size_t)Verdict::Count;

inline const char* verdictKey(Verdict v) {
  switch (v) {
    case Verdict::Applied:             return "applied";
    case Verdict::Agreed:              return "agreed";
    case Verdict::IgnoredOff:          return "ignored_off";
    case Verdict::IgnoredMeshOff:      return "ignored_mesh_off";
    case Verdict::IgnoredNoMaster:     return "ignored_no_master";
    case Verdict::IgnoredNotNamed:     return "ignored_not_named";
    case Verdict::IgnoredMaster:       return "ignored_master";
    case Verdict::IgnoredBetterSource: return "ignored_better_source";
    case Verdict::IgnoredWeakSource:   return "ignored_weak_source";
    case Verdict::IgnoredOtherMaster:  return "ignored_other_master";
    case Verdict::IgnoredTooSoon:      return "ignored_too_soon";
    case Verdict::RefusedBadTime:      return "refused_bad_time";
    case Verdict::HeldLargeChange:     return "held_large_change";
    case Verdict::Count:               break;
  }
  return "?";
}

constexpr bool verdictIsIgnored(Verdict v) {
  return v == Verdict::IgnoredOff || v == Verdict::IgnoredMeshOff || v == Verdict::IgnoredNoMaster ||
         v == Verdict::IgnoredNotNamed ||
         v == Verdict::IgnoredMaster ||
         v == Verdict::IgnoredBetterSource || v == Verdict::IgnoredWeakSource ||
         v == Verdict::IgnoredOtherMaster || v == Verdict::IgnoredTooSoon;
}

struct FollowerInputs {
  bool      settingOn;
  bool      meshOn;                // mesh mode
  bool      hasNamedMaster;        // a master or backup fingerprint is set on this device
  bool      senderIsNamed;         // the sender is that master or backup
  bool      senderIsNamedPrimary;  // the sender is the primary of the two
  bool      actingMaster;
  bool      clockSet;
  OwnSource ownSource;
  int64_t   nowEpochMs;            // this device's clock (meaningful when clockSet)
  int64_t   nowMonoMs;             // monotonic time now
};

struct Received {
  Frame   frame;
  uint8_t sender[6];
  int64_t receiptMonoMs;           // monotonic time the frame arrived
};

struct FollowerState {
  bool    holdHave;
  uint8_t holdMac[6];
  int64_t holdEpochMs;
  int64_t holdReceiptMonoMs;
  bool    stickyHave;
  uint8_t stickyMac[6];
  uint8_t stickyRank;
  int64_t stickyMonoMs;
  bool    appliedHave;
  int64_t appliedMonoMs;
};

struct Decision {
  Verdict verdict;
  int64_t candidateEpochMs;  // what the clock is set to when applied
  int64_t differenceMs;      // candidate minus the own clock (0 when that is not set)
};

// Only a named master gets this far: the primary outranks the backup.
inline uint8_t senderRank(const FollowerInputs& in) { return in.senderIsNamedPrimary ? 1 : 0; }

// Judges one frame and updates `st`. The caller sets the clock when the
// verdict is Applied; it works on a copy of `st` and keeps the copy only when
// it did set the clock (see espnowMeshTimeApplyTick).
inline Decision followerDecide(FollowerState& st, const FollowerInputs& in, const Received& rx) {
  Decision d = { Verdict::RefusedBadTime, 0, 0 };
  const Frame& f = rx.frame;
  // Nothing is computed from a time that is not a real date (a hostile
  // INT64_MAX must not overflow): candidate and difference stay 0 for it.
  const bool frameTimeReal = epochMsPlausible(f.epochMs);
  if (frameTimeReal) {
    int64_t waited = in.nowMonoMs - rx.receiptMonoMs;
    if (waited < 0) waited = 0;
    d.candidateEpochMs = f.epochMs + waited;
    if (in.clockSet) d.differenceMs = d.candidateEpochMs - in.nowEpochMs;
  }

  if (!in.settingOn) { d.verdict = Verdict::IgnoredOff; return d; }
  if (!in.meshOn) { d.verdict = Verdict::IgnoredMeshOff; return d; }
  if (in.actingMaster) { d.verdict = Verdict::IgnoredMaster; return d; }
  if (!in.hasNamedMaster) { d.verdict = Verdict::IgnoredNoMaster; return d; }
  if (!in.senderIsNamed) { d.verdict = Verdict::IgnoredNotNamed; return d; }
  if (f.source == 0 || !frameTimeReal || !epochMsPlausible(d.candidateEpochMs)) {
    d.verdict = Verdict::RefusedBadTime;
    return d;
  }
  if (in.clockSet && ownSourceIsReal(in.ownSource)) { d.verdict = Verdict::IgnoredBetterSource; return d; }
  if (in.clockSet && f.source >= kSrcWeakFirst) { d.verdict = Verdict::IgnoredWeakSource; return d; }

  const uint8_t rank = senderRank(in);
  const bool sameSticky = st.stickyHave && memcmp(st.stickyMac, rx.sender, 6) == 0;
  if (st.stickyHave && !sameSticky && (in.nowMonoMs - st.stickyMonoMs) < kStickyMs &&
      rank <= st.stickyRank) {
    d.verdict = Verdict::IgnoredOtherMaster;
    return d;
  }
  const bool sameHold = st.holdHave && memcmp(st.holdMac, rx.sender, 6) == 0;
  auto follow = [&]() {
    st.stickyHave = true;
    memcpy(st.stickyMac, rx.sender, 6);
    st.stickyRank = rank;
    st.stickyMonoMs = in.nowMonoMs;
    if (sameHold) st.holdHave = false;
  };
  auto apply = [&]() {
    follow();
    st.appliedHave = true;
    st.appliedMonoMs = in.nowMonoMs;
    d.verdict = Verdict::Applied;
    return d;
  };

  if (!in.clockSet) return apply();
  if (st.appliedHave && (in.nowMonoMs - st.appliedMonoMs) < kApplyGapMs) {
    d.verdict = Verdict::IgnoredTooSoon;
    return d;
  }
  const int64_t diff = d.differenceMs < 0 ? -d.differenceMs : d.differenceMs;
  if (diff < kAgreeBandMs) {
    follow();
    d.verdict = Verdict::Agreed;
    return d;
  }
  if (diff <= kLargeChangeMs) return apply();

  // Over an hour.
  if (sameHold) {
    const int64_t apart = rx.receiptMonoMs - st.holdReceiptMonoMs;
    int64_t drift = (f.epochMs - st.holdEpochMs) - apart;
    if (drift < 0) drift = -drift;
    if (apart >= kHoldConfirmMinMs && apart <= kHoldMaxMs && drift <= kHoldAgreeMs) {
      return apply();
    }
    // Said again too early: the first claim stands and is confirmed later.
    if (apart < kHoldConfirmMinMs && drift <= kHoldAgreeMs) {
      d.verdict = Verdict::HeldLargeChange;
      return d;
    }
  }
  // A new claim is remembered when none is, when the one remembered is this
  // sender's, or when it has run out. Another sender's live claim stays.
  const bool holdLive = st.holdHave && (in.nowMonoMs - st.holdReceiptMonoMs) <= kHoldMaxMs;
  if (!holdLive || sameHold) {
    st.holdHave = true;
    memcpy(st.holdMac, rx.sender, 6);
    st.holdEpochMs = f.epochMs;
    st.holdReceiptMonoMs = rx.receiptMonoMs;
  }
  d.verdict = Verdict::HeldLargeChange;
  return d;
}

}  // namespace meshtime
