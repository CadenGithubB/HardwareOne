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
static uint16_t bfSensorMask() { return (uint16_t)(gSettings.bondForwardSensors & espnowsensor::kSensorMaskAll); }
static void bfSetSettingOn(bool on) { setSetting(gSettings.bondForwardToMesh, on); }
static void bfSetSensorMask(uint16_t mask) { setSetting(gSettings.bondForwardSensors, mask); }
static bool bfMeshOn() { return meshEnabled(); }
// on/off, true/false, 1/0 (what a settings editor sends): 1, 0, or -1.
static int bfParseBool(const char* word) { return parseBoolArg(String(word)); }
static uint8_t bfRole() { return gSettings.meshRole; }
static void bfTargets(SensorReceiverTargets* out) { sensorReceiverTargets(out); }
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
  bool session;
  uint8_t mac[6];
  espnowfwd::TState state;
  uint8_t lastResult;
  uint8_t streams;
};
struct BfView {
  espnowfwd::WhyNot why;
  bool linkActive;
  uint8_t linkGen;
  uint8_t partner[6];
  BfTargetView targets[2];
};

struct BfState {
  // ---- espnow_task only ----
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
  memcpy(f.partnerMac, sBf.link.partner, 6);
  memcpy(f.partnerKey, sBf.link.partnerKey, sizeof(f.partnerKey));
  char name[40];
  bfName(sBf.link.partner, name, sizeof(name));
  espnowsensor::eventFieldPut(f.partnerName, sizeof(f.partnerName), name, sizeof(name));
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

static espnowfwd::DecideInputs bfInputs(const BfBond& b, const SensorReceiverTargets& tg, bool agreementOk) {
  espnowfwd::DecideInputs in = {};
  in.settingOn = bfSettingOn();
  in.bondMaster = b.modeOn && b.havePartner && b.isMaster && b.partnerPaired;
  in.agreementOk = agreementOk;
  in.meshOn = bfMeshOn();
  in.meshRole = bfRole();
  in.masterNamed = tg.masterNamed || tg.backupNamed;
  in.targetsPaired = (uint8_t)((tg.havePrimary ? 1 : 0) + (tg.haveBackup ? 1 : 0));
  return in;
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
  SensorReceiverTargets tg = {};
  bfTargets(&tg);
  const bool agreementOk = espnowfwd::agreementAllows(sBf.agree, b.agreeState, b.roleConflict, b.epoch);
  const espnowfwd::WhyNot why = espnowfwd::forwardDecide(bfInputs(b, tg, agreementOk));
  sBf.tickWhy = why;
  const bool want = why == espnowfwd::WhyNot::Forwards;

  // The standing link ends when forwarding stops, the partner or its key
  // changed, or a partner change ran (the hold epoch moved).
  espnowfwd::StandingLink& link = sBf.link;
  if (link.active && (!want || !espnowfwd::macSame(link.partner, b.partner) || link.epoch != b.epoch ||
                      memcmp(link.partnerKey, b.partnerKey, sizeof(link.partnerKey)) != 0)) {
    for (auto& t : sBf.targets) espnowfwd::targetBeginWithdraw(t, link.linkGen, link.partner, nowMs);
    link.active = false;
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

  const uint16_t mask = bfSensorMask();
  BfTargetView views[2] = {};
  for (uint8_t kind = 0; kind < 2; ++kind) {
    espnowfwd::TargetState& t = sBf.targets[kind];
    const bool have = kind == espnowfwd::kTargetPrimary ? tg.havePrimary : tg.haveBackup;
    const uint8_t* mac = kind == espnowfwd::kTargetPrimary ? tg.primary : tg.backup;
    if (t.have && (!have || !espnowfwd::macSame(t.mac, mac))) {
      // Another master is named now: the one that was told about the link
      // gets one withdrawal, if its session is up.
      if ((t.state != espnowfwd::TState::Idle || t.wTries) && bfSessionId(t.mac) != 0) {
        bfSendWithdraw(t.mac, t.state != espnowfwd::TState::Idle ? t.linkGen : t.wGen,
                       t.state != espnowfwd::TState::Idle ? sBf.lastPartner : t.wPartner);
      }
      memset(&t, 0, sizeof(t));
    }
    if (!have) continue;
    if (!t.have) {
      memset(&t, 0, sizeof(t));
      t.have = true;
      memcpy(t.mac, mac, 6);
    }
    const bool isPartner = b.havePartner && espnowfwd::macSame(mac, b.partner);
    views[kind].have = true;
    views[kind].isPartner = isPartner;
    memcpy(views[kind].mac, mac, 6);
    const uint16_t sid = bfSessionId(mac);
    views[kind].session = sid != 0;
    // The master named here may be the bond partner itself: it is never told
    // of a link and gets no data, but like any master this device forwards
    // nothing to, it hears the withdrawal of a link it was told of (it was the
    // master before it became the partner) and, once a session, that this
    // device forwards for nobody. No session is started with it: the bond owns
    // that session.
    espnowfwd::TargetWant w = {};
    w.want = want && link.active && !isPartner;
    w.linkGen = link.linkGen;
    w.streams = espnowfwd::streamsForTarget(kind);
    w.sensorMask = mask;
    if (sid == 0) {
      if (!isPartner && (w.want || t.wTries)) (void)bfHandshake(mac);
      views[kind].state = t.state;
      views[kind].lastResult = t.lastResult;
      views[kind].streams = t.acceptedStreams;
      continue;
    }
    const uint8_t act = espnowfwd::targetStep(t, w, sid, bfSettled(mac, nowMs), nowMs);
    if (act & espnowfwd::kActWithdraw) bfSendWithdraw(mac, t.wGen, t.wPartner);
    if (act & espnowfwd::kActWithdrawAll) bfSendWithdraw(mac, sBf.lastGen, nullptr);
    if (act & espnowfwd::kActAnnounce) bfSendAnnounce(t);
    views[kind].state = t.state;
    views[kind].lastResult = t.lastResult;
    views[kind].streams = t.acceptedStreams;
  }

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
  (void)kind;
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
  espnowsensor::ReadingHeader fh = h;
  fh.flags = (uint8_t)((h.flags & (espnowsensor::kReadingFlagAgeValid | espnowsensor::kReadingFlagStreamStart)) |
                       espnowsensor::kReadingFlagForwarded);
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

// A battery or event frame to every accepted target that carries it.
template <typename SendFn>
static void bfToTargets(uint8_t stream, uint8_t streamBit, const uint8_t* payload, uint16_t len, SendFn send) {
  bool any = false;
  bool queueFull = false;
  espnowfwd::WhyNot first = espnowfwd::WhyNot::NoMaster;
  bool haveFirst = false;
  for (uint8_t kind = 0; kind < 2; ++kind) {
    const espnowfwd::TargetState& t = sBf.targets[kind];
    if (!t.have) continue;
    const uint16_t sid = bfSessionId(t.mac);
    if (espnowfwd::targetCarries(t, sid, streamBit, -1)) {
      if (send(t.mac, payload, len)) any = true;
      else queueFull = true;
    } else if (!haveFirst) {
      first = espnowfwd::targetWhyNot(t, sid, streamBit, -1);
      haveFirst = true;
    }
  }
  if (any) bfCount(stream, espnowfwd::WhyNot::Forwards);
  else bfCount(stream, queueFull ? espnowfwd::WhyNot::QueueFull : first);
}

void bondForwardPower(const uint8_t src[6], const espnowsensor::PowerFrame& f) {
  const espnowfwd::WhyNot why = bfFrameCheck(src);
  if (why != espnowfwd::WhyNot::Forwards) {
    bfCount(kBfStreamBattery, why);
    return;
  }
  espnowsensor::PowerFrame g = f;
  g.fwdLink = sBf.link.linkGen;
  uint8_t payload[espnowsensor::kPowerFrameLen];
  espnowsensor::packPowerFrame(payload, g);
  bfToTargets(kBfStreamBattery, espnowfwd::kStreamBattery, payload, (uint16_t)sizeof(payload), bfSendPower);
}

void bondForwardEvent(const uint8_t src[6], const espnowsensor::EventFrame& f) {
  const espnowfwd::WhyNot why = bfFrameCheck(src);
  if (why != espnowfwd::WhyNot::Forwards) {
    bfCount(kBfStreamEvents, why);
    return;
  }
  espnowsensor::EventFrame g = f;
  g.flags = (uint8_t)((f.flags & espnowsensor::kEventCutMask) | espnowsensor::kEventFlagForwarded);
  g.fwdLink = sBf.link.linkGen;
  uint8_t payload[espnowsensor::kEventFrameLen];
  espnowsensor::packEventFrame(payload, g);
  bfToTargets(kBfStreamEvents, espnowfwd::kStreamEvents, payload, (uint16_t)sizeof(payload), bfSendEvent);
}

void bondForwardNotKept(uint8_t streamId) {
  if (!bfSettingOn()) return;
  bfCount(bfStreamOf(streamId), espnowfwd::WhyNot::NotKeptHere);
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
  return espnowfwd::forwardDecide(bfInputs(b, tg, agreementOk));
}

static void bfSensorWords(uint16_t mask, char* out, size_t cap) {
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
    now[k].session = bfSessionId(mac) != 0;
  }
  memcpy(view.targets, now, sizeof(now));
  const BfTargetView* accepted[2] = { nullptr, nullptr };
  const BfTargetView* other = nullptr;
  for (uint8_t k = 0; k < 2; ++k) {
    const BfTargetView& t = view.targets[k];
    if (!t.have || t.isPartner) continue;
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
    if (!t.have || t.isPartner) continue;
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
  if (!verb) return bfDescribe(reply, cap, kBfLeadCommand);
  const int onOff = strcmp(verb, "sensors") == 0 ? -1 : bfParseBool(verb);
  if (onOff >= 0) {
    if (arg) return kUsage;
    bfSetSettingOn(onOff == 1);
    return bfDescribe(reply, cap, kBfLeadCommand);
  }
  if (strcmp(verb, "sensors") != 0 || !arg || extra) return kUsage;
  uint16_t mask = 0;
  if (strcmp(arg, "all") == 0) {
    mask = (uint16_t)espnowsensor::kSensorMaskAll;
  } else if (strcmp(arg, "default") == 0) {
    mask = espnowfwd::kSensorMaskDefault;
  } else {
    char list[128];
    strlcpy(list, arg, sizeof(list));
    char* inner = nullptr;
    for (char* name = strtok_r(list, ",", &inner); name; name = strtok_r(nullptr, ",", &inner)) {
      uint8_t id = 0;
      if (!espnowsensor::sensorIdFromName(name, id)) {
        snprintf(reply, cap, "Error: '%s' is not a sensor (thermal, tof, imu, gps, input, fmradio, camera, microphone, "
                             "rtc, presence, apds)", name);
        return reply;
      }
      mask = (uint16_t)(mask | espnowsensor::sensorBit(id));
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
  const uint16_t mask = bfSensorMask();
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
    bfAppend(out, cap, &pos, "%s{\"kind\":\"%s\",\"mac\":\"" MAC_FMT "\",\"name\":\"%s\",\"isPartner\":%s,\"state\":\"%s\"",
             first ? "" : ",", k == espnowfwd::kTargetPrimary ? "primary" : "backup", MAC_ARGS(t.mac), nameJson,
             t.isPartner ? "true" : "false", espnowfwd::targetStateKey(t.state));
    if (t.lastResult) bfAppend(out, cap, &pos, ",\"result\":\"%s\"", espnowfwd::resultKey(t.lastResult));
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
  const bool whole = bfAppend(out, cap, &pos, "},\"answersIgnored\":%lu}", (unsigned long)counts.answersIgnored);
  if (!whole) {
    out[0] = '\0';
    return 0;
  }
  return pos;
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
}

// BOND_FORWARD_HOST_END

#endif  // ENABLE_ESPNOW && ENABLE_BONDED_MODE
