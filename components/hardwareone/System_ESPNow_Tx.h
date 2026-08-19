#ifndef SYSTEM_ESPNOW_TX_H
#define SYSTEM_ESPNOW_TX_H

#include <stdint.h>
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============================================================================
// Single-sender ESP-NOW TX task
// ============================================================================
//
// Why this exists
// ---------------
// Before: every task that wanted to send an ESP-NOW frame had to be sized for
// the *full* AEAD + frame + capture + Wi-Fi-ISR depth (~4–5 KB local on top of
// the caller's own stack). When AEAD was added in Phase 3.5, only espnow_task
// and cmd_exec_task were re-sized. sensor_bcast (sized for plaintext mesh
// broadcast) overflowed the moment the first bond-encrypted sensor stream
// transmit took the deep path — see the SENSOR_BCAST_STACK_WORDS history in
// System_TaskUtils.h.
//
// After (this module): sends CAN route through one dispatcher task with a
// generously-sized stack. A producer builds a tiny Job descriptor, copies the
// payload into a heap buffer, and enqueues; it never touches the AEAD seal, the
// frame buffer, or esp_now_send, so its stack shrinks back to "what does my own
// work need."
//
// HOW FAR THIS ACTUALLY GOT — read before sizing a task's stack off this file.
// The migration is PARTIAL. Only callers that go through submit/submitSync (the
// bond encrypted-send path, cmd_exec deferred handlers, RX-side responses) get
// the benefit. Notable holdouts that still build and send frames on their OWN
// stack:
//   * (RETIRED HOLDOUT) sensor_bcast's mesh path was migrated by the D2 secure
//     fetcher: sensorBroadcasterTask → transmitSensorData → v4_send_sensor_envelope,
//     which stacks only a uint8_t buffer[256] and hands off via sendAead — the
//     AEAD seal, frame build, and capture now run on the dispatcher task.
//   * Mesh heartbeats — call v4_send_payload_smart / v4_broadcast_category
//     directly.
// So "every sender is a Job" is the design intent, not the current state; the
// stack-sizing hazard this module was built to kill is still live on the
// heartbeat path.
//
// Knock-on benefits (CONDITIONAL — they arrive only once the holdouts above are
// migrated; none of them are banked yet)
// -----------------
//   * EspNowTxGuard mutex would become redundant (single sender ⇒ no race on
//     SessionState::txSeqNext + nonce build). It is still very much alive and
//     load-bearing — 9 sites, including the session TX critical section at
//     System_ESPNow_Sessions.cpp:295 — precisely because sends still originate
//     from multiple tasks. The DEADLOCK RULE on submitSync() below depends on
//     this lock existing; do not treat it as vestigial.
//   * frame[250] in v4_send_session_wrapped + line[700] in captureEspNowFrame
//     become hoist-to-static candidates only when every sender is single-
//     threaded by construction. Not today.
//   * Adding a new sender = enqueue a Job. Never again "did anyone forget to
//     re-size this task's stack after AEAD landed?"
//
// What it does NOT do
// -------------------
//   * RX path is unchanged. Handlers still dispatch on espnow_task; heavy
//     work still defers to cmd_exec_task via submitDeferredToCmdExec. The
//     ONLY change is where the *send half* lives.
//   * V4 ACK tracking (sendStatus*) is unchanged. submit() is fire-and-
//     forget at the wire level; per-message delivery confirmation still flows
//     through the existing ACK state machine.
//   * Fragmentation (v4_send_encrypted_chunked) stays at the caller for now.
//     The per-fragment ACK-wait loop is hard to model as a single job; defer
//     to a later phase once Phase 2 (sensor_bcast) validates the design.
//
// JOB KINDS
// ---------
//   JOB_AEAD_SMART   — funneled into v4_send_payload_smart. Auto-resolves
//                      peer identity + session, AEAD-seals, captures, sends.
//                      This covers ~95% of senders, including bondSendEncrypted
//                      (caller pre-packs the bond token into the plaintext
//                      payload; kind stays AEAD_SMART — the bond token is not
//                      a separate layer to the wire format).
//
//   JOB_RAW          — v4_send_frame raw plaintext (broadcast, KEY_EX). Live;
//                      dispatched in runJob alongside JOB_AEAD_SMART.
//
// Future kinds (not implemented — runJob has no case for these):
//   JOB_FRAG_ACK     — tiny plaintext frag-ack (high priority).
//   JOB_AEAD_CHUNKED — multi-frag AEAD with internal ACK-wait loop.
//
// COMPLETION SEMANTICS
// --------------------
// submit() is fire-and-forget. The bool it returns reflects only whether the
// job was queued, not whether the wire send eventually succeeded. Wire-level
// outcome is tracked in espnowtx::Stats (sent / failed) for diagnostics; per-
// message delivery confirmation is the caller's V4 ACK loop, same as before.
//
// Callers that genuinely need synchronous "did this leave the box?" semantics
// have submitSync() (and the sendAeadSync() helper) below — the dispatcher wakes
// the caller through a per-job completion object. CMD_RESP already sends this
// way. Note the constraint that comes with it: sync callers must not be on
// espnow_task, since blocking there stalls the RX drain.
// ============================================================================

namespace espnowtx {

struct SyncCompletion;

enum JobKind : uint8_t {
  JOB_AEAD_SMART = 0,    // -> v4_send_payload_smart  (encrypted, auto-session)
  JOB_RAW        = 1,    // -> v4_send_frame          (plaintext single frame:
                         //    KEY_EX handshake, plaintext broadcasts). FF×6 dst ok.
};

struct Job {
  JobKind  kind;
  uint8_t  peerMac[6];   // unicast dst; FF×6 (broadcast) only for JOB_RAW
  uint8_t  type;         // V4 opcode (ESPNOW_V4_TYPE_*)
  uint16_t flags;        // V4 flags (caller composes)
  uint32_t msgId;        // caller-generated message id (for ACK matching)
  uint8_t  ttl;          // hop budget; typically 1
  uint16_t payloadLen;   // bytes in *payload*; may be 0
  uint8_t* payload;      // heap-allocated (prefer PSRAM); tx task takes
                         // ownership and free()s after send. May be nullptr
                         // if payloadLen == 0.
  SyncCompletion* completion; // INTERNAL — owned jointly by submitSync() and
                         // the dispatcher. Leave nullptr for fire-and-forget
                         // submit(). Zero-initialize the Job (`Job j = {};`).
};

// One-time init. Creates the job queue and spawns espnow_tx task. Call
// AFTER ESP-NOW is initialized but BEFORE any task that might submit().
// Returns true if already initialized.
bool init();

// Stop accepting jobs, fail and retire every queued job, join the dispatcher,
// and release its queue/task/completion resources. A job already executing is
// allowed to finish; stop() waits for it. Accepted synchronous jobs are woken
// with failure. Returns false if the dispatcher did not quiesce within the
// timeout; in that case resources remain intact and a later stop() can retry.
// After a successful stop(), init() may be called again.
bool stop(uint32_t timeoutMs);

// Enqueue a job. Non-blocking: returns false immediately if the queue is
// full. On false, the CALLER retains ownership of job.payload (must free
// it). On true, the tx task will free it after the send.
//
// Safe to call from any task except an ISR.
bool submit(const Job& job);

// Synchronous send: enqueue the job and BLOCK the calling task until the
// dispatcher has actually performed the wire send, then return its result
// (true = esp_now_send/v4_send_* returned OK, false = failed/timed out).
//
// Use this for callers that branch on the send outcome (CLI cmd_resp, file
// chunks). It also provides natural backpressure: if the queue is full the
// caller blocks here instead of dropping, so a burst producer throttles to
// the dispatcher's drain rate.
//
// Mechanism: the job carries a slot from a lifecycle-owned completion pool.
// Each slot has a private EventGroup bit and explicit caller/dispatcher
// ownership. A timeout marks the caller gone; the dispatcher retires the slot
// later, so completion never touches a task handle or satisfies a future call.
//
// DEADLOCK RULE: never call submitSync() while holding the session seal /
// rekey lock — the dispatcher needs that lock to send. Handshake/crypto sends
// must use fire-and-forget submit() instead.
//
// OWNERSHIP: submitSync takes ownership of job.payload and frees it on EVERY
// path (success, enqueue failure, or wait timeout). The caller must not free it.
//
// CALLER LIFETIME: the calling task must remain alive until submitSync returns.
// A task that is externally force-deleted while blocked cannot release its
// completion slot. The dispatcher will retain the lifecycle safely (no stale
// task notification or UAF), and stop() will fail rather than free that slot.
//
// Safe to call from any task except an ISR. Must NOT be called from the
// espnow_tx dispatcher task itself (would self-deadlock).
bool submitSync(Job& job, uint32_t timeoutMs);

// ----------------------------------------------------------------------------
// Convenience helpers — build a JOB_AEAD_SMART Job from raw bytes and submit.
// Both ps_alloc the payload buffer internally; the clerk takes ownership and
// frees per the same contract as the underlying submit/submitSync. Saves each
// caller the ps_alloc + Job-fill + free-on-failure boilerplate (~10 lines per
// site → 1 line per site).
// ----------------------------------------------------------------------------

// Fire-and-forget AEAD send. Returns true iff the queue accepted the job
// (queue-full and ps_alloc-fail both return false; on false the helper frees
// any buffer it allocated, no leak).
//
// SAFE FROM ANY TASK including espnow_task. RX-side handlers MUST use this
// variant rather than sendAeadSync — sendAeadSync would block espnow_task and
// stall the RX drainer, violating the architecture invariant documented at
// the top of processMeshHeartbeats.
bool sendAead(const uint8_t mac[6], uint8_t type, uint16_t flags,
              uint32_t msgId, const uint8_t* payload, uint16_t payloadLen,
              uint8_t ttl);

// Blocking AEAD send. Returns true iff the dispatcher reported the wire send
// succeeded within timeoutMs. Worst-case blocking is ~2 × timeoutMs (queue
// enqueue + completion wait both honor the timeout).
//
// SAFE FROM cmd_exec and other non-RX tasks. MUST NOT be called from
// espnow_task (stalls RX drainer) or from the espnow_tx dispatcher itself
// (self-deadlock; submitSync's guard rejects this).
bool sendAeadSync(const uint8_t mac[6], uint8_t type, uint16_t flags,
                  uint32_t msgId, const uint8_t* payload, uint16_t payloadLen,
                  uint8_t ttl, uint32_t timeoutMs);

// Diagnostic counters. Logged by the tx task every 10s; CLI / other code can
// also read them via getStats().
struct Stats {
  uint32_t submitted;     // submit() called this many times
  uint32_t sent;          // wire send returned OK
  uint32_t failed;        // wire send returned error (v4_send_* false)
  uint32_t dropped;       // submit() couldn't queue (queue full)
  uint32_t queueDepthHwm; // max queue depth observed at submit() time
};
void getStats(Stats* out);

// Atomically sample the task handle and stack high-water mark while holding the
// stable lifecycle-owner mutex, so stop() cannot delete the task between handle
// lookup and the FreeRTOS query. The returned handle is an opaque snapshot key
// only; callers must not dereference it or issue later task API calls with it.
bool getTaskStackSnapshot(TaskHandle_t* outHandle, UBaseType_t* outWatermark);

}  // namespace espnowtx

#endif  // SYSTEM_ESPNOW_TX_H
