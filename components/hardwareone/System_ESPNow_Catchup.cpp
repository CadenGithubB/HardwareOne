// System_ESPNow_Catchup.cpp - file catch-up, device side: the CATCHUP_* server
//
// See System_ESPNow_Catchup.h and FILES_IMPL_SPEC.md sections 2, 6, 7 and 9.1.
//
//   RX handler (espnow_task)  parse; reader rule; pacer; copy into the one
//                             slot; hand to cmd_exec. A refusal is answered
//                             once per (peer, session); busy at once.
//   catchupServeJob (cmd_exec) ledger (reconcile when due), LIST page or READ
//                             window (up to 8 frames, sealed on send where the
//                             kind says so) or a digest; never sends, never
//                             sleeps for pacing.
//   catchupServeTick (espnow_task) one frame per tick, 20 ms apart, only while
//                             the TX queue holds fewer than 16; a frame that
//                             will not go is retried for 500 ms, then the
//                             window is abandoned (the collector re-asks).
//
// Memory: the slot, the 8 x 202-byte frames, a 4 KB read block, the LIST
// entries, the pacer and the counters are EXT_RAM_BSS statics. Internal RAM:
// one spinlock. No task, no String member, no heap.

#include "System_ESPNow_Catchup.h"

#if ENABLE_ESPNOW

#include <stdarg.h>
#include <string.h>
#include <time.h>

#include <esp_attr.h>  // EXT_RAM_BSS_ATTR
#include "freertos/FreeRTOS.h"

#include "System_Debug.h"
#include "System_ESPNow.h"               // generateMessageId, sessions
#include "System_ESPNow_CatchupCore.h"
#include "System_ESPNow_RxCtx.h"
#include "System_ESPNow_Tx.h"            // espnowtx::sendAead, espnowtx::queueDepth

// CATCHUP_SERVE_COMMON_BEGIN (test_espnow_catchup_serve.py compiles the two marked regions)
namespace ec = espnowcatchup;

namespace {
uint8_t answerOpFor(uint8_t op) { return op == ec::kOpListReq ? ec::kOpListResp : ec::kOpReadResp; }
// Every answer: a fresh header msgId, no ACK request, ttl 1 (as EVENT_PUSH);
// correlation is by reqId. Safe from espnow_task (fire and forget).
bool csSend(const uint8_t mac[6], uint8_t op, const uint8_t* p, size_t n) {
  return espnowtx::sendAead(mac, op, 0, generateMessageId(), p, (uint16_t)n, 1);
}
// reqId and link of a request whose length the receive table has checked.
[[maybe_unused]] void requestIds(const V4RxCtx& ctx, uint8_t op, uint32_t* reqId, uint8_t* link) {
  *reqId = ctx.payload && ctx.payloadLen >= 4 ? ec::le::get32(ctx.payload) : 0;
  const size_t at = op == ec::kOpListReq ? 10 : 16;
  *link = ctx.payload && ctx.payloadLen > at ? ctx.payload[at] : 0;
}
}  // namespace
// CATCHUP_SERVE_COMMON_END

#if !ENABLE_ESPNOW_SEND

// ---------------------------------------------------------------------------
// Built without file catch-up: unsupported (3) once per peer session, then
// counted and dropped. espnow_task only (both handlers): no lock.
// ---------------------------------------------------------------------------
namespace {
struct Answered { uint8_t mac[6]; uint16_t sessionId; uint8_t used; };
EXT_RAM_BSS_ATTR Answered sAnswered[4];
EXT_RAM_BSS_ATTR uint8_t sAnsweredNext;
EXT_RAM_BSS_ATTR uint32_t sNotBuiltDropped;

void notBuilt(const V4RxCtx& ctx, uint8_t op) {
  const uint8_t* src = ctx.recv_info->src_addr;
  uint32_t reqId = 0;
  uint8_t link = 0;
  requestIds(ctx, op, &reqId, &link);
  const uint16_t sid = espnowSessionIdOf(src);
  for (const Answered& a : sAnswered) {
    if (a.used && a.sessionId == sid && memcmp(a.mac, src, 6) == 0) { ++sNotBuiltDropped; return; }
  }
  Answered& a = sAnswered[sAnsweredNext % 4];
  sAnsweredNext = (uint8_t)((sAnsweredNext + 1) % 4);
  memcpy(a.mac, src, 6);
  a.sessionId = sid;
  a.used = 1;
  if (reqId == 0) return;
  uint8_t buf[ec::kListRespHeaderLen];
  const uint8_t answer = answerOpFor(op);
  const size_t n = ec::packStatus(answer, reqId, link, ec::kStUnsupported, buf, sizeof(buf));
  if (n) (void)csSend(src, answer, buf, n);
}
}  // namespace

V4Verdict v4hCatchupListReq(const V4RxCtx& ctx) { notBuilt(ctx, ec::kOpListReq); return ctx.accepted(); }
V4Verdict v4hCatchupReadReq(const V4RxCtx& ctx) { notBuilt(ctx, ec::kOpReadReq); return ctx.accepted(); }
void catchupServeTick(uint32_t) {}

#else  // ENABLE_ESPNOW_SEND

#include <mbedtls/sha256.h>

#include "System_Battery.h"
#include "System_CaptureCrypto.h"
#include "System_CatchupLedger.h"
#include "System_Clock.h"
#include "System_CommandRegistry.h"
#include "System_ESPNow_PowerEventCore.h"  // espnowsensor::jsonTextEscape
#include "System_ESPNow_Sensors.h"         // espnowNamedMasterKind, espnowSensorBootTag, sensorReceiverTargets
#include "System_MacText.h"
#include "System_Settings.h"
#include "System_Utils.h"                  // submitDeferredToCmdExec, RETURN_VALID_IF_VALIDATE_CSTR, parseIntArg

// CATCHUP_SERVE_HOST_BEGIN
namespace {

enum class SlotState : uint8_t { Free = 0, Queued, Running, Ready };

// The one request being served (FILES_IMPL_SPEC 6.1). State moves Free ->
// Queued (RX) -> Running (cmd_exec job) -> Ready (job done) -> Free (tick);
// sOut belongs to the job while Running and to the tick while Ready.
struct ServeSlot {
  SlotState   state;
  uint8_t     op;                      // kOpListReq / kOpReadReq
  uint8_t     mac[6];
  uint16_t    sessionId;
  uint16_t    pathCap;                 // v4_single_frame_cap(mac) at intake
  ec::ListReq list;
  ec::ReadReq read;
  uint8_t     nFrames, sent;
  bool        retrying;
  uint16_t    frameLen[ec::kWindowMaxFrames];
  uint32_t    nextSendMs, retrySinceMs;
  uint32_t    queuedAtMs;              // a job lost by cmd_exec frees the slot after kQueuedMaxMs
  uint32_t    fileId, plainBytes;      // for the last-request line
};
constexpr uint32_t kQueuedMaxMs = 30000;

struct ServeStats {
  uint32_t lists, reads, digests, windowsSent, framesSent, abandoned;
  uint32_t refused, refusedSilent, busy, badRequests, sealFailures, submitFailures;
  bool     haveLast;
  uint8_t  lastMac[6];
  uint8_t  lastOp;
  uint8_t  lastFrames;
  uint32_t lastFileId, lastBytes, lastAtMs;
  time_t   lastEpoch;                  // 0 while the clock was not set
};

EXT_RAM_BSS_ATTR ServeSlot sSlot;
EXT_RAM_BSS_ATTR uint8_t sOut[ec::kWindowMaxFrames][ec::kDirectPayload];
EXT_RAM_BSS_ATTR uint8_t sRead[ec::kDigestBlock];   // one window (<= 8 x 182) or one digest block
EXT_RAM_BSS_ATTR ec::Entry sEntries[8];             // one LIST page holds at most 7 entries
EXT_RAM_BSS_ATTR ec::LedgerEntry sCur;              // the entry a READ serves, copied out of the lock
EXT_RAM_BSS_ATTR ec::ServePacer sPacer;
EXT_RAM_BSS_ATTR ServeStats sStats;
portMUX_TYPE sSlotMux = portMUX_INITIALIZER_UNLOCKED;   // the one internal-RAM item of the server

#define CS_LOCK()   portENTER_CRITICAL(&sSlotMux)
#define CS_UNLOCK() portEXIT_CRITICAL(&sSlotMux)

static_assert(sizeof(sRead) >= size_t(ec::kWindowMaxFrames) * (ec::kDirectPayload - ec::kReadRespHeaderLen),
              "the read block holds a whole window");

// ---- intake (espnow_task) ---------------------------------------------------

ec::ReaderInputs readerInputs(const uint8_t* src, uint8_t link) {
  ec::ReaderInputs in = {};
  in.built = true;
  in.serveOn = gSettings.catchupServe;
  in.link = link;
#if ENABLE_BONDED_MODE
  in.bondWorkerInBondMode = gSettings.bondModeEnabled && isBondWorker();
  in.requesterIsBondPartner = isBondPeerMac(src);
  in.bondTokenValid = isBondSessionTokenValid();
#endif
  in.namedKind = espnowNamedMasterKind(src);
  uint32_t caps = 0;
  bool confirmed = false;
  if (espnowPeerSessionCaps(src, &caps, &confirmed)) {
    in.capsConfirmed = confirmed;
    in.requesterCaps = caps;
  }
  return in;
}

// The battery floor applies only where the percentage is measured (Q14: the
// P4X-EYE has no reading, so none applies there).
void batteryInputs(bool* known, bool* discharging, uint8_t* pct) {
  const BatteryState s = getBatterySnapshot();
  *known = s.percentageValid && s.chargingKnown && !s.stale && s.percentage == s.percentage;
  *discharging = !s.isCharging;
  const float p = s.percentage == s.percentage ? s.percentage : 0.0f;
  *pct = p <= 0.0f ? 0 : p >= 100.0f ? 100 : (uint8_t)(p + 0.5f);
}

void catchupServeJob(void* arg);

void intake(const V4RxCtx& ctx, uint8_t op, uint32_t reqId, uint8_t link, uint32_t fromFileId,
            const ec::ListReq* list, const ec::ReadReq* read) {
  const uint8_t* src = ctx.recv_info->src_addr;
  const uint8_t answer = answerOpFor(op);
  uint8_t buf[ec::kListRespHeaderLen + ec::kRetryAfterLen];
  if (reqId == 0) {   // a request nobody could match an answer to
    CS_LOCK(); ++sStats.badRequests; CS_UNLOCK();
    return;
  }
  const uint32_t now = millis();
  const uint16_t sid = espnowSessionIdOf(src);
  const ec::Reader rd = ec::readerAllowed(readerInputs(src, link));
  if (rd != ec::Reader::Allowed) {
    bool answerIt;
    CS_LOCK();
    answerIt = sPacer.refusalAnsweredOnce(src, sid);
    if (answerIt) ++sStats.refused;
    else ++sStats.refusedSilent;
    CS_UNLOCK();
    if (!answerIt) return;
    const size_t n = ec::packStatus(answer, reqId, link, ec::readerStatus(rd), buf, sizeof(buf));
    if (n) (void)csSend(src, answer, buf, n);
    DEBUG_ESPNOWF("[CATCHUP] refused " MAC_FMT ": %s", MAC_ARGS(src), ec::readerName(rd));
    return;
  }
  bool known = false, discharging = false;
  uint8_t pct = 0;
  batteryInputs(&known, &discharging, &pct);
  const uint8_t depth = espnowtx::queueDepth();
  const bool settled = espnowSessionSettled(src, now);
  const uint16_t pathCap = v4_single_frame_cap(src);
  ec::ServePacer::Decision d;
  CS_LOCK();
  d = sPacer.onRequest(src, sid, op, fromFileId, now, sSlot.state != SlotState::Free, depth, settled, known,
                       discharging, pct);
  if (d.v == ec::ServePacer::Verdict::Go) {
    sSlot.state = SlotState::Queued;
    sSlot.op = op;
    memcpy(sSlot.mac, src, 6);
    sSlot.sessionId = sid;
    sSlot.pathCap = pathCap > ec::kDirectPayload ? ec::kDirectPayload : pathCap;
    if (list) sSlot.list = *list;
    if (read) sSlot.read = *read;
    sSlot.nFrames = 0;
    sSlot.sent = 0;
    sSlot.queuedAtMs = now;
  } else {
    ++sStats.busy;
  }
  CS_UNLOCK();
  if (d.v != ec::ServePacer::Verdict::Go) {
    const size_t n = ec::packBusy(answer, reqId, link, d.retryAfterS, buf, sizeof(buf));
    if (n) (void)csSend(src, answer, buf, n);
    return;
  }
  if (!submitDeferredToCmdExec(catchupServeJob, nullptr)) {
    CS_LOCK();
    sSlot.state = SlotState::Free;
    sPacer.release();
    ++sStats.submitFailures;
    ++sStats.busy;
    CS_UNLOCK();
    const size_t n = ec::packBusy(answer, reqId, link, 1, buf, sizeof(buf));
    if (n) (void)csSend(src, answer, buf, n);
  }
}

// ---- the job (cmd_exec) -----------------------------------------------------

uint16_t answerCap(const ServeSlot& req, uint8_t maxFrame) { return ec::effectiveCap(req.pathCap, maxFrame); }

uint8_t readStatusFrame(const ec::ReadReq& r, uint8_t status, uint32_t offset, uint32_t end, uint16_t* lens) {
  ec::ReadRespHeader h = {};
  h.reqId = r.reqId;
  h.status = status;
  h.link = r.link;
  h.fileId = r.fileId;
  h.offset = offset;
  h.end = end;
  lens[0] = (uint16_t)ec::packReadRespHeader(h, sOut[0], sizeof(sOut[0]));
  return 1;
}

uint8_t buildList(const ServeSlot& req, uint32_t now, uint16_t* lens) {
  const ec::ListReq& r = req.list;
  const bool keyReady = catchupLedgerKeyReady();
  const ec::LedgerState* s = catchupLedgerLock();
  if (!s) {
    lens[0] = (uint16_t)ec::packStatus(ec::kOpListResp, r.reqId, r.link, ec::kStNotReady, sOut[0], sizeof(sOut[0]));
    return 1;
  }
  uint32_t next = 0;
  uint8_t flags = 0;
  const size_t k = ec::collectListEntries(*s, r.fromFileId, r.kindMask, keyReady, nullptr, 0, now, sEntries,
                                          sizeof(sEntries) / sizeof(sEntries[0]), &next, &flags);
  ec::ListRespHeader h = {};
  h.reqId = r.reqId;
  h.status = ec::kStOk;
  h.link = r.link;
  h.nextFileId = next;
  h.ledgerGen = s->gen;
  h.ledgerId = s->ledgerId;
  h.bootTag = espnowSensorBootTag();
  h.flags = flags;
  size_t packed = 0;
  // The entries' names point into the ledger: packed before the unlock.
  const size_t n = ec::packListResp(h, sEntries, k, answerCap(req, r.maxFrame), r.maxEntries, sOut[0],
                                    sizeof(sOut[0]), &packed);
  catchupLedgerUnlock();
  lens[0] = (uint16_t)n;
  return n ? 1 : 0;
}

enum class Fetch : uint8_t { Ok, Retry, Gone, IoError };

// Opens the current file of sCur (head check), takes its growth into the
// ledger and reads up to `maxBytes` at logical `offset` into sRead.
Fetch fetch(uint32_t offset, uint32_t maxBytes, bool checkHead, uint32_t now, uint32_t* end, size_t* got) {
  uint32_t phys = 0, sizeNow = 0;
  if (!ec::toPhysical(sCur, offset, &phys)) phys = 0;
  const CatchupReadResult rr = catchupLedgerRead(sCur.fileId, checkHead, phys, sRead, maxBytes, &sizeNow, got);
  if (rr == CatchupReadResult::Missing || rr == CatchupReadResult::Shrunk || rr == CatchupReadResult::Replaced)
    return Fetch::Retry;
  if (rr != CatchupReadResult::Ok) return Fetch::IoError;
  if (sizeNow > sCur.size) {
    catchupLedgerNoteSize(sCur.fileId, sizeNow, now);
    sCur.size = sizeNow;
    sCur.lastGrowMs = now;
    sCur.flags |= ec::kLeGrowSeen;
  }
  *end = ec::logicalEnd(sCur);
  return Fetch::Ok;
}

uint8_t buildDigest(const ServeSlot& req, uint32_t now, uint16_t* lens, bool* retry) {
  const ec::ReadReq& r = req.read;
  bool allowed;
  CS_LOCK();
  allowed = sPacer.digestAllowed(req.mac, req.sessionId, r.fileId);
  CS_UNLOCK();
  if (!allowed) {
    lens[0] = (uint16_t)ec::packBusy(ec::kOpReadResp, r.reqId, r.link, 3600, sOut[0], sizeof(sOut[0]));
    return 1;
  }
  uint32_t end = 0;
  size_t got = 0;
  Fetch f = fetch(sCur.base, 0, true, now, &end, &got);
  if (f == Fetch::Retry) { *retry = true; return 0; }
  if (f != Fetch::Ok) return readStatusFrame(r, ec::kStIoError, r.offset, ec::logicalEnd(sCur), lens);
  ec::DigestPlan d;
  if (!ec::planDigest(r.offset, r.maxBytes, sCur.base, end, &d))
    return readStatusFrame(r, r.offset > end ? ec::kStPastEnd : ec::kStTooLarge, r.offset, end, lens);
  mbedtls_sha256_context sha;   // about 110 B of cmd_exec stack
  mbedtls_sha256_init(&sha);
  bool ok = mbedtls_sha256_starts(&sha, 0) == 0;
  for (uint16_t k = 0; ok && k < d.blocks; ++k) {
    uint32_t from = 0, len = 0, blockEnd = 0;
    ec::digestBlock(d, k, &from, &len);
    f = fetch(from, len, false, now, &blockEnd, &got);   // the FS lock is released between blocks
    if (f != Fetch::Ok || got != len) { ok = false; break; }
    ok = mbedtls_sha256_update(&sha, sRead, len) == 0;
    vTaskDelay(1);
  }
  uint8_t digest[32];
  ok = ok && mbedtls_sha256_finish(&sha, digest) == 0;
  mbedtls_sha256_free(&sha);
  // The file must still be the one the head names (a rotation mid-digest).
  if (ok && fetch(sCur.base, 0, true, now, &end, &got) != Fetch::Ok) { *retry = true; return 0; }
  if (!ok) return readStatusFrame(r, ec::kStIoError, r.offset, end, lens);
  ec::ReadRespHeader h = {};
  h.reqId = r.reqId;
  h.status = ec::kStOk;
  h.flags = (uint8_t)(ec::kRfDigest | ec::kRfLast | (d.to == end ? ec::kRfAtEnd : 0));
  h.link = r.link;
  h.fileId = r.fileId;
  h.offset = d.from;
  h.end = end;
  ec::packReadRespHeader(h, sOut[0], sizeof(sOut[0]));
  memcpy(sOut[0] + ec::kReadRespHeaderLen, digest, ec::kDigestLen);
  lens[0] = (uint16_t)(ec::kReadRespHeaderLen + ec::kDigestLen);
  CS_LOCK(); ++sStats.digests; CS_UNLOCK();
  return 1;
}

uint8_t buildRead(const ServeSlot& req, uint32_t now, uint16_t* lens, uint32_t* plainOut) {
  const ec::ReadReq& r = req.read;
  for (int attempt = 0; attempt < 2; ++attempt) {
    const bool keyReady = catchupLedgerKeyReady();
    uint8_t st;
    bool known = false;
    uint32_t ledgerId = 0;
    {
      const ec::LedgerState* s = catchupLedgerLock();
      if (!s) return readStatusFrame(r, ec::kStNotReady, r.offset, 0, lens);
      const ec::LedgerEntry* pe = nullptr;
      st = ec::readCheck(*s, r, keyReady, &pe);
      const ec::LedgerEntry* e = (r.link == 0 && r.ledgerId == s->ledgerId) ? ec::findById(*s, r.fileId) : nullptr;
      known = e != nullptr;
      if (e) sCur = *e;
      ledgerId = s->ledgerId;
      catchupLedgerUnlock();
    }
    // Q8: "I have stored every byte before offset" from a named master, for
    // this exact ledger. Raises `delivered` only; frees nothing on the device.
    if (attempt == 0 && known && (r.flags & ec::kRqStoredBefore)) catchupLedgerNoteDelivered(r.fileId, r.offset);
    if (st == ec::kStTrimmed) return readStatusFrame(r, st, sCur.base, ec::logicalEnd(sCur), lens);
    if (st != ec::kStOk) return readStatusFrame(r, st, r.offset, known ? ec::logicalEnd(sCur) : 0, lens);

    bool retry = false;
    if (r.mode == ec::kModeDigest) {
      const uint8_t n = buildDigest(req, now, lens, &retry);
      if (!retry) return n;
    } else {
      const bool seal = sCur.seal == uint8_t(ec::Seal::OnSend);
      const uint16_t cap = answerCap(req, r.maxFrame);
      const uint16_t perFrame = ec::planWindow(0, 1, 0, req.pathCap, r.maxFrame, seal).plainPerFrame;
      uint32_t most = uint32_t(perFrame) * ec::kWindowMaxFrames;
      if (r.maxBytes && r.maxBytes < most) most = r.maxBytes;
      uint32_t end = 0;
      size_t got = 0;
      const Fetch f = fetch(r.offset, most, true, now, &end, &got);
      if (f == Fetch::Retry) {
        retry = true;
      } else if (f != Fetch::Ok) {
        return readStatusFrame(r, ec::kStIoError, r.offset, ec::logicalEnd(sCur), lens);
      } else {
        const ec::WindowPlan w = ec::planWindow(r.offset, end, r.maxBytes, req.pathCap, r.maxFrame, seal);
        if (w.frames == 0) return readStatusFrame(r, ec::kStPastEnd, r.offset, end, lens);
        if (w.plainBytes != got) return readStatusFrame(r, ec::kStIoError, r.offset, end, lens);
        const bool grow = ec::growing(sCur, false, now);
        for (uint8_t k = 0; k < w.frames; ++k) {
          uint32_t off = 0;
          uint16_t len = 0;
          ec::windowFrame(w, r.offset, k, &off, &len);
          const bool sealThis = seal && len > 0;   // an empty at-end frame carries no data
          ec::ReadRespHeader h = {};
          h.reqId = r.reqId;
          h.status = ec::kStOk;
          h.flags = (uint8_t)((k + 1 == w.frames ? ec::kRfLast : 0) | (off + len == end ? ec::kRfAtEnd : 0) |
                              (grow ? ec::kRfGrowing : 0) | (sealThis ? ec::kRfSealedOnSend : 0));
          h.link = r.link;
          h.fileId = r.fileId;
          h.offset = off;
          h.end = end;
          uint8_t* q = sOut[k];
          ec::packReadRespHeader(h, q, sizeof(sOut[k]));
          const uint8_t* plain = sRead + (off - r.offset);
          if (sealThis) {
            uint8_t aad[ec::kAadLen];
            ec::buildSealAad(aad, ledgerId, r.fileId, off, len, sCur.kind);
            const int n = captureCryptoSealChunk(aad, sizeof(aad), plain, len, q + ec::kReadRespHeaderLen,
                                                 cap - ec::kReadRespHeaderLen);
            if (n != int(len) + int(ec::kSealOverhead)) {   // fail closed: never the plaintext
              CS_LOCK(); ++sStats.sealFailures; CS_UNLOCK();
              return readStatusFrame(r, ec::kStNotReady, r.offset, end, lens);
            }
            lens[k] = (uint16_t)(ec::kReadRespHeaderLen + n);
          } else {
            memcpy(q + ec::kReadRespHeaderLen, plain, len);
            lens[k] = (uint16_t)(ec::kReadRespHeaderLen + len);
          }
        }
        *plainOut = w.plainBytes;
        return w.frames;
      }
    }
    // The file at the entry's path is not the one the ledger knows (rotated,
    // replaced, removed): reconcile once, then look again by id.
    if (attempt == 0 && retry && catchupLedgerReconcileNow(now)) continue;
    break;
  }
  return readStatusFrame(r, ec::kStGone, r.offset, 0, lens);
}

void catchupServeJob(void*) {
  ServeSlot req;
  CS_LOCK();
  if (sSlot.state != SlotState::Queued) { CS_UNLOCK(); return; }
  sSlot.state = SlotState::Running;
  req = sSlot;
  CS_UNLOCK();
  const uint32_t now = millis();
  uint16_t lens[ec::kWindowMaxFrames] = {};
  uint8_t frames = 0;
  uint32_t plain = 0;
  const bool isList = req.op == ec::kOpListReq;
  if (!catchupLedgerEnsure(now, isList && req.list.fromFileId == 0)) {
    lens[0] = isList ? (uint16_t)ec::packStatus(ec::kOpListResp, req.list.reqId, req.list.link, ec::kStNotReady,
                                                sOut[0], sizeof(sOut[0]))
                     : (uint16_t)ec::packStatus(ec::kOpReadResp, req.read.reqId, req.read.link, ec::kStNotReady,
                                                sOut[0], sizeof(sOut[0]));
    frames = lens[0] ? 1 : 0;
  } else if (isList) {
    frames = buildList(req, now, lens);
  } else {
    frames = buildRead(req, now, lens, &plain);
  }
  CS_LOCK();
  sSlot.nFrames = frames;
  memcpy(sSlot.frameLen, lens, sizeof(lens));
  sSlot.sent = 0;
  sSlot.retrying = false;
  sSlot.nextSendMs = now;
  sSlot.fileId = isList ? 0 : req.read.fileId;
  sSlot.plainBytes = plain;
  sSlot.state = frames ? SlotState::Ready : SlotState::Free;
  if (isList) ++sStats.lists;
  else ++sStats.reads;
  CS_UNLOCK();
}

}  // namespace

// ---- RX handlers and the tick (espnow_task) ---------------------------------

V4Verdict v4hCatchupListReq(const V4RxCtx& ctx) {
  ec::ListReq r;
  if (!ec::parseListReq(ctx.payload, ctx.payloadLen, &r)) return ctx.refuseWith(espnowanswer::kBadRequest);   // the table checked minLen
  intake(ctx, ec::kOpListReq, r.reqId, r.link, r.fromFileId, &r, nullptr);
  return ctx.accepted();
}

V4Verdict v4hCatchupReadReq(const V4RxCtx& ctx) {
  ec::ReadReq r;
  if (!ec::parseReadReq(ctx.payload, ctx.payloadLen, &r)) return ctx.refuseWith(espnowanswer::kBadRequest);
  intake(ctx, ec::kOpReadReq, r.reqId, r.link, 1, nullptr, &r);
  return ctx.accepted();
}

void catchupServeTick(uint32_t nowMs) {
  uint8_t mac[6];
  uint8_t k, op;
  uint16_t len;
  uint32_t next;
  CS_LOCK();
  sPacer.expire(nowMs);
  // A queued job cmd_exec never ran (it checks for Queued and returns if the
  // slot moved on): free the slot rather than answer busy until reboot.
  if (sSlot.state == SlotState::Queued && (uint32_t)(nowMs - sSlot.queuedAtMs) >= kQueuedMaxMs) {
    sSlot.state = SlotState::Free;
    ++sStats.abandoned;
  }
  if (sSlot.state != SlotState::Ready || sSlot.sent >= sSlot.nFrames) {
    if (sSlot.state == SlotState::Ready) sSlot.state = SlotState::Free;
    CS_UNLOCK();
    return;
  }
  memcpy(mac, sSlot.mac, 6);
  k = sSlot.sent;
  op = answerOpFor(sSlot.op);
  len = sSlot.frameLen[k];
  next = sSlot.nextSendMs;
  CS_UNLOCK();
  if (!espnowSessionUp(mac)) {   // the requester went away: nothing to send to
    CS_LOCK();
    if (sSlot.state == SlotState::Ready) { sSlot.state = SlotState::Free; ++sStats.abandoned; }
    CS_UNLOCK();
    return;
  }
  if ((int32_t)(nowMs - next) < 0) return;
  if (espnowtx::queueDepth() >= ec::kTxDepthLimit) return;   // live traffic first
  const bool ok = csSend(mac, op, sOut[k], len);
  const time_t wall = time(nullptr);
  CS_LOCK();
  if (sSlot.state == SlotState::Ready && sSlot.sent == k) {
    if (ok) {
      ++sSlot.sent;
      sSlot.nextSendMs = nowMs + ec::kFrameSpacingMs;
      sSlot.retrying = false;
      ++sStats.framesSent;
      if (sSlot.sent >= sSlot.nFrames) {
        sSlot.state = SlotState::Free;
        sPacer.noteWindowDone(mac, nowMs);
        ++sStats.windowsSent;
        sStats.haveLast = true;
        memcpy(sStats.lastMac, mac, 6);
        sStats.lastOp = sSlot.op;
        sStats.lastFrames = sSlot.nFrames;
        sStats.lastFileId = sSlot.fileId;
        sStats.lastBytes = sSlot.plainBytes;
        sStats.lastAtMs = nowMs;
        sStats.lastEpoch = Clock::isValidEpoch(wall) ? wall : 0;
      }
    } else if (!sSlot.retrying) {
      sSlot.retrying = true;
      sSlot.retrySinceMs = nowMs;
    } else if ((uint32_t)(nowMs - sSlot.retrySinceMs) >= ec::kSendRetryMs) {
      sSlot.state = SlotState::Free;   // the collector re-asks from its first missing offset
      ++sStats.abandoned;
    }
  }
  CS_UNLOCK();
}

// CATCHUP_SERVE_HOST_END

// ---------------------------------------------------------------------------
// Commands and the setting (module "catchup")
// ---------------------------------------------------------------------------

namespace {

void csConsole(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void csConsole(const char* fmt, ...) {
  char line[200];
  va_list args;
  va_start(args, fmt);
  vsnprintf(line, sizeof(line), fmt, args);
  va_end(args);
  broadcastOutput(line);
}

bool csAppend(char* out, size_t cap, size_t* pos, const char* fmt, ...) __attribute__((format(printf, 4, 5)));
bool csAppend(char* out, size_t cap, size_t* pos, const char* fmt, ...) {
  if (*pos >= cap) return false;
  va_list args;
  va_start(args, fmt);
  const int n = vsnprintf(out + *pos, cap - *pos, fmt, args);
  va_end(args);
  if (n < 0 || (size_t)n >= cap - *pos) {
    out[*pos] = '\0';
    return false;
  }
  *pos += (size_t)n;
  return true;
}

// 1,048,211
const char* grouped(uint32_t v, char* out, size_t cap) {
  char digits[12];
  const int n = snprintf(digits, sizeof(digits), "%lu", (unsigned long)v);
  size_t w = 0;
  for (int i = 0; i < n && w + 2 < cap; ++i) {
    if (i && (n - i) % 3 == 0) out[w++] = ',';
    out[w++] = digits[i];
  }
  out[w] = '\0';
  return out;
}

// 1.62 MB / 412 KB / 900 B
const char* sizeText(uint64_t v, char* out, size_t cap) {
  if (v >= 1000000ull) snprintf(out, cap, "%.2f MB", (double)v / 1e6);
  else if (v >= 1000ull) snprintf(out, cap, "%lu KB", (unsigned long)((v + 500) / 1000));
  else snprintf(out, cap, "%lu B", (unsigned long)v);
  return out;
}

const char* sealKey(const ec::LedgerEntry& e) {
  if (e.seal == uint8_t(ec::Seal::OnSend)) return "on_send";
  return (e.flags & ec::kLeSealedAtRest) ? "at_rest" : "none";
}
const char* sealWords(const ec::LedgerEntry& e) {
  if (e.seal == uint8_t(ec::Seal::OnSend)) return "sealed on send";
  return (e.flags & ec::kLeSealedAtRest) ? "sealed at rest" : "plain";
}

// "home-node" (its device name) or its address; "none" when not named.
void masterName(bool named, bool paired, const uint8_t mac[6], char* out, size_t cap) {
  if (!named) { strlcpy(out, "none", cap); return; }
  if (!paired) { strlcpy(out, "named, not paired", cap); return; }
  const String name = getEspNowDeviceName(mac);
  if (name.length()) {
    ec::displayName(name.c_str(), name.length(), out, cap);
  } else {
    macToDisplay(mac, out, cap);
  }
}

void utcText(time_t t, char* out, size_t cap) {
  struct tm tmv;
  gmtime_r(&t, &tmv);
  strftime(out, cap, "%H:%M:%S UTC", &tmv);
}

struct CsArgs { bool json, bad; uint32_t from; };
void parseArgs(const String& input, CsArgs* a) {
  memset(a, 0, sizeof(*a));
  char copy[64];
  strlcpy(copy, input.c_str(), sizeof(copy));
  char* save = nullptr;
  for (char* w = strtok_r(copy, " \t", &save); w; w = strtok_r(nullptr, " \t", &save)) {
    if (strcmp(w, "json") == 0) {
      a->json = true;
    } else if (strcmp(w, "from") == 0) {
      const char* n = strtok_r(nullptr, " \t", &save);
      long long v = 0;
      if (!n || !parseIntArg(n, 0, (long long)UINT32_MAX, v)) { a->bad = true; return; }
      a->from = (uint32_t)v;
    } else {
      a->bad = true;
      return;
    }
  }
}

}  // namespace

const char* cmd_espnow_catchup(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CsArgs args;
  parseArgs(argsInput, &args);
  if (args.bad) return "Usage: espnowcatchup [json] [from <n>]";
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  char* reply = getDebugBuffer();
  const size_t cap = GLOBAL_DEBUG_BUFFER_SIZE;
  const uint32_t now = millis();
  if (!catchupLedgerEnsure(now, false)) return "Error: the file catch-up ledger is not ready (no PSRAM, or storage unavailable)";

  SensorReceiverTargets t;
  sensorReceiverTargets(&t);
  char primary[48], backup[48];
  masterName(t.masterNamed, t.havePrimary, t.primary, primary, sizeof(primary));
  masterName(t.backupNamed, t.haveBackup, t.backup, backup, sizeof(backup));
  const bool keyReady = catchupLedgerKeyReady();
  uint8_t keyId[8];
  const bool haveKeyId = keyReady && captureCryptoKeyId(keyId);
  char keyHex[17] = "";
  for (int i = 0; haveKeyId && i < 8; ++i) snprintf(keyHex + 2 * i, 3, "%02x", keyId[i]);
  ServeStats st;
  CS_LOCK();
  st = sStats;
  CS_UNLOCK();
  CatchupLedgerStats ls;
  catchupLedgerStats(&ls);
  char lastPeer[48] = "";
  if (st.haveLast) masterName(true, true, st.lastMac, lastPeer, sizeof(lastPeer));
  char lastUtc[24] = "";
  if (st.haveLast && st.lastEpoch) utcText(st.lastEpoch, lastUtc, sizeof(lastUtc));

  const ec::LedgerState* s = catchupLedgerLock();
  if (!s) return "Error: the file catch-up ledger is busy; try again";
  unsigned offered = 0, untaken = 0, unsealedHealth = 0, noKey = 0;
  uint64_t offeredBytes = 0, untakenBytes = 0;
  for (uint8_t i = 0; i < s->count; ++i) {
    const ec::LedgerEntry& e = s->e[i];
    if (e.state != uint8_t(ec::EntryState::Present)) continue;
    const ec::Offer o = ec::entryOffer(e, keyReady);
    if (o == ec::Offer::WithheldUnsealedHealth) { ++unsealedHealth; continue; }
    if (o == ec::Offer::WithheldNoKey) { ++noKey; continue; }
    if (o != ec::Offer::Serve) continue;
    const uint32_t end = ec::logicalEnd(e);
    ++offered;
    offeredBytes += end - e.base;
    const uint32_t taken = e.delivered > e.base ? e.delivered : e.base;
    if (taken < end) { ++untaken; untakenBytes += end - taken; }
  }

  if (args.json) {
    size_t pos = 0;
    char idHex[9];
    snprintf(idHex, sizeof(idHex), "%08lx", (unsigned long)s->ledgerId);
    char pj[96], bj[96];
    espnowsensor::jsonTextEscape(primary, pj, sizeof(pj));
    espnowsensor::jsonTextEscape(backup, bj, sizeof(bj));
    csAppend(reply, cap, &pos,
             "{\"schema\":1,\"serve\":%s,\"built\":true,\"named\":{\"primary\":%s%s%s,\"backup\":%s%s%s},"
             "\"keyReady\":%s,\"keyId\":%s%s%s,\"ledgerId\":\"%s\",\"ledgerGen\":%lu,\"from\":%lu,\"files\":[",
             gSettings.catchupServe ? "true" : "false", t.masterNamed ? "\"" : "", t.masterNamed ? pj : "null",
             t.masterNamed ? "\"" : "", t.backupNamed ? "\"" : "", t.backupNamed ? bj : "null",
             t.backupNamed ? "\"" : "", keyReady ? "true" : "false", haveKeyId ? "\"" : "",
             haveKeyId ? keyHex : "null", haveKeyId ? "\"" : "", idHex, (unsigned long)s->gen,
             (unsigned long)args.from);
    static const size_t kTail = 360;   // the closing fields
    bool first = true;
    uint32_t next = 0;
    for (uint8_t i = 0; i < s->count; ++i) {
      const ec::LedgerEntry& e = s->e[i];
      if (e.fileId < args.from || !ec::entryOffered(e, keyReady)) continue;
      char shown[200], name[260];
      const char* rel = ec::entryName(e);
      ec::displayName(rel, strlen(rel), shown, sizeof(shown));
      espnowsensor::jsonTextEscape(shown, name, sizeof(name));
      const size_t mark = pos;
      const bool fit =
          pos + kTail < cap &&
          csAppend(reply, cap - kTail, &pos,
                   "%s{\"fileId\":%lu,\"kind\":\"%s\",\"name\":\"%s\",\"onSd\":%s,\"seal\":\"%s\",\"start\":%lu,"
                   "\"end\":%lu,\"delivered\":%lu,\"growing\":%s}",
                   first ? "" : ",", (unsigned long)e.fileId, ec::kindName(ec::Kind(e.kind)), name,
                   (e.flags & ec::kLeOnSd) ? "true" : "false", sealKey(e), (unsigned long)e.base,
                   (unsigned long)ec::logicalEnd(e), (unsigned long)(e.delivered > e.base ? e.delivered : e.base),
                   ec::growing(e, false, now) ? "true" : "false");
      if (!fit) {
        pos = mark;
        reply[pos] = '\0';
        next = e.fileId;
        break;
      }
      first = false;
    }
    catchupLedgerUnlock();
    csAppend(reply, cap, &pos, "],\"withheld\":{\"unsealedHealth\":%u,\"noKey\":%u},\"lastRequest\":", unsealedHealth,
             noKey);
    if (st.haveLast) {
      char peer[96];
      espnowsensor::jsonTextEscape(lastPeer, peer, sizeof(peer));
      char utc[24] = "";
      if (st.lastEpoch) {
        struct tm tmv;
        gmtime_r(&st.lastEpoch, &tmv);
        strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &tmv);
      }
      csAppend(reply, cap, &pos, "{\"peer\":\"%s\",\"utc\":%s%s%s,\"op\":\"%s\",\"fileId\":%lu,\"frames\":%u,\"bytes\":%lu}",
               peer, utc[0] ? "\"" : "", utc[0] ? utc : "null", utc[0] ? "\"" : "",
               st.lastOp == ec::kOpListReq ? "list" : "read", (unsigned long)st.lastFileId, (unsigned)st.lastFrames,
               (unsigned long)st.lastBytes);
    } else {
      csAppend(reply, cap, &pos, "null");
    }
    csAppend(reply, cap, &pos, ",\"reconcileMs\":%lu,\"refused\":%lu,\"refusedSilent\":%lu,\"busy\":%lu,\"next\":",
             (unsigned long)ls.lastReconcileMs, (unsigned long)st.refused, (unsigned long)st.refusedSilent,
             (unsigned long)st.busy);
    if (next) csAppend(reply, cap, &pos, "%lu}", (unsigned long)next);
    else csAppend(reply, cap, &pos, "null}");
    return reply;
  }

  char a[24], b[24], c[24];
  csConsole("[ESP-NOW] File catch-up: what this device offers the masters named on it");
  csConsole("  Serving: %s; named master: %s%s; backup: %s; capture key: %s%s%s%s",
            gSettings.catchupServe ? "on" : "off (espnowcatchupserve on)", primary, t.masterNamed ? " (primary)" : "",
            backup, keyReady ? "ready" : "not available (sealed-on-send files are withheld)",
            haveKeyId ? " (id " : "", haveKeyId ? keyHex : "", haveKeyId ? ")" : "");
  csConsole("  Offered: %u file%s, %s; not yet taken by any master: %u file%s, %s", offered, offered == 1 ? "" : "s",
            sizeText(offeredBytes, a, sizeof(a)), untaken, untaken == 1 ? "" : "s",
            sizeText(untakenBytes, b, sizeof(b)));
  unsigned shown = 0, cardOut = 0;
  for (uint8_t i = 0; i < s->count; ++i) {
    const ec::LedgerEntry& e = s->e[i];
    if (e.state == uint8_t(ec::EntryState::MissingCardOut)) { ++cardOut; continue; }
    if (e.fileId < args.from || !ec::entryOffered(e, keyReady)) continue;
    char name[200];
    const char* rel = ec::entryName(e);
    ec::displayName(rel, strlen(rel), name, sizeof(name));
    const uint32_t taken = e.delivered > e.base ? e.delivered : e.base;
    csConsole("    #%lu %-10s %-34s %-14s %s-%s taken to %s%s", (unsigned long)e.fileId,
              ec::kindName(ec::Kind(e.kind)), name, sealWords(e), grouped(e.base, a, sizeof(a)),
              grouped(ec::logicalEnd(e), b, sizeof(b)), grouped(taken, c, sizeof(c)),
              ec::growing(e, false, now) ? " (growing)" : "");
    ++shown;
  }
  const uint8_t ledgerFlags = s->flags;
  catchupLedgerUnlock();
  if (unsealedHealth || noKey)
    csConsole("  Withheld: %u health file%s not sealed at rest; %u sealed-on-send file%s without a capture key",
              unsealedHealth, unsealedHealth == 1 ? "" : "s", noKey, noKey == 1 ? "" : "s");
  if (cardOut) csConsole("  On the SD card, which is out: %u file%s (kept; offered again when it is back)", cardOut,
                         cardOut == 1 ? "" : "s");
  if (ledgerFlags & ec::kLsBeyondHorizon)
    csConsole("  Older files exist beyond the newest %u per device; they are not offered", ec::kHorizonTotal);
  if (st.haveLast) {
    csConsole("  Last request: %s %s, %s #%lu (%u frame%s, %s B); last reconcile %lu.%02lu s%s", lastPeer,
              lastUtc[0] ? lastUtc : "(clock not set)", st.lastOp == ec::kOpListReq ? "list" : "read",
              (unsigned long)st.lastFileId, (unsigned)st.lastFrames, st.lastFrames == 1 ? "" : "s",
              grouped(st.lastBytes, a, sizeof(a)), (unsigned long)(ls.lastReconcileMs / 1000),
              (unsigned long)((ls.lastReconcileMs % 1000) / 10), ls.lastWalkCut ? " (walk cut at 64 entries)" : "");
  } else {
    csConsole("  No request served this boot; last reconcile %lu.%02lu s%s", (unsigned long)(ls.lastReconcileMs / 1000),
              (unsigned long)((ls.lastReconcileMs % 1000) / 10), ls.lastWalkCut ? " (walk cut at 64 entries)" : "");
  }
  csConsole("  Refused this boot: %lu (%lu answered silently); busy answers: %lu; windows abandoned: %lu",
            (unsigned long)(st.refused + st.refusedSilent), (unsigned long)st.refusedSilent, (unsigned long)st.busy,
            (unsigned long)st.abandoned);
  (void)shown;
  snprintf(reply, cap, "OK: %u file%s offered, %u with data no master has taken", offered, offered == 1 ? "" : "s",
           untaken);
  return reply;
}

namespace {
// Columns: jsonKey, type, value, intDefault, floatDefault, stringDefault, min,
// max, label, options, isSecret, group, cmdKey.
const SettingEntry catchupSettingEntries[] = {
  { "catchupServe", SETTING_BOOL, &gSettings.catchupServe, 1, 0, nullptr, 0, 1,
    "Serve file catch-up to the masters named on this device", nullptr, false, nullptr, "espnowcatchupserve" },
};
}  // namespace

const char* cmd_espnow_catchupserve(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return handleSettingCommand(&catchupSettingEntries[0], argsInput);
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule catchupSettingsModule = {
  "catchup",
  "espnow.catchup",
  catchupSettingEntries,
  sizeof(catchupSettingEntries) / sizeof(catchupSettingEntries[0]),
  nullptr,
  "File catch-up: the whitelisted logs this device serves to the masters named on it"
};

extern const CommandEntry catchupCommands[] = {
  { "espnowcatchup", "Files this device offers its named masters, and what they took [json] [from <n>]", true,
    cmd_espnow_catchup, "Usage: espnowcatchup [json] [from <n>]" },
  { "espnowcatchupserve", "Serve file catch-up to the named masters [on|off]", true, cmd_espnow_catchupserve,
    "Usage: espnowcatchupserve [on|off]" },
};
extern const size_t catchupCommandsCount = sizeof(catchupCommands) / sizeof(catchupCommands[0]);

#endif  // ENABLE_ESPNOW_SEND

#endif  // ENABLE_ESPNOW
