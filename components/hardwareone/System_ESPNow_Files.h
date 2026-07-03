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
// - HYBRID size-routed receive (Phase A/B). Files <= kFileSlotMaxFileSize (128 KB)
//   are buffered WHOLE in PSRAM and written once at FILE_END — the original,
//   unchanged path; small bond config files (_settings_out.json, automations.json)
//   always land here and keep their in-RAM FILE_END processing. Files LARGER than
//   that STREAM to a .part file on flash, bounded by flash, not PSRAM. espnow_task
//   only memcpy's each chunk into a 4 KB double-buffer; a filled buffer is handed to
//   cmd_exec_task (via submitDeferredToCmdExec), which does the actual flash write —
//   so the RX drain is never blocked on flash (the single-task inline-flush variant
//   dropped chunks at ~87 when an erase stalled RX). This is purely additive:
//   anything that worked at <= 128 KB behaves byte-for-byte as before; only
//   previously-rejected big files take the new path. See
//   docs/ESPNOW_FILE_STREAMING_PHASE_A.md.
//
// - No new opcodes (FILE_PROGRESS / FILE_CANCEL). Current code uses silent
//   timeouts and the existing v4_send_ack on FILE_END. Adding explicit
//   progress / cancel opcodes is a follow-up.
//
// - Boot-time cleanup: deletes /espnow/received/.part-* on init (orphaned RAM-path
//   staging temps AND streaming .part files from a transfer that died mid-flight).
//
// - Per-slot mutex: FILE_START / FILE_DATA / FILE_END all run INLINE on espnow_task
//   (the RX drain), so within a transfer the frames are serialized by that single
//   task. The timeout sweep runs on the heartbeat task and the slot table is shared,
//   so a single mutex protects allocation / find / release. In STREAMING mode the
//   actual .part writes (and all teardown) run on cmd_exec_task, never espnow_task;
//   the mutex guards the buffer/FIFO metadata while the flash write happens outside
//   it, and a streaming slot is only ever freed by its cmd_exec finalize/abort job
//   (RECEIVING → COMPLETING → FREE), so the sweep never closes a handle mid-write.
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

// Per-file PSRAM cap (the whole file is buffered in PSRAM during receive). 128 KB;
// with 4 slots that's 512 KB peak PSRAM — fine on an 8 MB part. The SENDER pre-checks
// this same constant in cmd_espnow_sendfile and refuses an oversize file up front with
// a clear reason, rather than streaming it and letting the receiver reject at FILE_START
// (which isn't signaled back). Raise further only if needed; the real unbounded fix is
// streaming chunks straight to a .part file on flash instead of buffering whole-file.
constexpr uint32_t kFileSlotMaxFileSize = 131072;

// Files LARGER than kFileSlotMaxFileSize are STREAMED chunk-by-chunk straight to
// a .part file on flash (no whole-file PSRAM buffer) — Phase A. Files <= the cap
// still use the original whole-file-in-PSRAM path, UNCHANGED, so the small config
// files the bond layer parses in RAM (_settings_out.json, automations.json, …) are
// never affected (they're always well under 128 KB → always the RAM path).
// Streaming is bounded by free flash; this is a sanity ceiling (and stays under the
// 16-bit chunk-count limit of ~13 MB). Per-file RAM in streaming mode is just two
// 4 KB double-buffers, regardless of file size.
constexpr uint32_t kFileSlotMaxStreamSize = 4u * 1024u * 1024u;  // 4 MB sanity ceiling

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

// True when every expected chunk has arrived (or fileSize == 0). For a streaming
// slot, a gap or write error during receive makes this permanently false.
bool fileSlotsIsComplete(const FileTransferSlot* slot);

// --- Streaming mode (file > kFileSlotMaxFileSize) ---------------------------
// Big files don't buffer whole in PSRAM; they stream to a .part file on flash.
// CRITICAL: the flash writes do NOT run on espnow_task (that stalls RX and drops
// chunks). espnow_task only memcpy's each chunk into a small double-buffer; when a
// buffer fills it's handed to cmd_exec_task (via submitDeferredToCmdExec) which does
// the actual flash write. All teardown (close/rename/delete/free) also runs on
// cmd_exec_task so it never races espnow_task's fills. This mirrors the established
// "defer heavy/FS work off the RX task" pattern used by SESSION_OPEN/CONFIRM/REKEY.
//
// Lifecycle: RECEIVING → (FILE_END or gap/timeout) → COMPLETING (a finalize/abort
// job is queued on cmd_exec) → FREE (the job releases the slot). espnow_task and the
// timeout sweep never free a streaming slot directly.

// True if this slot streams to flash. For these, fileSlotsGetBuffer() returns null.
bool        fileSlotsIsStreaming(const FileTransferSlot* slot);
// The on-flash staging path of a streaming slot (the finalize job renames it).
const char* fileSlotsGetPartPath(const FileTransferSlot* slot);

enum StreamAppendResult : uint8_t {
  STREAM_APPEND_OK   = 0,  // chunk buffered (and a drain job submitted if a buffer filled)
  STREAM_APPEND_DUP  = 1,  // duplicate/old chunk — ignored
  STREAM_APPEND_FAIL = 2,  // gap, backpressure, or error — transfer aborted THIS chunk (cleanup queued); caller should FILE_CANCEL and log once
  STREAM_APPEND_ALREADY_FAILED = 3,  // slot was already aborting from an earlier chunk — caller should FILE_CANCEL but NOT re-log (avoids per-chunk durable-log flood)
};
// Append one in-order chunk to a streaming slot (runs on espnow_task). Self-
// contained: on a full buffer it submits a cmd_exec drain job; on failure it marks
// the slot failed and submits a cmd_exec abort job. Never touches flash itself.
StreamAppendResult fileSlotsStreamAppend(FileTransferSlot* slot, uint16_t chunkIdx,
                                         const uint8_t* data, uint16_t dataLen);
// FILE_END for a streaming slot (espnow_task): commit the last partial buffer and
// mark COMPLETING. Returns true if a finalize job should be submitted; false if the
// slot was already failing/aborting (caller should just FILE_CANCEL instead).
bool        fileSlotsStreamBeginFinalize(FileTransferSlot* slot);
// Abort a streaming transfer (gap/timeout/incomplete): mark failed + queue cleanup
// on cmd_exec. Idempotent. Safe to call from espnow_task or the heartbeat sweep.
void        fileSlotsStreamFail(FileTransferSlot* slot);
// cmd_exec finalize: drain remaining buffers + close the .part. Returns true if the
// whole file was written without error (caller then renames it into place).
bool        fileSlotsStreamFinalizeWrite(FileTransferSlot* slot);
// cmd_exec job entrypoints (submitted via submitDeferredToCmdExec, arg = slot ptr).
void        fileSlotsStreamDrainJob(void* arg);
void        fileSlotsStreamAbortJob(void* arg);

// Accessors for the FILE_END processor.
const uint8_t* fileSlotsGetBuffer(const FileTransferSlot* slot);
uint32_t       fileSlotsGetReceivedBytes(const FileTransferSlot* slot);
const char*    fileSlotsGetFilename(const FileTransferSlot* slot);
const uint8_t* fileSlotsGetSenderMac(const FileTransferSlot* slot);
uint16_t       fileSlotsGetReceivedChunks(const FileTransferSlot* slot);
uint16_t       fileSlotsGetTotalChunks(const FileTransferSlot* slot);
uint32_t       fileSlotsGetTotalSize(const FileTransferSlot* slot);
uint32_t       fileSlotsGetMsgId(const FileTransferSlot* slot);

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
