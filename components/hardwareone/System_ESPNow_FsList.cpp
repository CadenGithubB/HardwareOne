// System_ESPNow_FsList.cpp — bonded-peer remote directory listing protocol
//
// See the header for design notes. This file owns:
//   - The static pending-request table (sender side)
//   - The static deferred-work slot (receiver side)
//   - VFS directory enumeration + entry serialization
//   - msgId allocation for outgoing REQ/REPLY frames

#include "System_ESPNow_FsList.h"

#if ENABLE_ESPNOW

#include <string.h>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "System_AuthIdentity.h"   // SYSTEM_IDENTITY_SCOPE, currentAuthContext
#include "System_CommandTypes.h"   // ExecReq::DeferredFn (signature for submitDeferredToCmdExec)
#include "System_Debug.h"
#include "System_ESPNow.h"          // generateMessageId
#include "System_ESPNow_Tx.h"       // espnowtx::sendAead / sendAeadSync (Step 3c)
#include "System_FileManager.h"     // FileEntry, FILE_MANAGER_MAX_CACHED_ITEMS
#include "System_Filesystem.h"      // filesystemReady, getPermissions
#include "System_MemUtil.h"         // ps_alloc — PSRAM-backed reply buffer (off espnow_task stack)
#include "System_Mutex.h"           // FsLockGuard
#include "System_VFS.h"             // VFS::openGuarded, listVirtualEntries, etc.

// v4_send_payload_smart is the universal "send this payload to that MAC"
// helper — picks encrypted-chunked under a session, plaintext-fragmented
// otherwise. Lives in System_ESPNow.cpp (not exposed in the header — it's
// considered an implementation detail of the protocol layer). Forward-decl
// it here rather than promote it to the public surface; that decision can
// be revisited if a third protocol module ends up needing it.
extern bool v4_send_payload_smart(const uint8_t* dst, uint8_t type,
                                  uint16_t flags, uint32_t msgId,
                                  const uint8_t* payload, uint16_t payloadLen,
                                  uint8_t ttl);

// Push a deferred callback onto cmd_exec_task. Used to move heavy FS work
// (directory scan, file send, JSON build) off espnow_task — see the FS
// deferred-work refactor comment near captureDeferred() below. Implementation
// in System_Utils.cpp; defined header-light here to avoid pulling the full
// cmd_exec_task surface into this translation unit.
extern bool submitDeferredToCmdExec(ExecReq::DeferredFn fn, void* arg);

// ============================================================================
// Sender (client) side — pending request table
// ============================================================================
//
// Single pending-request table shared by all three peer-FS operations
// (LIST, STAT, GET). Each entry carries an opcode tag that picks the
// callback variant; the receive paths use that tag to fire the right
// typed callback when a reply lands.

enum FsRpcOp : uint8_t {
  FS_OP_LIST = 0,
  FS_OP_STAT = 1,
  FS_OP_GET  = 2,
};

struct PendingRequest {
  uint32_t  reqId;        // 0 = slot free
  uint8_t   peerMac[6];
  uint8_t   op;           // FsRpcOp
  uint32_t  deadlineMs;
  // Union of callback variants. Only the one matching `op` is valid.
  union {
    FsListReplyCallback list;
    FsStatReplyCallback stat;
    FsGetAckCallback    get;
  } cb;
};

static PendingRequest sPending[FS_LIST_MAX_PENDING];

// Monotonic reqId allocator. Starts at 1 (0 is the "no request" sentinel).
// Wraps after 4 billion — extremely unlikely to collide with any still-
// outstanding requests by the time it wraps.
static uint32_t sNextReqId = 1;

// ============================================================================
// Receiver (peer) side — single deferred slot
// ============================================================================
//
// One deferred-work slot serves all three operations; the `op` tag tells the
// tick handler which payload type the union holds and which response to
// build. If a second request arrives while one is queued, the receiver
// sends an immediate TOO_BUSY reply rather than queueing.

struct DeferredReply {
  bool      pending;
  uint8_t   op;           // FsRpcOp — which payload union member is valid
  uint8_t   srcMac[6];
  union {
    V4PayloadFsListReq list;
    V4PayloadFsStatReq stat;
    V4PayloadFsGetReq  get;
  } req;
};

static DeferredReply sDeferred = {};

// ============================================================================
// Mutex — protects sPending[] and sDeferred against concurrent access from
// BTC_TASK (RX callbacks) and the main ESP-NOW task (tick + send API).
// ============================================================================
static SemaphoreHandle_t sMutex = nullptr;

namespace {
struct ScopedLock {
  bool held;
  ScopedLock() : held(false) {
    if (sMutex) held = (xSemaphoreTake(sMutex, pdMS_TO_TICKS(50)) == pdTRUE);
  }
  ~ScopedLock() { if (held) xSemaphoreGive(sMutex); }
};
} // namespace

// ============================================================================
// Lifecycle
// ============================================================================

void fsListInit() {
  if (sMutex) return;
  sMutex = xSemaphoreCreateMutex();
  if (!sMutex) {
    ERROR_ESPNOWF("[FSLIST] Failed to create mutex");
    return;
  }
  memset(sPending, 0, sizeof(sPending));
  memset(&sDeferred, 0, sizeof(sDeferred));
  sNextReqId = 1;
  DEBUG_ESPNOWF("[FSLIST] Initialized");
}

// ============================================================================
// Sender (client) API
// ============================================================================

// Find a free slot in the pending table. Returns index, or -1 if full.
// Lock MUST be held.
static int allocPendingSlotLocked() {
  for (int i = 0; i < FS_LIST_MAX_PENDING; i++) {
    if (sPending[i].reqId == 0) return i;
  }
  return -1;
}

// Lookup pending request by reqId. Returns index, or -1 if not found.
// Lock MUST be held.
static int findPendingByIdLocked(uint32_t reqId) {
  if (reqId == 0) return -1;
  for (int i = 0; i < FS_LIST_MAX_PENDING; i++) {
    if (sPending[i].reqId == reqId) return i;
  }
  return -1;
}

// Common slot bookkeeping shared by all three send paths. Allocates a reqId
// + slot, fills the per-op fields, returns the reqId (or 0 on no-free-slot).
// Lock MUST be held. Caller fills the callback union member matching `op`
// before sending.
static uint32_t allocSlotAndReqIdLocked(int* outSlot, uint8_t op, const uint8_t peerMac[6]) {
  int slot = allocPendingSlotLocked();
  if (slot < 0) {
    WARN_ESPNOWF("[FSLIST] No free pending slot (sender table full)");
    return 0;
  }
  uint32_t reqId = sNextReqId++;
  if (sNextReqId == 0) sNextReqId = 1;   // skip sentinel on wrap
  sPending[slot].reqId = reqId;
  memcpy(sPending[slot].peerMac, peerMac, 6);
  sPending[slot].op = op;
  sPending[slot].deadlineMs = millis() + FS_LIST_REQUEST_TIMEOUT_MS;
  *outSlot = slot;
  return reqId;
}

uint32_t fsListSendRequest(const uint8_t peerMac[6],
                           const char* path,
                           uint16_t startIndex,
                           FsListReplyCallback callback) {
  if (!peerMac || !path || !callback) return 0;

  fsListInit();
  ScopedLock lk;
  if (!lk.held) return 0;

  int slot = -1;
  uint32_t reqId = allocSlotAndReqIdLocked(&slot, FS_OP_LIST, peerMac);
  if (reqId == 0) return 0;
  sPending[slot].cb.list = callback;

  V4PayloadFsListReq req = {};
  req.reqId = reqId;
  req.startIndex = startIndex;
  req.maxEntries = FS_LIST_ENTRIES_PER_REPLY;
  strlcpy(req.path, path, sizeof(req.path));

  uint32_t msgId = generateMessageId();
  // Step 3c: cmd_exec context (CLI caller) — submitSync gives backpressure.
  bool sent = espnowtx::sendAeadSync(peerMac, ESPNOW_V4_TYPE_FS_LIST_REQ, 0, msgId,
                                     (const uint8_t*)&req, sizeof(req), 1, 2000);
  if (!sent) {
    sPending[slot].reqId = 0;
    WARN_ESPNOWF("[FSLIST] LIST send failed reqId=%u path='%s'", reqId, path);
    return 0;
  }
  DEBUG_ESPNOWF("[FSLIST] Sent LIST reqId=%u path='%s' startIdx=%u", reqId, path, startIndex);
  return reqId;
}

uint32_t fsStatSendRequest(const uint8_t peerMac[6],
                           const char* path,
                           FsStatReplyCallback callback) {
  if (!peerMac || !path || !callback) return 0;

  fsListInit();
  ScopedLock lk;
  if (!lk.held) return 0;

  int slot = -1;
  uint32_t reqId = allocSlotAndReqIdLocked(&slot, FS_OP_STAT, peerMac);
  if (reqId == 0) return 0;
  sPending[slot].cb.stat = callback;

  V4PayloadFsStatReq req = {};
  req.reqId = reqId;
  strlcpy(req.path, path, sizeof(req.path));

  uint32_t msgId = generateMessageId();
  // Step 3c: cmd_exec context (CLI caller).
  bool sent = espnowtx::sendAeadSync(peerMac, ESPNOW_V4_TYPE_FS_STAT_REQ, 0, msgId,
                                     (const uint8_t*)&req, sizeof(req), 1, 2000);
  if (!sent) {
    sPending[slot].reqId = 0;
    WARN_ESPNOWF("[FSLIST] STAT send failed reqId=%u path='%s'", reqId, path);
    return 0;
  }
  DEBUG_ESPNOWF("[FSLIST] Sent STAT reqId=%u path='%s'", reqId, path);
  return reqId;
}

uint32_t fsGetSendRequest(const uint8_t peerMac[6],
                          const char* path,
                          FsGetAckCallback callback) {
  if (!peerMac || !path || !callback) return 0;

  fsListInit();
  ScopedLock lk;
  if (!lk.held) return 0;

  int slot = -1;
  uint32_t reqId = allocSlotAndReqIdLocked(&slot, FS_OP_GET, peerMac);
  if (reqId == 0) return 0;
  sPending[slot].cb.get = callback;

  V4PayloadFsGetReq req = {};
  req.reqId = reqId;
  strlcpy(req.path, path, sizeof(req.path));

  uint32_t msgId = generateMessageId();
  // Step 3c: cmd_exec context (CLI caller).
  bool sent = espnowtx::sendAeadSync(peerMac, ESPNOW_V4_TYPE_FS_GET_REQ, 0, msgId,
                                     (const uint8_t*)&req, sizeof(req), 1, 2000);
  if (!sent) {
    sPending[slot].reqId = 0;
    WARN_ESPNOWF("[FSLIST] GET send failed reqId=%u path='%s'", reqId, path);
    return 0;
  }
  DEBUG_ESPNOWF("[FSLIST] Sent GET reqId=%u path='%s'", reqId, path);
  return reqId;
}

void fsListCancel(uint32_t reqId) {
  ScopedLock lk;
  if (!lk.held) return;
  int slot = findPendingByIdLocked(reqId);
  if (slot >= 0) {
    // Just clear reqId — the union doesn't need to be wiped; the next allocation
    // overwrites it. Future replies for the cancelled reqId find no slot and drop.
    sPending[slot].reqId = 0;
  }
}

// ============================================================================
// RX bridge — sender side (reply arrives)
// ============================================================================

void fsListOnReplyReceived(const uint8_t srcMac[6],
                           const uint8_t* payload, uint16_t payloadLen) {
  if (payloadLen < sizeof(V4PayloadFsListReplyHeader)) {
    WARN_ESPNOWF("[FSLIST] REPLY too short: %u bytes", payloadLen);
    return;
  }

  // Validate the structure: header followed by entryCount entries.
  V4PayloadFsListReplyHeader hdr;
  memcpy(&hdr, payload, sizeof(hdr));
  uint16_t expectedLen = sizeof(hdr) + (uint16_t)hdr.entryCount * sizeof(V4PayloadFsEntry);
  if (payloadLen < expectedLen) {
    WARN_ESPNOWF("[FSLIST] REPLY reqId=%u entryCount=%u: payload %u < expected %u",
                 hdr.reqId, hdr.entryCount, payloadLen, expectedLen);
    return;
  }
  if (hdr.entryCount > FS_LIST_ENTRIES_PER_REPLY) {
    WARN_ESPNOWF("[FSLIST] REPLY reqId=%u entryCount=%u exceeds cap %u — clamping",
                 hdr.reqId, hdr.entryCount, FS_LIST_ENTRIES_PER_REPLY);
    hdr.entryCount = FS_LIST_ENTRIES_PER_REPLY;
  }

  const V4PayloadFsEntry* entries =
      (hdr.entryCount > 0)
          ? (const V4PayloadFsEntry*)(payload + sizeof(hdr))
          : nullptr;

  // Snapshot callback + clear slot under lock so we can fire callback without
  // holding the mutex (callback may want to call fsListSendRequest again,
  // which would deadlock with a recursive lock attempt).
  FsListReplyCallback cb = nullptr;
  {
    ScopedLock lk;
    if (!lk.held) return;
    int slot = findPendingByIdLocked(hdr.reqId);
    if (slot < 0) {
      // Unknown reqId — likely a duplicate reply or a reply for a cancelled
      // request. Drop silently; this is normal.
      return;
    }
    // Defensive: only fire if the slot's op matches what we're handling. A
    // reqId collision between LIST and STAT/GET shouldn't happen (single
    // counter) but if it ever did we'd crash on type-punning.
    if (sPending[slot].op != FS_OP_LIST) return;
    cb = sPending[slot].cb.list;
    sPending[slot].reqId = 0;
  }

  if (cb) {
    cb(srcMac, &hdr, entries);
  }
}

// ============================================================================
// RX bridge — receiver side (request arrives, defer to tick)
// ============================================================================

// Send an immediate TOO_BUSY response of the right shape for whatever the
// incoming request was. Called when sDeferred is already occupied.
static void sendBusyReply(uint8_t op, const uint8_t srcMac[6],
                          const uint8_t* payload) {
  uint32_t msgId = generateMessageId();
  switch (op) {
    case FS_OP_LIST: {
      V4PayloadFsListReplyHeader busy = {};
      memcpy(&busy.reqId, payload, sizeof(busy.reqId));
      busy.status = FS_LIST_STATUS_TOO_BUSY;
      // Step 3c: espnow_task RX-handler context — fire-and-forget.
      espnowtx::sendAead(srcMac, ESPNOW_V4_TYPE_FS_LIST_REPLY, 0, msgId,
                         (const uint8_t*)&busy, sizeof(busy), 1);
      break;
    }
    case FS_OP_STAT: {
      V4PayloadFsStatReply busy = {};
      memcpy(&busy.reqId, payload, sizeof(busy.reqId));
      busy.status = FS_LIST_STATUS_TOO_BUSY;
      // Step 3c: espnow_task RX-handler context — fire-and-forget.
      espnowtx::sendAead(srcMac, ESPNOW_V4_TYPE_FS_STAT_REPLY, 0, msgId,
                         (const uint8_t*)&busy, sizeof(busy), 1);
      break;
    }
    case FS_OP_GET: {
      V4PayloadFsGetAck busy = {};
      memcpy(&busy.reqId, payload, sizeof(busy.reqId));
      busy.status = FS_LIST_STATUS_TOO_BUSY;
      // Step 3c: espnow_task RX-handler context — fire-and-forget.
      espnowtx::sendAead(srcMac, ESPNOW_V4_TYPE_FS_GET_ACK, 0, msgId,
                         (const uint8_t*)&busy, sizeof(busy), 1);
      break;
    }
  }
}

// Forward decl — full definition further down. Called by the cmd_exec
// runner below (which is declared before processDeferredLocked in source
// order so the runner can live near captureDeferred for readability).
static void processDeferredLocked();

// Runs on cmd_exec_task (24 KB stack). Locks sMutex, drains the deferred
// slot via processDeferredLocked() — the same code that fsListTick used to
// invoke from espnow_task, but now living on a stack that can absorb the
// 2.5 KB+ VFS/LittleFS work without overflowing.
//
// THE ARG IS UNUSED. We funnel through the single global sDeferred slot; the
// "what to do" is encoded in sDeferred.op, not in the arg. submitDeferred-
// ToCmdExec's callback signature is `void(*)(void*)` so the parameter is
// declared but ignored.
//
// Reentrancy: if cmd_exec_task picks up this job and starts running while a
// second FS request arrives on espnow_task, the second request will either
// (a) see sDeferred.pending == true (if the lock guard is held or the slot
// hasn't been cleared yet) and emit a TOO_BUSY reply, or (b) capture into
// the slot AFTER processDeferredLocked released the lock + cleared pending,
// in which case it will submitDeferredToCmdExec a fresh job — that job runs
// after this one finishes. Single-slot serialization is preserved.
static void runDeferredFsOpOnCmdExec(void* arg) {
  (void)arg;
  if (!sMutex) return;
  ScopedLock lk;
  if (!lk.held) return;
  processDeferredLocked();
}

// Common deferred-slot capture for any incoming request opcode. Verifies
// the payload size, copies into the union member matching `op`, sets the
// op tag, returns. If the deferred slot is already busy, fires a TOO_BUSY
// reply immediately from this BTC_TASK context (small single-frame send
// is acceptable here).
static void captureDeferred(uint8_t op, const uint8_t srcMac[6],
                            const uint8_t* payload, uint16_t payloadLen,
                            uint16_t expectedSize) {
  if (payloadLen < expectedSize) {
    WARN_ESPNOWF("[FSLIST] REQ (op=%u) too short: %u < %u",
                 op, payloadLen, expectedSize);
    return;
  }
  fsListInit();
  ScopedLock lk;
  if (!lk.held) return;

  if (sDeferred.pending) {
    sendBusyReply(op, srcMac, payload);
    return;
  }

  memcpy(sDeferred.srcMac, srcMac, 6);
  sDeferred.op = op;
  // Copy into the right union member. The op tag tells the tick handler
  // which member to read.
  switch (op) {
    case FS_OP_LIST:
      memcpy(&sDeferred.req.list, payload, sizeof(sDeferred.req.list));
      sDeferred.req.list.path[sizeof(sDeferred.req.list.path) - 1] = '\0';
      DEBUG_ESPNOWF("[FSLIST] Deferred LIST reqId=%u path='%s'",
                    sDeferred.req.list.reqId, sDeferred.req.list.path);
      break;
    case FS_OP_STAT:
      memcpy(&sDeferred.req.stat, payload, sizeof(sDeferred.req.stat));
      sDeferred.req.stat.path[sizeof(sDeferred.req.stat.path) - 1] = '\0';
      DEBUG_ESPNOWF("[FSLIST] Deferred STAT reqId=%u path='%s'",
                    sDeferred.req.stat.reqId, sDeferred.req.stat.path);
      break;
    case FS_OP_GET:
      memcpy(&sDeferred.req.get, payload, sizeof(sDeferred.req.get));
      sDeferred.req.get.path[sizeof(sDeferred.req.get.path) - 1] = '\0';
      DEBUG_ESPNOWF("[FSLIST] Deferred GET reqId=%u path='%s'",
                    sDeferred.req.get.reqId, sDeferred.req.get.path);
      break;
  }
  sDeferred.pending = true;

  // Hand the deferred work off to cmd_exec_task. Used to be processed by
  // fsListTick() on espnow_task, but the 2.5 KB stack buffer in
  // processListDeferred + VFS::openGuarded + LittleFS directory iteration
  // overflowed espnow_task's 22 KB budget. cmd_exec_task has 24 KB stack
  // and was designed exactly for this kind of heavy crypto/FS work (it's
  // where SESSION_OPEN/CONFIRM also defer to — see comment block at
  // System_ESPNow.cpp:3855).
  //
  // On submit failure (queue full) we send a TOO_BUSY response synchronously
  // and clear the pending flag so the peer can retry instead of waiting
  // for a reply that will never come.
  if (!submitDeferredToCmdExec(runDeferredFsOpOnCmdExec, nullptr)) {
    WARN_ESPNOWF("[FSLIST] cmd_exec queue full — sending BUSY for op=%u", op);
    sendBusyReply(op, srcMac, payload);
    sDeferred.pending = false;
  }
}

void fsListOnRequestReceived(const uint8_t srcMac[6],
                             const uint8_t* payload, uint16_t payloadLen) {
  captureDeferred(FS_OP_LIST, srcMac, payload, payloadLen,
                  sizeof(V4PayloadFsListReq));
}

void fsStatOnRequestReceived(const uint8_t srcMac[6],
                             const uint8_t* payload, uint16_t payloadLen) {
  captureDeferred(FS_OP_STAT, srcMac, payload, payloadLen,
                  sizeof(V4PayloadFsStatReq));
}

void fsGetOnRequestReceived(const uint8_t srcMac[6],
                            const uint8_t* payload, uint16_t payloadLen) {
  captureDeferred(FS_OP_GET, srcMac, payload, payloadLen,
                  sizeof(V4PayloadFsGetReq));
}

// ============================================================================
// Reply / ack receivers — match reqId, fire typed callback
// ============================================================================

void fsStatOnReplyReceived(const uint8_t srcMac[6],
                           const uint8_t* payload, uint16_t payloadLen) {
  if (payloadLen < sizeof(V4PayloadFsStatReply)) {
    WARN_ESPNOWF("[FSLIST] STAT_REPLY too short: %u bytes", payloadLen);
    return;
  }
  V4PayloadFsStatReply reply;
  memcpy(&reply, payload, sizeof(reply));

  FsStatReplyCallback cb = nullptr;
  {
    ScopedLock lk;
    if (!lk.held) return;
    int slot = findPendingByIdLocked(reply.reqId);
    if (slot < 0 || sPending[slot].op != FS_OP_STAT) return;
    cb = sPending[slot].cb.stat;
    sPending[slot].reqId = 0;
  }
  if (cb) cb(srcMac, &reply);
}

void fsGetOnAckReceived(const uint8_t srcMac[6],
                        const uint8_t* payload, uint16_t payloadLen) {
  if (payloadLen < sizeof(V4PayloadFsGetAck)) {
    WARN_ESPNOWF("[FSLIST] GET_ACK too short: %u bytes", payloadLen);
    return;
  }
  V4PayloadFsGetAck ack;
  memcpy(&ack, payload, sizeof(ack));

  FsGetAckCallback cb = nullptr;
  {
    ScopedLock lk;
    if (!lk.held) return;
    int slot = findPendingByIdLocked(ack.reqId);
    if (slot < 0 || sPending[slot].op != FS_OP_GET) return;
    cb = sPending[slot].cb.get;
    sPending[slot].reqId = 0;
  }
  if (cb) cb(srcMac, &ack);
}

// ============================================================================
// Reply builder — runs on main task tick
// ============================================================================

// ============================================================================
// Per-op deferred work (LIST / STAT / GET). Called by processDeferredLocked
// WITHOUT holding sMutex — the dispatcher releases the mutex before invoking
// these so a slow VFS read or send doesn't block other API calls.
// ============================================================================

// Forward decls.
static void processListDeferred(const uint8_t srcMac[6], const V4PayloadFsListReq& req);
static void processStatDeferred(const uint8_t srcMac[6], const V4PayloadFsStatReq& req);
static void processGetDeferred(const uint8_t srcMac[6], const V4PayloadFsGetReq& req);

// Dispatcher. Lock is HELD on entry; we release before per-op work and
// re-acquire on the way out so the caller sees consistent state.
static void processDeferredLocked() {
  if (!sDeferred.pending) return;

  uint8_t op = sDeferred.op;
  uint8_t srcMac[6];
  memcpy(srcMac, sDeferred.srcMac, 6);

  // Snapshot the appropriate union member before clearing — sDeferred is
  // about to be freed for the next request.
  union {
    V4PayloadFsListReq list;
    V4PayloadFsStatReq stat;
    V4PayloadFsGetReq  get;
  } req;
  switch (op) {
    case FS_OP_LIST: req.list = sDeferred.req.list; break;
    case FS_OP_STAT: req.stat = sDeferred.req.stat; break;
    case FS_OP_GET:  req.get  = sDeferred.req.get;  break;
  }
  sDeferred.pending = false;

  xSemaphoreGive(sMutex);

  switch (op) {
    case FS_OP_LIST: processListDeferred(srcMac, req.list); break;
    case FS_OP_STAT: processStatDeferred(srcMac, req.stat); break;
    case FS_OP_GET:  processGetDeferred(srcMac, req.get);   break;
  }

  xSemaphoreTake(sMutex, portMAX_DELAY);
}

static void processListDeferred(const uint8_t srcMac[6], const V4PayloadFsListReq& req) {
  // Reply runs on cmd_exec_task (24 KB stack — see runDeferredFsOpOnCmdExec).
  // The reply buffer is 140 + 32*76 = 2572 B; comfortably fits on the
  // cmd_exec stack, but we still PSRAM-allocate it for two reasons:
  //   (1) Stack headroom for VFS::openGuarded + LittleFS recursive directory
  //       iteration internals (LittleFS uses substantial stack for path
  //       resolution + per-entry metadata reads).
  //   (2) Defense in depth — if a future change re-introduces the espnow_task
  //       dispatch path, having the buffer already off-stack prevents a
  //       recurrence of the FS_LIST stack-overflow class of bug.
  // ps_alloc prefers PSRAM (~8 MB) but falls back to DRAM if PSRAM is full;
  // either way the allocation is off-task-stack. On alloc failure we send
  // a NOT_READY reply via a tiny on-stack header (acceptable — header alone
  // is ~140 B, no entry array needed for an error reply).
  const size_t kReplyBufSize = sizeof(V4PayloadFsListReplyHeader)
                             + FS_LIST_ENTRIES_PER_REPLY * sizeof(V4PayloadFsEntry);
  uint8_t* replyBuf = (uint8_t*)ps_alloc(kReplyBufSize, AllocPref::PreferPSRAM,
                                         "fslist.reply");
  if (!replyBuf) {
    ERROR_MEMORYF("[FSLIST] ps_alloc(%zu) failed — sending NOT_READY for reqId=%u",
                  kReplyBufSize, req.reqId);
    V4PayloadFsListReplyHeader errHdr = {};
    errHdr.reqId  = req.reqId;
    errHdr.status = FS_LIST_STATUS_NOT_READY;
    strlcpy(errHdr.path, req.path, sizeof(errHdr.path));
    uint32_t errMsgId = generateMessageId();
    // Step 3c: cmd_exec context (deferred handler).
    espnowtx::sendAeadSync(srcMac, ESPNOW_V4_TYPE_FS_LIST_REPLY, 0, errMsgId,
                           (const uint8_t*)&errHdr, sizeof(errHdr), 1, 2000);
    return;
  }
  V4PayloadFsListReplyHeader* hdr = (V4PayloadFsListReplyHeader*)replyBuf;
  V4PayloadFsEntry* entries =
      (V4PayloadFsEntry*)(replyBuf + sizeof(V4PayloadFsListReplyHeader));

  memset(replyBuf, 0, kReplyBufSize);
  hdr->reqId = req.reqId;
  strlcpy(hdr->path, req.path, sizeof(hdr->path));

  if (!filesystemReady) {
    hdr->status = FS_LIST_STATUS_NOT_READY;
    goto send_reply;
  }

  {
    // Read under SYSTEM identity — bonded peer is trusted at device level.
    // Per-user identity propagation can be layered on later via a token in
    // the request's reserved bytes.
    SYSTEM_IDENTITY_SCOPE("espnow.fs_list_reply");
    const AuthContext& ctx = currentAuthContext();
    String dirPath = VFS::normalize(req.path);

    // existsGuarded + isDirectory check
    if (!VFS::existsGuarded(dirPath, ctx)) {
      hdr->status = FS_LIST_STATUS_NOT_FOUND;
      goto send_reply;
    }

    {
      FsLockGuard guard("fslist.reply");
      File dir = VFS::openGuarded(dirPath, "r", ctx);
      if (!dir) {
        hdr->status = FS_LIST_STATUS_PERM_DENIED;
        goto send_reply;
      }
      if (!dir.isDirectory()) {
        dir.close();
        hdr->status = FS_LIST_STATUS_NOT_A_DIR;
        goto send_reply;
      }

      // Phase 1: iterate the directory and count + capture entries past
      // startIndex up to FS_LIST_ENTRIES_PER_REPLY.
      uint16_t skip = req.startIndex;
      uint16_t emit = 0;
      uint16_t total = 0;
      uint16_t cap = req.maxEntries > FS_LIST_ENTRIES_PER_REPLY
                         ? FS_LIST_ENTRIES_PER_REPLY
                         : req.maxEntries;

      // SD entries come back rooted at the SD root, not at our "/sd/..."
      // mount convention. Mirror the prefix stripping logic in
      // buildFilesListing — see System_Filesystem.cpp:230 for the canonical
      // version. Mount points + nested-path skip likewise mirror that code.
      String fsDirPath = VFS::stripSdPrefix(dirPath);

      // Virtual mount points first (e.g. /sd at LittleFS root).
      VFS::VirtualEntry virtuals[4];
      const size_t nVirt = VFS::listVirtualEntries(
          dirPath, virtuals, sizeof(virtuals) / sizeof(virtuals[0]));
      for (size_t v = 0; v < nVirt; v++) {
        if (total >= skip && emit < cap) {
          V4PayloadFsEntry& e = entries[emit++];
          strlcpy(e.name, virtuals[v].name, sizeof(e.name));
          e.isFolder = virtuals[v].isFolder ? 1 : 0;
          e.size = 0;
          char fullPath[160];
          snprintf(fullPath, sizeof(fullPath), "%s%s%s",
                   dirPath.c_str(), dirPath == "/" ? "" : "/",
                   virtuals[v].name);
          e.perms = getPermissions(String(fullPath), ctx);
        }
        total++;
      }

      File file = dir.openNextFile();
      while (file) {
        String fileName = String(file.name());
        if (fsDirPath != "/") {
          String expectedPrefix = fsDirPath;
          if (!expectedPrefix.endsWith("/")) expectedPrefix += "/";
          if (fileName.startsWith(expectedPrefix)) {
            fileName = fileName.substring(expectedPrefix.length());
          }
        } else if (fileName.startsWith("/")) {
          fileName = fileName.substring(1);
        }
        // Skip nested paths
        if (fileName.length() == 0 || fileName.indexOf('/') != -1) {
          file = dir.openNextFile();
          continue;
        }
        // Skip hidden
        if (fileName.startsWith(".")) {
          file = dir.openNextFile();
          continue;
        }

        if (total >= skip && emit < cap) {
          V4PayloadFsEntry& e = entries[emit++];
          strlcpy(e.name, fileName.c_str(), sizeof(e.name));
          e.isFolder = file.isDirectory() ? 1 : 0;
          e.size = e.isFolder ? 0 : (uint32_t)file.size();
          String full = formatPath(dirPath.c_str(), e.name);
          e.perms = getPermissions(full, ctx);
        }
        total++;
        file = dir.openNextFile();
      }
      dir.close();

      hdr->status = FS_LIST_STATUS_OK;
      hdr->entryCount = (uint8_t)emit;
      hdr->totalEntries = total;
      hdr->hasMore = (skip + emit < total) ? 1 : 0;
      hdr->nextStartIndex = (uint16_t)(skip + emit);
    }
  }

send_reply: ;
  uint16_t payloadLen = sizeof(V4PayloadFsListReplyHeader)
                      + (uint16_t)hdr->entryCount * sizeof(V4PayloadFsEntry);
  uint32_t msgId = generateMessageId();
  // Step 3c: cmd_exec context (deferred handler). 3s timeout — REPLY payload
  // can be up to ~500B with full entry list which fragments on the wire.
  bool sent = espnowtx::sendAeadSync(srcMac, ESPNOW_V4_TYPE_FS_LIST_REPLY, 0, msgId,
                                     replyBuf, payloadLen, 1, 3000);
  DEBUG_ESPNOWF("[FSLIST] Sent LIST REPLY reqId=%u status=%u entries=%u/%u hasMore=%u sent=%d",
                hdr->reqId, hdr->status, hdr->entryCount, hdr->totalEntries,
                hdr->hasMore, (int)sent);
  // ps_alloc'd reply buffer — free here so heap (PSRAM or DRAM fallback)
  // isn't held across the next deferred request. v4_send_payload_smart
  // copies the payload into its own fragmentation buffers, so the original
  // can be freed immediately after the send call returns.
  free(replyBuf);
}

// ----------------------------------------------------------------------------
// STAT — return total/used/free bytes for the storage backing a path.
// ----------------------------------------------------------------------------
// Path resolution picks the storage tier:
//   - "/sd" or "/sd/..." → SDCARD
//   - everything else    → LITTLEFS (the root tier)
// VFS::getStats does the underlying capacity calls; we just translate to
// the wire payload.

static void processStatDeferred(const uint8_t srcMac[6], const V4PayloadFsStatReq& req) {
  V4PayloadFsStatReply reply = {};
  reply.reqId = req.reqId;
  strlcpy(reply.path, req.path, sizeof(reply.path));

  if (!filesystemReady) {
    reply.status = FS_LIST_STATUS_NOT_READY;
    goto send_stat;
  }

  {
    SYSTEM_IDENTITY_SCOPE("espnow.fs_stat_reply");
    String norm = VFS::normalize(req.path);
    VFS::StorageType tier = VFS::INTERNAL;
    if (norm.startsWith("/sd")) tier = VFS::SDCARD;
    if (tier == VFS::SDCARD && !VFS::isSDAvailable()) {
      reply.status = FS_LIST_STATUS_NOT_FOUND;
      goto send_stat;
    }
    uint64_t total = 0, used = 0, free = 0;
    if (!VFS::getStats(tier, total, used, free)) {
      reply.status = FS_LIST_STATUS_IO_ERROR;
      goto send_stat;
    }
    reply.status         = FS_LIST_STATUS_OK;
    reply.totalBytes     = total;
    reply.usedBytes      = used;
    reply.freeBytes      = free;
    reply.percentUsedX10 = (total > 0)
        ? (uint16_t)((used * 1000ULL + total / 2) / total)  // round to nearest 0.1%
        : 0;
  }

send_stat: ;
  uint32_t msgId = generateMessageId();
  // Step 3c: cmd_exec context (deferred handler).
  bool sent = espnowtx::sendAeadSync(srcMac, ESPNOW_V4_TYPE_FS_STAT_REPLY, 0, msgId,
                                     (const uint8_t*)&reply, sizeof(reply), 1, 2000);
  DEBUG_ESPNOWF("[FSLIST] Sent STAT REPLY reqId=%u status=%u used=%llu/%llu pct=%u sent=%d",
                reply.reqId, reply.status,
                (unsigned long long)reply.usedBytes,
                (unsigned long long)reply.totalBytes,
                reply.percentUsedX10, (int)sent);
}

// ----------------------------------------------------------------------------
// GET — validate the requested file, ACK the request (with size), then
// kick the existing FILE_START/DATA/END pipeline to send it to the caller.
// ----------------------------------------------------------------------------
// The ACK lets the caller surface NOT_FOUND/PERM_DENIED synchronously; the
// FILE_* transfer that follows is async and lands the file via the existing
// receive path (System_ESPNow_Files).

// Forward declaration: defined in System_ESPNow.cpp. The function reads the
// file under the calling task's identity (we install SYSTEM here before
// calling) and pumps FILE_START/DATA/END to the destination MAC.
extern bool sendFileToMac(const uint8_t* mac, const String& localPath);

static void processGetDeferred(const uint8_t srcMac[6], const V4PayloadFsGetReq& req) {
  V4PayloadFsGetAck ack = {};
  ack.reqId = req.reqId;
  strlcpy(ack.path, req.path, sizeof(ack.path));

  if (!filesystemReady) {
    ack.status = FS_LIST_STATUS_NOT_READY;
    goto send_ack;
  }

  {
    SYSTEM_IDENTITY_SCOPE("espnow.fs_get_ack");
    const AuthContext& ctx = currentAuthContext();
    String filePath = VFS::normalize(req.path);

    if (!VFS::existsGuarded(filePath, ctx)) {
      ack.status = FS_LIST_STATUS_NOT_FOUND;
      goto send_ack;
    }

    {
      FsLockGuard guard("fsget.size_probe");
      File f = VFS::openGuarded(filePath, "r", ctx);
      if (!f) {
        ack.status = FS_LIST_STATUS_PERM_DENIED;
        goto send_ack;
      }
      if (f.isDirectory()) {
        f.close();
        // Reuse NOT_A_DIR semantically inverted — caller wanted a file but
        // got a directory. We don't have a NOT_A_FILE code; PERM_DENIED is
        // misleading. Reuse NOT_A_DIR (status code 2) which is the inverse;
        // the wire docs say it means "type mismatch on the path".
        ack.status = FS_LIST_STATUS_NOT_A_DIR;
        goto send_ack;
      }
      ack.fileSize = (uint32_t)f.size();
      f.close();
    }
    ack.status = FS_LIST_STATUS_OK;
  }

send_ack: ;
  uint32_t msgId = generateMessageId();
  // Step 3c: cmd_exec context (deferred handler).
  espnowtx::sendAeadSync(srcMac, ESPNOW_V4_TYPE_FS_GET_ACK, 0, msgId,
                         (const uint8_t*)&ack, sizeof(ack), 1, 2000);
  DEBUG_ESPNOWF("[FSLIST] Sent GET ACK reqId=%u status=%u fileSize=%u",
                ack.reqId, ack.status, ack.fileSize);

  // Initiate the actual file transfer if the ACK was OK. sendFileToMac is
  // synchronous-ish (pumps FILE_START/DATA/END with per-fragment ACK waits),
  // so this can take several seconds for large files. Now runs on
  // cmd_exec_task (deferred from espnow_task via submitDeferredToCmdExec
  // — see runDeferredFsOpOnCmdExec) which leaves espnow_task free to drain
  // RX so the per-fragment ACKs we're waiting for can come in.
  if (ack.status == FS_LIST_STATUS_OK) {
    SYSTEM_IDENTITY_SCOPE("espnow.fs_get_send");
    String filePath = VFS::normalize(req.path);
    bool ok = sendFileToMac(srcMac, filePath);
    DEBUG_ESPNOWF("[FSLIST] GET transfer for reqId=%u path='%s' result=%d",
                  req.reqId, filePath.c_str(), (int)ok);
  }
}

// ============================================================================
// Tick — process deferred replies, sweep timed-out senders
// ============================================================================

void fsListTick() {
  if (!sMutex) return;
  ScopedLock lk;
  if (!lk.held) return;

  // NOTE: deferred reply work used to be processed here — that path overflowed
  // espnow_task's 22 KB stack on the LIST reply (2.5 KB stack buffer + VFS +
  // LittleFS dir iteration). Capture now hands off to cmd_exec_task via
  // submitDeferredToCmdExec at the end of captureDeferred() — see the comment
  // there. This tick now only sweeps SENDER-side timeouts.

  // (2) Timeout sweep on pending sender requests
  uint32_t now = millis();
  for (int i = 0; i < FS_LIST_MAX_PENDING; i++) {
    if (sPending[i].reqId == 0) continue;
    if ((int32_t)(now - sPending[i].deadlineMs) < 0) continue;

    // Expired — fire the per-op callback with a synthesized IO_ERROR. We
    // snapshot fields and clear the slot before firing so a callback that
    // re-issues the request can grab a fresh slot without recursion.
    uint32_t expReqId = sPending[i].reqId;
    uint8_t expMac[6];
    memcpy(expMac, sPending[i].peerMac, 6);
    uint8_t op = sPending[i].op;
    FsListReplyCallback listCb = nullptr;
    FsStatReplyCallback statCb = nullptr;
    FsGetAckCallback    getCb  = nullptr;
    switch (op) {
      case FS_OP_LIST: listCb = sPending[i].cb.list; break;
      case FS_OP_STAT: statCb = sPending[i].cb.stat; break;
      case FS_OP_GET:  getCb  = sPending[i].cb.get;  break;
    }
    sPending[i].reqId = 0;

    xSemaphoreGive(sMutex);
    if (listCb) {
      V4PayloadFsListReplyHeader err = {};
      err.reqId = expReqId;
      err.status = FS_LIST_STATUS_IO_ERROR;
      listCb(expMac, &err, nullptr);
    } else if (statCb) {
      V4PayloadFsStatReply err = {};
      err.reqId = expReqId;
      err.status = FS_LIST_STATUS_IO_ERROR;
      statCb(expMac, &err);
    } else if (getCb) {
      V4PayloadFsGetAck err = {};
      err.reqId = expReqId;
      err.status = FS_LIST_STATUS_IO_ERROR;
      getCb(expMac, &err);
    }
    xSemaphoreTake(sMutex, portMAX_DELAY);
    DEBUG_ESPNOWF("[FSLIST] Timed out reqId=%u op=%u", expReqId, op);
  }
}

#endif // ENABLE_ESPNOW
