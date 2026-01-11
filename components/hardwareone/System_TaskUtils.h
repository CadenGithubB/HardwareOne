#ifndef TASK_UTILS_H
#define TASK_UTILS_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
