// G2 QuickList control rows: wear detection, silent mode, auto brightness,
// head-up display and brightness.
// See G2_QuickList.h.
#include "G2_QuickList.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_G2_QUICKLIST

#include <Arduino.h>
#include <atomic>
#include <stdarg.h>
#include <freertos/FreeRTOS.h>

#include "G2_Protocol.h"
#include "System_CommandLimits.h"   // CMD_RESULT_MAX
#include "System_Debug.h"
#include "System_Settings.h"
#include "System_Utils.h"

namespace ql = hw1_g2_quicklist;

namespace {

constexpr uint32_t kRetryMs = 1000;        // after a refused submit, a failed send or a missing ACK
constexpr uint32_t kJobLapMs = 500;        // owner lap cap while a job runs
constexpr uint32_t kPendingLapMs = 200;    // owner lap cap while an action waits
constexpr uint32_t kMinLapMs = 50;
constexpr uint8_t kUnknownReadTriesMax = 3; // reads that prove nothing about the firmware
constexpr size_t kRxHexBytes = 48;         // debug line: first 48 payload bytes
constexpr uint8_t kSilent = (uint8_t)ql::Control::Silent;
constexpr uint8_t kAutoBright = (uint8_t)ql::Control::AutoBright;
constexpr uint8_t kHeadUp = (uint8_t)ql::Control::HeadUp;
constexpr uint8_t kBrightness = (uint8_t)ql::Control::Brightness;

enum class Phase : uint8_t { Down, Settling, Idle, Unsupported };

bool feedOn() { return gSettings.g2QuickListFeed; }

// Status published for the commands (owner writes, commands copy).
struct Pub {
  Phase phase;
  ql::Shown shown[ql::kRowCount], next[ql::kRowCount];
  uint8_t shownLevel[ql::kRowCount], nextLevel[ql::kRowCount];   // the Brightness row's level
  bool rowSent[ql::kRowCount], rowPending[ql::kRowCount], jobInFlight, manualThisLink;
  // FULL_UPDATEs (G2_QuickListCore.h kPushAckWaitMs): every push sends all five rows.
  uint32_t fullSent, fullSendFails, fullAcks, otherAcks, fullRetries, ackTimeouts, fullGiveUps;
  bool havePush, lastAcked, awaitingAck;
  bool staleLens;                      // a push was given up; the re-read schedule sends the list again
  ql::PushReason lastReason;           // the last FULL_UPDATE's reason
  uint8_t lastMagic;
  uint16_t lastBytes;
  uint32_t lastSentAtMs, lastAckAfterMs;   // when the last one went out; its ACK's delay
  uint32_t ticksOurs, dupes, foreign, notCompleted, disabled, busy, rateLimited, otherReports;
  bool haveTick;
  uint32_t lastTickMs, lastTickMagic, lastTickUid;
  ql::TickAction lastTickAction;
  uint32_t reads;
  uint32_t applies[ql::kRowCount];
  bool haveApply[ql::kRowCount], lastApplyTarget[ql::kRowCount], lastApplyAcked[ql::kRowCount];
  bool lastApplyKnown[ql::kRowCount], lastApplyOn[ql::kRowCount];
  ql::WearStage lastApplyStage[ql::kRowCount];
  ql::Recheck lastApplyRecheck[ql::kRowCount];   // whether the last write's read-back needed its recheck
  // The last Brightness write: the step chosen, whether auto brightness was on
  // and its OFF write was sent / ACKed, whether the level was written, and the
  // read-back's level and auto brightness.
  uint8_t lastBrightTarget, lastBrightLevel;
  bool lastBrightAutoWasOn, lastBrightAutoWritten, lastBrightAutoAcked, lastBrightLevelWritten;
  bool lastBrightLevelKnown, lastBrightAutoKnown, lastBrightAutoOn;
  uint32_t silentReadbacks, silentPushes, silentPushDupes, silentPushStale;
  uint32_t autoBrightReadbacks, headUpReadbacks, brightnessReadbacks;
  uint32_t submitFails, jobTimeouts, rxStale;
  // Settings reads made by jobs (read jobs, pre-reads, read-backs).
  uint32_t settingsReads, readsUnanswered, readsResent;
  bool haveLastRead;
  uint8_t lastReadKind;           // ReadKind
  uint32_t lastReadAtMs;
  ql::ReadDiag lastRead;
  // Re-reads after "?" (the policy's schedule, copied for the status).
  uint32_t recoveryReads;
  ql::Recovery recovery;
  bool recoveryBlocked;
};

// Which of a job's settings reads a ReadDiag describes.
// Recheck: a write's second read-back (G2_QuickListCore.h kReadbackRecheckMs).
enum class ReadKind : uint8_t { Read, PreRead, ReadBack, Recheck };
const char* readKindName(ReadKind k) {
  switch (k) {
    case ReadKind::Read:     return "read";
    case ReadKind::PreRead:  return "pre-read";
    case ReadKind::ReadBack: return "read-back";
    case ReadKind::Recheck:  return "read-back recheck";
  }
  return "?";
}

// One job on cmd_exec at a time (owner <-> cmd_exec, under sJobMux).
struct Job {
  uint32_t token, rightGen, submittedMs;
  uint8_t kind;     // 1 read (every row), 2 apply
  uint8_t control;  // apply: the row being written (ql::Control)
  bool target, started, done;  // started: the job passed its checks and talks to the glasses
  bool background;  // read: the re-read after "?" (afterRecoveryRead, no tick rate limit)
  ql::WearResult result;       // apply
  ql::ReadResult read;         // read
};
constexpr uint8_t kJobRead = 1;
constexpr uint8_t kJobApply = 2;

}  // namespace

// ---- owner-only state ------------------------------------------------------
EXT_RAM_BSS_ATTR static ql::Policy sPolicy;            // owner only
EXT_RAM_BSS_ATTR static uint8_t sOwnerPb[ql::kPbMax];  // owner only (one FULL_UPDATE, about 190 B)
EXT_RAM_BSS_ATTR static ql::Title sOwnerTitles[ql::kRowCount];   // owner only
EXT_RAM_BSS_ATTR static char sRxHex[2 * kRxHexBytes + 1];  // owner only (debug line)
EXT_RAM_BSS_ATTR static ql::TickReport sRep;           // owner only
EXT_RAM_BSS_ATTR static Pub sOwn;                      // owner's working copy of the status
// ---- command task ----------------------------------------------------------
EXT_RAM_BSS_ATTR static uint8_t sCmdPb[ql::kPbMax];    // command task (preview)
EXT_RAM_BSS_ATTR static char sCmdHex[2 * ql::kPbMax + 1];
EXT_RAM_BSS_ATTR static ql::Title sCmdTitles[ql::kRowCount];   // command task (preview)
// Command task replies (PSRAM). `g2 list status` with five rows runs past the
// old 2 KB (host-measured with every counter at 10 digits and the longest
// texts: see full-update-implement.md), so the reply buffer is the executor's
// whole result size. `g2 list preview` is about 0.9 KB (the ~190 B FULL_UPDATE
// as hex, at most 524 characters).
EXT_RAM_BSS_ATTR static char sReply[CMD_RESULT_MAX];

// owner-only scalars
static Phase sPhase = Phase::Down;
static uint32_t sKeyL = 0, sKeyR = 0, sSettleStartMs = 0, sJobSubmittedMs = 0;
static bool sRetryArmed = false;
static uint32_t sRetryFromMs = 0;
static ql::After sAfter = ql::After::None;
static bool sManualThisLink = false, sAutoDone = false;
static ql::Decision sPendingTick{};
static bool sTickPending = false;
static ql::HeldChange sSilentChange{};   // newest silent change push, applied once no job runs
static uint32_t sJobSerial = 0;
static ql::PushReason sReadReason = ql::PushReason::None;   // what the read job's push is for
static uint8_t sUnknownReadRun = 0;
static bool sBlockedByBusy = false;   // last lap: a Push waited only for a voice/QuickNote session
static bool sRecoveryBlocked = false; // last lap: a due re-read waited for the feed, the pair or a session

// cross-task. A `g2 list push` request carries the right temple's generation
// it was made on (0 = none), so it never carries over to another connection.
static std::atomic<uint32_t> sPushRequestGen{0};
static std::atomic<bool> sResync{false};
static std::atomic<uint8_t> sLastMagic{0};
// sNextShown: 2 bits per row's next title, row i at bits 2i..2i+1, and the
// Brightness row's next level at bits 16..23 (preview).
static std::atomic<uint32_t> sNextShown{0};
static_assert(2 * ql::kRowCount <= 16, "the packed next titles fit below the level byte");

// job slot (owner <-> cmd_exec)
static Job sJob{};
static portMUX_TYPE sJobMux = portMUX_INITIALIZER_UNLOCKED;

// published status
EXT_RAM_BSS_ATTR static Pub sPub;
static portMUX_TYPE sPubMux = portMUX_INITIALIZER_UNLOCKED;

namespace {

void publish() {
  sOwn.phase = sPhase;
  for (uint8_t i = 0; i < ql::kRowCount; ++i) {
    sOwn.shown[i] = sPolicy.row[i].shown;
    sOwn.next[i] = sPolicy.row[i].next;
    sOwn.shownLevel[i] = sPolicy.row[i].shownLevel;
    sOwn.nextLevel[i] = sPolicy.row[i].nextLevel;
    sOwn.rowSent[i] = sPolicy.row[i].magic != 0;
    sOwn.rowPending[i] = sPolicy.row[i].pending;
  }
  sOwn.jobInFlight = sPolicy.jobInFlight;
  sOwn.manualThisLink = sManualThisLink;
  sOwn.recovery = sPolicy.recovery;
  sOwn.recoveryBlocked = sRecoveryBlocked;
  sOwn.staleLens = sPolicy.staleLens;
  sOwn.awaitingAck = sPolicy.push.awaitingAck;
  portENTER_CRITICAL(&sPubMux);
  sPub = sOwn;
  portEXIT_CRITICAL(&sPubMux);
  uint32_t packed = (uint32_t)sPolicy.row[kBrightness].nextLevel << 16;
  for (uint8_t i = 0; i < ql::kRowCount; ++i) packed |= (uint32_t)((uint8_t)sPolicy.row[i].next & 3) << (2 * i);
  sNextShown.store(packed, std::memory_order_relaxed);
}

Pub snapshot() {
  Pub p;
  portENTER_CRITICAL(&sPubMux);
  p = sPub;
  portEXIT_CRITICAL(&sPubMux);
  return p;
}

// Wrap-safe retry gate: a refused submit, a failed send or a missing ACK waits kRetryMs.
void armRetry(uint32_t now) { sRetryArmed = true; sRetryFromMs = now; }
bool retryReady(uint32_t now) {
  if (!sRetryArmed) return true;
  if ((uint32_t)(now - sRetryFromMs) < kRetryMs) return false;
  sRetryArmed = false;
  return true;
}

// Owner state for a new or lost link.
void resetLink() {
  ql::linkReset(sPolicy);
  sPhase = Phase::Down;
  sAfter = ql::After::None;
  sTickPending = false;
  sSilentChange = ql::HeldChange{};
  sManualThisLink = false;
  sAutoDone = false;
  sRetryArmed = false;
  sReadReason = ql::PushReason::None;
  sUnknownReadRun = 0;
  sBlockedByBusy = false;
  sRecoveryBlocked = false;   // the re-read schedule went with linkReset
  // A job still queued finds its token gone and exits without touching the glasses.
  portENTER_CRITICAL(&sJobMux);
  sJob.token = 0;
  portEXIT_CRITICAL(&sJobMux);
}

// cmd_exec: a settings read that was made and got no correlated reply.
void logUnansweredRead(ReadKind kind, const ql::ReadDiag& d) {
  if (!ql::readMade(d) || ql::readAnswered(d)) return;
  DEBUG_G2F("[G2-LIST] settings %s unanswered: %u attempt(s) in %lu ms; %u other replies, "
            "%u other settings frames during the wait",
            readKindName(kind), (unsigned)d.attempts, (unsigned long)d.totalMs, (unsigned)d.otherReplies,
            (unsigned)d.otherFrames);
}

// Owner: counts one job's settings read for `g2 list status`.
void noteRead(ReadKind kind, const ql::ReadDiag& d, uint32_t now) {
  if (!ql::readMade(d)) return;
  sOwn.settingsReads++;
  if (!ql::readAnswered(d)) sOwn.readsUnanswered++;
  else if (ql::readNeededResend(d)) sOwn.readsResent++;
  sOwn.haveLastRead = true;
  sOwn.lastReadKind = (uint8_t)kind;
  sOwn.lastReadAtMs = now;
  sOwn.lastRead = d;
}

// ---- cmd_exec job (the only blocking code) ---------------------------------
void quickListJob(void* arg) {                 // runs on cmd_exec
  const uint32_t token = (uint32_t)(uintptr_t)arg;
  const uint32_t startMs = millis();
  Job j;
  portENTER_CRITICAL(&sJobMux);
  const bool mine = sJob.token == token && !sJob.done;
  // The start deadline keeps an old tick from being applied. A read is harmless
  // late (a long console read on cmd_exec can hold it), so it still runs.
  const bool late = sJob.kind != kJobRead &&
                    (uint32_t)(startMs - sJob.submittedMs) > ql::kJobStartDeadlineMs;
  if (mine && !late) sJob.started = true;      // from here the owner waits for the result
  j = sJob;
  portEXIT_CRITICAL(&sJobMux);
  if (!mine) return;                           // superseded or cancelled
  ql::WearResult r{};
  ql::ReadResult rd{};
  if (late) {
    r.stage = ql::WearStage::Stale;
  } else if (j.kind == kJobRead) {
    (void)g2QuickListSettingsRead(j.rightGen, &rd);
    r.stage = ql::WearStage::Done;
  } else if (j.control == kSilent) {
    (void)g2QuickListSilentWrite(j.rightGen, j.target, &r);
  } else if (j.control == kAutoBright) {
    (void)g2QuickListAutoBrightWrite(j.rightGen, j.target, &r);
  } else if (j.control == kHeadUp) {
    (void)g2QuickListHeadUpWrite(j.rightGen, j.target, &r);
  } else if (j.control == kBrightness) {
    (void)g2QuickListBrightnessWrite(j.rightGen, &r);   // both writes of one tick in this one job
  } else {
    (void)g2QuickListWearWrite(j.rightGen, j.target, &r);
  }
  // One line per unanswered settings read, here on cmd_exec (not on g2_owner).
  logUnansweredRead(ReadKind::Read, rd.diag);
  logUnansweredRead(ReadKind::PreRead, r.preRead);
  logUnansweredRead(ReadKind::ReadBack, r.readBack);
  logUnansweredRead(ReadKind::Recheck, r.recheck);
  portENTER_CRITICAL(&sJobMux);
  if (sJob.token == token) { sJob.result = r; sJob.read = rd; sJob.done = true; }
  portEXIT_CRITICAL(&sJobMux);
  g2QuickListWake();
}

const char* jobName(uint8_t kind, uint8_t control) {
  if (kind == kJobRead) return "read";
  if (control == kSilent) return "silent write";
  if (control == kAutoBright) return "auto-brightness write";
  if (control == kHeadUp) return "head-up write";
  if (control == kBrightness) return "brightness write";
  return "wear write";
}

// background: the re-read after "?" (does not use up the tick rate limit).
bool submitJob(uint8_t kind, ql::Control control, bool target, uint32_t rightGen, uint32_t now,
               bool background = false) {
  uint32_t token = ++sJobSerial;
  if (!token) token = ++sJobSerial;
  portENTER_CRITICAL(&sJobMux);
  sJob = Job{};
  sJob.token = token;
  sJob.rightGen = rightGen;
  sJob.submittedMs = now;
  sJob.kind = kind;
  sJob.control = (uint8_t)control;
  sJob.target = target;
  sJob.background = background;
  sJob.started = false;
  sJob.done = false;
  portEXIT_CRITICAL(&sJobMux);
  if (!submitDeferredToCmdExec(quickListJob, (void*)(uintptr_t)token)) {
    portENTER_CRITICAL(&sJobMux);
    if (sJob.token == token) sJob.token = 0;
    portEXIT_CRITICAL(&sJobMux);
    sOwn.submitFails++;
    armRetry(now);
    DEBUG_G2F("[G2-LIST] cmd_exec queue full: %s retried in 1 s", jobName(kind, (uint8_t)control));
    return false;
  }
  if (background) ql::noteBackgroundJobStarted(sPolicy);
  else ql::noteJobStarted(sPolicy, now);
  sJobSubmittedMs = now;
  if (kind == kJobRead) sOwn.reads++;
  else sOwn.applies[ql::rowSlot(control)]++;
  return true;
}

// A send failed or an ACK did not come: the list goes out again after the
// retry gate, or, at the kPushTriesMax-th failure in a row, is given up; the
// re-read schedule then sends it again (ql::pushFailed arms it).
void pushFailure(uint32_t now) {
  armRetry(now);
  if (ql::pushFailed(sPolicy, now)) {
    sOwn.fullGiveUps++;
    sAfter = ql::After::None;
    DEBUG_G2F("[G2-LIST] list update failed %u times; a re-read in %lu s sends it again",
              (unsigned)ql::kPushTriesMax, (unsigned long)(ql::recoveryWaitMs(sPolicy.recovery, now) / 1000));
  } else {
    sOwn.fullRetries++;
  }
}

// One step of the push (G2_QuickListCore.h kPushAckWaitMs): one cmd 2
// FULL_UPDATE with all five rows at index 0..4 and their current titles,
// which replaces the list on the glasses; its ACK wakes the owner.
void doPush(uint32_t rightGen, uint32_t now) {
  switch (ql::pushStep(sPolicy, now)) {
    case ql::PushStep::Idle:
      sAfter = ql::After::None;
      return;
    case ql::PushStep::WaitAck:
      return;
    case ql::PushStep::AckTimeout:
      sOwn.ackTimeouts++;
      DEBUG_G2F("[G2-LIST] list update magic=%u got no ACK in %u ms", (unsigned)sPolicy.push.magic,
                (unsigned)ql::kPushAckWaitMs);
      pushFailure(now);
      return;
    case ql::PushStep::Send:
      break;
  }
  const uint8_t magic = ql::nextMagic(sLastMagic.load(std::memory_order_relaxed));
  const char* titles[ql::kRowCount];
  for (uint8_t i = 0; i < ql::kRowCount; ++i) {
    sOwnerTitles[i] = ql::rowTitle((ql::Control)i, sPolicy.row[i].next, sPolicy.row[i].nextLevel);
    titles[i] = sOwnerTitles[i].text;
  }
  const size_t n = ql::encodeRows(magic, titles, sOwnerPb, sizeof sOwnerPb);
  sLastMagic.store(magic, std::memory_order_relaxed);   // consumed even if the send fails
  const bool ok = n && g2QuickListWireSend(rightGen, sOwnerPb, n);
  if (ok) {
    // The send can wait seconds for the TX gate: the ACK wait starts when it
    // returned, not at the lap start (a late ACK would otherwise look missing).
    const uint32_t sentAt = millis();
    ql::pushSent(sPolicy, magic, sentAt);
    sOwn.fullSent++;
    sOwn.havePush = true;
    sOwn.lastAcked = false;
    sOwn.lastReason = sPolicy.push.reason;
    sOwn.lastMagic = magic;
    sOwn.lastBytes = (uint16_t)n;
    sOwn.lastSentAtMs = sentAt;
    if (sPhase == Phase::Unsupported) sPhase = Phase::Idle;   // the send needs the exact pair
    DEBUG_G2F("[G2-LIST] list update #%lu (%s) magic=%u, %u bytes: \"%s\" / \"%s\" / \"%s\" / \"%s\" / \"%s\"",
              (unsigned long)sOwn.fullSent, ql::pushReasonName(sPolicy.push.reason), (unsigned)magic, (unsigned)n,
              titles[0], titles[1], titles[2], titles[3], titles[4]);
    return;
  }
  sOwn.fullSendFails++;
  pushFailure(now);   // the lap's clock: the retry gate and re-read checks below compare against it
}

void noteApply(ql::Control c, bool target, const ql::WearResult& r) {
  const uint8_t s = ql::rowSlot(c);
  sOwn.haveApply[s] = true;
  sOwn.lastApplyTarget[s] = target;
  sOwn.lastApplyStage[s] = r.stage;
  sOwn.lastApplyAcked[s] = r.acked;
  sOwn.lastApplyKnown[s] = r.known;
  sOwn.lastApplyOn[s] = r.on;
  sOwn.lastApplyRecheck[s] = ql::recheckOf(r);
  if (c == ql::Control::Silent && r.known) sOwn.silentReadbacks++;
  if (c == ql::Control::AutoBright && r.known) sOwn.autoBrightReadbacks++;
  if (c == ql::Control::HeadUp && r.known) sOwn.headUpReadbacks++;
  if (c == ql::Control::Brightness) {
    if (r.known) sOwn.brightnessReadbacks++;
    sOwn.lastBrightTarget = r.target;
    sOwn.lastBrightAutoWasOn = r.autoWasOn;
    sOwn.lastBrightAutoWritten = r.autoWritten;
    sOwn.lastBrightAutoAcked = r.autoAcked;
    sOwn.lastBrightLevelWritten = r.levelWritten;
    sOwn.lastBrightLevelKnown = r.readOk && r.levelKnown;
    sOwn.lastBrightLevel = r.level;
    sOwn.lastBrightAutoKnown = r.readOk && r.autoKnown;
    sOwn.lastBrightAutoOn = r.autoOn;
    DEBUG_G2F("[G2-LIST] brightness step to %u by QuickList tick (auto was %s; auto-off write %s, ack=%d; "
              "level write %s, ack=%d): %s readback level=%s%u auto=%s%s",
              (unsigned)r.target, r.autoWasOn ? "on" : "off", r.autoWritten ? "sent" : "not needed",
              r.autoAcked ? 1 : 0, r.levelWritten ? "sent" : "not sent", r.acked ? 1 : 0, ql::stageName(r.stage),
              r.levelKnown ? "" : "?", (unsigned)r.level, r.autoKnown ? (r.autoOn ? "on" : "off") : "unknown",
              ql::recheckSuffix(ql::recheckOf(r)));
    return;
  }
  DEBUG_G2F("[G2-LIST] %s %s by QuickList tick: %s ack=%d readback=%s%s", ql::controlName(c),
            target ? "on" : "off", ql::stageName(r.stage), r.acked ? 1 : 0,
            r.known ? (r.on ? "on" : "off") : "unknown", ql::recheckSuffix(ql::recheckOf(r)));
}

}  // namespace

// ---- owner state machine ----------------------------------------------------
void g2QuickListTick() {
  const uint32_t now = millis();
  G2QuickListLinkState L;
  g2QuickListLink(&L);

  // 1. LINK. A new pair of generations, or both temples ready again after a
  // gap, starts over with the settle timer (Down always means reset already).
  if (!L.bothReady || L.leftGeneration != sKeyL || L.rightGeneration != sKeyR || sPhase == Phase::Down) {
    if (sPhase != Phase::Down) resetLink();
    sKeyL = sKeyR = 0;
    if (!L.bothReady) { publish(); return; }
    sKeyL = L.leftGeneration;
    sKeyR = L.rightGeneration;
    sPhase = Phase::Settling;
    sSettleStartMs = now;
  }

  // 2. JOB RESULT
  if (sPolicy.jobInFlight) {
    Job j;
    portENTER_CRITICAL(&sJobMux);
    j = sJob;
    portEXIT_CRITICAL(&sJobMux);
    if (j.done && j.token == sJobSerial) {
      const ql::WearResult& r = j.result;
      const ql::ReadResult& rd = j.read;
      if (j.rightGen != L.rightGeneration) {
        sPolicy.jobInFlight = false;   // drop: another connection
      } else if (j.kind == kJobRead && j.background) {
        // The re-read after "?": a failed one changes and pushes nothing, a
        // successful one sends the list only when a title changes (or the
        // last update was not ACKed).
        noteRead(ReadKind::Read, rd.diag, now);
        const ql::After a = ql::afterRecoveryRead(sPolicy, rd, L.firmwareExact || rd.fullProfile);
        if (a != ql::After::None) sAfter = a;
        if (a == ql::After::Push)
          ql::pushWant(sPolicy, sPolicy.staleLens ? ql::PushReason::Resend : ql::PushReason::Recovery);
        ql::recoveryAfterRead(sPolicy, now);   // every row known: reset; else the next step
        if (rd.readOk && rd.sw[kSilent].known) sOwn.silentReadbacks++;
        if (rd.readOk && rd.sw[kAutoBright].known) sOwn.autoBrightReadbacks++;
        if (rd.readOk && rd.sw[kHeadUp].known) sOwn.headUpReadbacks++;
        if (rd.readOk && rd.sw[kBrightness].known) sOwn.brightnessReadbacks++;
      } else if (j.kind == kJobRead) {
        noteRead(ReadKind::Read, rd.diag, now);
        if (!L.firmwareExact && !rd.fullProfile && !L.firmwareKnown) {
          // The read failed or came too late on a cold link and the version
          // strings are still unknown: no evidence of another firmware, so
          // retry a few times instead of marking the connection Unsupported.
          sPolicy.jobInFlight = false;
          if (++sUnknownReadRun < kUnknownReadTriesMax) {
            sAfter = ql::After::ReadThenPush;
            armRetry(now);
          } else {
            sAfter = ql::After::None;
            sUnknownReadRun = 0;
            DEBUG_G2F("[G2-LIST] settings read failed %u times, firmware unknown; left until the next push",
                      (unsigned)kUnknownReadTriesMax);
          }
        } else {
          sUnknownReadRun = 0;
          sAfter = ql::afterRead(sPolicy, rd, L.firmwareExact || rd.fullProfile);
          // A read that left a row "?" schedules a re-read; one that gave every row a state resets it.
          if (sAfter == ql::After::Push) {
            ql::recoveryAfterRead(sPolicy, now);
            ql::pushWant(sPolicy, sReadReason);
          }
          if (rd.readOk && rd.sw[kSilent].known) sOwn.silentReadbacks++;
          if (rd.readOk && rd.sw[kAutoBright].known) sOwn.autoBrightReadbacks++;
          if (rd.readOk && rd.sw[kHeadUp].known) sOwn.headUpReadbacks++;
          if (rd.readOk && rd.sw[kBrightness].known) sOwn.brightnessReadbacks++;
        }
      } else {
        const ql::Control c = (ql::Control)j.control;
        noteRead(ReadKind::PreRead, r.preRead, now);
        noteRead(ReadKind::ReadBack, r.readBack, now);
        noteRead(ReadKind::Recheck, r.recheck, now);   // made only when the first did not match
        sAfter = ql::afterApply(sPolicy, c, r);
        noteApply(c, j.target, r);
        // Another row may still show "?" (a write never resets the schedule).
        if (sAfter == ql::After::Push) {
          ql::recoveryArmIfUnknown(sPolicy, now);
          ql::pushWant(sPolicy, ql::PushReason::Tick);
        } else if (sAfter == ql::After::ReadThenPush) {
          sReadReason = ql::PushReason::Tick;
        }
      }
    } else {
      // A job that never started is given up after kJobTimeoutMs. One that
      // started (its helpers are all bounded) is waited for, so no push goes
      // out during a switch write; kJobBackstopMs covers one that never reports.
      const uint32_t age = now - sJobSubmittedMs;
      bool cancelled = false;
      portENTER_CRITICAL(&sJobMux);
      const bool reported = sJob.done && sJob.token == sJobSerial;   // picked up next lap
      if (!reported && (age > ql::kJobBackstopMs || (!sJob.started && age > ql::kJobTimeoutMs))) {
        sJob.token = 0;                // a late result is ignored, a queued job exits
        cancelled = true;
      }
      portEXIT_CRITICAL(&sJobMux);
      if (cancelled) {
        sPolicy.jobInFlight = false;
        sOwn.jobTimeouts++;
        // "?" on the rows the job was about: all for a read, the written one for a write
        // (the brightness rows go together: a write of one may change the other).
        // A re-read after "?" that never reported changes no row (like a failed one).
        if (j.kind == kJobRead && j.background) {
          // rows keep their state; the next step is scheduled below
        } else if (j.kind == kJobRead) {
          for (uint8_t i = 0; i < ql::kRowCount; ++i) ql::markUnknown(sPolicy, (ql::Control)i);
          sAfter = ql::After::Push;
        } else {
          ql::markUnknown(sPolicy, (ql::Control)j.control);
          if (j.control == kBrightness) ql::markUnknown(sPolicy, ql::Control::AutoBright);
          if (j.control == kAutoBright) ql::markUnknown(sPolicy, ql::Control::Brightness);
          sAfter = ql::After::Push;
        }
        if (sAfter == ql::After::Push) ql::pushWant(sPolicy, ql::PushReason::Timeout);
        ql::recoveryArmIfUnknown(sPolicy, now);
        DEBUG_G2F("[G2-LIST] job timed out after %u ms", (unsigned)age);
      }
    }
  }
  if (sAfter == ql::After::Unsupported) {
    sPhase = Phase::Unsupported;
    sAfter = ql::After::None;
    // A push under way or a scheduled resend (a tick job was rejected, a read
    // found another firmware) cannot go on: stop it, as step 6 does off the
    // exact pair.
    if (sPolicy.push.awaitingAck || sPolicy.push.resendDue || sPolicy.staleLens) ql::pushAbort(sPolicy);
  }

  // 2b. SILENT CHANGE PUSH (held by g2QuickListOnSilentPush; never while a job runs)
  {
    const bool live = (feedOn() || sManualThisLink) && sPolicy.row[kSilent].magic != 0;
    ql::ChangeAction a = ql::ChangeAction::Noted;
    if (ql::takeChange(sPolicy, sSilentChange, ql::Control::Silent, live, &a)) {
      if (a == ql::ChangeAction::Duplicate) sOwn.silentPushDupes++;
      if (a == ql::ChangeAction::Push) ql::pushWant(sPolicy, ql::PushReason::Change);
      DEBUG_G2F("[G2-LIST] silent change push: %s", ql::changeActionName(a));
    }
  }

  // 3. TICK (stashed by g2QuickListOnRx)
  if (sTickPending && !sPolicy.jobInFlight && retryReady(now)) {
    switch (sPendingTick.action) {
      case ql::TickAction::Apply:
        if (submitJob(kJobApply, sPendingTick.control, sPendingTick.targetOn, L.rightGeneration, now))
          sTickPending = false;
        break;
      case ql::TickAction::Refresh:
        if (submitJob(kJobRead, ql::Control::Wear, false, L.rightGeneration, now)) {
          sTickPending = false;
          sReadReason = ql::PushReason::Tick;
        }
        break;
      default:
        sTickPending = false;
        break;
    }
  }

  // 4. SETTLE
  if (sPhase == Phase::Settling && (uint32_t)(now - sSettleStartMs) >= ql::kSettleMs) {
    sPhase = Phase::Idle;
    if (L.firmwareKnown && !L.firmwareExact) {
      sPhase = Phase::Unsupported;
    } else if (feedOn() && !sAutoDone) {
      sAutoDone = true;
      sAfter = ql::After::ReadThenPush;
      sReadReason = ql::PushReason::Connect;
    }
  }

  // 5. COMMANDS
  if (sResync.exchange(false) && sPhase == Phase::Idle && feedOn() && !sAutoDone) {
    sAutoDone = true;
    sAfter = ql::After::ReadThenPush;
    sReadReason = ql::PushReason::Connect;
  }
  uint32_t reqGen = sPushRequestGen.load();
  if (reqGen && reqGen != sKeyR) {
    sPushRequestGen.compare_exchange_strong(reqGen, 0);   // made on another connection: dropped
  } else if (reqGen && sPhase != Phase::Down && sPhase != Phase::Settling && !sPolicy.jobInFlight) {
    sPushRequestGen.compare_exchange_strong(reqGen, 0);
    sManualThisLink = true;
    sPhase = Phase::Idle;              // retry Unsupported once
    sAfter = ql::After::ReadThenPush;
    sReadReason = ql::PushReason::Manual;
  }

  // 6. ACT ON sAfter (never while a job runs: no push during a switch write).
  // A row marked for a re-push (a tick while busy or rate-limited, a silent
  // change) sends the list once nothing else waits; an awaited ACK is waited
  // for. Every push is one FULL_UPDATE of all five rows.
  sBlockedByBusy = false;
  if (!sPolicy.jobInFlight) {
    if (sAfter == ql::After::None && sPhase == Phase::Idle && (ql::pushWanted(sPolicy) || sPolicy.push.awaitingAck))
      sAfter = ql::After::Push;
    switch (sAfter) {
      case ql::After::ReadThenPush:
        if (retryReady(now) && submitJob(kJobRead, ql::Control::Wear, false, L.rightGeneration, now))
          sAfter = ql::After::None;
        break;
      case ql::After::Push:
        if (!L.firmwareExact) {
          // The rows are only ever sent to the exact pair; nothing to wait for.
          if (L.firmwareKnown) sPhase = Phase::Unsupported;
          sAfter = ql::After::None;
          ql::pushAbort(sPolicy);      // no row stays pending, no ACK is awaited
        } else if (L.busy) {
          sBlockedByBusy = true;       // no fast laps for a session that may run for hours
        } else if (retryReady(now)) {
          doPush(L.rightGeneration, now);
        }
        break;
      case ql::After::Drop:
        sAfter = ql::After::None;
        break;
      default:
        break;
    }
  }

  // 6b. RE-READ AFTER "?" (backoff in the policy; never while a job, an action,
  // a tick or a held change waits, never during a voice/QuickNote session).
  {
    ql::RecoveryGate g{};
    g.phaseIdle = sPhase == Phase::Idle;
    g.enabled = feedOn() || sManualThisLink;
    g.firmwareExact = L.firmwareExact;
    g.busy = L.busy;
    g.otherWork = sAfter != ql::After::None || ql::pushWanted(sPolicy) || sPolicy.push.awaitingAck || sTickPending ||
                  sSilentChange.pending;
    const ql::RecoveryAction a = ql::decideRecovery(sPolicy, g, now);
    sRecoveryBlocked = a == ql::RecoveryAction::Blocked;
    if (a == ql::RecoveryAction::Read && retryReady(now) &&
        submitJob(kJobRead, ql::Control::Wear, false, L.rightGeneration, now, true)) {
      ql::recoveryTaken(sPolicy.recovery);
      sOwn.recoveryReads++;
      DEBUG_G2F("[G2-LIST] re-reading the settings %s (step %u)",
                sPolicy.staleLens ? "to send the list again" : "after \"?\" rows", (unsigned)sPolicy.recovery.step);
    }
  }

  // 7.
  publish();
}

uint32_t g2QuickListOwnerWaitMs() {
  const uint32_t now = millis();
  if (sPhase == Phase::Settling) {
    const uint32_t elapsed = now - sSettleStartMs;
    const uint32_t left = elapsed >= ql::kSettleMs ? 0 : ql::kSettleMs - elapsed;
    return left > kMinLapMs ? left : kMinLapMs;
  }
  if (sPolicy.jobInFlight) return kJobLapMs;
  if (sAfter != ql::After::None || sTickPending || sSilentChange.pending) {
    // A Push held only by a voice or QuickNote session: the owner's own cap
    // (and the session end, parsed on g2_owner) brings the next lap.
    if (sBlockedByBusy && !sTickPending && !sSilentChange.pending) return UINT32_MAX;
    if (sRetryArmed) {
      const uint32_t elapsed = now - sRetryFromMs;
      if (elapsed < kRetryMs) {
        const uint32_t left = kRetryMs - elapsed;
        return left > kMinLapMs ? left : kMinLapMs;
      }
    }
    // A FULL_UPDATE waiting for its ACK: the ACK's RX wakes the owner;
    // otherwise the lap that finds it missing.
    const uint32_t ackLeft = ql::pushAckWaitMs(sPolicy, now);
    if (ackLeft != UINT32_MAX && !sTickPending && !sSilentChange.pending)
      return ackLeft > kMinLapMs ? ackLeft : kMinLapMs;
    return kPendingLapMs;
  }
  // A scheduled re-read after "?": wake when it is due (and the retry gate is
  // open). One held by the feed, the pair or a session waits for a wake.
  if (sRecoveryBlocked) return UINT32_MAX;
  uint32_t left = ql::recoveryWaitMs(sPolicy.recovery, now);
  if (left == UINT32_MAX) return UINT32_MAX;
  if (sRetryArmed) {
    const uint32_t elapsed = now - sRetryFromMs;
    if (elapsed < kRetryMs && kRetryMs - elapsed > left) left = kRetryMs - elapsed;
  }
  return left > kMinLapMs ? left : kMinLapMs;
}

void g2QuickListOwnerStopped() {
  resetLink();
  sKeyL = sKeyR = 0;
  publish();
}

// ---- RX (g2_owner, non-blocking) -------------------------------------------
bool g2QuickListOnRx(uint32_t rightGeneration, uint8_t flag, const uint8_t* pb, size_t len) {
  if (rightGeneration != sKeyR || sPhase == Phase::Down) {
    sOwn.rxStale++;
  } else if (flag == G2_FLAG_RESPONSE) {
    ql::Ack a;
    if (ql::parseAck(pb, len, a) && ql::noteAck(sPolicy, a)) {
      sOwn.fullAcks++;                 // one ACK covers all five rows
      sOwn.lastAcked = true;
      sOwn.lastAckAfterMs = millis() - sOwn.lastSentAtMs;
    } else {
      sOwn.otherAcks++;
    }
  } else if (flag == G2_FLAG_NOTIFY) {
    if (!ql::parseTickReport(pb, len, sRep)) {
      sOwn.otherReports++;
    } else {
      const bool enabled = feedOn() || sManualThisLink;
      for (uint8_t i = 0; i < sRep.count; ++i) {
        const ql::ReportItem& it = sRep.items[i];
        ql::Decision d = ql::decideItem(sPolicy, enabled, sRep.glassesMagic, it, millis());
        // One job at a time: a second row ticked in the same report is re-pushed
        // unchanged, like a tick while a job runs.
        if (sTickPending && (d.action == ql::TickAction::Apply || d.action == ql::TickAction::Refresh)) {
          ql::noteRepush(sPolicy, d.control);
          d.action = ql::TickAction::Busy;
        }
        // The re-push of a tick that switched nothing sends the whole list.
        if (d.action == ql::TickAction::Busy || d.action == ql::TickAction::RateLimited)
          ql::pushWant(sPolicy, ql::PushReason::Repush);
        switch (d.action) {
          case ql::TickAction::Foreign:      sOwn.foreign++; break;
          case ql::TickAction::NotCompleted: sOwn.notCompleted++; break;
          case ql::TickAction::Disabled:     sOwn.disabled++; break;
          case ql::TickAction::Duplicate:    sOwn.dupes++; break;
          case ql::TickAction::Busy:         sOwn.busy++; break;
          case ql::TickAction::RateLimited:  sOwn.rateLimited++; break;
          case ql::TickAction::Refresh:
          case ql::TickAction::Apply:        sOwn.ticksOurs++; break;
        }
        if (ql::controlForUid(it.uid, nullptr)) {
          sOwn.haveTick = true;
          sOwn.lastTickMs = millis();
          sOwn.lastTickMagic = sRep.glassesMagic;
          sOwn.lastTickUid = it.uid;
          sOwn.lastTickAction = d.action;
        }
        if (d.action == ql::TickAction::Apply || d.action == ql::TickAction::Refresh) {
          sPendingTick = d;
          sTickPending = true;
        }
      }
    }
  } else {
    sOwn.otherReports++;
  }
  if (isDebugOutputEnabled(DEBUG_G2)) {
    const size_t shown = len < kRxHexBytes ? len : kRxHexBytes;
    ql::formatHex(pb, shown, sRxHex, sizeof sRxHex);
    DEBUG_G2F("[G2-R] QuickList flag=0x%02X len=%u pb=%s%s", (unsigned)flag, (unsigned)len, sRxHex,
              len > kRxHexBytes ? "..." : "");
  }
  publish();
  return true;
}

void g2QuickListOnSilentPush(uint32_t rightGeneration, bool on) {
  if (rightGeneration != sKeyR || sPhase == Phase::Down) {
    sOwn.silentPushStale++;
  } else {
    sOwn.silentPushes++;
    ql::holdChange(sSilentChange, on);   // the owner lap that follows this RX drain applies it
  }
  publish();
}

// ---- commands ----------------------------------------------------------------
namespace {

// Appends one formatted piece to sReply (command task), truncating at its end.
size_t sReplyLen = 0;
void addf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void addf(const char* fmt, ...) {
  if (sReplyLen + 1 >= sizeof sReply) return;
  va_list ap;
  va_start(ap, fmt);
  const int w = vsnprintf(sReply + sReplyLen, sizeof sReply - sReplyLen, fmt, ap);
  va_end(ap);
  if (w > 0) sReplyLen += (size_t)w;
  if (sReplyLen >= sizeof sReply) sReplyLen = sizeof sReply - 1;
}

const char* phaseName(Phase p) {
  switch (p) {
    case Phase::Down:        return "down";
    case Phase::Settling:    return "settling";
    case Phase::Idle:        return "idle";
    case Phase::Unsupported: return "unsupported";
  }
  return "?";
}

void addRowLine(const Pub& p, ql::Control c) {
  const uint8_t s = ql::rowSlot(c);
  if (p.rowSent[s]) {
    addf("Row %u: \"%s\" uid 0x%08X%s\n", (unsigned)ql::rowIndex(c),
        ql::rowTitle(c, p.shown[s], p.shownLevel[s]).text, (unsigned)ql::rowUid(c),
        p.rowPending[s] ? "; update pending" : "");
  } else {
    addf("Row %u: \"%s\" uid 0x%08X, not sent on this connection%s\n", (unsigned)ql::rowIndex(c),
        ql::rowTitle(c, p.next[s], p.nextLevel[s]).text, (unsigned)ql::rowUid(c), p.rowPending[s] ? " (push pending)" : "");
  }
}

void addApplyLine(const Pub& p, ql::Control c, const char* label) {
  const uint8_t s = ql::rowSlot(c);
  if (p.haveApply[s]) {
    addf("%s writes %lu (last: to %s, %s, ack %s, readback %s%s)", label, (unsigned long)p.applies[s],
        p.lastApplyTarget[s] ? "on" : "off", ql::stageName(p.lastApplyStage[s]), p.lastApplyAcked[s] ? "yes" : "no",
        p.lastApplyKnown[s] ? (p.lastApplyOn[s] ? "on" : "off") : "unknown",
        ql::recheckSuffix(p.lastApplyRecheck[s]));
  } else {
    addf("%s writes %lu (last: none)", label, (unsigned long)p.applies[s]);
  }
}

// The Brightness row's writes: the step, the auto-brightness OFF write (when
// auto brightness was on) and the level write, and what the read-back showed.
void addBrightnessApplyLine(const Pub& p) {
  const uint8_t s = kBrightness;
  if (!p.haveApply[s]) {
    addf("Brightness writes %lu (last: none)", (unsigned long)p.applies[s]);
    return;
  }
  char autoPart[48], levelPart[24], readPart[48];
  if (p.lastBrightAutoWritten)
    snprintf(autoPart, sizeof autoPart, "auto-brightness off written, ack %s", p.lastBrightAutoAcked ? "yes" : "no");
  else
    snprintf(autoPart, sizeof autoPart, "auto brightness %s", p.lastBrightAutoWasOn ? "on, not written" : "already off");
  if (p.lastBrightLevelWritten)
    snprintf(levelPart, sizeof levelPart, "ack %s", p.lastApplyAcked[s] ? "yes" : "no");
  else
    snprintf(levelPart, sizeof levelPart, "not written");
  if (!p.lastBrightAutoKnown && !p.lastBrightLevelKnown) {
    snprintf(readPart, sizeof readPart, "unknown");
  } else if (p.lastBrightLevelKnown) {
    snprintf(readPart, sizeof readPart, "level %u, auto %s", (unsigned)p.lastBrightLevel,
             p.lastBrightAutoKnown ? (p.lastBrightAutoOn ? "on" : "off") : "?");
  } else {
    snprintf(readPart, sizeof readPart, "level ?, auto %s", p.lastBrightAutoOn ? "on" : "off");
  }
  addf("Brightness writes %lu (last: to %u, %s; %s, level %s; readback %s%s)", (unsigned long)p.applies[s],
       (unsigned)p.lastBrightTarget, ql::stageName(p.lastApplyStage[s]), autoPart, levelPart, readPart,
       ql::recheckSuffix(p.lastApplyRecheck[s]));
}

}  // namespace

const char* cmd_g2liststatus(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  (void)args;
  G2QuickListLinkState L;
  g2QuickListLink(&L);
  const Pub p = snapshot();
  const uint32_t now = millis();
  sReplyLen = 0;
  sReply[0] = '\0';
  addf("Glasses QuickList rows: feed %s (saved); this connection: manual push %s\n",
      feedOn() ? "ON" : "off", p.manualThisLink ? "yes" : "no");
  addf("Temples: %s (gen L%u R%u); firmware L %s R %s (%s)\n",
      L.bothReady ? "both ready" : "not ready", (unsigned)L.leftGeneration, (unsigned)L.rightGeneration,
      L.leftFw[0] ? L.leftFw : "?", L.rightFw[0] ? L.rightFw : "?",
      L.firmwareExact ? "2.3.2.14 pair"
                      : L.firmwareKnown ? "not the 2.3.2.14 pair: no rows" : "unknown: read on first push");
  addRowLine(p, ql::Control::Wear);
  addRowLine(p, ql::Control::Silent);
  addRowLine(p, ql::Control::AutoBright);
  addRowLine(p, ql::Control::HeadUp);
  addRowLine(p, ql::Control::Brightness);
  addf("Phase: %s%s\n", phaseName(p.phase), p.jobInFlight ? " (job running)" : "");
  char last[96];
  if (p.haveTick) {
    snprintf(last, sizeof last, "%lu s ago (glasses #%lu, uid 0x%08lX, %s)",
             (unsigned long)((now - p.lastTickMs) / 1000), (unsigned long)p.lastTickMagic,
             (unsigned long)p.lastTickUid, ql::tickActionName(p.lastTickAction));
  } else {
    snprintf(last, sizeof last, "none");
  }
  addf("Ticks: ours %lu, duplicate %lu, foreign %lu, not completed %lu, ignored (feed off) %lu, "
      "while busy %lu, rate-limited %lu, other reports %lu; last tick %s\n",
      (unsigned long)p.ticksOurs, (unsigned long)p.dupes, (unsigned long)p.foreign,
      (unsigned long)p.notCompleted, (unsigned long)p.disabled, (unsigned long)p.busy,
      (unsigned long)p.rateLimited, (unsigned long)p.otherReports, last);
  addApplyLine(p, ql::Control::Wear, "Wear");
  addf("; reads %lu; job queue refusals %lu, timeouts %lu\n", (unsigned long)p.reads,
      (unsigned long)p.submitFails, (unsigned long)p.jobTimeouts);
  addApplyLine(p, ql::Control::Silent, "Silent");
  addf("; silent read-backs %lu; silent change pushes %lu (duplicate %lu, old connection %lu)\n",
      (unsigned long)p.silentReadbacks, (unsigned long)p.silentPushes, (unsigned long)p.silentPushDupes,
      (unsigned long)p.silentPushStale);
  addApplyLine(p, ql::Control::AutoBright, "Auto brightness");
  addf("; auto-brightness read-backs %lu (no change push is known)\n", (unsigned long)p.autoBrightReadbacks);
  addApplyLine(p, ql::Control::HeadUp, "Head-up");
  addf("; head-up read-backs %lu\n", (unsigned long)p.headUpReadbacks);
  addBrightnessApplyLine(p);
  addf("; brightness read-backs %lu\n", (unsigned long)p.brightnessReadbacks);
  addf("Settings reads %lu (unanswered %lu, answered only after a resend %lu); last: ",
      (unsigned long)p.settingsReads, (unsigned long)p.readsUnanswered, (unsigned long)p.readsResent);
  if (p.haveLastRead) {
    const ql::ReadDiag& d = p.lastRead;
    const char* kind = readKindName((ReadKind)p.lastReadKind);
    const unsigned long ago = (unsigned long)((now - p.lastReadAtMs) / 1000);
    if (ql::readAnswered(d)) {
      addf("%s %lu s ago, answered by attempt %u of %u after %lu ms", kind, ago, (unsigned)d.answeredAttempt,
          (unsigned)d.attempts, (unsigned long)d.replyMs);
    } else {
      addf("%s %lu s ago, no answer after %u attempt(s), %lu ms", kind, ago, (unsigned)d.attempts,
          (unsigned long)d.totalMs);
    }
    addf("; %u other replies, %u other settings frames during the wait\n", (unsigned)d.otherReplies,
        (unsigned)d.otherFrames);
  } else {
    addf("none\n");
  }
  if (!p.recovery.armed) {
    addf("Re-reads after \"?\" %lu; none scheduled\n", (unsigned long)p.recoveryReads);
  } else if (p.recoveryBlocked) {
    addf("Re-reads after \"?\" %lu; next one due, held (feed off, not the 2.3.2.14 pair, or a voice or "
        "QuickNote session)\n", (unsigned long)p.recoveryReads);
  } else {
    const uint32_t left = ql::recoveryWaitMs(p.recovery, now);
    addf("Re-reads after \"?\" %lu; next in %lu s\n", (unsigned long)p.recoveryReads,
        (unsigned long)((left + 999) / 1000));
  }
  // Full updates: every push sends all five rows in one FULL_UPDATE.
  addf("Full updates %lu (send failed %lu), acked %lu (other ACKs %lu), retries %lu (ACK timeouts %lu), "
      "given up %lu; last sent: ",
      (unsigned long)p.fullSent, (unsigned long)p.fullSendFails, (unsigned long)p.fullAcks,
      (unsigned long)p.otherAcks, (unsigned long)p.fullRetries, (unsigned long)p.ackTimeouts,
      (unsigned long)p.fullGiveUps);
  if (p.havePush) {
    char ack[40];
    if (p.lastAcked) snprintf(ack, sizeof ack, "acked after %lu ms", (unsigned long)p.lastAckAfterMs);
    else snprintf(ack, sizeof ack, "%s", p.awaitingAck ? "waiting for its ACK" : "not acked");
    addf("%s, magic %u, %u bytes, %lu s ago, %s%s\n", ql::pushReasonName(p.lastReason), (unsigned)p.lastMagic,
        (unsigned)p.lastBytes, (unsigned long)((now - p.lastSentAtMs) / 1000), ack,
        p.staleLens ? "; the lens may be stale: the next re-read sends the list again" : "");
  } else {
    addf("none%s\n", p.staleLens ? "; the lens may be stale: the next re-read sends the list again" : "");
  }
  addf("Frames from an old connection %lu", (unsigned long)p.rxStale);
  return sReply;
}

const char* cmd_g2listfeed(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = args;
  a.trim();
  a.toLowerCase();
  if (!a.length())
    return feedOn() ? "Glasses QuickList rows (wear detection, silent mode, auto brightness, head-up display, "
                      "brightness): on"
                    : "Glasses QuickList rows (wear detection, silent mode, auto brightness, head-up display, "
                      "brightness): off";
  if (argIsOn(a)) {
    setSetting(gSettings.g2QuickListFeed, true);
    sResync.store(true);
    g2QuickListWake();
    return "Glasses QuickList rows ON (saved): after each connect of glasses on firmware 2.3.2.14 the "
           "glasses' QuickList is replaced with \"Wear Detection: On/Off\", \"Silent Mode: On/Off\", "
           "\"Auto Brightness: On/Off\", \"Head-Up Display: On/Off\" and \"Brightness: "
           "Auto/25%/50%/75%/100%\" rows (items added in the Even app are removed); tick one and leave "
           "the list to switch that setting (Brightness turns auto brightness off, then steps 25, 50, "
           "75, 100, 25).";
  }
  if (argIsOff(a)) {
    setSetting(gSettings.g2QuickListFeed, false);
    g2QuickListWake();
    return "Glasses QuickList rows off (saved). HardwareOne sends no more lists and ignores ticks on "
           "the rows; it sends nothing on feed off, so the last list stays in the glasses' QuickList.";
  }
  return "Error: invalid arguments — Usage: g2 list feed [on|off]";
}

const char* cmd_g2listpush(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = args;
  a.trim();
  if (a.length()) return "Error: invalid arguments — Usage: g2 list push";
  G2QuickListLinkState L;
  if (!g2QuickListLink(&L))
    return "Error: Both temples must be connected and ready, nothing sent. Run openg2, then retry.";
  if (L.firmwareKnown && !L.firmwareExact) {
    snprintf(sReply, sizeof sReply, "Error: Glasses firmware L %s / R %s is not the 2.3.2.14 pair, nothing sent.",
             L.leftFw, L.rightFw);
    return sReply;
  }
  sPushRequestGen.store(L.rightGeneration);   // this connection only
  g2QuickListWake();
  return "Queued: read the settings on the right temple, then replace the glasses' QuickList with the "
         "\"Wear Detection\", \"Silent Mode\", \"Auto Brightness\", \"Head-Up Display\" and "
         "\"Brightness\" rows (items added in the Even app are removed). g2 list status shows the result.";
}

const char* cmd_g2listpreview(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = args;
  a.trim();
  if (a.length()) return "Error: invalid arguments — Usage: g2 list preview";
  const uint32_t packed = sNextShown.load(std::memory_order_relaxed);
  const uint8_t magic = ql::nextMagic(sLastMagic.load(std::memory_order_relaxed));
  const char* titles[ql::kRowCount];
  for (uint8_t i = 0; i < ql::kRowCount; ++i) {
    const uint8_t level = i == kBrightness ? (uint8_t)(packed >> 16) : 0;
    sCmdTitles[i] = ql::rowTitle((ql::Control)i, (ql::Shown)((packed >> (2 * i)) & 3), level);
    titles[i] = sCmdTitles[i].text;
  }
  const size_t n = ql::encodeRows(magic, titles, sCmdPb, sizeof sCmdPb);
  if (!n) return "Error: the QuickList rows did not encode";
  ql::formatHex(sCmdPb, n, sCmdHex, sizeof sCmdHex);
  sReplyLen = 0;
  sReply[0] = '\0';
  addf("Next QuickList push (not sent): sid 0x0C flag 0x20 to the right temple, cmd 2 FULL_UPDATE, magic %u, "
      "total_count %u; it replaces the whole list, rows top to bottom:", (unsigned)magic, (unsigned)ql::kRowCount);
  for (uint8_t i = 0; i < ql::kRowCount; ++i)
    addf("\nRow %u: uid 0x%08X, \"%s\" (%u bytes with NUL)", (unsigned)i, (unsigned)ql::rowUid((ql::Control)i),
        titles[i], (unsigned)(strlen(titles[i]) + 1));
  addf("\npb (%u bytes): %s", (unsigned)n, sCmdHex);
  return sReply;
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES && ENABLE_G2_QUICKLIST
