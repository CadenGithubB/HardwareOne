#include "System_ESPNow_Tx.h"

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "System_Debug.h"
#include "System_TaskUtils.h"
#include "System_MemUtil.h"  // ps_alloc / AllocPref for sendAead{,Sync} helpers

// External hooks into the existing send paths. Declared here rather than via
// a header so this TU stays free of the System_ESPNow.cpp giant. Phase 2+
// migrations only need the symbols, not the full ESP-NOW state surface.
extern bool v4_send_payload_smart(const uint8_t* dst, uint8_t type, uint16_t flags,
                                  uint32_t msgId,
                                  const uint8_t* payload, uint16_t payloadLen,
                                  uint8_t ttl);
extern bool v4_send_frame(const uint8_t* dst, uint8_t type, uint16_t flags,
                          uint32_t msgId,
                          const uint8_t* payload, uint16_t payloadLen,
                          uint8_t ttl);

namespace espnowtx {

// Notification values used to wake a submitSync() caller. Non-zero so a stray
// zero-valued notification can't be mistaken for a result.
static constexpr uint32_t TX_NOTIFY_OK   = 1;
static constexpr uint32_t TX_NOTIFY_FAIL = 2;

// ----------------------------------------------------------------------------
// Tunables
// ----------------------------------------------------------------------------
// Queue depth. Sized for the worst-case burst: ~10 enabled sensors at 10 Hz =
// 100 sends/s peak from sensor_bcast alone, plus heartbeats + occasional CLI.
// The dispatcher drains as fast as Wi-Fi can swallow frames (~1–3 ms each),
// so 32 slots = ~30–100 ms of buffering before backpressure kicks in. Beyond
// that, dropping new submissions is preferable to blocking producers.
static constexpr UBaseType_t kQueueDepth = 32;

// HWM diagnostic cadence. Periodic — does not race with normal operation;
// 10 s is plenty often to spot pathological growth during a stream test.
static constexpr unsigned long kHwmLogIntervalMs = 10000;

// ----------------------------------------------------------------------------
// Module state
// ----------------------------------------------------------------------------
static QueueHandle_t sJobQueue       = nullptr;
static TaskHandle_t  sTxTaskHandle   = nullptr;
static portMUX_TYPE  sStatsLock      = portMUX_INITIALIZER_UNLOCKED;
static Stats         sStats          = {};

// ----------------------------------------------------------------------------
// Stats helpers — all writes under the spinlock so a CLI reader can snapshot
// without tearing. Reads outside the spinlock are tolerated (best-effort) for
// the HWM-update fast path in submit().
// ----------------------------------------------------------------------------
static inline void statsBump(uint32_t Stats::* field) {
  taskENTER_CRITICAL(&sStatsLock);
  (sStats.*field)++;
  taskEXIT_CRITICAL(&sStatsLock);
}

static inline void statsObserveDepth(UBaseType_t depth) {
  // Cheap lock-free pre-check — avoids the critical section on the common
  // path where depth doesn't beat the existing HWM.
  if ((uint32_t)depth <= sStats.queueDepthHwm) return;
  taskENTER_CRITICAL(&sStatsLock);
  if ((uint32_t)depth > sStats.queueDepthHwm) sStats.queueDepthHwm = (uint32_t)depth;
  taskEXIT_CRITICAL(&sStatsLock);
}

// ----------------------------------------------------------------------------
// Dispatcher
// ----------------------------------------------------------------------------
static void runJob(const Job& job) {
  bool ok = false;
  switch (job.kind) {
    case JOB_AEAD_SMART:
      ok = v4_send_payload_smart(job.peerMac, job.type, job.flags,
                                 job.msgId, job.payload, job.payloadLen,
                                 job.ttl);
      break;
    case JOB_RAW:
      ok = v4_send_frame(job.peerMac, job.type, job.flags,
                         job.msgId, job.payload, job.payloadLen,
                         job.ttl);
      break;
    // Future kinds (JOB_BROADCAST_AUTH, JOB_FRAG_ACK) land here.
  }

  statsBump(ok ? &Stats::sent : &Stats::failed);

  if (job.payload) free(job.payload);

  // Wake a synchronous caller (submitSync) with the result. Done AFTER the
  // payload free so by the time the caller resumes the job is fully retired.
  // Fire-and-forget submit() leaves notifyTask null and skips this.
  if (job.notifyTask) {
    xTaskNotify(job.notifyTask, ok ? TX_NOTIFY_OK : TX_NOTIFY_FAIL,
                eSetValueWithOverwrite);
  }
}

static void txTask(void* /*param*/) {
  DEBUGF(DEBUG_ESPNOW_CORE,
         "[ESPNOW_TX] task started on core %d, stack=%u words queue=%u slots",
         (int)xPortGetCoreID(),
         (unsigned)ESPNOW_TX_STACK_WORDS, (unsigned)kQueueDepth);

  unsigned long lastHwmLog = millis();

  for (;;) {
    Job job;
    if (xQueueReceive(sJobQueue, &job, pdMS_TO_TICKS(1000)) == pdTRUE) {
      runJob(job);
    }

    const unsigned long now = millis();
    if (now - lastHwmLog >= kHwmLogIntervalMs) {
      lastHwmLog = now;
      UBaseType_t freeWords = uxTaskGetStackHighWaterMark(nullptr);
      // Snapshot stats under lock to avoid torn reads.
      Stats snap;
      taskENTER_CRITICAL(&sStatsLock);
      snap = sStats;
      taskEXIT_CRITICAL(&sStatsLock);
      DEBUGF(DEBUG_ESPNOW_CORE,
             "[ESPNOW_TX] stack free=%u peak_used=%u of %u | submitted=%u sent=%u failed=%u dropped=%u depth_hwm=%u",
             (unsigned)freeWords,
             (unsigned)(ESPNOW_TX_STACK_WORDS - freeWords),
             (unsigned)ESPNOW_TX_STACK_WORDS,
             (unsigned)snap.submitted, (unsigned)snap.sent,
             (unsigned)snap.failed,    (unsigned)snap.dropped,
             (unsigned)snap.queueDepthHwm);
    }
  }
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------
bool init() {
  if (sJobQueue) return true;  // idempotent

  sJobQueue = xQueueCreate(kQueueDepth, sizeof(Job));
  if (!sJobQueue) {
    DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] FAIL: xQueueCreate(%u slots, %u B each)",
           (unsigned)kQueueDepth, (unsigned)sizeof(Job));
    return false;
  }

  BaseType_t rc = xTaskCreateLogged(txTask, "espnow_tx",
                                    ESPNOW_TX_STACK_WORDS, nullptr,
                                    TASK_PRIORITY_HIGH, &sTxTaskHandle,
                                    "espnow.tx");
  if (rc != pdPASS) {
    DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] FAIL: xTaskCreateLogged rc=%d", (int)rc);
    vQueueDelete(sJobQueue);
    sJobQueue = nullptr;
    return false;
  }

  DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] init OK");
  return true;
}

bool submit(const Job& job) {
  if (!sJobQueue) return false;

  statsBump(&Stats::submitted);

  if (xQueueSend(sJobQueue, &job, 0) != pdTRUE) {
    statsBump(&Stats::dropped);
    return false;
  }

  // Sample depth AFTER the send so the observation reflects post-enqueue
  // state. Lock-free fast path inside statsObserveDepth keeps this cheap.
  statsObserveDepth(uxQueueMessagesWaiting(sJobQueue));
  return true;
}

bool submitSync(Job& job, uint32_t timeoutMs) {
  // submitSync OWNS job.payload on every return path: it is freed here on an
  // enqueue failure, and by the dispatcher once the job is accepted (even if
  // we then time out waiting). The caller must never free it.
  if (!sJobQueue) {
    if (job.payload) free(job.payload);
    return false;
  }
  // Self-deadlock guard: the dispatcher must never wait on its own queue.
  if (xTaskGetCurrentTaskHandle() == sTxTaskHandle) {
    if (job.payload) free(job.payload);
    return false;
  }

  job.notifyTask = xTaskGetCurrentTaskHandle();
  // Drop any stale pending notification so a leftover can't satisfy our wait.
  xTaskNotifyStateClear(nullptr);

  statsBump(&Stats::submitted);

  // Blocking enqueue (bounded by the caller's timeout) gives backpressure: a
  // burst producer throttles to the dispatcher's drain rate instead of dropping.
  if (xQueueSend(sJobQueue, &job, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
    statsBump(&Stats::dropped);
    if (job.payload) free(job.payload);   // dispatcher never took it
    return false;
  }
  statsObserveDepth(uxQueueMessagesWaiting(sJobQueue));

  // Wait for the dispatcher to send and notify. The payload now belongs to the
  // dispatcher and is freed there regardless of our wait outcome — so on a
  // timeout we return without freeing (no double-free, no use-after-free; the
  // result lived in the notification value, not a shared object).
  uint32_t result = 0;
  if (xTaskNotifyWait(0, 0xFFFFFFFFUL, &result, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
    DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] submitSync TIMEOUT (%ums) type=%u",
           (unsigned)timeoutMs, (unsigned)job.type);
    // NOTE: a timeout means the dispatcher is wedged — that's the real fault to
    // chase. After a timeout, a later in-flight completion could (with one
    // notification slot) satisfy a subsequent submitSync from this same task;
    // in normal operation the send is sub-millisecond and this never fires.
    return false;
  }
  return result == TX_NOTIFY_OK;
}

void getStats(Stats* out) {
  if (!out) return;
  taskENTER_CRITICAL(&sStatsLock);
  *out = sStats;
  taskEXIT_CRITICAL(&sStatsLock);
}

TaskHandle_t getTaskHandle() {
  return sTxTaskHandle;
}

// ----------------------------------------------------------------------------
// Convenience helpers — see header for contract and task-context rules.
// ----------------------------------------------------------------------------

bool sendAead(const uint8_t mac[6], uint8_t type, uint16_t flags,
              uint32_t msgId, const uint8_t* payload, uint16_t payloadLen,
              uint8_t ttl) {
  uint8_t* psBuf = nullptr;
  if (payloadLen > 0) {
    psBuf = (uint8_t*)ps_alloc(payloadLen, AllocPref::PreferPSRAM, "espnow.tx.aead");
    if (!psBuf) {
      DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] sendAead ps_alloc failed type=%u len=%u",
             (unsigned)type, (unsigned)payloadLen);
      return false;
    }
    memcpy(psBuf, payload, payloadLen);
  }
  Job job = {};
  job.kind       = JOB_AEAD_SMART;
  memcpy(job.peerMac, mac, 6);
  job.type       = type;
  job.flags      = flags;
  job.msgId      = msgId;
  job.ttl        = ttl;
  job.payload    = psBuf;
  job.payloadLen = payloadLen;
  if (!submit(job)) {
    // submit() does NOT take ownership on failure — caller frees.
    if (psBuf) free(psBuf);
    return false;
  }
  return true;
}

bool sendAeadSync(const uint8_t mac[6], uint8_t type, uint16_t flags,
                  uint32_t msgId, const uint8_t* payload, uint16_t payloadLen,
                  uint8_t ttl, uint32_t timeoutMs) {
  uint8_t* psBuf = nullptr;
  if (payloadLen > 0) {
    psBuf = (uint8_t*)ps_alloc(payloadLen, AllocPref::PreferPSRAM, "espnow.tx.aead.sync");
    if (!psBuf) {
      DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] sendAeadSync ps_alloc failed type=%u len=%u",
             (unsigned)type, (unsigned)payloadLen);
      return false;
    }
    memcpy(psBuf, payload, payloadLen);
  }
  Job job = {};
  job.kind       = JOB_AEAD_SMART;
  memcpy(job.peerMac, mac, 6);
  job.type       = type;
  job.flags      = flags;
  job.msgId      = msgId;
  job.ttl        = ttl;
  job.payload    = psBuf;
  job.payloadLen = payloadLen;
  // submitSync OWNS the buffer on every return path (its contract) — do not
  // free psBuf here on failure.
  return submitSync(job, timeoutMs);
}

}  // namespace espnowtx
