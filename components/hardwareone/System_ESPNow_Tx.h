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
// After (this module): all sends route through one dispatcher task with a
// generously-sized stack. Producers (sensor_bcast, processMeshHeartbeats,
// cmd_exec deferred handlers, RX-side responses) build a tiny Job descriptor,
// copy the payload into a heap buffer, and enqueue. They never touch the
// AEAD seal, the frame buffer, or esp_now_send themselves. Their stacks
// shrink back to "what does my own work need."
//
// Knock-on benefits
// -----------------
//   * EspNowTxGuard mutex becomes redundant (single sender ⇒ no race on
//     sessionState.nextSendSeq + nonce build). Removed in Phase 5.
//   * frame[218] in v4_send_session_wrapped + line[700] in captureEspNowFrame
//     become hoist-to-static candidates once Phase 5 lands (single-threaded
//     by construction).
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
// Future kinds (not in Phase 1):
//   JOB_RAW          — v4_send_frame raw plaintext (broadcast, KEY_EX).
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
// (CLI cmd_resp, web responses) currently still call v4_send_payload_smart
// directly — those migrate in Phase 3 once we add a sync-completion variant.
// ============================================================================

namespace espnowtx {

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
  TaskHandle_t notifyTask; // INTERNAL — set by submitSync() to the caller's
                         // task handle so the dispatcher can wake it with the
                         // result. Leave nullptr for fire-and-forget submit().
                         // Zero-initialize the Job (`Job j = {};`) and this is
                         // correctly null.
};

// One-time init. Creates the job queue and spawns espnow_tx task. Call
// AFTER ESP-NOW is initialized but BEFORE any task that might submit().
// Returns true if already initialized.
bool init();

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
// Mechanism: the job carries the caller's TaskHandle; after the send the
// dispatcher wakes it via xTaskNotify (result in the notification value).
// No shared heap object, so a timeout cannot cause a use-after-free.
//
// DEADLOCK RULE: never call submitSync() while holding the session seal /
// rekey lock — the dispatcher needs that lock to send. Handshake/crypto sends
// must use fire-and-forget submit() instead.
//
// CAVEAT (one notification slot on this build): the calling task's task-
// notification value must be otherwise unused for the duration of the call.
// Our sync callers (cmd_exec, file-tx) drive their queues via xQueueReceive,
// not notifications, so this holds. submitSync clears any stale notification
// before waiting.
//
// OWNERSHIP: submitSync takes ownership of job.payload and frees it on EVERY
// path (success, enqueue failure, or wait timeout). The caller must not free it.
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

// Optional accessor — currently used only by reportAllTaskStacks() to include
// espnow_tx in the periodic stack-usage report. Returns nullptr until init().
TaskHandle_t getTaskHandle();

}  // namespace espnowtx

#endif  // SYSTEM_ESPNOW_TX_H
