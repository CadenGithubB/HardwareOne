#include "BLE_CentralTx.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>

static SemaphoreHandle_t sBleCentralTx = nullptr;
static StaticSemaphore_t sBleCentralTxStorage;
static portMUX_TYPE sBleCentralTxInitMux = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<unsigned> sAccessoryWaiters{0};
static std::atomic<bool> sAccessoryTxHeld{false};
static std::atomic<TickType_t> sLastAccessoryTx{0};

void bleCentralTxInit() {
  portENTER_CRITICAL(&sBleCentralTxInitMux);
  if (!sBleCentralTx) sBleCentralTx = xSemaphoreCreateMutexStatic(&sBleCentralTxStorage);
  portEXIT_CRITICAL(&sBleCentralTxInitMux);
}

bool bleCentralTxTake(uint32_t timeoutMs) {
  if (!sBleCentralTx) bleCentralTxInit();
  if (!sBleCentralTx) return false;
  TickType_t ticks = (timeoutMs == 0) ? 0 : pdMS_TO_TICKS(timeoutMs);
  ++sAccessoryWaiters;
  const bool held = xSemaphoreTake(sBleCentralTx, ticks) == pdTRUE;
  --sAccessoryWaiters;
  if (held) {
    sAccessoryTxHeld.store(true, std::memory_order_release);
    sLastAccessoryTx.store(xTaskGetTickCount(), std::memory_order_release);
  }
  return held;
}

void bleCentralTxGive() {
  if (!sBleCentralTx) return;
  if (sAccessoryTxHeld.exchange(false, std::memory_order_acq_rel))
    sLastAccessoryTx.store(xTaskGetTickCount(), std::memory_order_release);
  xSemaphoreGive(sBleCentralTx);
}

bool bleCentralTxIsHeld() {
  if (!sBleCentralTx) return false;
  return xSemaphoreGetMutexHolder(sBleCentralTx) != nullptr;
}

bool bleCentralTxIsHeldByOther() {
  if (!sBleCentralTx) return false;
  TaskHandle_t holder = xSemaphoreGetMutexHolder(sBleCentralTx);
  if (!holder) return false;
  return holder != xTaskGetCurrentTaskHandle();
}

BleCentralTxGuard::BleCentralTxGuard(uint32_t timeoutMs)
    : held(bleCentralTxTake(timeoutMs)) {}

BleCentralTxGuard::~BleCentralTxGuard() {
  if (held) bleCentralTxGive();
}

bool bleCentralTxAccessoryPressure() {
  return sAccessoryTxHeld.load(std::memory_order_acquire) ||
      sAccessoryWaiters.load(std::memory_order_acquire) != 0 ||
      TickType_t(xTaskGetTickCount() - sLastAccessoryTx.load(std::memory_order_acquire)) < pdMS_TO_TICKS(20);
}
bool bleCentralTxTryPhone(bool bypassPriority) {
  bleCentralTxInit();
  if (!sBleCentralTx || (!bypassPriority && bleCentralTxAccessoryPressure())) return false;
  if (xSemaphoreTake(sBleCentralTx, 0) != pdTRUE) return false;
  if (!bypassPriority && bleCentralTxAccessoryPressure()) { xSemaphoreGive(sBleCentralTx); return false; }
  return true;
}
