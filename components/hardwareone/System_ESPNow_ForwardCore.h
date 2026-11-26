// System_ESPNow_ForwardCore.h - rules of forwarding a bond partner's data to the mesh master
//
// A bond worker (X) sends its readings, battery state, events and sensor
// status to its bond master (B) only. B may forward them to the master named
// on B, and only when that master is paired in B's primary mesh (M): a
// setting on B (bondforward, off by default). B announces the link in its own
// frame, FORWARD_LINK (155), and M answers; only then are X's frames sent on,
// as ordinary SENSOR_ENVELOPE / POWER_STATUS / EVENT_PUSH / SENSOR_STATUS
// frames from B whose reserved link byte (and, for readings and events, a
// reserved flag bit) says "this is my bond partner's". On M the partner is its
// own sender, kept under (partner, via B), with its own counters. M may ask
// for X's readings as from any sender (SENSOR_REQ with the link byte, to B);
// B turns the request into its own bond-link control of X's sensors.
//
// This header has the rules only: the 96-byte announcement frame, whether B
// forwards and why not, B's state per receiving master, M's link table entry,
// how announcements, withdrawals and forwarded frames are judged, and how B
// takes a sensor request for X. Pure logic on caller-owned state: no heap, no
// OS, no logging
// (host test test_espnow_forward_core.cpp, archived 2026-11-26). The owners are
// System_ESPNow_BondForward.cpp (B) and System_ESPNow_ForwardRx.cpp (M).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "System_ESPNow_SensorCore.h"  // sensorBit, kSensorMaskAll, roleSends / roleReceives
#include "System_DeviceName.h"         // the device-name rule; names compare ignoring case

namespace espnowfwd {

using espnowsensor::SensorMask;

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
constexpr SensorMask kResumeAll = (SensorMask)((1ull << kStreamIdCount) - 1u);
static_assert(kStreamIdCount <= 32, "the resume mask holds every stream");
// The sensor status frame (which sensors the partner has and streams): part
// of the readings stream, for no sensor in particular, never a resume.
constexpr uint8_t kStreamIdStatus  = kStreamIdCount;          // 13

constexpr uint8_t streamBitOf(uint8_t streamId) {
  return streamId < REMOTE_SENSOR_MAX || streamId == kStreamIdStatus ? kStreamReadings
       : (streamId == kStreamIdBattery ? kStreamBattery : (streamId == kStreamIdEvents ? kStreamEvents : 0));
}

// Which sensors B forwards: every one but the input device by default (10 Hz,
// meant for the bond master's own screen).
constexpr SensorMask kSensorMaskDefault =
    (SensorMask)(espnowsensor::kSensorMaskAll & ~espnowsensor::sensorBit(REMOTE_SENSOR_INPUT));

// The streams B forwards to a receiving master: readings to the primary only
// (B's own rule for readings switched on by hand), battery and events to both.
// Both must be paired in B's primary mesh (inPrimaryMesh): a bond master in
// several meshes forwards to no other mesh.
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
//   5  result      answer only (kResult*): 0 ok (accepted, or the withdrawal
//                  done), else a shared status (5 no_slot, 6 bad_request) or
//                  this frame's own code (16 and up); 1.0 step 9 (rule F3)
//   6  answers     answer only: the op answered (1 or 2)
//   7  announceGen announce: B's announcement number for this master, 1..255,
//                  stepped on every announcement B sends (a re-send and the
//                  10-minute refresh included); an answer echoes it. 0 in a
//                  withdrawal and a query (1.0 rule, back-capture design W4)
//   8  partnerMac  X's address; all zero in a withdrawal of everything and in
//                  a query
//  14  reserved    2 bytes, sent 0, ignored
//  16  sensorMask  32 bits; bit i: sensor i is forwarded (with streams bit 0); echoed
//  20  partnerKey  X's Ed25519 public key as B holds it (announce)
//  52  partnerName the name B shows for X (announce): a device name under
//                  System_DeviceName.h's rule, NUL always present, the rest 0;
//                  empty when B has no valid name for X. The receiver reads it
//                  with devicename::getField and shows X's address otherwise.
//  84  reserved    12 bytes, sent 0, ignored
// ---------------------------------------------------------------------------
constexpr size_t kLinkFrameLen = 96;
constexpr size_t kPartnerKeyLen = 32;
constexpr size_t kPartnerNameLen = devicename::kFieldLen;
constexpr size_t kLinkOffMask = 16;
constexpr size_t kLinkOffKey  = 20;
constexpr size_t kLinkOffName = 52;
static_assert(kLinkOffKey == kLinkOffMask + 4 && kLinkOffName == kLinkOffKey + kPartnerKeyLen &&
              kLinkOffName + kPartnerNameLen <= kLinkFrameLen, "FORWARD_LINK field offsets");

constexpr uint8_t kOpAnnounce = 1;
constexpr uint8_t kOpWithdraw = 2;
constexpr uint8_t kOpQuery    = 3;
constexpr uint8_t kOpAnswer   = 4;

// The answer's result (1.0 step 9, rule F3: 0 = ok, the shared status table
// 1-15 of System_ESPNow_AnswerCore.h, then this frame's own codes from 16).
// Any other value reads as a refusal this firmware does not know.
constexpr uint8_t kResultOk          = 0;   // the announcement accepted, or the withdrawal done
constexpr uint8_t kResultAccepted    = kResultOk;
constexpr uint8_t kResultFull        = 5;   // shared no_slot: it already keeps the links of 16 devices
constexpr uint8_t kResultBadPartner  = 6;   // shared bad_request: the partner's address or key is not valid
constexpr uint8_t kResultNotReceiver = 16;
constexpr uint8_t kResultKeyMismatch = 17;
constexpr uint8_t kResultStale       = 18;
constexpr uint8_t kResultNameTaken   = 19;

struct LinkFrame {
  uint8_t  op;
  uint8_t  linkGen;
  uint16_t viaBootTag;
  uint8_t  streams;
  uint8_t  result;
  uint8_t  answers;
  uint8_t  announceGen;  // byte 7
  uint8_t  partnerMac[6];
  SensorMask sensorMask;
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
  out[7] = f.announceGen;
  memcpy(out + 8, f.partnerMac, 6);
  out[kLinkOffMask + 0] = (uint8_t)(f.sensorMask & 0xFF);
  out[kLinkOffMask + 1] = (uint8_t)((f.sensorMask >> 8) & 0xFF);
  out[kLinkOffMask + 2] = (uint8_t)((f.sensorMask >> 16) & 0xFF);
  out[kLinkOffMask + 3] = (uint8_t)((f.sensorMask >> 24) & 0xFF);
  memcpy(out + kLinkOffKey, f.partnerKey, kPartnerKeyLen);
  memcpy(out + kLinkOffName, f.partnerName, kPartnerNameLen);
}

// False for a payload shorter than the frame, an op this firmware does not
// know, or an announcement of link number 0. A longer payload is read up to
// the frame's length.
inline bool parseLinkFrame(const uint8_t* in, size_t len, LinkFrame& f) {
  if (!in || len < kLinkFrameLen) return false;
  if (in[0] < kOpAnnounce || in[0] > kOpAnswer) return false;
  // An announcement names a link: its number is never 0, which marks "not
  // forwarded" in a frame (and in a sensor request: a request for the
  // device's own sensors).
  if (in[0] == kOpAnnounce && in[1] == 0) return false;
  f.op = in[0];
  f.linkGen = in[1];
  f.viaBootTag = (uint16_t)(in[2] | ((uint16_t)in[3] << 8));
  f.streams = (uint8_t)(in[4] & kStreamsAll);
  f.result = in[5];
  f.answers = in[6];
  f.announceGen = in[7];
  memcpy(f.partnerMac, in + 8, 6);
  const uint8_t* m = in + kLinkOffMask;
  f.sensorMask = ((SensorMask)m[0] | ((SensorMask)m[1] << 8) | ((SensorMask)m[2] << 16) | ((SensorMask)m[3] << 24)) &
                 espnowsensor::kSensorMaskAll;
  memcpy(f.partnerKey, in + kLinkOffKey, kPartnerKeyLen);
  memcpy(f.partnerName, in + kLinkOffName, kPartnerNameLen);
  return true;
}

inline const char* resultKey(uint8_t result) {
  switch (result) {
    case kResultAccepted:    return "accepted";
    case kResultNotReceiver: return "not_receiver";
    case kResultBadPartner:  return "bad_partner";
    case kResultKeyMismatch: return "key_mismatch";
    case kResultFull:        return "full";
    case kResultStale:       return "stale";
    case kResultNameTaken:   return "name_taken";
    default:                 return "refused";
  }
}

// After "<master> refused: ".
inline const char* resultText(uint8_t result) {
  switch (result) {
    case kResultAccepted:    return "accepted";
    case kResultNotReceiver: return "it is not a mesh master or backup master";
    case kResultBadPartner:  return "the partner's address or key is not valid";
    case kResultKeyMismatch: return "it has the partner paired under another key";
    case kResultFull:        return "it already keeps the links of 16 devices";
    case kResultStale:       return "the announcement was older than one it already had";
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

// Announcement numbers (byte 7): per target, 1..255 and never 0, stepped on
// every announcement sent. Only the answer to the latest one is taken as an
// acceptance (targetAnswer).
constexpr uint8_t announceGenNext(uint8_t gen) { return linkGenNext(gen); }

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
  NotPrimaryMesh,   // ... none of them is paired in this device's primary mesh
  TargetIsPartner,  // the named master is the bond partner itself (that target is skipped)
  NoSession,        // the master has no session with this device
  NotAccepted,      // announced, no answer yet
  Refused,          // the master refused the link
  QueueFull,        // the send queue took no more
  TooLong,          // a reading over 536 bytes (cannot happen: B keeps at most that)
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
    case WhyNot::NotPrimaryMesh:  return "not_primary_mesh";
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

static_assert(espnowsensor::kReadingMaxBytes == 536, "whyNotText names the 536-byte limit");

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
    case WhyNot::NotPrimaryMesh:  return "the master named here is paired in another mesh than this device's primary mesh";
    case WhyNot::TargetIsPartner: return "the master named here is the bond partner itself";
    case WhyNot::NoSession:       return "the master has no session with this device yet";
    case WhyNot::NotAccepted:     return "the master has not accepted yet";
    case WhyNot::Refused:         return "the master refused";
    case WhyNot::QueueFull:       return "the send queue is full";
    case WhyNot::TooLong:         return "the reading is over the 536-byte limit";
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
  uint8_t targetsInPrimary;// ... and paired in this device's primary mesh
};

inline WhyNot forwardDecide(const DecideInputs& in) {
  if (!in.settingOn) return WhyNot::Off;
  if (!in.bondMaster) return WhyNot::NotBondMaster;
  if (!in.agreementOk) return WhyNot::BondNotAgreed;
  if (!in.meshOn) return WhyNot::MeshOff;
  if (!espnowsensor::roleSends(in.meshRole)) return WhyNot::IsMaster;
  if (!in.masterNamed) return WhyNot::NoMaster;
  if (in.targetsPaired == 0) return WhyNot::MasterNotPaired;
  if (in.targetsInPrimary == 0) return WhyNot::NotPrimaryMesh;
  return WhyNot::Forwards;
}

// ---- The primary mesh ----------------------------------------------------------
// A device may be in up to four meshes (gSettings.meshes). Its primary mesh is
// the default one (the enabled slot flagged isDefault, else the first enabled
// slot, else slot 0, which the single-mesh settings fill). Forwarded data goes
// only to a master paired in that mesh: the other meshes never get it.
inline uint8_t primaryMeshSlot(const bool* enabled, const bool* isDefault, uint8_t slots) {
  for (uint8_t i = 0; i < slots; ++i) {
    if (enabled[i] && isDefault[i]) return i;
  }
  for (uint8_t i = 0; i < slots; ++i) {
    if (enabled[i]) return i;
  }
  return 0;
}
// `peerSlot`: the mesh slot the master is paired in (-1: not paired).
constexpr bool inPrimaryMesh(int peerSlot, uint8_t primarySlot) { return peerSlot >= 0 && peerSlot == (int)primarySlot; }

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
constexpr size_t kStatusJsonCap = 3072;
constexpr size_t kStatusLineMax = 255;
constexpr size_t kStatusLinesCap = 1280;  // five lines

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
  SensorMask sensorMask;
  uint8_t  acceptedStreams; // what the answer echoed, within what was announced
  SensorMask acceptedMask;
  uint8_t  announceGen;     // the number of the last announcement sent (0: none yet)
  uint8_t  lastResult;      // of the last answer to an announcement (valid when haveResult)
  bool     haveResult;      // an announcement was answered (kResultOk is 0)
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
  SensorMask sensorMask;
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
        t.announceGen = announceGenNext(t.announceGen);
        t.nextMs = nowMs + kAnnounceBackoffMs[t.backoff];
        if (t.backoff + 1 < kAnnounceBackoffSteps) t.backoff++;
        t.lastAnnounceMs = nowMs;
        t.toldSessionId = sessionId;
      }
      break;
    case TState::Accepted:
      if ((uint32_t)(nowMs - t.lastAnnounceMs) >= kRefreshMs) {
        act |= kActAnnounce;
        t.announceGen = announceGenNext(t.announceGen);
        t.lastAnnounceMs = nowMs;
        t.toldSessionId = sessionId;
      }
      break;
    case TState::Refused:
      if (deadlineReached(nowMs, t.nextMs)) {
        targetRestartAnnounce(t, nowMs);
        act |= kActAnnounce;
        t.announceGen = announceGenNext(t.announceGen);
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
constexpr SensorMask announcedMask(const TargetState& t) {
  return (t.streams & kStreamReadings) ? t.sensorMask : (SensorMask)0;
}

enum class AnswerEffect : uint8_t { Ignored, Accepted, Refused, Stale, WithdrawDone, Noted };

// An answer from this target. Used only when it answers what is outstanding:
// the same op, link number and boot tag, and (1.0 rule, back-capture design
// W4) the number of the latest announcement (announceGen). An answer with an
// older number, an acceptance or a refusal alike, answers an announcement
// since replaced: the newer one is still out and gets its own answer, so the
// older verdict never overrides it. An acceptance's streams and sensors must
// also be a subset of what that announcement carried: a master on older
// firmware drops the streams and sensors it does not know when it reads the
// announcement and echoes the rest, and what it echoes is what it takes.
// `Stale`: the owner steps its link number and announces again.
inline AnswerEffect targetAnswer(TargetState& t, const LinkFrame& a, uint16_t myBootTag, uint32_t nowMs) {
  if (a.op != kOpAnswer) return AnswerEffect::Ignored;
  if (a.answers == kOpAnnounce) {
    if (t.state != TState::Announcing && t.state != TState::Accepted) return AnswerEffect::Ignored;
    if (a.linkGen != t.linkGen || a.viaBootTag != myBootTag) return AnswerEffect::Ignored;
    if (t.announceGen == 0 || a.announceGen != t.announceGen) return AnswerEffect::Ignored;
    if (a.result == kResultAccepted &&
        ((a.streams & (uint8_t)~t.streams) != 0 || (a.sensorMask & (SensorMask)~announcedMask(t)) != 0)) {
      return AnswerEffect::Ignored;
    }
    t.lastResult = a.result;
    t.haveResult = true;
    if (a.result == kResultAccepted) {
      t.state = TState::Accepted;
      t.acceptedStreams = (uint8_t)(a.streams & t.streams);
      t.acceptedMask = (SensorMask)(a.sensorMask & t.sensorMask);
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
// The partner's sensor status, forwarded: SENSOR_STATUS (151), 16 bytes,
// little endian (V4PayloadSensorStatus):
//
//   0  presentMask    bit i: sensor i is connected on the sender (here: X)
//   4  streamingMask  bit i: the sender streams sensor i
//   8  bootTag        the sender's boot tag (here: X's)
//  10  fwdLink        0, or B's link number: X's status, forwarded by B
//  11  reserved       5 bytes, 0
//
// The frame has no flag byte: the link byte alone marks it, as in the battery
// frame. X sends it to B over the bond link as BOND_SENSOR_STATUS (185) with
// the byte 0; B sends it on with the sensors it forwards only.
// ---------------------------------------------------------------------------
constexpr size_t kStatusFrameLen = 16;

struct StatusFrame {
  uint32_t presentMask;
  uint32_t streamingMask;
  uint16_t bootTag;
  uint8_t  fwdLink;
};

inline void packStatusFrame(uint8_t out[kStatusFrameLen], const StatusFrame& f) {
  memset(out, 0, kStatusFrameLen);
  for (int i = 0; i < 4; ++i) {
    out[i] = (uint8_t)(f.presentMask >> (8 * i));
    out[4 + i] = (uint8_t)(f.streamingMask >> (8 * i));
  }
  out[8] = (uint8_t)(f.bootTag & 0xFF);
  out[9] = (uint8_t)(f.bootTag >> 8);
  out[10] = f.fwdLink;
}

inline bool parseStatusFrame(const uint8_t* in, size_t len, StatusFrame& f) {
  if (!in || len < kStatusFrameLen) return false;
  f.presentMask = 0;
  f.streamingMask = 0;
  for (int i = 0; i < 4; ++i) {
    f.presentMask |= (uint32_t)in[i] << (8 * i);
    f.streamingMask |= (uint32_t)in[4 + i] << (8 * i);
  }
  f.bootTag = (uint16_t)(in[8] | ((uint16_t)in[9] << 8));
  f.fwdLink = in[10];
  return true;
}

// ---------------------------------------------------------------------------
// A sensor request for the partner (SENSOR_REQ, 153, with its link byte set).
// The mesh master asks for X's readings as it asks any sender: subscribe with
// a lease, a one-shot, unsubscribe. It addresses B (the via) with B's link
// number in byte 5 of the request (V4PayloadSensorReq.fwdLink). X never
// takes a request from a mesh master (meshDataAllowed): B takes it, when
// forwarding is on and the link stands, and turns it into its own bond-link
// control of X's sensors (BOND_STREAM_CTRL on / off); the readings come back
// through the forwarding. Only the master B forwards readings to may ask, and
// only for sensors B forwards.
// ---------------------------------------------------------------------------
constexpr uint8_t kReqSubscribe   = 0;
constexpr uint8_t kReqUnsubscribe = 1;
constexpr uint8_t kReqOneshot     = 2;
// A one-shot holds X's sensor on this long at least: two hops, and X's first
// reading after the switch.
constexpr uint32_t kReqOneshotHoldMs = 3000;

enum class ReqVerdict : uint8_t {
  Apply = 0,
  Off,               // bondforward is off here
  NotBondMaster,     // bond mode off, no paired partner, or this device is the worker
  NoLink,            // no standing link (or a partner change runs)
  OldLink,           // the request names another link number
  NotPrimaryMaster,  // the sender is not the master this device forwards readings to (accepted)
  BadMode,           // a mode this firmware does not know
  NotForwarded,      // none of the sensors asked for is forwarded (bondforward sensors)
  PartnerOffline,    // the partner is not connected and synced: nothing can be switched
  PartnerStatusUnknown,  // the partner has not sent its sensor list (BOND_SENSOR_STATUS) yet
  PartnerLacksSensor,    // the partner lists none of the sensors asked for that are forwarded
  Count
};
constexpr size_t kReqVerdictCount = (size_t)ReqVerdict::Count;

inline const char* reqVerdictKey(ReqVerdict v) {
  switch (v) {
    case ReqVerdict::Apply:            return "applied";
    case ReqVerdict::Off:              return "off";
    case ReqVerdict::NotBondMaster:    return "not_bond_master";
    case ReqVerdict::NoLink:           return "no_link";
    case ReqVerdict::OldLink:          return "old_link";
    case ReqVerdict::NotPrimaryMaster: return "not_primary_master";
    case ReqVerdict::BadMode:          return "bad_mode";
    case ReqVerdict::NotForwarded:     return "not_forwarded";
    case ReqVerdict::PartnerOffline:   return "partner_offline";
    case ReqVerdict::PartnerStatusUnknown: return "partner_status_unknown";
    case ReqVerdict::PartnerLacksSensor:   return "partner_lacks_sensor";
    case ReqVerdict::Count:            break;
  }
  return "?";
}

inline const char* reqVerdictText(ReqVerdict v) {
  switch (v) {
    case ReqVerdict::Apply:            return "applied";
    case ReqVerdict::Off:              return "forwarding is off (bondforward on)";
    case ReqVerdict::NotBondMaster:    return "only a bond master with a paired partner takes requests for it";
    case ReqVerdict::NoLink:           return "this device forwards nothing for its partner now";
    case ReqVerdict::OldLink:          return "the request names an old link";
    case ReqVerdict::NotPrimaryMaster: return "only the master this device forwards readings to may ask";
    case ReqVerdict::BadMode:          return "the request's mode is not known";
    case ReqVerdict::NotForwarded:     return "none of those sensors is forwarded (bondforward sensors)";
    case ReqVerdict::PartnerOffline:   return "the partner is not connected";
    case ReqVerdict::PartnerStatusUnknown: return "the partner has not said yet which sensors it has";
    case ReqVerdict::PartnerLacksSensor:   return "the partner does not have that sensor running";
    case ReqVerdict::Count:            break;
  }
  return "?";
}

struct ReqInputs {
  bool     settingOn;
  bool     bondMaster;          // bond mode on, a paired partner, role master
  bool     linkActive;          // a standing link, no partner change since
  uint8_t  linkGen;
  uint8_t  fwdLink;             // the request's link byte (not 0)
  bool     fromAcceptedPrimary; // the sender is the primary target, accepted on its ACTIVE session, with readings
  uint8_t  mode;
  SensorMask mask;                // the sensors asked for
  SensorMask forwardedMask;       // the sensors the target accepted
  bool     partnerOnline;
  bool     partnerStatusKnown;  // the partner's sensor list (BOND_SENSOR_STATUS) came on this link's partner
  SensorMask partnerPresent;      // the sensors that list names (present and running at the partner)
};

// First match. An unsubscribe needs neither the sensors nor the partner: it
// only ends leases (what is still on at X is switched off when it can be).
// A subscribe or one-shot is judged against the partner's own sensor list: a
// sensor X does not list is never leased (X would stream nothing, and the
// lease would say it does). Before X sent its list (it does once per bond
// session, within seconds of the link) nothing is known, so the request is
// refused rather than guessed; the master asks again. Sensors X lacks among
// several asked for are left out (reqLeftOut) and the rest applied.
inline ReqVerdict sensorReqDecide(const ReqInputs& in) {
  if (!in.settingOn) return ReqVerdict::Off;
  if (!in.bondMaster) return ReqVerdict::NotBondMaster;
  if (!in.linkActive) return ReqVerdict::NoLink;
  if (in.fwdLink != in.linkGen) return ReqVerdict::OldLink;
  if (!in.fromAcceptedPrimary) return ReqVerdict::NotPrimaryMaster;
  if (in.mode > kReqOneshot) return ReqVerdict::BadMode;
  if (in.mode == kReqUnsubscribe) return ReqVerdict::Apply;
  if (!(in.mask & in.forwardedMask)) return ReqVerdict::NotForwarded;
  if (!in.partnerOnline) return ReqVerdict::PartnerOffline;
  if (!in.partnerStatusKnown) return ReqVerdict::PartnerStatusUnknown;
  if (!(in.mask & in.forwardedMask & in.partnerPresent)) return ReqVerdict::PartnerLacksSensor;
  return ReqVerdict::Apply;
}

// The sensors an applied request leases or ends: an unsubscribe ends any it
// names; a subscribe or one-shot only those forwarded that X lists.
inline SensorMask reqApplyMask(const ReqInputs& in) {
  if (in.mode == kReqUnsubscribe) return in.mask;
  return (SensorMask)(in.mask & in.forwardedMask & in.partnerPresent);
}

// The forwarded sensors a subscribe or one-shot asked for that X does not
// list: left out of the lease (counted as partner_lacks_sensor).
inline SensorMask reqLeftOut(const ReqInputs& in) {
  if (in.mode == kReqUnsubscribe || !in.partnerStatusKnown) return 0;
  return (SensorMask)(in.mask & in.forwardedMask & (SensorMask)~in.partnerPresent);
}

// B's leases on X's sensors, one per sensor. `started`: B switched the sensor
// on at X for the lease, so the lease's end switches it off. A sensor X
// streamed already (its user's choice, or B's own bondstream) is not started
// by a lease and goes on after it. `onSession`: the bond session the "on" was
// sent on; on a new session (X restarted, its streams are off) it is sent
// again. `byUser`: the sensors B's user switched on at X by hand (bondstream,
// the bond page) and not off since: a lease never starts or stops them.
// `offOwed`: the sensors a lease started and whose end switches them off,
// until the "off" went out (leaseOffSent): a failed "off" is sent again, as a
// failed "on" is.
struct ReqLeases {
  SensorMask leased;
  SensorMask started;
  SensorMask byUser;
  SensorMask offOwed;
  uint32_t untilMs[REMOTE_SENSOR_MAX];
  uint16_t onSession[REMOTE_SENSOR_MAX];
};

struct LeaseActs {
  SensorMask on;   // send "on" for these sensors
  SensorMask off;  // send "off" for these (they are owed until sent)
};

// A lease ends: when it started the sensor, the "off" is owed.
inline SensorMask leaseEndOne(ReqLeases& l, SensorMask bit) {
  const bool started = (l.started & bit) != 0;
  l.leased = (SensorMask)(l.leased & ~bit);
  l.started = (SensorMask)(l.started & ~bit);
  if (!started) return 0;
  l.offOwed = (SensorMask)(l.offOwed | bit);
  return bit;
}

// A request that was decided Apply. `mask`: the sensors asked for that are
// forwarded (an unsubscribe: any). `streaming`: the sensors X streams already
// (its status) or B's user chose to stream there (its saved bondstream
// settings); `l.byUser` counts as well. A sensor whose "off" is still owed
// streams because of the lease that ended, not by anyone's choice: a new
// lease takes it over (and sends "on" again, which costs nothing).
inline LeaseActs leaseApply(ReqLeases& l, uint8_t mode, SensorMask mask, SensorMask streaming, uint32_t intervalMs,
                            uint32_t leaseMs, uint32_t nowMs) {
  LeaseActs a = { 0, 0 };
  for (uint8_t i = 0; i < REMOTE_SENSOR_MAX; ++i) {
    const SensorMask bit = espnowsensor::sensorBit(i);
    if (!(mask & bit)) continue;
    if (mode == kReqUnsubscribe) {
      if (!(l.leased & bit)) continue;
      a.off = (SensorMask)(a.off | leaseEndOne(l, bit));
      continue;
    }
    uint32_t forMs = leaseMs;
    if (mode == kReqOneshot) forMs = intervalMs > kReqOneshotHoldMs ? intervalMs : kReqOneshotHoldMs;
    // A renewal sets the end anew (as a sender's own lease does). A one-shot
    // never cuts a running subscription short.
    const uint32_t until = nowMs + forMs;
    if (!(l.leased & bit) || mode != kReqOneshot || (int32_t)(until - l.untilMs[i]) > 0) l.untilMs[i] = until;
    l.leased = (SensorMask)(l.leased | bit);
    const bool owed = (l.offOwed & bit) != 0;
    l.offOwed = (SensorMask)(l.offOwed & ~bit);
    if (!(l.started & bit) && !(l.byUser & bit) && (owed || !(streaming & bit))) {
      l.started = (SensorMask)(l.started | bit);
      a.on = (SensorMask)(a.on | bit);
    }
  }
  return a;
}

// Leases that ran out: their sensors, when a lease started them, are
// switched off.
inline SensorMask leaseExpire(ReqLeases& l, uint32_t nowMs) {
  SensorMask off = 0;
  for (uint8_t i = 0; i < REMOTE_SENSOR_MAX; ++i) {
    const SensorMask bit = espnowsensor::sensorBit(i);
    if (!(l.leased & bit) || (int32_t)(nowMs - l.untilMs[i]) < 0) continue;
    off = (SensorMask)(off | leaseEndOne(l, bit));
  }
  return off;
}

// Leases on sensors outside `keep` end at once: the sensor is no longer
// forwarded (bondforward sensors), or the master that asked no longer gets
// the readings (keep 0: it left the primary mesh, or became the partner).
inline SensorMask leaseKeepOnly(ReqLeases& l, SensorMask keep) {
  SensorMask off = 0;
  for (uint8_t i = 0; i < REMOTE_SENSOR_MAX; ++i) {
    const SensorMask bit = espnowsensor::sensorBit(i);
    if ((l.leased & bit) && !(keep & bit)) off = (SensorMask)(off | leaseEndOne(l, bit));
  }
  return off;
}

// This device's user switched the sensor at X by hand (bondstream): on, it is
// the user's stream now, and the lease's end leaves it on; off, the lease
// ends with it (a stop by hand ends the stream whoever started it, and the
// user's own "off" is the one a lease owed).
inline void leaseByHand(ReqLeases& l, uint8_t sensor, bool on) {
  if (sensor >= REMOTE_SENSOR_MAX) return;
  const SensorMask bit = espnowsensor::sensorBit(sensor);
  l.started = (SensorMask)(l.started & ~bit);
  l.offOwed = (SensorMask)(l.offOwed & ~bit);
  if (on) {
    l.byUser = (SensorMask)(l.byUser | bit);
  } else {
    l.byUser = (SensorMask)(l.byUser & ~bit);
    l.leased = (SensorMask)(l.leased & ~bit);
  }
}

// Every lease ends (the link ended, forwarding was switched off, another
// master is named). Returns the sensors a lease started (their "off" is owed).
inline SensorMask leaseEndAll(ReqLeases& l) {
  const SensorMask off = l.started;
  l.offOwed = (SensorMask)(l.offOwed | l.started);
  l.leased = 0;
  l.started = 0;
  return off;
}

// The "off" for `bit` went out.
inline void leaseOffSent(ReqLeases& l, SensorMask bit) { l.offOwed = (SensorMask)(l.offOwed & ~bit); }

// This device is no longer the bond master of the partner the leases were
// on (another partner, the bond ended, the worker role): nothing is owed to
// it any more, and its user's streams there are not this device's to keep.
inline void leaseForgetPartner(ReqLeases& l) {
  l.leased = 0;
  l.started = 0;
  l.offOwed = 0;
  l.byUser = 0;
}

// The sensors a lease started whose "on" went over another bond session than
// `sessionNow` (0: none now, nothing to send).
inline SensorMask leaseResend(const ReqLeases& l, uint16_t sessionNow) {
  if (sessionNow == 0) return 0;
  SensorMask on = 0;
  for (uint8_t i = 0; i < REMOTE_SENSOR_MAX; ++i) {
    const SensorMask bit = espnowsensor::sensorBit(i);
    if ((l.started & bit) && l.onSession[i] != sessionNow) on = (SensorMask)(on | bit);
  }
  return on;
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
  SensorMask sensorMask;
  LState   state;
  bool     haveLast;
  uint16_t lastBootTag;
  uint8_t  lastGen;
  bool     queriedOnce;
  SensorMask resumeMask;
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
constexpr SensorMask resumeBitsOfSensors(SensorMask mask) { return (SensorMask)(mask & espnowsensor::kSensorMaskAll); }

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
// (0: none refuses). Any paired device may announce (the receive table lets
// only a paired device's session frames through): a bond pair is one device
// shown as two, and its data is accepted by the same rule as the bond
// master's own readings, a role that keeps data. What a bond master sends for
// its partner is always shown "via" it and never merged with a direct record.
// `self`: this device. `partnerPaired`: the partner is paired with this
// device; `partnerKeyMatches`: under the announced key. `nameTaken`: the
// partner is not paired here and the announced name is another paired
// device's or this device's own.
inline uint8_t announceCheck(const LinkFrame& f, const uint8_t via[6], const uint8_t self[6], bool receives,
                             bool partnerPaired, bool partnerKeyMatches, bool nameTaken) {
  if (!receives) return kResultNotReceiver;
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
    e.resumeMask = (SensorMask)(e.resumeMask | resumeBitsOfSensors((SensorMask)(f.sensorMask & ~e.sensorMask)));
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
// Text helpers of the readers.
// ---------------------------------------------------------------------------
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

// Two names compare equal ignoring ASCII case (the device-name rule's
// comparison, System_DeviceName.h). Null equals nothing.
inline bool nameSameIgnoringCase(const char* a, const char* b) {
  return a && b && devicename::sameIgnoringCase(a, b);
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

// Where a sensor request for a forwarded origin goes (espnowForwardRequestTarget).
enum class ForwardTarget : uint8_t {
  Found,
  NoVia,   // the device named as the forwarder is not known
  NoLink,  // no active link names the origin (of that device)
};

enum class ForwardResolve : uint8_t {
  Own,        // the sender's own frame (or its bond partner's, over the bond link)
  Forwarded,  // the sender forwards it for its bond partner: `via` is set
  Refused,    // forwarded, but no link of the sender names it (counted; a query may go out)
  Malformed,  // the forwarded mark makes no sense for this frame
};
