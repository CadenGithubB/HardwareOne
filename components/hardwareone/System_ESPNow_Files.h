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
// - FILE_CANCEL (opcode 115) is live: the receiver sends it on timeout, gap,
//   write failure, CRC mismatch, and FILE_START rejection, and the SENDER now
//   aborts its chunk loop when a cancel matches its in-flight (peer, msgId)
//   send. FILE_PROGRESS (114) remains a reserved follow-up.
//
// - Boot-time cleanup: deletes /espnow/received/.part-* on init (orphaned RAM-path
//   staging temps AND streaming .part files from a transfer that died mid-flight).
//
// - Slot-table mutex: FILE_START / FILE_DATA / FILE_END all run INLINE on espnow_task
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
// (PATH_BUSY was removed in the integrity pass: a same-peer same-name FILE_START
// now supersedes the stale transfer instead of being rejected against it, and
// different peers never conflicted in the first place — destinations are
// per-peer directories.)
constexpr const char* kFileSlotErrBusy     = "BUSY";       // all slots in use
constexpr const char* kFileSlotErrBadArgs  = "BAD_ARGS";   // size/chunk params invalid
constexpr const char* kFileSlotErrAlloc    = "ALLOC";      // PSRAM alloc failed
constexpr const char* kFileSlotErrTooBig   = "TOO_BIG";    // exceeds per-file budget

// Per-file PSRAM cap for the whole-file-in-PSRAM receive path. 128 KB; with 4 slots
// that's 512 KB peak PSRAM — fine on an 8 MB part.
//
// This is a ROUTING boundary, not a transfer ceiling: files over it are not rejected,
// they stream to flash instead (see kFileSlotMaxStreamSize below, which IS the hard
// ceiling and is what the sender pre-checks in cmd_espnow_sendfile). So raising this
// does not admit bigger transfers — it only moves more files onto the path that costs
// fileSize bytes of PSRAM per slot. 4 slots × the new value is the peak you're buying.
// The direction that helps is DOWN.
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

// One-time allocation of the gFileSlots table in PSRAM + the slot-table mutex.
// Idempotent. Returns false on allocation failure (caller should disable
// file-transfer functionality).
bool fileSlotsInit();

// Boot-time cleanup. Walks /espnow/received and deletes files whose basename
// starts with ".part-" — staging temps left over from a previous boot that
// crashed mid-transfer (both the RAM path's ".part-<mac>-<hash>-<msgId>" and
// streaming's ".part-strm-<mac>-<hash>-<slot>"; also sweeps the older
// msgId-keyed names from
// pre-integrity-pass firmware, since the prefix match doesn't care about the
// suffix). Idempotent; safe to call before any transfer activity.
void fileSlotsBootCleanup();

// Allocate a slot for a new incoming FILE_START. Returns nullptr if no
// slot is available; *errOut (if non-null) is set to a static reason string
// (one of the kFileSlotErr* constants).
//
// Conflict handling (integrity pass): a FILE_START from the SAME peer whose
// msgId OR filename-hash matches an active slot supersedes that slot — the
// old transfer is dead (duplicate START of this attempt, a sender-side retry
// with a fresh msgId, or a hash collision; all three mean the old stream will
// never complete). RAM slots are released inline; streaming slots are failed
// via the cmd_exec deferral (their .part/buffers may be mid-write) and the new
// transfer takes a DIFFERENT free slot. Different peers never conflict — the
// destination inbox is per-peer.
//
// On success the slot transitions to RECEIVING and is fully populated —
// `filename` and the rest of the metadata are copied in here, so the caller
// (the v4h_file_start handler) has nothing to bind before FILE_DATA arrives.
//
// What gets allocated depends on the size routing (kFileSlotMaxFileSize):
// a RAM-path slot gets its whole-file PSRAM data buffer + chunk bitmap; a
// streaming slot gets two PSRAM half-buffers instead and leaves dataBuffer
// null — its .part is opened later, lazily, on cmd_exec.
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
// RAM-path slots ONLY — a streaming slot returns false here; those go through
// fileSlotsStreamAppend. Bounds-checks against the slot's fileSize. Updates
// receivedBytes / receivedChunks / chunkMap.
//
// Returns false on bounds violation (or a streaming/non-RECEIVING slot). A
// duplicate chunk returns TRUE, not false: the data is re-written unconditionally
// so a retry can repair a corrupted earlier frame, and only the counters are
// suppressed on the second arrival.
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
  STREAM_APPEND_FINALIZING     = 4,  // slot left RECEIVING for a SUCCESSFUL finalize (FILE_END already processed); a late/duplicate chunk — caller must NOT cancel or log, the transfer is fine
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
// Running esp_crc32_le over the chunks accepted so far (streaming slots only —
// RAM slots compute one-shot over the whole buffer at FILE_END instead, since
// their chunks can arrive out of order). Final once fileSlotsIsComplete().
uint32_t       fileSlotsGetReceivedCrc(const FileTransferSlot* slot);
// True while the slot is in RECEIVING. v4h_file_end uses this to drop a
// duplicated FILE_END for a streaming slot already handed to its cmd_exec
// finalize/abort job (fileSlotsFindByMsg matches COMPLETING slots too, and
// re-entering the teardown path there would emit a spurious FILE_CANCEL for a
// transfer that is actually succeeding).
bool           fileSlotsIsReceiving(const FileTransferSlot* slot);

// Release a slot — free PSRAM buffer, return to FREE state. Idempotent.
// STREAMING slots: only call this from cmd_exec_task (their .part handle and
// buffers may be mid-write there). From espnow_task use
// fileSlotsMarkTeardownPending instead.
void fileSlotsRelease(FileTransferSlot* slot);

// Park a streaming slot in COMPLETING with its teardown owed, for when the
// cmd_exec abort job could not be queued (queue full). The timeout sweep
// retries the submit each tick until it lands. Safe to call from espnow_task —
// it frees nothing itself, which is exactly the point.
void fileSlotsMarkTeardownPending(FileTransferSlot* slot);

// Identifies an expired transfer so the caller can notify its sender via
// FILE_CANCEL. Filled by fileSlotsTimeoutSweep when an `out` array is passed.
struct FileSlotExpiry {
  uint8_t  peerMac[6];
  uint32_t msgId;
};

// Periodic stale-slot sweep. Called from the espnow heartbeat tick.
// Slots with no frame in kFileSlotTimeoutMs are released. Returns the number
// of expired (peerMac,msgId) pairs actually RECORDED into `out` (never more
// than outCap — expired slots beyond the cap are still torn down, just not
// reported), so the caller can send each recorded sender a
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
