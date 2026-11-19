// System_ESPNow_ForwardRx.cpp - the mesh master's side of forwarding for a bond partner
//
// See System_ESPNow_ForwardRx.h for what this is and
// System_ESPNow_ForwardCore.h for the rules.
#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include "System_ESPNow_ForwardRx.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "System_CommandReply.h"      // kCommandReplyOutOfMemory
#include "System_Debug.h"
#include "System_ESPNow.h"
#include "System_ESPNow_HandshakeBackoff.h"  // WarnLimiter
#include "System_ESPNow_Identity.h"
#include "System_ESPNow_PowerEventCore.h"  // wireTextCopy, jsonTextEscape
#include "System_ESPNow_RxCtx.h"
#include "System_ESPNow_Tx.h"
#include "System_ESPNow_Wire.h"
#include "System_MacText.h"
#include "System_MemUtil.h"
#include "System_Settings.h"
#include "System_Utils.h"
#if ENABLE_BONDED_MODE
#include "System_ESPNow_BondForward.h"
#endif

// ---- What the rules below ask of the device ---------------------------------
// Everything between FORWARD_RX_HOST_BEGIN and FORWARD_RX_HOST_END reaches the
// settings, the pairing table, the radio and the console only through these,
// so the host suite espnow_forward_rx runs that part as it is.

static portMUX_TYPE sFwdMux = portMUX_INITIALIZER_UNLOCKED;
#define FWD_LOCK()   portENTER_CRITICAL(&sFwdMux)
#define FWD_UNLOCK() portEXIT_CRITICAL(&sFwdMux)

static uint32_t fwdNow() { return (uint32_t)millis(); }
static uint8_t fwdRole() { return gSettings.meshRole; }
static void fwdSelfMac(uint8_t out[6]) { espnowSelfStaMac(out); }
// The key `mac` is paired under (false: not paired).
static bool fwdPairedKey(const uint8_t mac[6], uint8_t key[32]) {
  const PeerIdentity* pid = peerIdentityFindByMac(mac);
  if (!pid) return false;
  memcpy(key, pid->longTermPub, 32);
  return true;
}
static bool fwdIsPaired(const uint8_t mac[6]) { return peerIdentityFindByMac(mac) != nullptr; }
// The name this device knows a paired device by ("" when it has none).
static void fwdPeerName(const uint8_t mac[6], char* out, size_t cap) {
  strlcpy(out, getEspNowDeviceName(mac).c_str(), cap);
}
// `name` is (ignoring case) the name of a paired device or of this device.
static bool fwdNameTaken(const char* name) {
  if (espnowfwd::nameSameIgnoringCase(name, gSettings.espnowDeviceName.c_str())) return true;
  const uint8_t slots = peerIdentitySlotCount();
  for (uint8_t i = 0; i < slots; ++i) {
    const PeerIdentity* pid = peerIdentityAt(i);
    if (!pid) continue;
    if (espnowfwd::nameSameIgnoringCase(name, getEspNowDeviceName(pid->mac).c_str())) return true;
  }
  return false;
}
static bool fwdResolveName(const char* who, uint8_t mac[6]) { return resolveDeviceNameOrMac(String(who), mac); }
static bool fwdSend(const uint8_t mac[6], const uint8_t* payload, uint16_t len) {
  return espnowtx::sendAead(mac, ESPNOW_V4_TYPE_FORWARD_LINK, 0, generateMessageId(), payload, len, 1);
}
static void* fwdAlloc(size_t bytes) { return ps_alloc(bytes, AllocPref::PreferPsram, "espnow.forwardLinks"); }
static void fwdConsole(const char* fmt, ...) {
  char line[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  broadcastOutput(line);
}
static char* fwdReply(size_t* cap) {
  if (!ensureDebugBuffer()) return nullptr;
  *cap = GLOBAL_DEBUG_BUFFER_SIZE;
  return getDebugBuffer();
}
// A query or an answer is for the bond master's side (bond builds only).
// False: nothing here takes it.
static bool fwdToBondSide(const uint8_t src[6], const espnowfwd::LinkFrame& f) {
#if ENABLE_BONDED_MODE
  return bondForwardOnLinkFrame(src, f);
#else
  (void)src;
  (void)f;
  return false;
#endif
}

// FORWARD_RX_HOST_BEGIN

// Frames this device refused, whoever sent them. Under sFwdMux.
struct FwdRefusals {
  uint32_t malformed;         // 155 too short or an unknown op; a forwarded mark that makes no sense
  uint32_t unannounced;       // forwarded data from a device with no active link
  uint32_t staleLink;         // ... with another link number
  uint32_t streamNotAnnounced;
  uint32_t ignored;           // an answer or a query sent to this device's mesh-master side
};

struct FwdState {
  espnowfwd::ForwardLink* links;          // kLinkSlots, PSRAM, made by the first announcement or query
  FwdRefusals refused;
  // A refused announcement is said once a minute at most (espnow_task only).
  WarnLimiter refusedWarn;
  uint32_t lastTickMs;
  bool ticked;
};

EXT_RAM_BSS_ATTR static FwdState sFwd;  // PSRAM: tasks only, never the radio callback

// The entry of `via`, or nullptr. espnow_task reads without the lock (it is
// the only writer).
static espnowfwd::ForwardLink* fwdFind(const uint8_t via[6]) {
  if (!sFwd.links || !via) return nullptr;
  for (uint8_t i = 0; i < espnowfwd::kLinkSlots; ++i) {
    if (sFwd.links[i].inUse && espnowfwd::macSame(sFwd.links[i].via, via)) return &sFwd.links[i];
  }
  return nullptr;
}

// The entry of `via`, made when there is none: a free one, else one whose
// via is no longer paired. nullptr: the table could not be made, or sixteen
// paired devices hold entries. espnow_task, without the lock (the pairing
// lookup must not run under it).
static espnowfwd::ForwardLink* fwdEntryFor(const uint8_t via[6]) {
  if (!sFwd.links) {
    const size_t bytes = (size_t)espnowfwd::kLinkSlots * sizeof(espnowfwd::ForwardLink);
    espnowfwd::ForwardLink* links = (espnowfwd::ForwardLink*)fwdAlloc(bytes);
    if (!links) return nullptr;
    memset(links, 0, bytes);
    FWD_LOCK();
    sFwd.links = links;
    FWD_UNLOCK();
  }
  espnowfwd::ForwardLink* found = fwdFind(via);
  if (found) return found;
  espnowfwd::ForwardLink* freeEntry = nullptr;
  for (uint8_t i = 0; i < espnowfwd::kLinkSlots && !freeEntry; ++i) {
    if (!sFwd.links[i].inUse) freeEntry = &sFwd.links[i];
  }
  for (uint8_t i = 0; i < espnowfwd::kLinkSlots && !freeEntry; ++i) {
    if (!fwdIsPaired(sFwd.links[i].via)) freeEntry = &sFwd.links[i];
  }
  if (!freeEntry) return nullptr;
  FWD_LOCK();
  memset(freeEntry, 0, sizeof(*freeEntry));
  memcpy(freeEntry->via, via, 6);
  freeEntry->inUse = true;
  FWD_UNLOCK();
  return freeEntry;
}

static void fwdNameOf(const uint8_t mac[6], char* out, size_t cap) {
  fwdPeerName(mac, out, cap);
  if (!out[0]) macToDisplay(mac, out, cap);
}

// An answer to `f` from this device.
static void fwdAnswer(const uint8_t to[6], const espnowfwd::LinkFrame& f, uint8_t result) {
  espnowfwd::LinkFrame a = {};
  a.op = espnowfwd::kOpAnswer;
  a.answers = f.op;
  a.result = result;
  a.linkGen = f.linkGen;
  a.viaBootTag = f.viaBootTag;
  a.announceGen = f.announceGen;  // which announcement this answers (byte 7)
  a.streams = f.streams;
  a.sensorMask = f.sensorMask;
  memcpy(a.partnerMac, f.partnerMac, 6);
  uint8_t payload[espnowfwd::kLinkFrameLen];
  espnowfwd::packLinkFrame(payload, a);
  (void)fwdSend(to, payload, (uint16_t)sizeof(payload));
}

// FORWARD_LINK from a paired sender over its session.
static void forwardLinkIngest(const uint8_t* src, const char* srcName, const uint8_t* payload, size_t len) {
  espnowfwd::LinkFrame f = {};
  if (!src || !espnowfwd::parseLinkFrame(payload, len, f)) {
    FWD_LOCK();
    sFwd.refused.malformed++;
    FWD_UNLOCK();
    return;
  }
  if (f.op == espnowfwd::kOpQuery || f.op == espnowfwd::kOpAnswer) {
    if (!fwdToBondSide(src, f)) {
      FWD_LOCK();
      sFwd.refused.ignored++;
      FWD_UNLOCK();
    }
    return;
  }
  const uint32_t nowMs = fwdNow();
  const bool receives = espnowsensor::roleReceives(fwdRole());
  char viaName[32];
  strlcpy(viaName, srcName && srcName[0] ? srcName : "", sizeof(viaName));
  if (!viaName[0]) fwdNameOf(src, viaName, sizeof(viaName));

  if (f.op == espnowfwd::kOpWithdraw) {
    if (!receives) {
      fwdAnswer(src, f, espnowfwd::kResultNotReceiver);
      return;
    }
    espnowfwd::ForwardLink* e = fwdFind(src);
    bool ended = false;
    char partnerName[33] = "";
    if (e) {
      FWD_LOCK();
      ended = espnowfwd::linkApplyWithdraw(*e, f, nowMs);
      memcpy(partnerName, e->name, sizeof(partnerName));
      FWD_UNLOCK();
    }
    fwdAnswer(src, f, espnowfwd::kResultWithdrawn);
    if (ended) fwdConsole("[ESP-NOW] %s no longer forwards for %s", viaName, partnerName);
    return;
  }

  // An announcement.
  uint8_t self[6];
  fwdSelfMac(self);
  uint8_t pairedKey[32];
  const bool partnerPaired = fwdPairedKey(f.partnerMac, pairedKey);
  const bool keyMatches = partnerPaired && memcmp(pairedKey, f.partnerKey, 32) == 0;
  // The name shown for the partner: this device's own for a paired partner,
  // else the announced one when it passes the device-name rule (never cut or
  // cleaned); its address otherwise. Only an announced name can be taken.
  char shown[devicename::kFieldLen] = "";
  bool announcedName = false;
  if (partnerPaired) {
    fwdNameOf(f.partnerMac, shown, sizeof(shown));
  } else {
    announcedName = devicename::getField(f.partnerName, sizeof(f.partnerName), shown);
    if (!announcedName) macToDisplay(f.partnerMac, shown, sizeof(shown));
  }
  const bool nameTaken = announcedName && fwdNameTaken(shown);
  // Any paired device may announce (the receive table lets only paired
  // devices' session frames through): the same rule as for its own readings.
  uint8_t result = espnowfwd::announceCheck(f, src, self, receives, partnerPaired, keyMatches, nameTaken);
  espnowfwd::AnnounceEffect effect = espnowfwd::AnnounceEffect::Repeat;
  if (result == 0) {
    espnowfwd::ForwardLink* e = fwdEntryFor(src);
    if (!e) {
      result = espnowfwd::kResultFull;
    } else if (!espnowfwd::announceFresh(*e, f)) {
      result = espnowfwd::kResultStale;
    } else {
      FWD_LOCK();
      effect = espnowfwd::linkApplyAnnounce(*e, f, shown, partnerPaired, nowMs);
      FWD_UNLOCK();
      result = espnowfwd::kResultAccepted;
    }
  }
  fwdAnswer(src, f, result);
  char partnerText[18];
  macToDisplay(f.partnerMac, partnerText, sizeof(partnerText));
  if (result == espnowfwd::kResultAccepted && effect != espnowfwd::AnnounceEffect::Repeat &&
      effect != espnowfwd::AnnounceEffect::NewStream) {
    fwdConsole("[ESP-NOW] %s forwards the readings, battery state and events of its bond partner %s (%s)", viaName,
               shown, partnerText);
  } else if (result != espnowfwd::kResultAccepted && result != espnowfwd::kResultStale) {
    // Once a minute at most: a bond master that was refused waits 10 minutes,
    // but any paired device can send announcements at frame rate.
    uint32_t held = 0;
    if (warnLimiterTake(sFwd.refusedWarn, nowMs, 60000, &held)) {
      char more[64] = "";
      if (held) snprintf(more, sizeof(more), " (%lu more refused since the last such line)", (unsigned long)held);
      fwdConsole("[ESP-NOW] %s asked to forward for %s (%s), refused: %s%s", viaName, shown, partnerText,
                 espnowfwd::resultText(result), more);
    }
  }
}

// A forwarded frame no link of the sender names: the sender is asked once
// every 5 s which link it has (it then announces again).
static void fwdQueryIfDue(const uint8_t src[6], uint32_t nowMs) {
  espnowfwd::ForwardLink* e = fwdEntryFor(src);
  if (!e) return;
  bool due = false;
  FWD_LOCK();
  if (e->state == espnowfwd::LState::None) e->state = espnowfwd::LState::Queried;
  if (espnowfwd::queryDue(*e, nowMs)) {
    e->queriedOnce = true;
    e->lastQueryMs = nowMs;
    due = true;
  }
  FWD_UNLOCK();
  if (!due) return;
  espnowfwd::LinkFrame q = {};
  q.op = espnowfwd::kOpQuery;
  uint8_t payload[espnowfwd::kLinkFrameLen];
  espnowfwd::packLinkFrame(payload, q);
  (void)fwdSend(src, payload, (uint16_t)sizeof(payload));
}

ForwardResolve espnowForwardResolve(const uint8_t src[6], const char* srcName, bool bondLink, bool hasBit, bool bitSet,
                                    uint8_t fwdLink, uint8_t streamId, ForwardOrigin* out) {
  if (!out) return ForwardResolve::Malformed;
  memset(out, 0, sizeof(*out));
  if (src) memcpy(out->mac, src, 6);
  out->link = bondLink ? espnowfwd::kLinkBond : espnowfwd::kLinkMesh;
  strlcpy(out->name, srcName ? srcName : "", sizeof(out->name));
  if (!src) return ForwardResolve::Malformed;
  // The common case: nothing forwarded, nothing to look up.
  if (!bitSet && fwdLink == 0) return ForwardResolve::Own;
  const uint32_t nowMs = fwdNow();
  espnowfwd::ForwardLink* e = fwdFind(src);
  espnowfwd::Attribution a;
  FWD_LOCK();
  a = espnowfwd::attribute(e, bondLink, hasBit, bitSet, fwdLink, streamId);
  switch (a.verdict) {
    case espnowfwd::Verdict::Forwarded:
      memcpy(out->mac, e->partner, 6);
      memcpy(out->via, src, 6);
      out->link = espnowfwd::kLinkForwarded;
      out->resumePending = a.resumePending;
      memcpy(out->name, e->name, sizeof(out->name));
      e->frames++;
      e->lastDataMs = nowMs;
      break;
    case espnowfwd::Verdict::Malformed:
      sFwd.refused.malformed++;
      break;
    case espnowfwd::Verdict::Unannounced:
      sFwd.refused.unannounced++;
      if (e) e->refusedUnannounced++;
      break;
    case espnowfwd::Verdict::StaleLink:
      sFwd.refused.staleLink++;
      e->refusedStaleLink++;
      break;
    case espnowfwd::Verdict::StreamNotAnnounced:
      sFwd.refused.streamNotAnnounced++;
      e->refusedStream++;
      break;
    case espnowfwd::Verdict::Own:
      break;
  }
  FWD_UNLOCK();
  switch (a.verdict) {
    case espnowfwd::Verdict::Own:       return ForwardResolve::Own;
    case espnowfwd::Verdict::Forwarded: return ForwardResolve::Forwarded;
    case espnowfwd::Verdict::Malformed: return ForwardResolve::Malformed;
    case espnowfwd::Verdict::Unannounced:
    case espnowfwd::Verdict::StaleLink:
      // Only a device that keeps data asks (a worker refuses the frame anyway).
      if (espnowsensor::roleReceives(fwdRole())) fwdQueryIfDue(src, nowMs);
      return ForwardResolve::Refused;
    case espnowfwd::Verdict::StreamNotAnnounced:
      return ForwardResolve::Refused;
  }
  return ForwardResolve::Refused;
}

void espnowForwardResumeTaken(const uint8_t via[6], uint8_t streamId) {
  espnowfwd::ForwardLink* e = fwdFind(via);
  if (!e || streamId >= espnowfwd::kStreamIdCount) return;
  FWD_LOCK();
  e->resumeMask = (espnowsensor::SensorMask)(e->resumeMask & ~(1u << streamId));
  FWD_UNLOCK();
}

bool espnowForwardLinkActive(const uint8_t via[6], const uint8_t origin[6]) {
  bool active = false;
  FWD_LOCK();
  for (uint8_t i = 0; sFwd.links && i < espnowfwd::kLinkSlots; ++i) {
    const espnowfwd::ForwardLink& e = sFwd.links[i];
    if (e.inUse && espnowfwd::macSame(e.via, via)) {
      active = e.state == espnowfwd::LState::Active && espnowfwd::macSame(e.partner, origin);
      break;
    }
  }
  FWD_UNLOCK();
  return active;
}

// A copy of entry `index`. False: not in use.
static bool fwdCopyAt(uint8_t index, espnowfwd::ForwardLink* out) {
  bool have = false;
  FWD_LOCK();
  if (sFwd.links && index < espnowfwd::kLinkSlots && sFwd.links[index].inUse &&
      sFwd.links[index].state != espnowfwd::LState::None) {
    *out = sFwd.links[index];
    have = true;
  }
  FWD_UNLOCK();
  return have;
}

size_t espnowForwardLinkCount() {
  size_t n = 0;
  for (uint8_t i = 0; i < espnowfwd::kLinkSlots; ++i) {
    espnowfwd::ForwardLink e;
    if (fwdCopyAt(i, &e) && e.state == espnowfwd::LState::Active) n++;
  }
  return n;
}

// The active link that names `who` (the partner's shown name, any case, or
// its address), of `via` when given: one copy, taken under the lock.
static bool fwdFindActive(const char* who, const uint8_t* via, espnowfwd::ForwardLink* out) {
  if (!who || !who[0]) return false;
  uint8_t asMac[6];
  const bool isMac = espnowfwd::macFromText(who, strlen(who), asMac);
  for (uint8_t i = 0; i < espnowfwd::kLinkSlots; ++i) {
    if (!fwdCopyAt(i, out) || out->state != espnowfwd::LState::Active) continue;
    if (via && !espnowfwd::macSame(out->via, via)) continue;
    if (isMac ? espnowfwd::macSame(out->partner, asMac) : espnowfwd::nameSameIgnoringCase(out->name, who)) return true;
  }
  return false;
}

bool espnowForwardFindOrigin(const char* who, const uint8_t* via, uint8_t outOrigin[6], uint8_t outVia[6]) {
  espnowfwd::ForwardLink e;
  if (!fwdFindActive(who, via, &e)) return false;
  memcpy(outOrigin, e.partner, 6);
  memcpy(outVia, e.via, 6);
  return true;
}

void espnowForwardOriginName(const uint8_t via[6], const uint8_t origin[6], char* out, size_t cap) {
  if (!out || cap == 0) return;
  out[0] = '\0';
  for (uint8_t i = 0; i < espnowfwd::kLinkSlots; ++i) {
    espnowfwd::ForwardLink e;
    if (fwdCopyAt(i, &e) && espnowfwd::macSame(e.via, via) && espnowfwd::macSame(e.partner, origin)) {
      strlcpy(out, e.name, cap);
      return;
    }
  }
}

ForwardTarget espnowForwardRequestTarget(const char* who, const char* viaWho, uint8_t outVia[6], uint8_t outOrigin[6],
                                         uint8_t* outLinkGen) {
  if (!who || !who[0] || !outVia || !outOrigin || !outLinkGen) return ForwardTarget::NoLink;
  uint8_t via[6];
  const uint8_t* onlyVia = nullptr;
  if (viaWho) {
    if (!fwdResolveName(viaWho, via)) return ForwardTarget::NoVia;
    onlyVia = via;
  }
  // The partner, the via and the link number from the same copy of the link
  // (the link may change on espnow_task meanwhile). An active link's number
  // is never 0 (an announcement of 0 is malformed): 0 in a request would
  // address the via's own sensors.
  espnowfwd::ForwardLink e;
  if (!fwdFindActive(who, onlyVia, &e) || e.linkGen == 0) return ForwardTarget::NoLink;
  memcpy(outOrigin, e.partner, 6);
  memcpy(outVia, e.via, 6);
  *outLinkGen = e.linkGen;
  return ForwardTarget::Found;
}

// Once a second on espnow_task: a device that no longer keeps data (its role
// changed) ends every link it holds.
void espnowForwardRxTick(uint32_t nowMs) {
  if (sFwd.ticked && (uint32_t)(nowMs - sFwd.lastTickMs) < 1000) return;
  sFwd.ticked = true;
  sFwd.lastTickMs = nowMs;
  if (espnowsensor::roleReceives(fwdRole())) return;
  for (uint8_t i = 0; sFwd.links && i < espnowfwd::kLinkSlots; ++i) {
    espnowfwd::ForwardLink& e = sFwd.links[i];
    if (!e.inUse || e.state != espnowfwd::LState::Active) continue;
    FWD_LOCK();
    e.state = espnowfwd::LState::Ended;
    e.endedMs = nowMs;
    e.resumeMask = 0;
    FWD_UNLOCK();
  }
}

// ---- Commands (cmd_exec) ------------------------------------------------------

static bool fwdAppend(char* reply, size_t cap, size_t* pos, const char* fmt, ...) {
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

static void fwdStreamsJson(uint8_t streams, char* out, size_t cap) {
  size_t pos = 0;
  out[0] = '\0';
  fwdAppend(out, cap, &pos, "[");
  bool first = true;
  if (streams & espnowfwd::kStreamReadings) { fwdAppend(out, cap, &pos, "\"readings\""); first = false; }
  if (streams & espnowfwd::kStreamBattery) { fwdAppend(out, cap, &pos, "%s\"battery\"", first ? "" : ","); first = false; }
  if (streams & espnowfwd::kStreamEvents) fwdAppend(out, cap, &pos, "%s\"events\"", first ? "" : ",");
  fwdAppend(out, cap, &pos, "]");
}

static void fwdStreamsWords(uint8_t streams, char* out, size_t cap) {
  size_t pos = 0;
  out[0] = '\0';
  bool first = true;
  if (streams & espnowfwd::kStreamReadings) { fwdAppend(out, cap, &pos, "readings"); first = false; }
  if (streams & espnowfwd::kStreamBattery) { fwdAppend(out, cap, &pos, "%sbattery", first ? "" : ", "); first = false; }
  if (streams & espnowfwd::kStreamEvents) { fwdAppend(out, cap, &pos, "%sevents", first ? "" : ", "); first = false; }
  if (first) fwdAppend(out, cap, &pos, "nothing");
}

// One link as a JSON object. 0 when it does not fit.
static size_t fwdLinkJson(const espnowfwd::ForwardLink& e, uint32_t nowMs, char* out, size_t cap) {
  char viaName[32];
  char viaJson[72];
  char nameJson[72];
  char streams[48];
  char sensors[160];
  fwdNameOf(e.via, viaName, sizeof(viaName));
  espnowsensor::jsonTextEscape(viaName, viaJson, sizeof(viaJson));
  espnowsensor::jsonTextEscape(e.name, nameJson, sizeof(nameJson));
  fwdStreamsJson(e.streams, streams, sizeof(streams));
  size_t spos = 0;
  sensors[0] = '\0';
  fwdAppend(sensors, sizeof(sensors), &spos, "[");
  bool first = true;
  for (uint8_t k = 0; k < REMOTE_SENSOR_MAX; ++k) {
    if (!(e.sensorMask & espnowsensor::sensorBit(k))) continue;
    fwdAppend(sensors, sizeof(sensors), &spos, "%s\"%s\"", first ? "" : ",", espnowsensor::sensorName(k));
    first = false;
  }
  fwdAppend(sensors, sizeof(sensors), &spos, "]");
  size_t pos = 0;
  fwdAppend(out, cap, &pos,
            "{\"via\":\"" MAC_FMT "\",\"viaName\":\"%s\",\"partner\":\"" MAC_FMT "\",\"name\":\"%s\",\"namePaired\":%s,"
            "\"linkGen\":%u,\"viaBootTag\":%u,\"state\":\"%s\",\"confirmed\":%s,\"streams\":%s,\"sensors\":%s",
            MAC_ARGS(e.via), viaJson, MAC_ARGS(e.partner), nameJson, e.namePaired ? "true" : "false", (unsigned)e.linkGen,
            (unsigned)e.viaBootTag, espnowfwd::linkStateKey(e.state), espnowfwd::linkConfirmed(e, nowMs) ? "true" : "false",
            streams, sensors);
  if (e.state == espnowfwd::LState::Active || e.state == espnowfwd::LState::Ended) {
    fwdAppend(out, cap, &pos, ",\"acceptedAgoS\":%lu", (unsigned long)((uint32_t)(nowMs - e.acceptedMs) / 1000));
  } else {
    fwdAppend(out, cap, &pos, ",\"acceptedAgoS\":null");
  }
  if (e.frames) fwdAppend(out, cap, &pos, ",\"lastDataAgoS\":%lu", (unsigned long)((uint32_t)(nowMs - e.lastDataMs) / 1000));
  else fwdAppend(out, cap, &pos, ",\"lastDataAgoS\":null");
  const bool whole = fwdAppend(out, cap, &pos,
                               ",\"frames\":%lu,\"refused\":{\"unannounced\":%lu,\"staleLink\":%lu,\"streamNotAnnounced\":%lu}}",
                               (unsigned long)e.frames, (unsigned long)e.refusedUnannounced,
                               (unsigned long)e.refusedStaleLink, (unsigned long)e.refusedStream);
  if (!whole) {
    if (cap) out[0] = '\0';
    return 0;
  }
  return pos;
}

// espnowforwardlinks [json] [from <n>]
const char* cmd_espnow_forwardlinks(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  size_t cap = 0;
  char* reply = fwdReply(&cap);
  if (!reply) return kCommandReplyOutOfMemory;
  bool json = false;
  uint32_t from = 0;
  char copy[64];
  strlcpy(copy, argsInput.c_str(), sizeof(copy));
  char* save = nullptr;
  for (char* word = strtok_r(copy, " \t", &save); word; word = strtok_r(nullptr, " \t", &save)) {
    if (strcmp(word, "json") == 0) {
      json = true;
    } else if (strcmp(word, "from") == 0) {
      char* number = strtok_r(nullptr, " \t", &save);
      char* end = nullptr;
      const unsigned long value = number ? strtoul(number, &end, 10) : 0;
      if (!number || number[0] < '0' || number[0] > '9' || !end || *end != '\0') {
        return "Usage: espnowforwardlinks [json] [from <n>]";
      }
      from = (uint32_t)value;
    } else {
      return "Usage: espnowforwardlinks [json] [from <n>]";
    }
  }
  const uint32_t nowMs = fwdNow();
  if (json) {
    size_t pos = 0;
    FwdRefusals refused;
    FWD_LOCK();
    refused = sFwd.refused;
    FWD_UNLOCK();
    fwdAppend(reply, cap, &pos, "{\"schema\":1,\"role\":\"%s\"", espnowsensor::roleKey(fwdRole()));
    fwdAppend(reply, cap, &pos,
              ",\"refused\":{\"malformed\":%lu,\"unannounced\":%lu,\"staleLink\":%lu,\"streamNotAnnounced\":%lu,"
              "\"ignored\":%lu},\"from\":%lu,\"links\":[",
              (unsigned long)refused.malformed, (unsigned long)refused.unannounced, (unsigned long)refused.staleLink,
              (unsigned long)refused.streamNotAnnounced, (unsigned long)refused.ignored, (unsigned long)from);
    static const size_t kTail = 24;  // room for `],"next":15}`
    bool first = true;
    int next = -1;
    for (uint32_t i = from; i < espnowfwd::kLinkSlots; ++i) {
      espnowfwd::ForwardLink e;
      if (!fwdCopyAt((uint8_t)i, &e)) continue;
      if (pos + kTail + 2 >= cap) { next = (int)i; break; }
      const size_t room = cap - pos - kTail - 1;
      const size_t m = fwdLinkJson(e, nowMs, reply + pos + (first ? 0 : 1), room);
      if (m == 0) { next = (int)i; break; }
      if (!first) reply[pos] = ',';
      pos += m + (first ? 0 : 1);
      first = false;
    }
    reply[pos] = '\0';
    if (next >= 0) fwdAppend(reply, cap, &pos, "],\"next\":%d}", next);
    else fwdAppend(reply, cap, &pos, "],\"next\":null}");
    return reply;
  }
  unsigned shown = 0;
  for (uint8_t i = 0; i < espnowfwd::kLinkSlots; ++i) {
    espnowfwd::ForwardLink e;
    if (!fwdCopyAt(i, &e)) continue;
    if (shown++ == 0) fwdConsole("[ESP-NOW] Devices that forward their bond partner's data to this device");
    char viaName[32];
    char partner[18];
    char streams[40];
    char state[96];
    fwdNameOf(e.via, viaName, sizeof(viaName));
    macToDisplay(e.partner, partner, sizeof(partner));
    fwdStreamsWords(e.streams, streams, sizeof(streams));
    if (e.state == espnowfwd::LState::Active) {
      snprintf(state, sizeof(state), "link %u, since %lu s%s", (unsigned)e.linkGen,
               (unsigned long)((uint32_t)(nowMs - e.acceptedMs) / 1000),
               espnowfwd::linkConfirmed(e, nowMs) ? "" : ", not confirmed for 30 minutes");
    } else if (e.state == espnowfwd::LState::Ended) {
      snprintf(state, sizeof(state), "ended %lu s ago", (unsigned long)((uint32_t)(nowMs - e.endedMs) / 1000));
    } else {
      snprintf(state, sizeof(state), "sent forwarded data without a link; asked which link it has");
    }
    char data[40] = "no data yet";
    if (e.frames) {
      snprintf(data, sizeof(data), "last data %lu s ago", (unsigned long)((uint32_t)(nowMs - e.lastDataMs) / 1000));
    }
    fwdConsole("  %s forwards for %s (%s): %s; %s, %s; refused: not announced %lu, old link %lu, stream not "
               "announced %lu",
               viaName, e.name[0] ? e.name : partner, partner, streams, state, data, (unsigned long)e.refusedUnannounced,
               (unsigned long)e.refusedStaleLink, (unsigned long)e.refusedStream);
  }
  if (shown == 0) {
    return espnowsensor::roleReceives(fwdRole())
               ? "OK: no device forwards its bond partner's data to this device"
               : "OK: this device keeps no forwarded data (only a mesh master or backup master does)";
  }
  snprintf(reply, cap, "OK: %u link%s", shown, shown == 1 ? "" : "s");
  return reply;
}

// FORWARD_RX_HOST_END

void v4hForwardLink(const V4RxCtx& ctx) {
  forwardLinkIngest(ctx.recv_info->src_addr, ctx.deviceName, ctx.payload, ctx.payloadLen);
}

#endif  // ENABLE_ESPNOW
