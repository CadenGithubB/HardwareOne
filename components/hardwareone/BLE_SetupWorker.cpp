#include "BLE_SetupWorker.h"
#if HW1_BLE_SETUP_WORKER
#include "BLE_CoexRecords.h"
#include "System_TaskUtils.h"
#include <Arduino.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#if ENABLE_G2_GLASSES
extern const BleSetupOps g2SetupOps, g2RingSetupOps;
#if ENABLE_BLE_PHONE_PIPE
extern const BleSetupOps g2RingRetireSetupOps;
#endif
#endif
#if ENABLE_BLE_PHONE_PIPE
extern const BleSetupOps phonePipeSetupOps;
#endif

namespace {
namespace hs = hw1setup;
const BleSetupOps* const kOps[hs::kClients] = {
#if ENABLE_G2_GLASSES
  &g2SetupOps,
#if ENABLE_BLE_PHONE_PIPE
  &g2RingRetireSetupOps,
#else
  nullptr,
#endif
  &g2RingSetupOps,
#else
  nullptr, nullptr, nullptr,
#endif
#if ENABLE_BLE_PHONE_PIPE
  &phonePipeSetupOps,
#else
  nullptr,
#endif
};
// Stack in BYTES, internal RAM. The glasses and ring jobs run their connect
// bodies here (5120 B, as the old g2_ble_connect worker); without the glasses
// the worker only picks and waits on phone turns.
constexpr uint32_t kStackBytes = ENABLE_G2_GLASSES ? 5120 : 4096;
constexpr uint32_t kLongHoldMs = 120000;

portMUX_TYPE sCoreMux = portMUX_INITIALIZER_UNLOCKED;
hs::Core sCore;
StaticSemaphore_t sWakeStorage;
SemaphoreHandle_t sWake = nullptr;
TaskHandle_t sTask = nullptr;
enum class Init : uint8_t { Uninitialized, Starting, Ready, Stopping };
Init sInit = Init::Uninitialized;

const BleSetupOps* ops(hs::Client c) { return kOps[size_t(c)]; }

struct RecordHooks {
  void logSetupWait(const char* label, uint32_t ms) { bleCoexLogSetupWait(label, ms); }
  void setAccessoryWait(uint32_t ms) { bleCoexSetAccessoryWait(ms); }
  void clearAccessoryWait() { bleCoexClearAccessoryWait(); }
};

struct Env {
  void lock() { portENTER_CRITICAL(&sCoreMux); }
  void unlock() { portEXIT_CRITICAL(&sCoreMux); }
  uint32_t now() { return millis(); }
  void waitWake(uint32_t ms) {
    (void)xSemaphoreTake(sWake, ms == hs::kForever ? portMAX_DELAY : pdMS_TO_TICKS(ms));
  }
  hs::Verdict ready(hs::Client c, uint32_t t, const hs::WaitInfo& info) { return ops(c)->ready(t, info); }
  void waiting(hs::Client c, uint32_t t, uint8_t reason) { if (ops(c)->waiting) ops(c)->waiting(t, reason); }
  hs::End run(hs::Client c, uint32_t t, const hs::RunInfo& info) {
    const BleSetupOps* o = ops(c);
    RecordHooks hooks;
    const uint32_t began = millis();
    const hs::End end = hs::runRecorded(hooks, o->label(t), o->recordWait, info, [&] { return o->run(t, info); });
    const uint32_t held = millis() - began;
    if (held >= kLongHoldMs) ESP_LOGW("BLE_SETUP", "%s held setup %lu s", o->label(t), (unsigned long)(held / 1000));
    return end;
  }
  void finish(hs::Client c, uint32_t t, const hs::FinishInfo& info) { ops(c)->finish(t, info); }
};

// After a wake cycle: whether the worker leaves, which it does once a release
// asked it to and no job is claimed, queued, running or finishing. It nulls
// its own handle under the lock a start checks, so the next start creates a
// fresh task; the caller deletes the task right after, with nothing between.
bool workerLeaves() {
  portENTER_CRITICAL(&sCoreMux);
  const bool leave = sInit == Init::Stopping && sCore.idle(hs::kAll);
  if (leave) {
    sTask = nullptr;
    sInit = Init::Uninitialized;
  }
  portEXIT_CRITICAL(&sCoreMux);
  return leave;
}

void loop(void*) {
  Env env;
  for (;;) {
    hs::serve(sCore, env);
    if (workerLeaves()) vTaskDelete(nullptr);
  }
}
}  // namespace

bool bleSetupWorkerStart() {
  portENTER_CRITICAL(&sCoreMux);
  if (sInit != Init::Uninitialized) {
    const bool ready = sInit == Init::Ready;
    portEXIT_CRITICAL(&sCoreMux);
    return ready;
  }
  sInit = Init::Starting;
  portEXIT_CRITICAL(&sCoreMux);
  if (!sWake) sWake = xSemaphoreCreateBinaryStatic(&sWakeStorage);
  TaskHandle_t task = nullptr;
  const bool created = sWake &&
      xTaskCreateLogged(loop, "ble_setup", kStackBytes, nullptr, 5, &task, "ble.setup", APP_CORE) == pdPASS;
  portENTER_CRITICAL(&sCoreMux);
  if (created) sTask = task;
  sInit = created ? Init::Ready : Init::Uninitialized;
  portEXIT_CRITICAL(&sCoreMux);
  if (!created) ESP_LOGE("BLE_SETUP", "worker start failed");
  return created;
}

bool bleSetupWorkerRelease(uint32_t timeoutMs) {
  const TaskHandle_t self = xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&sCoreMux);
  if (sInit == Init::Uninitialized) {
    portEXIT_CRITICAL(&sCoreMux);
    return true;
  }
  if (sInit != Init::Ready || !sTask || self == sTask) {
    portEXIT_CRITICAL(&sCoreMux);
    return false;
  }
  sInit = Init::Stopping;  // no new claim, and a start says no, until this ends
  portEXIT_CRITICAL(&sCoreMux);
  // A job in progress finishes on its own, never forced; then the worker
  // leaves at its next wake cycle and nulls sTask (workerLeaves).
  if (bleSetupWaitIdle(hs::kAll, timeoutMs)) {
    bleSetupWake();
    const uint32_t start = millis();
    for (;;) {
      portENTER_CRITICAL(&sCoreMux);
      const bool gone = sTask == nullptr;
      portEXIT_CRITICAL(&sCoreMux);
      if (gone || hs::expired(millis(), start, 1000)) break;
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  // Fail closed: a worker still there is kept and takes claims again.
  portENTER_CRITICAL(&sCoreMux);
  const bool released = sTask == nullptr;
  sInit = released ? Init::Uninitialized : Init::Ready;
  portEXIT_CRITICAL(&sCoreMux);
  return released;
}

bool bleSetupClaim(BleSetupClient client, uint32_t* token) {
  if (!ops(client)) return false;
  portENTER_CRITICAL(&sCoreMux);
  const bool ok = sInit == Init::Ready && sCore.claim(client, token);
  portEXIT_CRITICAL(&sCoreMux);
  return ok;
}

bool bleSetupPublish(BleSetupClient client, uint32_t token) {
  portENTER_CRITICAL(&sCoreMux);
  const bool ok = sCore.publish(client, token, millis());
  portEXIT_CRITICAL(&sCoreMux);
  if (ok) bleSetupWake();
  return ok;
}

void bleSetupCancelClaim(BleSetupClient client, uint32_t token) {
  portENTER_CRITICAL(&sCoreMux);
  (void)sCore.cancelClaim(client, token);
  portEXIT_CRITICAL(&sCoreMux);
}

BleSetupWithdraw bleSetupWithdraw(BleSetupClient client, uint32_t token) {
  hs::Record taken;
  portENTER_CRITICAL(&sCoreMux);
  const auto result = sCore.withdraw(client, token, millis(), &taken);
  portEXIT_CRITICAL(&sCoreMux);
  if (result == hs::Core::Withdraw::Running) return BleSetupWithdraw::Running;
  if (result != hs::Core::Withdraw::Withdrawn) return BleSetupWithdraw::NotLive;
  ops(client)->finish(taken.token, hs::FinishInfo{hs::End::Withdrawn, hs::kNone, taken.audioMs});
  portENTER_CRITICAL(&sCoreMux);
  sCore.finished(client);
  portEXIT_CRITICAL(&sCoreMux);
  return BleSetupWithdraw::Withdrawn;
}

bool bleSetupLive(BleSetupClient client, uint32_t token) {
  portENTER_CRITICAL(&sCoreMux);
  const bool live = sCore.live(client, token);
  portEXIT_CRITICAL(&sCoreMux);
  return live;
}

bool bleSetupIdle(uint8_t mask) {
  portENTER_CRITICAL(&sCoreMux);
  const bool idle = sCore.idle(mask);
  portEXIT_CRITICAL(&sCoreMux);
  return idle;
}

bool bleSetupWaitIdle(uint8_t mask, uint32_t timeoutMs) {
  if (sTask && xTaskGetCurrentTaskHandle() == sTask) return bleSetupIdle(mask);
  const uint32_t start = millis();
  for (;;) {
    if (bleSetupIdle(mask)) return true;
    if (hs::expired(millis(), start, timeoutMs)) return false;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

bool bleSetupAccessoryBusy() {
  portENTER_CRITICAL(&sCoreMux);
  const bool busy = sCore.busy(hs::kAccessories);
  portEXIT_CRITICAL(&sCoreMux);
  return busy;
}

bool bleSetupOtherQueued(BleSetupClient self) {
  portENTER_CRITICAL(&sCoreMux);
  const bool queued = sCore.otherQueued(self);
  portEXIT_CRITICAL(&sCoreMux);
  return queued;
}

void bleSetupWake() { if (sWake) (void)xSemaphoreGive(sWake); }

void bleSetupWaitWake(uint32_t ms) {
  if (sWake) (void)xSemaphoreTake(sWake, pdMS_TO_TICKS(ms));
}
#endif
