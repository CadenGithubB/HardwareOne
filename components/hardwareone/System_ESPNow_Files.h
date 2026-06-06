#ifndef SYSTEM_ESPNOW_FILES_H
#define SYSTEM_ESPNOW_FILES_H

// ============================================================================
// ESP-NOW V4 Phase 4 — concurrent file-transfer slots.
//
// Replaces the original single-flight `gActiveFileTransfer*` + global
// `gFileTransferLocked` flag with a 4-slot table. Each slot tracks an
// independent in-progress transfer keyed by (peerMac, msgId). Multiple
// peers can upload simultaneously; same-destination-path conflicts are
// rejected; timed-out slots get reclaimed by a periodic sweep.
//
// Scope decisions:
//
// - In-RAM (PSRAM) accumulation, single write at FILE_END. The plan called
//   for a dedicated file_writer_task that drains 4 KB buffers to disk as
//   they fill. We're shipping the simpler "accumulate full file in PSRAM
//   then write once" model for now because the per-file cap (64 KB) × 4
//   slots = 256 KB worst case, which is fine on 2 MB PSRAM. The writer-
//   task pattern is a follow-up if file sizes ever grow past the budget.
//
// - No new opcodes (FILE_PROGRESS / FILE_CANCEL). Current code uses silent
//   timeouts and the existing v4_send_ack on FILE_END. Adding explicit
//   progress / cancel opcodes is a follow-up.
//
// - Boot-time cleanup: deletes /tmp/.transfer-*.part on init. Necessary
//   for the writer-task pattern (deferred) but harmless either way.
//
// - Per-slot mutex: the receive callback path is single-threaded
//   (espnow_task), so cross-task races within FILE_DATA aren't possible.
//   But FILE_END can run on cmd_exec_task in the deferred-work pattern,
//   and the timeout sweep runs on the heartbeat task, so the slot table
//   itself gets a single mutex protecting allocation / find / release.
// ============================================================================

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <stdint.h>
#include <stddef.h>

// Opaque to callers — the implementation owns the layout.
struct FileTransferSlot;

// Slot states, exposed for diagnostics CLI.
enum FileSlotState : uint8_t {
  FILE_SLOT_FREE       = 0,
  FILE_SLOT_RECEIVING  = 1,
  FILE_SLOT_COMPLETING = 2,  // FILE_END arrived, processing on cmd_exec
  FILE_SLOT_FAILED     = 3,
};

// Reason codes for fileSlotsAllocate failure (set in *errOut).
constexpr const char* kFileSlotErrBusy     = "BUSY";       // all slots in use
constexpr const char* kFileSlotErrPathBusy = "PATH_BUSY";  // same dest path in use
constexpr const char* kFileSlotErrBadArgs  = "BAD_ARGS";   // size/chunk params invalid
constexpr const char* kFileSlotErrAlloc    = "ALLOC";      // PSRAM alloc failed
constexpr const char* kFileSlotErrTooBig   = "TOO_BIG";    // exceeds per-file budget

// Per-file PSRAM cap. 64 KB matches the pre-Phase-4 limit; 4 slots × 64 KB
// = 256 KB peak. Adjust if files grow.
constexpr uint32_t kFileSlotMaxFileSize = 65536;

// Stale-slot reclaim threshold. No frame received in this window → drop.
constexpr uint32_t kFileSlotTimeoutMs = 30000;

// One-time allocation of the gFileSlots table in PSRAM + per-slot mutex.
// Idempotent. Returns false on allocation failure (caller should disable
// file-transfer functionality).
bool fileSlotsInit();

// Boot-time cleanup. Walks /tmp and deletes .transfer-*.part files left
// over from a previous boot that crashed mid-transfer. Idempotent; safe to
// call before any transfer activity.
void fileSlotsBootCleanup();

// Allocate a slot for a new incoming FILE_START. Returns nullptr if no
// slot is available or a same-destination-path conflict is detected; in
// either case *errOut (if non-null) is set to a static reason string
// (one of the kFileSlotErr* constants).
//
// On success, the slot's PSRAM data buffer is allocated and the slot
// transitions to RECEIVING. Caller (the v4h_file_start handler) is
// responsible for writing the destination path / filename metadata into
// the slot via fileSlotsBindMetadata before any FILE_DATA arrives.
FileTransferSlot* fileSlotsAllocate(const uint8_t peerMac[6],
                                     uint32_t msgId,
                                     const char* filename,
                                     uint32_t fileSize,
                                     uint16_t chunkCount,
                                     uint16_t chunkSize,
                                     const char** errOut);

// Lookup the slot that owns an incoming chunk by (peerMac, msgId).
// Returns nullptr if no slot matches.
FileTransferSlot* fileSlotsFindByMsg(const uint8_t peerMac[6], uint32_t msgId);

// Write a chunk into the slot's buffer at offset = chunkIdx * chunkSize.
// Bounds-checks against the slot's fileSize. Updates receivedBytes /
// receivedChunks / chunkMap. Returns false on bounds violation or if the
// chunk is a duplicate (already in the map).
bool fileSlotsWriteChunk(FileTransferSlot* slot,
                          uint16_t chunkIdx,
                          const uint8_t* data,
                          uint16_t dataLen);

// True when every expected chunk has arrived (or fileSize == 0).
bool fileSlotsIsComplete(const FileTransferSlot* slot);

// Accessors for the FILE_END processor.
const uint8_t* fileSlotsGetBuffer(const FileTransferSlot* slot);
uint32_t       fileSlotsGetReceivedBytes(const FileTransferSlot* slot);
const char*    fileSlotsGetFilename(const FileTransferSlot* slot);
const uint8_t* fileSlotsGetSenderMac(const FileTransferSlot* slot);
uint32_t       fileSlotsGetMsgId(const FileTransferSlot* slot);
uint16_t       fileSlotsGetReceivedChunks(const FileTransferSlot* slot);
uint16_t       fileSlotsGetTotalChunks(const FileTransferSlot* slot);
uint32_t       fileSlotsGetTotalSize(const FileTransferSlot* slot);

// Release a slot — free PSRAM buffer, return to FREE state. Idempotent.
void fileSlotsRelease(FileTransferSlot* slot);

// Identifies an expired transfer so the caller can notify its sender via
// FILE_CANCEL. Filled by fileSlotsTimeoutSweep when an `out` array is passed.
struct FileSlotExpiry {
  uint8_t  peerMac[6];
  uint32_t msgId;
};

// Periodic stale-slot sweep. Called from the espnow heartbeat tick.
// Slots with no frame in kFileSlotTimeoutMs are released. Returns the
// count of expired slots. If `out` is non-null, up to `outCap` expired
// (peerMac,msgId) pairs are recorded so the caller can send each sender a
// FILE_CANCEL(TIMEOUT). Pass nullptr/0 to skip collection.
uint8_t fileSlotsTimeoutSweep(uint32_t nowMs, FileSlotExpiry* out = nullptr, uint8_t outCap = 0);

// ---- Diagnostics ------------------------------------------------------------

uint8_t fileSlotsSlotCount();
uint8_t fileSlotsActiveCount();

struct FileTransferSlotInfo {
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
};

// Snapshot a slot's metadata for the `espnowfiles` CLI. Returns false if
// the slot index is out of range or the slot is FREE.
bool fileSlotsSnapshot(uint8_t slotIdx, FileTransferSlotInfo* out);

#endif  // ENABLE_ESPNOW

#endif  // SYSTEM_ESPNOW_FILES_H
