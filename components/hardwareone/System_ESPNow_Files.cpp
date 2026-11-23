#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include "System_ESPNow_Files.h"

#include <Arduino.h>
#include <string.h>

#include <esp_attr.h>         // EXT_RAM_BSS_ATTR — the result memory
#include <esp_crc.h>          // esp_crc32_le — whole-file CRC + filename hash
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "System_Debug.h"
#include "System_MemUtil.h"   // ps_alloc / AllocPref
#include "System_VFS.h"       // boot cleanup of orphaned .part staging files
#include "System_Mutex.h"     // FsLockGuard — the .part writes hold the FS lock
#include "System_CommandTypes.h"  // ExecReq::DeferredFn — drain/abort jobs on cmd_exec
#include "System_Utils.h"  // submitDeferredToCmdExec

namespace {

constexpr uint8_t kFileSlots = 4;

// Streaming double-buffer: espnow_task fills one 4 KB half while cmd_exec
// writes the other to the .part. Two halves = the 8 KB/slot we already used, just
// split so the radio task is never blocked on flash. One filled buffer = ~20 chunks
// (~430 ms) of slack for the writer to drain a NOR erase — plenty (writer turnaround
// is tens of ms on an idle receiver).
constexpr uint32_t kFileStreamBufSize = 4096;
constexpr uint8_t  kFileStreamBufs    = 2;

struct FileTransferSlotImpl {
  uint8_t  state;             // FileSlotState
  uint8_t  peerMac[6];
  uint32_t transferId;
  char     filename[kFileSlotNameMax + 1];
  uint8_t  kind;          // FILE_START's kind: 0 file, 1 bond answer
  uint32_t filenameHash;  // esp_crc32_le over the (bounded) wire filename — the
                          // stable per-(peer,file) identity: keys the streaming
                          // .part name and the supersede-conflict check, both of
                          // which must survive sender retries (fresh transferId) and
                          // reboots
  uint32_t totalSize;
  uint32_t receivedBytes;
  uint16_t totalChunks;
  uint16_t receivedChunks;
  uint16_t chunkSize;
  uint32_t startedMs;
  uint32_t lastFrameMs;
  uint32_t runningCrc;    // streaming mode: esp_crc32_le accumulated per accepted
                          // chunk (strictly in-order, so this is the whole-file
                          // CRC once complete). RAM mode leaves it 0 and computes
                          // one-shot at FILE_END (chunks may arrive out of order).

  // --- RAM mode (file <= kFileSlotMaxFileSize): whole file buffered in PSRAM,
  //     written once at FILE_END. The original, unchanged path. ---
  uint8_t* dataBuffer;        // PSRAM, totalSize bytes (min 1)
  uint32_t dataBufferLen;     // capacity (== totalSize, capped)
  uint8_t* chunkMap;
  uint16_t chunkMapBytes;

  // --- Streaming mode (file > kFileSlotMaxFileSize): chunks stream to a .part file
  //     via cmd_exec; dataBuffer/chunkMap stay null. espnow_task fills the
  //     active buffer (memcpy only); a full buffer is committed to streamPendQ and a
  //     drain job is queued; cmd_exec writes pending buffers to gStreamFile[idx].
  //     Strictly sequential (sender transmits in order); any gap aborts the
  //     transfer — there's no random-access buffer to back-fill. ---
  bool     streaming;
  uint8_t* streamBuf[kFileStreamBufs];   // PSRAM double-buffer halves
  uint32_t streamBufCap;                 // capacity of each half
  uint32_t streamBufLen[kFileStreamBufs];// committed bytes per half (set when handed off)
  uint32_t streamFill;                   // bytes in the active buffer so far
  uint8_t  streamActive;                 // index espnow_task is currently filling
  uint8_t  streamPendQ[kFileStreamBufs]; // FIFO of buffer indices awaiting flush
  uint8_t  streamPendHead;               // FIFO read position
  uint8_t  streamPendCount;              // # buffers awaiting flush (0..kFileStreamBufs)
  bool     streamDrainQueued;            // a drain job is already in gCmdExecQ
  bool     streamOpened;                 // .part opened yet (cmd_exec lazy-opens it)
  uint16_t nextChunk;                    // next expected (strictly sequential) chunk
  bool     streamFailed;                 // gap / write error → transfer fails
  bool     teardownPending;              // COMPLETING, but its cmd_exec abort job
                                         // could not be queued (queue full). The
                                         // timeout sweep retries the submit until
                                         // it lands — never release such a slot
                                         // inline from espnow_task.
  char     partPath[80];                 // on-flash staging path for this slot's .part
};

FileTransferSlotImpl* gFileSlots = nullptr;
SemaphoreHandle_t      gFileSlotsMutex = nullptr;

// Per-slot .part handle for streaming mode. Kept OUT of the POD struct (which is
// memset on alloc/release) because File is a C++ object with a destructor — a
// memset over it would leak the underlying handle. Indexed by slot index.
//
// Every touch of this handle is cmd_exec's, and DELIBERATELY OUTSIDE
// gFileSlotsMutex: streamDrainPending lazy-opens it and writes it holding only the
// FS lock, and finalize/release (releaseSlot) close it the same way. That is the
// whole point of the module — espnow_task must never block on flash — so the mutex
// covers the FIFO/metadata steps around the write, not the write. Safety comes from
// ownership instead: a buffer sitting in the PENDING FIFO is untouchable by
// espnow_task, and a streaming slot is only freed by its own cmd_exec
// finalize/abort job, so no other task can close the handle mid-write. Do not
// "fix" this by taking the slot mutex across the flash work.
File gStreamFile[kFileSlots];

// Slot-table lock.
//
// Invariant: gFileSlotsMutex guards table bookkeeping only. It is never held
// across filesystem work, a wait for the filesystem lock or anything else that
// can block, so every critical section is a few memory operations long. (It used
// to be held across the close + remove of a streaming teardown, which waits for
// the filesystem lock: a long directory walk elsewhere then made the receive
// path's bounded wait miss, and a miss was read as "no such transfer".)
//
// Two kinds of taker, the same split System_ESPNow_FsList.cpp uses for its lock:
//   - The per-frame receive path and the timeout sweep (espnow_task) keep the
//     short bounded wait, so RX never stalls. A miss there is reported as BUSY,
//     never as "not found" (see FileSlotLookup, STREAM_APPEND_BUSY).
//   - Takers that would leak a slot, pin it, write a buffer twice or cancel a
//     complete file by giving up — every cmd_exec job, plus release, finalize
//     hand-off and teardown parking — wait without a limit (kFileSlotsLockWait).
//     Given the invariant that wait is microseconds.
const TickType_t kFileSlotsLockBounded = pdMS_TO_TICKS(50);
const TickType_t kFileSlotsLockWait    = portMAX_DELAY;

struct FileSlotsLockGuard {
  bool ok;
  explicit FileSlotsLockGuard(TickType_t wait = kFileSlotsLockBounded) : ok(false) {
    if (gFileSlotsMutex) ok = (xSemaphoreTake(gFileSlotsMutex, wait) == pdTRUE);
  }
  ~FileSlotsLockGuard() {
    if (ok && gFileSlotsMutex) xSemaphoreGive(gFileSlotsMutex);
  }
  FileSlotsLockGuard(const FileSlotsLockGuard&) = delete;
  FileSlotsLockGuard& operator=(const FileSlotsLockGuard&) = delete;
};

// Defined in the second anonymous-namespace block below (same unnamed
// namespace, same TU) — fileSlotsAllocate's supersede path needs it to fail a
// streaming slot without touching its cmd_exec-owned .part handle.
void fileSlotsStreamFailLocked(FileTransferSlotImpl& s);

FileTransferSlotImpl* impl(FileTransferSlot* s) {
  return reinterpret_cast<FileTransferSlotImpl*>(s);
}
const FileTransferSlotImpl* implC(const FileTransferSlot* s) {
  return reinterpret_cast<const FileTransferSlotImpl*>(s);
}

// Free a slot's memory and return it to FREE. CALLER HOLDS gFileSlotsMutex.
// Memory only — no filesystem work happens here (see the lock invariant above).
// A streaming slot's .part handle and staging file are dealt with by releaseSlot
// BEFORE it calls this; the inline callers (allocate's supersede, the sweep) only
// ever pass RAM-path slots.
void releaseLocked(FileTransferSlotImpl& s) {
  for (uint8_t b = 0; b < kFileStreamBufs; b++) {
    if (s.streamBuf[b]) {
      ps_free(s.streamBuf[b]);
      s.streamBuf[b] = nullptr;
    }
  }
  if (s.dataBuffer) {
    ps_free(s.dataBuffer);
    s.dataBuffer = nullptr;
  }
  if (s.chunkMap) {
    ps_free(s.chunkMap);
    s.chunkMap = nullptr;
  }
  memset(&s, 0, sizeof(s));
  s.state = FILE_SLOT_FREE;
}

// Release a slot from outside the lock: fileSlotsRelease and the cmd_exec abort
// job. Waits for the slot lock (skipping would leak the slot).
//
// RAM-path slot: freed in one critical section.
//
// Streaming slot (cmd_exec only — it owns the .part handle): three steps, so
// the filesystem lock is never waited for with the slot lock held.
//   1. Under the slot lock: detach. The slot is parked in COMPLETING with no
//      teardown owed, which keeps it claimed — allocate cannot hand the index
//      (and with it the staging path and gStreamFile entry) to a new transfer,
//      the sweep skips it, and a late chunk or FILE_END is turned away before
//      it touches a buffer. From here on this job is the slot's only writer.
//   2. No slot lock: close the handle and remove the staging file under the
//      filesystem lock. partPath is read in place: it is written at allocation
//      and wiped in step 3, and nothing else writes a claimed slot.
//   3. Under the slot lock: free the buffers and mark the slot FREE.
void releaseSlot(FileTransferSlotImpl& s) {
  {
    FileSlotsLockGuard g(kFileSlotsLockWait);
    if (!g.ok) return;                       // no mutex: module never initialised
    if (s.state == FILE_SLOT_FREE) return;   // idempotent
    if (!s.streaming) {
      // FILE-3: a RAM-path transfer released with every chunk in was written
      // by its FILE_END (or refused there, which is rare: then this counts a
      // file too many until the free figure is next read). From here on it is
      // no longer in flight (slotUnwrittenBytes), so its bytes are counted
      // against the stored figure instead. A counter add: no filesystem work.
      if ((s.state == FILE_SLOT_RECEIVING || s.state == FILE_SLOT_COMPLETING) && s.totalSize != 0 &&
          s.receivedChunks == s.totalChunks && s.receivedBytes == s.totalSize) {
        VFS::noteBulkWriteTaken(s.totalSize);
      }
      releaseLocked(s);
      return;
    }
    s.state           = FILE_SLOT_COMPLETING;
    s.teardownPending = false;
  }
  {
    const int idx = (int)(&s - gFileSlots);
    FsLockGuard fg("fileslots.stream.release");  // hold the FS lock for close + remove
    if (idx >= 0 && idx < kFileSlots && gStreamFile[idx]) {
      gStreamFile[idx].close();
    }
    if (s.partPath[0]) {
      // Best-effort: removes the .part on failure/timeout. After a successful
      // FILE_END rename the .part no longer exists, so this is a harmless no-op.
      AuthContext ctx = VFS::systemAuth(VFS::Scopes::ESPNOW_RECEIVED, "espnow.file_part_cleanup");
      VFS::removeGuarded(s.partPath, ctx);
    }
  }
  FileSlotsLockGuard g(kFileSlotsLockWait);
  if (g.ok) releaseLocked(s);
}

}  // namespace

bool fileSlotsInit() {
  if (gFileSlots && gFileSlotsMutex) return true;
  if (!gFileSlotsMutex) {
    gFileSlotsMutex = xSemaphoreCreateMutex();
    if (!gFileSlotsMutex) {
      ERROR_ESPNOWF("[FileSlots] mutex create failed");
      return false;
    }
  }
  if (!gFileSlots) {
    gFileSlots = (FileTransferSlotImpl*)ps_alloc(
      sizeof(FileTransferSlotImpl) * kFileSlots,
      AllocPref::PreferPsram, "espnow.fileslots");
    if (!gFileSlots) {
      ERROR_ESPNOWF("[FileSlots] failed to allocate %u-slot table", (unsigned)kFileSlots);
      return false;
    }
    memset(gFileSlots, 0, sizeof(FileTransferSlotImpl) * kFileSlots);
  }
  INFO_ESPNOWF("[FileSlots] initialized %u slots", (unsigned)kFileSlots);
  return true;
}

void fileSlotsBootCleanup() {
  // Remove orphaned staging files left if a crash / power-loss hit a transfer
  // before its .part was renamed into place. How wide that window is depends on
  // the path:
  //   - RAM path: brief. The write + atomic rename happen in one synchronous
  //     v4h_file_end call, so an orphan needs a crash inside that window.
  //   - Streaming path: the ENTIRE transfer. The .part is opened at the first
  //     flush and stays open until FILE_END (multi-minute for a big file), so
  //     any reset mid-transfer strands one — these are the common case.
  // Sweeping also keeps stray "/espnow/received/.part-*" temps from accumulating
  // and cluttering the file browser.
  const char* dirPath = "/espnow/received";
  AuthContext ctx = VFS::systemAuth(VFS::Scopes::ESPNOW_RECEIVED, "espnow.file_boot_cleanup");
  File dir = VFS::openGuarded(dirPath, "r", ctx);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    return;
  }
  // Collect basenames first — removing during openNextFile() can invalidate
  // the LittleFS directory iterator.
  char victims[8][96];
  uint8_t n = 0;
  for (File e = dir.openNextFile(); e && n < 8; e = dir.openNextFile()) {
    if (!e.isDirectory()) {
      const char* nm = e.name();
      const char* base = strrchr(nm, '/');
      base = base ? base + 1 : nm;
      if (strncmp(base, ".part-", 6) == 0) {
        snprintf(victims[n], sizeof(victims[n]), "%s/%s", dirPath, base);
        n++;
      }
    }
    e.close();
  }
  dir.close();
  for (uint8_t i = 0; i < n; i++) {
    if (VFS::removeGuarded(victims[i], ctx)) {
      INFO_ESPNOWF("[FileSlots] boot cleanup removed orphan %s", victims[i]);
    }
  }
}

// FILE-3: what a slot's transfer may still put on LittleFS. A RAM-mode file is
// written whole at FILE_END, a streaming one has its unreceived bytes and at
// most its two half-buffers to go. Free and failed slots write nothing.
static uint32_t slotUnwrittenBytes(const FileTransferSlotImpl& s) {
  if (s.state != FILE_SLOT_RECEIVING && s.state != FILE_SLOT_COMPLETING) return 0;
  if (!s.streaming) return s.totalSize;
  const uint32_t unreceived = s.totalSize > s.receivedBytes ? s.totalSize - s.receivedBytes : 0;
  const uint64_t left = (uint64_t)unreceived + (uint64_t)s.streamBufCap * kFileStreamBufs;
  return left > s.totalSize ? s.totalSize : (uint32_t)left;
}

FileTransferSlot* fileSlotsAllocate(const uint8_t peerMac[6],
                                     uint32_t transferId,
                                     const char* filename,
                                     uint32_t fileSize,
                                     uint16_t chunkCount,
                                     uint16_t chunkSize,
                                     uint8_t kind,
                                     const char** errOut) {
  auto setErr = [&](const char* e) { if (errOut) *errOut = e; };
  if (!gFileSlots || !peerMac || !filename) {
    setErr(kFileSlotErrBadArgs);
    return nullptr;
  }
  if (fileSize > kFileSlotMaxStreamSize) {
    setErr(kFileSlotErrTooBig);
    return nullptr;
  }
  if (chunkSize == 0 && fileSize > 0) {
    setErr(kFileSlotErrBadArgs);
    return nullptr;
  }
  if (chunkCount == 0 && fileSize > 0) {
    setErr(kFileSlotErrBadArgs);
    return nullptr;
  }

  FileSlotsLockGuard g;
  if (!g.ok) {
    setErr(kFileSlotErrBusy);
    return nullptr;
  }

  // Filename identity hash. strnlen-bounded to the slot's name buffer (the
  // handler hands a terminated copy of at most kFileSlotNameMax bytes); the
  // bound matches the strncpy below so the hash always describes exactly what
  // the slot stores. This is the stable per-(peer,file) key: unlike
  // transferId it survives sender retries (each sendFileToMac call draws a
  // fresh transferId).
  const uint32_t nameHash = esp_crc32_le(0, (const uint8_t*)filename,
      strnlen(filename, sizeof(FileTransferSlotImpl::filename) - 1));

  // Slot scan: find a free slot and resolve conflicts with THIS peer's active
  // slots. Two distinct match kinds, and conflating them is transfer-fatal:
  //
  //   same (peer, transferId) = a DUPLICATE FILE_START of the transfer already
  //                             in flight: a copy the accept memory did not
  //                             catch (sent again under a new msgId, or after
  //                             the memory's 5 s).
  //                             Treat it as IDEMPOTENT: hand back the existing
  //                             slot untouched. Tearing it down here would
  //                             discard every chunk received so far, and for a
  //                             streaming slot would transiently leave TWO
  //                             slots sharing (peer,transferId) — which
  //                             fileSlotsFindByTransfer cannot disambiguate, so the
  //                             still-arriving chunks would route to the dead
  //                             one, emit FILE_CANCEL, and (with the abort
  //                             matcher) kill the sender's healthy transfer.
  //
  //   same (peer, nameHash),
  //   DIFFERENT transferId    = a genuine sender RETRY (every sendFileToMac
  //                             call draws a fresh transferId), or a CRC32 name
  //                             collision. Either way the old stream can never
  //                             complete — SUPERSEDE it. RAM slots release
  //                             inline (espnow_task owns all their state);
  //                             streaming slots MUST defer teardown to
  //                             cmd_exec, which may be mid-write on their
  //                             .part handle (see the gStreamFile ownership
  //                             comment), so they fail via
  //                             fileSlotsStreamFailLocked and the new transfer
  //                             takes a DIFFERENT free slot.
  //
  // Different peers never conflict — the destination inbox is per-peer
  // (/espnow/received/<mac>/), so the old cross-peer PATH_BUSY rejection was
  // blocking legitimate same-named transfers for no reason.
  // Pass 1 — survey only. Nothing is destroyed here: a supersede has to know a
  // slot will actually be available before it kills the incumbent, or a retry
  // that arrives with the table full would take out the old transfer AND be
  // rejected itself (the sender is fire-and-forget; it would not try again).
  int freeIdx = -1;
  int supersedeIdx = -1;
  for (uint8_t i = 0; i < kFileSlots; i++) {
    FileTransferSlotImpl& s = gFileSlots[i];
    if (s.state == FILE_SLOT_FREE) {
      if (freeIdx < 0) freeIdx = (int)i;
      continue;
    }
    if (memcmp(s.peerMac, peerMac, 6) != 0) continue;
    // COMPLETING slots belong to their cmd_exec finalize/abort job. Their
    // staging file carries the slot index, so it can never be the one this new
    // transfer is about to open — no interlock needed here.
    if (s.state != FILE_SLOT_RECEIVING) continue;

    const bool sameId   = (s.transferId == transferId);
    const bool sameFile = (s.filenameHash == nameHash);
    if (!sameId && !sameFile) continue;

    // A true duplicate FILE_START must describe the SAME transfer in every
    // field, not just carry the same transferId. The id is random, but a
    // 32-bit draw can repeat — adopting that slot would pour a
    // different file into the old one's buffer at the old one's offsets and
    // then report the failure against the WRONG filename.
    if (sameId && sameFile && s.totalSize == fileSize &&
        s.totalChunks == chunkCount && s.chunkSize == chunkSize && s.kind == kind) {
      DEBUG_ESPNOWF("[FileSlots] duplicate FILE_START for slot %u (transferId=%lu) — reusing, %u/%u chunks so far",
                    (unsigned)i, (unsigned long)transferId,
                    (unsigned)s.receivedChunks, (unsigned)s.totalChunks);
      s.lastFrameMs = (uint32_t)millis();   // don't let the dup age the slot out
      setErr(nullptr);
      return reinterpret_cast<FileTransferSlot*>(&s);
    }
    // Otherwise the incumbent is dead: either a genuine sender retry (same
    // file, fresh transferId — sendFileToMac draws one per call) or a reused id
    // describing a different file. Either way it can never complete.
    if (supersedeIdx < 0) supersedeIdx = (int)i;
  }

  // FILE-3: storage admission, before anything is torn down for the newcomer.
  // Received files land on LittleFS (/espnow/received), so the file has to fit
  // above the log reserve together with what the other transfers in flight
  // will still write. This runs on espnow_task: the stored free figure only
  // (VFS::BulkFigure::Peek), never a volume walk. A refusal is answered by the
  // caller like every other allocation error (the FILE_START's ACK status). A
  // streaming transfer is asked again, with a fresh figure, when cmd_exec
  // opens its .part (streamDrainPending).
  {
    uint64_t pendingBytes = fileSize;
    for (uint8_t i = 0; i < kFileSlots; i++) {
      if ((int)i == supersedeIdx) continue;  // replaced by this transfer
      pendingBytes += slotUnwrittenBytes(gFileSlots[i]);
    }
    const size_t ask = pendingBytes > SIZE_MAX ? SIZE_MAX : (size_t)pendingBytes;
    if (!VFS::admitBulkWrite(VFS::INTERNAL, ask, "ESP-NOW file receive", VFS::BulkFigure::Peek)) {
      setErr(kFileSlotErrStorage);
      return nullptr;
    }
  }

  // Pass 2 — act, now that freeIdx is known.
  if (supersedeIdx >= 0) {
    FileTransferSlotImpl& old = gFileSlots[supersedeIdx];
    if (!old.streaming) {
      WARN_ESPNOWF("[FileSlots] FILE_START supersedes slot %u ('%s', old transferId=%lu, new transferId=%lu) — releasing old",
                   (unsigned)supersedeIdx, old.filename,
                   (unsigned long)old.transferId, (unsigned long)transferId);
      releaseLocked(old);              // RAM slot: espnow_task owns all its state
      if (freeIdx < 0) freeIdx = supersedeIdx;
    } else if (freeIdx >= 0) {
      WARN_ESPNOWF("[FileSlots] FILE_START supersedes slot %u ('%s', old transferId=%lu, new transferId=%lu) — failing old on cmd_exec",
                   (unsigned)supersedeIdx, old.filename,
                   (unsigned long)old.transferId, (unsigned long)transferId);
      fileSlotsStreamFailLocked(old);  // -> COMPLETING; its abort job frees it
    } else {
      // Table full and the incumbent is streaming (we cannot reuse its index —
      // only cmd_exec may free it). Reject the newcomer and leave the old
      // transfer alone: killing it here would lose both.
      WARN_ESPNOWF("[FileSlots] BUSY: cannot supersede streaming slot %u with no free slot to move into",
                   (unsigned)supersedeIdx);
      setErr(kFileSlotErrBusy);
      return nullptr;
    }
  }
  if (freeIdx < 0) {
    WARN_ESPNOWF("[FileSlots] BUSY: all %u slots in use", (unsigned)kFileSlots);
    setErr(kFileSlotErrBusy);
    return nullptr;
  }

  FileTransferSlotImpl& s = gFileSlots[freeIdx];
  memset(&s, 0, sizeof(s));

  const bool streaming = (fileSize > kFileSlotMaxFileSize);
  if (streaming) {
    // Streaming path: two PSRAM half-buffers, no whole-file buffer / bitmap. The
    // .part is opened lazily on cmd_exec at the first flush, so espnow_task does
    // ZERO flash here. Bond config files never reach here (< 128 KB → RAM path).
    bool allocOk = true;
    for (uint8_t b = 0; b < kFileStreamBufs; b++) {
      s.streamBuf[b] = (uint8_t*)ps_alloc(kFileStreamBufSize, AllocPref::RequirePsram,
                                          "espnow.file.streambuf");
      if (!s.streamBuf[b]) { allocOk = false; break; }
    }
    if (!allocOk) {
      for (uint8_t b = 0; b < kFileStreamBufs; b++) {
        if (s.streamBuf[b]) { ps_free(s.streamBuf[b]); s.streamBuf[b] = nullptr; }
      }
      ERROR_ESPNOWF("[FileSlots] stream buffer alloc failed (%u x %lu bytes)",
                    (unsigned)kFileStreamBufs, (unsigned long)kFileStreamBufSize);
      setErr(kFileSlotErrAlloc);
      return nullptr;
    }
    s.streamBufCap = kFileStreamBufSize;
    // Stage path = (peer, filenameHash, SLOT INDEX).
    //
    // The (peer, filenameHash) part is the FILE's identity, not the attempt's
    // id: a retry (fresh transferId) can find its predecessor's partial, and
    // partials of different attempts never "w"-truncate each other.
    // A future resume can glob this prefix to find a candidate partial.
    //
    // The slot index makes the path unique among LIVE slots, which is load
    // bearing: a slot whose cmd_exec teardown could not be queued parks in
    // COMPLETING (teardownPending) still holding its open handle, and a retry
    // of the same file legitimately allocates a different slot. Without the
    // index both would name the same file, and the parked slot's eventual
    // abort — releaseSlot closes the handle and removeGuarded()s partPath —
    // would delete the file the live transfer is streaming into. The receive
    // CRC could not catch it either: it is accumulated over bytes as they
    // arrive, never re-read from flash.
    snprintf(s.partPath, sizeof(s.partPath),
             "/espnow/received/.part-strm-%02x%02x%02x%02x%02x%02x-%08lx-%u",
             peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5],
             (unsigned long)nameHash, (unsigned)freeIdx);
    s.streaming = true;
    INFO_ESPNOWF("[FileSlots] slot %u STREAMING '%.63s' to %s (%lu bytes)",
                 (unsigned)freeIdx, filename, s.partPath, (unsigned long)fileSize);
  } else {
    uint32_t allocSize = fileSize > 0 ? fileSize : 1;
    s.dataBuffer = (uint8_t*)ps_alloc(allocSize, AllocPref::RequirePsram,
                                      "espnow.file.data");
    if (!s.dataBuffer) {
      ERROR_ESPNOWF("[FileSlots] PSRAM alloc failed (%lu bytes)", (unsigned long)allocSize);
      setErr(kFileSlotErrAlloc);
      return nullptr;
    }
    s.dataBufferLen = allocSize;

    s.chunkMapBytes = (uint16_t)((chunkCount + 7) / 8);
    if (s.chunkMapBytes == 0) s.chunkMapBytes = 1;
    s.chunkMap = (uint8_t*)ps_alloc(s.chunkMapBytes, AllocPref::RequirePsram,
                                    "espnow.file.chunkmap");  // PSRAM, matches dataBuffer
    if (!s.chunkMap) {
      ERROR_ESPNOWF("[FileSlots] chunkMap alloc failed (%u bytes)", (unsigned)s.chunkMapBytes);
      ps_free(s.dataBuffer);
      s.dataBuffer = nullptr;
      setErr(kFileSlotErrAlloc);
      return nullptr;
    }
    memset(s.chunkMap, 0, s.chunkMapBytes);
  }

  memcpy(s.peerMac, peerMac, 6);
  s.transferId     = transferId;
  s.kind           = kind;
  s.filenameHash   = nameHash;
  strncpy(s.filename, filename, sizeof(s.filename) - 1);
  s.filename[sizeof(s.filename) - 1] = '\0';
  s.totalSize      = fileSize;
  s.totalChunks    = chunkCount;
  s.chunkSize      = chunkSize;
  s.receivedBytes  = 0;
  s.receivedChunks = 0;
  s.startedMs      = (uint32_t)millis();
  s.lastFrameMs    = s.startedMs;
  s.state          = FILE_SLOT_RECEIVING;

  INFO_ESPNOWF("[FileSlots] slot %u allocated for '%.63s' from " MAC_FMT " "
               "(transferId=%lu, %lu bytes, %u chunks)",
               (unsigned)freeIdx, filename,
               MAC_ARGS(peerMac),
               (unsigned long)transferId, (unsigned long)fileSize, (unsigned)chunkCount);
  return reinterpret_cast<FileTransferSlot*>(&s);
}

FileTransferSlot* fileSlotsFindByTransfer(const uint8_t peerMac[6], uint32_t transferId,
                                     FileSlotLookup* resultOut) {
  auto setResult = [&](FileSlotLookup r) { if (resultOut) *resultOut = r; };
  setResult(FILE_SLOT_LOOKUP_NOT_FOUND);
  if (!gFileSlots || !peerMac) return nullptr;
  FileSlotsLockGuard g;
  if (!g.ok) {
    // The table could not be read in the bounded wait. Nothing is known about
    // this transfer — do not let the caller mistake that for "no such slot".
    setResult(FILE_SLOT_LOOKUP_BUSY);
    return nullptr;
  }
  for (uint8_t i = 0; i < kFileSlots; i++) {
    FileTransferSlotImpl& s = gFileSlots[i];
    if (s.state == FILE_SLOT_FREE) continue;
    if (s.transferId == transferId && memcmp(s.peerMac, peerMac, 6) == 0) {
      setResult(FILE_SLOT_LOOKUP_FOUND);
      return reinterpret_cast<FileTransferSlot*>(&s);
    }
  }
  return nullptr;
}

bool fileSlotsWriteChunk(FileTransferSlot* slot,
                          uint16_t chunkIdx,
                          const uint8_t* data,
                          uint16_t dataLen) {
  if (!slot || !data) return false;
  FileTransferSlotImpl& s = *impl(slot);

  // Streaming slots go through fileSlotsStreamAppend (deferred-write to cmd_exec).
  // This function is the RAM path only.
  if (s.streaming) return false;

  if (s.state != FILE_SLOT_RECEIVING) return false;
  if (s.chunkSize == 0 || s.totalChunks == 0) return false;
  if (chunkIdx >= s.totalChunks) return false;

  uint32_t offset = (uint32_t)chunkIdx * (uint32_t)s.chunkSize;
  if (offset + dataLen > s.dataBufferLen) {
    ERROR_ESPNOWF("[FileSlots] chunk bounds: offset=%lu + len=%u > buf=%lu",
                  (unsigned long)offset, dataLen, (unsigned long)s.dataBufferLen);
    return false;
  }

  uint16_t byteIndex = (uint16_t)(chunkIdx / 8);
  uint8_t  bitMask   = (uint8_t)(1u << (chunkIdx % 8));
  bool alreadyHave = (byteIndex < s.chunkMapBytes) &&
                     ((s.chunkMap[byteIndex] & bitMask) != 0);
  // Always write (covers retries with corrupted prior frame); only count
  // chunk + bytes once.
  memcpy(s.dataBuffer + offset, data, dataLen);
  s.lastFrameMs = (uint32_t)millis();
  if (!alreadyHave) {
    if (byteIndex < s.chunkMapBytes) s.chunkMap[byteIndex] |= bitMask;
    s.receivedBytes  += dataLen;
    s.receivedChunks++;
    return true;
  }
  return true;
}

bool fileSlotsIsComplete(const FileTransferSlot* slot) {
  if (!slot) return false;
  const FileTransferSlotImpl& s = *implC(slot);
  if (s.streaming && s.streamFailed) return false;  // a gap/write error is unrecoverable in stream mode
  if (s.totalSize == 0) return true;
  return (s.receivedChunks == s.totalChunks) && (s.receivedBytes == s.totalSize);
}

bool fileSlotsIsStreaming(const FileTransferSlot* slot) {
  return slot ? implC(slot)->streaming : false;
}
const char* fileSlotsGetPartPath(const FileTransferSlot* slot) {
  return slot ? implC(slot)->partPath : "";
}

namespace {

// Mark a streaming slot failed and queue its teardown on cmd_exec. CALLER HOLDS
// gFileSlotsMutex. Idempotent: only the RECEIVING→COMPLETING edge fires, so the
// timeout sweep, a gap, and FILE_END can't double-tear-down.
void fileSlotsStreamFailLocked(FileTransferSlotImpl& s) {
  if (!s.streaming || s.state != FILE_SLOT_RECEIVING) return;
  s.streamFailed = true;
  s.state = FILE_SLOT_COMPLETING;
  // Hand close/delete/free to cmd_exec so it never races an in-flight write.
  if (!submitDeferredToCmdExec(fileSlotsStreamAbortJob, reinterpret_cast<FileTransferSlot*>(&s))) {
    // Queue full. Do NOT release inline: this runs on espnow_task, and a
    // streaming slot's buffers/.part handle may be in use RIGHT NOW by
    // streamDrainPending on cmd_exec, which deliberately does its flash write
    // outside the slot mutex (see the gStreamFile ownership comment). Freeing
    // streamBuf[] here would hand that write a null pointer — LoadProhibited.
    // The two conditions are correlated (the queue fills precisely when
    // cmd_exec is stalled in a slow flash write), so this is not hypothetical.
    // Instead leave the slot COMPLETING with teardownPending set; the timeout
    // sweep retries the submit every tick until it lands. COMPLETING slots are
    // invisible to allocate and to the sweep's expiry logic, so nothing races
    // it in the meantime — it costs one slot until the queue drains.
    s.teardownPending = true;
    WARN_ESPNOWF("[FileSlots] cmd_exec queue full — deferring streaming teardown of slot %u to the sweep",
                 (unsigned)(&s - gFileSlots));
  }
}

// Write every buffer in the FIFO to the .part, in order. Runs on cmd_exec. The
// flash write happens OUTSIDE gFileSlotsMutex (taken only for the tiny FIFO/metadata
// steps) so espnow_task's fills are never blocked on flash. Lazily opens the .part
// on the first write. Sets streamFailed on any error.
void streamDrainPending(FileTransferSlotImpl& s) {
  int idx = (int)(&s - gFileSlots);
  if (idx < 0 || idx >= kFileSlots) return;
  for (;;) {
    uint8_t  buf;
    uint32_t len;
    uint32_t total;
    bool     needOpen;
    char     partPathCopy[80];
    {
      FileSlotsLockGuard g(kFileSlotsLockWait);
      if (!g.ok) return;
      if (!s.streaming || s.streamFailed || s.streamPendCount == 0) return;
      buf      = s.streamPendQ[s.streamPendHead];
      len      = s.streamBufLen[buf];
      total    = s.totalSize;
      needOpen = !s.streamOpened;
      strlcpy(partPathCopy, s.partPath, sizeof(partPathCopy));
    }
    // --- flash work, NO slot mutex held (a PENDING buffer is never touched by
    //     espnow_task, so reading streamBuf[buf] here is race-free) ---
    bool wrote = false, openedNow = false;
    {
      FsLockGuard fg("fileslots.stream.write");
      // FILE-3: the acceptance on espnow_task used a stored free figure that
      // may be old. Nothing of this file is on flash yet, so ask once more
      // here on cmd_exec, where a fresh reading (a volume walk when the stored
      // one is stale) is allowed. Refused: no .part is made, the write below
      // does not happen and the transfer fails like any stream write error.
      if (needOpen && !VFS::admitBulkWrite(VFS::INTERNAL, total, "ESP-NOW file receive",
                                           VFS::BulkFigure::Refresh)) {
        ERROR_ESPNOWF("[FileSlots] no room above the reserve for %s (%lu bytes): transfer failed",
                      partPathCopy, (unsigned long)total);
      } else if (needOpen) {
        AuthContext actx = VFS::systemAuth(VFS::Scopes::ESPNOW_RECEIVED, "espnow.file_stream_open");
        VFS::mkdirGuarded("/espnow/received", actx);
        gStreamFile[idx] = VFS::openGuarded(partPathCopy, "w", actx, true);
        if (gStreamFile[idx]) openedNow = true;
        else ERROR_ESPNOWF("[FileSlots] stream open failed: %s", partPathCopy);
      }
      if (gStreamFile[idx]) {
        size_t wn = gStreamFile[idx].write(s.streamBuf[buf], len);
        wrote = (wn == len);
        if (!wrote) ERROR_ESPNOWF("[FileSlots] stream write short (%u/%lu)", (unsigned)wn, (unsigned long)len);
        // FILE-3: these bytes are on flash and no longer in flight
        // (slotUnwrittenBytes): count them against the stored free figure.
        else VFS::noteBulkWriteTaken(len);
      }
    }
    {
      // Must not be skipped: the buffer above is on flash now, and leaving it in
      // the FIFO would write it a second time on the next drain while the
      // arrival-side CRC still matched.
      FileSlotsLockGuard g(kFileSlotsLockWait);
      if (!g.ok) return;
      if (openedNow) s.streamOpened = true;
      if (!wrote) { s.streamFailed = true; return; }
      s.streamPendHead = (uint8_t)((s.streamPendHead + 1) % kFileStreamBufs);
      if (s.streamPendCount > 0) s.streamPendCount--;
    }
  }
}

}  // namespace

StreamAppendResult fileSlotsStreamAppend(FileTransferSlot* slot, uint16_t chunkIdx,
                                         const uint8_t* data, uint16_t dataLen) {
  if (!slot || !data) return STREAM_APPEND_FAIL;
  FileTransferSlotImpl& s = *impl(slot);
  FileSlotsLockGuard g;
  if (!g.ok) return STREAM_APPEND_BUSY;   // nothing read, nothing changed — not a failure of the transfer
  if (!s.streaming) return STREAM_APPEND_FAIL;
  // Abort already in progress from an earlier chunk (streamFailed set, or slot
  // handed to COMPLETING teardown): report ALREADY_FAILED so the caller cancels
  // without emitting another durable RECV_FAILED — the first failing chunk logged it.
  // Distinguish "this transfer is dying" from "this transfer already finished".
  // A slot that left RECEIVING WITHOUT streamFailed is finalizing successfully
  // (FILE_END processed, finalize job queued); a late or MAC-duplicated chunk
  // arriving now must not make the caller emit a FILE_CANCEL — that cancel
  // would reach the sender, match its abort slot, and turn a transfer that
  // actually succeeded into a reported failure.
  if (s.state != FILE_SLOT_RECEIVING && !s.streamFailed) return STREAM_APPEND_FINALIZING;
  if (s.streamFailed || s.state != FILE_SLOT_RECEIVING) return STREAM_APPEND_ALREADY_FAILED;

  if (s.chunkSize == 0 || s.totalChunks == 0 || chunkIdx >= s.totalChunks) {
    fileSlotsStreamFailLocked(s);
    return STREAM_APPEND_FAIL;
  }
  if (chunkIdx < s.nextChunk) return STREAM_APPEND_DUP;        // duplicate/old
  if (chunkIdx > s.nextChunk) {                                // gap — a chunk was lost
    WARN_ESPNOWF("[FileSlots] stream gap: got chunk %u, expected %u — aborting transfer",
                 (unsigned)chunkIdx, (unsigned)s.nextChunk);
    fileSlotsStreamFailLocked(s);
    return STREAM_APPEND_FAIL;
  }
  if (dataLen > s.streamBufCap) {                              // malformed: chunk bigger than a buffer
    ERROR_ESPNOWF("[FileSlots] stream chunk %u len %u > buf %lu — aborting",
                  (unsigned)chunkIdx, (unsigned)dataLen, (unsigned long)s.streamBufCap);
    fileSlotsStreamFailLocked(s);
    return STREAM_APPEND_FAIL;
  }

  uint8_t act = s.streamActive;
  if (s.streamFill + dataLen > s.streamBufCap) {               // active buffer full → commit + switch
    if (s.streamPendCount >= 1) {
      // No free buffer: cmd_exec is a full buffer behind. Rare on an idle receiver;
      // abort cleanly rather than silently drop into a gap.
      WARN_ESPNOWF("[FileSlots] stream backpressure (writer behind) — aborting");
      fileSlotsStreamFailLocked(s);
      return STREAM_APPEND_FAIL;
    }
    s.streamBufLen[act] = s.streamFill;
    s.streamPendQ[(s.streamPendHead + s.streamPendCount) % kFileStreamBufs] = act;
    s.streamPendCount++;
    if (!s.streamDrainQueued) {
      if (submitDeferredToCmdExec(fileSlotsStreamDrainJob, slot)) {
        s.streamDrainQueued = true;
      } else {
        WARN_ESPNOWF("[FileSlots] stream drain submit failed (queue full) — aborting");
        fileSlotsStreamFailLocked(s);
        return STREAM_APPEND_FAIL;
      }
    }
    s.streamActive = (uint8_t)((act + 1) % kFileStreamBufs);
    act = s.streamActive;
    s.streamFill = 0;
  }
  memcpy(s.streamBuf[act] + s.streamFill, data, dataLen);
  // Whole-file CRC, accumulated at the accept point only: the dup/gap checks
  // above guarantee strictly-in-order, exactly-once bytes, so this equals a
  // one-shot CRC of the finished file. ~200 table lookups per chunk against a
  // ~21 ms/chunk cadence — noise. Compared against FILE_END's declared value
  // in v4h_file_end before finalize is allowed to begin.
  s.runningCrc = esp_crc32_le(s.runningCrc, data, dataLen);
  s.streamFill += dataLen;
  s.nextChunk++;
  s.receivedBytes += dataLen;
  s.receivedChunks++;
  s.lastFrameMs = (uint32_t)millis();
  return STREAM_APPEND_OK;
}

void fileSlotsStreamDrainJob(void* arg) {        // cmd_exec
  FileTransferSlot* slot = (FileTransferSlot*)arg;
  if (!slot) return;
  FileTransferSlotImpl& s = *impl(slot);
  for (;;) {
    streamDrainPending(s);
    // Re-arm: the next committed buffer queues a fresh drain. Waits for the lock —
    // a skipped re-arm would leave the flag set and no drain would ever run again.
    // The flag is cleared only while the FIFO is seen empty under the same lock
    // hold: a buffer the receive task committed after the drain's last look found
    // the flag still set and queued nothing, so it is drained here instead of
    // being stranded until the other buffer fills and the transfer is aborted
    // for backpressure.
    FileSlotsLockGuard g(kFileSlotsLockWait);
    if (!g.ok) return;
    // A failed stream keeps its unwritten buffer in the FIFO (streamDrainPending
    // returns without popping it, and returns at once from then on), so the
    // FIFO never empties: without this exit the job spun here for good, on
    // cmd_exec, after any .part open or write error. The buffers are freed by
    // the teardown that FILE_END, the next chunk's cancel or the timeout sweep
    // starts. Found 2026-11-20 with the FILE-3 refusal, which takes this path.
    if (s.streamPendCount == 0 || s.streamFailed) { s.streamDrainQueued = false; return; }
  }
}

void fileSlotsStreamAbortJob(void* arg) {        // cmd_exec
  FileTransferSlot* slot = (FileTransferSlot*)arg;
  if (!slot) return;
  // close + delete .part + free buffers + FREE. Waits for the slot lock: this job
  // is the only thing that will ever free the slot (a COMPLETING slot is skipped
  // by allocate and by the sweep), so giving up on a busy lock pinned it for good.
  releaseSlot(*impl(slot));
}

void fileSlotsStreamFail(FileTransferSlot* slot) {
  if (!slot) return;
  FileSlotsLockGuard g;
  if (!g.ok) return;
  fileSlotsStreamFailLocked(*impl(slot));
}

bool fileSlotsStreamBeginFinalize(FileTransferSlot* slot) {     // espnow_task (FILE_END)
  if (!slot) return false;
  FileTransferSlotImpl& s = *impl(slot);
  // Waits for the lock: the caller reads `false` as "the slot is already failing"
  // and cancels, which must not happen to a complete, CRC-checked file because
  // the table was busy for a moment.
  FileSlotsLockGuard g(kFileSlotsLockWait);
  if (!g.ok) return false;
  if (!s.streaming || s.state != FILE_SLOT_RECEIVING || s.streamFailed) return false;
  if (s.streamFill > 0) {                          // commit the trailing partial buffer
    uint8_t act = s.streamActive;
    s.streamBufLen[act] = s.streamFill;
    s.streamPendQ[(s.streamPendHead + s.streamPendCount) % kFileStreamBufs] = act;
    s.streamPendCount++;
    s.streamFill = 0;
  }
  s.state = FILE_SLOT_COMPLETING;                  // hands off to the finalize job
  return true;
}

bool fileSlotsBeginRamFinalize(FileTransferSlot* slot) {     // espnow_task (FILE_END)
  if (!slot) return false;
  FileTransferSlotImpl& s = *impl(slot);
  FileSlotsLockGuard g(kFileSlotsLockWait);
  if (!g.ok) return false;
  if (s.streaming || s.state != FILE_SLOT_RECEIVING) return false;
  s.state = FILE_SLOT_COMPLETING;  // the espnow_task loop finishes and releases it
  return true;
}

bool fileSlotsIsStreamFailing(const FileTransferSlot* slot) {
  if (!slot) return false;
  const FileTransferSlotImpl* s = implC(slot);
  return s->streaming && s->streamFailed;
}

bool fileSlotsIsRamFinalizing(const FileTransferSlot* slot) {
  if (!slot) return false;
  const FileTransferSlotImpl* s = implC(slot);
  return !s->streaming && s->state == FILE_SLOT_COMPLETING;
}

bool fileSlotsStreamFinalizeWrite(FileTransferSlot* slot) {     // cmd_exec
  if (!slot) return false;
  FileTransferSlotImpl& s = *impl(slot);
  streamDrainPending(s);                           // flush any remaining buffers
  bool ok;
  int  idx;
  {
    FileSlotsLockGuard g(kFileSlotsLockWait);
    if (!g.ok) return false;
    ok  = !s.streamFailed;
    idx = (int)(&s - gFileSlots);
  }
  {
    FsLockGuard fg("fileslots.stream.close");
    if (idx >= 0 && idx < kFileSlots && gStreamFile[idx]) {
      gStreamFile[idx].flush();
      gStreamFile[idx].close();
    }
  }
  return ok;
}

const uint8_t* fileSlotsGetBuffer(const FileTransferSlot* slot) {
  return slot ? implC(slot)->dataBuffer : nullptr;
}
uint32_t fileSlotsGetReceivedBytes(const FileTransferSlot* slot) {
  return slot ? implC(slot)->receivedBytes : 0;
}
const char* fileSlotsGetFilename(const FileTransferSlot* slot) {
  return slot ? implC(slot)->filename : "";
}
const uint8_t* fileSlotsGetSenderMac(const FileTransferSlot* slot) {
  return slot ? implC(slot)->peerMac : nullptr;
}
uint16_t fileSlotsGetReceivedChunks(const FileTransferSlot* slot) {
  return slot ? implC(slot)->receivedChunks : 0;
}
uint16_t fileSlotsGetTotalChunks(const FileTransferSlot* slot) {
  return slot ? implC(slot)->totalChunks : 0;
}
uint32_t fileSlotsGetTotalSize(const FileTransferSlot* slot) {
  return slot ? implC(slot)->totalSize : 0;
}
uint32_t fileSlotsGetReceivedCrc(const FileTransferSlot* slot) {
  return slot ? implC(slot)->runningCrc : 0;
}
bool fileSlotsIsReceiving(const FileTransferSlot* slot) {
  return slot ? (implC(slot)->state == FILE_SLOT_RECEIVING) : false;
}
uint32_t fileSlotsGetTransferId(const FileTransferSlot* slot) {
  return slot ? implC(slot)->transferId : 0;
}
uint8_t fileSlotsGetKind(const FileTransferSlot* slot) {
  return slot ? implC(slot)->kind : 0;
}
uint32_t fileSlotsExpectedChunkLen(const FileTransferSlot* slot, uint32_t chunkIdx) {
  if (!slot) return 0;
  const FileTransferSlotImpl& s = *implC(slot);
  if (s.chunkSize == 0 || chunkIdx >= s.totalChunks) return 0;
  const uint64_t offset = (uint64_t)chunkIdx * s.chunkSize;
  if (offset >= s.totalSize) return 0;
  const uint64_t left = s.totalSize - offset;
  return left < s.chunkSize ? (uint32_t)left : s.chunkSize;
}

// ---- Result memory (1.0 step 7) ----------------------------------------------
// The last FILE_RESULTs this device sent, so a copy of FILE_END that arrives
// after the slot was released (the sender did not hear the first FILE_RESULT
// and asks again) gets the same verdict instead of a refusal. PSRAM .bss;
// guarded by the slot-table lock (bounded wait on espnow_task, unbounded on
// cmd_exec, like every other taker).
namespace {
struct FileResultMemo {
  uint8_t  peerMac[6];
  uint8_t  status;
  uint8_t  used;
  uint32_t transferId;
  uint32_t bytesStored;
  uint32_t atMs;
};
constexpr uint8_t kFileResultMemos = 8;
EXT_RAM_BSS_ATTR FileResultMemo gFileResultMemos[kFileResultMemos];
uint8_t gFileResultNext = 0;

// CALLER HOLDS gFileSlotsMutex.
void rememberResultLocked(const uint8_t peerMac[6], uint32_t transferId, uint8_t status, uint32_t bytesStored) {
  FileResultMemo* slot = nullptr;
  for (uint8_t i = 0; i < kFileResultMemos; i++) {  // the same transfer again: overwrite it
    FileResultMemo& m = gFileResultMemos[i];
    if (m.used && m.transferId == transferId && memcmp(m.peerMac, peerMac, 6) == 0) { slot = &m; break; }
  }
  if (!slot) {
    slot = &gFileResultMemos[gFileResultNext];
    gFileResultNext = (uint8_t)((gFileResultNext + 1) % kFileResultMemos);
  }
  memcpy(slot->peerMac, peerMac, 6);
  slot->transferId  = transferId;
  slot->status      = status;
  slot->bytesStored = bytesStored;
  slot->atMs        = (uint32_t)millis();
  slot->used        = 1;
}
}  // namespace

void fileSlotsRememberResult(const uint8_t peerMac[6], uint32_t transferId, uint8_t status, uint32_t bytesStored) {
  if (!peerMac || transferId == 0) return;
  FileSlotsLockGuard g(kFileSlotsLockWait);
  if (!g.ok) return;
  rememberResultLocked(peerMac, transferId, status, bytesStored);
}

FileResultLookup fileSlotsFindResult(const uint8_t peerMac[6], uint32_t transferId, uint8_t* statusOut,
                                     uint32_t* bytesOut) {
  if (!peerMac || transferId == 0) return FILE_RESULT_LOOKUP_NOT_FOUND;
  FileSlotsLockGuard g;
  if (!g.ok) return FILE_RESULT_LOOKUP_BUSY;
  const uint32_t now = (uint32_t)millis();
  for (uint8_t i = 0; i < kFileResultMemos; i++) {
    const FileResultMemo& m = gFileResultMemos[i];
    if (!m.used || m.transferId != transferId || memcmp(m.peerMac, peerMac, 6) != 0) continue;
    if ((uint32_t)(now - m.atMs) >= kFileResultMemoryMs) return FILE_RESULT_LOOKUP_NOT_FOUND;
    if (statusOut) *statusOut = m.status;
    if (bytesOut) *bytesOut = m.bytesStored;
    return FILE_RESULT_LOOKUP_FOUND;
  }
  return FILE_RESULT_LOOKUP_NOT_FOUND;
}

void fileSlotsForgetResults() {
  FileSlotsLockGuard g(kFileSlotsLockWait);
  if (!g.ok) return;
  memset(gFileResultMemos, 0, sizeof(gFileResultMemos));
  gFileResultNext = 0;
}

void fileSlotsRelease(FileTransferSlot* slot) {
  if (!slot) return;
  releaseSlot(*impl(slot));
}

void fileSlotsMarkTeardownPending(FileTransferSlot* slot) {
  if (!slot) return;
  // Waits for the lock: this is the last resort that keeps a slot whose abort job
  // could not be queued reachable by the sweep; skipping it would pin the slot.
  FileSlotsLockGuard g(kFileSlotsLockWait);
  if (!g.ok) return;
  FileTransferSlotImpl& s = *impl(slot);
  if (!s.streaming || s.state == FILE_SLOT_FREE) return;
  s.streamFailed    = true;
  s.state           = FILE_SLOT_COMPLETING;
  s.teardownPending = true;
}

uint8_t fileSlotsTimeoutSweep(uint32_t nowMs, FileSlotExpiry* out, uint8_t outCap) {
  if (!gFileSlots) return 0;
  FileSlotsLockGuard g;
  if (!g.ok) return 0;
  uint8_t expired = 0;
  for (uint8_t i = 0; i < kFileSlots; i++) {
    FileTransferSlotImpl& s = gFileSlots[i];
    // Retry a teardown whose cmd_exec submit failed while the queue was full.
    // Only cmd_exec may free a streaming slot's buffers/.part handle, so the
    // slot sits COMPLETING until this lands. Not an expiry — no FILE_CANCEL,
    // the failing path already sent one.
    if (s.state == FILE_SLOT_COMPLETING && s.teardownPending) {
      if (submitDeferredToCmdExec(fileSlotsStreamAbortJob,
                                  reinterpret_cast<FileTransferSlot*>(&s))) {
        s.teardownPending = false;
        INFO_ESPNOWF("[FileSlots] deferred streaming teardown of slot %u queued", (unsigned)i);
      } else if ((nowMs - s.lastFrameMs) >= kFileSlotTimeoutMs) {
        // Still stuck a full timeout later. Nothing here can safely force it
        // (only cmd_exec may free a streaming slot), but a permanently wedged
        // teardown pins a slot plus two PSRAM buffers plus an open handle, so
        // make it visible instead of silently degrading the table. Re-armed
        // each timeout period so it reports periodically, not every 10 ms tick.
        s.lastFrameMs = nowMs;
        WARN_ESPNOWF("[FileSlots] slot %u teardown still unqueueable after %lums "
                     "(cmd_exec queue full / PSRAM exhausted) — slot pinned",
                     (unsigned)i, (unsigned long)kFileSlotTimeoutMs);
      }
      continue;
    }
    if (s.state != FILE_SLOT_RECEIVING) continue;  // FREE, or COMPLETING (already tearing down)
    if ((nowMs - s.lastFrameMs) < kFileSlotTimeoutMs) continue;
    WARN_ESPNOWF("[FileSlots] slot %u TIMEOUT: '%s' from " MAC_FMT " "
                 "(transferId=%lu, %u/%u chunks, no frame in %ums)",
                 (unsigned)i, s.filename,
                 MAC_ARGS(s.peerMac),
                 (unsigned long)s.transferId,
                 (unsigned)s.receivedChunks, (unsigned)s.totalChunks,
                 (unsigned)(nowMs - s.lastFrameMs));
    // Record (peer,transferId) so the caller can FILE_CANCEL(TIMEOUT) the sender
    // before we wipe the slot. Count ONLY what is written — the return value
    // is the number of valid out[] entries (a slot expiring past outCap is
    // still torn down below, just not reported; previously the unconditional
    // increment could claim entries that were never written).
    if (out && expired < outCap) {
      memcpy(out[expired].peerMac, s.peerMac, 6);
      out[expired].transferId = s.transferId;
      expired++;
    }
    // A FILE_END that still comes (late) is answered with this verdict.
    rememberResultLocked(s.peerMac, s.transferId, kFileResultTimeoutStatus, 0);
    // Streaming slots own a .part handle + buffers that cmd_exec may be mid-write
    // on — defer their teardown to cmd_exec (close/delete/free there). RAM slots
    // have no such handoff; release inline.
    if (s.streaming) fileSlotsStreamFailLocked(s);
    else             releaseLocked(s);
  }
  return expired;
}

uint8_t fileSlotsSlotCount() { return kFileSlots; }

uint8_t fileSlotsActiveCount() {
  if (!gFileSlots) return 0;
  uint8_t n = 0;
  for (uint8_t i = 0; i < kFileSlots; i++) {
    if (gFileSlots[i].state != FILE_SLOT_FREE) n++;
  }
  return n;
}

bool fileSlotsSnapshot(uint8_t slotIdx, FileTransferSlotInfo* out) {
  if (!gFileSlots || !out || slotIdx >= kFileSlots) return false;
  const FileTransferSlotImpl& s = gFileSlots[slotIdx];
  if (s.state == FILE_SLOT_FREE) return false;
  out->state          = s.state;
  memcpy(out->peerMac, s.peerMac, 6);
  out->transferId     = s.transferId;
  out->kind           = s.kind;
  strncpy(out->filename, s.filename, sizeof(out->filename) - 1);
  out->filename[sizeof(out->filename) - 1] = '\0';
  out->totalSize      = s.totalSize;
  out->receivedBytes  = s.receivedBytes;
  out->totalChunks    = s.totalChunks;
  out->receivedChunks = s.receivedChunks;
  out->chunkSize      = s.chunkSize;
  out->startedMs      = s.startedMs;
  out->lastFrameMs    = s.lastFrameMs;
  return true;
}

#endif  // ENABLE_ESPNOW
