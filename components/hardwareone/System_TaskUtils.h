#ifndef SYSTEM_TASKUTILS_H
#define SYSTEM_TASKUTILS_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============================================================================
// Centralized Task Stack Sizes (words; 1 word = 4 bytes on ESP32)
// ============================================================================

constexpr uint32_t CMD_EXEC_STACK_WORDS = 8192;      // ~32KB (automation add validates commands via findCommand; extra headroom prevents overflow)
constexpr uint32_t SENSOR_QUEUE_STACK_WORDS = 3072;  // ~12KB (bumped back from
                                                     // the 2765 "reduced 10%" value.
                                                     // gamepadInit (seesaw.begin →
                                                     // SWReset → ~10-deep Wire1 calls)
                                                     // was overflowing the 11KB budget
                                                     // when the dual-bus refactor's
                                                     // per-bus locals in executeTransaction
                                                     // (uint8_t bus + SemaphoreHandle_t
                                                     // mutex) pushed the peak by ~8 B.
                                                     // 12KB gives a comfortable margin
                                                     // for future per-sensor I2C work.
constexpr uint32_t ESPNOW_HB_STACK_WORDS = 5530;     // ~22KB. Phase 3.4+ Ed25519/X25519 work is NOT done here:
                                                     // SESSION_OPEN/CONFIRM RX handlers PSRAM-copy the payload and
                                                     // submitDeferredToCmdExec() the verify→keygen→ECDH→KDF→sign chain
                                                     // onto cmd_exec_task (24KB stack, single-threaded, HWM ~17KB →
                                                     // crypto peak ~5KB fits in existing headroom). Keeping this task
                                                     // small (~22KB) saves DRAM; do not bump back up without first
                                                     // checking whether the heavy work has been re-inlined.
constexpr uint32_t THERMAL_STACK_WORDS = 4096;       // ~16KB
constexpr uint32_t IMU_STACK_WORDS = 4096;           // ~16KB (BNO055 init retries need extra stack)
constexpr uint32_t TOF_STACK_WORDS = 3072;           // ~12KB
constexpr uint32_t FMRADIO_STACK_WORDS = 4608;       // ~18KB
constexpr uint32_t INPUT_STACK_WORDS = 3584;       // ~14KB
constexpr uint32_t DEBUG_OUT_STACK_WORDS = 3584;     // ~14KB — was 16KB, trimmed 2KB for
                                                     // DRAM-pressure relief. Boot HWM is
                                                     // ~9KB → ~5KB headroom over peak burst
                                                     // (LittleFS append + String work in
                                                     // [ERROR] flood). Don't drop below 12KB
                                                     // without reproducing the prior crash.
constexpr uint32_t APDS_STACK_WORDS = 3072;          // ~12KB
constexpr uint32_t GPS_STACK_WORDS = 3072;           // ~12KB
constexpr uint32_t PRESENCE_STACK_WORDS = 3072;      // ~12KB
constexpr uint32_t RTC_STACK_WORDS = 4096;           // ~16KB
constexpr uint32_t SENSOR_BCAST_STACK_WORDS = 6144;  // ~24KB — bumped from 12KB after first
                                                     // bond-encrypted sensor-stream transmit
                                                     // overflowed sensor_bcast (canary stomp).
                                                     // Dial back to (measured HWM + ~1KB) once
                                                     // [SENSOR_BCAST] stack peak_used has been
                                                     // observed under the worst-case path
                                                     // (bond-worker AEAD send + WiFi ISR).
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
bool createInputTask();
bool createThermalTask();
bool createIMUTask();
bool createToFTask();
bool createFMRadioTask();
bool createAPDSTask();
bool createPresenceTask();
bool createGPSTask();
bool createRTCTask();

// ============================================================================
// Sensor Task Exit Helper
// ============================================================================

// Emit the standard "task disabled" log message and self-delete the task.
// `subsys` must match one of the per-subsystem INFO_*_LIFECYCLEF macros — e.g.
// SENSOR_TASK_EXIT(IMU) expands to INFO_IMU_LIFECYCLEF(...).
// Routes through the Lifecycle sub-flag (added in Option β) so the user can
// suppress task-exit chatter via the per-sensor Lifecycle toggle without
// touching the master flag.
// NOTE: do NOT clear the task handle before calling this — the create/start
// function uses eTaskGetState() to detect whether the task is still running.
#define SENSOR_TASK_EXIT(subsys)                                               \
  INFO_##subsys##_LIFECYCLEF("Task disabled - cleaning up and deleting");      \
  vTaskDelete(nullptr)

// ============================================================================
// Automated Stack Watermark Monitoring
// ============================================================================

// Report stack usage for a single task
void reportTaskStack(TaskHandle_t handle, const char* name, uint32_t allocatedWords);

// Report all sensor task stacks plus system tasks
void reportAllTaskStacks();

#endif // SYSTEM_TASKUTILS_H
