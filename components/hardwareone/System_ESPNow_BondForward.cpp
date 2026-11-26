// System_ESPNow_BondForward.cpp - a bond master forwards its partner's data to the mesh master
//
// See System_ESPNow_BondForward.h for what this is and
// System_ESPNow_ForwardCore.h for the rules. espnow_task runs the tick, the
// answers and the three forward calls; cmd_exec runs 'bondforward' and the
// status readers, which read a snapshot under sBfMux.
#include "System_BuildConfig.h"

#if ENABLE_ESPNOW && ENABLE_BONDED_MODE

#include "System_ESPNow_BondForward.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "System_Debug.h"
#include "System_ESPNow.h"
#include "System_ESPNow_Handlers_Crypto.h"  // espnowHandshakeAutoStart
#include "System_ESPNow_HandshakeBackoff.h" // WarnLimiter
#include "System_ESPNow_Identity.h"
#include "System_ESPNow_Sensors.h"          // sensorReceiverTargets, espnowSensorBootTag
#include "System_ESPNow_Sessions.h"         // sendStatusRegister
#include "System_ESPNow_Tx.h"
#include "System_ESPNow_Wire.h"
#include "System_MacText.h"
#include "System_Settings.h"
#include "System_Utils.h"

// ---- What the rules below ask of the device ---------------------------------
// Everything between BOND_FORWARD_HOST_BEGIN and BOND_FORWARD_HOST_END reaches
// the bond state, the settings, the sessions and the radio only through these,
// so the host suite espnow_bond_forward runs that part as it is.

static portMUX_TYPE sBfMux = portMUX_INITIALIZER_UNLOCKED;
#define BF_LOCK()   portENTER_CRITICAL(&sBfMux)
#define BF_UNLOCK() portEXIT_CRITICAL(&sBfMux)

static uint32_t bfNow() { return (uint32_t)millis(); }

// What this device knows of its bond. Only the single-bond settings are read:
// a bond is 1:1 (the per-mesh bond fields are not used at runtime).
struct BfBond {
  bool     modeOn;
  bool     havePartner;
  uint8_t  partner[6];
  bool     isMaster;
  bool     partnerPaired;
  uint8_t  partnerKey[32];
  uint8_t  agreeState;     // BondAgreementState
  bool     roleConflict;
  uint32_t epoch;          // the bond hold epoch (a partner change steps it)
};
static void bfBond(BfBond* out) {
  memset(out, 0, sizeof(*out));
  out->modeOn = gSettings.bondModeEnabled;
  out->havePartner = gSettings.bondPeerMac.length() > 0 && parseMacAddress(gSettings.bondPeerMac, out->partner);
  out->isMaster = isBondMaster();
  if (out->havePartner) {
    const PeerIdentity* pid = peerIdentityFindByMac(out->partner);
    if (pid) {
      out->partnerPaired = true;
      memcpy(out->partnerKey, pid->longTermPub, 32);
    }
  }
  const BondAgreementView view = bondAgreementView();
  out->agreeState = view.state;
  out->roleConflict = view.roleConflict;
  out->epoch = bondHoldEpochNow();
}
static bool bfDeclined() { return bondAgreementView().state == BOND_AGREEMENT_DECLINED; }
static uint32_t bfEpoch() { return bondHoldEpochNow(); }
// A partner change runs (cmd_exec writes the partner, the role and bond mode
// one by one, and resets the agreement last), or one began since `epoch`.
static bool bfHeld(uint32_t epoch) { return bondHeldSince(epoch); }
static bool bfSettingOn() { return gSettings.bondForwardToMesh; }
static espnowsensor::SensorMask bfSensorMask() { return gSettings.bondForwardSensors & espnowsensor::kSensorMaskAll; }
static void bfSetSettingOn(bool on) { setSetting(gSettings.bondForwardToMesh, on); }
static void bfSetSensorMask(espnowsensor::SensorMask mask) { setSetting(gSettings.bondForwardSensors, mask); }
static bool bfMeshOn() { return meshEnabled(); }
// on/off, true/false, 1/0 (what a settings editor sends): 1, 0, or -1.
static int bfParseBool(const char* word) { return parseBoolArg(String(word)); }
static uint8_t bfRole() { return gSettings.meshRole; }
static void bfTargets(SensorReceiverTargets* out) { sensorReceiverTargets(out); }
// `mac` is paired in this device's primary mesh (the default slot of
// gSettings.meshes): only a master there gets the partner's data.
static bool bfInPrimaryMesh(const uint8_t mac[6]) { return espnowPeerInPrimaryMesh(mac); }
// The sessionId of the ACTIVE session with `mac` (0: none). A rekey keeps it.
static uint16_t bfSessionId(const uint8_t mac[6]) { return espnowSessionIdOf(mac); }
static bool bfSettled(const uint8_t mac[6], uint32_t nowMs) { return espnowSessionSettled(mac, nowMs); }
static bool bfHandshake(const uint8_t mac[6]) { return espnowHandshakeAutoStart(mac); }
static uint16_t bfBootTag() { return espnowSensorBootTag(); }
static uint16_t bfFrameCap(const uint8_t mac[6]) { return v4_single_frame_cap(mac); }
static bool bfSendLink(const uint8_t mac[6], const uint8_t* payload, uint16_t len) {
  return espnowtx::sendAead(mac, ESPNOW_V4_TYPE_FORWARD_LINK, 0, generateMessageId(), payload, len, 1);
}
static bool bfSendPart(const uint8_t mac[6], const uint8_t* part, uint16_t len) {
  return espnowtx::sendAead(mac, ESPNOW_V4_TYPE_SENSOR_ENVELOPE, 0, generateMessageId(), part, len, 1);
}
// As this device's own battery frame: acknowledged, and watched (two missed
// acknowledgements clear a session the master no longer has).
static bool bfSendPower(const uint8_t mac[6], const uint8_t* payload, uint16_t len) {
  const uint32_t msgId = generateMessageId();
  if (!espnowtx::sendAead(mac, ESPNOW_V4_TYPE_POWER_STATUS, ESPNOW_V4_FLAG_ACK_REQ, msgId, payload, len, 1)) return false;
  sendStatusRegister(msgId, mac);
  return true;
}
static bool bfSendEvent(const uint8_t mac[6], const uint8_t* payload, uint16_t len) {
  return espnowtx::sendAead(mac, ESPNOW_V4_TYPE_EVENT_PUSH, 0, generateMessageId(), payload, len, 1);
}
static bool bfSendStatus(const uint8_t mac[6], const uint8_t* payload, uint16_t len) {
  return espnowtx::sendAead(mac, ESPNOW_V4_TYPE_SENSOR_STATUS, 0, generateMessageId(), payload, len, 1);
}
// The bond-link control of the partner's sensors (BOND_STREAM_CTRL), the
// same one 'bondstream' and the bond page use. False: not sent (the partner is
// not synced, the queue is full).
static bool bfStreamCtrl(uint8_t sensor, bool on) { return sendBondStreamCtrl((RemoteSensorType)sensor, on); }
// The partner is connected and synced: a stream control can be sent.
static bool bfPartnerOnline() { return isBondModeOnline() && isBondSynced(); }
// The sensors this device's user chose to stream at the partner and that
// start again with every bond session (the saved 'bondstream' settings, step
// 9f). Not the runtime streaming flags: on a bond master with sensors of its
// own they also say what a mesh master subscribed here, which is not the
// partner's. What the user switched by hand this boot reaches the leases
// through bondForwardStreamByHand.
static espnowsensor::SensorMask bfSavedStreams() {
  espnowsensor::SensorMask mask = 0;
  for (uint8_t i = 0; i < REMOTE_SENSOR_MAX; ++i) {
    const bool* saved = bondStreamSetting((RemoteSensorType)i);
    if (saved && *saved) mask = mask | espnowsensor::sensorBit(i);
  }
  return mask;
}
static_assert(espnowfwd::kReqSubscribe == SENSOR_REQ_SUBSCRIBE && espnowfwd::kReqUnsubscribe == SENSOR_REQ_UNSUBSCRIBE &&
                  espnowfwd::kReqOneshot == SENSOR_REQ_ONESHOT,
              "the request modes of System_ESPNow_ForwardCore.h are those of SENSOR_REQ");
static void bfName(const uint8_t mac[6], char* out, size_t cap) {
  strlcpy(out, getEspNowDeviceName(mac).c_str(), cap);
  if (!out[0]) macToDisplay(mac, out, cap);
}
static void bfConsole(const char* fmt, ...) {
  char line[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  broadcastOutput(line);
}
static char* bfReply(size_t* cap) {
  if (!ensureDebugBuffer()) return nullptr;
  *cap = GLOBAL_DEBUG_BUFFER_SIZE;
  return getDebugBuffer();
}

// BOND_FORWARD_HOST_BEGIN

using SensorMask = espnowsensor::SensorMask;

constexpr uint8_t kBfStreamReadings = 0;
constexpr uint8_t kBfStreamBattery = 1;
constexpr uint8_t kBfStreamEvents = 2;

struct BfCounts {
  uint32_t forwarded[3];
  uint32_t notForwarded[3][espnowfwd::kWhyNotCount];
  uint32_t answersIgnored;
};

// What a command or the web page shows. Written by the tick (espnow_task)
// under sBfMux.
struct BfTargetView {
  bool have;
  bool isPartner;
  bool inPrimary;   // paired in this device's primary mesh
  bool session;
  uint8_t mac[6];
  espnowfwd::TState state;
  uint8_t lastResult;
  bool haveResult;   // lastResult is an answer (kResultOk is 0)
  uint8_t streams;
};
struct BfView {
  espnowfwd::WhyNot why;
  bool linkActive;
  uint8_t linkGen;
  uint8_t partner[6];
  BfTargetView targets[2];
};

// The mesh master's sensor requests for the partner. Under sBfMux.
struct BfRequests {
  espnowfwd::ReqLeases leases;
  uint8_t partner[6];   // the partner the leases and the user's streams are on (set by the tick)
  uint32_t applied;
  uint32_t refused;
  uint8_t lastRefused;  // espnowfwd::ReqVerdict (0: none)
  uint32_t refusedBy[espnowfwd::kReqVerdictCount];  // per reason (Apply unused)
  // Requests that asked for a sensor the partner does not list: refused
  // whole (partner_lacks_sensor) or with those sensors left out.
  uint32_t lacking;
  SensorMask lastLacking;  // the sensors the last such request asked for in vain
};

struct BfState {
  // ---- espnow_task only ----
  // The partner's last sensor status (BOND_SENSOR_STATUS), sent on to the
  // primary when it accepts, so it lists the partner's sensors at once.
  bool haveStatus;
  uint8_t statusFrom[6];
  espnowfwd::StatusFrame status;
  WarnLimiter reqWarn;
  WarnLimiter lackWarn;  // "... left out: the partner does not have it running"

  espnowfwd::StandingLink link;
  espnowfwd::AgreeMemo agree;
  uint8_t lastGen;                     // the last link number used in this boot (0: none)
  uint8_t lastPartner[6];              // the partner of the last link (for a withdrawal)
  espnowfwd::TargetState targets[2];   // primary, backup
  espnowfwd::WhyNot tickWhy;
  uint32_t lastTickMs;
  bool ticked;
  WarnLimiter warn;
  uint32_t sinceLine;
  uint8_t lastStream;
  uint8_t lastReason;
  uint8_t part[espnowsensor::kFrameCapDirect];  // one part of a forwarded reading
  // ---- under sBfMux ----
  BfCounts counts;
  BfView view;
  BfRequests req;
};

EXT_RAM_BSS_ATTR static BfState sBf;  // PSRAM: tasks only, never the radio callback

static uint8_t bfStreamOf(uint8_t streamId) {
  return streamId < REMOTE_SENSOR_MAX ? kBfStreamReadings
       : (streamId == espnowfwd::kStreamIdBattery ? kBfStreamBattery : kBfStreamEvents);
}

static void bfCount(uint8_t stream, espnowfwd::WhyNot why) {
  if (stream > kBfStreamEvents || why >= espnowfwd::WhyNot::Count) return;
  BF_LOCK();
  if (why == espnowfwd::WhyNot::Forwards) sBf.counts.forwarded[stream]++;
  else sBf.counts.notForwarded[stream][(size_t)why]++;
  BF_UNLOCK();
  if (why != espnowfwd::WhyNot::Forwards) {
    sBf.sinceLine++;
    sBf.lastStream = stream;
    sBf.lastReason = (uint8_t)why;
  }
}

static void bfSendFrame(const uint8_t to[6], const espnowfwd::LinkFrame& f) {
  uint8_t payload[espnowfwd::kLinkFrameLen];
  espnowfwd::packLinkFrame(payload, f);
  (void)bfSendLink(to, payload, (uint16_t)sizeof(payload));
}

static void bfSendAnnounce(const espnowfwd::TargetState& t) {
  espnowfwd::LinkFrame f = {};
  f.op = espnowfwd::kOpAnnounce;
  f.linkGen = sBf.link.linkGen;
  f.viaBootTag = bfBootTag();
  f.streams = t.streams;
  f.sensorMask = (t.streams & espnowfwd::kStreamReadings) ? t.sensorMask : 0;
  f.announceGen = t.announceGen;  // stepped by targetStep for this announcement
  memcpy(f.partnerMac, sBf.link.partner, 6);
  memcpy(f.partnerKey, sBf.link.partnerKey, sizeof(f.partnerKey));
  char name[40];
  bfName(sBf.link.partner, name, sizeof(name));
  // The device-name rule: a name that does not pass it goes as none (the
  // mesh master then shows the partner's address).
  devicename::putField(f.partnerName, name);
  bfSendFrame(t.mac, f);
}

static void bfSendWithdraw(const uint8_t to[6], uint8_t gen, const uint8_t partner[6]) {
  espnowfwd::LinkFrame f = {};
  f.op = espnowfwd::kOpWithdraw;
  f.linkGen = gen;
  f.viaBootTag = bfBootTag();
  if (partner) memcpy(f.partnerMac, partner, 6);
  bfSendFrame(to, f);
}

// Which named masters are paired in this device's primary mesh (bit 0 the
// primary, bit 1 the backup). The pairing lookup runs outside the lock.
static uint8_t bfInPrimaryMask(const SensorReceiverTargets& tg) {
  uint8_t mask = 0;
  if (tg.havePrimary && bfInPrimaryMesh(tg.primary)) mask |= 1;
  if (tg.haveBackup && bfInPrimaryMesh(tg.backup)) mask |= 2;
  return mask;
}

static espnowfwd::DecideInputs bfInputs(const BfBond& b, const SensorReceiverTargets& tg, bool agreementOk,
                                        uint8_t inPrimary) {
  espnowfwd::DecideInputs in = {};
  in.settingOn = bfSettingOn();
  in.bondMaster = b.modeOn && b.havePartner && b.isMaster && b.partnerPaired;
  in.agreementOk = agreementOk;
  in.meshOn = bfMeshOn();
  in.meshRole = bfRole();
  in.masterNamed = tg.masterNamed || tg.backupNamed;
  in.targetsPaired = (uint8_t)((tg.havePrimary ? 1 : 0) + (tg.haveBackup ? 1 : 0));
  in.targetsInPrimary = (uint8_t)(((inPrimary & 1) ? 1 : 0) + ((inPrimary & 2) ? 1 : 0));
  return in;
}

// The partner's sensors a lease switches on (and the bond session the "on"
// went over, so a new session sends it again). espnow_task.
static void bfLeaseOn(SensorMask on, uint16_t partnerSid) {
  for (uint8_t i = 0; i < REMOTE_SENSOR_MAX; ++i) {
    const SensorMask bit = espnowsensor::sensorBit(i);
    if (!(on & bit)) continue;
    const bool sent = bfStreamCtrl(i, true);
    BF_LOCK();
    sBf.req.leases.onSession[i] = sent ? partnerSid : 0;  // 0: sent again on the next tick
    BF_UNLOCK();
  }
}

// Every "off" a lease's end owes the partner; one that could not be sent
// stays owed and goes again on the next tick. espnow_task.
static void bfSendOwedOff() {
  BF_LOCK();
  const SensorMask owed = sBf.req.leases.offOwed;
  BF_UNLOCK();
  for (uint8_t i = 0; owed && i < REMOTE_SENSOR_MAX; ++i) {
    const SensorMask bit = espnowsensor::sensorBit(i);
    if (!(owed & bit) || !bfStreamCtrl(i, false)) continue;
    BF_LOCK();
    espnowfwd::leaseOffSent(sBf.req.leases, bit);
    BF_UNLOCK();
  }
}

// Every lease ends; what a lease switched on at the partner is switched off.
// The leases are always on the partner there is now: the tick forgets them
// first when the partner changed or this device is no longer its bond master.
static void bfLeasesEnd() {
  BF_LOCK();
  (void)espnowfwd::leaseEndAll(sBf.req.leases);
  BF_UNLOCK();
  bfSendOwedOff();
}

// The partner's last sensor status, to the primary master when it carries
// readings, with the sensors it accepted only and this link's number.
static void bfSendPartnerStatus() {
  const espnowfwd::TargetState& t = sBf.targets[espnowfwd::kTargetPrimary];
  if (!sBf.haveStatus || !sBf.link.active || !espnowfwd::macSame(sBf.statusFrom, sBf.link.partner) || !t.have) return;
  if (!espnowfwd::targetCarries(t, bfSessionId(t.mac), espnowfwd::kStreamReadings, -1)) return;
  espnowfwd::StatusFrame g = sBf.status;
  g.presentMask &= t.acceptedMask;
  g.streamingMask &= t.acceptedMask;
  g.fwdLink = sBf.link.linkGen;
  uint8_t payload[espnowfwd::kStatusFrameLen];
  espnowfwd::packStatusFrame(payload, g);
  (void)bfSendStatus(t.mac, payload, (uint16_t)sizeof(payload));
}

void bondForwardTick(uint32_t nowMs) {
  if (sBf.ticked && (uint32_t)(nowMs - sBf.lastTickMs) < 1000) return;
  // While a partner change runs, the partner, the role and the agreement are
  // half old, half new (the agreement of the old partner is reset last): the
  // tick waits for it, as every bond step of espnow_task does (bondTickHeld).
  // Frames of either partner are held off meanwhile by the receive gate.
  const uint32_t epoch = bfEpoch();
  if (bfHeld(epoch)) return;
  BfBond b;
  bfBond(&b);
  if (bfHeld(epoch) || b.epoch != epoch) return;  // a change began while it was read
  sBf.ticked = true;
  sBf.lastTickMs = nowMs;
  // The leases, the "off" they owe and the user's streams belong to this
  // device as bond master of one partner: with another partner, or no longer
  // its bond master, they are forgotten (nothing is sent to anyone).
  {
    const bool masterOfPartner = b.modeOn && b.havePartner && b.isMaster;
    uint8_t partnerNow[6] = {0};
    if (masterOfPartner) memcpy(partnerNow, b.partner, 6);
    BF_LOCK();
    if (!espnowfwd::macSame(sBf.req.partner, partnerNow)) {
      espnowfwd::leaseForgetPartner(sBf.req.leases);
      memcpy(sBf.req.partner, partnerNow, 6);
    }
    BF_UNLOCK();
  }
  SensorReceiverTargets tg = {};
  bfTargets(&tg);
  const bool agreementOk = espnowfwd::agreementAllows(sBf.agree, b.agreeState, b.roleConflict, b.epoch);
  const uint8_t inPrimary = bfInPrimaryMask(tg);
  const espnowfwd::WhyNot why = espnowfwd::forwardDecide(bfInputs(b, tg, agreementOk, inPrimary));
  sBf.tickWhy = why;
  const bool want = why == espnowfwd::WhyNot::Forwards;

  // The standing link ends when forwarding stops, the partner or its key
  // changed, or a partner change ran (the hold epoch moved). The mesh
  // master's leases on the partner's sensors end with it.
  espnowfwd::StandingLink& link = sBf.link;
  if (link.active && (!want || !espnowfwd::macSame(link.partner, b.partner) || link.epoch != b.epoch ||
                      memcmp(link.partnerKey, b.partnerKey, sizeof(link.partnerKey)) != 0)) {
    for (auto& t : sBf.targets) espnowfwd::targetBeginWithdraw(t, link.linkGen, link.partner, nowMs);
    link.active = false;
    bfLeasesEnd();
  }
  if (want && !link.active) {
    sBf.lastGen = espnowfwd::linkGenNext(sBf.lastGen);
    link.active = true;
    memcpy(link.partner, b.partner, 6);
    memcpy(link.partnerKey, b.partnerKey, sizeof(link.partnerKey));
    link.epoch = b.epoch;
    link.linkGen = sBf.lastGen;
    memcpy(sBf.lastPartner, b.partner, 6);
  }

  const SensorMask mask = bfSensorMask();
  bool primaryGetsReadings = false;  // the primary target is wanted (the leases' master)
  BfTargetView views[2] = {};
  for (uint8_t kind = 0; kind < 2; ++kind) {
    espnowfwd::TargetState& t = sBf.targets[kind];
    const bool have = kind == espnowfwd::kTargetPrimary ? tg.havePrimary : tg.haveBackup;
    const uint8_t* mac = kind == espnowfwd::kTargetPrimary ? tg.primary : tg.backup;
    if (t.have && (!have || !espnowfwd::macSame(t.mac, mac))) {
      // Another master is named now: the one that was told about the link
      // gets one withdrawal, if its session is up. Its leases on the
      // partner's sensors end.
      if ((t.state != espnowfwd::TState::Idle || t.wTries) && bfSessionId(t.mac) != 0) {
        bfSendWithdraw(t.mac, t.state != espnowfwd::TState::Idle ? t.linkGen : t.wGen,
                       t.state != espnowfwd::TState::Idle ? sBf.lastPartner : t.wPartner);
      }
      memset(&t, 0, sizeof(t));
      if (kind == espnowfwd::kTargetPrimary) bfLeasesEnd();
    }
    if (!have) continue;
    if (!t.have) {
      memset(&t, 0, sizeof(t));
      t.have = true;
      memcpy(t.mac, mac, 6);
    }
    const bool isPartner = b.havePartner && espnowfwd::macSame(mac, b.partner);
    const bool inPrimaryMesh = (inPrimary & (1u << kind)) != 0;
    views[kind].have = true;
    views[kind].isPartner = isPartner;
    views[kind].inPrimary = inPrimaryMesh;
    memcpy(views[kind].mac, mac, 6);
    const uint16_t sid = bfSessionId(mac);
    views[kind].session = sid != 0;
    // The master named here may be the bond partner itself: it is never told
    // of a link and gets no data, but like any master this device forwards
    // nothing to, it hears the withdrawal of a link it was told of (it was the
    // master before it became the partner) and, once a session, that this
    // device forwards for nobody. No session is started with it: the bond owns
    // that session.
    // A master paired in another mesh than this device's primary mesh gets
    // nothing either: a link it was told of is withdrawn.
    espnowfwd::TargetWant w = {};
    w.want = want && link.active && !isPartner && inPrimaryMesh;
    w.linkGen = link.linkGen;
    w.streams = espnowfwd::streamsForTarget(kind);
    w.sensorMask = mask;
    if (kind == espnowfwd::kTargetPrimary) primaryGetsReadings = w.want;
    if (!w.want && link.active && t.state != espnowfwd::TState::Idle) {
      espnowfwd::targetBeginWithdraw(t, link.linkGen, link.partner, nowMs);
    }
    if (sid == 0) {
      if (!isPartner && (w.want || t.wTries)) (void)bfHandshake(mac);
      views[kind].state = t.state;
      views[kind].lastResult = t.lastResult;
      views[kind].haveResult = t.haveResult;
      views[kind].streams = t.acceptedStreams;
      continue;
    }
    const uint8_t act = espnowfwd::targetStep(t, w, sid, bfSettled(mac, nowMs), nowMs);
    if (act & espnowfwd::kActWithdraw) bfSendWithdraw(mac, t.wGen, t.wPartner);
    if (act & espnowfwd::kActWithdrawAll) bfSendWithdraw(mac, sBf.lastGen, nullptr);
    if (act & espnowfwd::kActAnnounce) bfSendAnnounce(t);
    views[kind].state = t.state;
    views[kind].lastResult = t.lastResult;
    views[kind].haveResult = t.haveResult;
    views[kind].streams = t.acceptedStreams;
  }

  // The mesh master's leases on the partner's sensors: those that ran out, or
  // whose sensor is no longer forwarded, or whose master no longer gets the
  // readings (it left the primary mesh), are switched off; those whose "on"
  // went over an older bond session (the partner restarted) are switched on
  // again. An "off" that could not be sent is sent again, also after the
  // link ended, while this device is still the partner's bond master.
  const bool online = bfPartnerOnline();
  if (link.active) {
    const uint16_t partnerSid = bfSessionId(link.partner);
    BF_LOCK();
    (void)espnowfwd::leaseExpire(sBf.req.leases, nowMs);
    (void)espnowfwd::leaseKeepOnly(sBf.req.leases, primaryGetsReadings ? mask : 0);
    const SensorMask on = online ? espnowfwd::leaseResend(sBf.req.leases, partnerSid) : 0;
    BF_UNLOCK();
    if (on) bfLeaseOn(on, partnerSid);
  }
  if (online) bfSendOwedOff();

  BF_LOCK();
  sBf.view.why = why;
  sBf.view.linkActive = link.active;
  sBf.view.linkGen = link.linkGen;
  memcpy(sBf.view.partner, b.havePartner ? b.partner : sBf.lastPartner, 6);
  memcpy(sBf.view.targets, views, sizeof(views));
  BF_UNLOCK();

  // One line a minute while frames of the partner are not forwarded.
  if (sBf.sinceLine && bfSettingOn() && warnLimiterTake(sBf.warn, nowMs, 60000, nullptr)) {
    static const char* const kStreamWords[3] = { "readings", "battery state", "events" };
    char partner[32];
    bfName(link.active ? link.partner : sBf.lastPartner, partner, sizeof(partner));
    bfConsole("[ESP-NOW] %s's %s not forwarded: %s (%lu frames since the last such line)", partner,
              kStreamWords[sBf.lastStream < 3 ? sBf.lastStream : 0],
              espnowfwd::whyNotText((espnowfwd::WhyNot)sBf.lastReason), (unsigned long)sBf.sinceLine);
    sBf.sinceLine = 0;
  }
}

bool bondForwardOnLinkFrame(const uint8_t src[6], const espnowfwd::LinkFrame& f) {
  espnowfwd::TargetState* t = nullptr;
  uint8_t kind = 0;
  for (uint8_t k = 0; k < 2; ++k) {
    if (sBf.targets[k].have && espnowfwd::macSame(sBf.targets[k].mac, src)) {
      t = &sBf.targets[k];
      kind = k;
    }
  }
  if (!t) {
    BF_LOCK();
    sBf.counts.answersIgnored++;
    BF_UNLOCK();
    return true;
  }
  if (f.op == espnowfwd::kOpQuery) {
    espnowfwd::targetQuery(*t);
    return true;
  }
  const espnowfwd::TState before = t->state;
  const espnowfwd::AnswerEffect effect = espnowfwd::targetAnswer(*t, f, bfBootTag(), bfNow());
  char master[32];
  char partner[32];
  bfName(src, master, sizeof(master));
  bfName(sBf.link.active ? sBf.link.partner : sBf.lastPartner, partner, sizeof(partner));
  switch (effect) {
    case espnowfwd::AnswerEffect::Accepted:
      if (before != espnowfwd::TState::Accepted) {
        bfConsole("[ESP-NOW] %s accepted: this device forwards %s's %s to it", master, partner,
                  (t->acceptedStreams & espnowfwd::kStreamReadings) ? "readings, battery state and events"
                                                                    : "battery state and events");
        // The partner's sensors are listed there before its first reading.
        if (kind == espnowfwd::kTargetPrimary) bfSendPartnerStatus();
      }
      break;
    case espnowfwd::AnswerEffect::Refused:
      bfConsole("[ESP-NOW] %s refused forwarding for %s: %s", master, partner, espnowfwd::resultText(f.result));
      break;
    case espnowfwd::AnswerEffect::Stale:
      // The master holds a newer number of this boot: a new link number.
      if (sBf.link.active) {
        sBf.lastGen = espnowfwd::linkGenNext(sBf.lastGen);
        sBf.link.linkGen = sBf.lastGen;
      }
      break;
    case espnowfwd::AnswerEffect::Ignored:
      BF_LOCK();
      sBf.counts.answersIgnored++;
      BF_UNLOCK();
      break;
    case espnowfwd::AnswerEffect::WithdrawDone:
    case espnowfwd::AnswerEffect::Noted:
      break;
  }
  return true;
}

// Whether a frame of the partner may be forwarded at all now.
static espnowfwd::WhyNot bfFrameCheck(const uint8_t src[6]) {
  return espnowfwd::forwardFrameAllowed(sBf.link, src, bfSettingOn(), bfDeclined(), bfEpoch(), sBf.tickWhy);
}

void bondForwardReading(const uint8_t src[6], const espnowsensor::ReadingHeader& h, const uint8_t* text, size_t len) {
  espnowfwd::WhyNot why = bfFrameCheck(src);
  if (why != espnowfwd::WhyNot::Forwards) {
    bfCount(kBfStreamReadings, why);
    return;
  }
  const espnowfwd::TargetState& t = sBf.targets[espnowfwd::kTargetPrimary];
  if (!t.have) {
    bfCount(kBfStreamReadings, espnowfwd::WhyNot::NoMaster);
    return;
  }
  const uint16_t sid = bfSessionId(t.mac);
  if (!espnowfwd::targetCarries(t, sid, espnowfwd::kStreamReadings, h.sensorType)) {
    bfCount(kBfStreamReadings, espnowfwd::targetWhyNot(t, sid, espnowfwd::kStreamReadings, h.sensorType));
    return;
  }
  espnowsensor::PartPlan plan;
  if (!text || !espnowsensor::planParts(len, bfFrameCap(t.mac), plan)) {
    bfCount(kBfStreamReadings, espnowfwd::WhyNot::TooLong);
    return;
  }
  // The partner's header as received, flag bits this firmware does not know
  // included (1.0 rule W2); only the forwarding fields and the parts change.
  espnowsensor::ReadingHeader fh = h;
  fh.flags = (uint8_t)(h.flags | espnowsensor::kReadingFlagForwarded);
  fh.fwdLink = sBf.link.linkGen;
  fh.partCount = plan.count;
  for (uint8_t i = 0; i < plan.count; ++i) {
    size_t offset = 0, n = 0;
    espnowsensor::partSpan(plan, len, i, offset, n);
    fh.partIndex = i;
    espnowsensor::packReadingHeader(sBf.part, fh);
    memcpy(sBf.part + espnowsensor::kReadingHeaderLen, text + offset, n);
    if (!bfSendPart(t.mac, sBf.part, (uint16_t)(espnowsensor::kReadingHeaderLen + n))) {
      bfCount(kBfStreamReadings, espnowfwd::WhyNot::QueueFull);
      return;
    }
  }
  bfCount(kBfStreamReadings, espnowfwd::WhyNot::Forwards);
}

// A battery or event frame to every accepted target that carries it: the
// received `len` bytes when they fit the target's path, else the frame's 1.0
// length `frameLen` (espnowsensor::forwardSendLen).
template <typename SendFn>
static void bfToTargets(uint8_t stream, uint8_t streamBit, const uint8_t* payload, size_t len, size_t frameLen,
                        SendFn send) {
  bool any = false;
  bool queueFull = false;
  espnowfwd::WhyNot first = espnowfwd::WhyNot::NoMaster;
  bool haveFirst = false;
  for (uint8_t kind = 0; kind < 2; ++kind) {
    const espnowfwd::TargetState& t = sBf.targets[kind];
    if (!t.have) continue;
    const uint16_t sid = bfSessionId(t.mac);
    if (espnowfwd::targetCarries(t, sid, streamBit, -1)) {
      const size_t n = espnowsensor::forwardSendLen(len, frameLen, bfFrameCap(t.mac));
      if (!n) {
        // Cannot happen at 1.0 (both frames fit a routed frame); counted, not sent.
        if (!haveFirst) {
          first = espnowfwd::WhyNot::TooLong;
          haveFirst = true;
        }
      } else if (send(t.mac, payload, (uint16_t)n)) {
        any = true;
      } else {
        queueFull = true;
      }
    } else if (!haveFirst) {
      first = espnowfwd::targetWhyNot(t, sid, streamBit, -1);
      haveFirst = true;
    }
  }
  if (any) bfCount(stream, espnowfwd::WhyNot::Forwards);
  else bfCount(stream, queueFull ? espnowfwd::WhyNot::QueueFull : first);
}

// The partner's battery and event frames go on verbatim, at their full
// received length, with only the forwarding fields patched (1.0 rule W2):
// what a later firmware appends or sets in reserved bytes reaches the master.
void bondForwardPower(const uint8_t src[6], const uint8_t* payload, size_t len) {
  const espnowfwd::WhyNot why = bfFrameCheck(src);
  if (why != espnowfwd::WhyNot::Forwards) {
    bfCount(kBfStreamBattery, why);
    return;
  }
  uint8_t out[espnowsensor::kForwardCopyMax];
  const size_t n = espnowsensor::forwardPowerCopy(out, payload, len, sBf.link.linkGen);
  if (!n) return;  // shorter than the frame: cannot happen, the ingest parsed it
  bfToTargets(kBfStreamBattery, espnowfwd::kStreamBattery, out, n, espnowsensor::kPowerFrameLen, bfSendPower);
}

void bondForwardEvent(const uint8_t src[6], const uint8_t* payload, size_t len) {
  const espnowfwd::WhyNot why = bfFrameCheck(src);
  if (why != espnowfwd::WhyNot::Forwards) {
    bfCount(kBfStreamEvents, why);
    return;
  }
  uint8_t out[espnowsensor::kForwardCopyMax];
  const size_t n = espnowsensor::forwardEventCopy(out, payload, len, sBf.link.linkGen);
  if (!n) return;  // shorter than the frame: cannot happen, the ingest parsed it
  bfToTargets(kBfStreamEvents, espnowfwd::kStreamEvents, out, n, espnowsensor::kEventFrameLen, bfSendEvent);
}

void bondForwardNotKept(uint8_t streamId) {
  if (!bfSettingOn()) return;
  bfCount(bfStreamOf(streamId), espnowfwd::WhyNot::NotKeptHere);
}

void bondForwardStatus(const uint8_t src[6], const espnowfwd::StatusFrame& f) {
  if (!src) return;
  sBf.haveStatus = true;
  memcpy(sBf.statusFrom, src, 6);
  sBf.status = f;
  sBf.status.fwdLink = 0;
  if (bfFrameCheck(src) != espnowfwd::WhyNot::Forwards) return;
  bfSendPartnerStatus();
}

static void bfSensorWords(SensorMask mask, char* out, size_t cap);
static void bfRequestLines(char* out, size_t cap, size_t* pos);

void bondForwardSensorRequest(const uint8_t src[6], uint8_t mode, uint32_t sensorMask, uint32_t intervalMs,
                              uint32_t leaseMs, uint8_t fwdLink) {
  if (!src) return;
  const uint32_t nowMs = bfNow();
  const uint32_t epoch = bfEpoch();
  BfBond b;
  bfBond(&b);
  const espnowfwd::TargetState& t = sBf.targets[espnowfwd::kTargetPrimary];
  espnowfwd::ReqInputs in = {};
  in.settingOn = bfSettingOn();
  in.bondMaster = b.modeOn && b.havePartner && b.isMaster && b.partnerPaired;
  in.linkActive = sBf.link.active && !bfHeld(epoch) && sBf.link.epoch == epoch &&
                  espnowfwd::macSame(sBf.link.partner, b.partner);
  in.linkGen = sBf.link.linkGen;
  in.fwdLink = fwdLink;
  in.fromAcceptedPrimary = t.have && espnowfwd::macSame(t.mac, src) &&
                           espnowfwd::targetCarries(t, bfSessionId(src), espnowfwd::kStreamReadings, -1);
  in.mode = mode;
  in.mask = (SensorMask)(sensorMask & espnowsensor::kSensorMaskAll);
  in.forwardedMask = t.acceptedMask;
  in.partnerOnline = bfPartnerOnline();
  // The partner's own sensor list (BOND_SENSOR_STATUS, kept by
  // bondForwardStatus): a sensor it does not list is never leased.
  in.partnerStatusKnown = sBf.haveStatus && espnowfwd::macSame(sBf.statusFrom, b.partner);
  in.partnerPresent =
      in.partnerStatusKnown ? (SensorMask)(sBf.status.presentMask & espnowsensor::kSensorMaskAll) : (SensorMask)0;
  const espnowfwd::ReqVerdict verdict = espnowfwd::sensorReqDecide(in);
  const SensorMask lacking = verdict == espnowfwd::ReqVerdict::PartnerLacksSensor
                               ? (SensorMask)(in.mask & in.forwardedMask)
                               : (verdict == espnowfwd::ReqVerdict::Apply ? espnowfwd::reqLeftOut(in) : (SensorMask)0);
  if (lacking) {
    BF_LOCK();
    sBf.req.lacking++;
    sBf.req.lastLacking = lacking;
    BF_UNLOCK();
  }
  if (verdict != espnowfwd::ReqVerdict::Apply) {
    BF_LOCK();
    sBf.req.refused++;
    sBf.req.lastRefused = (uint8_t)verdict;
    sBf.req.refusedBy[(size_t)verdict]++;
    BF_UNLOCK();
    uint32_t held = 0;
    if (warnLimiterTake(sBf.reqWarn, nowMs, 60000, &held)) {
      char master[32];
      bfName(src, master, sizeof(master));
      char words[160] = "";
      if (lacking) {
        words[0] = ' ';
        words[1] = '(';
        bfSensorWords(lacking, words + 2, sizeof(words) - 3);
        strlcat(words, ")", sizeof(words));
      }
      bfConsole("[ESP-NOW] Sensor request from %s for the bond partner refused: %s%s (%lu more in the last minute)",
                master, espnowfwd::reqVerdictText(verdict), words, (unsigned long)held);
    }
    return;
  }
  // The sensors X streams already (its status says so) or this device's user
  // streams there (saved; by hand: the leases' byUser) are not started by the
  // lease, and its end leaves them on.
  SensorMask streaming = bfSavedStreams();
  if (sBf.haveStatus && espnowfwd::macSame(sBf.statusFrom, sBf.link.partner)) {
    streaming = (SensorMask)(streaming | (sBf.status.streamingMask & espnowsensor::kSensorMaskAll));
  }
  const SensorMask mask = espnowfwd::reqApplyMask(in);
  if (lacking) {
    uint32_t held = 0;
    if (warnLimiterTake(sBf.lackWarn, nowMs, 60000, &held)) {
      char master[32];
      bfName(src, master, sizeof(master));
      char words[160];
      bfSensorWords(lacking, words, sizeof(words));
      bfConsole("[ESP-NOW] Sensor request from %s for the bond partner: %s left out: %s (%lu more in the last minute)",
                master, words, espnowfwd::reqVerdictText(espnowfwd::ReqVerdict::PartnerLacksSensor),
                (unsigned long)held);
    }
  }
  BF_LOCK();
  const espnowfwd::LeaseActs acts =
      espnowfwd::leaseApply(sBf.req.leases, mode, mask, streaming, intervalMs, leaseMs, nowMs);
  sBf.req.applied++;
  BF_UNLOCK();
  if (acts.on) bfLeaseOn(acts.on, bfSessionId(sBf.link.partner));
  if (acts.off) bfSendOwedOff();
}

void bondForwardStreamByHand(uint8_t sensor, bool on) {
  BF_LOCK();
  espnowfwd::leaseByHand(sBf.req.leases, sensor, on);
  BF_UNLOCK();
}

// ---- What is shown (cmd_exec, the web server) ------------------------------------

static bool bfAppend(char* out, size_t cap, size_t* pos, const char* fmt, ...) {
  if (*pos >= cap) return false;
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(out + *pos, cap - *pos, fmt, args);
  va_end(args);
  if (n < 0) return false;
  *pos += (size_t)n;
  if (*pos >= cap) { *pos = cap; return false; }
  return true;
}

static void bfSnapshot(BfView* view, BfCounts* counts) {
  BF_LOCK();
  if (view) *view = sBf.view;
  if (counts) *counts = sBf.counts;
  BF_UNLOCK();
}

// What forwardDecide says now, from the settings as they are (the tick may
// not have run since a command changed one).
static espnowfwd::WhyNot bfWhyNow(BfBond* bondOut) {
  BfBond b;
  bfBond(&b);
  SensorReceiverTargets tg = {};
  bfTargets(&tg);
  espnowfwd::AgreeMemo memo = sBf.agree;  // a copy: the tick owns the real one
  const bool agreementOk = espnowfwd::agreementAllows(memo, b.agreeState, b.roleConflict, b.epoch);
  if (bondOut) *bondOut = b;
  return espnowfwd::forwardDecide(bfInputs(b, tg, agreementOk, bfInPrimaryMask(tg)));
}

static void bfSensorWords(SensorMask mask, char* out, size_t cap) {
  size_t pos = 0;
  out[0] = '\0';
  bool first = true;
  for (uint8_t k = 0; k < REMOTE_SENSOR_MAX; ++k) {
    if (!(mask & espnowsensor::sensorBit(k))) continue;
    bfAppend(out, cap, &pos, "%s%s", first ? "" : ", ", espnowsensor::sensorName(k));
    first = false;
  }
  if (first) bfAppend(out, cap, &pos, "none");
}

// "hub (accepted)", "hub (waiting for it to accept)", ...
static void bfTargetWords(const BfTargetView& t, char* out, size_t cap) {
  char name[32];
  bfName(t.mac, name, sizeof(name));
  if (!t.session) {
    snprintf(out, cap, "%s (no session with this device yet)", name);
  } else if (t.state == espnowfwd::TState::Accepted) {
    snprintf(out, cap, "%s (accepted)", name);
  } else if (t.state == espnowfwd::TState::Refused) {
    snprintf(out, cap, "%s (refused: %s)", name, espnowfwd::resultText(t.lastResult));
  } else {
    snprintf(out, cap, "%s (waiting for it to accept)", name);
  }
}

// What 'bondforward' says (`lead` "OK: forwarding is "), or the line of
// 'bondstatus' (`lead` "Forwarding to the mesh: ").
static const char kBfLeadCommand[] = "OK: forwarding is ";
static const char kBfLeadLine[] = "Forwarding to the mesh: ";

// `lines`: the form of 'bondstatus', whose lines go to the console one by one
// (each at most espnowfwd::kStatusLineMax): a second master is said on a line of
// its own.
static const char* bfDescribe(char* reply, size_t cap, const char* lead, bool lines = false) {
  BfBond b;
  const espnowfwd::WhyNot why = bfWhyNow(&b);
  BfView view;
  bfSnapshot(&view, nullptr);
  char partner[32] = "the partner";
  if (b.havePartner) bfName(b.partner, partner, sizeof(partner));
  size_t pos = 0;
  if (!bfSettingOn()) {
    bfAppend(reply, cap, &pos, "%soff.", lead);
    return reply;
  }
  if (b.modeOn && b.havePartner && !b.isMaster) {
    if (lead == kBfLeadCommand) {
      bfAppend(reply, cap, &pos, "OK: stored, but this device is the bond worker: only a bond master forwards. Its own "
                                 "data goes to its bond master only.");
    } else {
      bfAppend(reply, cap, &pos, "%son, but this device is the bond worker: only a bond master forwards.", lead);
    }
    return reply;
  }
  if (why == espnowfwd::WhyNot::IsMaster) {
    bfAppend(reply, cap, &pos, "%son. This device is the mesh master: %s's readings, battery state and "
                               "events are kept here; nothing goes over the air.", lead, partner);
    return reply;
  }
  if (why != espnowfwd::WhyNot::Forwards) {
    bfAppend(reply, cap, &pos, "%son, but nothing is forwarded yet: %s.", lead, espnowfwd::whyNotText(why));
    return reply;
  }
  // Forwarding is wanted: what each master named here said (a master the
  // tick has not looked at yet is waiting to be asked).
  SensorReceiverTargets tg = {};
  bfTargets(&tg);
  BfTargetView now[2] = {};
  for (uint8_t k = 0; k < 2; ++k) {
    const bool have = k == espnowfwd::kTargetPrimary ? tg.havePrimary : tg.haveBackup;
    if (!have) continue;
    const uint8_t* mac = k == espnowfwd::kTargetPrimary ? tg.primary : tg.backup;
    if (view.targets[k].have && espnowfwd::macSame(view.targets[k].mac, mac)) {
      now[k] = view.targets[k];
    } else {
      now[k].have = true;
      memcpy(now[k].mac, mac, 6);
      now[k].isPartner = b.havePartner && espnowfwd::macSame(mac, b.partner);
      now[k].state = espnowfwd::TState::Announcing;
    }
    now[k].inPrimary = bfInPrimaryMesh(mac);
    now[k].session = bfSessionId(mac) != 0;
  }
  memcpy(view.targets, now, sizeof(now));
  const BfTargetView* accepted[2] = { nullptr, nullptr };
  const BfTargetView* other = nullptr;
  for (uint8_t k = 0; k < 2; ++k) {
    const BfTargetView& t = view.targets[k];
    if (!t.have || t.isPartner || !t.inPrimary) continue;
    if (t.session && t.state == espnowfwd::TState::Accepted) accepted[k] = &t;
    else if (!other) other = &t;
  }
  if (!accepted[0] && !accepted[1]) {
    if (!other) {
      bfAppend(reply, cap, &pos, "%son, but nothing is forwarded yet: %s.", lead,
               espnowfwd::whyNotText(espnowfwd::WhyNot::TargetIsPartner));
      return reply;
    }
    char name[32];
    bfName(other->mac, name, sizeof(name));
    if (!other->session) {
      bfAppend(reply, cap, &pos, "%son, but nothing is forwarded yet: %s has no session with this "
                                 "device yet.", lead, name);
    } else if (other->state == espnowfwd::TState::Refused) {
      bfAppend(reply, cap, &pos, "%son, but nothing is forwarded yet: %s refused: %s.", lead, name,
               espnowfwd::resultText(other->lastResult));
    } else {
      bfAppend(reply, cap, &pos, "%son, but nothing is forwarded yet: waiting for %s to accept.", lead, name);
    }
    return reply;
  }
  bfAppend(reply, cap, &pos, "%son:", lead);
  bool first = true;
  for (uint8_t k = 0; k < 2; ++k) {
    const BfTargetView& t = view.targets[k];
    if (!t.have || t.isPartner || !t.inPrimary) continue;
    char words[160];
    bfTargetWords(t, words, sizeof(words));
    const bool readings = k == espnowfwd::kTargetPrimary;
    bfAppend(reply, cap, &pos, "%s %s's %s go%s to %s", first ? "" : (lines ? ";\n " : ";"), partner,
             readings ? "readings, battery state and events" : "battery state and events", first ? "" : " also",
             words);
    first = false;
  }
  bfAppend(reply, cap, &pos, ".");
  return reply;
}

// bondforward [on | off | sensors <all | default | name,name,...>]
const char* cmd_bond_forward(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  size_t cap = 0;
  char* reply = bfReply(&cap);
  if (!reply) return "Error: out of memory";
  reply[0] = '\0';
  char copy[160];
  strlcpy(copy, argsInput.c_str(), sizeof(copy));
  char* save = nullptr;
  char* verb = strtok_r(copy, " \t", &save);
  char* arg = verb ? strtok_r(nullptr, " \t", &save) : nullptr;
  char* extra = arg ? strtok_r(nullptr, " \t", &save) : nullptr;
  static const char kUsage[] = "Usage: bondforward [on | off | sensors <all | default | name,name,...>]";
  if (!verb) {
    bfDescribe(reply, cap, kBfLeadCommand);
    size_t pos = strnlen(reply, cap);
    bfRequestLines(reply, cap, &pos);
    return reply;
  }
  const int onOff = strcmp(verb, "sensors") == 0 ? -1 : bfParseBool(verb);
  if (onOff >= 0) {
    if (arg) return kUsage;
    bfSetSettingOn(onOff == 1);
    return bfDescribe(reply, cap, kBfLeadCommand);
  }
  if (strcmp(verb, "sensors") != 0 || !arg || extra) return kUsage;
  SensorMask mask = 0;
  if (strcmp(arg, "all") == 0) {
    mask = (SensorMask)espnowsensor::kSensorMaskAll;
  } else if (strcmp(arg, "default") == 0) {
    mask = espnowfwd::kSensorMaskDefault;
  } else {
    char list[128];
    strlcpy(list, arg, sizeof(list));
    char* inner = nullptr;
    for (char* name = strtok_r(list, ",", &inner); name; name = strtok_r(nullptr, ",", &inner)) {
      uint8_t id = 0;
      if (!espnowsensor::sensorIdFromName(name, id)) {
        static_assert(REMOTE_SENSOR_MAX == 11,
                      "a sensor type was added or removed: update the names in this 'is not a sensor (...)' "
                      "text of cmd_bond_forward (every kSensorTable name, in its order), then change the 11");
        snprintf(reply, cap, "Error: '%s' is not a sensor (thermal, tof, imu, gps, input, fmradio, camera, microphone, "
                             "rtc, presence, apds)", name);
        return reply;
      }
      mask = (SensorMask)(mask | espnowsensor::sensorBit(id));
    }
  }
  bfSetSensorMask(mask);
  char words[160];
  bfSensorWords(mask, words, sizeof(words));
  snprintf(reply, cap, "OK: forwarded sensors: %s%s", words,
           (mask & espnowsensor::sensorBit(REMOTE_SENSOR_INPUT)) ? ""
                                                                 : " (input is left out: bondforward sensors all)");
  return reply;
}

static void bfStreamsJson(uint8_t streams, char* out, size_t cap) {
  size_t pos = 0;
  out[0] = '\0';
  bfAppend(out, cap, &pos, "[");
  bool first = true;
  if (streams & espnowfwd::kStreamReadings) { bfAppend(out, cap, &pos, "\"readings\""); first = false; }
  if (streams & espnowfwd::kStreamBattery) { bfAppend(out, cap, &pos, "%s\"battery\"", first ? "" : ","); first = false; }
  if (streams & espnowfwd::kStreamEvents) bfAppend(out, cap, &pos, "%s\"events\"", first ? "" : ",");
  bfAppend(out, cap, &pos, "]");
}

size_t bondForwardStatusJson(char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  BfView view;
  BfCounts counts;
  bfSnapshot(&view, &counts);
  const espnowfwd::WhyNot why = bfWhyNow(nullptr);
  size_t pos = 0;
  bfAppend(out, cap, &pos, "{\"setting\":%s,\"sensors\":[", bfSettingOn() ? "true" : "false");
  const SensorMask mask = bfSensorMask();
  bool first = true;
  for (uint8_t k = 0; k < REMOTE_SENSOR_MAX; ++k) {
    if (!(mask & espnowsensor::sensorBit(k))) continue;
    bfAppend(out, cap, &pos, "%s\"%s\"", first ? "" : ",", espnowsensor::sensorName(k));
    first = false;
  }
  bfAppend(out, cap, &pos, "],\"forwards\":%s", why == espnowfwd::WhyNot::Forwards ? "true" : "false");
  if (why == espnowfwd::WhyNot::Forwards) bfAppend(out, cap, &pos, ",\"whyNot\":null");
  else bfAppend(out, cap, &pos, ",\"whyNot\":\"%s\"", espnowfwd::whyNotKey(why));
  if (view.linkActive) bfAppend(out, cap, &pos, ",\"linkGen\":%u", (unsigned)view.linkGen);
  else bfAppend(out, cap, &pos, ",\"linkGen\":null");
  if (!espnowfwd::macIsZero(view.partner)) {
    char name[32];
    char nameJson[72];
    bfName(view.partner, name, sizeof(name));
    espnowsensor::jsonTextEscape(name, nameJson, sizeof(nameJson));
    bfAppend(out, cap, &pos, ",\"partner\":{\"mac\":\"" MAC_FMT "\",\"name\":\"%s\"}", MAC_ARGS(view.partner), nameJson);
  } else {
    bfAppend(out, cap, &pos, ",\"partner\":null");
  }
  bfAppend(out, cap, &pos, ",\"targets\":[");
  first = true;
  for (uint8_t k = 0; k < 2; ++k) {
    const BfTargetView& t = view.targets[k];
    if (!t.have) continue;
    char name[32];
    char nameJson[72];
    char streams[48];
    bfName(t.mac, name, sizeof(name));
    espnowsensor::jsonTextEscape(name, nameJson, sizeof(nameJson));
    bfStreamsJson(espnowfwd::streamsForTarget(k), streams, sizeof(streams));
    bfAppend(out, cap, &pos,
             "%s{\"kind\":\"%s\",\"mac\":\"" MAC_FMT "\",\"name\":\"%s\",\"isPartner\":%s,\"inPrimaryMesh\":%s,"
             "\"state\":\"%s\"",
             first ? "" : ",", k == espnowfwd::kTargetPrimary ? "primary" : "backup", MAC_ARGS(t.mac), nameJson,
             t.isPartner ? "true" : "false", t.inPrimary ? "true" : "false", espnowfwd::targetStateKey(t.state));
    if (t.haveResult) bfAppend(out, cap, &pos, ",\"result\":\"%s\"", espnowfwd::resultKey(t.lastResult));
    else bfAppend(out, cap, &pos, ",\"result\":null");
    bfAppend(out, cap, &pos, ",\"streams\":%s,\"session\":%s}", streams, t.session ? "true" : "false");
    first = false;
  }
  static const char* const kStreamKeys[3] = { "readings", "battery", "events" };
  bfAppend(out, cap, &pos, "],\"forwarded\":{\"readings\":%lu,\"battery\":%lu,\"events\":%lu},\"notForwarded\":{",
           (unsigned long)counts.forwarded[0], (unsigned long)counts.forwarded[1], (unsigned long)counts.forwarded[2]);
  for (uint8_t s = 0; s < 3; ++s) {
    bfAppend(out, cap, &pos, "%s\"%s\":{", s ? "," : "", kStreamKeys[s]);
    bool firstReason = true;
    for (size_t r = 1; r < espnowfwd::kWhyNotCount; ++r) {
      if (!counts.notForwarded[s][r]) continue;
      bfAppend(out, cap, &pos, "%s\"%s\":%lu", firstReason ? "" : ",", espnowfwd::whyNotKey((espnowfwd::WhyNot)r),
               (unsigned long)counts.notForwarded[s][r]);
      firstReason = false;
    }
    bfAppend(out, cap, &pos, "}");
  }
  BfRequests req;
  BF_LOCK();
  req = sBf.req;
  BF_UNLOCK();
  bfAppend(out, cap, &pos, "},\"answersIgnored\":%lu,\"sensorRequests\":{\"applied\":%lu,\"refused\":%lu,\"lastRefused\":",
           (unsigned long)counts.answersIgnored, (unsigned long)req.applied, (unsigned long)req.refused);
  if (req.lastRefused) {
    bfAppend(out, cap, &pos, "\"%s\"", espnowfwd::reqVerdictKey((espnowfwd::ReqVerdict)req.lastRefused));
  } else {
    bfAppend(out, cap, &pos, "null");
  }
  bfAppend(out, cap, &pos, ",\"refusedBy\":{");
  first = true;
  for (size_t r = 1; r < espnowfwd::kReqVerdictCount; ++r) {
    if (!req.refusedBy[r]) continue;
    bfAppend(out, cap, &pos, "%s\"%s\":%lu", first ? "" : ",", espnowfwd::reqVerdictKey((espnowfwd::ReqVerdict)r),
             (unsigned long)req.refusedBy[r]);
    first = false;
  }
  bfAppend(out, cap, &pos, "},\"partnerLacks\":{\"count\":%lu,\"last\":[", (unsigned long)req.lacking);
  first = true;
  for (uint8_t k = 0; k < REMOTE_SENSOR_MAX; ++k) {
    if (!(req.lastLacking & espnowsensor::sensorBit(k))) continue;
    bfAppend(out, cap, &pos, "%s\"%s\"", first ? "" : ",", espnowsensor::sensorName(k));
    first = false;
  }
  bfAppend(out, cap, &pos, "]},\"leased\":[");
  first = true;
  for (uint8_t k = 0; k < REMOTE_SENSOR_MAX; ++k) {
    if (!(req.leases.leased & espnowsensor::sensorBit(k))) continue;
    bfAppend(out, cap, &pos, "%s\"%s\"", first ? "" : ",", espnowsensor::sensorName(k));
    first = false;
  }
  const bool whole = bfAppend(out, cap, &pos, "]}}");
  if (!whole) {
    out[0] = '\0';
    return 0;
  }
  return pos;
}

// The mesh master's sensor requests for the partner, as lines of 'bondstatus'
// and 'bondforward': nothing before the first request.
static void bfRequestLines(char* out, size_t cap, size_t* pos) {
  BfRequests req;
  BF_LOCK();
  req = sBf.req;
  BF_UNLOCK();
  if (!req.applied && !req.refused) return;
  char leased[160];
  bfSensorWords(req.leases.leased, leased, sizeof(leased));
  bfAppend(out, cap, pos, "\n  Sensor requests for the partner: %lu applied, %lu refused",
           (unsigned long)req.applied, (unsigned long)req.refused);
  if (req.lastRefused) {
    bfAppend(out, cap, pos, " (last: %s)", espnowfwd::reqVerdictText((espnowfwd::ReqVerdict)req.lastRefused));
  }
  bfAppend(out, cap, pos, "; leased now: %s.", leased);
  const uint32_t unknown = req.refusedBy[(size_t)espnowfwd::ReqVerdict::PartnerStatusUnknown];
  if (!req.lacking && !unknown) return;
  if (req.lacking) {
    char lacking[160];
    bfSensorWords(req.lastLacking, lacking, sizeof(lacking));
    bfAppend(out, cap, pos, "\n  Sensors the partner does not have running, asked for %lu times (last: %s): never leased",
             (unsigned long)req.lacking, lacking);
    if (unknown) bfAppend(out, cap, pos, "; %lu requests came before its sensor list was known", (unsigned long)unknown);
  } else {
    bfAppend(out, cap, pos, "\n  %lu requests came before the partner's sensor list was known", (unsigned long)unknown);
  }
  bfAppend(out, cap, pos, ".");
}

void bondForwardStatusLines(char* out, size_t cap) {
  if (!out || cap == 0) return;
  BfCounts counts;
  bfSnapshot(nullptr, &counts);
  size_t pos = 0;
  out[0] = '\0';
  bfDescribe(out, cap, kBfLeadLine, true);
  pos = strnlen(out, cap);
  uint32_t notForwarded = 0;
  size_t topStream = 0, topReason = 0;
  for (size_t s = 0; s < 3; ++s) {
    for (size_t r = 1; r < espnowfwd::kWhyNotCount; ++r) {
      notForwarded += counts.notForwarded[s][r];
      if (counts.notForwarded[s][r] > counts.notForwarded[topStream][topReason]) {
        topStream = s;
        topReason = r;
      }
    }
  }
  bfAppend(out, cap, &pos, "\n  Forwarded %lu readings, %lu battery frames, %lu events",
           (unsigned long)counts.forwarded[0], (unsigned long)counts.forwarded[1], (unsigned long)counts.forwarded[2]);
  if (notForwarded) {
    bfAppend(out, cap, &pos, "; %lu not forwarded, mostly: %s", (unsigned long)notForwarded,
             espnowfwd::whyNotText((espnowfwd::WhyNot)topReason));
  }
  bfAppend(out, cap, &pos, ".");
  bfRequestLines(out, cap, &pos);
}

// BOND_FORWARD_HOST_END

#endif  // ENABLE_ESPNOW && ENABLE_BONDED_MODE
