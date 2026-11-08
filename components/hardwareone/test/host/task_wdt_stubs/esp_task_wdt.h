#pragma once
// Host stand-in for the three task-watchdog calls System_TaskWdt.h makes.
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NOT_FOUND 0x105
esp_err_t esp_task_wdt_add(void* task);
esp_err_t esp_task_wdt_status(void* task);
esp_err_t esp_task_wdt_reset(void);
