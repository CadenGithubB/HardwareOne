// System_ESPNow_PowerEvents.cpp - battery state and system events over the mesh
//
// See System_ESPNow_PowerEvents.h for what this is and
// System_ESPNow_PowerEventCore.h for the rules. Two tasks meet here:
//   espnow_task  sends this device's battery state and events to the masters
//                named on it (a bond worker: to its bond master only, as
//                180/182), and receives the frames of other devices: from a
//                mesh peer (152/132, perhaps forwarded by a bond master for
//                its partner: System_ESPNow_ForwardCore.h), or from this
//                device's bond partner (180/182);
//   cmd_exec     espnowbattery, espnowevents, espnowsensorstatus.
// What they share is under sPeMux; the rest belongs to espnow_task.
#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include "System_ESPNow_PowerEvents.h"

#include <Arduino.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "System_AuthIdentity.h"       // currentExecIsAdmin: event counts are for an admin
#include "System_Battery.h"
#include "System_Clock.h"
#include "System_CommandReply.h"      // kCommandReplyOutOfMemory
#include "System_Debug.h"
#include "System_ESPNow.h"
#include "System_ESPNow_BondForward.h"      // the bond master forwards its partner's frames
#include "System_ESPNow_ForwardRx.h"        // whose is a received frame
#include "System_ESPNow_Handlers_Crypto.h"  // espnowHandshakeAutoStart
#include "System_ESPNow_HandshakeBackoff.h" // WarnLimiter
#include "System_ESPNow_Identity.h"
#include "System_ESPNow_RxCtx.h"
#include "System_ESPNow_Sensors.h"    // sensorReceiverTargets, espnowSensorBootTag
#include "System_ESPNow_Sessions.h"   // sendStatusRegister
#include "System_ESPNow_Tx.h"
#include "System_ESPNow_Wire.h"
#include "System_Events.h"
#include "System_MacText.h"        // MAC_FMT, macToDisplay
#include "System_MemUtil.h"
#include "System_Notifications.h"     // notifKindTier
#include "System_Settings.h"
#include "System_Utils.h"             // RETURN_VALID_IF_VALIDATE_CSTR, macToDisplayStr

static_assert((int)BATTERY_UNKNOWN == 0 && (int)BATTERY_CHARGING == 1 && (int)BATTERY_FULL == 2 &&
              (int)BATTERY_DISCHARGING == 3 && (int)BATTERY_LOW == 4 && (int)BATTERY_CRITICAL == 5 &&
              (int)BATTERY_NOT_PRESENT == 6 && (int)BATTERY_HIGH == 7 && (int)BATTERY_GOOD == 8 &&
              (int)BATTERY_MEDIUM == 9 && (int)BATTERY_EMPTY == 10 && espnowsensor::kPowerStatusCount == 11,
              "POWER_STATUS carries BatteryStatus by number: these are on the wire");
static_assert((int)NTIER_VERBOSE == espnowsensor::kEventTierVerbose && (int)NTIER_STANDARD == espnowsensor::kEventTierStandard &&
              (int)NTIER_ALERT == espnowsensor::kEventTierAlert, "EVENT_PUSH carries the notification tier by number");
static_assert((int)NOTIF_SOURCE_UNKNOWN == espnowsensor::kEventSourceUnknown && (int)NOTIF_SOURCE_CLI == espnowsensor::kEventSourceCli &&
              (int)NOTIF_SOURCE_OLED == espnowsensor::kEventSourceOled && (int)NOTIF_SOURCE_WEB == espnowsensor::kEventSourceWeb &&
              (int)NOTIF_SOURCE_VOICE == espnowsensor::kEventSourceVoice && (int)NOTIF_SOURCE_REMOTE == espnowsensor::kEventSourceRemote &&
              (int)NOTIF_SOURCE_SYSTEM == espnowsensor::kEventSourceSystem && (int)NOTIF_SOURCE_G2 == espnowsensor::kEventSourceG2,
              "EVENT_PUSH carries NotificationSource by number");
static_assert(espnowsensor::kEventMeshes == Settings::N_MESHES, "one event list per mesh slot");
static_assert(espnowsensor::kEventKindLen >= SYSTEM_EVENT_KIND_TOKEN_CAP - 1,
              "every kind name of this firmware fits the frame's kind field");

// ---- What the rules below ask of the device ---------------------------------
// Everything from POWER_EVENTS_HOST_BEGIN to POWER_EVENTS_HOST_END reaches the
// battery, the event ring, the clock, the settings, the peer tables and the
// radio only through these, so the host suite espnow_power_events runs that
// part as it is with its own versions of them.

static portMUX_TYPE sPeMux = portMUX_INITIALIZER_UNLOCKED;
#define PE_LOCK()   portENTER_CRITICAL(&sPeMux)
#define PE_UNLOCK() portEXIT_CRITICAL(&sPeMux)

static constexpr uint8_t kPePeerSlots = MESH_PEER_MAX;

static uint32_t peUptimeMs() { return (uint32_t)millis(); }
static bool peClockSet() { return Clock::isSynced(); }
static int64_t peEpochMs() { return Clock::epochMillis(); }
static uint8_t peClockSource() { return (uint8_t)Clock::syncSource(); }
// How the clock that stamped an arrival was set: "none" (not set), "carried"
// (set, but by no source since boot), or the source's name.
static const char* peClockKey(int64_t arrivalEpochMs, uint8_t source) {
  if (arrivalEpochMs == 0) return "none";
  if (source == (uint8_t)Clock::SYNC_NONE) return "carried";
  return Clock::syncSourceName((Clock::SyncSource)source);
}
// Stored times are UTC; the timezone is applied here, where a time is shown.
static void peFormatLocal(int64_t epochMs, char* out, size_t cap) {
  char iso[24];
  char tz[16];
  Clock::formatIso8601Local((time_t)(epochMs / 1000), iso, sizeof(iso));
  snprintf(out, cap, "%s %s", iso, Clock::formatTzOffsetLabel(tz, sizeof(tz)));
}
static uint8_t peRole() { return gSettings.meshRole; }
static bool peMeshOn() { return meshEnabled(); }
static uint32_t pePowerIntervalS() { return gSettings.powerStatusInterval; }
static uint8_t peEventFloor() { return gSettings.eventPush; }
static uint32_t peBootCounter() { return gBootCounter; }
static uint16_t peBootTag() { return espnowSensorBootTag(); }
// True when this firmware reads a battery at all.
static bool peBatteryInBuild() {
  return ENABLE_BATTERY_MONITOR && (BATTERY_BACKEND_ADC || BATTERY_BACKEND_FUEL_GAUGE);
}
static void peBatteryInputs(espnowsensor::PowerInputs* in, uint32_t nowMs) {
  const BatteryState state = getBatterySnapshot();
  in->status = (uint8_t)state.status;
#if ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_FUEL_GAUGE
  in->backend = espnowsensor::kPowerBackendFuelGauge;
#elif ENABLE_BATTERY_MONITOR && BATTERY_BACKEND_ADC
  in->backend = espnowsensor::kPowerBackendAdc;
#else
  in->backend = espnowsensor::kPowerBackendNone;
#endif
  in->hasSample = state.hasSample;
  in->sampleAgeMs = state.hasSample ? espnowsensor::elapsedSinceMs(nowMs, state.lastReadMs) : 0;
  in->voltageValid = state.voltageValid;
  in->voltage = state.voltage;
  in->percentageValid = state.percentageValid;
  in->percentageEstimated = state.percentageEstimated;
  in->percentage = state.percentage;
  in->presenceKnown = state.presenceKnown;
  in->present = state.present;
  in->chargingKnown = state.chargingKnown;
  in->charging = state.isCharging;
  in->chargingEstimated = state.chargingEstimated;
  in->usbKnown = state.usbKnown;
  in->usbPresent = state.usbPresent;
  in->rateValid = state.rateValid;
  in->ratePctPerHr = state.cratePctPerHr;
  in->statusEstimated = state.statusEstimated;
  in->stale = state.stale;
}
// The masters named on this device that are paired (kept by sensorLinkTick).
static void peTargets(SensorReceiverTargets* out) { sensorReceiverTargets(out); }
// The bond: a bond worker sends its battery state and events to its bond
// master only (espnowfwd::meshDataAllowed). Only the single-bond settings are
// read.
static bool peBondWorker() {
#if ENABLE_BONDED_MODE
  return gSettings.bondModeEnabled && isBondWorker();
#else
  return false;
#endif
}
static bool peBondPartner(uint8_t mac[6]) {
#if ENABLE_BONDED_MODE
  return isBondModeOnline() && parseMacAddress(gSettings.bondPeerMac, mac);
#else
  (void)mac;
  return false;
#endif
}
// The receiving bond master of 180/182.
static bool peIsBondMaster() {
#if ENABLE_BONDED_MODE
  return gSettings.bondModeEnabled && isBondMaster();
#else
  return false;
#endif
}
static bool peSessionUp(const uint8_t mac[6]) { return espnowSessionUp(mac); }
static bool peSessionSettled(const uint8_t mac[6], uint32_t nowMs) { return espnowSessionSettled(mac, nowMs); }
static bool peHandshake(const uint8_t mac[6]) { return espnowHandshakeAutoStart(mac); }
// POWER_STATUS asks for an acknowledgement and has it watched: two in a row
// that nobody acknowledges clear a session the master no longer has
// (sendStatusSweep), and the next look starts a new one. espnow_task only
// (sendStatusRegister has no lock).
static bool peSendPower(const uint8_t mac[6], const uint8_t* payload, uint16_t len) {
  const uint32_t msgId = generateMessageId();
  if (!espnowtx::sendAead(mac, ESPNOW_V4_TYPE_POWER_STATUS, ESPNOW_V4_FLAG_ACK_REQ, msgId, payload, len, 1)) {
    return false;
  }
  sendStatusRegister(msgId, mac);
  return true;
}
static bool peSendEvent(const uint8_t mac[6], const uint8_t* payload, uint16_t len) {
  return espnowtx::sendAead(mac, ESPNOW_V4_TYPE_EVENT_PUSH, 0, generateMessageId(), payload, len, 1);
}
// To the bond partner: BOND_POWER_STATUS / BOND_EVENT_PUSH over the bond
// session (single initiator; the bond heartbeat owns that session).
static bool peSendBondPower(const uint8_t mac[6], const uint8_t* payload, uint16_t len) {
  return v4_send_bond_power(mac, payload, len);
}
static bool peSendBondEvent(const uint8_t mac[6], const uint8_t* payload, uint16_t len) {
  return v4_send_bond_event(mac, payload, len);
}
// Whose is a received frame (System_ESPNow_ForwardRx.cpp).
static ForwardResolve peWhose(const uint8_t src[6], const char* name, bool bondLink, bool hasBit, bool bitSet,
                                uint8_t fwdLink, uint8_t streamId, ForwardOrigin* out) {
  return espnowForwardResolve(src, name, bondLink, hasBit, bitSet, fwdLink, streamId, out);
}
static void peResumeTaken(const uint8_t via[6], uint8_t streamId) { espnowForwardResumeTaken(via, streamId); }
static bool peLinkActive(const uint8_t via[6], const uint8_t origin[6]) { return espnowForwardLinkActive(via, origin); }
// The bond master hands what it kept of its partner to its forwarder.
// The payload as received, at its full length: it goes on verbatim (1.0 rule W2).
static void peForwardPower(const uint8_t src[6], const uint8_t* payload, size_t len) {
  bondForwardPower(src, payload, len);
}
static void peForwardEvent(const uint8_t src[6], const uint8_t* payload, size_t len) {
  bondForwardEvent(src, payload, len);
}
static void peNotKept(uint8_t streamId) { bondForwardNotKept(streamId); }
// The name of the device that forwarded a record.
static void peViaName(const uint8_t mac[6], char* out, size_t cap) {
  strlcpy(out, getEspNowDeviceName(mac).c_str(), cap);
  if (!out[0]) macToDisplay(mac, out, cap);
}
// One event after *cursor (oldest first); *skipped gains what the ring
// overwrote before it could be read.
static int peEventFetch(uint32_t* cursor, SystemEvent* out, uint32_t* skipped) {
  return systemEventFetchSince(cursor, out, 1, skipped);
}
static uint32_t peEventLatestSeq() { return systemEventLatestSeq(); }
// The number of the oldest event the ring still holds (latest + 1 when it
// holds none): the ring keeps the last SYSEVT_RING_SIZE posts.
static uint32_t peEventOldestSeq() {
  return espnowsensor::eventOldestHeld(systemEventLatestSeq(), SYSEVT_RING_SIZE);
}
static uint8_t peKindTier(uint8_t kind) { return notifKindTier(kind); }
static const char* peKindName(uint8_t kind) { return systemEventKindName(kind); }
static bool peIsPaired(const uint8_t mac[6]) { return peerIdentityFindByMac(mac) != nullptr; }
// The mesh slot a paired sender belongs to.
static uint8_t peMeshOf(const uint8_t mac[6]) {
  const PeerIdentity* pid = peerIdentityFindByMac(mac);
  return pid ? pid->meshId : 0;
}
static void* peAlloc(size_t bytes, const char* tag) { return ps_alloc(bytes, AllocPref::PreferPsram, tag); }
static bool peResolve(const char* who, uint8_t mac[6]) {
  if (resolveDeviceNameOrMac(String(who), mac)) return true;
  // A bond partner another device forwards for, by the name its link shows.
  uint8_t via[6];
  return espnowForwardFindOrigin(who, nullptr, mac, via);
}
static void peConsole(const char* fmt, ...) {
  char line[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  broadcastOutput(line);
}
// The user the command runs for is an admin (the events other devices sent
// are theirs to read: espnowevents is admin-only, and so are their counts).
static bool peCallerIsAdmin() { return currentExecIsAdmin(); }
static char* peReply(size_t* cap) {
  if (!ensureDebugBuffer()) return nullptr;
  *cap = GLOBAL_DEBUG_BUFFER_SIZE;
  return getDebugBuffer();
}

// POWER_EVENTS_HOST_BEGIN

// What a receiver keeps of one sender: its latest battery state with this
// device's own arrival stamp, and the counter state and counts of its battery
// frames and its events. A sender is its origin and, for forwarded frames, the
// bond master that forwarded them (origin, via): what a bond master says about
// its partner is never filed under the partner's own record. A record is given
// up only by espnowfwd::recordReusable.
struct PeCollectPeer {
  bool inUse;
  uint8_t mac[6];            // the origin
  uint8_t via[6];            // all zero, or the bond master that forwards for it
  uint8_t link;              // espnowfwd::kLink*
  char name[32];
  bool havePower;
  espnowsensor::PowerFrame power;
  int64_t powerEpochMs;      // this device's UTC clock when it arrived; 0: not set
  uint32_t powerUptimeMs;
  uint8_t powerClock;
  espnowsensor::CounterTrack powerTrack;
  espnowsensor::CounterCounts powerCounts;
  espnowsensor::CounterTrack eventTrack;
  espnowsensor::CounterCounts eventCounts;
  uint32_t senderSkipped;    // events the sender says it left out
  uint32_t refusedRate;      // events this device did not store: over its rate limit for one sender
  uint32_t eventsStored;
  espnowsensor::TokenBucket eventBucket;
};

// Frames the receiver refused, whoever sent them.
struct PeRefusals {
  uint32_t notReceiver;  // this device's role does not keep them
  uint32_t noRecord;     // sixteen paired senders already send
  uint32_t noMemory;
  uint32_t malformed;    // too short to be the frame, or a forwarded mark that makes no sense
  uint32_t notBondMaster;  // 180/182 on a device that is not a bond master
  uint32_t forwardRefused; // forwarded, but no active link of the sender names it
};

struct PeState {
  // ---- What was received. Under sPeMux. ----
  // kPePeerSlots records (PSRAM), made by the first frame this device keeps:
  // only a device that is sent battery state or events has them.
  PeCollectPeer* peers;
  // The recent events, one list of kEventListLen per mesh slot (PSRAM), each
  // made by the first event from that mesh.
  espnowsensor::StoredEvent* lists[espnowsensor::kEventMeshes];
  uint32_t rxSerial;         // the receive number of the newest stored event
  PeRefusals refused;
  // ---- What was sent, for the status. Under sPeMux. ----
  uint32_t powerSent;
  bool powerEverSent;
  uint32_t powerLastSentMs;
  uint32_t eventsSent;
  uint32_t eventsLeftOut;
  // ---- espnow_task only. ----
  uint32_t lastLookMs;
  espnowsensor::PowerSendState powerSend;
  uint16_t powerCounter;
  uint32_t eventCursor;      // the number of the last event that was dealt with (0: start at the oldest held)
  uint16_t eventCounter;
  uint32_t eventSkippedPending;  // left out since the last event frame that went out
  espnowsensor::TokenBucket eventBucket;
  SystemEvent event;         // the one being sent
  WarnLimiter roleWarn;
  WarnLimiter fullWarn;
};

EXT_RAM_BSS_ATTR static PeState sPe;  // PSRAM: tasks only, never the radio callback

// ---- Who this device sends to, and whether ------------------------------------

struct PeSendPlan {
  espnowsensor::PushWhyNot batteryWhy;
  espnowsensor::PushWhyNot eventsWhy;
  uint8_t targetCount;       // named masters that are paired (a bond worker: its partner)
  uint8_t targetMac[2][6];   // the primary first
  bool targetReady[2];       // a session a frame can use now
  bool viaBond;              // the one target is the bond partner (180/182)
};

static void pePlan(uint32_t nowMs, PeSendPlan* plan) {
  SensorReceiverTargets targets = {};
  peTargets(&targets);
  memset(plan, 0, sizeof(*plan));
  espnowsensor::PushInputs in = {};
  in.bondWorker = !espnowfwd::meshDataAllowed(peBondWorker());
  if (in.bondWorker) {
    // A bond worker: its bond partner, and no mesh master whatever is named.
    uint8_t partner[6];
    in.bondOnline = peBondPartner(partner);
    if (in.bondOnline) {
      plan->viaBond = true;
      memcpy(plan->targetMac[plan->targetCount++], partner, 6);
    }
  } else {
    if (targets.havePrimary) memcpy(plan->targetMac[plan->targetCount++], targets.primary, 6);
    if (targets.haveBackup) memcpy(plan->targetMac[plan->targetCount++], targets.backup, 6);
  }
  in.meshOn = peMeshOn();
  in.meshRole = peRole();
  in.masterNamed = targets.masterNamed || targets.backupNamed;
  in.targets = plan->targetCount;
  for (uint8_t i = 0; i < plan->targetCount; ++i) {
    plan->targetReady[i] = peSessionSettled(plan->targetMac[i], nowMs);
    if (plan->targetReady[i]) in.targetsReady++;
  }
  in.inBuild = peBatteryInBuild();
  in.settingOn = espnowsensor::powerIntervalMs(pePowerIntervalS()) != 0;
  plan->batteryWhy = espnowsensor::pushDecide(in);
  in.inBuild = true;
  in.settingOn = espnowsensor::eventPassesFloor(peEventFloor(), espnowsensor::kEventTierAlert);
  plan->eventsWhy = espnowsensor::pushDecide(in);
}

// ---- Send (espnow_task) ---------------------------------------------------------

// The battery state: one frame per sample that is due, the same to every
// named master with a session. A master without one sees the gap as loss,
// which is true for it.
static void pePowerStep(const PeSendPlan& plan, uint32_t nowMs) {
  if (plan.batteryWhy != espnowsensor::PushWhyNot::Sends) return;
  espnowsensor::PowerInputs in = {};
  peBatteryInputs(&in, nowMs);
  espnowsensor::PowerFrame frame = {};
  espnowsensor::powerFrameFill(frame, in);
  if (!espnowsensor::powerSendDue(sPe.powerSend, frame, pePowerIntervalS(), nowMs)) return;
  frame.bootTag = peBootTag();
  frame.counter = (uint16_t)(sPe.powerCounter + 1);
  uint8_t payload[espnowsensor::kPowerFrameLen];
  espnowsensor::packPowerFrame(payload, frame);
  bool any = false;
  for (uint8_t i = 0; i < plan.targetCount; ++i) {
    if (!plan.targetReady[i]) continue;
    const bool sent = plan.viaBond ? peSendBondPower(plan.targetMac[i], payload, (uint16_t)sizeof(payload))
                                   : peSendPower(plan.targetMac[i], payload, (uint16_t)sizeof(payload));
    if (sent) any = true;
  }
  if (!any) return;  // the send queue took none: the next look tries again, with the same number
  sPe.powerCounter = frame.counter;
  espnowsensor::powerSent(sPe.powerSend, frame, nowMs);
  PE_LOCK();
  sPe.powerSent++;
  sPe.powerEverSent = true;
  sPe.powerLastSentMs = nowMs;
  PE_UNLOCK();
}

// The events: this device reads its own event ring with a cursor of its own,
// at most four events sent or left out a look. An event under the floor is
// passed over and costs nothing of those four (the whole ring can be passed
// over in one look), so an alert behind a run of lesser events is not held
// back. One the rate limit has no room for, and one the ring overwrote before
// it could be read (whatever its tier: it can no longer be read), is counted
// into `skipped` of the next frame that goes out.
static constexpr int kPeEventsPerLook = 4;
static constexpr int kPeEventsExaminedPerLook = 64;  // more than the ring holds
static void peEventStep(const PeSendPlan& plan, uint32_t nowMs) {
  // No named master has a session yet, or the bond partner is not connected:
  // the cursor waits, and the events go later with their age.
  if (plan.eventsWhy == espnowsensor::PushWhyNot::NoSession ||
      plan.eventsWhy == espnowsensor::PushWhyNot::BondOffline) {
    return;
  }
  // Not sending at all (switched off, no master named, the master's role):
  // the cursor follows the head, so nothing old is sent once that changes.
  if (plan.eventsWhy != espnowsensor::PushWhyNot::Sends) {
    sPe.eventCursor = peEventLatestSeq();
    sPe.eventSkippedPending = 0;
    return;
  }
  const uint8_t floor = peEventFloor();
  // Events left out count into `skipped` of the next frame and into the status.
  auto leftOut = [](uint32_t count) {
    if (count == 0) return;
    sPe.eventSkippedPending += count;
    PE_LOCK();
    sPe.eventsLeftOut += count;
    PE_UNLOCK();
  };
  int budget = kPeEventsPerLook;
  for (int examined = 0; examined < kPeEventsExaminedPerLook && budget > 0; ++examined) {
    // What the ring overwrote while this reader waited is accounted here and
    // stepped over at once, so it is counted exactly once (and the ring's own
    // count of silently lost events is not charged for a wait this reader
    // reports itself).
    const uint32_t oldest = peEventOldestSeq();
    if (sPe.eventCursor + 1 < oldest) {
      leftOut(oldest - (sPe.eventCursor + 1));
      sPe.eventCursor = oldest - 1;
    }
    uint32_t cursor = sPe.eventCursor;
    uint32_t overwritten = 0;
    if (peEventFetch(&cursor, &sPe.event, &overwritten) <= 0) break;
    const SystemEvent& ev = sPe.event;
    // (the few the ring overwrote between the look above and the fetch)
    if (overwritten) {
      leftOut(overwritten);
      sPe.eventCursor = ev.seq - 1;
    }
    const uint8_t tier = peKindTier(ev.kind);
    if (!espnowsensor::eventPassesFloor(floor, tier)) {
      sPe.eventCursor = cursor;
      continue;
    }
    budget--;
    if (!espnowsensor::bucketHas(sPe.eventBucket, espnowsensor::kEventTxBurst, espnowsensor::kEventTxRefillMs, nowMs)) {
      sPe.eventCursor = cursor;
      leftOut(1);
      continue;
    }
    espnowsensor::EventFrame frame = {};
    frame.counter = (uint16_t)(sPe.eventCounter + 1);
    frame.bootTag = peBootTag();
    frame.tier = tier;
    frame.source = ev.source < espnowsensor::kEventSourceCount ? ev.source : espnowsensor::kEventSourceUnknown;
    frame.skipped = espnowsensor::eventSkippedField(sPe.eventSkippedPending);
    frame.eventSeq = ev.seq;
    // (an event posted after this pass read the uptime is 0 ms old, not 49 days)
    frame.ageMs = espnowsensor::elapsedSinceMs(nowMs, ev.tsMs);
    // The kind travels as its name: its number may differ between firmware versions.
    const char* kind = peKindName(ev.kind);
    if (espnowsensor::eventFieldPut(frame.kind, sizeof(frame.kind), kind, kind ? strlen(kind) : 0)) {
      frame.flags |= espnowsensor::kEventCutKind;
    }
    if (espnowsensor::eventFieldPut(frame.subject, sizeof(frame.subject), ev.subject, sizeof(ev.subject))) {
      frame.flags |= espnowsensor::kEventCutSubject;
    }
    if (espnowsensor::eventFieldPut(frame.detail, sizeof(frame.detail), ev.detail, sizeof(ev.detail))) {
      frame.flags |= espnowsensor::kEventCutDetail;
    }
    if (espnowsensor::eventFieldPut(frame.who, sizeof(frame.who), ev.who, sizeof(ev.who))) {
      frame.flags |= espnowsensor::kEventCutWho;
    }
    uint8_t payload[espnowsensor::kEventFrameLen];
    espnowsensor::packEventFrame(payload, frame);
    bool any = false;
    for (uint8_t i = 0; i < plan.targetCount; ++i) {
      if (!plan.targetReady[i]) continue;
      const bool sent = plan.viaBond ? peSendBondEvent(plan.targetMac[i], payload, (uint16_t)sizeof(payload))
                                     : peSendEvent(plan.targetMac[i], payload, (uint16_t)sizeof(payload));
      if (sent) any = true;
    }
    if (!any) break;  // the send queue took none: this event waits for the next look
    (void)espnowsensor::bucketTake(sPe.eventBucket, espnowsensor::kEventTxBurst, espnowsensor::kEventTxRefillMs, nowMs);
    sPe.eventCounter = frame.counter;
    sPe.eventCursor = cursor;
    sPe.eventSkippedPending = 0;
    PE_LOCK();
    sPe.eventsSent++;
    PE_UNLOCK();
  }
}

void espnowPowerEventsTick(uint32_t nowMs) {
  if ((uint32_t)(nowMs - sPe.lastLookMs) < 1000) return;
  sPe.lastLookMs = nowMs;
  PeSendPlan plan;
  pePlan(nowMs, &plan);
  // A named master without a session gets a handshake start (one a look,
  // backed off like every automatic start): this is what brings a worker back
  // whose master restarted where the heartbeat does not reach. Never the bond
  // partner: the bond heartbeat owns that session, and only the higher
  // address of a bond starts one (bondSendEncryptedAsync).
  const bool wants = plan.batteryWhy == espnowsensor::PushWhyNot::Sends || plan.batteryWhy == espnowsensor::PushWhyNot::NoSession ||
                     plan.eventsWhy == espnowsensor::PushWhyNot::Sends || plan.eventsWhy == espnowsensor::PushWhyNot::NoSession;
  if (wants && !plan.viaBond) {
    for (uint8_t i = 0; i < plan.targetCount; ++i) {
      if (plan.targetReady[i] || peSessionUp(plan.targetMac[i])) continue;
      if (peHandshake(plan.targetMac[i])) break;
    }
  }
  pePowerStep(plan, nowMs);
  peEventStep(plan, nowMs);
}

// ---- Receive (espnow_task) --------------------------------------------------------

static void peRefuse(uint32_t PeRefusals::*counter) {
  PE_LOCK();
  (sPe.refused.*counter)++;
  PE_UNLOCK();
}

static const uint8_t kPeNoVia[6] = { 0, 0, 0, 0, 0, 0 };

static bool peSameSender(const PeCollectPeer& peer, const uint8_t mac[6], const uint8_t* via) {
  return memcmp(peer.mac, mac, 6) == 0 && memcmp(peer.via, via ? via : kPeNoVia, 6) == 0;
}

// The record of a sender (origin, via), made when there is none: a free one,
// else one that may be given up (espnowfwd::recordReusable). nullptr when
// none may. Without the lock (the pairing lookup must not run under it).
static PeCollectPeer* pePeerFor(const ForwardOrigin& origin) {
  const uint8_t* via = origin.link == espnowfwd::kLinkForwarded ? origin.via : kPeNoVia;
  PeCollectPeer* freeRecord = nullptr;
  for (uint8_t i = 0; i < kPePeerSlots; ++i) {
    if (sPe.peers[i].inUse && peSameSender(sPe.peers[i], origin.mac, via)) {
      // The same origin may come over the mesh and over the bond link.
      if (sPe.peers[i].link != origin.link) {
        PE_LOCK();
        sPe.peers[i].link = origin.link;
        PE_UNLOCK();
      }
      return &sPe.peers[i];
    }
    if (!sPe.peers[i].inUse && !freeRecord) freeRecord = &sPe.peers[i];
  }
  if (!freeRecord) {
    for (uint8_t i = 0; i < kPePeerSlots; ++i) {
      const PeCollectPeer& r = sPe.peers[i];
      const bool forwarded = r.link == espnowfwd::kLinkForwarded;
      if (espnowfwd::recordReusable(r.link, forwarded ? false : peIsPaired(r.mac), forwarded ? peIsPaired(r.via) : false,
                                    forwarded ? peLinkActive(r.via, r.mac) : false)) {
        freeRecord = &sPe.peers[i];
        break;
      }
    }
  }
  if (!freeRecord) return nullptr;
  PE_LOCK();
  memset(freeRecord, 0, sizeof(*freeRecord));
  memcpy(freeRecord->mac, origin.mac, 6);
  memcpy(freeRecord->via, via, 6);
  freeRecord->link = origin.link;
  freeRecord->inUse = true;
  PE_UNLOCK();
  return freeRecord;
}

// The gate of both frames after the receive table's: this device's role must
// keep them (a master or backup master; for a frame over the bond link, this
// device must be the bond master, whatever its mesh role), and it must have a
// record for the sender. A refusal is counted, and said once a minute.
// nullptr: refused.
static PeCollectPeer* peAccept(const ForwardOrigin& origin, const char* senderName, uint32_t nowMs) {
  const char* name = senderName ? senderName : "?";
  if (origin.link == espnowfwd::kLinkBond) {
    if (!peIsBondMaster()) {
      peRefuse(&PeRefusals::notBondMaster);
      return nullptr;
    }
  } else if (!espnowsensor::roleReceives(peRole())) {
    peRefuse(&PeRefusals::notReceiver);
    uint32_t held = 0;
    if (warnLimiterTake(sPe.roleWarn, nowMs, 60000, &held)) {
      peConsole("[ESP-NOW] Battery state and events from %s refused: this device is not a mesh master or backup"
                " master (%lu more in the last minute)", name, (unsigned long)held);
    }
    return nullptr;
  }
  if (!sPe.peers) {
    const size_t bytes = (size_t)kPePeerSlots * sizeof(PeCollectPeer);
    PeCollectPeer* peers = (PeCollectPeer*)peAlloc(bytes, "espnow.collectPeers");
    if (peers) {
      memset(peers, 0, bytes);
      PE_LOCK();
      sPe.peers = peers;
      PE_UNLOCK();
    }
  }
  PeCollectPeer* peer = sPe.peers ? pePeerFor(origin) : nullptr;
  if (peer) return peer;
  peRefuse(sPe.peers ? &PeRefusals::noRecord : &PeRefusals::noMemory);
  uint32_t held = 0;
  if (warnLimiterTake(sPe.fullWarn, nowMs, 60000, &held)) {
    if (sPe.peers) {
      peConsole("[ESP-NOW] Battery state and events from %s refused: this device already keeps those of %u paired"
                " devices (%lu more in the last minute)", name, (unsigned)kPePeerSlots, (unsigned long)held);
    } else {
      peConsole("[ESP-NOW] Battery state and events from %s refused: this device is out of memory"
                " (%lu more in the last minute)", name, (unsigned long)held);
    }
  }
  return nullptr;
}

// Whose a frame is (its origin, and the bond master that forwarded it). False:
// refused and counted.
static bool peOriginOf(const uint8_t* sender, const char* senderName, bool bondLink, bool hasBit, bool bitSet,
                       uint8_t fwdLink, uint8_t streamId, ForwardOrigin* origin) {
  switch (peWhose(sender, senderName, bondLink, hasBit, bitSet, fwdLink, streamId, origin)) {
    case ForwardResolve::Own:
    case ForwardResolve::Forwarded:
      return true;
    case ForwardResolve::Refused:
      peRefuse(&PeRefusals::forwardRefused);
      return false;
    case ForwardResolve::Malformed:
      break;
  }
  peRefuse(&PeRefusals::malformed);
  return false;
}

// POWER_STATUS from a paired sender over its session (`bondLink`: BOND_POWER_STATUS
// from this device's bond partner). The receiver stamps it with its own
// clock; an older frame never replaces a newer one. A bond master hands what
// it kept of its partner to its forwarder.
static void powerEventsIngestPower(const uint8_t* sender, const char* senderName, const uint8_t* payload, size_t len,
                                   bool bondLink) {
  const uint32_t nowMs = peUptimeMs();
  espnowsensor::PowerFrame frame = {};
  if (!sender || !espnowsensor::parsePowerFrame(payload, len, frame)) {
    peRefuse(&PeRefusals::malformed);
    return;
  }
  ForwardOrigin origin;
  if (!peOriginOf(sender, senderName, bondLink, false, frame.fwdLink != 0, frame.fwdLink, espnowfwd::kStreamIdBattery,
                  &origin)) {
    return;
  }
  PeCollectPeer* peer = peAccept(origin, origin.name, nowMs);
  if (!peer) {
    if (bondLink) peNotKept(espnowfwd::kStreamIdBattery);
    return;
  }
  const bool clockSet = peClockSet();
  const int64_t epochMs = clockSet ? peEpochMs() : 0;
  const uint8_t clock = peClockSource();
  PE_LOCK();
  strlcpy(peer->name, origin.name, sizeof(peer->name));
  const espnowsensor::CounterVerdict verdict = espnowsensor::counterJudge(
      peer->powerTrack, peer->powerCounts, frame.bootTag, frame.counter, origin.resumePending, nowMs);
  const bool kept = espnowsensor::counterVerdictKeeps(verdict);
  if (kept) {
    peer->power = frame;
    peer->power.fwdLink = 0;
    peer->havePower = true;
    peer->powerEpochMs = epochMs;
    peer->powerUptimeMs = nowMs;
    peer->powerClock = clock;
  }
  PE_UNLOCK();
  if (origin.resumePending) peResumeTaken(origin.via, espnowfwd::kStreamIdBattery);
  if (bondLink && kept) peForwardPower(sender, payload, len);
}

// EVENT_PUSH from a paired sender over its session (`bondLink`:
// BOND_EVENT_PUSH from this device's bond partner). Nothing in the frame is
// trusted to be text: every field is copied with its length and cleaned
// before it is stored. The event goes into the list of the sender's mesh (a
// forwarded one: of the device that forwarded it) and nowhere else: it is
// never posted into this device's own event ring.
static void powerEventsIngestEvent(const uint8_t* sender, const char* senderName, const uint8_t* payload, size_t len,
                                   bool bondLink) {
  const uint32_t nowMs = peUptimeMs();
  espnowsensor::EventFrame frame = {};
  if (!sender || !espnowsensor::parseEventFrame(payload, len, frame)) {
    peRefuse(&PeRefusals::malformed);
    return;
  }
  ForwardOrigin origin;
  if (!peOriginOf(sender, senderName, bondLink, true, (frame.flags & espnowsensor::kEventFlagForwarded) != 0,
                  frame.fwdLink, espnowfwd::kStreamIdEvents, &origin)) {
    return;
  }
  PeCollectPeer* peer = peAccept(origin, origin.name, nowMs);
  if (!peer) {
    if (bondLink) peNotKept(espnowfwd::kStreamIdEvents);
    return;
  }
  uint8_t mesh = peMeshOf(sender);  // a forwarded event: the mesh of the device that forwarded it
  if (mesh >= espnowsensor::kEventMeshes) mesh = 0;
  if (!sPe.lists[mesh]) {
    const size_t bytes = (size_t)espnowsensor::kEventListLen * sizeof(espnowsensor::StoredEvent);
    espnowsensor::StoredEvent* list = (espnowsensor::StoredEvent*)peAlloc(bytes, "espnow.events");
    if (!list) {
      peRefuse(&PeRefusals::noMemory);
      if (bondLink) peNotKept(espnowfwd::kStreamIdEvents);
      return;
    }
    memset(list, 0, bytes);
    PE_LOCK();
    sPe.lists[mesh] = list;
    PE_UNLOCK();
  }
  espnowsensor::StoredEvent stored = {};
  memcpy(stored.senderMac, origin.mac, 6);
  if (origin.link == espnowfwd::kLinkForwarded) memcpy(stored.via, origin.via, 6);
  stored.link = origin.link;
  stored.tier = frame.tier;
  stored.source = frame.source;
  stored.flags = (uint8_t)(frame.flags & espnowsensor::kEventCutMask);
  stored.mesh = mesh;
  stored.counter = frame.counter;
  stored.bootTag = frame.bootTag;
  stored.eventSeq = frame.eventSeq;
  stored.ageMs = frame.ageMs;
  stored.arrivalUptimeMs = nowMs;
  stored.arrivalEpochMs = peClockSet() ? peEpochMs() : 0;
  stored.arrivalClock = peClockSource();
  espnowsensor::wireTextCopy(frame.kind, sizeof(frame.kind), stored.kind, sizeof(stored.kind));
  espnowsensor::wireTextCopy(frame.subject, sizeof(frame.subject), stored.subject, sizeof(stored.subject));
  espnowsensor::wireTextCopy(frame.detail, sizeof(frame.detail), stored.detail, sizeof(stored.detail));
  espnowsensor::wireTextCopy(frame.who, sizeof(frame.who), stored.who, sizeof(stored.who));
  bool store = false;
  PE_LOCK();
  strlcpy(peer->name, origin.name, sizeof(peer->name));
  const espnowsensor::CounterVerdict verdict = espnowsensor::counterJudge(
      peer->eventTrack, peer->eventCounts, frame.bootTag, frame.counter, origin.resumePending, nowMs);
  const bool kept = espnowsensor::counterVerdictKeeps(verdict);
  if (kept) {
    peer->senderSkipped += frame.skipped;
    // One sender may fill at most its share: sixteen at once, then one a second.
    if (!espnowsensor::bucketTake(peer->eventBucket, espnowsensor::kEventRxBurst, espnowsensor::kEventRxRefillMs, nowMs)) {
      peer->refusedRate++;
    } else {
      peer->eventsStored++;
      store = true;
    }
  }
  PE_UNLOCK();
  if (origin.resumePending) peResumeTaken(origin.via, espnowfwd::kStreamIdEvents);
  // The bond master forwards what the counter rule kept, whatever its own
  // list's rate limit did (that limit is about this device's list).
  if (bondLink && kept) peForwardEvent(sender, payload, len);
  if (!store) return;
  // Which entry it takes is worked out without the lock (a full list costs
  // 24 x 24 compares over 3.6 KB of PSRAM): only this task writes the lists,
  // so they cannot change meanwhile. The entry is written under it.
  espnowsensor::StoredEvent* list = sPe.lists[mesh];
  const uint8_t slot = espnowsensor::eventListSlot(list, espnowsensor::kEventListLen);
  PE_LOCK();
  stored.rx = ++sPe.rxSerial;
  list[slot] = stored;
  PE_UNLOCK();
}

// ---- What is shown (cmd_exec) ---------------------------------------------------------

// Appends to a reply; false once it is full (the reply is then cut at `cap`).
static bool peAppend(char* reply, size_t cap, size_t* pos, const char* fmt, ...) {
  if (*pos >= cap) return false;
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(reply + *pos, cap - *pos, fmt, args);
  va_end(args);
  if (n < 0) return false;
  *pos += (size_t)n;
  if (*pos >= cap) { *pos = cap; return false; }
  return true;
}

// A copy of the record at `index`, taken under the lock. False: not in use.
static bool pePeerCopyAt(uint8_t index, PeCollectPeer* out) {
  bool have = false;
  PE_LOCK();
  if (sPe.peers && index < kPePeerSlots && sPe.peers[index].inUse) {
    *out = sPe.peers[index];
    have = true;
  }
  PE_UNLOCK();
  return have;
}

// The record of (mac, via); via nullptr: the origin's own record.
static bool pePeerCopyFor(const uint8_t mac[6], const uint8_t* via, PeCollectPeer* out) {
  for (uint8_t i = 0; i < kPePeerSlots; ++i) {
    if (pePeerCopyAt(i, out) && peSameSender(*out, mac, via)) return true;
  }
  return false;
}

static bool peForwarded(const PeCollectPeer& peer) { return peer.link == espnowfwd::kLinkForwarded; }

// The name to show for a sender: the one its frames came with, else its
// address; a forwarded one with " via <device>".
static void pePeerShownName(const PeCollectPeer& peer, char* out, size_t cap) {
  char base[33];
  if (peer.name[0]) strlcpy(base, peer.name, sizeof(base));
  else macToDisplay(peer.mac, base, sizeof(base));
  if (!peForwarded(peer)) {
    strlcpy(out, base, cap);
    return;
  }
  char via[32];
  peViaName(peer.via, via, sizeof(via));
  snprintf(out, cap, "%s via %s", base, via);
}

// `,"link":"..","via":{..}|null` of a JSON row.
static void peLinkJson(uint8_t link, const uint8_t via[6], char* out, size_t cap) {
  if (link != espnowfwd::kLinkForwarded) {
    snprintf(out, cap, ",\"link\":\"%s\",\"via\":null", espnowfwd::linkKey(link));
    return;
  }
  char name[32];
  char nameJson[72];
  peViaName(via, name, sizeof(name));
  espnowsensor::jsonTextEscape(name, nameJson, sizeof(nameJson));
  snprintf(out, cap, ",\"link\":\"forwarded\",\"via\":{\"mac\":\"" MAC_FMT "\",\"name\":\"%s\"}", MAC_ARGS(via),
           nameJson);
}

void espnowPowerEventsSendView(PowerEventsSendView* out) {
  if (!out) return;
  PeSendPlan plan;
  pePlan(peUptimeMs(), &plan);
  out->batteryWhy = plan.batteryWhy;
  out->eventsWhy = plan.eventsWhy;
  out->batteryIntervalS = (uint16_t)(espnowsensor::powerIntervalMs(pePowerIntervalS()) / 1000);
  out->eventFloor = peEventFloor() < espnowsensor::kEventPushCount ? peEventFloor() : espnowsensor::kEventPushOff;
  PE_LOCK();
  out->batterySent = sPe.powerSent;
  out->batteryEverSent = sPe.powerEverSent;
  out->batteryLastSentMs = sPe.powerLastSentMs;
  out->eventsSent = sPe.eventsSent;
  out->eventsLeftOut = sPe.eventsLeftOut;
  PE_UNLOCK();
}

// One sender's battery state as a JSON object. What the sender did not know
// is null. 0 when it does not fit `cap`.
static size_t peBatteryRowJson(const PeCollectPeer& peer, uint32_t nowMs, char* out, size_t cap) {
  const espnowsensor::PowerFrame& f = peer.power;
  char name[33];
  char nameJson[72];
  char linkJson[160];
  if (peer.name[0]) strlcpy(name, peer.name, sizeof(name));
  else macToDisplay(peer.mac, name, sizeof(name));
  espnowsensor::jsonTextEscape(name, nameJson, sizeof(nameJson));
  peLinkJson(peer.link, peer.via, linkJson, sizeof(linkJson));
  size_t pos = 0;
  peAppend(out, cap, &pos, "{\"mac\":\"" MAC_FMT "\",\"name\":\"%s\"%s", MAC_ARGS(peer.mac), nameJson, linkJson);
  if (peer.powerEpochMs != 0) peAppend(out, cap, &pos, ",\"arrivalEpochMs\":%lld", (long long)peer.powerEpochMs);
  else peAppend(out, cap, &pos, ",\"arrivalEpochMs\":null");
  peAppend(out, cap, &pos, ",\"arrivalUptimeMs\":%lu,\"agoMs\":%lu,\"clock\":\"%s\",\"bootTag\":%u,\"counter\":%u",
           (unsigned long)peer.powerUptimeMs, (unsigned long)(uint32_t)(nowMs - peer.powerUptimeMs),
           peClockKey(peer.powerEpochMs, peer.powerClock), (unsigned)f.bootTag, (unsigned)f.counter);
  peAppend(out, cap, &pos, ",\"status\":\"%s\",\"backend\":\"%s\"", espnowsensor::powerStatusKey(f.status),
           espnowsensor::powerBackendKey(f.backend));
  if (f.known & espnowsensor::kPowerVoltageValid) {
    peAppend(out, cap, &pos, ",\"voltage\":%u.%03u", (unsigned)(f.voltageMv / 1000), (unsigned)(f.voltageMv % 1000));
  } else {
    peAppend(out, cap, &pos, ",\"voltage\":null");
  }
  if (f.known & espnowsensor::kPowerPercentValid) {
    const unsigned pct = f.percentX10 > 1000 ? 1000u : f.percentX10;
    peAppend(out, cap, &pos, ",\"percent\":%u.%u", pct / 10, pct % 10);
  } else {
    peAppend(out, cap, &pos, ",\"percent\":null");
  }
  auto yesNo = [&](const char* key, uint16_t knownBit, uint16_t valueBit) {
    if (f.known & knownBit) peAppend(out, cap, &pos, ",\"%s\":%s", key, (f.known & valueBit) ? "true" : "false");
    else peAppend(out, cap, &pos, ",\"%s\":null", key);
  };
  auto flag = [&](const char* key, uint16_t bit) {
    peAppend(out, cap, &pos, ",\"%s\":%s", key, (f.known & bit) ? "true" : "false");
  };
  flag("percentEstimated", espnowsensor::kPowerPercentEstimated);
  yesNo("present", espnowsensor::kPowerPresenceKnown, espnowsensor::kPowerPresent);
  yesNo("charging", espnowsensor::kPowerChargingKnown, espnowsensor::kPowerCharging);
  flag("chargingEstimated", espnowsensor::kPowerChargingEstimated);
  yesNo("usb", espnowsensor::kPowerUsbKnown, espnowsensor::kPowerUsbPresent);
  if (f.known & espnowsensor::kPowerRateValid) {
    const int rate = f.rateX10;
    const unsigned magnitude = (unsigned)(rate < 0 ? -rate : rate);
    peAppend(out, cap, &pos, ",\"ratePctPerHr\":%s%u.%u", rate < 0 ? "-" : "", magnitude / 10, magnitude % 10);
  } else {
    peAppend(out, cap, &pos, ",\"ratePctPerHr\":null");
  }
  flag("statusEstimated", espnowsensor::kPowerStatusEstimated);
  flag("stale", espnowsensor::kPowerStale);
  if (f.sampleAgeMs != 65535) peAppend(out, cap, &pos, ",\"sampleAgeMs\":%u", (unsigned)f.sampleAgeMs);
  else peAppend(out, cap, &pos, ",\"sampleAgeMs\":null");
  const espnowsensor::CounterCounts& c = peer.powerCounts;
  const bool whole = peAppend(out, cap, &pos, ",\"received\":%lu,\"lost\":%lu,\"late\":%lu,\"restarts\":%lu,\"resumes\":%lu}",
                              (unsigned long)c.received, (unsigned long)c.lost, (unsigned long)c.late,
                              (unsigned long)c.restarts, (unsigned long)c.resumes);
  if (!whole) {
    if (cap) out[0] = '\0';
    return 0;
  }
  return pos;
}

// One sender's event counts as a JSON object. 0 when it does not fit.
static size_t peEventCountsJson(const PeCollectPeer& peer, bool withName, char* out, size_t cap) {
  size_t pos = 0;
  peAppend(out, cap, &pos, "{");
  if (withName) {
    char name[33];
    char nameJson[72];
    char linkJson[160];
    if (peer.name[0]) strlcpy(name, peer.name, sizeof(name));
    else macToDisplay(peer.mac, name, sizeof(name));
    espnowsensor::jsonTextEscape(name, nameJson, sizeof(nameJson));
    peLinkJson(peer.link, peer.via, linkJson, sizeof(linkJson));
    peAppend(out, cap, &pos, "\"mac\":\"" MAC_FMT "\",\"name\":\"%s\"%s,", MAC_ARGS(peer.mac), nameJson, linkJson);
  }
  const espnowsensor::CounterCounts& c = peer.eventCounts;
  const bool whole = peAppend(out, cap, &pos,
                              "\"received\":%lu,\"stored\":%lu,\"lost\":%lu,\"late\":%lu,\"restarts\":%lu,"
                              "\"resumes\":%lu,\"senderSkipped\":%lu,\"refusedRate\":%lu}",
                              (unsigned long)c.received, (unsigned long)peer.eventsStored, (unsigned long)c.lost,
                              (unsigned long)c.late, (unsigned long)c.restarts, (unsigned long)c.resumes,
                              (unsigned long)peer.senderSkipped, (unsigned long)peer.refusedRate);
  if (!whole) {
    if (cap) out[0] = '\0';
    return 0;
  }
  return pos;
}

static bool peSentEvents(const PeCollectPeer& peer) {
  return peer.eventCounts.received != 0 || peer.refusedRate != 0;
}

size_t espnowBatteryJsonFor(const uint8_t mac[6], const uint8_t* via, char* out, size_t cap) {
  PeCollectPeer peer;
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!mac || !pePeerCopyFor(mac, via, &peer) || !peer.havePower) return 0;
  return peBatteryRowJson(peer, peUptimeMs(), out, cap);
}

bool espnowEventCountsVisible() { return peCallerIsAdmin(); }

size_t espnowEventCountsJsonFor(const uint8_t mac[6], const uint8_t* via, char* out, size_t cap) {
  PeCollectPeer peer;
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!espnowEventCountsVisible()) return 0;
  if (!mac || !pePeerCopyFor(mac, via, &peer) || !peSentEvents(peer)) return 0;
  return peEventCountsJson(peer, false, out, cap);
}

// The battery state in words: "71.5 % (estimated), 3.912 V, discharging, USB power: no".
static void peBatteryWords(const espnowsensor::PowerFrame& f, char* out, size_t cap) {
  size_t pos = 0;
  if (cap) out[0] = '\0';
  if (f.known & espnowsensor::kPowerPercentValid) {
    const unsigned pct = f.percentX10 > 1000 ? 1000u : f.percentX10;
    peAppend(out, cap, &pos, "%u.%u %%%s", pct / 10, pct % 10,
             (f.known & espnowsensor::kPowerPercentEstimated) ? " (estimated)" : "");
  }
  if (f.known & espnowsensor::kPowerVoltageValid) {
    peAppend(out, cap, &pos, "%s%u.%03u V", pos ? ", " : "", (unsigned)(f.voltageMv / 1000), (unsigned)(f.voltageMv % 1000));
  }
  peAppend(out, cap, &pos, "%s%s", pos ? ", " : "", espnowsensor::powerStatusText(f.status));
  if (f.known & espnowsensor::kPowerUsbKnown) {
    peAppend(out, cap, &pos, ", USB power: %s", (f.known & espnowsensor::kPowerUsbPresent) ? "yes" : "no");
  }
  if (f.known & espnowsensor::kPowerStale) peAppend(out, cap, &pos, ", its last sample was old");
}

size_t espnowPowerEventsPeerText(const uint8_t mac[6], const uint8_t* via, char* out, size_t cap) {
  PeCollectPeer peer;
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!mac || !pePeerCopyFor(mac, via, &peer)) return 0;
  size_t pos = 0;
  if (peer.havePower) {
    if (peer.power.known & espnowsensor::kPowerPercentValid) {
      const unsigned pct = peer.power.percentX10 > 1000 ? 1000u : peer.power.percentX10;
      peAppend(out, cap, &pos, "battery %u.%u %% %lu s ago", pct / 10, pct % 10,
               (unsigned long)((uint32_t)(peUptimeMs() - peer.powerUptimeMs) / 1000));
    } else {
      peAppend(out, cap, &pos, "battery %s %lu s ago", espnowsensor::powerStatusText(peer.power.status),
               (unsigned long)((uint32_t)(peUptimeMs() - peer.powerUptimeMs) / 1000));
    }
  }
  if (peSentEvents(peer) && espnowEventCountsVisible()) {
    peAppend(out, cap, &pos, "%s%lu events", pos ? ", " : "", (unsigned long)peer.eventCounts.received);
  }
  return pos;
}

// ---- Commands (cmd_exec) ------------------------------------------------------------------

// The arguments of espnowbattery and espnowevents: `json`, `since <n>`,
// `from <n>`, and one other word (a device's name or address).
struct PeArgs {
  bool json;
  bool haveSince;
  uint32_t since;
  bool haveFrom;
  uint32_t from;
  char who[40];
  bool bad;
};

static void peParseArgs(const char* text, PeArgs* out) {
  memset(out, 0, sizeof(*out));
  char copy[96];
  strlcpy(copy, text ? text : "", sizeof(copy));
  char* save = nullptr;
  for (char* word = strtok_r(copy, " \t", &save); word; word = strtok_r(nullptr, " \t", &save)) {
    const bool isSince = strcmp(word, "since") == 0;
    if (strcmp(word, "json") == 0) {
      out->json = true;
    } else if (isSince || strcmp(word, "from") == 0) {
      char* number = strtok_r(nullptr, " \t", &save);
      char* end = nullptr;
      const unsigned long value = number ? strtoul(number, &end, 10) : 0;
      if (!number || number[0] < '0' || number[0] > '9' || !end || *end != '\0') {
        out->bad = true;
        return;
      }
      if (isSince) { out->haveSince = true; out->since = (uint32_t)value; }
      else { out->haveFrom = true; out->from = (uint32_t)value; }
    } else if (out->who[0] == '\0') {
      strlcpy(out->who, word, sizeof(out->who));
    } else {
      out->bad = true;
      return;
    }
  }
}

// "refused":{...} for both JSON replies.
static void peRefusedJson(char* reply, size_t cap, size_t* pos) {
  PeRefusals refused;
  PE_LOCK();
  refused = sPe.refused;
  PE_UNLOCK();
  peAppend(reply, cap, pos,
           ",\"refused\":{\"notReceiver\":%lu,\"full\":%lu,\"noMemory\":%lu,\"malformed\":%lu,\"notBondMaster\":%lu,"
           "\"forwardRefused\":%lu}",
           (unsigned long)refused.notReceiver, (unsigned long)refused.noRecord, (unsigned long)refused.noMemory,
           (unsigned long)refused.malformed, (unsigned long)refused.notBondMaster, (unsigned long)refused.forwardRefused);
}

const char* cmd_espnow_battery(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  size_t cap = 0;
  char* reply = peReply(&cap);
  if (!reply) return kCommandReplyOutOfMemory;
  PeArgs args;
  peParseArgs(argsInput.c_str(), &args);
  if (args.bad || args.haveSince) return "Usage: espnowbattery [json] [<name_or_mac>] [from <n>]";
  uint8_t onlyMac[6];
  const bool only = args.who[0] != '\0';
  if (only && !peResolve(args.who, onlyMac)) return "Error: no such device (use its name or its MAC address)";

  const uint32_t nowMs = peUptimeMs();
  PowerEventsSendView sending;
  espnowPowerEventsSendView(&sending);
  size_t pos = 0;

  if (args.json) {
    const char* whyKey = espnowsensor::pushWhyNotKey(sending.batteryWhy);
    peAppend(reply, cap, &pos, "{\"schema\":1,\"role\":\"%s\",\"sends\":{\"sends\":%s", espnowsensor::roleKey(peRole()),
             whyKey ? "false" : "true");
    if (whyKey) peAppend(reply, cap, &pos, ",\"whyNot\":\"%s\"", whyKey);
    else peAppend(reply, cap, &pos, ",\"whyNot\":null");
    peAppend(reply, cap, &pos, ",\"intervalS\":%u,\"sent\":%lu", (unsigned)sending.batteryIntervalS,
             (unsigned long)sending.batterySent);
    if (sending.batteryEverSent) {
      peAppend(reply, cap, &pos, ",\"lastSentAgoS\":%lu}", (unsigned long)((uint32_t)(nowMs - sending.batteryLastSentMs) / 1000));
    } else {
      peAppend(reply, cap, &pos, ",\"lastSentAgoS\":null}");
    }
    peRefusedJson(reply, cap, &pos);
    // The devices, in the order of their records. A reply holds about ten;
    // `next` is then the number to pass as 'from' for the rest.
    peAppend(reply, cap, &pos, ",\"from\":%lu,\"devices\":[", (unsigned long)args.from);
    static const size_t kTail = 24;  // room for `],"next":255}`
    bool first = true;
    int next = -1;
    for (uint32_t i = args.from; i < kPePeerSlots; ++i) {
      PeCollectPeer peer;
      if (!pePeerCopyAt((uint8_t)i, &peer) || !peer.havePower) continue;
      if (only && memcmp(peer.mac, onlyMac, 6) != 0) continue;
      if (pos + kTail + 2 >= cap) { next = (int)i; break; }
      const size_t room = cap - pos - kTail - 1;
      const size_t n = peBatteryRowJson(peer, nowMs, reply + pos + (first ? 0 : 1), room);
      if (n == 0) { next = (int)i; break; }
      if (!first) reply[pos] = ',';
      pos += n + (first ? 0 : 1);
      first = false;
    }
    reply[pos] = '\0';
    if (next >= 0) peAppend(reply, cap, &pos, "],\"next\":%d}", next);
    else peAppend(reply, cap, &pos, "],\"next\":null}");
    return reply;
  }

  unsigned shown = 0;
  for (uint8_t i = 0; i < kPePeerSlots; ++i) {
    PeCollectPeer peer;
    if (!pePeerCopyAt(i, &peer) || !peer.havePower) continue;
    if (only && memcmp(peer.mac, onlyMac, 6) != 0) continue;
    if (shown++ == 0) peConsole("[ESP-NOW] Battery state received from other devices");
    char name[72];
    char words[112];
    char when[48] = "this device's clock was not set";
    pePeerShownName(peer, name, sizeof(name));
    peBatteryWords(peer.power, words, sizeof(words));
    if (peer.powerEpochMs != 0) peFormatLocal(peer.powerEpochMs, when, sizeof(when));
    peConsole("  %s: %s; %lu s ago (%s); %lu frames kept, %lu lost", name, words,
              (unsigned long)((uint32_t)(nowMs - peer.powerUptimeMs) / 1000), when,
              (unsigned long)peer.powerCounts.received, (unsigned long)peer.powerCounts.lost);
  }
  if (sending.batteryWhy == espnowsensor::PushWhyNot::Sends) {
    peConsole("[ESP-NOW] This device sends its own battery state to the master named on it every %u s (%lu sent).",
              (unsigned)sending.batteryIntervalS, (unsigned long)sending.batterySent);
  } else if (sending.batteryWhy != espnowsensor::PushWhyNot::IsMaster) {
    peConsole("[ESP-NOW] This device does not send its own battery state: %s.",
              espnowsensor::pushWhyNotText(sending.batteryWhy));
  }
  if (shown == 0) {
    return espnowsensor::roleReceives(peRole())
               ? "OK: no battery state received from other devices yet"
               : "OK: this device keeps no battery state of other devices (only a mesh master or backup master does)";
  }
  snprintf(reply, cap, "OK: battery state of %u device%s", shown, shown == 1 ? "" : "s");
  return reply;
}

// The event after receive number `after`, copied under the lock, one list
// per hold (a hold scans 24 entries of PSRAM at most). False: none.
static bool peEventCopyNext(uint32_t after, espnowsensor::StoredEvent* out) {
  bool have = false;
  for (uint8_t m = 0; m < espnowsensor::kEventMeshes; ++m) {
    PE_LOCK();
    const espnowsensor::StoredEvent* list = sPe.lists[m];
    const espnowsensor::StoredEvent* next =
        list ? espnowsensor::eventListNext(&list, 1, espnowsensor::kEventListLen, after) : nullptr;
    if (next && (!have || next->rx < out->rx)) {
      *out = *next;
      have = true;
    }
    PE_UNLOCK();
  }
  return have;
}

// The name of an event's sender (without the via), by its record; its
// address when there is none.
static void peEventSenderName(const espnowsensor::StoredEvent& e, char* out, size_t cap) {
  PeCollectPeer peer;
  if (pePeerCopyFor(e.senderMac, e.link == espnowfwd::kLinkForwarded ? e.via : nullptr, &peer) && peer.name[0]) {
    strlcpy(out, peer.name, cap);
    return;
  }
  macToDisplay(e.senderMac, out, cap);
}

// One stored event as a JSON object. 0 when it does not fit `cap`.
static size_t peEventJson(const espnowsensor::StoredEvent& e, char* out, size_t cap) {
  char name[32];
  char nameJson[72];
  char kind[2 * sizeof(e.kind)];
  char subject[2 * sizeof(e.subject)];
  char detail[2 * sizeof(e.detail)];
  char who[2 * sizeof(e.who)];
  char linkJson[160];
  peEventSenderName(e, name, sizeof(name));
  espnowsensor::jsonTextEscape(name, nameJson, sizeof(nameJson));
  peLinkJson(e.link, e.via, linkJson, sizeof(linkJson));
  espnowsensor::jsonTextEscape(e.kind, kind, sizeof(kind));
  espnowsensor::jsonTextEscape(e.subject, subject, sizeof(subject));
  espnowsensor::jsonTextEscape(e.detail, detail, sizeof(detail));
  espnowsensor::jsonTextEscape(e.who, who, sizeof(who));
  size_t pos = 0;
  peAppend(out, cap, &pos, "{\"rx\":%lu,\"mesh\":%u,\"mac\":\"" MAC_FMT "\",\"name\":\"%s\"%s", (unsigned long)e.rx,
           (unsigned)e.mesh, MAC_ARGS(e.senderMac), nameJson, linkJson);
  if (e.arrivalEpochMs != 0) peAppend(out, cap, &pos, ",\"arrivalEpochMs\":%lld", (long long)e.arrivalEpochMs);
  else peAppend(out, cap, &pos, ",\"arrivalEpochMs\":null");
  peAppend(out, cap, &pos,
           ",\"arrivalUptimeMs\":%lu,\"clock\":\"%s\",\"ageMs\":%lu,\"eventSeq\":%lu,\"bootTag\":%u,\"counter\":%u"
           ",\"tier\":\"%s\",\"source\":\"%s\",\"kind\":\"%s\",\"subject\":\"%s\",\"detail\":\"%s\",\"who\":\"%s\",\"cut\":[",
           (unsigned long)e.arrivalUptimeMs, peClockKey(e.arrivalEpochMs, e.arrivalClock), (unsigned long)e.ageMs,
           (unsigned long)e.eventSeq, (unsigned)e.bootTag, (unsigned)e.counter, espnowsensor::eventTierKey(e.tier),
           espnowsensor::eventSourceKey(e.source), kind, subject, detail, who);
  bool firstCut = true;
  auto cut = [&](uint8_t bit, const char* field) {
    if (!(e.flags & bit)) return;
    peAppend(out, cap, &pos, "%s\"%s\"", firstCut ? "" : ",", field);
    firstCut = false;
  };
  cut(espnowsensor::kEventCutKind, "kind");
  cut(espnowsensor::kEventCutSubject, "subject");
  cut(espnowsensor::kEventCutDetail, "detail");
  cut(espnowsensor::kEventCutWho, "who");
  if (!peAppend(out, cap, &pos, "]}")) {
    if (cap) out[0] = '\0';
    return 0;
  }
  return pos;
}

const char* cmd_espnow_events(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  size_t cap = 0;
  char* reply = peReply(&cap);
  if (!reply) return kCommandReplyOutOfMemory;
  PeArgs args;
  peParseArgs(argsInput.c_str(), &args);
  if (args.bad || args.haveFrom || args.who[0] != '\0') return "Usage: espnowevents [json] [since <rx>]";

  PowerEventsSendView sending;
  espnowPowerEventsSendView(&sending);
  uint32_t latest;
  PE_LOCK();
  latest = sPe.rxSerial;
  PE_UNLOCK();
  // A number above the newest is from before this device restarted.
  const uint32_t since = espnowsensor::eventSinceFor(args.haveSince ? args.since : 0, latest);
  uint32_t held = 0;
  espnowsensor::StoredEvent e;
  for (uint32_t after = since; peEventCopyNext(after, &e); after = e.rx) held++;
  const uint32_t overwritten = (latest - since) > held ? (latest - since) - held : 0;
  size_t pos = 0;

  if (args.json) {
    const char* whyKey = espnowsensor::pushWhyNotKey(sending.eventsWhy);
    peAppend(reply, cap, &pos, "{\"schema\":1,\"role\":\"%s\",\"boot\":%lu,\"latest\":%lu,\"since\":%lu,\"overwritten\":%lu",
             espnowsensor::roleKey(peRole()), (unsigned long)peBootCounter(), (unsigned long)latest, (unsigned long)since,
             (unsigned long)overwritten);
    peAppend(reply, cap, &pos, ",\"sends\":{\"sends\":%s", whyKey ? "false" : "true");
    if (whyKey) peAppend(reply, cap, &pos, ",\"whyNot\":\"%s\"", whyKey);
    else peAppend(reply, cap, &pos, ",\"whyNot\":null");
    peAppend(reply, cap, &pos, ",\"floor\":\"%s\",\"sent\":%lu,\"leftOut\":%lu}", espnowsensor::eventPushKey(sending.eventFloor),
             (unsigned long)sending.eventsSent, (unsigned long)sending.eventsLeftOut);
    peRefusedJson(reply, cap, &pos);
    // The senders that sent events, with their counts. They may take the
    // reply up to the room that is kept for the events (two of the largest);
    // a sender that does not fit is left out and `sendersMore` says so
    // ('espnowsensorstatus json <device>' has each sender's counts).
    static const size_t kEventRoom = 1500;
    peAppend(reply, cap, &pos, ",\"senders\":[");
    bool first = true;
    bool sendersMore = false;
    for (uint8_t i = 0; i < kPePeerSlots; ++i) {
      PeCollectPeer peer;
      if (!pePeerCopyAt(i, &peer) || !peSentEvents(peer)) continue;
      char row[320];
      const size_t n = peEventCountsJson(peer, true, row, sizeof(row));
      if (n == 0) continue;
      if (pos + n + 1 + kEventRoom > cap) {
        sendersMore = true;
        break;
      }
      peAppend(reply, cap, &pos, "%s%s", first ? "" : ",", row);
      first = false;
    }
    // The events after `since`, oldest first, as many as the reply holds.
    // `more`: ask again with since = the last rx of this reply.
    peAppend(reply, cap, &pos, "],\"sendersMore\":%s,\"events\":[", sendersMore ? "true" : "false");
    static const size_t kTail = 20;  // room for `],"more":false}`
    bool more = false;
    first = true;
    for (uint32_t after = since; peEventCopyNext(after, &e); after = e.rx) {
      if (pos + kTail + 2 >= cap) { more = true; break; }
      const size_t room = cap - pos - kTail - 1;
      const size_t n = peEventJson(e, reply + pos + (first ? 0 : 1), room);
      if (n == 0) { more = true; break; }
      if (!first) reply[pos] = ',';
      pos += n + (first ? 0 : 1);
      first = false;
    }
    if (pos < cap) reply[pos] = '\0';
    peAppend(reply, cap, &pos, "],\"more\":%s}", more ? "true" : "false");
    return reply;
  }

  unsigned shown = 0;
  for (uint32_t after = since; peEventCopyNext(after, &e); after = e.rx) {
    if (shown++ == 0) peConsole("[ESP-NOW] Events received from other devices (oldest first)");
    char name[72];
    char when[48];
    peEventSenderName(e, name, sizeof(name));
    if (e.link == espnowfwd::kLinkForwarded) {
      char via[32];
      peViaName(e.via, via, sizeof(via));
      strlcat(name, " via ", sizeof(name));
      strlcat(name, via, sizeof(name));
    }
    if (e.arrivalEpochMs != 0) peFormatLocal(e.arrivalEpochMs, when, sizeof(when));
    else snprintf(when, sizeof(when), "clock not set, up %lu s", (unsigned long)(e.arrivalUptimeMs / 1000));
    char by[40];
    if (e.who[0]) snprintf(by, sizeof(by), "%s:%s", espnowsensor::eventSourceKey(e.source), e.who);
    else snprintf(by, sizeof(by), "%s", espnowsensor::eventSourceKey(e.source));
    peConsole("  #%lu %s  %s  %s  %s: %s | %s (by %s)%s", (unsigned long)e.rx, when, name,
              espnowsensor::eventTierKey(e.tier), e.kind[0] ? e.kind : "?", e.subject[0] ? e.subject : "-",
              e.detail[0] ? e.detail : "-", by, e.flags ? " [text cut]" : "");
  }
  if (overwritten) {
    peConsole("[ESP-NOW] %lu older event%s no longer held (the list keeps %u per mesh).", (unsigned long)overwritten,
              overwritten == 1 ? " is" : "s are", (unsigned)espnowsensor::kEventListLen);
  }
  if (sending.eventsWhy == espnowsensor::PushWhyNot::Sends) {
    peConsole("[ESP-NOW] This device sends its own events (%s%s) to the master named on it (%lu sent, %lu left out).",
              espnowsensor::eventPushKey(sending.eventFloor),
              sending.eventFloor == espnowsensor::kEventPushVerbose ? ", all of them" : " and above",
              (unsigned long)sending.eventsSent, (unsigned long)sending.eventsLeftOut);
  } else if (sending.eventsWhy != espnowsensor::PushWhyNot::IsMaster) {
    peConsole("[ESP-NOW] This device does not send its own events: %s.", espnowsensor::pushWhyNotText(sending.eventsWhy));
  }
  if (shown == 0) {
    return espnowsensor::roleReceives(peRole())
               ? "OK: no events received from other devices"
               : "OK: this device keeps no events of other devices (only a mesh master or backup master does)";
  }
  snprintf(reply, cap, "OK: %u event%s, the newest is #%lu", shown, shown == 1 ? "" : "s", (unsigned long)latest);
  return reply;
}

// POWER_EVENTS_HOST_END

void v4hPowerStatus(const V4RxCtx& ctx) {
  powerEventsIngestPower(ctx.recv_info->src_addr, ctx.deviceName, ctx.payload, ctx.payloadLen, /*bondLink=*/false);
}

void v4hEventPush(const V4RxCtx& ctx) {
  powerEventsIngestEvent(ctx.recv_info->src_addr, ctx.deviceName, ctx.payload, ctx.payloadLen, /*bondLink=*/false);
}

#if ENABLE_BONDED_MODE
// BOND_POWER_STATUS / BOND_EVENT_PUSH: the receive table let them through only
// from the configured bond partner over its session (REQ_BOND_PEER).
void v4hBondPowerStatus(const V4RxCtx& ctx) {
  powerEventsIngestPower(ctx.recv_info->src_addr, ctx.deviceName, ctx.payload, ctx.payloadLen, /*bondLink=*/true);
}

void v4hBondEventPush(const V4RxCtx& ctx) {
  powerEventsIngestEvent(ctx.recv_info->src_addr, ctx.deviceName, ctx.payload, ctx.payloadLen, /*bondLink=*/true);
}
#endif

#endif  // ENABLE_ESPNOW
