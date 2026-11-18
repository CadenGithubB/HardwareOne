// System_ESPNow_MeshTime.cpp - mesh time (TIME_SYNC, opcode 35)
//
// See System_ESPNow_MeshTime.h for what this is and System_ESPNow_TimeCore.h
// for the rules. Three tasks meet here:
//   espnow_task  receives a frame (kept for the main loop) and, on the
//                master, sends the clock to the paired devices;
//   main loop    judges the kept frame and sets the clock, where the other
//                clock sources are handed in as well;
//   cmd_exec     espnowtimesync, espnowtimestatus.
// What they share is under sMeshTimeMux; the rest belongs to one task and
// says which.
#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include "System_ESPNow_MeshTime.h"

#include <Arduino.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <esp_timer.h>

#include "System_Clock.h"
#include "System_CommandRegistry.h"   // argWantsJson
#include "System_CommandReply.h"      // kCommandReplyOutOfMemory
#include "System_Debug.h"
#include "System_ESPNow.h"
#include "System_ESPNow_Handlers_Crypto.h"  // espnowHandshakeAutoStart
#include "System_ESPNow_Identity.h"
#include "System_ESPNow_RxCtx.h"
#include "System_ESPNow_Sensors.h"    // espnowNamedMasterKind
#include "System_ESPNow_Sessions.h"
#include "System_ESPNow_TimeCore.h"
#include "System_ESPNow_Tx.h"
#include "System_ESPNow_Wire.h"
#include "System_Settings.h"
#include "System_Utils.h"             // RETURN_VALID_IF_VALIDATE_CSTR, macToDisplayStr

static_assert((int)meshtime::OwnSource::None == (int)Clock::SYNC_NONE &&
              (int)meshtime::OwnSource::Rtc == (int)Clock::SYNC_RTC &&
              (int)meshtime::OwnSource::Ntp == (int)Clock::SYNC_NTP &&
              (int)meshtime::OwnSource::Manual == (int)Clock::SYNC_MANUAL &&
              (int)meshtime::OwnSource::Ring == (int)Clock::SYNC_RING &&
              (int)meshtime::OwnSource::Cm5 == (int)Clock::SYNC_CM5 &&
              (int)meshtime::OwnSource::Mesh == (int)Clock::SYNC_MESH,
              "meshtime::OwnSource mirrors Clock::SyncSource");
static_assert((int)MESH_ROLE_MASTER == 1 && (int)MESH_ROLE_BACKUP_MASTER == 2,
              "the role numbers this file reads");

// ---- What the rules below ask of the device ---------------------------------
// Everything from MESH_TIME_HOST_BEGIN to MESH_TIME_HOST_END reaches the
// clock, the settings, the peer tables and the radio only through these, so
// the host suite espnow_time_status runs that part as it is with its own
// versions of them.

static portMUX_TYPE sMeshTimeMux = portMUX_INITIALIZER_UNLOCKED;
#define MESH_TIME_LOCK()   portENTER_CRITICAL(&sMeshTimeMux)
#define MESH_TIME_UNLOCK() portEXIT_CRITICAL(&sMeshTimeMux)

static constexpr uint8_t kMeshTimePeerSlots = MESH_PEER_MAX;

static uint32_t mtUptimeMs() { return (uint32_t)millis(); }
static int64_t mtMonoMs() { return (int64_t)(esp_timer_get_time() / 1000); }
static bool mtClockSet() { return Clock::isSynced(); }
static int64_t mtEpochMs() { return Clock::epochMillis(); }
static meshtime::OwnSource mtOwnSource() { return (meshtime::OwnSource)Clock::syncSource(); }
static bool mtClockHeld() { return Clock::confirmedClockHeld(); }
static uint32_t mtStepSerial() { return Clock::stepSerial(); }
// Seconds since the clock was last set this boot; false when nothing set it.
static bool mtLastSetAgo(uint32_t* agoSeconds) { return Clock::lastStep(nullptr, agoSeconds); }
// The one place mesh time sets the clock: settimeofday, then the chokepoint.
static void mtSetClock(int64_t epochMs) {
  const time_t before = time(nullptr);
  struct timeval tv;
  tv.tv_sec = (time_t)(epochMs / 1000);
  tv.tv_usec = (suseconds_t)((epochMs % 1000) * 1000);
  settimeofday(&tv, nullptr);
  Clock::clockStepped(Clock::SYNC_MESH, before);
}
static void mtFormatLocal(int64_t epochMs, char* out, size_t cap) {
  char iso[24];
  char tz[16];
  Clock::formatIso8601Local((time_t)(epochMs / 1000), iso, sizeof(iso));
  snprintf(out, cap, "%s %s", iso, Clock::formatTzOffsetLabel(tz, sizeof(tz)));
}
static bool mtSettingOn() { return gSettings.meshTime; }
static bool mtMeshOn() { return meshEnabled(); }
static uint8_t mtRole() { return gSettings.meshRole; }
static bool mtBackupPromoted() { return espnowBackupPromoted(); }
static bool mtHasNamedMaster() { return espnowHasNamedMaster(); }
// The two fingerprints as one number: a change of either drops the sender
// followed and the claim held (TimeCore row 0), by whatever path it came.
static uint64_t mtNamedTag() {
  return meshtime::namedMastersTag(gSettings.espnowMasterFingerprint.c_str(),
                                   gSettings.espnowBackupMasterFingerprint.c_str());
}
// 0: not a named master, 1: the primary, 2: the backup.
static int mtNamedKind(const uint8_t mac[6]) { return espnowNamedMasterKind(mac); }
// The paired devices: every stored identity.
static uint8_t mtPeerSlots() { return peerIdentitySlotCount(); }
static bool mtPeerAt(uint8_t i, uint8_t mac[6]) {
  const PeerIdentity* pid = peerIdentityAt(i);
  if (!pid) return false;
  memcpy(mac, pid->mac, 6);
  return true;
}
static bool mtIsPaired(const uint8_t mac[6]) { return peerIdentityFindByMac(mac) != nullptr; }
// True with an ACTIVE session; *establishedAtMs is then when it came up.
static bool mtSession(const uint8_t mac[6], uint32_t* establishedAtMs) {
  return espnowSessionUp(mac, establishedAtMs);
}
static bool mtSend(const uint8_t mac[6], const uint8_t* payload, uint16_t len) {
  return espnowtx::sendAead(mac, ESPNOW_V4_TYPE_TIME_SYNC, 0, generateMessageId(), payload, len, 1);
}
static bool mtHandshake(const uint8_t mac[6]) { return espnowHandshakeAutoStart(mac); }
static void mtPeerName(const uint8_t mac[6], char* out, size_t cap) {
  String name = getEspNowDeviceName(mac);
  if (name.length() == 0) name = macToDisplayStr(mac);
  strlcpy(out, name.c_str(), cap);
}
static void mtConsole(const char* fmt, ...) {
  char line[160];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  broadcastOutput(line);
}
static char* mtReply(size_t* cap) {
  if (!ensureDebugBuffer()) return nullptr;
  *cap = GLOBAL_DEBUG_BUFFER_SIZE;
  return getDebugBuffer();
}
static bool mtWantsJson(const String& args) { return argWantsJson(args); }

// MESH_TIME_HOST_BEGIN

// What the handler keeps for the main loop: the newest frame of each of up
// to four senders. A sender's next frame replaces its own; another sender's
// frame never pushes it out, so a paired device that sends TIME_SYNC
// frames all the time cannot keep the master's frame from being judged. A
// frame from a master named on this device may take the place of one from a
// device that is not named; any other frame that finds all four taken is
// dropped and counted as not used.
static constexpr uint8_t kMeshTimeStashSlots = 4;
struct MeshTimeStash {
  bool pending;
  bool named;           // the sender is a master named on this device
  meshtime::Received rx;
};

// The last frame that was judged, for espnowtimestatus.
struct MeshTimeLast {
  bool have;
  uint8_t mac[6];
  uint8_t source;
  uint16_t sinceSetMin;
  meshtime::Verdict verdict;
  int64_t differenceMs;
  uint32_t atMs;
};

struct MeshTimeState {
  // Under sMeshTimeMux.
  MeshTimeStash stash[kMeshTimeStashSlots];
  MeshTimeLast last;
  uint32_t counts[meshtime::kVerdictCount];
  uint32_t shortFrames;
  uint32_t droppedFrames;   // found the four kept frames all taken
  bool roundAsked;          // espnowtimesync asked for a round
  bool everSent;
  uint32_t lastSentMs;
  uint32_t framesSent;
  uint8_t roundReached;     // peers the current round has reached
  // Main loop only.
  meshtime::FollowerState follower;
  bool heldLineHave;        // a "held" console line was printed ...
  uint32_t heldLineMs;      // ... at this uptime (one a minute at most)
  // espnow_task only.
  meshtime::RoundState round;
  meshtime::PeerSendState peers[kMeshTimePeerSlots];
  uint32_t lastLookMs;
};

EXT_RAM_BSS_ATTR static MeshTimeState sMeshTime;  // PSRAM: tasks only, never the radio callback

// ---- Receive: keep the frame -------------------------------------------------

// espnow_task. A frame that is too short is counted and dropped; a good one
// replaces the same sender's frame that the main loop has not judged yet.
static void meshTimeKeepFrame(const uint8_t sender[6], const uint8_t* payload, size_t len) {
  meshtime::Received rx = {};
  if (!sender || !meshtime::parseFrame(payload, len, rx.frame)) {
    MESH_TIME_LOCK();
    sMeshTime.shortFrames++;
    MESH_TIME_UNLOCK();
    return;
  }
  memcpy(rx.sender, sender, 6);
  rx.receiptMonoMs = mtMonoMs();
  const bool named = mtNamedKind(sender) != 0;  // (the pairing lookup is not done under the lock)
  MESH_TIME_LOCK();
  MeshTimeStash* slot = nullptr;
  for (auto& e : sMeshTime.stash) {
    if (e.pending && memcmp(e.rx.sender, sender, 6) == 0) { slot = &e; break; }
  }
  for (auto& e : sMeshTime.stash) {
    if (!slot && !e.pending) slot = &e;
  }
  for (auto& e : sMeshTime.stash) {
    if (!slot && named && !e.named) slot = &e;
  }
  if (slot) {
    if (slot->pending && memcmp(slot->rx.sender, sender, 6) != 0) sMeshTime.droppedFrames++;  // the one it pushed out
    slot->pending = true;
    slot->named = named;
    slot->rx = rx;
  } else {
    sMeshTime.droppedFrames++;
  }
  MESH_TIME_UNLOCK();
}

// ---- Apply: judge the kept frame and set the clock ----------------------------

static meshtime::FollowerInputs meshTimeFollowerInputs(const uint8_t sender[6]) {
  meshtime::FollowerInputs in = {};
  in.namedTag = mtNamedTag();
  in.settingOn = mtSettingOn();
  in.meshOn = mtMeshOn();
  in.hasNamedMaster = mtHasNamedMaster();
  const int kind = mtNamedKind(sender);
  in.senderIsNamed = kind != 0;
  in.senderIsNamedPrimary = kind == 1;
  in.actingMaster = mtMeshOn() && mtRole() == 1;
  in.clockSet = mtClockSet();
  in.ownSource = mtOwnSource();
  in.nowEpochMs = in.clockSet ? mtEpochMs() : 0;
  in.nowMonoMs = mtMonoMs();
  return in;
}

// Judges one kept frame and sets the clock when it is applied.
static void meshTimeJudge(const MeshTimeStash& stash) {
  // timeset runs on another task and an SNTP reply is reported by the tick
  // before this one, so the clock's state can change between the decision
  // and the step. The step counter is read at the decision and again right
  // before the clock is set; when it moved, the frame is judged once more
  // against the new state (and then finds the better source).
  meshtime::Decision decision = { meshtime::Verdict::IgnoredTooSoon, 0, 0 };
  for (int attempt = 0; attempt < 2; ++attempt) {
    const uint32_t serial = mtStepSerial();
    const meshtime::FollowerInputs in = meshTimeFollowerInputs(stash.rx.sender);
    meshtime::FollowerState trial = sMeshTime.follower;
    decision = meshtime::followerDecide(trial, in, stash.rx);
    if (decision.verdict != meshtime::Verdict::Applied) {
      sMeshTime.follower = trial;
      break;
    }
    if (mtStepSerial() != serial) {
      decision.verdict = meshtime::Verdict::IgnoredTooSoon;
      continue;
    }
    mtSetClock(decision.candidateEpochMs);
    sMeshTime.follower = trial;
    break;
  }

  MeshTimeLast previous;
  MESH_TIME_LOCK();
  previous = sMeshTime.last;
  sMeshTime.last.have = true;
  memcpy(sMeshTime.last.mac, stash.rx.sender, 6);
  sMeshTime.last.source = stash.rx.frame.source;
  sMeshTime.last.sinceSetMin = stash.rx.frame.sinceSetMin;
  sMeshTime.last.verdict = decision.verdict;
  sMeshTime.last.differenceMs = decision.differenceMs;
  sMeshTime.last.atMs = mtUptimeMs();
  sMeshTime.counts[(size_t)decision.verdict]++;
  MESH_TIME_UNLOCK();

  char name[32];
  mtPeerName(stash.rx.sender, name, sizeof(name));
  if (decision.verdict == meshtime::Verdict::Applied) {
    char when[48];
    mtFormatLocal(decision.candidateEpochMs, when, sizeof(when));
    mtConsole("[ESP-NOW] Clock set from mesh time: %s (from %s, its source: %s)", when, name,
              meshtime::wireSourceName(stash.rx.frame.source));
  } else if (decision.verdict == meshtime::Verdict::HeldLargeChange &&
             !(previous.have && previous.verdict == meshtime::Verdict::HeldLargeChange &&
               memcmp(previous.mac, stash.rx.sender, 6) == 0)) {
    // Once per new claim, and one line a minute at most: frames that take
    // turns with other times cannot fill the console.
    const uint32_t nowMs = mtUptimeMs();
    if (!sMeshTime.heldLineHave || (uint32_t)(nowMs - sMeshTime.heldLineMs) >= 60000) {
      sMeshTime.heldLineHave = true;
      sMeshTime.heldLineMs = nowMs;
      mtConsole("[ESP-NOW] Mesh time from %s differs from this device's clock by more than an hour (%+lld s): "
                "held until %s says it again",
                name, (long long)(decision.differenceMs / 1000), name);
    }
  }
}

void espnowMeshTimeApplyTick() {
  MeshTimeStash kept[kMeshTimeStashSlots];
  MESH_TIME_LOCK();
  for (uint8_t i = 0; i < kMeshTimeStashSlots; ++i) {
    kept[i] = sMeshTime.stash[i];
    sMeshTime.stash[i].pending = false;
  }
  MESH_TIME_UNLOCK();
  // In the order they arrived.
  for (;;) {
    MeshTimeStash* next = nullptr;
    for (auto& e : kept) {
      if (e.pending && (!next || e.rx.receiptMonoMs < next->rx.receiptMonoMs)) next = &e;
    }
    if (!next) break;
    next->pending = false;
    meshTimeJudge(*next);
  }
}

// ---- Send (espnow_task) --------------------------------------------------------

static meshtime::SenderInputs meshTimeSenderInputs() {
  meshtime::SenderInputs in = {};
  in.settingOn = mtSettingOn();
  in.meshOn = mtMeshOn();
  in.actingMaster = mtRole() == 1;
  in.clockSet = mtClockSet();
  in.clockHeld = in.clockSet && mtClockHeld();
  in.source = mtOwnSource();
  return in;
}

// The send state of `mac`. A slot of a device that is no longer paired is
// taken when none is free; nullptr when all 16 belong to paired devices.
static meshtime::PeerSendState* meshTimePeerSlot(const uint8_t mac[6]) {
  meshtime::PeerSendState* freeSlot = nullptr;
  for (auto& p : sMeshTime.peers) {
    if (p.inUse && memcmp(p.mac, mac, 6) == 0) return &p;
    if (!p.inUse && !freeSlot) freeSlot = &p;
  }
  if (!freeSlot) {
    for (auto& p : sMeshTime.peers) {
      if (!mtIsPaired(p.mac)) { freeSlot = &p; break; }
    }
  }
  if (!freeSlot) return nullptr;
  memset(freeSlot, 0, sizeof(*freeSlot));
  freeSlot->inUse = true;
  memcpy(freeSlot->mac, mac, 6);
  return freeSlot;
}

void espnowMeshTimeSendTick(uint32_t nowMs) {
  if ((uint32_t)(nowMs - sMeshTime.lastLookMs) < meshtime::kPeerGapMs) return;
  sMeshTime.lastLookMs = nowMs;

  bool asked;
  MESH_TIME_LOCK();
  asked = sMeshTime.roundAsked;
  sMeshTime.roundAsked = false;
  MESH_TIME_UNLOCK();

  const meshtime::SenderInputs in = meshTimeSenderInputs();
  if (!in.meshOn || !in.actingMaster) return;
  const bool sends = meshtime::senderDecide(in) == meshtime::SendWhyNot::Sends;
  if (sends && meshtime::roundBegin(sMeshTime.round, nowMs, mtStepSerial(), asked)) {
    MESH_TIME_LOCK();
    sMeshTime.roundReached = 0;
    MESH_TIME_UNLOCK();
  }

  // One frame and at most one handshake start per look.
  bool handshakeStarted = false;
  const uint8_t slots = mtPeerSlots();
  for (uint8_t i = 0; i < slots; ++i) {
    uint8_t mac[6];
    if (!mtPeerAt(i, mac)) continue;
    uint32_t establishedAtMs = 0;
    if (!mtSession(mac, &establishedAtMs)) {
      // The master is the receiving end of readings, battery state and
      // events, and a device reached only through a relay gets no session
      // from the heartbeat. So the master starts one, with the usual
      // back-off, whether or not it sends time.
      if (!handshakeStarted) handshakeStarted = mtHandshake(mac);
      continue;
    }
    if (!sends) continue;
    meshtime::PeerSendState* peer = meshTimePeerSlot(mac);
    if (!peer || !meshtime::peerDue(*peer, sMeshTime.round, true, establishedAtMs, nowMs)) continue;

    meshtime::Frame frame = {};
    frame.epochMs = mtEpochMs();
    frame.source = meshtime::wireSourceFor(in.source);
    frame.flags = mtBackupPromoted() ? meshtime::kFlagActingBackup : 0;
    uint32_t agoSeconds = 0;
    frame.sinceSetMin = mtLastSetAgo(&agoSeconds) ? meshtime::sinceSetMinutes(agoSeconds) : (uint16_t)65535;
    uint8_t payload[meshtime::kFrameLen];
    meshtime::packFrame(payload, frame);
    if (!mtSend(mac, payload, (uint16_t)sizeof(payload))) return;  // queue full: the next look tries again
    const bool inRound = peer->roundSerialSent != sMeshTime.round.serial;
    meshtime::peerSent(*peer, sMeshTime.round, establishedAtMs, nowMs);
    MESH_TIME_LOCK();
    sMeshTime.everSent = true;
    sMeshTime.lastSentMs = nowMs;
    sMeshTime.framesSent++;
    if (inRound && sMeshTime.roundReached < 0xFF) sMeshTime.roundReached++;
    MESH_TIME_UNLOCK();
    return;
  }
}

// ---- Commands (cmd_exec) ---------------------------------------------------------

static const char* meshTimeRoleName(uint8_t role) {
  return role == 1 ? "master" : (role == 2 ? "backup master" : "worker");
}

// Where this device's clock comes from, in words.
static const char* meshTimeOwnSourceText(bool clockSet, meshtime::OwnSource source) {
  if (!clockSet) return "not set";
  switch (source) {
    case meshtime::OwnSource::None:   return "carried over a restart";
    case meshtime::OwnSource::Rtc:    return "rtc";
    case meshtime::OwnSource::Ntp:    return "ntp";
    case meshtime::OwnSource::Manual: return "manual";
    case meshtime::OwnSource::Ring:   return "ring";
    case meshtime::OwnSource::Cm5:    return "cm5";
    case meshtime::OwnSource::Mesh:   return "mesh";
  }
  return "?";
}

// The same as a JSON value: "none" while not set, "carried" for a clock that
// no source has set since boot.
static const char* meshTimeOwnSourceKey(bool clockSet, meshtime::OwnSource source) {
  if (!clockSet) return "none";
  return source == meshtime::OwnSource::None ? "carried" : meshTimeOwnSourceText(true, source);
}

// Paired devices, and how many of them have a session a frame can use now.
static void meshTimeCountPeers(uint32_t nowMs, unsigned* paired, unsigned* ready) {
  *paired = 0;
  *ready = 0;
  const uint8_t slots = mtPeerSlots();
  for (uint8_t i = 0; i < slots; ++i) {
    uint8_t mac[6];
    if (!mtPeerAt(i, mac)) continue;
    (*paired)++;
    uint32_t establishedAtMs = 0;
    if (mtSession(mac, &establishedAtMs) && meshtime::sessionSettled(establishedAtMs, nowMs)) (*ready)++;
  }
}

const char* cmd_espnow_timesync(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  size_t cap = 0;
  char* reply = mtReply(&cap);
  if (!reply) return kCommandReplyOutOfMemory;

  const meshtime::SenderInputs in = meshTimeSenderInputs();
  switch (meshtime::senderDecide(in)) {
    case meshtime::SendWhyNot::Sends:
      break;
    case meshtime::SendWhyNot::Off:
      return "Error: mesh time is switched off (espnowmeshtime on).";
    case meshtime::SendWhyNot::MeshOff:
      return "Error: Mesh mode not enabled. Use 'espnowmode mesh' first.";
    case meshtime::SendWhyNot::NotMaster:
      snprintf(reply, cap, "Error: only the mesh master sends time. This device's role is %s.",
               meshTimeRoleName(mtRole()));
      return reply;
    case meshtime::SendWhyNot::ClockNotSet:
      return "Error: this device's clock is not set. Nothing sent.";
    case meshtime::SendWhyNot::SourceNotReal:
      snprintf(reply, cap,
               "Error: this device's clock is %s, which is not a real source (rtc, ntp, manual, cm5). "
               "Nothing sent.",
               in.source == meshtime::OwnSource::None ? "carried over a restart"
                                                      : meshTimeOwnSourceText(true, in.source));
      return reply;
    case meshtime::SendWhyNot::ClockUnconfirmed:
      return "Error: an NTP reply moved this device's clock by more than a day and nothing has "
             "confirmed it yet. Nothing sent.";
  }

  unsigned paired = 0, ready = 0;
  meshTimeCountPeers(mtUptimeMs(), &paired, &ready);
  MESH_TIME_LOCK();
  sMeshTime.roundAsked = true;
  MESH_TIME_UNLOCK();
  if (paired == 0) return "OK: no paired devices to send time to.";
  if (ready == paired) {
    snprintf(reply, cap, "OK: time is being sent to %u of %u paired devices", ready, paired);
  } else {
    snprintf(reply, cap, "OK: time is being sent to %u of %u paired devices (%u %s no session yet)", ready,
             paired, paired - ready, (paired - ready) == 1 ? "has" : "have");
  }
  return reply;
}

// Appends to the reply; false once it is full.
static bool meshTimeAppend(char* reply, size_t cap, size_t* pos, const char* fmt, ...) {
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

// `name` as the inside of a JSON string: quotes, backslashes and control
// bytes cannot end the string early.
static void meshTimeJsonText(const char* name, char* out, size_t cap) {
  size_t pos = 0;
  for (const char* p = name; *p && pos + 2 < cap; ++p) {
    const unsigned char ch = (unsigned char)*p;
    if (ch == '"' || ch == '\\') { out[pos++] = '\\'; out[pos++] = (char)ch; }
    else if (ch < 0x20 || ch == 0x7F) out[pos++] = '?';
    else out[pos++] = (char)ch;
  }
  out[pos] = '\0';
}

const char* cmd_espnow_timestatus(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  size_t cap = 0;
  char* reply = mtReply(&cap);
  if (!reply) return kCommandReplyOutOfMemory;

  const uint32_t nowMs = mtUptimeMs();
  const meshtime::SenderInputs in = meshTimeSenderInputs();
  const meshtime::SendWhyNot why = meshtime::senderDecide(in);
  const uint8_t role = mtRole();
  uint32_t setAgoS = 0;
  const bool haveSetAgo = mtLastSetAgo(&setAgoS);
  unsigned paired = 0, ready = 0;
  meshTimeCountPeers(nowMs, &paired, &ready);
  const bool masterNamed = mtHasNamedMaster();

  MeshTimeLast last;
  uint32_t counts[meshtime::kVerdictCount];
  bool everSent;
  uint32_t lastSentMs, framesSent, shortFrames, droppedFrames;
  uint8_t roundReached;
  MESH_TIME_LOCK();
  last = sMeshTime.last;
  memcpy(counts, sMeshTime.counts, sizeof(counts));
  everSent = sMeshTime.everSent;
  lastSentMs = sMeshTime.lastSentMs;
  framesSent = sMeshTime.framesSent;
  shortFrames = sMeshTime.shortFrames;
  droppedFrames = sMeshTime.droppedFrames;
  roundReached = sMeshTime.roundReached;
  MESH_TIME_UNLOCK();

  uint32_t ignored = droppedFrames;
  for (size_t i = 0; i < meshtime::kVerdictCount; ++i) {
    if (meshtime::verdictIsIgnored((meshtime::Verdict)i)) ignored += counts[i];
  }
  const uint32_t applied = counts[(size_t)meshtime::Verdict::Applied];
  const uint32_t agreed = counts[(size_t)meshtime::Verdict::Agreed];
  const uint32_t held = counts[(size_t)meshtime::Verdict::HeldLargeChange];
  const uint32_t refused = counts[(size_t)meshtime::Verdict::RefusedBadTime] + shortFrames;

  char lastName[32] = "";
  if (last.have) mtPeerName(last.mac, lastName, sizeof(lastName));
  size_t pos = 0;

  if (mtWantsJson(argsInput)) {
    char nameJson[72];
    meshTimeJsonText(lastName, nameJson, sizeof(nameJson));
    const char* whyKey = meshtime::sendWhyNotKey(why);
    meshTimeAppend(reply, cap, &pos,
                   "{\"schema\":1,\"meshTime\":%s,\"role\":\"%s\",\"masterNamed\":%s,\"clockSet\":%s,"
                   "\"clockSource\":\"%s\"",
                   in.settingOn ? "true" : "false", role == 1 ? "master" : (role == 2 ? "backup" : "worker"),
                   masterNamed ? "true" : "false", in.clockSet ? "true" : "false",
                   meshTimeOwnSourceKey(in.clockSet, in.source));
    if (haveSetAgo) meshTimeAppend(reply, cap, &pos, ",\"clockSetAgoS\":%lu", (unsigned long)setAgoS);
    else meshTimeAppend(reply, cap, &pos, ",\"clockSetAgoS\":null");
    if (in.clockSet) meshTimeAppend(reply, cap, &pos, ",\"epoch\":%lld", (long long)(mtEpochMs() / 1000));
    else meshTimeAppend(reply, cap, &pos, ",\"epoch\":null");
    meshTimeAppend(reply, cap, &pos, ",\"sends\":%s", whyKey ? "false" : "true");
    if (whyKey) meshTimeAppend(reply, cap, &pos, ",\"sendsWhyNot\":\"%s\"", whyKey);
    else meshTimeAppend(reply, cap, &pos, ",\"sendsWhyNot\":null");
    if (everSent) meshTimeAppend(reply, cap, &pos, ",\"lastSentAgoS\":%lu", (unsigned long)((nowMs - lastSentMs) / 1000));
    else meshTimeAppend(reply, cap, &pos, ",\"lastSentAgoS\":null");
    meshTimeAppend(reply, cap, &pos, ",\"framesSent\":%lu,\"peersSent\":%u,\"peersReady\":%u,\"peersPaired\":%u",
                   (unsigned long)framesSent, (unsigned)roundReached, ready, paired);
    if (last.have) {
      char mac[18];
      macToDisplay(last.mac, mac, sizeof(mac));
      meshTimeAppend(reply, cap, &pos,
                     ",\"lastFrame\":{\"mac\":\"%s\",\"name\":\"%s\",\"agoS\":%lu,\"source\":\"%s\","
                     "\"sinceSetMin\":%u,\"result\":\"%s\",\"differenceS\":%lld}",
                     mac, nameJson, (unsigned long)((nowMs - last.atMs) / 1000),
                     meshtime::wireSourceName(last.source), (unsigned)last.sinceSetMin,
                     meshtime::verdictKey(last.verdict), (long long)(last.differenceMs / 1000));
    } else {
      meshTimeAppend(reply, cap, &pos, ",\"lastFrame\":null");
    }
    meshTimeAppend(reply, cap, &pos,
                   ",\"counts\":{\"applied\":%lu,\"agreed\":%lu,\"held\":%lu,\"ignored\":%lu,\"refused\":%lu}}",
                   (unsigned long)applied, (unsigned long)agreed, (unsigned long)held, (unsigned long)ignored,
                   (unsigned long)refused);
    return reply;
  }

  meshTimeAppend(reply, cap, &pos, "Mesh time\n");
  if (!in.clockSet) {
    meshTimeAppend(reply, cap, &pos, "  This device's clock: not set\n");
  } else {
    char when[48];
    mtFormatLocal(mtEpochMs(), when, sizeof(when));
    if (in.source == meshtime::OwnSource::None) {
      meshTimeAppend(reply, cap, &pos,
                     "  This device's clock: set, carried over a restart (no source has set it since) (%s)\n", when);
    } else {
      meshTimeAppend(reply, cap, &pos, "  This device's clock: set, from %s, last set %lu s ago (%s)\n",
                     meshTimeOwnSourceText(true, in.source), (unsigned long)setAgoS, when);
    }
  }
  if (!in.settingOn) {
    meshTimeAppend(reply, cap, &pos, "  Mesh time is switched off (espnowmeshtime on).\n");
  }
  if (role == 1 && in.meshOn) {
    switch (why) {
      case meshtime::SendWhyNot::Sends:
        meshTimeAppend(reply, cap, &pos,
                       "  Role: master. Sends its clock to paired devices when their session starts and every 10 min.\n");
        break;
      case meshtime::SendWhyNot::ClockNotSet:
        meshTimeAppend(reply, cap, &pos, "  Role: master. Not sending: this device's clock is not set.\n");
        break;
      case meshtime::SendWhyNot::SourceNotReal:
        meshTimeAppend(reply, cap, &pos,
                       "  Role: master. Not sending: this device's clock is not set from a real source "
                       "(rtc, ntp, manual, cm5); it is %s.\n",
                       in.source == meshtime::OwnSource::None ? "carried over a restart"
                                                              : meshTimeOwnSourceText(true, in.source));
        break;
      case meshtime::SendWhyNot::ClockUnconfirmed:
        meshTimeAppend(reply, cap, &pos,
                       "  Role: master. Not sending: an NTP reply moved this device's clock by more than a day "
                       "and nothing has confirmed it yet.\n");
        break;
      default:
        meshTimeAppend(reply, cap, &pos, "  Role: master. Not sending.\n");
        break;
    }
    if (everSent) {
      meshTimeAppend(reply, cap, &pos,
                     "  Last sent: %lu s ago. This round has reached %u of %u paired devices (%u with a session).\n",
                     (unsigned long)((nowMs - lastSentMs) / 1000), (unsigned)roundReached, paired, ready);
    } else {
      meshTimeAppend(reply, cap, &pos, "  Nothing sent yet (%u paired devices, %u with a session).\n", paired, ready);
    }
  } else if (!in.meshOn) {
    meshTimeAppend(reply, cap, &pos, "  Mesh mode is off: no time is sent or taken.\n");
  } else if (!masterNamed) {
    meshTimeAppend(reply, cap, &pos,
                   "  Role: %s. Takes no mesh time: no master is named on this device. "
                   "Name it with 'espnowmasterfingerprint <64-hex-pubkey>'.\n",
                   meshTimeRoleName(role));
  } else {
    meshTimeAppend(reply, cap, &pos,
                   "  Role: %s. Takes time from the master named on this device when it has no better source "
                   "(rtc, ntp, manual, cm5).\n",
                   meshTimeRoleName(role));
  }

  if (!last.have) {
    if (role != 1) meshTimeAppend(reply, cap, &pos, "  No time frame received yet.\n");
  } else {
    meshTimeAppend(reply, cap, &pos, "  Last time frame: %lu s ago from %s (its source: %s). ",
                   (unsigned long)((nowMs - last.atMs) / 1000), lastName, meshtime::wireSourceName(last.source));
    switch (last.verdict) {
      case meshtime::Verdict::Applied:
        meshTimeAppend(reply, cap, &pos, "Applied.\n");
        break;
      case meshtime::Verdict::Agreed:
        meshTimeAppend(reply, cap, &pos, "It agrees with this device's clock; nothing was changed.\n");
        break;
      case meshtime::Verdict::IgnoredOff:
        meshTimeAppend(reply, cap, &pos, "Not used: mesh time is switched off.\n");
        break;
      case meshtime::Verdict::IgnoredMeshOff:
        meshTimeAppend(reply, cap, &pos, "Not used: mesh mode is off.\n");
        break;
      case meshtime::Verdict::IgnoredNoMaster:
        meshTimeAppend(reply, cap, &pos, "Not used: no master is named on this device (espnowmasterfingerprint).\n");
        break;
      case meshtime::Verdict::IgnoredNotNamed:
        meshTimeAppend(reply, cap, &pos, "Not used: %s is not the master named on this device.\n", lastName);
        break;
      case meshtime::Verdict::IgnoredMaster:
        meshTimeAppend(reply, cap, &pos, "Not used: this device is the mesh master.\n");
        break;
      case meshtime::Verdict::IgnoredBetterSource:
        meshTimeAppend(reply, cap, &pos, "Not used: this device has a better source (%s).\n",
                       meshTimeOwnSourceText(in.clockSet, in.source));
        break;
      case meshtime::Verdict::IgnoredWeakSource:
        meshTimeAppend(reply, cap, &pos,
                       "Not used: the sender's clock has no real source, and this device's clock is set.\n");
        break;
      case meshtime::Verdict::IgnoredOtherMaster:
        meshTimeAppend(reply, cap, &pos, "Not used: this device follows the time of another sender.\n");
        break;
      case meshtime::Verdict::IgnoredTooSoon:
        meshTimeAppend(reply, cap, &pos, "Not used: this device's clock was set a moment before.\n");
        break;
      case meshtime::Verdict::RefusedBadTime:
        meshTimeAppend(reply, cap, &pos, "Refused: the time in the frame is not a real date.\n");
        break;
      case meshtime::Verdict::HeldLargeChange:
        meshTimeAppend(reply, cap, &pos,
                       "Held: it differs from this device's clock by more than an hour (%+lld s). "
                       "It is applied when %s says it again.\n",
                       (long long)(last.differenceMs / 1000), lastName);
        break;
      case meshtime::Verdict::Count:
        break;
    }
  }
  meshTimeAppend(reply, cap, &pos, "  Frames: %lu applied, %lu agreed, %lu held, %lu not used, %lu refused",
                 (unsigned long)applied, (unsigned long)agreed, (unsigned long)held, (unsigned long)ignored,
                 (unsigned long)refused);
  return reply;
}

// MESH_TIME_HOST_END

void v4hTimeSync(const V4RxCtx& ctx) {
  meshTimeKeepFrame(ctx.recv_info->src_addr, ctx.payload, ctx.payloadLen);
}

#endif  // ENABLE_ESPNOW
