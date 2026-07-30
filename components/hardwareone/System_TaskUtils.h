#ifndef SYSTEM_TASKUTILS_H
#define SYSTEM_TASKUTILS_H

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ============================================================================
// Centralized Task Stack Sizes — VALUES ARE BYTES
// ============================================================================
// ESP-IDF's xTaskCreate() takes usStackDepth in BYTES, not words. This is an
// explicit IDF deviation from vanilla FreeRTOS — see
// components/freertos/FreeRTOS-Kernel/include/freertos/task.h: "BYTES. Note
// that this differs from vanilla FreeRTOS." (On this port portSTACK_TYPE is
// uint8_t, so StackType_t is 1 byte and "words" == bytes anyway.)
// uxTaskGetStackHighWaterMark() likewise returns BYTES here: prvTaskCheckFreeStackSpace
// counts fill bytes then divides by sizeof(StackType_t) — a no-op at 1.
//
// The _STACK_WORDS names below are therefore a MISNOMER kept only to avoid a
// 155-site rename; the numbers are byte counts and are passed through
// unscaled. Sanity check: these sum to ~248 KB, which fits the S3's 512 KB
// SRAM — the old "x4" reading implied ~992 KB and the device could not boot.
//
// ⚠ HISTORICAL FIGURES IN THE COMMENTS BELOW ARE 4x INFLATED. Every "~NNKB"
// note and every cited HWM/peak_used measurement was produced by the reporter
// while it multiplied by 4 (fixed 2026-07-16 in System_TaskUtils.cpp). Divide
// any cited KB figure by 4, or re-measure, before trusting it. The headline
// annotation on each constant has been corrected; the narratives have not.
// ============================================================================

constexpr uint32_t CMD_EXEC_STACK_WORDS = 8192;     // 8 KB (was annotated ~32KB — 4x wrong). Automation add validates commands via findCommand.
constexpr uint32_t SENSOR_QUEUE_STACK_WORDS = 4096;     // 4 KB (was annotated 16KB — 4x wrong). History below cites 4x-inflated HWMs:
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
constexpr uint32_t ESPNOW_HB_STACK_WORDS = 6656;     // 6.5 KB (was annotated 26 KB — 4x wrong). Bumped from 5530 B. Notes below are 4x-inflated:
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
constexpr uint32_t THERMAL_STACK_WORDS = 6144;     // 6 KB (was annotated ~24KB — 4x wrong). Notes below are 4x-inflated:
                                                     // restored from 16KB: the MLX90640
                                                     // getFrame()/CalculateTo path plus per-poll debug
                                                     // logging overflowed 16KB on ESP32-classic when
                                                     // frame reads fail (NACK retry storm). 24KB was the
                                                     // original pre-trim value.
constexpr uint32_t IMU_STACK_WORDS = 4096;     // 4 KB (was ~16KB) — BNO055 init retries need extra stack
constexpr uint32_t TOF_STACK_WORDS = 3072;     // 3 KB (was ~12KB)
constexpr uint32_t FMRADIO_STACK_WORDS = 4608;     // 4.5 KB (was ~18KB)
constexpr uint32_t INPUT_STACK_WORDS = 3584;     // 3.5 KB (was ~14KB)
constexpr uint32_t DEBUG_OUT_STACK_WORDS = 4096;     // 4 KB (was annotated 16KB — 4x wrong). ⚠ HW-OBSERVED 81% USED (~764 B free):
                                                     // The earlier 3 KB "floor" (HWM ~2.3 KB → ~0.7 KB
                                                     // headroom) was NOT enough: an [ERROR] logged
                                                     // during boot (NTP-sync timeout) sent the task
                                                     // through appendLineWithCap()->LittleFS append,
                                                     // whose deep write path + the timestamp String
                                                     // exceeded the headroom and overflowed. The HWM
                                                     // is driven entirely by debug_out doing inline
                                                     // FILE I/O (system-log + error-log writes); see
                                                     // the drain loop. Do NOT trim below this 4 KB
                                                     // unless those file writes are moved off this task.
constexpr uint32_t APDS_STACK_WORDS = 3072;     // 3 KB (was ~12KB)
constexpr uint32_t GPS_STACK_WORDS = 3072;     // 3 KB (was ~12KB)
constexpr uint32_t PRESENCE_STACK_WORDS = 3072;     // 3 KB (was ~12KB)
constexpr uint32_t RTC_STACK_WORDS = 4096;     // 4 KB (was ~16KB)
constexpr uint32_t SENSOR_BCAST_STACK_WORDS = 4096;     // 4 KB (was annotated ~16KB — 4x wrong). Notes below are 4x-inflated:
                                                     // MEASURED sizing. With sends
                                                     // offloaded to espnow_tx, [SENSOR_BCAST]
                                                     // HWM still showed peak_used=2464 words
                                                     // (~9.6KB of 12KB) — only ~2.4KB margin,
                                                     // too thin. The ~9.6KB is ambient depth
                                                     // (WiFi/interrupt activity landing on this
                                                     // HIGH-prio task during heavy RX), NOT the
                                                     // broadcaster's own ~1.5KB of work. 16KB
                                                     // gives ~6KB margin over the observed peak.
constexpr uint32_t ESPNOW_TX_STACK_WORDS = 5120;     // 5 KB (was annotated ~20KB — 4x wrong). Notes below are 4x-inflated:
                                                     // single dispatcher task that
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
constexpr uint32_t MIC_RECORD_STACK_WORDS = 4096;     // 4 KB (was ~16KB) — microphone recording
constexpr uint32_t MIC_VIZ_STACK_WORDS = 4096;     // 4 KB (was ~16KB) — microphone visualizer
constexpr uint32_t SR_STACK_WORDS = 8192;     // 8 KB (was ~32KB) — speech recognition inference
constexpr uint32_t SR_SNIP_STACK_WORDS = 4096;     // 4 KB (was ~16KB) — speech recognition snippet writer
constexpr uint32_t EI_CONTINUOUS_STACK_WORDS = 8192;     // 8 KB (was ~32KB) — EdgeImpulse continuous inference
constexpr uint32_t MAP_RENDER_STACK_WORDS = 8192;     // 8 KB (was ~32KB) — async map rendering
constexpr uint32_t LLM_VIEW_STACK_WORDS   = 6144;     // 6 KB (was annotated ~24KB — 4x wrong). Sized for return-to-Apps onDone:
                                                     // the return-to-Apps onDone it runs on its own stack: g2ShowAppsMenu
                                                     // does an FS scan + deep vsnprintf logging (~16-18KB observed; an
                                                     // undersized 16KB worker overflowed on Back). Kept under the map's
                                                     // 32KB because this DRAM-fragmented config often has no 32KB block.

// Centralized task priorities
// LOW  (1) — sensor pollers, debug output, RTC, map render
// NORMAL (3) — auxiliary media tasks (mic visualizer, SR snip writer)
// HIGH (5) — real-time data paths (ESP-NOW, mic recording, SR inference)
constexpr UBaseType_t TASK_PRIORITY_LOW    = 1;
constexpr UBaseType_t TASK_PRIORITY_NORMAL = 3;
constexpr UBaseType_t TASK_PRIORITY_HIGH   = 5;
constexpr UBaseType_t SR_TASK_PRIORITY_LEVEL = TASK_PRIORITY_HIGH;  // kept for backward compat

// Named cores for the placement policy documented below.
//   PRO_CORE (0) — Wi-Fi / BLE / ESP-NOW radio + I/O paths live here.
//   APP_CORE (1) — compute/render and all I2C work run here.
constexpr BaseType_t PRO_CORE = 0;
constexpr BaseType_t APP_CORE = 1;

// Core that every I2C-touching task pins to (APP core). Core 0 is saturated by
// the Wi-Fi stack + ESP-NOW; a task that floats onto Core 0 and gets starved
// mid-I2C-transaction lets the legacy driver's bus-recovery path storm the bus →
// panic(4) / INT-WDT. Core 1 carries no OS-owned work in this build, so the
// transaction finishes before its timeout. Full rationale (and the FeatherS3 out-and-about crash loop it fixed,
// docs/NewCapture 2026-07-22) is at the sensor tasks in System_TaskUtils.cpp.
// Shared here so the sensor-queue processor — created in HardwareOne.cpp /
// System_I2C.cpp, and which runs the I2C device-init transactions — pins too.
constexpr BaseType_t I2C_SENSOR_CORE = APP_CORE;

// ============================================================================
// TASK CORE-PLACEMENT POLICY  (ESP32-S3, dual core — read before creating tasks)
// ============================================================================
// Core 0 (PRO) is saturated by the Wi-Fi stack, the BLE controller, and the
// ESP-NOW callback/heartbeat — AND, in this build, the main task too:
// sdkconfig sets CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0=y, and
// CONFIG_AUTOSTART_ARDUINO is unset so Arduino's loopTask does not exist at
// all (CONFIG_ARDUINO_RUNNING_CORE=1 is inert without it). Core 1 (APP)
// therefore carries only what we pin there.
// CONSEQUENCE for reasoning about races: the main-loop tick runs on Core 0
// alongside the radio callbacks, so "the loop is on the other core" is NOT a
// reason two paths can't interleave. Placement rule by task class:
//
//   Core 1 (APP) — pin here:
//     * ANY task that touches the shared I2C/Wire bus (use I2C_SENSOR_CORE).
//       This is the hard rule — no exceptions. An unpinned I2C task that floats
//       onto the saturated Core 0 and is preempted mid-transaction storms the
//       bus → panic(4). This is what crash-looped the FeatherS3 out-and-about.
//     * CPU-bound compute/render (SR inference, map/image render) — keep it off
//       the radio core so it can't crowd Wi-Fi/BLE.
//   Core 0 (PRO) — pin here:
//     * BLE-reply / command I/O (cmd_exec) and serial/log I/O, and the ESP-NOW
//       real-time path — they belong with the radio + I/O they serve.
//   Float (tskNO_AFFINITY) — allowed ONLY for short-lived workers that touch no
//     shared bus and aren't latency-sensitive, AND only as a *documented* choice.
//
// Verifying placement/load: `perftop` prints per-core headroom as the IDLE0 /
// IDLE1 rows (Core N load ≈ 100 − IDLEn%) plus per-task CPU%. Measure the worst
// case (offline + GPS + logging + camera/BLE) before and after any change; keep
// comfortable IDLE1 headroom so pinned I2C work on Core 1 can't itself starve.
// ============================================================================

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
// allocatedBytes: the byte count passed to xTaskCreate (see the BYTES note at
// the top of this header — the *_STACK_WORDS constants are byte counts).
void reportTaskStack(TaskHandle_t handle, const char* name, uint32_t allocatedBytes);

// Report all sensor task stacks plus system tasks
void reportAllTaskStacks();

// Print the main-loop performance snapshot (laps/s, period, per-section avg,
// worst-N stalls) gathered by loopHealthTick() in HardwareOne.cpp. Used by the
// `perftop` command. Implemented in HardwareOne.cpp.
void perfPrintLoopHealth();

// Struct-read form of the same snapshot for on-device renderers (OLED perf
// screen) — no text parsing. False while the first 5 s window accumulates.
// Implemented in HardwareOne.cpp.
bool perfGetLoopSnapshot(uint32_t& lapsPerSec, uint32_t& avgMs, uint32_t& maxMs,
                         uint32_t& stalls5s, uint32_t& totalStalls);

#endif // SYSTEM_TASKUTILS_H
