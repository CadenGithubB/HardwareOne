#include "System_ESPNow_Tx.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/event_groups.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

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

// ----------------------------------------------------------------------------
// Tunables
// ----------------------------------------------------------------------------
// Queue depth. Sized for the worst-case burst: ~10 enabled sensors at 10 Hz =
// 100 sends/s peak from sensor_bcast alone, plus heartbeats + occasional CLI.
// The dispatcher drains as fast as Wi-Fi can swallow frames (~1–3 ms each),
// so 32 slots = ~30–100 ms of buffering before backpressure kicks in. Beyond
// that, dropping new submissions is preferable to blocking producers.
static constexpr UBaseType_t kQueueDepth = 32;

// FreeRTOS reserves the high 8 EventGroup bits, leaving 24 independent wait
// bits on this 32-bit build. The pool bounds concurrent synchronous callers;
// fire-and-forget jobs still use the full 32-slot queue.
static constexpr uint8_t kSyncCompletionSlots = 24;
static_assert(sizeof(EventBits_t) == sizeof(uint32_t),
              "ESP-NOW TX completion pool requires 24-bit EventGroups");

// HWM diagnostic cadence. Periodic — does not race with normal operation;
// 10 s is plenty often to spot pathological growth during a stream test.
static constexpr unsigned long kHwmLogIntervalMs = 10000;

// ----------------------------------------------------------------------------
// Module state
// ----------------------------------------------------------------------------
enum class Lifecycle : uint8_t {
  STOPPED,
  INITIALIZING,
  RUNNING,
  CLOSING,
};

static QueueHandle_t    sJobQueue        = nullptr;
static TaskHandle_t     sTxTaskHandle    = nullptr;
static SemaphoreHandle_t sStopDone       = nullptr;
static SemaphoreHandle_t sCompletionLock = nullptr;
static EventGroupHandle_t sCompletionEvents = nullptr;
static Lifecycle        sLifecycle       = Lifecycle::STOPPED;
static uint32_t         sActiveSubmitters = 0;
static portMUX_TYPE     sStateLock       = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE     sStatsLock       = portMUX_INITIALIZER_UNLOCKED;
static Stats            sStats           = {};
// Stable, never-freed ownership lock for the whole stop/wait/delete sequence.
// Without it two public stop() callers could wait on the same one-shot join
// semaphore while the winner deletes that semaphore and the shared lifecycle.
static StaticSemaphore_t sStopOwnerStorage;
static SemaphoreHandle_t sStopOwner =
    xSemaphoreCreateMutexStatic(&sStopOwnerStorage);

// Lifecycle-owned completion slots avoid hot-path heap churn. A caller keeps
// its slot until it consumes a result or times out; after a timeout the
// dispatcher retains and eventually releases the slot. No task handle is ever
// retained, so a late result cannot notify a deleted task or satisfy a future
// request. The small slot table is task-only data and is PSRAM-preferred.
struct SyncCompletion {
  bool inUse;
  bool callerWaiting;
  bool dispatcherOwned;
  int8_t result;  // 1 success, -1 failure, 0 pending
  uint8_t index;
};

// The three kernel objects are statically backed by one init-time INTERNAL
// allocation, avoiding three independently-fragmenting control-block allocs.
struct LifecycleControl {
  StaticSemaphore_t stopDoneStorage;
  StaticSemaphore_t completionLockStorage;
  StaticEventGroup_t completionEventsStorage;
};

static LifecycleControl* sControl = nullptr;
static SyncCompletion* sCompletionPool = nullptr;

static SyncCompletion* completionCreate() {
  if (!sCompletionLock || !sCompletionEvents || !sCompletionPool) return nullptr;
  SyncCompletion* completion = nullptr;
  xSemaphoreTake(sCompletionLock, portMAX_DELAY);
  for (uint8_t i = 0; i < kSyncCompletionSlots; ++i) {
    SyncCompletion& slot = sCompletionPool[i];
    if (slot.inUse) continue;
    xEventGroupClearBits(sCompletionEvents, (EventBits_t)1U << i);
    slot.inUse = true;
    slot.callerWaiting = true;
    slot.dispatcherOwned = true;
    slot.result = 0;
    slot.index = i;
    completion = &slot;
    break;
  }
  xSemaphoreGive(sCompletionLock);
  return completion;
}

static void completionDiscard(SyncCompletion* completion) {
  if (!completion) return;
  xSemaphoreTake(sCompletionLock, portMAX_DELAY);
  xEventGroupClearBits(sCompletionEvents, (EventBits_t)1U << completion->index);
  completion->inUse = false;
  completion->callerWaiting = false;
  completion->dispatcherOwned = false;
  completion->result = 0;
  xSemaphoreGive(sCompletionLock);
}

static void completionFinish(SyncCompletion* completion, bool ok) {
  if (!completion) return;
  xSemaphoreTake(sCompletionLock, portMAX_DELAY);
  completion->result = ok ? 1 : -1;
  completion->dispatcherOwned = false;
  if (completion->callerWaiting) {
    // Set while holding the pool mutex. The caller also takes this mutex before
    // releasing/reusing the slot, closing the timeout-vs-set stale-bit race.
    xEventGroupSetBits(sCompletionEvents, (EventBits_t)1U << completion->index);
  } else {
    completion->inUse = false;
    completion->result = 0;
  }
  xSemaphoreGive(sCompletionLock);
}

static bool completionWait(SyncCompletion* completion, uint32_t timeoutMs) {
  const EventBits_t bit = (EventBits_t)1U << completion->index;
  (void)xEventGroupWaitBits(sCompletionEvents, bit, pdFALSE, pdTRUE,
                            pdMS_TO_TICKS(timeoutMs));

  xSemaphoreTake(sCompletionLock, portMAX_DELAY);
  // State, rather than the wait return, is authoritative at the exact timeout
  // boundary: a dispatcher completion that won the mutex is a valid result.
  const bool finished = !completion->dispatcherOwned;
  const bool ok = finished && completion->result == 1;
  completion->callerWaiting = false;
  if (finished) {
    xEventGroupClearBits(sCompletionEvents, bit);
    completion->inUse = false;
    completion->result = 0;
  }
  xSemaphoreGive(sCompletionLock);
  return ok;
}

static bool completionPoolIdle() {
  if (!sCompletionLock || !sCompletionPool) return true;
  bool idle = true;
  xSemaphoreTake(sCompletionLock, portMAX_DELAY);
  for (uint8_t i = 0; i < kSyncCompletionSlots; ++i) {
    if (sCompletionPool[i].inUse) {
      idle = false;
      break;
    }
  }
  xSemaphoreGive(sCompletionLock);
  return idle;
}

static bool claimRunningQueue(QueueHandle_t* outQueue, TaskHandle_t* outTask = nullptr) {
  bool claimed = false;
  taskENTER_CRITICAL(&sStateLock);
  if (sLifecycle == Lifecycle::RUNNING && sJobQueue) {
    ++sActiveSubmitters;
    *outQueue = sJobQueue;
    if (outTask) *outTask = sTxTaskHandle;
    claimed = true;
  }
  taskEXIT_CRITICAL(&sStateLock);
  return claimed;
}

static void releaseQueueClaim() {
  taskENTER_CRITICAL(&sStateLock);
  if (sActiveSubmitters > 0) --sActiveSubmitters;
  taskEXIT_CRITICAL(&sStateLock);
}

static bool closingAndNoSubmitters() {
  bool ready;
  taskENTER_CRITICAL(&sStateLock);
  ready = sLifecycle == Lifecycle::CLOSING && sActiveSubmitters == 0;
  taskEXIT_CRITICAL(&sStateLock);
  return ready;
}

static bool lifecycleIsClosing() {
  bool closing;
  taskENTER_CRITICAL(&sStateLock);
  closing = sLifecycle == Lifecycle::CLOSING;
  taskEXIT_CRITICAL(&sStateLock);
  return closing;
}

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
  // Complete AFTER freeing the payload so a woken synchronous caller observes
  // a fully retired job. Fire-and-forget jobs leave completion null.
  completionFinish(job.completion, ok);
}

static void failJob(const Job& job) {
  statsBump(&Stats::failed);
  if (job.payload) free(job.payload);
  completionFinish(job.completion, false);
}

static void txTask(void* param) {
  QueueHandle_t queue = static_cast<QueueHandle_t>(param);
  DEBUGF(DEBUG_ESPNOW_CORE,
         "[ESPNOW_TX] task started on core %d, stack=%u words queue=%u slots",
         (int)xPortGetCoreID(),
         (unsigned)ESPNOW_TX_STACK_WORDS, (unsigned)kQueueDepth);

  unsigned long lastHwmLog = millis();
  uint32_t cancelled = 0;

  for (;;) {
    if (lifecycleIsClosing()) {
      Job queued;
      while (xQueueReceive(queue, &queued, 0) == pdTRUE) {
        failJob(queued);
        ++cancelled;
      }

      // No producer can claim the queue after CLOSING. Once every producer
      // that claimed it while RUNNING has left xQueueSend and the queue is
      // empty, no future enqueue is possible and the dispatcher can join.
      if (closingAndNoSubmitters() && uxQueueMessagesWaiting(queue) == 0 &&
          completionPoolIdle()) {
        break;
      }
      vTaskDelay(1);
      continue;
    }

    Job job;
    if (xQueueReceive(queue, &job, pdMS_TO_TICKS(1000)) == pdTRUE) {
      // CLOSING may have won immediately after xQueueReceive. Treat that job
      // as queued work (fail/wake/free), not as a new radio operation.
      if (lifecycleIsClosing()) {
        failJob(job);
        ++cancelled;
      } else {
        runJob(job);
      }
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

  DEBUGF(DEBUG_ESPNOW_CORE,
         "[ESPNOW_TX] quiesced; cancelled=%u", (unsigned)cancelled);
  // stop() owns deletion. Signal only after the queue can no longer be
  // touched, then park so stop() has an explicit join point before deleting
  // the task stack and queue.
  xSemaphoreGive(sStopDone);
  vTaskSuspend(nullptr);
}

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------
bool init() {
  if (!sStopOwner || xSemaphoreTake(sStopOwner, portMAX_DELAY) != pdTRUE) {
    DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] init lifecycle owner unavailable");
    return false;
  }
  struct InitOwnerGuard {
    ~InitOwnerGuard() { xSemaphoreGive(sStopOwner); }
  } initOwnerGuard;

  taskENTER_CRITICAL(&sStateLock);
  if (sLifecycle == Lifecycle::RUNNING) {
    taskEXIT_CRITICAL(&sStateLock);
    return true;
  }
  if (sLifecycle != Lifecycle::STOPPED) {
    taskEXIT_CRITICAL(&sStateLock);
    DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] init rejected during lifecycle transition");
    return false;
  }
  sLifecycle = Lifecycle::INITIALIZING;
  taskEXIT_CRITICAL(&sStateLock);

  QueueHandle_t queue = xQueueCreate(kQueueDepth, sizeof(Job));
  if (!queue) {
    DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] FAIL: xQueueCreate(%u slots, %u B each)",
           (unsigned)kQueueDepth, (unsigned)sizeof(Job));
    taskENTER_CRITICAL(&sStateLock);
    sLifecycle = Lifecycle::STOPPED;
    taskEXIT_CRITICAL(&sStateLock);
    return false;
  }

  LifecycleControl* control = (LifecycleControl*)ps_calloc(
      1, sizeof(LifecycleControl), AllocPref::RequireInternal, "espnow.tx.lifecycle");
  SyncCompletion* completionPool = (SyncCompletion*)ps_alloc(
      sizeof(SyncCompletion) * kSyncCompletionSlots,
      AllocPref::PreferPSRAM, "espnow.tx.completions");
  if (!control || !completionPool) {
    DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] FAIL: lifecycle control allocation");
    if (control) heap_caps_free(control);
    if (completionPool) free(completionPool);
    vQueueDelete(queue);
    taskENTER_CRITICAL(&sStateLock);
    sLifecycle = Lifecycle::STOPPED;
    taskEXIT_CRITICAL(&sStateLock);
    return false;
  }
  memset(completionPool, 0, sizeof(SyncCompletion) * kSyncCompletionSlots);

  SemaphoreHandle_t stopDone = xSemaphoreCreateBinaryStatic(&control->stopDoneStorage);
  SemaphoreHandle_t completionLock =
      xSemaphoreCreateMutexStatic(&control->completionLockStorage);
  EventGroupHandle_t completionEvents =
      xEventGroupCreateStatic(&control->completionEventsStorage);
  if (!stopDone || !completionLock || !completionEvents) {
    DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] FAIL: static lifecycle object creation");
    if (completionEvents) vEventGroupDelete(completionEvents);
    if (completionLock) vSemaphoreDelete(completionLock);
    if (stopDone) vSemaphoreDelete(stopDone);
    free(completionPool);
    heap_caps_free(control);
    vQueueDelete(queue);
    taskENTER_CRITICAL(&sStateLock);
    sLifecycle = Lifecycle::STOPPED;
    taskEXIT_CRITICAL(&sStateLock);
    return false;
  }

  taskENTER_CRITICAL(&sStateLock);
  sJobQueue = queue;
  sStopDone = stopDone;
  sCompletionLock = completionLock;
  sCompletionEvents = completionEvents;
  sControl = control;
  sCompletionPool = completionPool;
  sActiveSubmitters = 0;
  taskEXIT_CRITICAL(&sStateLock);

  // Pin to Core 0 (PRO_CORE): the RF send path belongs with the Wi-Fi/ESP-NOW
  // stack it feeds (esp_now_send runs in the Wi-Fi task on Core 0). HIGH prio so
  // it won't starve there; keeps it off the compute core.
  BaseType_t rc = xTaskCreateLogged(txTask, "espnow_tx",
                                    ESPNOW_TX_STACK_WORDS, queue,
                                    TASK_PRIORITY_HIGH, &sTxTaskHandle,
                                    "espnow.tx", PRO_CORE);
  if (rc != pdPASS) {
    DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] FAIL: xTaskCreateLogged rc=%d", (int)rc);
    vEventGroupDelete(completionEvents);
    vSemaphoreDelete(completionLock);
    vSemaphoreDelete(stopDone);
    vQueueDelete(queue);
    free(completionPool);
    heap_caps_free(control);
    taskENTER_CRITICAL(&sStateLock);
    sJobQueue = nullptr;
    sStopDone = nullptr;
    sCompletionLock = nullptr;
    sCompletionEvents = nullptr;
    sControl = nullptr;
    sCompletionPool = nullptr;
    sTxTaskHandle = nullptr;
    sLifecycle = Lifecycle::STOPPED;
    taskEXIT_CRITICAL(&sStateLock);
    return false;
  }

  taskENTER_CRITICAL(&sStatsLock);
  sStats = {};
  taskEXIT_CRITICAL(&sStatsLock);
  taskENTER_CRITICAL(&sStateLock);
  sLifecycle = Lifecycle::RUNNING;
  taskEXIT_CRITICAL(&sStateLock);

  DEBUGF(DEBUG_ESPNOW_CORE,
         "[ESPNOW_TX] init OK; completion pool=%u B PSRAM-preferred, control=%u B INTERNAL",
         (unsigned)(sizeof(SyncCompletion) * kSyncCompletionSlots),
         (unsigned)sizeof(LifecycleControl));
  return true;
}

bool stop(uint32_t timeoutMs) {
  if (!sStopOwner ||
      xSemaphoreTake(sStopOwner, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
    DEBUGF(DEBUG_ESPNOW_CORE,
           "[ESPNOW_TX] stop owner busy after %ums", (unsigned)timeoutMs);
    return false;
  }
  struct StopOwnerGuard {
    ~StopOwnerGuard() { xSemaphoreGive(sStopOwner); }
  } stopOwnerGuard;

  QueueHandle_t queue;
  TaskHandle_t task;
  SemaphoreHandle_t stopDone;

  taskENTER_CRITICAL(&sStateLock);
  if (sLifecycle == Lifecycle::STOPPED) {
    taskEXIT_CRITICAL(&sStateLock);
    return true;
  }
  if (sLifecycle == Lifecycle::INITIALIZING) {
    taskEXIT_CRITICAL(&sStateLock);
    DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] stop rejected while init is in progress");
    return false;
  }
  if (xTaskGetCurrentTaskHandle() == sTxTaskHandle) {
    taskEXIT_CRITICAL(&sStateLock);
    DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] stop rejected from dispatcher task");
    return false;
  }
  if (sLifecycle == Lifecycle::RUNNING) sLifecycle = Lifecycle::CLOSING;
  queue = sJobQueue;
  task = sTxTaskHandle;
  stopDone = sStopDone;
  taskEXIT_CRITICAL(&sStateLock);

  if (!queue || !task || !stopDone) {
    DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] stop found incomplete lifecycle state");
    return false;
  }
  // Wake xQueueReceive immediately. Existing submitters remain safe because
  // the queue is retained until the dispatcher observes their count at zero.
  xTaskAbortDelay(task);
  if (xSemaphoreTake(stopDone, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
    DEBUGF(DEBUG_ESPNOW_CORE,
           "[ESPNOW_TX] stop TIMEOUT after %ums; resources retained for retry",
           (unsigned)timeoutMs);
    return false;
  }

  vTaskDelete(task);
  vQueueDelete(queue);
  vEventGroupDelete(sCompletionEvents);
  vSemaphoreDelete(sCompletionLock);
  vSemaphoreDelete(stopDone);
  free(sCompletionPool);
  heap_caps_free(sControl);

  taskENTER_CRITICAL(&sStateLock);
  sJobQueue = nullptr;
  sTxTaskHandle = nullptr;
  sStopDone = nullptr;
  sCompletionLock = nullptr;
  sCompletionEvents = nullptr;
  sCompletionPool = nullptr;
  sControl = nullptr;
  sActiveSubmitters = 0;
  sLifecycle = Lifecycle::STOPPED;
  taskEXIT_CRITICAL(&sStateLock);
  DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] stopped and resources released");
  return true;
}

bool submit(const Job& job) {
  QueueHandle_t queue = nullptr;
  if (!claimRunningQueue(&queue)) return false;

  statsBump(&Stats::submitted);

  if (xQueueSend(queue, &job, 0) != pdTRUE) {
    statsBump(&Stats::dropped);
    releaseQueueClaim();
    return false;
  }

  // Sample depth AFTER the send so the observation reflects post-enqueue
  // state. Lock-free fast path inside statsObserveDepth keeps this cheap.
  statsObserveDepth(uxQueueMessagesWaiting(queue));
  releaseQueueClaim();
  return true;
}

bool submitSync(Job& job, uint32_t timeoutMs) {
  // submitSync OWNS job.payload on every return path: it is freed here on an
  // enqueue failure, and by the dispatcher once the job is accepted (even if
  // we then time out waiting). The caller must never free it.
  QueueHandle_t queue = nullptr;
  TaskHandle_t txTaskHandle = nullptr;
  if (!claimRunningQueue(&queue, &txTaskHandle)) {
    if (job.payload) free(job.payload);
    return false;
  }
  // Self-deadlock guard: the dispatcher must never wait on its own queue.
  if (xTaskGetCurrentTaskHandle() == txTaskHandle) {
    releaseQueueClaim();
    if (job.payload) free(job.payload);
    return false;
  }

  SyncCompletion* completion = completionCreate();
  if (!completion) {
    releaseQueueClaim();
    if (job.payload) free(job.payload);
    DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] submitSync completion pool exhausted");
    return false;
  }
  job.completion = completion;

  statsBump(&Stats::submitted);

  // Blocking enqueue (bounded by the caller's timeout) gives backpressure: a
  // burst producer throttles to the dispatcher's drain rate instead of dropping.
  BaseType_t queued = xQueueSend(queue, &job, pdMS_TO_TICKS(timeoutMs));
  job.completion = nullptr;
  if (queued != pdTRUE) {
    statsBump(&Stats::dropped);
    releaseQueueClaim();
    if (job.payload) free(job.payload);   // dispatcher never took it
    completionDiscard(completion);
    return false;
  }
  statsObserveDepth(uxQueueMessagesWaiting(queue));
  releaseQueueClaim();

  // The dispatcher owns the payload and slot from here. On timeout the caller
  // marks itself gone; the dispatcher later releases the slot after retiring
  // the job, so no stale event can survive into a future request.
  bool ok = completionWait(completion, timeoutMs);
  if (!ok) {
    DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] submitSync FAILED/TIMEOUT (%ums) type=%u",
           (unsigned)timeoutMs, (unsigned)job.type);
  }
  return ok;
}

void getStats(Stats* out) {
  if (!out) return;
  taskENTER_CRITICAL(&sStatsLock);
  *out = sStats;
  taskEXIT_CRITICAL(&sStatsLock);
}

bool getTaskStackSnapshot(TaskHandle_t* outHandle, UBaseType_t* outWatermark) {
  if (!outHandle || !outWatermark || !sStopOwner ||
      xSemaphoreTake(sStopOwner, portMAX_DELAY) != pdTRUE) {
    return false;
  }

  TaskHandle_t task = nullptr;
  taskENTER_CRITICAL(&sStateLock);
  if (sLifecycle == Lifecycle::RUNNING) task = sTxTaskHandle;
  taskEXIT_CRITICAL(&sStateLock);

  if (!task) {
    xSemaphoreGive(sStopOwner);
    return false;
  }

  // sStopOwner spans lookup + query, so stop() cannot delete this generation.
  const UBaseType_t watermark = uxTaskGetStackHighWaterMark(task);
  *outHandle = task;
  *outWatermark = watermark;
  xSemaphoreGive(sStopOwner);
  return true;
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
