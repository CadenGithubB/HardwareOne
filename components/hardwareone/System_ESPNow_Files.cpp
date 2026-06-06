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

namespace {

constexpr uint8_t kFileSlots = 4;

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

  uint8_t* dataBuffer;        // PSRAM, totalSize bytes (min 1)
  uint32_t dataBufferLen;     // capacity (== totalSize, capped)
  uint8_t* chunkMap;
  uint16_t chunkMapBytes;
};

FileTransferSlotImpl* gFileSlots = nullptr;
SemaphoreHandle_t      gFileSlotsMutex = nullptr;

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
  if (fileSize > kFileSlotMaxFileSize) {
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
  s.chunkMap = (uint8_t*)heap_caps_malloc(s.chunkMapBytes, MALLOC_CAP_8BIT);
  if (!s.chunkMap) {
    ERROR_ESPNOWF("[FileSlots] chunkMap alloc failed (%u bytes)", (unsigned)s.chunkMapBytes);
    heap_caps_free(s.dataBuffer);
    s.dataBuffer = nullptr;
    setErr(kFileSlotErrAlloc);
    return nullptr;
  }
  memset(s.chunkMap, 0, s.chunkMapBytes);

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
  if (s.totalSize == 0) return true;
  return (s.receivedChunks == s.totalChunks) && (s.receivedBytes == s.totalSize);
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
uint32_t fileSlotsGetMsgId(const FileTransferSlot* slot) {
  return slot ? implC(slot)->msgId : 0;
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
    if (s.state == FILE_SLOT_FREE) continue;
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
    releaseLocked(s);
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
