#pragma once
// Task-watchdog soak support.
//
// Ordinary builds: everything here is a no-op, and taskWdtAwareSemaphoreTake()
// is a plain xSemaphoreTake(). The watchdog keeps its SDK defaults (idle-task
// checks, print only; OTA layouts panic at 5 s, set in sdkconfig.ota.defaults).
//
// Soak builds (tools/build_board.sh <board> --wdt-soak, which defines
// HW1_WDT_SOAK=1 for the hardwareone and main components and layers
// config/sdkconfig.wdt_soak.defaults): the main loop task is subscribed after
// setup() and fed once per loop pass, and a trip panics. A long but bounded
// wait on the main task (a serial, UART-link or OLED command waiting up to
// 60 s for cmd_exec) feeds the watchdog between short slices so it is not
// mistaken for a hang. Nothing else is subscribed.

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#ifndef HW1_WDT_SOAK
#define HW1_WDT_SOAK 0
#endif

#if HW1_WDT_SOAK
#include "sdkconfig.h"
#include "esp_task_wdt.h"
#if !CONFIG_ESP_TASK_WDT_PANIC
#error "HW1_WDT_SOAK needs config/sdkconfig.wdt_soak.defaults (CONFIG_ESP_TASK_WDT_PANIC=y)"
#endif
#endif

// Longest a watched wait goes without feeding the watchdog.
constexpr uint32_t kTaskWdtWaitSliceMs = 1000;

// Reset the watchdog if the calling task is subscribed; otherwise nothing.
inline void taskWdtFeed() {
#if HW1_WDT_SOAK
  if (esp_task_wdt_status(nullptr) == ESP_OK) (void)esp_task_wdt_reset();
#endif
}

// Subscribe the calling task (the main loop). Returns true when subscribed.
// Ordinary builds: false, nothing changes.
inline bool taskWdtSubscribeCurrent() {
#if HW1_WDT_SOAK
  const esp_err_t err = esp_task_wdt_add(nullptr);
  return err == ESP_OK || esp_task_wdt_status(nullptr) == ESP_OK;
#else
  return false;
#endif
}

// xSemaphoreTake with the same total timeout, fed in kTaskWdtWaitSliceMs
// slices when the caller is subscribed. Returns pdTRUE as soon as the
// semaphore is taken, pdFALSE after timeoutMs, exactly as before.
inline BaseType_t taskWdtAwareSemaphoreTake(SemaphoreHandle_t sem, uint32_t timeoutMs) {
#if HW1_WDT_SOAK
  if (esp_task_wdt_status(nullptr) == ESP_OK) {
    const TickType_t total = pdMS_TO_TICKS(timeoutMs);
    const TickType_t slice = pdMS_TO_TICKS(kTaskWdtWaitSliceMs);
    const TickType_t start = xTaskGetTickCount();
    for (;;) {
      const TickType_t elapsed = xTaskGetTickCount() - start;
      const TickType_t left = elapsed < total ? total - elapsed : 0;
      if (xSemaphoreTake(sem, left < slice ? left : slice) == pdTRUE) return pdTRUE;
      if (left <= slice) return pdFALSE;
      (void)esp_task_wdt_reset();
    }
  }
#endif
  return xSemaphoreTake(sem, pdMS_TO_TICKS(timeoutMs));
}
