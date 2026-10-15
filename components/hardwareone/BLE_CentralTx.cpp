#include "BLE_CentralTx.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>

static SemaphoreHandle_t gBleCentralTx = nullptr;
static StaticSemaphore_t gBleCentralTxStorage;
static portMUX_TYPE gBleCentralTxInitMux = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<unsigned> gAccessoryWaiters{0};
static std::atomic<bool> gAccessoryTxHeld{false};
static std::atomic<TickType_t> gLastAccessoryTx{0};

void bleCentralTxInit() {
  portENTER_CRITICAL(&gBleCentralTxInitMux);
  if (!gBleCentralTx) gBleCentralTx = xSemaphoreCreateMutexStatic(&gBleCentralTxStorage);
  portEXIT_CRITICAL(&gBleCentralTxInitMux);
}

bool bleCentralTxTake(uint32_t timeoutMs) {
  if (!gBleCentralTx) bleCentralTxInit();
  if (!gBleCentralTx) return false;
  TickType_t ticks = (timeoutMs == 0) ? 0 : pdMS_TO_TICKS(timeoutMs);
  ++gAccessoryWaiters;
  const bool held = xSemaphoreTake(gBleCentralTx, ticks) == pdTRUE;
  --gAccessoryWaiters;
  if (held) {
    gAccessoryTxHeld.store(true, std::memory_order_release);
    gLastAccessoryTx.store(xTaskGetTickCount(), std::memory_order_release);
  }
  return held;
}

void bleCentralTxGive() {
  if (!gBleCentralTx) return;
  if (gAccessoryTxHeld.exchange(false, std::memory_order_acq_rel))
    gLastAccessoryTx.store(xTaskGetTickCount(), std::memory_order_release);
  xSemaphoreGive(gBleCentralTx);
}

bool bleCentralTxIsHeld() {
  if (!gBleCentralTx) return false;
  return xSemaphoreGetMutexHolder(gBleCentralTx) != nullptr;
}

bool bleCentralTxIsHeldByOther() {
  if (!gBleCentralTx) return false;
  TaskHandle_t holder = xSemaphoreGetMutexHolder(gBleCentralTx);
  if (!holder) return false;
  return holder != xTaskGetCurrentTaskHandle();
}

BleCentralTxGuard::BleCentralTxGuard(uint32_t timeoutMs)
    : held(bleCentralTxTake(timeoutMs)) {}

BleCentralTxGuard::~BleCentralTxGuard() {
  if (held) bleCentralTxGive();
}

bool bleCentralTxAccessoryPressure() {
  return gAccessoryTxHeld.load(std::memory_order_acquire) ||
      gAccessoryWaiters.load(std::memory_order_acquire) != 0 ||
      TickType_t(xTaskGetTickCount() - gLastAccessoryTx.load(std::memory_order_acquire)) < pdMS_TO_TICKS(20);
}
bool bleCentralTxTryPhone() {
  bleCentralTxInit();
  if (!gBleCentralTx || bleCentralTxAccessoryPressure()) return false;
  if (xSemaphoreTake(gBleCentralTx, 0) != pdTRUE) return false;
  if (bleCentralTxAccessoryPressure()) { xSemaphoreGive(gBleCentralTx); return false; }
  return true;
}
