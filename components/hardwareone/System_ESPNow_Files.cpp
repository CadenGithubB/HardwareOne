#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include "System_ESPNow_Files.h"

#include <Arduino.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "System_Debug.h"
#include "System_MemUtil.h"   // ps_alloc / AllocPref
#include "System_VFS.h"       // boot cleanup of orphaned .part staging files
#include "System_Mutex.h"     // FsLockGuard — the .part writes hold the FS lock
#include "System_CommandTypes.h"  // ExecReq::DeferredFn — drain/abort jobs on cmd_exec

// Hand a streaming flash write/teardown to cmd_exec_task so it never runs on
// espnow_task (the RX drain). Defined in System_Utils.cpp; the established pattern
// for "FS work that must not stall RX" (same as SESSION_OPEN/CONFIRM/REKEY).
extern bool submitDeferredToCmdExec(ExecReq::DeferredFn fn, void* arg);

namespace {

constexpr uint8_t kFileSlots = 4;

// Streaming double-buffer: espnow_task fills one 4 KB half while cmd_exec_task
// writes the other to the .part. Two halves = the 8 KB/slot we already used, just
// split so the radio task is never blocked on flash. One filled buffer = ~20 chunks
// (~430 ms) of slack for the writer to drain a NOR erase — plenty (writer turnaround
// is tens of ms on an idle receiver).
constexpr uint32_t kFileStreamBufSize = 4096;
constexpr uint8_t  kFileStreamBufs    = 2;

struct FileTransferSlotImpl {
  uint8_t  state;             // FileSlotState
  uint8_t  peerMac[6];
  uint32_t msgId;
  char     filename[64];
  uint32_t totalSize;
  uint32_t receivedBytes;
  uint16_t totalChunks;
  uint16_t receivedChunks;
  uint16_t chunkSize;
  uint32_t startedMs;
  uint32_t lastFrameMs;

  // --- RAM mode (file <= kFileSlotMaxFileSize): whole file buffered in PSRAM,
  //     written once at FILE_END. The original, unchanged path. ---
  uint8_t* dataBuffer;        // PSRAM, totalSize bytes (min 1)
  uint32_t dataBufferLen;     // capacity (== totalSize, capped)
  uint8_t* chunkMap;
  uint16_t chunkMapBytes;

  // --- Streaming mode (file > kFileSlotMaxFileSize): chunks stream to a .part file
  //     via cmd_exec_task; dataBuffer/chunkMap stay null. espnow_task fills the
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
  char     partPath[80];                 // on-flash staging path for this slot's .part
};

FileTransferSlotImpl* gFileSlots = nullptr;
SemaphoreHandle_t      gFileSlotsMutex = nullptr;

// Per-slot .part handle for streaming mode. Kept OUT of the POD struct (which is
// memset on alloc/release) because File is a C++ object with a destructor — a
// memset over it would leak the underlying handle. Indexed by slot index; opened
// in fileSlotsAllocate, written on espnow_task, closed in finalize/release. All
// access is serialized by gFileSlotsMutex.
File gStreamFile[kFileSlots];

struct FileSlotsLockGuard {
  bool ok;
  FileSlotsLockGuard() : ok(false) {
    if (gFileSlotsMutex) ok = (xSemaphoreTake(gFileSlotsMutex, pdMS_TO_TICKS(50)) == pdTRUE);
  }
  ~FileSlotsLockGuard() {
    if (ok && gFileSlotsMutex) xSemaphoreGive(gFileSlotsMutex);
  }
};

FileTransferSlotImpl* impl(FileTransferSlot* s) {
  return reinterpret_cast<FileTransferSlotImpl*>(s);
}
const FileTransferSlotImpl* implC(const FileTransferSlot* s) {
  return reinterpret_cast<const FileTransferSlotImpl*>(s);
}

void releaseLocked(FileTransferSlotImpl& s) {
  if (s.streaming) {
    int idx = (int)(&s - gFileSlots);
    {
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
    for (uint8_t b = 0; b < kFileStreamBufs; b++) {
      if (s.streamBuf[b]) {
        heap_caps_free(s.streamBuf[b]);
        s.streamBuf[b] = nullptr;
      }
    }
  }
  if (s.dataBuffer) {
    heap_caps_free(s.dataBuffer);
    s.dataBuffer = nullptr;
  }
  if (s.chunkMap) {
    heap_caps_free(s.chunkMap);
    s.chunkMap = nullptr;
  }
  memset(&s, 0, sizeof(s));
  s.state = FILE_SLOT_FREE;
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
      AllocPref::PreferPSRAM, "espnow.fileslots");
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
  // Remove orphaned staging files left if a crash / power-loss hit the brief
  // window between the staging write and the atomic rename in v4h_file_end.
  // Normally there are none (write + rename happen in one synchronous handler
  // call), but sweeping keeps stray "/espnow/received/.part-*" temps from
  // accumulating and cluttering the file browser.
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

FileTransferSlot* fileSlotsAllocate(const uint8_t peerMac[6],
                                     uint32_t msgId,
                                     const char* filename,
                                     uint32_t fileSize,
                                     uint16_t chunkCount,
                                     uint16_t chunkSize,
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

  // First pass: same-path conflict check (Phase 4 plan). Reject if any
  // active slot is already targeting this filename — prevents two peers
  // racing the same destination.
  int freeIdx = -1;
  for (uint8_t i = 0; i < kFileSlots; i++) {
    FileTransferSlotImpl& s = gFileSlots[i];
    if (s.state == FILE_SLOT_FREE) {
      if (freeIdx < 0) freeIdx = i;
      continue;
    }
    // Same (peer, msgId) — caller is re-sending FILE_START for an existing
    // transfer (e.g., they timed out and retried). Reset and reuse.
    if (s.msgId == msgId && memcmp(s.peerMac, peerMac, 6) == 0) {
      WARN_ESPNOWF("[FileSlots] re-FILE_START for slot %u (msgId=%lu peer match) — resetting",
                   (unsigned)i, (unsigned long)msgId);
      releaseLocked(s);
      freeIdx = (int)i;
      continue;
    }
    if (strncmp(s.filename, filename, sizeof(s.filename)) == 0) {
      WARN_ESPNOWF("[FileSlots] PATH_BUSY: '%s' already targeted by slot %u (msgId=%lu)",
                   filename, (unsigned)i, (unsigned long)s.msgId);
      setErr(kFileSlotErrPathBusy);
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
      s.streamBuf[b] = (uint8_t*)heap_caps_malloc(kFileStreamBufSize, MALLOC_CAP_SPIRAM);
      if (!s.streamBuf[b]) { allocOk = false; break; }
    }
    if (!allocOk) {
      for (uint8_t b = 0; b < kFileStreamBufs; b++) {
        if (s.streamBuf[b]) { heap_caps_free(s.streamBuf[b]); s.streamBuf[b] = nullptr; }
      }
      ERROR_ESPNOWF("[FileSlots] stream buffer alloc failed (%u x %lu bytes)",
                    (unsigned)kFileStreamBufs, (unsigned long)kFileStreamBufSize);
      setErr(kFileSlotErrAlloc);
      return nullptr;
    }
    s.streamBufCap = kFileStreamBufSize;
    // Stage path keyed by (peer, msgId) — same identity as the slot. msgId alone
    // can collide across two senders, and a streaming .part stays OPEN for the
    // whole (multi-minute) transfer, so a collision would interleave two writers
    // into one file. The peer MAC makes it unique.
    snprintf(s.partPath, sizeof(s.partPath),
             "/espnow/received/.part-strm-%02x%02x%02x%02x%02x%02x-%08lx",
             peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5],
             (unsigned long)msgId);
    s.streaming = true;
    INFO_ESPNOWF("[FileSlots] slot %u STREAMING '%s' to %s (%lu bytes)",
                 (unsigned)freeIdx, filename, s.partPath, (unsigned long)fileSize);
  } else {
    uint32_t allocSize = fileSize > 0 ? fileSize : 1;
    s.dataBuffer = (uint8_t*)heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM);
    if (!s.dataBuffer) {
      ERROR_ESPNOWF("[FileSlots] PSRAM alloc failed (%lu bytes)", (unsigned long)allocSize);
      setErr(kFileSlotErrAlloc);
      return nullptr;
    }
    s.dataBufferLen = allocSize;

    s.chunkMapBytes = (uint16_t)((chunkCount + 7) / 8);
    if (s.chunkMapBytes == 0) s.chunkMapBytes = 1;
    s.chunkMap = (uint8_t*)heap_caps_malloc(s.chunkMapBytes, MALLOC_CAP_SPIRAM);  // PSRAM, matches dataBuffer
    if (!s.chunkMap) {
      ERROR_ESPNOWF("[FileSlots] chunkMap alloc failed (%u bytes)", (unsigned)s.chunkMapBytes);
      heap_caps_free(s.dataBuffer);
      s.dataBuffer = nullptr;
      setErr(kFileSlotErrAlloc);
      return nullptr;
    }
    memset(s.chunkMap, 0, s.chunkMapBytes);
  }

  memcpy(s.peerMac, peerMac, 6);
  s.msgId          = msgId;
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

  INFO_ESPNOWF("[FileSlots] slot %u allocated for '%s' from %02X:%02X:%02X:%02X:%02X:%02X "
               "(msgId=%lu, %lu bytes, %u chunks)",
               (unsigned)freeIdx, filename,
               peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5],
               (unsigned long)msgId, (unsigned long)fileSize, (unsigned)chunkCount);
  return reinterpret_cast<FileTransferSlot*>(&s);
}

FileTransferSlot* fileSlotsFindByMsg(const uint8_t peerMac[6], uint32_t msgId) {
  if (!gFileSlots || !peerMac) return nullptr;
  FileSlotsLockGuard g;
  if (!g.ok) return nullptr;
  for (uint8_t i = 0; i < kFileSlots; i++) {
    FileTransferSlotImpl& s = gFileSlots[i];
    if (s.state == FILE_SLOT_FREE) continue;
    if (s.msgId == msgId && memcmp(s.peerMac, peerMac, 6) == 0) {
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
    // Queue full — fall back to inline teardown (we're already failing; rare).
    releaseLocked(s);
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
    bool     needOpen;
    char     partPathCopy[80];
    {
      FileSlotsLockGuard g;
      if (!g.ok) return;
      if (!s.streaming || s.streamFailed || s.streamPendCount == 0) return;
      buf      = s.streamPendQ[s.streamPendHead];
      len      = s.streamBufLen[buf];
      needOpen = !s.streamOpened;
      strncpy(partPathCopy, s.partPath, sizeof(partPathCopy) - 1);
      partPathCopy[sizeof(partPathCopy) - 1] = '\0';
    }
    // --- flash work, NO slot mutex held (a PENDING buffer is never touched by
    //     espnow_task, so reading streamBuf[buf] here is race-free) ---
    bool wrote = false, openedNow = false;
    {
      FsLockGuard fg("fileslots.stream.write");
      if (needOpen) {
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
      }
    }
    {
      FileSlotsLockGuard g;
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
  if (!g.ok) return STREAM_APPEND_FAIL;
  if (!s.streaming || s.state != FILE_SLOT_RECEIVING || s.streamFailed) return STREAM_APPEND_FAIL;

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
  streamDrainPending(s);
  FileSlotsLockGuard g;                           // re-arm: next committed buffer queues a fresh drain
  if (g.ok) s.streamDrainQueued = false;
}

void fileSlotsStreamAbortJob(void* arg) {        // cmd_exec
  FileTransferSlot* slot = (FileTransferSlot*)arg;
  if (!slot) return;
  FileSlotsLockGuard g;
  if (g.ok) releaseLocked(*impl(slot));           // close + delete .part + free buffers + FREE
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
  FileSlotsLockGuard g;
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

bool fileSlotsStreamFinalizeWrite(FileTransferSlot* slot) {     // cmd_exec
  if (!slot) return false;
  FileTransferSlotImpl& s = *impl(slot);
  streamDrainPending(s);                           // flush any remaining buffers
  bool ok;
  int  idx;
  {
    FileSlotsLockGuard g;
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
uint32_t fileSlotsGetMsgId(const FileTransferSlot* slot) {
  return slot ? implC(slot)->msgId : 0;
}

void fileSlotsRelease(FileTransferSlot* slot) {
  if (!slot) return;
  FileSlotsLockGuard g;
  if (!g.ok) return;
  releaseLocked(*impl(slot));
}

uint8_t fileSlotsTimeoutSweep(uint32_t nowMs, FileSlotExpiry* out, uint8_t outCap) {
  if (!gFileSlots) return 0;
  FileSlotsLockGuard g;
  if (!g.ok) return 0;
  uint8_t expired = 0;
  for (uint8_t i = 0; i < kFileSlots; i++) {
    FileTransferSlotImpl& s = gFileSlots[i];
    if (s.state != FILE_SLOT_RECEIVING) continue;  // FREE, or COMPLETING (already tearing down)
    if ((nowMs - s.lastFrameMs) < kFileSlotTimeoutMs) continue;
    WARN_ESPNOWF("[FileSlots] slot %u TIMEOUT: '%s' from %02X:%02X:%02X:%02X:%02X:%02X "
                 "(msgId=%lu, %u/%u chunks, no frame in %ums)",
                 (unsigned)i, s.filename,
                 s.peerMac[0], s.peerMac[1], s.peerMac[2], s.peerMac[3], s.peerMac[4], s.peerMac[5],
                 (unsigned long)s.msgId,
                 (unsigned)s.receivedChunks, (unsigned)s.totalChunks,
                 (unsigned)(nowMs - s.lastFrameMs));
    // Record (peer,msgId) so the caller can FILE_CANCEL(TIMEOUT) the sender
    // before we wipe the slot.
    if (out && expired < outCap) {
      memcpy(out[expired].peerMac, s.peerMac, 6);
      out[expired].msgId = s.msgId;
    }
    // Streaming slots own a .part handle + buffers that cmd_exec may be mid-write
    // on — defer their teardown to cmd_exec (close/delete/free there). RAM slots
    // have no such handoff; release inline.
    if (s.streaming) fileSlotsStreamFailLocked(s);
    else             releaseLocked(s);
    expired++;
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
  out->msgId          = s.msgId;
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
