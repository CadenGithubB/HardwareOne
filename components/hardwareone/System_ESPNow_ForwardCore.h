// System_ESPNow_ForwardCore.h - rules of forwarding a bond partner's data to the mesh master
//
// A bond worker (X) sends its readings, battery state and events to its bond
// master (B) only. B may forward them to the mesh master (M) named on B: a
// setting on B (bondforward, off by default). B announces the link in its own
// frame, FORWARD_LINK (155), and M answers; only then are X's frames sent on,
// as ordinary SENSOR_ENVELOPE / POWER_STATUS / EVENT_PUSH frames from B whose
// reserved link byte (and, for readings and events, a reserved flag bit)
// says "this is my bond partner's". On M the partner is its own sender, kept
// under (partner, via B), with its own counters.
//
// This header has the rules only: the 96-byte announcement frame, whether B
// forwards and why not, B's state per receiving master, M's link table entry
// and how announcements, withdrawals and forwarded frames are judged. Pure
// logic on caller-owned state: no heap, no OS, no logging
// (test/host/test_espnow_forward_core.cpp). The owners are
// System_ESPNow_BondForward.cpp (B) and System_ESPNow_ForwardRx.cpp (M).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "System_ESPNow_SensorCore.h"  // sensorBit, kSensorMaskAll, roleSends / roleReceives

namespace espnowfwd {

inline bool macIsZero(const uint8_t m[6]) {
  for (int i = 0; i < 6; ++i) {
    if (m[i]) return false;
  }
  return true;
}
inline bool macSame(const uint8_t a[6], const uint8_t b[6]) { return memcmp(a, b, 6) == 0; }

// ---------------------------------------------------------------------------
// How a kept record came to this device: its origin sent it over the mesh,
// over the bond link (to its bond master), or a bond master forwarded it.
// ---------------------------------------------------------------------------
constexpr uint8_t kLinkMesh      = 0;
constexpr uint8_t kLinkBond      = 1;
constexpr uint8_t kLinkForwarded = 2;

inline const char* linkKey(uint8_t link) {
  return link == kLinkBond ? "bond" : (link == kLinkForwarded ? "forwarded" : "mesh");
}

// ---------------------------------------------------------------------------
// The streams, as bits of the announcement's `streams`, and as stream numbers
// of the resume mask and of `attribute`: 0 to 10 a sensor type, 11 the
// battery frame, 12 the event frame.
// ---------------------------------------------------------------------------
constexpr uint8_t kStreamReadings = 0x01;
constexpr uint8_t kStreamBattery  = 0x02;
constexpr uint8_t kStreamEvents   = 0x04;
constexpr uint8_t kStreamsAll     = 0x07;

constexpr uint8_t kStreamIdBattery = REMOTE_SENSOR_MAX;      // 11
constexpr uint8_t kStreamIdEvents  = REMOTE_SENSOR_MAX + 1;  // 12
constexpr uint8_t kStreamIdCount   = REMOTE_SENSOR_MAX + 2;
constexpr uint16_t kResumeAll = (uint16_t)((1u << kStreamIdCount) - 1u);
static_assert(kStreamIdCount <= 16, "the resume mask holds every stream");

constexpr uint8_t streamBitOf(uint8_t streamId) {
  return streamId < REMOTE_SENSOR_MAX ? kStreamReadings
       : (streamId == kStreamIdBattery ? kStreamBattery : (streamId == kStreamIdEvents ? kStreamEvents : 0));
}

// Which sensors B forwards: every one but the input device by default (10 Hz,
// meant for the bond master's own screen).
constexpr uint16_t kSensorMaskDefault =
    (uint16_t)(espnowsensor::kSensorMaskAll & ~espnowsensor::sensorBit(REMOTE_SENSOR_INPUT));

// The streams B forwards to a receiving master: readings to the primary only
// (B's own rule for readings switched on by hand), battery and events to both.
constexpr uint8_t kTargetPrimary = 0;
constexpr uint8_t kTargetBackup  = 1;
constexpr uint8_t streamsForTarget(uint8_t kind) {
  return kind == kTargetPrimary ? kStreamsAll : (uint8_t)(kStreamBattery | kStreamEvents);
}

// ---------------------------------------------------------------------------
// FORWARD_LINK (155), 96 bytes, little endian (V4PayloadForwardLink):
//
//   0  op          1 announce (B to M), 2 withdraw (B to M), 3 query (M to B),
//                  4 answer (M to B)
//   1  linkGen     B's link number. A withdrawal of everything: B's last link
//                  number (0: none this boot). A query: 0
//   2  viaBootTag  B's boot tag (an answer echoes it; 0 in a query)
//   4  streams     bit 0 readings, bit 1 battery, bit 2 events (echoed)
//   5  result      answer only (kResult*)
//   6  answers     answer only: the op answered (1 or 2)
//   7  reserved    0
//   8  partnerMac  X's address; all zero in a withdrawal of everything and in
//                  a query
//  14  sensorMask  bit i: sensor i is forwarded (with streams bit 0); echoed
//  16  partnerKey  X's Ed25519 public key as B holds it (announce)
//  48  partnerName the name B shows for X, zero padded, not terminated when
//                  full, cut on a UTF-8 boundary (announce)
//  80  reserved    16 bytes, 0
// ---------------------------------------------------------------------------
constexpr size_t kLinkFrameLen = 96;
constexpr size_t kPartnerKeyLen = 32;
constexpr size_t kPartnerNameLen = 32;

constexpr uint8_t kOpAnnounce = 1;
constexpr uint8_t kOpWithdraw = 2;
constexpr uint8_t kOpQuery    = 3;
constexpr uint8_t kOpAnswer   = 4;

constexpr uint8_t kResultNone        = 0;
constexpr uint8_t kResultAccepted    = 1;
constexpr uint8_t kResultWithdrawn   = 2;
constexpr uint8_t kResultNotReceiver = 3;
constexpr uint8_t kResultBadPartner  = 4;
constexpr uint8_t kResultKeyMismatch = 5;
constexpr uint8_t kResultFull        = 6;
constexpr uint8_t kResultStale       = 7;
constexpr uint8_t kResultNotAllowed  = 8;
constexpr uint8_t kResultNameTaken   = 9;
constexpr uint8_t kResultCount       = 10;

struct LinkFrame {
  uint8_t  op;
  uint8_t  linkGen;
  uint16_t viaBootTag;
  uint8_t  streams;
  uint8_t  result;
  uint8_t  answers;
  uint8_t  partnerMac[6];
  uint16_t sensorMask;
  uint8_t  partnerKey[kPartnerKeyLen];
  char     partnerName[kPartnerNameLen];  // as on the wire
};

inline void packLinkFrame(uint8_t out[kLinkFrameLen], const LinkFrame& f) {
  memset(out, 0, kLinkFrameLen);
  out[0] = f.op;
  out[1] = f.linkGen;
  out[2] = (uint8_t)(f.viaBootTag & 0xFF);
  out[3] = (uint8_t)(f.viaBootTag >> 8);
  out[4] = (uint8_t)(f.streams & kStreamsAll);
  out[5] = f.result;
  out[6] = f.answers;
  memcpy(out + 8, f.partnerMac, 6);
  out[14] = (uint8_t)(f.sensorMask & 0xFF);
  out[15] = (uint8_t)(f.sensorMask >> 8);
  memcpy(out + 16, f.partnerKey, kPartnerKeyLen);
  memcpy(out + 48, f.partnerName, kPartnerNameLen);
}

// False for a payload shorter than the frame or an op this firmware does not
// know. A longer payload is read up to the frame's length.
inline bool parseLinkFrame(const uint8_t* in, size_t len, LinkFrame& f) {
  if (!in || len < kLinkFrameLen) return false;
  if (in[0] < kOpAnnounce || in[0] > kOpAnswer) return false;
  f.op = in[0];
  f.linkGen = in[1];
  f.viaBootTag = (uint16_t)(in[2] | ((uint16_t)in[3] << 8));
  f.streams = (uint8_t)(in[4] & kStreamsAll);
  f.result = in[5];
  f.answers = in[6];
  memcpy(f.partnerMac, in + 8, 6);
  f.sensorMask = (uint16_t)((in[14] | ((uint16_t)in[15] << 8)) & espnowsensor::kSensorMaskAll);
  memcpy(f.partnerKey, in + 16, kPartnerKeyLen);
  memcpy(f.partnerName, in + 48, kPartnerNameLen);
  return true;
}

inline const char* resultKey(uint8_t result) {
  switch (result) {
    case kResultAccepted:    return "accepted";
    case kResultWithdrawn:   return "withdrawn";
    case kResultNotReceiver: return "not_receiver";
    case kResultBadPartner:  return "bad_partner";
    case kResultKeyMismatch: return "key_mismatch";
    case kResultFull:        return "full";
    case kResultStale:       return "stale";
    case kResultNotAllowed:  return "not_allowed";
    case kResultNameTaken:   return "name_taken";
    default:                 return "refused";
  }
}

// After "<master> refused: ".
inline const char* resultText(uint8_t result) {
  switch (result) {
    case kResultAccepted:    return "accepted";
    case kResultWithdrawn:   return "the link was ended there";
    case kResultNotReceiver: return "it is not a mesh master or backup master";
    case kResultBadPartner:  return "the partner's address or key is not valid";
    case kResultKeyMismatch: return "it has the partner paired under another key";
    case kResultFull:        return "it already keeps the links of 16 devices";
    case kResultStale:       return "the announcement was older than one it already had";
    case kResultNotAllowed:  return "it does not accept forwarded data from this device (espnowforwardfrom there)";
    case kResultNameTaken:   return "the partner's name is taken by another device there";
    default:                 return "for a reason this firmware does not know";
  }
}

// ---------------------------------------------------------------------------
// Link numbers. B's linkGen is 1..255 (never 0, which marks "not forwarded"
// in a frame), one per boot, stepped for every new link. Two numbers of the
// same boot are ordered in u8 serial arithmetic.
// ---------------------------------------------------------------------------
constexpr uint8_t linkGenNext(uint8_t gen) { return gen >= 255 ? (uint8_t)1 : (uint8_t)(gen + 1); }

// True when `a` is newer than `b`: (a - b) mod 256 in 1..127.
constexpr bool serialNewer(uint8_t a, uint8_t b) {
  return (uint8_t)(a - b) >= 1 && (uint8_t)(a - b) <= 127;
}

// ---------------------------------------------------------------------------
// Whether B forwards, and why not. The keys are on the wire of the JSON
// replies; the words follow "nothing is forwarded yet: ".
// ---------------------------------------------------------------------------
enum class WhyNot : uint8_t {
  Forwards = 0,
  Off,              // bondforward is off
  NotBondMaster,    // bond mode off, no partner, partner not paired, or this device is the worker
  BondNotAgreed,    // the partner declined, roles conflict, or it never agreed
  MeshOff,
  IsMaster,         // this device is the acting mesh master: X's data is kept here
  NoMaster,         // no master is named on this device
  MasterNotPaired,  // ... none of them is paired
  TargetIsPartner,  // the named master is the bond partner itself (that target is skipped)
  NoSession,        // the master has no session with this device
  NotAccepted,      // announced, no answer yet
  Refused,          // the master refused the link
  QueueFull,        // the send queue took no more
  TooLong,          // a reading over 472 bytes (cannot happen: B keeps at most that)
  PartnerChanged,   // the frame came from another device than the link's partner, or a partner change ran
  SensorOff,        // this sensor is not forwarded (bondforward sensors)
  NotKeptHere,      // this device could not keep the frame (no record, no memory)
  Count
};
constexpr size_t kWhyNotCount = (size_t)WhyNot::Count;

inline const char* whyNotKey(WhyNot why) {
  switch (why) {
    case WhyNot::Forwards:        return "forwards";
    case WhyNot::Off:             return "off";
    case WhyNot::NotBondMaster:   return "not_bond_master";
    case WhyNot::BondNotAgreed:   return "bond_not_agreed";
    case WhyNot::MeshOff:         return "mesh_off";
    case WhyNot::IsMaster:        return "is_master";
    case WhyNot::NoMaster:        return "no_master";
    case WhyNot::MasterNotPaired: return "master_not_paired";
    case WhyNot::TargetIsPartner: return "target_is_partner";
    case WhyNot::NoSession:       return "no_session";
    case WhyNot::NotAccepted:     return "not_accepted";
    case WhyNot::Refused:         return "refused";
    case WhyNot::QueueFull:       return "queue_full";
    case WhyNot::TooLong:         return "too_long";
    case WhyNot::PartnerChanged:  return "partner_changed";
    case WhyNot::SensorOff:       return "sensor_off";
    case WhyNot::NotKeptHere:     return "not_kept_here";
    case WhyNot::Count:           break;
  }
  return "?";
}

inline const char* whyNotText(WhyNot why) {
  switch (why) {
    case WhyNot::Forwards:        return "forwarded";
    case WhyNot::Off:             return "forwarding is off (bondforward on)";
    case WhyNot::NotBondMaster:   return "only a bond master with a paired partner forwards";
    case WhyNot::BondNotAgreed:   return "the partner has not agreed to the bond";
    case WhyNot::MeshOff:         return "mesh mode is off";
    case WhyNot::IsMaster:        return "this device is the mesh master: the partner's data is kept here";
    case WhyNot::NoMaster:        return "no master is named on this device (espnowmasterfingerprint)";
    case WhyNot::MasterNotPaired: return "the master named on this device is not paired with it";
    case WhyNot::TargetIsPartner: return "the master named here is the bond partner itself";
    case WhyNot::NoSession:       return "the master has no session with this device yet";
    case WhyNot::NotAccepted:     return "the master has not accepted yet";
    case WhyNot::Refused:         return "the master refused";
    case WhyNot::QueueFull:       return "the send queue is full";
    case WhyNot::TooLong:         return "the reading is over the 472-byte limit";
    case WhyNot::PartnerChanged:  return "it came from another device than the partner the link names";
    case WhyNot::SensorOff:       return "that sensor is not forwarded (bondforward sensors)";
    case WhyNot::NotKeptHere:     return "this device could not keep it (no record or no memory left)";
    case WhyNot::Count:           break;
  }
  return "?";
}

// ---- The partner's agreement ------------------------------------------------
// The agreement as the bond code keeps it (BondAgreementState: 0 unknown,
// 1 agreed, 2 declined). An unknown answer after the partner agreed once in
// this hold epoch keeps a standing link: a partner that is out of range has
// no session, and nothing of it reaches this device anyway. A partner change
// steps the epoch and forgets the agreement; so does a decline.
constexpr uint8_t kAgreeUnknown  = 0;
constexpr uint8_t kAgreeAgreed   = 1;
constexpr uint8_t kAgreeDeclined = 2;

struct AgreeMemo {
  bool     agreedOnce;
  uint32_t epoch;
};

inline bool agreementAllows(AgreeMemo& m, uint8_t state, bool roleConflict, uint32_t epoch) {
  if (m.epoch != epoch) {
    m.agreedOnce = false;
    m.epoch = epoch;
  }
  if (state == kAgreeDeclined || (state == kAgreeAgreed && roleConflict)) {
    m.agreedOnce = false;
    return false;
  }
  if (state == kAgreeAgreed) m.agreedOnce = true;
  return m.agreedOnce;
}

// ---- forwardDecide, first match ----------------------------------------------
struct DecideInputs {
  bool    settingOn;
  bool    bondMaster;      // bond mode on, a paired partner, and this device's bond role is master
  bool    agreementOk;     // agreementAllows
  bool    meshOn;
  uint8_t meshRole;
  bool    masterNamed;     // a master or backup master fingerprint is set
  uint8_t targetsPaired;   // named masters that are paired (0 to 2)
};

inline WhyNot forwardDecide(const DecideInputs& in) {
  if (!in.settingOn) return WhyNot::Off;
  if (!in.bondMaster) return WhyNot::NotBondMaster;
  if (!in.agreementOk) return WhyNot::BondNotAgreed;
  if (!in.meshOn) return WhyNot::MeshOff;
  if (!espnowsensor::roleSends(in.meshRole)) return WhyNot::IsMaster;
  if (!in.masterNamed) return WhyNot::NoMaster;
  if (in.targetsPaired == 0) return WhyNot::MasterNotPaired;
  return WhyNot::Forwards;
}

// The one rule that keeps a bond worker's data off the mesh: a bond worker
// sends readings, battery state, events and sensor status to its bond master
// only, and takes no sensor requests from a mesh master.
constexpr bool meshDataAllowed(bool bondWorker) { return !bondWorker; }

// What B shows of its forwarding. The "forward" object of 'bondstatus json' and
// GET /api/bond/status is at most kStatusJsonCap bytes with every count at its
// largest, every reason and the longest names (the host suite
// espnow_bond_forward proves it). The forwarding lines of 'bondstatus' go to
// the console one by one, which takes at most kStatusLineMax bytes per line
// (DEBUG_MSG_SIZE - 1); kStatusLinesCap holds all of them.
constexpr size_t kStatusJsonCap = 2560;
constexpr size_t kStatusLineMax = 255;
constexpr size_t kStatusLinesCap = 768;

// ---------------------------------------------------------------------------
// B's standing link: the partner it names, that partner's key, and the bond
// hold epoch at the time it was made. A frame is forwarded only when it came
// from that partner, no partner change ran since, the setting is on and the
// partner has not declined. `tickWhy` is what forwardDecide said at the last
// tick (the reason when there is no link).
// ---------------------------------------------------------------------------
struct StandingLink {
  bool     active;
  uint8_t  partner[6];
  uint8_t  partnerKey[kPartnerKeyLen];
  uint32_t epoch;
  uint8_t  linkGen;
};

inline WhyNot forwardFrameAllowed(const StandingLink& link, const uint8_t src[6], bool settingOn, bool declined,
                                  uint32_t epoch, WhyNot tickWhy) {
  if (!settingOn) return WhyNot::Off;
  if (!link.active) return tickWhy != WhyNot::Forwards ? tickWhy : WhyNot::NotAccepted;
  if (!src || !macSame(src, link.partner) || epoch != link.epoch) return WhyNot::PartnerChanged;
  if (declined) return WhyNot::BondNotAgreed;
  return WhyNot::Forwards;
}

// ---------------------------------------------------------------------------
// B's state per receiving master. The owner keeps one per target kind
// (primary, backup), calls targetStep once a tick with the session the target
// has now, and sends what it returns. A session is named by its sessionId:
// a new one comes with every session open, a rekey keeps it. 0: no ACTIVE
// session. `settled`: the session has been ACTIVE for 2 s (the answering side
// is ACTIVE first; a frame sent at once could be dropped there).
// ---------------------------------------------------------------------------
enum class TState : uint8_t { Idle, Announcing, Accepted, Refused };

inline const char* targetStateKey(TState s) {
  switch (s) {
    case TState::Idle:       return "idle";
    case TState::Announcing: return "announcing";
    case TState::Accepted:   return "accepted";
    case TState::Refused:    return "refused";
  }
  return "?";
}

constexpr uint32_t kAnnounceBackoffMs[] = { 2000, 4000, 8000, 16000, 32000, 60000 };
constexpr uint8_t  kAnnounceBackoffSteps = sizeof(kAnnounceBackoffMs) / sizeof(kAnnounceBackoffMs[0]);
constexpr uint32_t kRefusedRetryMs   = 600000;  // 10 minutes
constexpr uint32_t kRefreshMs        = 600000;  // an accepted target is announced again every 10 minutes
constexpr uint8_t  kWithdrawTries    = 3;
constexpr uint32_t kWithdrawGapMs    = 2000;

constexpr bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) { return (int32_t)(nowMs - deadlineMs) >= 0; }

struct TargetState {
  bool     have;
  uint8_t  mac[6];
  TState   state;
  uint16_t sessionId;       // the session the state is about (0: none seen yet)
  uint16_t toldSessionId;   // the session on which the target was told this device's state
  uint8_t  linkGen;
  uint8_t  streams;         // what is announced
  uint16_t sensorMask;
  uint8_t  acceptedStreams; // what the answer echoed, within what was announced
  uint16_t acceptedMask;
  uint8_t  lastResult;      // of the last answer to an announcement (0: none)
  uint8_t  backoff;
  bool     queried;
  uint32_t nextMs;
  uint32_t lastAnnounceMs;
  // A withdrawal still to be answered.
  uint8_t  wTries;
  uint8_t  wGen;
  uint8_t  wPartner[6];
  uint32_t wNextMs;
};

struct TargetWant {
  bool     want;        // the link is to be announced to this target
  uint8_t  linkGen;
  uint8_t  streams;
  uint16_t sensorMask;
};

constexpr uint8_t kActAnnounce    = 0x01;
constexpr uint8_t kActWithdraw    = 0x02;  // the stored withdrawal (wGen, wPartner)
constexpr uint8_t kActWithdrawAll = 0x04;  // "I forward for nobody" (B's last linkGen)

// The link B told this target about ends (or the target is no longer one):
// it is withdrawn, up to kWithdrawTries times until answered. A target that
// was told nothing gets nothing.
inline void targetBeginWithdraw(TargetState& t, uint8_t gen, const uint8_t partner[6], uint32_t nowMs) {
  if (t.state == TState::Idle) return;
  t.state = TState::Idle;
  t.wTries = kWithdrawTries;
  t.wGen = gen;
  memcpy(t.wPartner, partner, 6);
  t.wNextMs = nowMs;
  t.acceptedStreams = 0;
  t.acceptedMask = 0;
}

inline void targetRestartAnnounce(TargetState& t, uint32_t nowMs) {
  t.state = TState::Announcing;
  t.backoff = 0;
  t.nextMs = nowMs;
}

inline uint8_t targetStep(TargetState& t, const TargetWant& w, uint16_t sessionId, bool settled, uint32_t nowMs) {
  if (sessionId == 0) return 0;  // nothing can be sent; the state stays for the next session
  uint8_t act = 0;
  if (sessionId != t.sessionId) {
    // A new session (the target may have restarted and forgotten the link): it
    // is told this device's state again. A rekey keeps the sessionId.
    t.sessionId = sessionId;
    t.toldSessionId = 0;
    if (t.state != TState::Idle) targetRestartAnnounce(t, nowMs);
  }
  if (!settled) return 0;
  if (t.wTries && deadlineReached(nowMs, t.wNextMs)) {
    act |= kActWithdraw;
    t.wTries--;
    t.wNextMs = nowMs + kWithdrawGapMs;
  }
  if (!w.want) {
    t.state = TState::Idle;
    t.queried = false;
    if (t.toldSessionId != sessionId) {
      act |= kActWithdrawAll;
      t.toldSessionId = sessionId;
    }
    return act;
  }
  if (t.state == TState::Idle || t.linkGen != w.linkGen) {
    t.linkGen = w.linkGen;
    targetRestartAnnounce(t, nowMs);
  }
  if (t.streams != w.streams || t.sensorMask != w.sensorMask) {
    // Announced again at once, also while an announcement is still out: an
    // answer to the older one no longer counts (targetAnswer).
    t.streams = w.streams;
    t.sensorMask = w.sensorMask;
    targetRestartAnnounce(t, nowMs);
  }
  if (t.queried) {
    t.queried = false;
    if (t.state != TState::Announcing) targetRestartAnnounce(t, nowMs);
    t.nextMs = nowMs;
  }
  switch (t.state) {
    case TState::Announcing:
      if (deadlineReached(nowMs, t.nextMs)) {
        act |= kActAnnounce;
        t.nextMs = nowMs + kAnnounceBackoffMs[t.backoff];
        if (t.backoff + 1 < kAnnounceBackoffSteps) t.backoff++;
        t.lastAnnounceMs = nowMs;
        t.toldSessionId = sessionId;
      }
      break;
    case TState::Accepted:
      if ((uint32_t)(nowMs - t.lastAnnounceMs) >= kRefreshMs) {
        act |= kActAnnounce;
        t.lastAnnounceMs = nowMs;
        t.toldSessionId = sessionId;
      }
      break;
    case TState::Refused:
      if (deadlineReached(nowMs, t.nextMs)) {
        targetRestartAnnounce(t, nowMs);
        act |= kActAnnounce;
        t.nextMs = nowMs + kAnnounceBackoffMs[0];
        t.backoff = 1;
        t.lastAnnounceMs = nowMs;
        t.toldSessionId = sessionId;
      }
      break;
    case TState::Idle:
      break;
  }
  return act;
}

// The sensor mask an announcement to this target carries: the sensors only
// when it carries readings.
constexpr uint16_t announcedMask(const TargetState& t) {
  return (t.streams & kStreamReadings) ? t.sensorMask : (uint16_t)0;
}

enum class AnswerEffect : uint8_t { Ignored, Accepted, Refused, Stale, WithdrawDone, Noted };

// An answer from this target. Used only when it answers what is outstanding:
// the same op, link number and boot tag; an acceptance also echoes the
// streams and sensors announced last (one that echoes an older set answers an
// announcement since replaced, and the newer one is still out). `Stale`: the
// owner steps its link number and announces again.
inline AnswerEffect targetAnswer(TargetState& t, const LinkFrame& a, uint16_t myBootTag, uint32_t nowMs) {
  if (a.op != kOpAnswer) return AnswerEffect::Ignored;
  if (a.answers == kOpAnnounce) {
    if (t.state != TState::Announcing && t.state != TState::Accepted) return AnswerEffect::Ignored;
    if (a.linkGen != t.linkGen || a.viaBootTag != myBootTag) return AnswerEffect::Ignored;
    if (a.result == kResultAccepted && (a.streams != t.streams || a.sensorMask != announcedMask(t))) {
      return AnswerEffect::Ignored;
    }
    t.lastResult = a.result;
    if (a.result == kResultAccepted) {
      t.state = TState::Accepted;
      t.acceptedStreams = (uint8_t)(a.streams & t.streams);
      t.acceptedMask = (uint16_t)(a.sensorMask & t.sensorMask);
      return AnswerEffect::Accepted;
    }
    t.acceptedStreams = 0;
    t.acceptedMask = 0;
    if (a.result == kResultStale) {
      targetRestartAnnounce(t, nowMs);
      return AnswerEffect::Stale;
    }
    t.state = TState::Refused;
    t.nextMs = nowMs + kRefusedRetryMs;
    return AnswerEffect::Refused;
  }
  if (a.answers == kOpWithdraw) {
    if (macIsZero(a.partnerMac)) return AnswerEffect::Noted;  // the answer to a withdrawal of everything
    if (t.wTries && a.linkGen == t.wGen && macSame(a.partnerMac, t.wPartner) && a.viaBootTag == myBootTag) {
      t.wTries = 0;
      return AnswerEffect::WithdrawDone;
    }
  }
  return AnswerEffect::Ignored;
}

// The target asked (it holds no link of this device, or another number).
inline void targetQuery(TargetState& t) {
  t.queried = true;
  t.toldSessionId = 0;
}

// Whether a frame of `streamBit` (and, for a reading, of `sensor`) goes to
// this target now: accepted on the session that is ACTIVE now.
inline bool targetCarries(const TargetState& t, uint16_t sessionIdNow, uint8_t streamBit, int sensor) {
  if (!t.have || t.state != TState::Accepted || sessionIdNow == 0 || sessionIdNow != t.sessionId) return false;
  if (!(t.acceptedStreams & streamBit)) return false;
  if (sensor >= 0 && !(t.acceptedMask & espnowsensor::sensorBit((uint8_t)sensor))) return false;
  return true;
}

// Why a frame of `streamBit` did not go to this target (targetCarries false).
inline WhyNot targetWhyNot(const TargetState& t, uint16_t sessionIdNow, uint8_t streamBit, int sensor) {
  if (sessionIdNow == 0) return WhyNot::NoSession;
  if (t.state == TState::Refused) return WhyNot::Refused;
  if (t.state != TState::Accepted || sessionIdNow != t.sessionId) return WhyNot::NotAccepted;
  (void)streamBit;
  (void)sensor;
  return WhyNot::SensorOff;  // accepted, but this stream or sensor is not carried to it
}

// ---------------------------------------------------------------------------
// M's link table: one entry per device that announced (the via). A bond is
// 1:1, so a via holds one link. The entry keeps the last (boot tag, link
// number) seen from the via, also after its link ended, so an announcement a
// relay held back cannot bring back a link that was ended or replaced.
// ---------------------------------------------------------------------------
enum class LState : uint8_t { None, Queried, Active, Ended };

inline const char* linkStateKey(LState s) {
  switch (s) {
    case LState::None:    return "none";
    case LState::Queried: return "queried";
    case LState::Active:  return "active";
    case LState::Ended:   return "ended";
  }
  return "?";
}

constexpr uint32_t kQueryGapMs = 5000;
constexpr uint32_t kConfirmedForMs = 1800000;  // a link not confirmed for 30 minutes is shown "not confirmed"
constexpr uint8_t  kLinkSlots = 16;
constexpr uint8_t  kAllowedMax = 4;

struct ForwardLink {
  bool     inUse;
  uint8_t  via[6];
  uint8_t  partner[6];
  uint8_t  partnerKey[kPartnerKeyLen];
  char     name[kPartnerNameLen + 1];  // shown name, cleaned and terminated
  bool     namePaired;                 // the name is this device's own for a paired partner
  uint16_t viaBootTag;
  uint8_t  linkGen;
  uint8_t  streams;
  uint16_t sensorMask;
  LState   state;
  bool     haveLast;
  uint16_t lastBootTag;
  uint8_t  lastGen;
  bool     queriedOnce;
  uint16_t resumeMask;
  uint32_t acceptedMs;
  uint32_t confirmedMs;
  uint32_t lastDataMs;
  uint32_t lastQueryMs;
  uint32_t endedMs;
  uint32_t frames;               // forwarded frames accepted
  uint32_t refusedUnannounced;
  uint32_t refusedStaleLink;
  uint32_t refusedStream;
};

// The resume bits of the sensors in `mask`.
constexpr uint16_t resumeBitsOfSensors(uint16_t mask) { return (uint16_t)(mask & espnowsensor::kSensorMaskAll); }

inline void linkNoteLast(ForwardLink& e, uint16_t bootTag, uint8_t gen) {
  if (!e.haveLast || bootTag != e.lastBootTag) {
    e.haveLast = true;
    e.lastBootTag = bootTag;
    e.lastGen = gen;
    return;
  }
  if (serialNewer(gen, e.lastGen)) e.lastGen = gen;
}

// The checks of an announcement that do not need the entry, first match
// (0: none refuses). `self`: this device. `partnerPaired`: the partner is
// paired with this device; `partnerKeyMatches`: under the announced key.
// `nameTaken`: the partner is not paired here and the announced name is
// another paired device's or this device's own.
inline uint8_t announceCheck(const LinkFrame& f, const uint8_t via[6], const uint8_t self[6], bool receives,
                             bool allowed, bool partnerPaired, bool partnerKeyMatches, bool nameTaken) {
  if (!receives) return kResultNotReceiver;
  if (!allowed) return kResultNotAllowed;
  bool keyZero = true;
  for (size_t i = 0; i < kPartnerKeyLen; ++i) {
    if (f.partnerKey[i]) { keyZero = false; break; }
  }
  if (macIsZero(f.partnerMac) || (f.partnerMac[0] & 0x01) || macSame(f.partnerMac, via) ||
      (self && macSame(f.partnerMac, self)) || keyZero) {
    return kResultBadPartner;
  }
  if (partnerPaired && !partnerKeyMatches) return kResultKeyMismatch;
  if (!partnerPaired && nameTaken) return kResultNameTaken;
  return 0;
}

// The generation rule: an announcement is applied when it repeats the active
// link, comes from another boot of the via, or carries a newer link number.
inline bool announceFresh(const ForwardLink& e, const LinkFrame& f) {
  if (!e.inUse || !e.haveLast) return true;
  if (e.state == LState::Active && macSame(f.partnerMac, e.partner) && f.viaBootTag == e.viaBootTag &&
      f.linkGen == e.linkGen) {
    return true;
  }
  if (f.viaBootTag != e.lastBootTag) return true;
  return serialNewer(f.linkGen, e.lastGen);
}

enum class AnnounceEffect : uint8_t { New, Repeat, NewStream, Replaced };

// Applies an announcement that passed every check. The entry belongs to the
// via already (inUse, via set by the owner).
inline AnnounceEffect linkApplyAnnounce(ForwardLink& e, const LinkFrame& f, const char* shownName, bool namePaired,
                                        uint32_t nowMs) {
  linkNoteLast(e, f.viaBootTag, f.linkGen);
  const bool wasActive = e.state == LState::Active;
  const bool samePartner = wasActive && macSame(f.partnerMac, e.partner);
  const bool repeat = samePartner && f.viaBootTag == e.viaBootTag && f.linkGen == e.linkGen;
  auto copyName = [&]() {
    memset(e.name, 0, sizeof(e.name));
    if (shownName) {
      size_t n = strlen(shownName);
      if (n > kPartnerNameLen) n = kPartnerNameLen;
      memcpy(e.name, shownName, n);
    }
    e.namePaired = namePaired;
  };
  if (repeat) {
    copyName();
    e.resumeMask = (uint16_t)(e.resumeMask | resumeBitsOfSensors((uint16_t)(f.sensorMask & ~e.sensorMask)));
    e.streams = f.streams;
    e.sensorMask = f.sensorMask;
    e.confirmedMs = nowMs;
    return AnnounceEffect::Repeat;
  }
  memcpy(e.partner, f.partnerMac, 6);
  memcpy(e.partnerKey, f.partnerKey, kPartnerKeyLen);
  copyName();
  e.viaBootTag = f.viaBootTag;
  e.linkGen = f.linkGen;
  e.streams = f.streams;
  e.sensorMask = f.sensorMask;
  e.state = LState::Active;
  e.resumeMask = kResumeAll;
  e.acceptedMs = nowMs;
  e.confirmedMs = nowMs;
  if (!wasActive) return AnnounceEffect::New;
  return samePartner ? AnnounceEffect::NewStream : AnnounceEffect::Replaced;
}

// A withdrawal of one link (partner and number match), or of everything
// (partner all zero): every link of another boot of the via, or not newer
// than the frame's number. True when the active link ended.
inline bool linkApplyWithdraw(ForwardLink& e, const LinkFrame& f, uint32_t nowMs) {
  bool ended = false;
  if (e.state == LState::Active) {
    if (macIsZero(f.partnerMac)) {
      ended = f.viaBootTag != e.viaBootTag || !serialNewer(e.linkGen, f.linkGen);
    } else {
      ended = macSame(f.partnerMac, e.partner) && f.viaBootTag == e.viaBootTag && f.linkGen == e.linkGen;
    }
  }
  if (ended) {
    e.state = LState::Ended;
    e.endedMs = nowMs;
    e.resumeMask = 0;
  }
  linkNoteLast(e, f.viaBootTag, f.linkGen);
  return ended;
}

inline bool linkConfirmed(const ForwardLink& e, uint32_t nowMs) {
  return e.state == LState::Active && (uint32_t)(nowMs - e.confirmedMs) < kConfirmedForMs;
}

// ---------------------------------------------------------------------------
// A frame on M: whose is it? First match. `bondLink`: it came as 179/180/182.
// `hasBit`: the frame type has a forwarded bit (readings, events; the battery
// frame has only the byte). `streamId`: 0..10 a sensor, 11 battery, 12 events.
// ---------------------------------------------------------------------------
enum class Verdict : uint8_t { Own, Forwarded, Malformed, Unannounced, StaleLink, StreamNotAnnounced };

inline const char* verdictKey(Verdict v) {
  switch (v) {
    case Verdict::Own:                return "own";
    case Verdict::Forwarded:          return "forwarded";
    case Verdict::Malformed:          return "malformed";
    case Verdict::Unannounced:        return "unannounced";
    case Verdict::StaleLink:          return "stale_link";
    case Verdict::StreamNotAnnounced: return "stream_not_announced";
  }
  return "?";
}

struct Attribution {
  Verdict verdict;
  bool    resumePending;  // the first frame of this stream since the link began: judged as a resume
};

inline Attribution attribute(const ForwardLink* e, bool bondLink, bool hasBit, bool bitSet, uint8_t fwdLink,
                             uint8_t streamId) {
  Attribution a = { Verdict::Own, false };
  if (bondLink && (bitSet || fwdLink != 0)) { a.verdict = Verdict::Malformed; return a; }
  if (hasBit && bitSet != (fwdLink != 0)) { a.verdict = Verdict::Malformed; return a; }
  if (fwdLink == 0) return a;
  if (!e || !e->inUse || e->state != LState::Active) { a.verdict = Verdict::Unannounced; return a; }
  if (fwdLink != e->linkGen) { a.verdict = Verdict::StaleLink; return a; }
  const uint8_t bit = streamBitOf(streamId);
  if (!bit || !(e->streams & bit) ||
      (streamId < REMOTE_SENSOR_MAX && !(e->sensorMask & espnowsensor::sensorBit(streamId)))) {
    a.verdict = Verdict::StreamNotAnnounced;
    return a;
  }
  a.verdict = Verdict::Forwarded;
  a.resumePending = streamId < kStreamIdCount && (e->resumeMask & (1u << streamId)) != 0;
  return a;
}

// One query per via every 5 s at most.
inline bool queryDue(const ForwardLink& e, uint32_t nowMs) {
  return !e.queriedOnce || (uint32_t)(nowMs - e.lastQueryMs) >= kQueryGapMs;
}

// A kept record may be given to another sender: one of the origin's own
// (mesh, bond link) when the origin is no longer paired; a forwarded one when
// its via is no longer paired or the via's link no longer names this origin.
constexpr bool recordReusable(uint8_t link, bool originPaired, bool viaPaired, bool linkActiveForOrigin) {
  return link == kLinkForwarded ? (!viaPaired || !linkActiveForOrigin) : !originPaired;
}

// ---------------------------------------------------------------------------
// The devices that may forward to this device (espnowforwardfrom): up to four
// entries, comma separated, each a paired device's address and the first 8
// bytes of the key it was paired under when it was added:
// "AA:BB:CC:DD:EE:FF/0123456789ABCDEF" (hex in either case). A device is
// allowed only while it is paired under that key: another identity paired
// later under the same address (an address can be set on an ESP32) is not.
// A text that is not such an entry is skipped.
// ---------------------------------------------------------------------------
constexpr size_t kAllowKeyLen = 8;
constexpr size_t kAllowEntryTextLen = 17 + 1 + 2 * kAllowKeyLen;               // 34
constexpr size_t kAllowListTextCap = kAllowedMax * (kAllowEntryTextLen + 1);  // 140: the longest list, its commas and the NUL

struct AllowEntry {
  uint8_t mac[6];
  uint8_t key[kAllowKeyLen];  // the first bytes of the device's Ed25519 key
};

inline int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// `text` of `len` bytes is exactly one address.
inline bool macFromText(const char* text, size_t len, uint8_t out[6]) {
  if (!text || len != 17) return false;
  for (int i = 0; i < 6; ++i) {
    const int hi = hexNibble(text[i * 3]);
    const int lo = hexNibble(text[i * 3 + 1]);
    if (hi < 0 || lo < 0) return false;
    if (i < 5 && text[i * 3 + 2] != ':') return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

// `text` of `len` bytes is exactly one entry "AA:BB:CC:DD:EE:FF/<16 hex>".
inline bool allowEntryFromText(const char* text, size_t len, AllowEntry& out) {
  if (!text || len != kAllowEntryTextLen || text[17] != '/') return false;
  if (!macFromText(text, 17, out.mac)) return false;
  for (size_t i = 0; i < kAllowKeyLen; ++i) {
    const int hi = hexNibble(text[18 + 2 * i]);
    const int lo = hexNibble(text[18 + 2 * i + 1]);
    if (hi < 0 || lo < 0) return false;
    out.key[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

inline size_t allowListParse(const char* text, AllowEntry out[kAllowedMax]) {
  size_t n = 0;
  if (!text) return 0;
  const char* p = text;
  while (*p && n < kAllowedMax) {
    while (*p == ' ' || *p == ',') ++p;
    const char* start = p;
    while (*p && *p != ',') ++p;
    size_t len = (size_t)(p - start);
    while (len && start[len - 1] == ' ') --len;
    AllowEntry e;
    if (allowEntryFromText(start, len, e)) {
      bool dup = false;
      for (size_t k = 0; k < n; ++k) dup = dup || macSame(out[k].mac, e.mac);
      if (!dup) out[n++] = e;
    }
  }
  return n;
}

// The entry allows `mac` paired under `key` (its 32-byte key; nullptr: not
// paired, never allowed).
inline bool allowEntryMatches(const AllowEntry& e, const uint8_t mac[6], const uint8_t* key) {
  return key && macSame(e.mac, mac) && memcmp(e.key, key, kAllowKeyLen) == 0;
}

inline bool allowListHas(const char* text, const uint8_t mac[6], const uint8_t* key) {
  AllowEntry list[kAllowedMax];
  const size_t n = allowListParse(text, list);
  for (size_t i = 0; i < n; ++i) {
    if (allowEntryMatches(list[i], mac, key)) return true;
  }
  return false;
}

// Writes the list as the setting holds it (upper case, comma separated).
inline size_t allowListFormat(const AllowEntry* list, size_t n, char* out, size_t cap) {
  static const char kHex[] = "0123456789ABCDEF";
  if (!out || cap == 0) return 0;
  size_t pos = 0;
  out[0] = '\0';
  for (size_t i = 0; i < n; ++i) {
    if (pos + (i ? 1 : 0) + kAllowEntryTextLen + 1 > cap) break;
    if (i) out[pos++] = ',';
    for (int b = 0; b < 6; ++b) {
      out[pos++] = kHex[list[i].mac[b] >> 4];
      out[pos++] = kHex[list[i].mac[b] & 0x0F];
      if (b < 5) out[pos++] = ':';
    }
    out[pos++] = '/';
    for (size_t b = 0; b < kAllowKeyLen; ++b) {
      out[pos++] = kHex[list[i].key[b] >> 4];
      out[pos++] = kHex[list[i].key[b] & 0x0F];
    }
    out[pos] = '\0';
  }
  return pos;
}

// KeyUpdated: the device was listed under another key (it was paired again)
// and is now listed under the one it has.
enum class AllowEdit : uint8_t { Added, AlreadyThere, KeyUpdated, Full, Removed, NotThere };

// Adds `mac` with its 32-byte `key` (add) or removes `mac` (whatever key it
// was listed under) in `text`, and writes the new list to `out`
// (`cap` >= kAllowListTextCap).
inline AllowEdit allowListEdit(const char* text, const uint8_t mac[6], const uint8_t* key, bool add, char* out,
                               size_t cap) {
  AllowEntry list[kAllowedMax + 1];
  size_t n = allowListParse(text, list);
  size_t at = n;
  for (size_t i = 0; i < n; ++i) {
    if (macSame(list[i].mac, mac)) at = i;
  }
  AllowEdit result;
  if (add) {
    if (!key) {
      result = AllowEdit::NotThere;  // only a paired device is added (the caller checks)
    } else if (at < n) {
      if (memcmp(list[at].key, key, kAllowKeyLen) == 0) {
        result = AllowEdit::AlreadyThere;
      } else {
        memcpy(list[at].key, key, kAllowKeyLen);
        result = AllowEdit::KeyUpdated;
      }
    } else if (n >= kAllowedMax) {
      result = AllowEdit::Full;
    } else {
      memcpy(list[n].mac, mac, 6);
      memcpy(list[n].key, key, kAllowKeyLen);
      n++;
      result = AllowEdit::Added;
    }
  } else if (at < n) {
    for (size_t i = at; i + 1 < n; ++i) list[i] = list[i + 1];
    n--;
    result = AllowEdit::Removed;
  } else {
    result = AllowEdit::NotThere;
  }
  allowListFormat(list, n, out, cap);
  return result;
}

// Two names compare equal ignoring ASCII case.
inline bool nameSameIgnoringCase(const char* a, const char* b) {
  if (!a || !b) return false;
  for (;; ++a, ++b) {
    char x = *a, y = *b;
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
    if (!x) return true;
  }
}

}  // namespace espnowfwd

// Who a received frame is about (System_ESPNow_ForwardRx.cpp answers it for
// the three ingests).
struct ForwardOrigin {
  uint8_t mac[6];        // the origin: the sender, or the partner it forwards for
  uint8_t via[6];        // all zero, or the device that forwarded it
  uint8_t link;          // espnowfwd::kLink*
  bool    resumePending; // judge it as a stream start (the first frame since the link began)
  char    name[33];      // the origin's name to keep with the record
};

enum class ForwardResolve : uint8_t {
  Own,        // the sender's own frame (or its bond partner's, over the bond link)
  Forwarded,  // the sender forwards it for its bond partner: `via` is set
  Refused,    // forwarded, but no link of the sender names it (counted; a query may go out)
  Malformed,  // the forwarded mark makes no sense for this frame
};
