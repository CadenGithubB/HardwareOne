// System_Collect.cpp - file catch-up, the collector (package 5)
//
// See System_Collect.h and FILES_IMPL_SPEC.md section 8.
//
//   espnow_task  v4hCatchupListResp / v4hCatchupReadResp copy what matches the
//                one outstanding request into PSRAM (a listing page, a window
//                frame, a status, a digest); they never touch the card or the
//                lock. collectTick (under sLock, try-lock) refreshes the gates
//                and the peers once a second, runs the scheduler and drives
//                the run: LIST pages -> plan (card job) -> READ windows ->
//                append (card job) -> ... -> digest -> finish (card job).
//   main loop    collectWriteTick runs the posted card job under sLock.
//   cmd_exec     the commands copy what they show under sLock and format it
//                without the lock.
// Which side may touch the run's PSRAM state (the listing, the window, the
// file list) is decided by the job slot: while a job is posted or running it
// belongs to the main loop, otherwise to espnow_task.
//
// Memory: every table and buffer is an EXT_RAM_BSS static (PSRAM); the lock
// is a PSRAM mutex. Internal RAM: the job slot's spinlock (8 bytes). No task,
// no String member, no heap.

#include "System_Collect.h"

#if ENABLE_ESPNOW

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   // strcasecmp

#include <esp_attr.h>  // EXT_RAM_BSS_ATTR
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "System_ESPNow_RxCtx.h"

#if !ENABLE_ESPNOW_COLLECT

// ---------------------------------------------------------------------------
// Built without the collector: answers to a request this device never sent
// are counted and dropped.
// ---------------------------------------------------------------------------
namespace {
EXT_RAM_BSS_ATTR uint32_t sCollectNotBuiltDropped;
}  // namespace
V4Verdict v4hCatchupListResp(const V4RxCtx& ctx) { ++sCollectNotBuiltDropped; return ctx.refuseWith(espnowanswer::kUnsupported); }
V4Verdict v4hCatchupReadResp(const V4RxCtx& ctx) { ++sCollectNotBuiltDropped; return ctx.refuseWith(espnowanswer::kUnsupported); }
void collectTick(uint32_t) {}
void collectWriteTick() {}

#else  // ENABLE_ESPNOW_COLLECT

#include "freertos/idf_additions.h"   // xSemaphoreCreateMutexWithCaps

#include "System_Clock.h"
#include "System_CollectCore.h"
#include "System_CollectStore.h"
#include "System_CommandRegistry.h"
#include "System_CommandReply.h"          // kCommandReplyOutOfMemory
#include "System_Debug.h"
#include "System_ESPNow.h"                // generateMessageId, sessions, resolveDeviceNameOrMac
#include "System_ESPNow_CatchupCore.h"
#include "System_ESPNow_Identity.h"       // peerIdentityFindByMac / peerIdentityAt: the paired key
#include "System_ESPNow_PowerEventCore.h" // espnowsensor::jsonTextEscape
#include "System_ESPNow_PowerEvents.h"    // espnowBatteryJsonFor: lowest battery first
#include "System_ESPNow_Sessions.h"       // sessionAt / sessionSlotCount
#include "System_ESPNow_Tx.h"             // espnowtx::sendAead, espnowtx::queueDepth
#include "System_MacText.h"
#include "System_Settings.h"
#include "System_Utils.h"                 // RETURN_VALID_IF_VALIDATE_CSTR
#include "System_VFS.h"

namespace {

namespace cc = collectcore;
namespace cu = espnowcatchup;

enum class Stage : uint8_t { Idle, Listing, Planning, Fetching, Writing, Verifying, Finishing };
enum class JobKind : uint8_t { None, Plan, Write, Verify, Finish };
enum : uint8_t { kJobIdle = 0, kJobPosted = 1, kJobRunning = 2, kJobDone = 3 };

constexpr uint32_t kGateEveryMs = 1000;
constexpr uint32_t kPeersEveryMs = 1000;
constexpr uint32_t kBatteryEveryMs = 30000;
constexpr uint32_t kNoAnswerBackoffMs = cc::kIntervalMs;   // "no answer": the next trigger resumes it
constexpr uint32_t kGateClosedBackoffMs = 1000;
constexpr TickType_t kWriterLockTicks = pdMS_TO_TICKS(1000);
constexpr TickType_t kCommandLockTicks = pdMS_TO_TICKS(500);
constexpr uint8_t kGapNotes = 8;

struct Client {
  Stage    stage;
  int8_t   dev;                   // index in sSched.dev
  uint8_t  mac[6];
  uint8_t  op;                    // the outstanding request (82 or 84); 0 none
  uint32_t reqId;
  uint8_t  tries;
  uint32_t sentMs, timeoutMs;
  uint16_t pathCap;
  bool     routed;
  // Listing.
  uint32_t fromFileId, nextPageAtMs;
  bool     pageReady, ledgerMoved;
  uint8_t  listStatus;
  uint16_t listRetry;
  uint32_t listNext;
  // Reading.
  cc::ReadPlan plan;
  bool     statusReady;
  uint8_t  readStatus;
  uint32_t readOffset;
  uint16_t readRetry;
  bool     digestReady;
  uint8_t  digest[cu::kDigestLen];
  // The run.
  bool     full, flushed;
  uint32_t runStartMs, runBytes, windowSentMs;
  char     keyFp[17];
  // How the run ends once the Finish job has saved the index.
  cc::Scheduler::Outcome endOutcome;
  uint32_t endBackoffMs;
  cc::Why  endWhy;
};

struct JobSlot {
  volatile uint8_t state;         // kJob*; under sJobMux
  JobKind  kind;
  bool     ok, ledgerReset, rejected;
  uint8_t  devIndex;
  uint8_t  mac[6];
  char     keyFp[17];
  bool     full;
  bool     claimSafe;             // Write: every committed byte is in a segment the saved index names
  bool     planFull;              // Plan: the index started fresh here; plan as `full`
  uint32_t fileId, offset, len, committed;
  uint8_t  digest[cu::kDigestLen];
  cc::Scheduler::Outcome outcome;
};

struct Stats {
  uint32_t unexpected, malformed, listPages, windows, bytes, timeouts, digestOk, digestMismatch, cardErrors,
           sendFailed, runs, ledgerResets;
};

struct GapNote {
  uint8_t  mac[6];
  uint8_t  why, used;
  uint32_t bytes, epoch;
  char     name[33];
};

// PSRAM: the run, the tables, the snapshots the commands format from.
EXT_RAM_BSS_ATTR cc::Scheduler sSched;
EXT_RAM_BSS_ATTR cc::DeviceFiles sFiles;
EXT_RAM_BSS_ATTR cc::Listing sListing;
EXT_RAM_BSS_ATTR cc::WindowAssembler sWin;
EXT_RAM_BSS_ATTR Client sClient;
EXT_RAM_BSS_ATTR JobSlot sJob;
EXT_RAM_BSS_ATTR Stats sStats;
EXT_RAM_BSS_ATTR GapNote sGapNotes[kGapNotes];
EXT_RAM_BSS_ATTR uint8_t sGapNext;
EXT_RAM_BSS_ATTR uint32_t sGapBefore[cc::kMaxFiles][cc::kGapWhyCount];
EXT_RAM_BSS_ATTR uint32_t sGapBeforeId[cc::kMaxFiles];
EXT_RAM_BSS_ATTR uint32_t sLastGateMs, sLastPeersMs, sLastBatteryMs, sSdGen, sLastIndexSaveMs, sBytesSinceQuota;
EXT_RAM_BSS_ATTR uint8_t sGateWasOpen, sFilesStale;
EXT_RAM_BSS_ATTR uint8_t sUnsavedSegment;   // a new segment holds bytes the saved index.json does not name
EXT_RAM_BSS_ATTR cc::Gate sGate;
EXT_RAM_BSS_ATTR char sBatteryJson[640];
EXT_RAM_BSS_ATTR SemaphoreHandle_t sLock;   // the mutex itself is PSRAM (WithCaps)
EXT_RAM_BSS_ATTR cc::Scheduler sSchedSnap;
EXT_RAM_BSS_ATTR cc::DeviceFiles sFilesSnap;
EXT_RAM_BSS_ATTR GapNote sGapSnap[kGapNotes];
EXT_RAM_BSS_ATTR uint8_t sGapSnapNext;
EXT_RAM_BSS_ATTR char sLine[1024];
portMUX_TYPE sJobMux = portMUX_INITIALIZER_UNLOCKED;   // internal: a spinlock

uint32_t nowEpoch() { return Clock::isSynced() ? (uint32_t)Clock::epochSeconds() : 0; }
uint64_t quotaBytes() { return (uint64_t)gSettings.collectQuotaMb << 20; }

uint32_t newReqId() {
  uint32_t id = 0;
  while (id == 0) id = generateMessageId();
  return id;
}

// ---- The job slot ----------------------------------------------------------
uint8_t jobState() {
  portENTER_CRITICAL(&sJobMux);
  const uint8_t s = sJob.state;
  portEXIT_CRITICAL(&sJobMux);
  return s;
}
void jobSetState(uint8_t s) {
  portENTER_CRITICAL(&sJobMux);
  sJob.state = s;
  portEXIT_CRITICAL(&sJobMux);
}
void postJob(JobKind kind) {
  sJob.kind = kind;
  sJob.ok = false;
  sJob.ledgerReset = false;
  sJob.rejected = false;
  sJob.committed = 0;
  sJob.claimSafe = false;
  sJob.planFull = false;
  sJob.devIndex = (uint8_t)sClient.dev;
  memcpy(sJob.mac, sClient.mac, 6);
  memcpy(sJob.keyFp, sClient.keyFp, sizeof(sJob.keyFp));
  sJob.full = sClient.full;
  jobSetState(kJobPosted);
}

// ---- Gap notes ("portable earlier: 1,200 bytes of events.log were trimmed") --
void noteGap(const uint8_t mac[6], const char* name, uint8_t why, uint32_t bytes, uint32_t epoch) {
  if (!bytes) return;
  GapNote& n = sGapNotes[sGapNext % kGapNotes];
  sGapNext = (uint8_t)((sGapNext + 1) % kGapNotes);
  memcpy(n.mac, mac, 6);
  n.why = why;
  n.used = 1;
  n.bytes = bytes;
  n.epoch = epoch;
  strlcpy(n.name, name ? name : "", sizeof(n.name));
}
void gapSnapshot() {
  for (uint8_t i = 0; i < cc::kMaxFiles; ++i) {
    sGapBeforeId[i] = i < sFiles.count ? sFiles.f[i].fileId : 0;
    for (uint8_t g = 0; g < cc::kGapWhyCount; ++g) sGapBefore[i][g] = i < sFiles.count ? sFiles.f[i].gapBytes[g] : 0;
  }
}
void gapNotesSince(uint32_t epoch) {
  for (uint8_t i = 0; i < sFiles.count; ++i) {
    const cc::FileState& f = sFiles.f[i];
    const uint32_t* before = nullptr;
    for (uint8_t k = 0; k < cc::kMaxFiles; ++k)
      if (sGapBeforeId[k] == f.fileId && f.fileId) { before = sGapBefore[k]; break; }
    for (uint8_t g = 0; g < cc::kGapWhyCount; ++g) {
      const uint32_t was = before ? before[g] : 0;
      if (f.gapBytes[g] > was) noteGap(sFiles.mac, f.name, g, f.gapBytes[g] - was, epoch);
    }
  }
}

// The per-device summary the commands show (after every card job).
void refreshSummary(cc::Device& d) {
  const cc::Progress p = cc::progressOf(sFiles);
  d.files = p.files;
  d.filesLeft = p.filesLeft;
  d.bytesLeft = p.bytesLeft;
  d.held = p.held;
  d.elsewhere = p.elsewhere;
  for (uint8_t g = 0; g < cc::kGapWhyCount; ++g) d.gaps[g] = p.gaps[g];
  d.pct = p.pct;
  d.ledgerId = sFiles.ledgerId;
  d.lastCaughtUpEpoch = sFiles.lastCaughtUpEpoch ? sFiles.lastCaughtUpEpoch : d.lastCaughtUpEpoch;
}

// ---- The card jobs (main loop, under sLock) ---------------------------------
bool ensureFilesLoaded(uint32_t epoch) {
  if (!sFilesStale && sFiles.used && memcmp(sFiles.mac, sJob.mac, 6) == 0 &&
      (sJob.keyFp[0] == 0 || strcmp(sFiles.peerKeyFp, sJob.keyFp) == 0))
    return true;
  collectstore::LoadResult lr = {};
  if (!collectstore::loadIndex(sJob.mac, sJob.keyFp, epoch, sFiles, &lr)) {
    sFiles.used = 0;
    return false;
  }
  sFilesStale = 0;
  sUnsavedSegment = 0;
  if (lr.identityChanged)
    INFO_ESPNOWF("[COLLECT] " MAC_FMT " was paired again: its earlier files were moved to files.prev-*", MAC_ARGS(sJob.mac));
  if (lr.badIndex) WARN_ESPNOWF("[COLLECT] " MAC_FMT ": index.json did not read; kept as index.json.bad", MAC_ARGS(sJob.mac));
  // No index (a new or replaced card, an unreadable index): the device's
  // `delivered` may be this hub's own earlier claims, so the first plan
  // fetches whatever the device still holds instead of marking it as at the
  // other master (robustness review 2026-11-19).
  if (lr.fresh || lr.badIndex) sJob.planFull = true;
  if (lr.recovered || lr.tornCut || lr.missing)
    INFO_ESPNOWF("[COLLECT] " MAC_FMT ": recovered %u segment(s), cut %u torn tail(s), %u missing", MAC_ARGS(sJob.mac),
                 (unsigned)lr.recovered, (unsigned)lr.tornCut, (unsigned)lr.missing);
  return true;
}

void runJob() {
  const uint32_t epoch = nowEpoch();
  const uint32_t now = (uint32_t)millis();
  cc::Device& dev = sSched.dev[sJob.devIndex < cc::kMaxDevices ? sJob.devIndex : 0];
  switch (sJob.kind) {
    case JobKind::Plan: {
      if (!ensureFilesLoaded(epoch)) { sJob.ok = false; break; }
      if (cc::ledgerChanged(sFiles, sListing.ledgerId)) {
        for (uint8_t i = 0; i < sFiles.count; ++i) collectstore::archiveFile(sFiles, sFiles.f[i], "ledger_reset", epoch);
        cc::retireAll(sFiles, sListing.ledgerId);
        sJob.ledgerReset = true;
      }
      gapSnapshot();
      cc::planFromList(sFiles, sListing, sJob.full || sJob.planFull, epoch,
                       [&](const cc::FileState& f) { collectstore::archiveFile(sFiles, f, "dropped", epoch); });
      gapNotesSince(epoch);
      const collectstore::QuotaResult q = collectstore::enforceQuota(sFiles, quotaBytes(), 0, epoch);
      dev.bytesOnCard = q.after;
      sBytesSinceQuota = 0;
      sJob.ok = collectstore::saveIndex(sFiles, epoch);
      if (sJob.ok) sUnsavedSegment = 0;
      sLastIndexSaveMs = now;
      dev.listFlags = sListing.respFlags;
      refreshSummary(dev);
      break;
    }
    case JobKind::Write: {
      cc::FileState* f = cc::findFile(sFiles, sJob.fileId);
      if (!f) { sJob.rejected = true; break; }
      const collectstore::WriteResult r = collectstore::appendWindow(sFiles, *f, sWin, epoch);
      sJob.committed = r.committed;
      sJob.rejected = r.rejected;
      sJob.ok = !r.ioError && !r.rejected;
      dev.bytesOnCard += r.committed;
      sBytesSinceQuota += r.committed;
      if (sBytesSinceQuota >= cc::kQuotaCheckBytes) {
        dev.bytesOnCard = collectstore::enforceQuota(sFiles, quotaBytes(), f->fileId, epoch).after;
        sBytesSinceQuota = 0;
      }
      // A new segment is named in the saved index before any READ claims
      // storedBefore past its start: after a crash or a card pull the load
      // recovers the length of a named segment, but an unnamed one would be
      // lost while the device counted it as delivered (robustness review).
      if (r.opened) sUnsavedSegment = 1;
      if (sUnsavedSegment || f->pos >= cc::effectiveEnd(*f) || r.ioError ||
          (uint32_t)(now - sLastIndexSaveMs) >= cc::kIndexSaveMs) {
        if (collectstore::saveIndex(sFiles, epoch)) sUnsavedSegment = 0;
        sLastIndexSaveMs = now;
      }
      sJob.claimSafe = !sUnsavedSegment && !r.ioError;
      refreshSummary(dev);
      break;
    }
    case JobKind::Verify: {
      cc::FileState* f = cc::findFile(sFiles, sJob.fileId);
      if (!f) break;
      uint8_t mine[cu::kDigestLen];
      if (collectstore::segmentDigest(sFiles, *f, sJob.offset, sJob.len, mine)) {
        const bool same = memcmp(mine, sJob.digest, sizeof(mine)) == 0;
        f->digest = same ? cc::DigestState::Ok : cc::DigestState::Mismatch;
        if (same) ++sStats.digestOk;
        else {
          ++sStats.digestMismatch;
          WARN_ESPNOWF("[COLLECT] " MAC_FMT ": %s differs from the device's copy (digest)", MAC_ARGS(sJob.mac), f->name);
        }
      } else {
        f->digestTriedRun = sFiles.runId;   // read back failed here: try again next run
      }
      sJob.ok = collectstore::saveIndex(sFiles, epoch);
      if (sJob.ok) sUnsavedSegment = 0;
      sLastIndexSaveMs = now;
      break;
    }
    case JobKind::Finish: {
      if (sJob.outcome == cc::Scheduler::Outcome::Current) sFiles.lastCaughtUpEpoch = epoch;
      sJob.ok = collectstore::saveIndex(sFiles, epoch);
      if (sJob.ok) sUnsavedSegment = 0;
      sLastIndexSaveMs = now;
      refreshSummary(dev);
      break;
    }
    case JobKind::None:
      break;
  }
  if (!sJob.ok && sJob.kind != JobKind::Verify && !sJob.rejected) ++sStats.cardErrors;
}

// ---- Sending -----------------------------------------------------------------
bool sendList(uint32_t now) {
  cu::ListReq q = {};
  q.reqId = newReqId();
  q.fromFileId = sClient.fromFileId;
  uint8_t buf[cu::kListReqLen];
  cu::packListReq(q, buf, sizeof(buf));
  sClient.op = cu::kOpListReq;
  sClient.reqId = q.reqId;
  sClient.sentMs = now;
  sClient.timeoutMs = sClient.routed ? cc::kTimeoutRoutedMs : cc::kTimeoutDirectMs;
  sClient.pageReady = false;
  sClient.ledgerMoved = false;
  // No ACK request, ttl 1, a fresh header msgId (the CATCHUP_* convention);
  // a frame that does not go is asked again at the timeout.
  if (!espnowtx::sendAead(sClient.mac, cu::kOpListReq, 0, generateMessageId(), buf, sizeof(buf), 1)) {
    ++sStats.sendFailed;
    return false;
  }
  return true;
}

bool sendRead(uint32_t now) {
  const cc::FileState* f = cc::findFile(sFiles, sClient.plan.fileId);
  if (!f) return false;
  cu::ReadReq q = {};
  q.reqId = newReqId();
  q.fileId = sClient.plan.fileId;
  q.offset = sClient.plan.offset;
  q.maxBytes = sClient.plan.maxBytes;
  q.mode = sClient.plan.mode;
  q.flags = sClient.plan.flags;
  q.ledgerId = sFiles.ledgerId;
  uint8_t buf[cu::kReadReqLen];
  cu::packReadReq(q, buf, sizeof(buf));
  sClient.op = cu::kOpReadReq;
  sClient.reqId = q.reqId;
  sClient.sentMs = now;
  sClient.windowSentMs = now;
  sClient.timeoutMs = sClient.routed ? cc::kTimeoutRoutedMs : cc::kTimeoutDirectMs;
  sClient.statusReady = false;
  sClient.digestReady = false;
  if (q.mode == cu::kModeData) sWin.begin(q.reqId, q.fileId, q.offset, cc::isSealOnSend(*f), now, sClient.routed);
  else sWin.clear();
  cc::Device& d = sSched.dev[sClient.dev];
  d.fetchingFileId = f->fileId;
  strlcpy(d.fetchingName, f->name, sizeof(d.fetchingName));
  d.state = q.mode == cu::kModeDigest ? cc::DevState::Verifying : cc::DevState::Fetching;
  if (!espnowtx::sendAead(sClient.mac, cu::kOpReadReq, 0, generateMessageId(), buf, sizeof(buf), 1)) {
    ++sStats.sendFailed;
    return false;
  }
  return true;
}

// ---- The run (espnow_task, under sLock) --------------------------------------
void endRunNow(cc::Scheduler::Outcome o, uint32_t now, uint32_t backoffMs, cc::Why why) {
  sSched.end(o, now, backoffMs, nowEpoch(), why);
  sClient.stage = Stage::Idle;
  sClient.op = 0;
  sWin.clear();
}
// From the fetch stage the index may have changed: a Finish job saves it first.
void finishRun(cc::Scheduler::Outcome o, uint32_t backoffMs, cc::Why why) {
  sClient.endOutcome = o;
  sClient.endBackoffMs = backoffMs;
  sClient.endWhy = why;
  sClient.op = 0;
  sWin.clear();
  sJob.outcome = o;
  postJob(JobKind::Finish);
  sClient.stage = Stage::Finishing;
}

void endFromAction(const cc::StatusAction& a, bool fetchStage, uint32_t now, uint8_t status) {
  cc::Scheduler::Outcome o = cc::Scheduler::Outcome::Backoff;
  cc::Why why = status == cu::kStBusy ? cc::Why::Busy : cc::Why::NotReady;
  switch (a.act) {
    case cc::Act::Relist:            o = cc::Scheduler::Outcome::Relist; why = cc::Why::None; break;
    case cc::Act::DeviceRefused:     o = cc::Scheduler::Outcome::Refused; why = cc::Why::Refused; break;
    case cc::Act::DeviceUnsupported: o = cc::Scheduler::Outcome::Unsupported; why = cc::Why::Unsupported; break;
    default: break;
  }
  if (fetchStage) finishRun(o, a.backoffMs, why);
  else endRunNow(o, now, a.backoffMs, why);
}

void startNext(uint32_t now) {
  const int i = sSched.pick(now);
  if (i < 0) return;
  cc::Device& d = sSched.dev[i];
  memset(&sClient, 0, sizeof(sClient));
  sClient.dev = (int8_t)i;
  memcpy(sClient.mac, d.mac, 6);
  sClient.full = d.forceFull != 0;
  if (const PeerIdentity* pid = peerIdentityFindByMac(d.mac)) {
    for (int k = 0; k < 8; ++k) snprintf(sClient.keyFp + 2 * k, 3, "%02x", pid->longTermPub[k]);
  }
  sClient.pathCap = v4_single_frame_cap(d.mac);
  if (sClient.pathCap > cu::kDirectPayload) sClient.pathCap = cu::kDirectPayload;
  sClient.routed = sClient.pathCap < cu::kDirectPayload;
  sClient.runStartMs = now;
  sClient.flushed = true;
  sSched.start(i, now);
  cc::listingReset(sListing);
  sClient.stage = Stage::Listing;
  ++sStats.runs;
  sendList(now);
}

void stepListing(uint32_t now) {
  if (sClient.pageReady) {
    sClient.pageReady = false;
    sClient.op = 0;
    sClient.tries = 0;
    if (sClient.ledgerMoved) {   // the ledger changed between pages: list again later (the device's 60 s rule)
      endRunNow(cc::Scheduler::Outcome::Relist, now, cc::kPastEndRelistMs, cc::Why::None);
      return;
    }
    if (sClient.listStatus != cu::kStOk) {
      endFromAction(cc::onListStatus(sClient.listStatus, sClient.listRetry), false, now, sClient.listStatus);
      return;
    }
    ++sStats.listPages;
    const bool more = sClient.listNext != 0 && sClient.listNext > sClient.fromFileId && !sListing.overflow &&
                      sListing.pages < cu::kMaxPagesPerRun;
    if (more) {
      sClient.fromFileId = sClient.listNext;
      sClient.nextPageAtMs = now + cu::kPageMinMs;
      return;
    }
    sListing.complete = sClient.listNext == 0;
    postJob(JobKind::Plan);
    sClient.stage = Stage::Planning;
    return;
  }
  if (sClient.op == 0) {
    if ((int32_t)(now - sClient.nextPageAtMs) >= 0) sendList(now);
    return;
  }
  if ((uint32_t)(now - sClient.sentMs) >= sClient.timeoutMs) {
    ++sStats.timeouts;
    if (++sClient.tries >= cc::kTries) {
      endRunNow(cc::Scheduler::Outcome::Suspend, now, kNoAnswerBackoffMs, cc::Why::NoAnswer);
      return;
    }
    sendList(now);
  }
}

void finishWindow(uint32_t now) {
  sClient.op = 0;
  if (sWin.mismatch) ++sStats.malformed;
  cc::FileState* f = cc::findFile(sFiles, sWin.fileId);
  if (!f) { sWin.clear(); return; }
  // The device's end as it answered (a growing file grows).
  if (sWin.frames && sWin.end > f->lastSeenEnd) f->lastSeenEnd = sWin.end;
  if (sWin.frames) {
    if (sWin.growing) f->flags |= cc::kFfGrowing;
    else if (sWin.atEnd) f->flags &= (uint8_t)~cc::kFfGrowing;
  }
  if (sWin.plainBytes() == 0) {
    // offset == end on the device: what was listed is all there is now.
    if (sWin.atEnd && sWin.end <= f->pos) { f->lastSeenEnd = f->pos; cc::settleState(*f); }
    else if (++sClient.tries >= cc::kTries) { finishRun(cc::Scheduler::Outcome::Suspend, kNoAnswerBackoffMs, cc::Why::NoAnswer); return; }
    sWin.clear();
    return;
  }
  ++sStats.windows;
  sJob.fileId = f->fileId;
  postJob(JobKind::Write);
  sClient.stage = Stage::Writing;
  sClient.flushed = false;
  (void)now;
}

void handleReadStatus(uint32_t now) {
  sClient.statusReady = false;
  sClient.op = 0;
  sWin.clear();
  cc::FileState* f = cc::findFile(sFiles, sClient.plan.fileId);
  if (!f) return;
  uint32_t before[cc::kGapWhyCount];
  memcpy(before, f->gapBytes, sizeof(before));
  const cc::StatusAction a =
      cc::onReadStatus(sFiles, *f, sClient.plan.mode, sClient.readStatus, sClient.readOffset, sClient.readRetry);
  for (uint8_t g = 0; g < cc::kGapWhyCount; ++g)
    if (f->gapBytes[g] > before[g]) noteGap(sFiles.mac, f->name, g, f->gapBytes[g] - before[g], nowEpoch());
  if (a.act == cc::Act::Continue) return;
  endFromAction(a, true, now, sClient.readStatus);
}

void stepFetching(uint32_t now) {
  if (sClient.op == cu::kOpReadReq) {
    if (sClient.statusReady) { handleReadStatus(now); return; }
    if (sClient.plan.mode == cu::kModeDigest) {
      if (sClient.digestReady) {
        sClient.op = 0;
        sJob.fileId = sClient.plan.fileId;
        sJob.offset = sClient.plan.offset;
        sJob.len = sClient.plan.maxBytes;
        memcpy(sJob.digest, sClient.digest, sizeof(sJob.digest));
        postJob(JobKind::Verify);
        sClient.stage = Stage::Verifying;
        return;
      }
      if ((uint32_t)(now - sClient.sentMs) >= sClient.timeoutMs) {
        ++sStats.timeouts;
        if (++sClient.tries >= cc::kTries) { finishRun(cc::Scheduler::Outcome::Suspend, kNoAnswerBackoffMs, cc::Why::NoAnswer); return; }
        sendRead(now);
      }
      return;
    }
    if (sWin.done) { finishWindow(now); return; }
    const bool silent = sWin.frames == 0 && (uint32_t)(now - sClient.sentMs) >= sClient.timeoutMs;
    if (sWin.timedOut(now) || silent) {
      ++sStats.timeouts;
      if (sWin.frames) { finishWindow(now); return; }   // the prefix is kept
      if (++sClient.tries >= cc::kTries) { finishRun(cc::Scheduler::Outcome::Suspend, kNoAnswerBackoffMs, cc::Why::NoAnswer); return; }
      sendRead(now);
    }
    return;
  }
  if (cc::runCeiling(sClient.runBytes, sClient.runStartMs, now)) {
    finishRun(cc::Scheduler::Outcome::Yield, 0, cc::Why::Yielded);
    return;
  }
  const cc::ReadPlan rp = cc::nextRead(sFiles, sClient.pathCap, sClient.flushed, sClient.full);
  if (!rp.any) {
    finishRun(cc::Scheduler::Outcome::Current, 0, cc::Why::None);
    return;
  }
  // Held while the send queue is deep; at most collectRateFps frames a second.
  if (espnowtx::queueDepth() >= cu::kTxDepthLimit) return;
  const uint8_t cost = rp.mode == cu::kModeData ? cc::kWindowFrames : 1;
  if (!sSched.rate.take(now, gSettings.collectRateFps, cost)) return;
  // The same request again (an empty or out-of-place answer) counts as a try.
  const bool same = rp.fileId == sClient.plan.fileId && rp.offset == sClient.plan.offset && rp.mode == sClient.plan.mode;
  if (!same) sClient.tries = 0;
  sClient.plan = rp;
  // A claim-only READ is sent once: the device's answer (or the next
  // listing) is what counts, a lost frame is claimed again next run.
  if (rp.claimOnly) {
    if (cc::FileState* cf = cc::findFile(sFiles, rp.fileId)) cf->lastSeenDelivered = rp.offset;
  }
  sendRead(now);
}

void jobFinished(uint32_t now) {
  const JobKind kind = sJob.kind;
  const bool ok = sJob.ok;
  jobSetState(kJobIdle);
  switch (kind) {
    case JobKind::Plan:
      if (!ok) {
        sFilesStale = 1;
        endRunNow(cc::Scheduler::Outcome::CardError, now, 0, cc::Why::CardError);
        return;
      }
      if (sJob.ledgerReset) {
        ++sStats.ledgerResets;
        INFO_ESPNOWF("[COLLECT] " MAC_FMT " made a new file ledger: its earlier files are in index-archive.jsonl",
                     MAC_ARGS(sClient.mac));
      }
      sSched.dev[sClient.dev].state = cc::DevState::Fetching;
      sClient.stage = Stage::Fetching;
      sClient.flushed = true;
      return;
    case JobKind::Write: {
      cc::Device& d = sSched.dev[sClient.dev];
      sClient.runBytes += sJob.committed;
      sStats.bytes += sJob.committed;
      d.bps = cc::ewmaBps(d.bps, sJob.committed, (uint32_t)(now - sClient.windowSentMs));
      sWin.clear();
      // The next READ may claim storedBefore only for bytes the saved index
      // can recover.
      sClient.flushed = sJob.claimSafe && (sJob.committed > 0 || ok);
      sClient.stage = Stage::Fetching;
      if (!ok && sJob.committed == 0) {
        sFilesStale = sJob.rejected ? 1 : sFilesStale;
        finishRun(sJob.rejected ? cc::Scheduler::Outcome::Relist : cc::Scheduler::Outcome::CardError,
                  sJob.rejected ? cc::kPastEndRelistMs : 0, sJob.rejected ? cc::Why::None : cc::Why::CardError);
      }
      return;
    }
    case JobKind::Verify:
      sClient.stage = Stage::Fetching;
      return;
    case JobKind::Finish:
      if (sClient.endOutcome == cc::Scheduler::Outcome::Current && !ok) {
        endRunNow(cc::Scheduler::Outcome::CardError, now, 0, cc::Why::CardError);
        return;
      }
      endRunNow(sClient.endOutcome, now, sClient.endBackoffMs, sClient.endWhy);
      return;
    case JobKind::None:
      return;
  }
}

void refreshGate(uint32_t now) {
  if (sLastGateMs && (uint32_t)(now - sLastGateMs) < kGateEveryMs) return;
  sLastGateMs = now ? now : 1;
  const uint8_t role = meshEnabled() ? gSettings.meshRole : 0xFF;
  sGate = cc::gate(true, gSettings.collectEnabled, role, VFS::isSdWritable(), Clock::isSynced());
  const uint32_t gen = VFS::sdMountGeneration();
  const bool open = sGate == cc::Gate::Open;
  // After this hub boots, or a card is put in: every peer once, 1 s apart.
  if (open && (!sGateWasOpen || gen != sSdGen)) {
    sSched.bootSweep(now);
    if (gen != sSdGen) sFilesStale = 1;
  }
  sGateWasOpen = open ? 1 : 0;
  sSdGen = gen;
}

uint8_t batteryPctOf(const uint8_t mac[6]) {
  const size_t n = espnowBatteryJsonFor(mac, nullptr, sBatteryJson, sizeof(sBatteryJson));
  if (!n) return cc::kBatteryUnknown;
  const char* p = strstr(sBatteryJson, "\"percent\":");
  if (!p) return cc::kBatteryUnknown;
  p += 10;
  if (*p < '0' || *p > '9') return cc::kBatteryUnknown;   // null
  const unsigned long v = strtoul(p, nullptr, 10);
  return v > 100 ? 100 : (uint8_t)v;
}

void refreshPeers(uint32_t now) {
  if (sLastPeersMs && (uint32_t)(now - sLastPeersMs) < kPeersEveryMs) return;
  sLastPeersMs = now ? now : 1;
  const bool battery = !sLastBatteryMs || (uint32_t)(now - sLastBatteryMs) >= kBatteryEveryMs;
  if (battery) sLastBatteryMs = now ? now : 1;
  uint8_t seen[cc::kMaxDevices] = {};
  const uint8_t slots = sessionSlotCount();
  for (uint8_t i = 0; i < slots; ++i) {
    const SessionState* s = sessionAt(i);
    if (!s || s->state != SESSION_ACTIVE) continue;
    uint32_t caps = 0;
    bool confirmed = false;
    const bool capsKnown = espnowPeerSessionCaps(s->peerMac, &caps, &confirmed) && confirmed;
    const cc::Device* known = sSched.find(s->peerMac);
    const uint8_t pct = battery ? batteryPctOf(s->peerMac) : (known ? known->batteryPct : cc::kBatteryUnknown);
    sSched.noteSession(s->peerMac, espnowSessionIdOf(s->peerMac), espnowSessionSettled(s->peerMac, now), capsKnown,
                       (caps & cu::kCapFilesServe) != 0, pct, now);
    if (const cc::Device* d = sSched.find(s->peerMac)) seen[d - sSched.dev] = 1;
  }
  for (uint8_t i = 0; i < cc::kMaxDevices; ++i)
    if (sSched.dev[i].used && !seen[i]) sSched.noteNoSession(sSched.dev[i]);
}

void tickLocked(uint32_t now) {
  refreshGate(now);
  refreshPeers(now);
  sSched.periodic(now);
  const uint8_t js = jobState();
  if (js == kJobPosted || js == kJobRunning) return;   // the main loop has the run's state
  if (js == kJobDone) jobFinished(now);
  const bool open = sGate == cc::Gate::Open;
  if (sClient.stage == Stage::Idle) {
    if (open) startNext(now);
    return;
  }
  // The gates closed (collect off, card out, role changed) or the session went.
  const bool sessionUp = espnowSessionUp(sClient.mac);
  if (sClient.stage == Stage::Listing) {
    if (!open) { endRunNow(cc::Scheduler::Outcome::Backoff, now, kGateClosedBackoffMs, cc::Why::None); return; }
    if (!sessionUp) { endRunNow(cc::Scheduler::Outcome::Suspend, now, 0, cc::Why::NoSession); return; }
    stepListing(now);
    return;
  }
  if (sClient.stage == Stage::Fetching) {
    if (!open) { finishRun(cc::Scheduler::Outcome::Backoff, kGateClosedBackoffMs, cc::Why::None); return; }
    if (!sessionUp) { finishRun(cc::Scheduler::Outcome::Suspend, 0, cc::Why::NoSession); return; }
    stepFetching(now);
  }
}

// ---- Command helpers (cmd_exec) ---------------------------------------------
void say(const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vsnprintf(sLine, sizeof(sLine), fmt, args);
  va_end(args);
  broadcastOutput(sLine);
}
bool app(char* out, size_t cap, size_t* pos, const char* fmt, ...) {
  if (*pos >= cap) return false;
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(out + *pos, cap - *pos, fmt, args);
  va_end(args);
  if (n < 0 || (size_t)n >= cap - *pos) { out[*pos] = 0; *pos = cap; return false; }
  *pos += (size_t)n;
  return true;
}
// "1,048,211"
void fmtNum(uint64_t v, char* out, size_t cap) {
  char raw[24];
  snprintf(raw, sizeof(raw), "%llu", (unsigned long long)v);
  const size_t n = strlen(raw);
  size_t w = 0;
  for (size_t i = 0; i < n && w + 2 < cap; ++i) {
    if (i && (n - i) % 3 == 0) out[w++] = ',';
    out[w++] = raw[i];
  }
  out[w] = 0;
}
// "412 KB", "1.6 MB", "28.1 GB"
void fmtBytes(uint64_t b, char* out, size_t cap) {
  if (b < 1024) snprintf(out, cap, "%u B", (unsigned)b);
  else if (b < (1ull << 20)) snprintf(out, cap, "%u KB", (unsigned)(b >> 10));
  else if (b < (1ull << 30)) snprintf(out, cap, "%u.%u MB", (unsigned)(b >> 20), (unsigned)(((b & 0xFFFFF) * 10) >> 20));
  else snprintf(out, cap, "%u.%u GB", (unsigned)(b >> 30), (unsigned)(((b & 0x3FFFFFFF) * 10) >> 30));
}
void fmtAgo(uint32_t ms, char* out, size_t cap) {
  const uint32_t s = ms / 1000;
  if (s < 120) snprintf(out, cap, "%u s ago", (unsigned)s);
  else if (s < 7200) snprintf(out, cap, "%u min ago", (unsigned)(s / 60));
  else snprintf(out, cap, "%u h ago", (unsigned)(s / 3600));
}
void fmtEta(uint32_t s, char* out, size_t cap) {
  if (s == 0xFFFFFFFFu) { out[0] = 0; return; }
  if (s < 90) snprintf(out, cap, ", about %u s left", (unsigned)(s ? s : 1));
  else if (s < 5400) snprintf(out, cap, ", about %u min left", (unsigned)((s + 30) / 60));
  else snprintf(out, cap, ", about %u h left", (unsigned)((s + 1800) / 3600));
}
void fmtHhmm(uint32_t epoch, char* out, size_t cap) {
  char t[24];
  if (!collectstore::formatUtc(epoch, t, sizeof(t))) { snprintf(out, cap, "never"); return; }
  snprintf(out, cap, "%.5s UTC", t + 11);
}
void deviceName(const uint8_t mac[6], char* out, size_t cap) {
  const String name = getEspNowDeviceName(mac);   // transient (cmd_exec)
  if (name.length()) strlcpy(out, name.c_str(), cap);
  else macToDisplay(mac, out, cap);
}
const char* gapText(uint8_t why) {
  switch (why) {
    case cc::kGapBeforeFirstFetch: return "had already left the device when this hub first saw the file";
    case cc::kGapTrimmed:          return "were trimmed on the device before they were fetched";
    case cc::kGapLostOnDevice:     return "were lost on the device before they were fetched";
    case cc::kGapNoLongerOffered:  return "are no longer offered by the device";
    default:                       return "are missing";
  }
}
const char* gateText(cc::Gate g) {
  switch (g) {
    case cc::Gate::Off:     return "collecting is off (collect on)";
    case cc::Gate::Role:    return "this device is not a mesh master or backup master";
    case cc::Gate::NoCard:  return "no writable SD card";
    case cc::Gate::NoClock: return "the clock is not set";
    default:                return "";
  }
}
cc::Gate gateNow() {
  const uint8_t role = meshEnabled() ? gSettings.meshRole : 0xFF;
  return cc::gate(true, gSettings.collectEnabled, role, VFS::isSdWritable(), Clock::isSynced());
}

struct Args {
  bool json, full, bad, haveFrom;
  uint32_t from;
  char device[48];
};
void parseArgs(const String& input, Args* a) {
  memset(a, 0, sizeof(*a));
  char buf[128];
  strlcpy(buf, input.c_str(), sizeof(buf));
  char* save = nullptr;
  for (char* t = strtok_r(buf, " \t", &save); t; t = strtok_r(nullptr, " \t", &save)) {
    if (strcasecmp(t, "json") == 0) { a->json = true; continue; }
    if (strcasecmp(t, "full") == 0) { a->full = true; continue; }
    if (strcasecmp(t, "from") == 0) {
      char* n = strtok_r(nullptr, " \t", &save);
      char* end = nullptr;
      const unsigned long v = n ? strtoul(n, &end, 10) : 0;
      if (!n || *end) { a->bad = true; return; }
      a->from = (uint32_t)v;
      a->haveFrom = true;
      continue;
    }
    if (a->device[0]) { a->bad = true; return; }
    strlcpy(a->device, t, sizeof(a->device));
  }
}

// Copies what the commands show; false when the lock is not free in time.
bool snapshot(const uint8_t* wantMac, bool* filesCopied) {
  *filesCopied = false;
  if (!sLock) { memset(&sSchedSnap, 0, sizeof(sSchedSnap)); memset(sGapSnap, 0, sizeof(sGapSnap)); return true; }
  if (xSemaphoreTake(sLock, kCommandLockTicks) != pdTRUE) return false;
  memcpy(&sSchedSnap, &sSched, sizeof(sSchedSnap));
  memcpy(sGapSnap, sGapNotes, sizeof(sGapSnap));
  sGapSnapNext = sGapNext;
  if (wantMac && sFiles.used && memcmp(sFiles.mac, wantMac, 6) == 0) {
    memcpy(&sFilesSnap, &sFiles, sizeof(sFilesSnap));
    *filesCopied = true;
  }
  xSemaphoreGive(sLock);
  return true;
}

const char* stateText(const cc::Device& d, char* out, size_t cap, uint32_t nowMs) {
  char a[24], b[24], eta[40], ago[24];
  switch (d.state) {
    case cc::DevState::Listing:
      snprintf(out, cap, "listing its files");
      break;
    case cc::DevState::Fetching:
    case cc::DevState::Verifying: {
      fmtBytes(d.held, a, sizeof(a));
      fmtBytes((uint64_t)d.held + d.bytesLeft, b, sizeof(b));
      fmtEta(cc::etaSeconds(d.bytesLeft, d.bps), eta, sizeof(eta));
      char name[100];
      cu::displayName(d.fetchingName, strlen(d.fetchingName), name, sizeof(name));
      snprintf(out, cap, "%s %s - %u of %u files, %s of %s (%u %%)%s",
               d.state == cc::DevState::Verifying ? "checking" : "fetching", name, (unsigned)(d.files - d.filesLeft),
               (unsigned)d.files, a, b, (unsigned)d.pct, eta);
      break;
    }
    case cc::DevState::Current:
      fmtHhmm(d.lastCaughtUpEpoch, a, sizeof(a));
      fmtBytes(d.bytesOnCard, b, sizeof(b));
      snprintf(out, cap, "up to date, caught up %s; %u files, %s on the card", a, (unsigned)d.files, b);
      break;
    case cc::DevState::Suspended:
      snprintf(out, cap, "suspended: %s", d.why == cc::Why::None ? "waiting for its session" : cc::whyText(d.why));
      break;
    case cc::DevState::Waiting:
      fmtAgo(nowMs - d.lastHeardMs, ago, sizeof(ago));
      snprintf(out, cap, "waiting: no session (last heard %s)", ago);
      break;
    case cc::DevState::Refused:
      snprintf(out, cap, "%s", cc::whyText(cc::Why::Refused));
      break;
    case cc::DevState::Unsupported:
      snprintf(out, cap, "no file catch-up in its firmware");
      break;
    case cc::DevState::Idle:
    default:
      if (d.why == cc::Why::NoFileServe) snprintf(out, cap, "no file catch-up in its firmware");
      else if (d.due && d.why != cc::Why::None) snprintf(out, cap, "queued (%s)", cc::whyText(d.why));
      else if (d.due) snprintf(out, cap, "queued");
      else if (d.lastCaughtUpEpoch) {
        fmtHhmm(d.lastCaughtUpEpoch, a, sizeof(a));
        snprintf(out, cap, "idle, caught up %s", a);
      } else snprintf(out, cap, "idle");
      break;
  }
  return out;
}

// One device as a JSON object (no `files`).
size_t deviceJson(const uint8_t mac[6], const cc::Device* d, uint32_t nowMs, char* out, size_t cap) {
  char name[48], nameJson[100], t[24];
  deviceName(mac, name, sizeof(name));
  espnowsensor::jsonTextEscape(name, nameJson, sizeof(nameJson));
  size_t pos = 0;
  app(out, cap, &pos, "{\"mac\":\"" MAC_FMT "\",\"name\":\"%s\"", MAC_ARGS(mac), nameJson);
  if (!d) {
    app(out, cap, &pos, ",\"state\":\"waiting\",\"whyNot\":\"no_session\",\"files\":null}");
    return pos;
  }
  const char* why = d->why == cc::Why::None ? nullptr : cc::whyKey(d->why);
  app(out, cap, &pos, ",\"state\":\"%s\",", cc::devStateKey(d->state));
  if (why) app(out, cap, &pos, "\"whyNot\":\"%s\"", why);
  else app(out, cap, &pos, "\"whyNot\":null");
  const uint32_t eta = cc::etaSeconds(d->bytesLeft, d->bps);
  app(out, cap, &pos, ",\"files\":%u,\"filesLeft\":%u,\"bytesLeft\":%lu,\"pct\":%u,", (unsigned)d->files,
      (unsigned)d->filesLeft, (unsigned long)d->bytesLeft, (unsigned)d->pct);
  if (eta == 0xFFFFFFFFu) app(out, cap, &pos, "\"etaS\":null,");
  else app(out, cap, &pos, "\"etaS\":%lu,", (unsigned long)eta);
  if (d->lastCaughtUpEpoch && collectstore::formatUtc(d->lastCaughtUpEpoch, t, sizeof(t)))
    app(out, cap, &pos, "\"lastCaughtUpUtc\":\"%s\",", t);
  else app(out, cap, &pos, "\"lastCaughtUpUtc\":null,");
  app(out, cap, &pos, "\"bytesOnCard\":%llu,\"gaps\":{\"beforeFirstFetch\":%lu,\"trimmed\":%lu,\"lostOnDevice\":%lu,"
      "\"noLongerOffered\":%lu},\"elsewhereBytes\":%lu,\"lastHeardAgoMs\":%lu}",
      (unsigned long long)d->bytesOnCard, (unsigned long)d->gaps[cc::kGapBeforeFirstFetch],
      (unsigned long)d->gaps[cc::kGapTrimmed], (unsigned long)d->gaps[cc::kGapLostOnDevice],
      (unsigned long)d->gaps[cc::kGapNoLongerOffered], (unsigned long)d->elsewhere,
      (unsigned long)(nowMs - d->lastHeardMs));
  return pos;
}

size_t fileJson(const cc::DeviceFiles& d, const cc::FileState& f, char* out, size_t cap) {
  char name[200], t[24], leaf[40];
  espnowsensor::jsonTextEscape(f.name, name, sizeof(name));
  const char* kind = cu::kindName(cu::Kind(f.kind));
  const char* seal = (f.flags & cc::kFfSealOnSend) ? "on_send" : (f.flags & cc::kFfSealedAtRest) ? "at_rest" : "none";
  size_t pos = 0;
  app(out, cap, &pos, "{\"fileId\":%lu,\"kind\":\"%s\",\"name\":\"%s\",\"onSd\":%s,\"seal\":\"%s\",\"createdBoot\":%lu,",
      (unsigned long)f.fileId, kind ? kind : "?", name, (f.flags & cc::kFfOnSd) ? "true" : "false", seal,
      (unsigned long)f.createdBoot);
  if (f.firstSeenEpoch && collectstore::formatUtc(f.firstSeenEpoch, t, sizeof(t))) app(out, cap, &pos, "\"firstSeenUtc\":\"%s\",", t);
  else app(out, cap, &pos, "\"firstSeenUtc\":null,");
  app(out, cap, &pos, "\"deviceStart\":%lu,\"lastSeenEnd\":%lu,\"lastSeenDelivered\":%lu,\"held\":%lu,\"pos\":%lu,\"segments\":[",
      (unsigned long)f.deviceStart, (unsigned long)f.lastSeenEnd, (unsigned long)f.lastSeenDelivered,
      (unsigned long)cc::heldBytes(f), (unsigned long)f.pos);
  for (uint8_t i = 0; i < f.nSeg; ++i) {
    cu::localSegmentName(d.ledgerId, f.fileId, f.seg[i].seg, cc::isSealOnSend(f), leaf, sizeof(leaf));
    app(out, cap, &pos, "%s{\"file\":\"%s\",\"from\":%lu,\"to\":%lu}", i ? "," : "", leaf, (unsigned long)f.seg[i].from,
        (unsigned long)f.seg[i].to);
  }
  app(out, cap, &pos, "],\"gaps\":[");
  for (uint8_t i = 0; i < f.nGap; ++i)
    app(out, cap, &pos, "%s{\"from\":%lu,\"to\":%lu,\"why\":\"%s\"}", i ? "," : "", (unsigned long)f.gap[i].from,
        (unsigned long)f.gap[i].to, cc::gapWhyKey(f.gap[i].why));
  app(out, cap, &pos, "],\"elsewhere\":[");
  for (uint8_t i = 0; i < f.nElse; ++i)
    app(out, cap, &pos, "%s{\"from\":%lu,\"to\":%lu}", i ? "," : "", (unsigned long)f.elsewhere[i].from,
        (unsigned long)f.elsewhere[i].to);
  const char* dg = cc::digestKey(f.digest);
  app(out, cap, &pos, "],\"state\":\"%s\",\"digest\":%s%s%s%s}", cc::fstateKey(f.state), dg ? "\"" : "", dg ? dg : "null",
      dg ? "\"" : "", (f.flags & cc::kFfEvicted) ? ",\"evicted\":true" : "");
  return pos;
}

// The devices collectstatus lists: the scheduler's, then paired ones it has not seen.
uint8_t statusMacs(uint8_t macs[][6], uint8_t max) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < cc::kMaxDevices && n < max; ++i)
    if (sSchedSnap.dev[i].used) memcpy(macs[n++], sSchedSnap.dev[i].mac, 6);
  for (uint8_t i = 0; n < max; ++i) {
    const PeerIdentity* p = peerIdentityAt(i);
    if (!p) break;
    if (!p->valid) continue;
    bool have = false;
    for (uint8_t k = 0; k < n && !have; ++k) have = memcmp(macs[k], p->mac, 6) == 0;
    if (!have) memcpy(macs[n++], p->mac, 6);
  }
  return n;
}

EXT_RAM_BSS_ATTR uint8_t sMacs[32][6];
EXT_RAM_BSS_ATTR char sObj[1536];

const char* statusOne(const Args& a, const uint8_t mac[6], char* reply, size_t cap) {
  bool copied = false;
  if (!snapshot(mac, &copied)) return "Error: the collector is busy; try again";
  if (!copied) {
    collectstore::LoadResult lr = {};
    if (!collectstore::loadIndex(mac, "", nowEpoch(), sFilesSnap, &lr, /*readOnly=*/true))
      return "Error: the SD card cannot be read";
  }
  const cc::Device* d = sSchedSnap.find(mac);
  const uint32_t nowMs = (uint32_t)millis();
  char name[48];
  deviceName(mac, name, sizeof(name));
  if (a.json) {
    size_t pos = 0;
    deviceJson(mac, d, nowMs, sObj, sizeof(sObj));
    app(reply, cap, &pos, "{\"schema\":1,\"collect\":%s,\"built\":true,\"device\":%s,\"files\":[",
        gSettings.collectEnabled ? "true" : "false", sObj);
    uint32_t next = 0;
    bool first = true;
    for (uint8_t i = (uint8_t)(a.from < sFilesSnap.count ? a.from : sFilesSnap.count); i < sFilesSnap.count; ++i) {
      const size_t n = fileJson(sFilesSnap, sFilesSnap.f[i], sObj, sizeof(sObj));
      if (pos + n + 32 >= cap) { next = i; break; }
      app(reply, cap, &pos, "%s%s", first ? "" : ",", sObj);
      first = false;
    }
    if (next) app(reply, cap, &pos, "],\"next\":%lu}", (unsigned long)next);
    else app(reply, cap, &pos, "],\"next\":null}");
    return reply;
  }
  char st[200];
  say("[ESP-NOW] Collector: the files of %s (" MAC_FMT ")", name, MAC_ARGS(mac));
  say("  %s", d ? stateText(*d, st, sizeof(st), nowMs) : "waiting: no session");
  if (!sFilesSnap.count) {
    snprintf(reply, cap, "OK: nothing pulled from %s yet", name);
    return reply;
  }
  uint64_t held = 0;
  for (uint8_t i = (uint8_t)(a.from < sFilesSnap.count ? a.from : sFilesSnap.count); i < sFilesSnap.count; ++i) {
    const cc::FileState& f = sFilesSnap.f[i];
    char nm[200], h[24], e[24];
    cu::displayName(f.name, strlen(f.name), nm, sizeof(nm));
    fmtNum(cc::heldBytes(f), h, sizeof(h));
    fmtNum(f.lastSeenEnd, e, sizeof(e));
    held += cc::heldBytes(f);
    const char* kind = cu::kindName(cu::Kind(f.kind));
    const char* dg = cc::digestKey(f.digest);
    say("  #%lu %-10s %s  %s/%s  %s%s%s%s%s", (unsigned long)f.fileId, kind ? kind : "?", nm[0] ? nm : "(no name)", h, e,
        cc::fstateKey(f.state), (f.flags & cc::kFfSealOnSend) ? ", sealed on send" : (f.flags & cc::kFfSealedAtRest) ? ", sealed at rest" : "",
        dg ? ", digest " : "", dg ? dg : "", (f.flags & cc::kFfEvicted) ? ", evicted for the card quota" : "");
    for (uint8_t g = 0; g < f.nGap; ++g) {
      char from[24], to[24];
      fmtNum(f.gap[g].from, from, sizeof(from));
      fmtNum(f.gap[g].to, to, sizeof(to));
      say("      gap %s-%s: %s", from, to, gapText(f.gap[g].why));
    }
    for (uint8_t k = 0; k < f.nElse; ++k) {
      char from[24], to[24];
      fmtNum(f.elsewhere[k].from, from, sizeof(from));
      fmtNum(f.elsewhere[k].to, to, sizeof(to));
      say("      %s-%s: at the other master", from, to);
    }
  }
  char hb[24];
  fmtBytes(held, hb, sizeof(hb));
  snprintf(reply, cap, "OK: %u files from %s, %s held", (unsigned)sFilesSnap.count, name, hb);
  return reply;
}

}  // namespace

// ---------------------------------------------------------------------------
// The receive handlers (espnow_task): only what matches the one outstanding
// request is taken.
// ---------------------------------------------------------------------------
V4Verdict v4hCatchupListResp(const V4RxCtx& ctx) {
  const uint8_t* src = ctx.recv_info->src_addr;
  cu::ListRespHeader h;
  if (!cu::parseListRespHeader(ctx.payload, ctx.payloadLen, &h)) { ++sStats.malformed; return ctx.refuseWith(espnowanswer::kBadRequest); }
  if (sClient.stage != Stage::Listing || sClient.op != cu::kOpListReq || sClient.pageReady || h.reqId != sClient.reqId ||
      memcmp(src, sClient.mac, 6) != 0) {
    ++sStats.unexpected;
    return ctx.refuse();
  }
  sClient.listStatus = h.status;
  sClient.listRetry = h.status == cu::kStBusy ? cu::busyRetryAfter(ctx.payload, ctx.payloadLen, h.headerLen) : 0;
  sClient.listNext = 0;
  if (h.status == cu::kStOk) {
    if (!cc::listingPage(sListing, h)) {
      sClient.ledgerMoved = true;
    } else {
      cu::EntryCursor cur = cu::entries(ctx.payload, ctx.payloadLen, h);
      cu::Entry e;
      uint8_t got = 0;
      while (cur.next(&e)) { cc::listingAdd(sListing, e); ++got; }
      if (got != h.entryCount) ++sStats.malformed;
      sClient.listNext = h.nextFileId;
    }
  }
  sClient.pageReady = true;
  return ctx.accepted();
}

V4Verdict v4hCatchupReadResp(const V4RxCtx& ctx) {
  const uint8_t* src = ctx.recv_info->src_addr;
  cu::ReadRespHeader h;
  const uint8_t* data = nullptr;
  size_t dataLen = 0;
  if (!cu::parseReadResp(ctx.payload, ctx.payloadLen, &h, &data, &dataLen)) { ++sStats.malformed; return ctx.refuseWith(espnowanswer::kBadRequest); }
  if (sClient.stage != Stage::Fetching || sClient.op != cu::kOpReadReq || h.reqId != sClient.reqId ||
      memcmp(src, sClient.mac, 6) != 0 || sClient.statusReady || sClient.digestReady) {
    ++sStats.unexpected;
    return ctx.refuse();
  }
  if (h.status != cu::kStOk) {
    sClient.readStatus = h.status;
    sClient.readOffset = h.offset;
    sClient.readRetry = h.status == cu::kStBusy ? cu::busyRetryAfter(ctx.payload, ctx.payloadLen, h.headerLen) : 0;
    sClient.statusReady = true;
    return ctx.accepted();  // the answer's status is stored for the client
  }
  if (h.fileId != sClient.plan.fileId) { ++sStats.unexpected; return ctx.refuse(); }
  if (sClient.plan.mode == cu::kModeDigest) {
    if (!(h.flags & cu::kRfDigest) || dataLen < cu::kDigestLen) { ++sStats.malformed; return ctx.refuseWith(espnowanswer::kBadRequest); }
    memcpy(sClient.digest, data, cu::kDigestLen);
    sClient.digestReady = true;
    return ctx.accepted();
  }
  const cc::WindowAssembler::Took t = sWin.onFrame(h, data, dataLen, (uint32_t)millis());
  if (t == cc::WindowAssembler::Took::Ignored) ++sStats.unexpected;
  return ctx.accepted();
}

// ---------------------------------------------------------------------------
// espnow_task (step 9f2f)
// ---------------------------------------------------------------------------
void collectTick(uint32_t nowMs) {
  if (!sLock) {
    sLock = xSemaphoreCreateMutexWithCaps(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!sLock) return;
  }
  if (xSemaphoreTake(sLock, 0) != pdTRUE) return;
  tickLocked(nowMs);
  xSemaphoreGive(sLock);
}

// ---------------------------------------------------------------------------
// Main loop: the card job collectTick posted.
// ---------------------------------------------------------------------------
void collectWriteTick() {
  if (!sLock) return;
  portENTER_CRITICAL(&sJobMux);
  const bool posted = sJob.state == kJobPosted;
  if (posted) sJob.state = kJobRunning;
  portEXIT_CRITICAL(&sJobMux);
  if (!posted) return;
  if (xSemaphoreTake(sLock, kWriterLockTicks) != pdTRUE) {
    jobSetState(kJobPosted);   // next loop
    return;
  }
  runJob();
  xSemaphoreGive(sLock);
  jobSetState(kJobDone);
}

// ---------------------------------------------------------------------------
// Commands (cmd_exec, admin)
// ---------------------------------------------------------------------------

// collectstatus [json] [<device>] [from <n>]
const char* cmd_collectstatus(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  char* reply = getDebugBuffer();
  const size_t cap = GLOBAL_DEBUG_BUFFER_SIZE;
  Args a;
  parseArgs(argsInput, &a);
  if (a.bad || a.full) return "Usage: collectstatus [json] [<device>] [from <n>]";
  if (a.device[0]) {
    uint8_t mac[6];
    if (!resolveDeviceNameOrMac(String(a.device), mac)) {
      snprintf(reply, cap, "Error: no paired device named %s", a.device);
      return reply;
    }
    return statusOne(a, mac, reply, cap);
  }
  bool copied = false;
  if (!snapshot(nullptr, &copied)) return "Error: the collector is busy; try again";
  const uint32_t nowMs = (uint32_t)millis();
  const cc::Gate g = gateNow();
  const bool mounted = VFS::isSdAvailable();
  uint64_t total = 0, used = 0, freeBytes = 0;
  const bool stats = mounted && VFS::getStats(VFS::SDCARD, total, used, freeBytes);
  const bool clockSet = Clock::isSynced();
  const char* source = Clock::syncSourceName(Clock::syncSource());
  const char* role = getMeshRoleString(gSettings.meshRole);
  const uint8_t n = statusMacs(sMacs, 32);
  if (a.json) {
    size_t pos = 0;
    app(reply, cap, &pos, "{\"schema\":1,\"collect\":%s,\"built\":true,", gSettings.collectEnabled ? "true" : "false");
    const char* gk = cc::gateKey(g);
    if (gk) app(reply, cap, &pos, "\"whyNot\":\"%s\",", gk);
    else app(reply, cap, &pos, "\"whyNot\":null,");
    app(reply, cap, &pos, "\"card\":{\"mounted\":%s,\"writable\":%s,\"freeBytes\":", mounted ? "true" : "false",
        VFS::isSdWritable() ? "true" : "false");
    if (stats) app(reply, cap, &pos, "%llu},", (unsigned long long)freeBytes);
    else app(reply, cap, &pos, "null},");
    app(reply, cap, &pos, "\"clock\":{\"set\":%s,\"source\":\"%s\"},\"role\":\"%s\",\"rateFps\":%u,\"quotaMb\":%u,"
        "\"devices\":[", clockSet ? "true" : "false", source ? source : "none", role, (unsigned)gSettings.collectRateFps,
        (unsigned)gSettings.collectQuotaMb);
    uint32_t next = 0;
    bool first = true;
    for (uint8_t i = (uint8_t)(a.from < n ? a.from : n); i < n; ++i) {
      const size_t len = deviceJson(sMacs[i], sSchedSnap.find(sMacs[i]), nowMs, sObj, sizeof(sObj));
      if (pos + len + 32 >= cap) { next = i; break; }
      app(reply, cap, &pos, "%s%s", first ? "" : ",", sObj);
      first = false;
    }
    if (next) app(reply, cap, &pos, "],\"next\":%lu}", (unsigned long)next);
    else app(reply, cap, &pos, "],\"next\":null}");
    return reply;
  }
  char freeText[24];
  if (stats) fmtBytes(freeBytes, freeText, sizeof(freeText));
  say("[ESP-NOW] Collector: files pulled from the devices that name this hub");
  say("  Collect: %s%s%s; card: %s%s%s; clock: %s%s%s%s; role: %s", gSettings.collectEnabled ? "on" : "off",
      g != cc::Gate::Open && g != cc::Gate::Off ? ", not pulling: " : "", g != cc::Gate::Open && g != cc::Gate::Off ? gateText(g) : "",
      mounted ? (VFS::isSdWritable() ? "mounted" : "mounted, read-only") : "none", stats ? ", " : "", stats ? freeText : "",
      clockSet ? "set" : "not set", clockSet ? " (" : "", clockSet && source ? source : "", clockSet ? ")" : "", role);
  unsigned fetching = 0;
  char st[220];
  for (uint8_t i = (uint8_t)(a.from < n ? a.from : n); i < n; ++i) {
    char name[48], mac[18];
    deviceName(sMacs[i], name, sizeof(name));
    macToDisplay(sMacs[i], mac, sizeof(mac));
    const cc::Device* d = sSchedSnap.find(sMacs[i]);
    if (d && (d->state == cc::DevState::Fetching || d->state == cc::DevState::Verifying || d->state == cc::DevState::Listing))
      ++fetching;
    if (strcmp(name, mac) == 0) say("  %s: %s", name, d ? stateText(*d, st, sizeof(st), nowMs) : "waiting: no session");
    else say("  %s (%s): %s", name, mac, d ? stateText(*d, st, sizeof(st), nowMs) : "waiting: no session");
  }
  for (uint8_t k = 0; k < kGapNotes; ++k) {
    const GapNote& gn = sGapSnap[(sGapSnapNext + k) % kGapNotes];
    if (!gn.used || !gn.bytes) continue;
    char name[48], bytes[24], file[100];
    deviceName(gn.mac, name, sizeof(name));
    fmtNum(gn.bytes, bytes, sizeof(bytes));
    cu::displayName(gn.name, strlen(gn.name), file, sizeof(file));
    say("  %s earlier: %s bytes of %s %s", name, bytes, file, gapText(gn.why));
  }
  snprintf(reply, cap, "OK: %u device%s, %u fetching", (unsigned)n, n == 1 ? "" : "s", fetching);
  return reply;
}

// collectnow <device> [full]
const char* cmd_collectnow(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  char* reply = getDebugBuffer();
  const size_t cap = GLOBAL_DEBUG_BUFFER_SIZE;
  Args a;
  parseArgs(argsInput, &a);
  if (a.bad || a.json || a.haveFrom || !a.device[0]) return "Usage: collectnow <device> [full]";
  uint8_t mac[6];
  if (!resolveDeviceNameOrMac(String(a.device), mac)) {
    snprintf(reply, cap, "Error: no paired device named %s", a.device);
    return reply;
  }
  char name[48];
  deviceName(mac, name, sizeof(name));
  switch (gateNow()) {
    case cc::Gate::Off:     return "Error: collecting is off (collect on)";
    case cc::Gate::Role:    return "Error: only a mesh master or backup master collects (espnowmeshrole)";
    case cc::Gate::NoCard:  return "Error: no card";
    case cc::Gate::NoClock: return "Error: the clock is not set";
    default: break;
  }
  if (!espnowSessionUp(mac)) {
    snprintf(reply, cap, "Error: %s has no session now", name);
    return reply;
  }
  uint32_t caps = 0;
  bool confirmed = false;
  if (espnowPeerSessionCaps(mac, &caps, &confirmed) && confirmed && !(caps & cu::kCapFilesServe)) {
    snprintf(reply, cap, "Error: %s's firmware has no file catch-up", name);
    return reply;
  }
  if (!sLock) return "Error: ESP-NOW is not running";
  if (xSemaphoreTake(sLock, kCommandLockTicks) != pdTRUE) return "Error: the collector is busy; try again";
  const uint32_t nowMs = (uint32_t)millis();
  cc::Device* d = sSched.touch(mac, nowMs);
  bool later = false;
  if (d) {
    sSched.force(*d, a.full, nowMs);
    const int active = sSched.activeIndex();
    later = active >= 0 && &sSched.dev[active] != d;
  }
  xSemaphoreGive(sLock);
  if (!d) return "Error: the collector's device table is full";
  snprintf(reply, cap, "OK: %s queued%s; it starts %s", name, a.full ? " (full: also what the other master has)" : "",
           later ? "after the current transfer" : "now");
  return reply;
}

static const SettingEntry kCollectSettingEntries[] = {
  { "collectEnabled", SETTING_BOOL, &gSettings.collectEnabled, 1, 0, nullptr, 0, 1, "Collect files from the devices that name this hub", nullptr, false, nullptr, "collect" },
  { "collectRateFps", SETTING_U8, &gSettings.collectRateFps, 40, 0, nullptr, 5, 100, "Collector frames per second", nullptr, false, nullptr, "collectrate" },
  { "collectQuotaMb", SETTING_U16, &gSettings.collectQuotaMb, 2048, 0, nullptr, 64, 65535, "Card quota per device (MB)", nullptr, false, nullptr, "collectquota" },
};
static const SettingEntry* collectEntry(const char* key) {
  for (const SettingEntry& e : kCollectSettingEntries)
    if (strcmp(e.jsonKey, key) == 0) return &e;
  return nullptr;
}

// collect [on|off]: with no argument, the value and what keeps it from pulling.
const char* cmd_collect(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String a = argsInput;
  a.trim();
  if (a.length()) return handleSettingCommand(collectEntry("collectEnabled"), a);
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  char* reply = getDebugBuffer();
  const cc::Gate g = gateNow();
  if (!gSettings.collectEnabled) snprintf(reply, GLOBAL_DEBUG_BUFFER_SIZE, "Collect: off (collect on starts it)");
  else if (g == cc::Gate::Open) snprintf(reply, GLOBAL_DEBUG_BUFFER_SIZE, "Collect: on, pulling (%u frames/s, %u MB per device)",
                                         (unsigned)gSettings.collectRateFps, (unsigned)gSettings.collectQuotaMb);
  else snprintf(reply, GLOBAL_DEBUG_BUFFER_SIZE, "Collect: on, not pulling: %s", gateText(g));
  return reply;
}

const char* cmd_collectrate(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return handleSettingCommand(collectEntry("collectRateFps"), argsInput);
}

const char* cmd_collectquota(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return handleSettingCommand(collectEntry("collectQuotaMb"), argsInput);
}

static bool collectModuleConnected() { return true; }
extern const SettingsModule collectSettingsModule = {
  "collect", "network.collect", kCollectSettingEntries,
  sizeof(kCollectSettingEntries) / sizeof(kCollectSettingEntries[0]),
  collectModuleConnected,
  "File catch-up: pull the files of the devices that name this hub onto the SD card"
};

extern const CommandEntry collectCommands[] = {
  { "collectstatus", "File catch-up: what this hub pulled from each device that names it.", true, cmd_collectstatus,
    "collectstatus [json] [<device>] [from <n>]" },
  { "collectnow", "Pull a device's files now (full: also what the other master has).", true, cmd_collectnow,
    "collectnow <device> [full]" },
  { "collect", "Pull files from the devices that name this hub (shows why not with no argument).", true, cmd_collect,
    "collect [on|off]" },
  { "collectrate", "Frames per second the collector asks for (5-100).", true, cmd_collectrate, "collectrate [<5..100>]" },
  { "collectquota", "Card quota per device in MB (64-65535).", true, cmd_collectquota, "collectquota [<64..65535>]", /*requiresSuperAdmin=*/true },
};
extern const size_t collectCommandsCount = sizeof(collectCommands) / sizeof(collectCommands[0]);

#endif  // ENABLE_ESPNOW_COLLECT

#endif  // ENABLE_ESPNOW
