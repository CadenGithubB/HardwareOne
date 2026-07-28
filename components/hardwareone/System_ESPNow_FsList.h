// System_ESPNow_FsList.h — bonded-peer remote directory listing
//
// Why this module exists
//   The OLED file browser wants to show a bonded peer's filesystem next to
//   the local one. Until this module existed, the only option was scraping
//   the text output of `remote:files /path` via the bonded CLI — brittle
//   format, no permission bits, no pagination, no identity story.
//
//   This module owns the protocol layer for structured directory listings:
//
//     CLIENT                                    PEER
//       │                                         │
//       │── fsListSendRequest(peerMac, path) ────►│
//       │            (V4_TYPE_FS_LIST_REQ)        │
//       │                                         ├─ defers to cmd_exec_task
//       │                                         │  reads VFS, builds reply
//       │◄──── V4_TYPE_FS_LIST_REPLY ─────────────┤
//       │   (header + N V4PayloadFsEntry)         │
//       │                                         │
//      callback fires with parsed entries
//
//   Both opcodes ride the existing bonded encrypted session
//   (REQ_PAIRED + REQ_BOND_MODE + REQ_SESSION_ENC) so they get the same
//   confidentiality + authentication as SETTINGS_REQ / MANIFEST_REQ.
//
// Identity model — device trust
//   The peer that receives an FS_LIST_REQ checks "is this from my bonded
//   peer?" via the standard pairing gate. If yes, it reads the directory
//   under SYSTEM identity and reports the perms its bonded-peer ACL would
//   give the caller. Per-user identity propagation is deliberately out of
//   scope for this iteration — same trust posture as the existing settings
//   sync over ESP-NOW. A future enhancement can add a per-user identity
//   token in the request payload's reserved bytes.
//
// Concurrency
//   Sender: pending-request table holds up to FS_LIST_MAX_PENDING outstanding
//   requests with timeouts. Each entry is { reqId, peerMac, callback,
//   deadlineMs }. Single-call cancel API lets the OLED ditch a request when
//   the user navigates away.
//
//   Receiver: single deferred-work slot. If a second request arrives while
//   one is in flight, the new one gets a FS_LIST_STATUS_TOO_BUSY reply
//   immediately (without queueing). Caller retries with backoff. This is
//   intentional: a directory listing read at SYSTEM identity is fast (a
//   few ms for typical VFS dirs) so head-of-line blocking is acceptable
//   in exchange for keeping deferred state trivial.
//
// Memory
//   Sender pending table: FS_LIST_MAX_PENDING × 20 B = 80 B DRAM (static).
//   Receiver deferred slot: ~150 B DRAM (static).
//   Reply send buffer: 2572 B (140 B header + 32 × 76 B entries), ps_alloc'd in
//   PSRAM by processListDeferred and freed after the send — deliberately NOT on
//   the stack. cmd_exec has 8 KB total, and LittleFS directory iteration wants a
//   lot of it; see the rationale at the allocation site.

#ifndef SYSTEM_ESPNOW_FSLIST_H
#define SYSTEM_ESPNOW_FSLIST_H

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <stdint.h>
#include <stddef.h>
#include "System_ESPNow_Wire.h"   // V4Payload structs, FS_LIST_ENTRIES_PER_REPLY

// Tunables — keep small. Sender side typically has at most 1-2 outstanding
// requests at a time (one per active surface). 4 covers a future "OLED is
// browsing peer's dir, web is also looking" scenario.
#define FS_LIST_MAX_PENDING        4
#define FS_LIST_REQUEST_TIMEOUT_MS 5000

// Reply callback. `header` and `entries` point into stack-allocated buffers
// owned by the receive path — copy what you need before returning. `entries`
// may be nullptr if header.entryCount == 0 (empty dir or error response).
// `peerMac` is the responding peer's MAC (matches the request target unless
// something very weird happened).
typedef void (*FsListReplyCallback)(const uint8_t peerMac[6],
                                    const V4PayloadFsListReplyHeader* header,
                                    const V4PayloadFsEntry* entries);

// Stat reply callback. `reply` is owned by the receive path — copy before
// returning. Replaces the prior fsusage-CLI-scrape pattern.
typedef void (*FsStatReplyCallback)(const uint8_t peerMac[6],
                                    const V4PayloadFsStatReply* reply);

// Get-file ACK callback. Fires when the peer responds with FS_GET_ACK
// (synchronous accept/reject). On status == FS_LIST_STATUS_OK the peer is
// initiating a FILE_START/DATA/END transfer back; the caller's existing
// inbound file-receive path will land the file on local VFS. On any other
// status, no transfer follows. `ack->fileSize` is informational.
typedef void (*FsGetAckCallback)(const uint8_t peerMac[6],
                                 const V4PayloadFsGetAck* ack);

// ============================================================================
// Lifecycle
// ============================================================================

// Initialize internal state (mutex, table zeroing). Safe to call multiple
// times; idempotent. Called from initEspNow().
void fsListInit();

// ============================================================================
// Sender (client) API
// ============================================================================

// Send an FS_LIST_REQ to `peerMac` for `path`. `startIndex` enables
// pagination (use 0 for the first batch; pass the previous reply's
// nextStartIndex for subsequent batches). Returns the assigned reqId
// (> 0) on success, or 0 if the request couldn't be queued (no slot,
// bonded peer not reachable, etc.).
//
// Callback fires exactly once when the reply arrives OR the request times
// out. On timeout, callback is invoked with a synthesized header where
// status = FS_LIST_STATUS_IO_ERROR and entryCount = 0.
//
// Caller is responsible for cancelling the request via fsListCancel() if
// it loses interest (e.g. user navigates away from the file browser).
uint32_t fsListSendRequest(const uint8_t peerMac[6],
                           const char* path,
                           uint16_t startIndex,
                           FsListReplyCallback callback);

// Cancel a pending request by reqId. After this returns, the callback for
// that request will NOT fire (if it hadn't already). Safe to call with an
// already-resolved or unknown reqId — no-op. Works for list / stat / get.
void fsListCancel(uint32_t reqId);

// Send FS_STAT_REQ to `peerMac`. Returns reqId on success, 0 on failure.
// Callback gets the typed reply (or a synthesized error on timeout).
uint32_t fsStatSendRequest(const uint8_t peerMac[6],
                           const char* path,
                           FsStatReplyCallback callback);

// Send FS_GET_REQ to `peerMac`. The callback fires when the peer ACKs
// (synchronous, before any FILE_* transfer). If ack->status == OK the
// peer is now sending the file via FILE_START/DATA/END — caller must
// observe the inbound file-receive path (existing pattern) for the
// actual content arrival.
uint32_t fsGetSendRequest(const uint8_t peerMac[6],
                          const char* path,
                          FsGetAckCallback callback);

// ============================================================================
// RX bridge — called from System_ESPNow.cpp's v4 dispatch handlers
// ============================================================================
// V4RxCtx now lives in System_ESPNow_RxCtx.h, but this module deliberately
// keeps raw-buffer APIs and lets System_ESPNow.cpp's thin v4h_ stubs unpack
// the fields — the protocol module stays standalone, no header dependency.

// Record an incoming FS_LIST_REQ into the deferred slot. Called from
// espnow_task's RX-handler dispatch (6656 B stack, and its job is to drain the
// RX ring fast) — minimal work only: validates the payload, copies fields into
// static storage, returns. Actual VFS read + reply send is handed to
// cmd_exec_task, which is where the heavy work belongs.
void fsListOnRequestReceived(const uint8_t srcMac[6],
                             const uint8_t* payload, uint16_t payloadLen);

// Match an incoming FS_LIST_REPLY against the pending-request table and
// fire the registered callback. Called from espnow_task's RX-handler dispatch —
// the callback runs on espnow_task too, so its body must be minimal (copy data,
// set a flag) or it stalls the RX drain.
void fsListOnReplyReceived(const uint8_t srcMac[6],
                           const uint8_t* payload, uint16_t payloadLen);

// FS_STAT_REQ + FS_STAT_REPLY mirror the list pattern: REQ defers to cmd_exec,
// REPLY matches reqId and fires its callback.
void fsStatOnRequestReceived(const uint8_t srcMac[6],
                             const uint8_t* payload, uint16_t payloadLen);
void fsStatOnReplyReceived(const uint8_t srcMac[6],
                           const uint8_t* payload, uint16_t payloadLen);

// FS_GET_REQ defers to cmd_exec (peer kicks off sendFileToMac there). FS_GET_ACK
// is the synchronous reply BEFORE the FILE_* transfer.
void fsGetOnRequestReceived(const uint8_t srcMac[6],
                            const uint8_t* payload, uint16_t payloadLen);
void fsGetOnAckReceived(const uint8_t srcMac[6],
                        const uint8_t* payload, uint16_t payloadLen);

// Per-tick driver — call from the main ESP-NOW task tick. SENDER-side only:
// sweeps the pending-request table for timeouts and fires callbacks with a
// synthesized error status for any that expired.
//
// Receiver-side reply building is NOT here. It runs on cmd_exec_task, submitted
// the moment the request is captured — an FS_LIST reply cannot be built on
// espnow_task without overflowing it.
void fsListTick();

#endif // ENABLE_ESPNOW
#endif // SYSTEM_ESPNOW_FSLIST_H
