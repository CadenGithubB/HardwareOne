#ifndef TASK_UTILS_H
#define TASK_UTILS_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============================================================================
// Centralized Task Stack Sizes (words; 1 word = 4 bytes on ESP32)
// ============================================================================

constexpr uint32_t CMD_EXEC_STACK_WORDS = 6144;      // ~24KB (automation run + debug vsnprintf frames need deep stack)
constexpr uint32_t SENSOR_QUEUE_STACK_WORDS = 2765;  // ~11KB - reduced 10%
constexpr uint32_t ESPNOW_HB_STACK_WORDS = 5530;     // ~22KB (mesh processing + debug logging + multi-peer scaling) - reduced 10%
constexpr uint32_t THERMAL_STACK_WORDS = 4096;       // ~16KB
constexpr uint32_t IMU_STACK_WORDS = 4096;           // ~16KB (BNO055 init retries need extra stack)
constexpr uint32_t TOF_STACK_WORDS = 3072;           // ~12KB
constexpr uint32_t FMRADIO_STACK_WORDS = 4608;       // ~18KB
constexpr uint32_t GAMEPAD_STACK_WORDS = 3584;       // ~14KB
constexpr uint32_t DEBUG_OUT_STACK_WORDS = 3072;     // ~12KB
constexpr uint32_t APDS_STACK_WORDS = 3072;          // ~12KB
constexpr uint32_t GPS_STACK_WORDS = 3072;           // ~12KB
constexpr uint32_t PRESENCE_STACK_WORDS = 3072;      // ~12KB
constexpr uint32_t RTC_STACK_WORDS = 4096;           // ~16KB
constexpr uint32_t SENSOR_BCAST_STACK_WORDS = 3072;  // ~12KB (ESP-NOW sensor broadcaster)
constexpr uint32_t MIC_RECORD_STACK_WORDS = 4096;    // ~16KB (microphone recording)
constexpr uint32_t MIC_VIZ_STACK_WORDS = 4096;       // ~16KB (microphone visualizer)
constexpr uint32_t SR_STACK_WORDS = 8192;             // ~32KB (speech recognition inference)
constexpr uint32_t SR_SNIP_STACK_WORDS = 4096;        // ~16KB (speech recognition snippet writer)
constexpr uint32_t EI_CONTINUOUS_STACK_WORDS = 8192;  // ~32KB (EdgeImpulse continuous inference)
constexpr uint32_t MAP_RENDER_STACK_WORDS = 8192;     // ~32KB (async map rendering)

// Centralized task priorities
// LOW  (1) — sensor pollers, debug output, RTC, map render
// NORMAL (3) — auxiliary media tasks (mic visualizer, SR snip writer)
// HIGH (5) — real-time data paths (ESP-NOW, mic recording, SR inference)
constexpr UBaseType_t TASK_PRIORITY_LOW    = 1;
constexpr UBaseType_t TASK_PRIORITY_NORMAL = 3;
constexpr UBaseType_t TASK_PRIORITY_HIGH   = 5;
constexpr UBaseType_t SR_TASK_PRIORITY_LEVEL = TASK_PRIORITY_HIGH;  // kept for backward compat

// ============================================================================
// FreeRTOS Task Creation with Memory Logging
// ============================================================================

// Create a FreeRTOS task with heap/PSRAM delta tracking
// Logs allocation info to system log when filesystem is ready
BaseType_t xTaskCreateLogged(TaskFunction_t pxTaskCode,
                              const char* pcName,
                              const uint32_t usStackDepth,
                              void* pvParameters,
                              UBaseType_t uxPriority,
                              TaskHandle_t* pxCreatedTask,
                              const char* tag);

// ============================================================================
// Sensor Task Creation Helpers
// ============================================================================

// Create sensor-specific tasks (implementations call xTaskCreateLogged)
bool createGamepadTask();
bool createThermalTask();
bool createIMUTask();
bool createToFTask();
bool createFMRadioTask();

// ============================================================================
// Sensor Task Exit Helper
// ============================================================================

// Emit the standard "task disabled" log message and self-delete the task.
// tag must be a string literal, e.g. SENSOR_TASK_EXIT("IMU").
// NOTE: do NOT clear the task handle before calling this — the create/start
// function uses eTaskGetState() to detect whether the task is still running.
#define SENSOR_TASK_EXIT(tag)                                                  \
  INFO_SENSORSF("[" tag "] Task disabled - cleaning up and deleting");         \
  vTaskDelete(nullptr)

// ============================================================================
// Automated Stack Watermark Monitoring
// ============================================================================

// Report stack usage for a single task
void reportTaskStack(TaskHandle_t handle, const char* name, uint32_t allocatedWords);

// Report all sensor task stacks plus system tasks
void reportAllTaskStacks();

#endif // TASK_UTILS_H
