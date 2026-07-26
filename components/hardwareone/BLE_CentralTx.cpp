#include "BLE_CentralTx.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

static SemaphoreHandle_t gBleCentralTx = nullptr;

void bleCentralTxInit() {
  if (gBleCentralTx) return;
  gBleCentralTx = xSemaphoreCreateMutex();
}

bool bleCentralTxTake(uint32_t timeoutMs) {
  if (!gBleCentralTx) bleCentralTxInit();
  if (!gBleCentralTx) return false;
  TickType_t ticks = (timeoutMs == 0) ? 0 : pdMS_TO_TICKS(timeoutMs);
  return xSemaphoreTake(gBleCentralTx, ticks) == pdTRUE;
}

void bleCentralTxGive() {
  if (!gBleCentralTx) return;
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
