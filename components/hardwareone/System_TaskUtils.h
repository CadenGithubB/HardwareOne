#ifndef SYSTEM_TASKUTILS_H
#define SYSTEM_TASKUTILS_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============================================================================
// Centralized Task Stack Sizes (words; 1 word = 4 bytes on ESP32)
// ============================================================================

constexpr uint32_t CMD_EXEC_STACK_WORDS = 8192;      // ~32KB (automation add validates commands via findCommand; extra headroom prevents overflow)
constexpr uint32_t SENSOR_QUEUE_STACK_WORDS = 4096;  // 16KB. Bumped from 3072 (~12KB):
                                                     // runtime task-stack reports flagged
                                                     // sensor_queue_task CRITICAL at ~79%
                                                     // peak (free ~2.5KB) with all sensors
                                                     // enabled + remote-sensor JSON. History:
                                                     // gamepadInit (seesaw.begin → SWReset →
                                                     // ~10-deep Wire1 calls) had already
                                                     // overflowed the old ~11KB budget, so
                                                     // headroom here is load-bearing.
                                                     // 12KB gives a comfortable margin
                                                     // for future per-sensor I2C work.
constexpr uint32_t ESPNOW_HB_STACK_WORDS = 6656;     // 26 KB. Bumped from 5530 (22 KB) after Step 3c
                                                     // HWM measurement on both gamepad+ANO builds showed
                                                     // peak_used 17.5 KB of 22 KB (79%) under steady-state
                                                     // bond traffic — only 4.6 KB margin, structurally
                                                     // identical on both devices so it's not transient. The
                                                     // ambient depth is RX-side (WiFi interrupt frame +
                                                     // AEAD unwrap + handler dispatch); Phase 3.4+ heavy
                                                     // crypto (Ed25519/X25519/HKDF/sign) still defers via
                                                     // submitDeferredToCmdExec → cmd_exec_task (32 KB stack).
                                                     // Step 4 (sendFileToMac chunk loop migration) may lower
                                                     // this peak further; re-measure HWM after Step 4 lands
                                                     // and consider trimming back if the post-Step-4 peak
                                                     // drops well below 17 KB. 26 KB chosen as 26×1024 = nice
                                                     // round number with ~8.5 KB margin over current HWM.
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
constexpr uint32_t SENSOR_BCAST_STACK_WORDS = 4096;  // ~16KB — MEASURED sizing. With sends
                                                     // offloaded to espnow_tx, [SENSOR_BCAST]
                                                     // HWM still showed peak_used=2464 words
                                                     // (~9.6KB of 12KB) — only ~2.4KB margin,
                                                     // too thin. The ~9.6KB is ambient depth
                                                     // (WiFi/interrupt activity landing on this
                                                     // HIGH-prio task during heavy RX), NOT the
                                                     // broadcaster's own ~1.5KB of work. 16KB
                                                     // gives ~6KB margin over the observed peak.
constexpr uint32_t ESPNOW_TX_STACK_WORDS = 5120;     // ~20KB — single dispatcher task that
                                                     // owns ALL ESP-NOW sends. The 10KB first
                                                     // guess CRASHED: the esp_now_send WiFi TX
                                                     // path (hardware LMK encryption) + AEAD
                                                     // seal + captureEspNowFrame line[700] +
                                                     // an interrupt frame landing at
                                                     // _frxt_int_exit is genuinely ~12–16KB
                                                     // deep — it's the SAME path that overflowed
                                                     // sensor_bcast at 12KB. espnow_task runs
                                                     // this exact send at 22KB and never crashes,
                                                     // so 24KB (above that proven-safe size) has
                                                     // real margin. HW-MEASURED 2026-06-02: peak_used
                                                     // held at 2728 words across taskstats + remote
                                                     // `files` streaming (dropped=0, depth_hwm=1).
                                                     // Trimmed 24KB->20KB (a conservative 4KB cut, not
                                                     // the full 8KB the measurement would allow): 5120
                                                     // leaves ~2392 words (~9.6KB, 1.88x) over peak.
                                                     // [ESPNOW_TX] HWM still logs every 10s.
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
// coreId defaults to tskNO_AFFINITY (floats across both cores); pass 0/1 to pin.
BaseType_t xTaskCreateLogged(TaskFunction_t pxTaskCode,
                              const char* pcName,
                              const uint32_t usStackDepth,
                              void* pvParameters,
                              UBaseType_t uxPriority,
                              TaskHandle_t* pxCreatedTask,
                              const char* tag,
                              BaseType_t coreId = tskNO_AFFINITY);

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

// Print the main-loop performance snapshot (laps/s, period, per-section avg,
// worst-N stalls) gathered by loopHealthTick() in HardwareOne.cpp. Used by the
// `perftop` command. Implemented in HardwareOne.cpp.
void perfPrintLoopHealth();

#endif // SYSTEM_TASKUTILS_H
