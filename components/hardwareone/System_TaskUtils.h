#ifndef TASK_UTILS_H
#define TASK_UTILS_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============================================================================
// Centralized Task Stack Sizes (words; 1 word = 4 bytes on ESP32)
// ============================================================================

constexpr uint32_t CMD_EXEC_STACK_WORDS = 5120;      // ~20KB (NTP/DNS/file I/O)
constexpr uint32_t SENSOR_QUEUE_STACK_WORDS = 3072;  // ~12KB
constexpr uint32_t ESPNOW_HB_STACK_WORDS = 6144;     // ~24KB (mesh processing + debug logging + multi-peer scaling)
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
// Automated Stack Watermark Monitoring
// ============================================================================

// Report stack usage for a single task
void reportTaskStack(TaskHandle_t handle, const char* name, uint32_t allocatedWords);

// Report all sensor task stacks plus system tasks
void reportAllTaskStacks();

#endif // TASK_UTILS_H
