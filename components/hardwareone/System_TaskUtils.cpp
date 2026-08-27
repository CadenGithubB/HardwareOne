#include "System_TaskUtils.h"
#include "System_Filesystem.h"    // For filesystemReady, isFsLockedByCurrentTask
#include "System_Mutex.h"  // For isFsLockedByCurrentTask
#include "System_Debug.h"  // For DEBUG_CLIF macro
#include "System_MemUtil.h"      // For ps_alloc, AllocPref
#include "System_ESPNow.h"       // Safe espnow_task stack snapshot
#include "System_ESPNow_Tx.h"    // Safe espnow_tx stack snapshot
#include "System_I2C.h"          // For queueProcessorTask
#include "HAL_Input.h"           // For INPUT_TASK_NAME / INPUT_TASK_TAG
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>      // task stack-size registry mutex
#include <esp_heap_caps.h>
#include <esp_attr.h>             // EXT_RAM_BSS_ATTR (task stack-size registry)
#include <cassert>                // ISR-context guard on the registry lock
#include <cstdio>                 // snprintf (per-interval CPU% formatting)
#include <cstring>                // strncmp/strlcpy (task stack-size registry)

// Some ESP-IDF configurations do not provide uxTaskGetSystemState (runtime
// stats disabled). Provide a weak stub so diagnostic commands still link. If
// the FreeRTOS library supplies a real implementation, it will override this.
extern "C" __attribute__((weak)) UBaseType_t uxTaskGetSystemState(
    TaskStatus_t* pxTaskStatusArray,
    UBaseType_t uxArraySize,
    uint32_t* pulTotalRunTime) {
  (void)pxTaskStatusArray;
  (void)uxArraySize;
  if (pulTotalRunTime) {
    *pulTotalRunTime = 0;
  }
  return 0;
}

// Forward declaration (implemented in HardwareOne.ino)
bool appendLineWithCap(const char* path, const String& line, size_t capBytes);

// External timestamp function
extern void getTimestampPrefixMsCached(char* buf, size_t bufSize);

// Per-sensor TaskHandle_t and enabled-flag externs come from the sensor
// headers (each header gates its decls on ENABLE_*_SENSOR; System_SensorStubs.h
// supplies stubs for sensors compiled out).
#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor_seesaw.h"
#endif
#if ENABLE_THERMAL_SENSOR
#include "i2csensor_mlx90640.h"
#endif
#if ENABLE_IMU_SENSOR
#include "i2csensor_bno055.h"
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor_vl53l4cx.h"
#endif
#if ENABLE_FM_RADIO
#include "i2csensor_rda5807.h"
#endif
#if ENABLE_GPS_SENSOR
#include "i2csensor_pa1010d.h"
#endif
#if ENABLE_APDS_SENSOR
#include "i2csensor_apds9960.h"
#endif
#if ENABLE_PRESENCE_SENSOR
#include "i2csensor_sths34pf80.h"
#endif
#if ENABLE_RTC_SENSOR
#include "i2csensor_ds3231.h"
#endif

// gCmdExecTaskHandle is not a sensor — declared separately.
extern TaskHandle_t gCmdExecTaskHandle;

// Per-sensor task functions (each defined in its own .cpp module).
extern void inputTask(void* parameter);
extern void thermalTask(void* parameter);
extern void imuTask(void* parameter);
extern void tofTask(void* parameter);
extern void fmRadioTask(void* parameter);
extern void apdsTask(void* parameter);
extern void presenceTask(void* parameter);
extern void gpsTask(void* parameter);
extern void rtcTask(void* parameter);

// ============================================================================
// Task Stack-Size Registry
// ============================================================================
// See the contract in System_TaskUtils.h. Append-only: entries are never
// removed, because a re-created task reuses its name and simply overwrites its
// size in place.
//
// Sizing: ~40 distinct names register today (20 direct taskStackRecord sites +
// ~19 through xTaskCreateLogged). Board profiles gate many of them, but the
// registry is append-only across a boot, so budget for the union, not the
// live set. 56 slots ≈ 1.1 KB of .bss. Overflow drops the newest entries, which
// degrades to "size unknown" rather than misreporting a wrong total.
static constexpr size_t TASK_STACK_REG_MAX = 56;

struct TaskStackEntry {
  char     name[configMAX_TASK_NAME_LEN];
  uint32_t bytes;
};

// PSRAM: cold diagnostic data, touched ~40 times per boot at task-creation
// time and by taskstats. Guarded by a FreeRTOS mutex (below), NOT a spinlock —
// strncmp'ing up to 56 entries of PSRAM under taskENTER_CRITICAL would put
// cache-missing reads under masked interrupts. The 2026-08-19 44-site caller
// sweep found every caller is plain task context (each already blocks on the
// heap mutex via xTaskCreate*), so a blocking take is legal everywhere.
EXT_RAM_BSS_ATTR static TaskStackEntry sTaskStackReg[TASK_STACK_REG_MAX];
static size_t sTaskStackRegCount = 0;

// Kernel object storage — must stay INTERNAL (Static*_t refusal rule), which
// is also what makes the mutex usable from the first statement of app_main:
// otaSafetyInitEarly() registers "ota_probation" there on OTA-verify boots,
// before initArduino(). The Meyers static makes first-create race-free
// (__cxa_guard) with no heap dependency.
static StaticSemaphore_t sTaskStackRegMuxStorage;
static SemaphoreHandle_t taskStackRegMutex() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutexStatic(&sTaskStackRegMuxStorage);
  return m;
}

// Leaf lock: never call out (log/broadcast/alloc) while holding it — the
// esp_timer task can reach taskStackRecord via g2EnqueueLensJob→pageSwapInit,
// so anything slow or re-entrant under this lock stalls every esp_timer in
// the firmware. ISR context is illegal for a mutex, hence the loud assert
// (today's callers are all task-context; this catches a future regression).
// Pre-scheduler the system is single-threaded, so proceeding unlocked is safe.
static bool taskStackRegLock() {
  assert(!xPortInIsrContext());
  if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) return false;
  xSemaphoreTake(taskStackRegMutex(), portMAX_DELAY);
  return true;
}
static void taskStackRegUnlock(bool locked) {
  if (locked) xSemaphoreGive(taskStackRegMutex());
}

// Names are copied under sizeof(name) == configMAX_TASK_NAME_LEN, the same
// bound prvInitialiseNewTask() uses for the TCB copy, so a name the kernel
// truncated truncates identically here and still compares equal.
void taskStackRecord(const char* name, uint32_t allocatedBytes) {
  if (!name || !name[0] || allocatedBytes == 0) return;

  const bool locked = taskStackRegLock();
  for (size_t i = 0; i < sTaskStackRegCount; i++) {
    if (strncmp(sTaskStackReg[i].name, name, configMAX_TASK_NAME_LEN - 1) == 0) {
      sTaskStackReg[i].bytes = allocatedBytes;  // re-created, possibly resized
      taskStackRegUnlock(locked);
      return;
    }
  }
  if (sTaskStackRegCount < TASK_STACK_REG_MAX) {
    TaskStackEntry& e = sTaskStackReg[sTaskStackRegCount];
    strlcpy(e.name, name, sizeof(e.name));
    e.bytes = allocatedBytes;
    sTaskStackRegCount++;
  }
  taskStackRegUnlock(locked);
}

uint32_t taskStackLookup(const char* name) {
  if (!name || !name[0]) return 0;

  uint32_t bytes = 0;
  const bool locked = taskStackRegLock();
  for (size_t i = 0; i < sTaskStackRegCount; i++) {
    if (strncmp(sTaskStackReg[i].name, name, configMAX_TASK_NAME_LEN - 1) == 0) {
      bytes = sTaskStackReg[i].bytes;
      break;
    }
  }
  taskStackRegUnlock(locked);
  return bytes;
}

// ============================================================================
// Live-heap measurement of a task snapshot (see System_TaskUtils.h)
// ============================================================================

// The walker below equates a TLSF block's start (what tlsf_walk_pool hands
// heap_caps_walk) with the pointer the kernel holds in pxStackBase / xHandle.
// That is only true while nothing sits between the block header and the user
// data: heap poisoning prepends a poison_head_t and task tracking prepends
// the owner handle (heap_caps_base.c MULTI_HEAP_ADD_BLOCK_OWNER_OFFSET). No
// board profile in this repo sets either, and the failure mode would be
// SILENT — every task reads '?', both reports sum zero — so refuse to build
// rather than guess an offset nobody has tested here.
#if CONFIG_HEAP_POISONING_LIGHT || CONFIG_HEAP_POISONING_COMPREHENSIVE || CONFIG_HEAP_TASK_TRACKING
#error "taskHeapMeasureSnapshot(): heap poisoning / task tracking shift user pointers off the TLSF block start; add the offset before enabling"
#endif

namespace {

struct TaskHeapWalkCtx {
  const TaskStatus_t* snapshot;
  TaskHeapMeasure*    out;
  size_t              count;
};

// One call per block per heap, under that heap's lock (multi_heap_walk takes
// portENTER_CRITICAL on the heap's portMUX), so this stays a pointer compare
// loop: no logging, no allocation, no registry lookups in here.
//
// THE CAP SET IS LOAD-BEARING. The caller walks MALLOC_CAP_INTERNAL |
// MALLOC_CAP_8BIT — the same set hw1InternalTotalBytes()/FreeBytes() describe.
// Never MALLOC_CAP_INVALID: that means "every heap" (heap_caps.c:600) and would
// let a PSRAM or RTCRAM block be measured and folded into a DRAM-only total.
// CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY and
// CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM are set on every board profile, so
// a PSRAM stack is one xTaskCreateWithCaps away; with the narrow set it simply
// stays 0 (unmeasured) and the caller reports it rather than mis-adding it.
bool taskHeapWalker(walker_heap_into_t heap_info, walker_block_info_t block, void* user) {
  (void)heap_info;
  if (!block.used) return true;
  const TaskHeapWalkCtx* ctx = static_cast<const TaskHeapWalkCtx*>(user);
  for (size_t i = 0; i < ctx->count; i++) {
    if (block.ptr == (const void*)ctx->snapshot[i].pxStackBase) {
      ctx->out[i].stackBytes = (uint32_t)block.size;
    }
    if (block.ptr == (const void*)ctx->snapshot[i].xHandle) {
      ctx->out[i].tcbBytes = (uint32_t)block.size;
    }
  }
  return true;
}

}  // namespace

void taskHeapMeasureSnapshot(const TaskStatus_t* snapshot, TaskHeapMeasure* out, size_t count) {
  if (!snapshot || !out || count == 0) return;
  for (size_t i = 0; i < count; i++) {
    out[i].stackBytes = 0;
    out[i].tcbBytes   = 0;
    out[i].regBytes   = 0;
  }
  // Walk FIRST: the pointer reads are the time-sensitive part (a task that
  // deletes itself after the snapshot has its blocks freed by IDLE), so they
  // go before the N mutex-guarded registry lookups, not after.
  TaskHeapWalkCtx ctx{ snapshot, out, count };
  heap_caps_walk(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT, taskHeapWalker, &ctx);
  for (size_t i = 0; i < count; i++) {
    // pcTaskName points INTO the TCB (&pxTCB->pcTaskName[0]). The address
    // guard is the one reportAllTaskStacks() has always used against a
    // snapshot row whose TCB was torn down after uxTaskGetSystemState().
    const char* name = snapshot[i].pcTaskName;
    out[i].regBytes = (name && (uintptr_t)name >= 0x3F000000) ? taskStackLookup(name) : 0;
  }
}

// ============================================================================
// Task Creation with Memory Logging
// ============================================================================

BaseType_t xTaskCreateLogged(TaskFunction_t pxTaskCode,
                              const char* pcName,
                              const uint32_t usStackDepth,
                              void* pvParameters,
                              UBaseType_t uxPriority,
                              TaskHandle_t* pxCreatedTask,
                              const char* tag,
                              BaseType_t coreId) {
  // Measure before
  size_t heapBefore = ESP.getFreeHeap();
  size_t psTot = ESP.getPsramSize();
  size_t psBefore = (psTot > 0) ? ESP.getFreePsram() : 0;
  size_t internalBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  // Record the total before creating, so `taskstats --json` can report stack
  // size alongside the high-water mark (FreeRTOS cannot recover it later).
  taskStackRecord(pcName, usStackDepth);

  BaseType_t res = xTaskCreatePinnedToCore(pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxCreatedTask, coreId);

  size_t internalAfter = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t largestAfter = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

  // Durable, always-on record of task-create FAILURES only. A task that fails
  // to spawn (heap/PSRAM exhaustion) means its whole feature silently never
  // runs — the canonical "it just stopped working" divergence. Successes are
  // deliberately NOT logged here: ~20 tasks spawn every boot and the useful
  // "it came up" signal lives one level up (per-sensor/subsystem online events),
  // while per-task memory telemetry is the DEBUG_MEMORY block below.
  if (res != pdPASS) {
    size_t psFree = (psTot > 0) ? ESP.getFreePsram() : 0;
    logSystemEvent("TASK", "failed to create '%s'%s%s (internal=%u largest=%u psram=%u) — feature unavailable this boot",
                   pcName ? pcName : "?",
                   (tag && tag[0]) ? " tag=" : "", (tag && tag[0]) ? tag : "",
                   (unsigned)internalAfter, (unsigned)largestAfter, (unsigned)psFree);
  }

  // Optionally log (only when FS is ready and not inside FS critical section)
  if (filesystemReady && !isFsLockedByCurrentTask() && isDebugFlagSet(DEBUG_MEMORY)) {
    char tsPrefix[40];
    getTimestampPrefixMsCached(tsPrefix, sizeof(tsPrefix));
    bool ok = (tsPrefix[0] == '[');
    if (ok) {
      for (size_t i = 1; tsPrefix[i] && i < sizeof(tsPrefix); ++i) {
        if (tsPrefix[i] == ']') {
          ok = true;
          break;
        }
        if (i == sizeof(tsPrefix) - 1) ok = false;
      }
    }
    String prefix = ok ? String(tsPrefix) : String("[BOOTING] | ");

    size_t heapAfter = ESP.getFreeHeap();
    size_t psAfter = (psTot > 0) ? ESP.getFreePsram() : 0;
    long heapDelta = (long)heapBefore - (long)heapAfter;
    long psDelta = (long)psBefore - (long)psAfter;

    String line;
    line.reserve(220);
    line += prefix;
    line += "ms=";
    line += String(millis());
    line += " op=task.create name=";
    line += (pcName ? pcName : "?");
    if (tag && tag[0]) {
      line += " tag=";
      line += tag;
    }
    // ESP-IDF's xTaskCreate takes usStackDepth in BYTES (deviation from vanilla
    // FreeRTOS; StackType_t is uint8_t here) — report it as-is. The old
    // "stackWords=N stackBytes=N*4" pair was 4x wrong. See System_TaskUtils.h.
    line += " stackBytes=";
    line += String((unsigned long)usStackDepth);
    line += " result=";
    line += (res == pdPASS ? "ok" : "fail");
    line += " heapBefore=";
    line += String(heapBefore);
    line += " heapAfter=";
    line += String(heapAfter);
    line += " heapDelta=";
    line += String(heapDelta);
    line += " internalBefore=";
    line += String(internalBefore);
    line += " internalAfter=";
    line += String(internalAfter);
    line += " largestBefore=";
    line += String(largestBefore);
    line += " largestAfter=";
    line += String(largestAfter);
    if (psTot > 0) {
      line += " psBefore=";
      line += String(psBefore);
      line += " psAfter=";
      line += String(psAfter);
      line += " psDelta=";
      line += String(psDelta);
    }
    // Memory allocation logging removed - LOG_ALLOC_FILE is obsolete
    broadcastOutput(line);
  }

  return res;
}

// ============================================================================
// Sensor Task Creation Helpers
// ============================================================================

bool createInputTask() {
  // Check for stale task handle (task deleted itself but handle not cleared)
  if (gInputTaskHandle != nullptr) {
    eTaskState state = eTaskGetState(gInputTaskHandle);
    if (state == eDeleted || state == eInvalid) {
      gInputTaskHandle = nullptr;
    }
  }
  if (gInputTaskHandle == nullptr) {
    const uint32_t stackWords = INPUT_STACK_WORDS;  // BYTES (3584 = 3.5 KB); name is a misnomer, see System_TaskUtils.h
    // Pin to Core 1 (APP): espnow_task + the Wi-Fi stack saturate Core 0, and a
    // starved input poll mid-I2C-transaction lets the legacy I2C driver's bus-
    // recovery path storm the I2C ISR → INT WDT (seen on bond role-swap re-sync).
    // Core 1 is near-idle, so the seesaw transaction completes before its 80ms
    // timeout instead of wedging.
    BaseType_t result = xTaskCreateLogged(
      inputTask,
      INPUT_TASK_NAME,
      stackWords,
      nullptr,
      1,
      &gInputTaskHandle,
      INPUT_TASK_TAG,
      1);

    if (result != pdPASS) {
      handleDeviceStopped(I2C_DEVICE_INPUT);
      return false;
    }
  }
  return true;  // Task created successfully or already exists
}

// All I2C sensor polling tasks are pinned to Core 1 (APP core). Core 0 is
// saturated by the Wi-Fi stack + ESP-NOW; an unpinned sensor poll that floats
// onto Core 0 and gets starved mid-I2C-transaction lets the legacy I2C driver's
// bus-recovery path storm the bus → panic(4) / INT-WDT. Core 1 is near-idle so
// the transaction completes before its timeout. This is the same reason the
// input task is pinned (see createInputTask). GPS — the busiest poller (a read
// every ~10 ms) — was the one left unpinned, and it crash-looped the FeatherS3
// on every out-and-about boot (docs/NewCapture, 2026-07-22): offline, the Wi-Fi
// stack thrashes to reach the absent home AP and saturates Core 0 harder, so the
// starve trips in seconds. Pinning every I2C poller closes the whole class.
// I2C_SENSOR_CORE now lives in System_TaskUtils.h so the sensor-queue processor
// (created in HardwareOne.cpp / System_I2C.cpp) can pin to the same core.

bool createThermalTask() {
  // Check for stale task handle (task deleted itself but handle not cleared)
  if (gThermalTaskHandle != nullptr) {
    eTaskState state = eTaskGetState(gThermalTaskHandle);
    if (state == eDeleted || state == eInvalid) {
      gThermalTaskHandle = nullptr;
    }
  }
  if (gThermalTaskHandle == nullptr) {
    const uint32_t thermalStack = THERMAL_STACK_WORDS;  // words; ~24KB (16KB was too tight on ESP32-classic — overflowed on failed-read retry storm + debug logging)
    if (xTaskCreateLogged(thermalTask, "thermal_task", thermalStack, nullptr, TASK_PRIORITY_LOW, &gThermalTaskHandle, "thermal", I2C_SENSOR_CORE) != pdPASS) {
      return false;
    }
  }
  return true;  // Task created successfully or already exists
}

bool createIMUTask() {
  // Check for stale task handle (task deleted itself but handle not cleared)
  if (gImuTaskHandle != nullptr) {
    eTaskState state = eTaskGetState(gImuTaskHandle);
    if (state == eDeleted || state == eInvalid) {
      gImuTaskHandle = nullptr;
    }
  }
  if (gImuTaskHandle == nullptr) {
    const uint32_t imuStack = IMU_STACK_WORDS;  // words; ~16KB (BNO055 init retries need extra stack)
    if (xTaskCreateLogged(imuTask, "imu_task", imuStack, nullptr, TASK_PRIORITY_LOW, &gImuTaskHandle, "imu", I2C_SENSOR_CORE) != pdPASS) {
      return false;
    }
    DEBUG_CLIF("imustart: IMU task created successfully");
  }
  return true;  // Task created successfully or already exists
}

bool createToFTask() {
  // Check for stale task handle (task deleted itself but handle not cleared)
  if (gTofTaskHandle != nullptr) {
    eTaskState state = eTaskGetState(gTofTaskHandle);
    if (state == eDeleted || state == eInvalid) {
      gTofTaskHandle = nullptr;
    }
  }
  if (gTofTaskHandle == nullptr) {
    const uint32_t tofStack = TOF_STACK_WORDS;  // words; ~12KB
    if (xTaskCreateLogged(tofTask, "tof_task", tofStack, nullptr, TASK_PRIORITY_LOW, &gTofTaskHandle, "tof", I2C_SENSOR_CORE) != pdPASS) {
      DEBUG_CLIF("tofstart: FAILED to create ToF task");
      return false;
    }
  }
  return true;  // Task created successfully or already exists
}

bool createFMRadioTask() {
  // Check for stale task handle (task deleted itself but handle not cleared)
  if (gFmRadioTaskHandle != nullptr) {
    eTaskState state = eTaskGetState(gFmRadioTaskHandle);
    if (state == eDeleted || state == eInvalid) {
      gFmRadioTaskHandle = nullptr;
    }
  }
  if (gFmRadioTaskHandle == nullptr) {
    const uint32_t fmRadioStack = FMRADIO_STACK_WORDS;  // words; ~18KB
    if (xTaskCreateLogged(fmRadioTask, "fmradio_task", fmRadioStack, nullptr, TASK_PRIORITY_LOW, &gFmRadioTaskHandle, "fmradio", I2C_SENSOR_CORE) != pdPASS) {
      DEBUG_CLIF("fmradiostart: FAILED to create FM Radio task");
      return false;
    }
    DEBUG_CLIF("fmradiostart: FM Radio task created successfully (handle=%p)", gFmRadioTaskHandle);
  }
  return true;  // Task created successfully or already exists
}

bool createAPDSTask() {
  // Check for stale task handle (task deleted itself but handle not cleared)
  if (gApdsTaskHandle != nullptr) {
    eTaskState state = eTaskGetState(gApdsTaskHandle);
    if (state == eDeleted || state == eInvalid) {
      gApdsTaskHandle = nullptr;
    }
  }
  if (gApdsTaskHandle == nullptr) {
    const uint32_t apdsStack = APDS_STACK_WORDS;  // words; ~12KB
    if (xTaskCreateLogged(apdsTask, "apds_task", apdsStack, nullptr, TASK_PRIORITY_LOW, &gApdsTaskHandle, "apds", I2C_SENSOR_CORE) != pdPASS) {
      return false;
    }
    DEBUG_CLIF("APDS task created successfully");
  }
  return true;  // Task created successfully or already exists
}

bool createPresenceTask() {
  // Check for stale task handle (task deleted itself but handle not cleared)
  if (gPresenceTaskHandle != nullptr) {
    eTaskState state = eTaskGetState(gPresenceTaskHandle);
    if (state == eDeleted || state == eInvalid) {
      gPresenceTaskHandle = nullptr;
    }
  }
  if (gPresenceTaskHandle == nullptr) {
    const uint32_t presenceStack = PRESENCE_STACK_WORDS;  // words; ~12KB
    if (xTaskCreateLogged(presenceTask, "presence_task", presenceStack, nullptr, TASK_PRIORITY_LOW, &gPresenceTaskHandle, "presence", I2C_SENSOR_CORE) != pdPASS) {
      return false;
    }
    DEBUG_CLIF("Presence task created successfully");
  }
  return true;  // Task created successfully or already exists
}

bool createGPSTask() {
  // Check for stale task handle (task deleted itself but handle not cleared)
  if (gGpsTaskHandle != nullptr) {
    eTaskState state = eTaskGetState(gGpsTaskHandle);
    if (state == eDeleted || state == eInvalid) {
      gGpsTaskHandle = nullptr;
    }
  }
  if (gGpsTaskHandle == nullptr) {
    const uint32_t gpsStack = GPS_STACK_WORDS;  // words; ~12KB
    if (xTaskCreateLogged(gpsTask, "gps_task", gpsStack, nullptr, TASK_PRIORITY_LOW, &gGpsTaskHandle, "gps", I2C_SENSOR_CORE) != pdPASS) {
      return false;
    }
    DEBUG_CLIF("GPS task created successfully");
  }
  return true;  // Task created successfully or already exists
}

bool createRTCTask() {
  // Check for stale task handle (task deleted itself but handle not cleared)
  if (gRtcTaskHandle != nullptr) {
    eTaskState state = eTaskGetState(gRtcTaskHandle);
    if (state == eDeleted || state == eInvalid) {
      gRtcTaskHandle = nullptr;
    }
  }
  if (gRtcTaskHandle == nullptr) {
    const uint32_t rtcStack = RTC_STACK_WORDS;  // words; ~16KB
    if (xTaskCreateLogged(rtcTask, "rtc_task", rtcStack, nullptr, TASK_PRIORITY_LOW, &gRtcTaskHandle, "rtc", I2C_SENSOR_CORE) != pdPASS) {
      return false;
    }
    DEBUG_CLIF("RTC task created successfully");
  }
  return true;  // Task created successfully or already exists
}

// ============================================================================
// Automated Stack Watermark Monitoring
// ============================================================================

// Report stack usage for a single task
void reportTaskStack(TaskHandle_t handle, const char* name, uint32_t allocatedBytes) {
  if (!handle) return;

  // Only call expensive FreeRTOS function when debug enabled
  if (!isDebugFlagSet(DEBUG_PERFORMANCE)) return;

  // Both the create-time depth and the high-water mark are in BYTES on this
  // port (StackType_t is uint8_t), so no word->byte scaling. The old "* 4" here
  // inflated every absolute figure 4x. See System_TaskUtils.h.
  UBaseType_t watermark = uxTaskGetStackHighWaterMark(handle);
  uint32_t usedBytes = allocatedBytes - (uint32_t)watermark;
  uint32_t usedPercent = (usedBytes * 100) / allocatedBytes;
  uint32_t freePercent = 100 - usedPercent;
  
  char tsPrefix[40];
  getTimestampPrefixMsCached(tsPrefix, sizeof(tsPrefix));
  String prefix = (tsPrefix[0] == '[') ? String(tsPrefix) : String("[BOOT] | ");
  
  String line;
  line.reserve(200);
  line += prefix;
  line += "task=";
  line += name;
  line += " stackTotal=";
  line += String(allocatedBytes);
  line += "B used=";
  line += String(usedBytes);
  line += "B (";
  line += String(usedPercent);
  line += "%) free=";
  line += String((unsigned long)watermark);
  line += "B (";
  line += String(freePercent);
  line += "%) watermark=";
  line += String(watermark);
  line += "words";
  
  // Memory allocation logging removed - LOG_ALLOC_FILE is obsolete
  
  DEBUG_PERFORMANCEF("%s", line.c_str());
}

// Report all sensor task stacks with comprehensive memory pressure stats
// --- Per-interval (delta) CPU% tracking --------------------------------------
// reportAllTaskStacks() prints both lifetime CPU% (ulRunTimeCounter/total) and
// a per-interval dCPU% computed against the previous report. Keyed by task
// HANDLE because names can be stale or duplicated. The run-time counters are
// esp_timer microseconds (CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER):
// 32-bit, wrapping ~every 71 min, but unsigned deltas over the ~60 s report
// cadence are safe. A delta wildly larger than the elapsed run-time clock means
// a TCB handle was reused by a new task (or the counter wrapped) → reported as
// n/a for that round rather than a garbage spike.
struct RunSample { TaskHandle_t handle; uint32_t prevRun; };
static RunSample*  sRunPrev      = nullptr;
static UBaseType_t sRunPrevCount = 0;
static UBaseType_t sRunPrevCap   = 0;
static uint32_t    sPrevTotalRun = 0;
static bool        sRunPrevValid = false;

// Interval CPU% for a task handle, or -1 if no usable baseline this round.
static int deltaCpuPercent(TaskHandle_t h, uint32_t curRun, uint32_t totalDelta) {
  if (!sRunPrevValid || totalDelta == 0 || !h) return -1;
  for (UBaseType_t i = 0; i < sRunPrevCount; i++) {
    if (sRunPrev[i].handle == h) {
      uint32_t d = curRun - sRunPrev[i].prevRun;            // wrap-safe unsigned
      if (d > totalDelta + (totalDelta / 4)) return -1;     // >125% → reuse/wrap
      uint64_t pct = (uint64_t)d * 100 / totalDelta;
      return pct > 100 ? 100 : (int)pct;
    }
  }
  return -1;  // task absent last round (newly created) → no baseline yet
}

void reportAllTaskStacks() {
  broadcastOutput("");
  broadcastOutput("========== TASK STACK REPORT ==========");
  
  // Memory stats - kept for fragmentation/warning calculations, but verbose output removed
  // (detailed memory overview now handled by periodicMemorySample())
  size_t heapFree = ESP.getFreeHeap();
  size_t largestFreeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  
  // Get all tasks from system
  UBaseType_t taskCount = uxTaskGetNumberOfTasks();
  static TaskStatus_t* taskArray = nullptr;
  static TaskHeapMeasure* taskMeasure = nullptr;   // parallel to taskArray
  static UBaseType_t taskCap = 0;
  UBaseType_t numTasks = 0;
  uint32_t totalRuntime = 0;

  // Take a consistent snapshot. Tasks can be created/deleted while we're printing.
  // If the task count grows between calls, retry with a larger buffer.
  for (int attempt = 0; attempt < 3; attempt++) {
    taskCount = uxTaskGetNumberOfTasks();
    // Add headroom so we don't thrash allocations if tasks are created concurrently.
    UBaseType_t needed = taskCount + 4;
    if (needed > taskCap) {
      if (taskArray)   { free(taskArray);   taskArray   = nullptr; }
      if (taskMeasure) { free(taskMeasure); taskMeasure = nullptr; }
      taskCap = 0;
      taskArray   = (TaskStatus_t*)ps_alloc(needed * sizeof(TaskStatus_t), AllocPref::PreferPSRAM, "task.pressure");
      taskMeasure = (TaskHeapMeasure*)ps_alloc(needed * sizeof(TaskHeapMeasure), AllocPref::PreferPSRAM, "task.measure");
      if (taskArray && taskMeasure) {
        taskCap = needed;
      } else {                       // partial failure must not leave a half-armed pair
        if (taskArray)   { free(taskArray);   taskArray   = nullptr; }
        if (taskMeasure) { free(taskMeasure); taskMeasure = nullptr; }
      }
    }
    if (!taskArray || !taskMeasure) {
      broadcastOutput("ERROR: Cannot allocate task array");
      return;
    }
    totalRuntime = 0;
    numTasks = uxTaskGetSystemState(taskArray, taskCount, &totalRuntime);
    // If we somehow got more tasks than we captured, retry with a larger buffer.
    if (numTasks <= taskCount) {
      break;
    }
    if (numTasks > taskCap) {
      // Force next iteration to resize.
      taskCap = 0;
    }
  }
  
  // If uxTaskGetSystemState() is not supported (returns 0), just report that
  // per-task stats are unavailable instead of printing misleading zeros.
  if (numTasks == 0) {
    broadcastOutput("");
    broadcastOutput("-- TASK BREAKDOWN --");
    broadcastOutput("  Per-task statistics not available (FreeRTOS trace disabled).");
    return;
  }

  // Per-interval CPU: elapsed run-time clock since the previous report (wrap-safe).
  uint32_t totalDelta = sRunPrevValid ? (uint32_t)(totalRuntime - sPrevTotalRun) : 0;

  // Measure every task's stack and TCB block off the live allocator, in one
  // walk, BEFORE any printing (the same helper the boot memory report uses,
  // so the two reporters can never disagree on what a task costs). The old
  // code carried a hardcoded TCB_SIZE = 104 (the real TCB_t is 376 B on this
  // IDF — 3.6x low) and assumed a 4 KB "used" margin for every kernel task.
  taskHeapMeasureSnapshot(taskArray, taskMeasure, numTasks);

  broadcastOutput("");
  BROADCAST_PRINTF("-- TASK BREAKDOWN (%u tasks) --", (unsigned)numTasks);
  broadcastOutput("  Name              Stack(KB)  Used(KB)  Free(KB)  Used%  CPU%  dCPU%  TCB(B)");
  broadcastOutput("  ----------------  ---------  --------  --------  -----  ----  -----  ------");
  broadcastOutput("  (Stack and TCB are measured allocator blocks; '?' = not a live internal-heap block, not summed)");

  uint32_t totalStackAllocated = 0;
  uint32_t totalStackUsed = 0;
  uint32_t totalTCBOverhead = 0;
  unsigned tcbMeasured = 0;
  unsigned stackUnmeasured = 0;

  // TCB column: measured bytes, or "?" when the TCB is not a live internal
  // heap block (never a fabricated constant).
  auto fmtTcb = [](char* buf, size_t n, uint32_t bytes) {
    if (bytes) snprintf(buf, n, "%u", (unsigned)bytes);
    else       snprintf(buf, n, "?");
  };

  // Known sensor tasks with their stack sizes
  struct KnownTask {
    const char* name;
    TaskHandle_t handle;
    uint32_t stackWords;
  };
  
  // Build known tasks list dynamically (some handles are runtime-resolved)
#if ENABLE_ESPNOW
  TaskHandle_t espnowHandle = nullptr;
  UBaseType_t espnowWatermark = 0;
  const bool espnowSnapshotValid =
      getEspNowTaskStackSnapshot(&espnowHandle, &espnowWatermark);
  TaskHandle_t espnowTxHandle = nullptr;
  UBaseType_t espnowTxWatermark = 0;
  const bool espnowTxSnapshotValid =
      espnowtx::getTaskStackSnapshot(&espnowTxHandle, &espnowTxWatermark);
#else
  TaskHandle_t espnowHandle = nullptr;
  UBaseType_t espnowWatermark = 0;
  const bool espnowSnapshotValid = false;
  TaskHandle_t espnowTxHandle = nullptr;
  UBaseType_t espnowTxWatermark = 0;
  const bool espnowTxSnapshotValid = false;
#endif

  const KnownTask knownTasks[] = {
    {"espnow_task", espnowHandle, ESPNOW_HB_STACK_WORDS},
    {"espnow_tx", espnowTxHandle, ESPNOW_TX_STACK_WORDS},
    {"cmd_exec_task", gCmdExecTaskHandle, CMD_EXEC_STACK_WORDS},
    {"sensor_queue_task", queueProcessorTask, SENSOR_QUEUE_STACK_WORDS},
    {INPUT_TASK_NAME, gInputTaskHandle, INPUT_STACK_WORDS},
    {"thermal_task", gThermalTaskHandle, THERMAL_STACK_WORDS},
    {"imu_task", gImuTaskHandle, IMU_STACK_WORDS},
    {"tof_task", gTofTaskHandle, TOF_STACK_WORDS},
    {"fmradio_task", gFmRadioTaskHandle, FMRADIO_STACK_WORDS},
    {"gps_task", gGpsTaskHandle, GPS_STACK_WORDS},
    {"apds_task", gApdsTaskHandle, APDS_STACK_WORDS},
    {"presence_task", gPresenceTaskHandle, PRESENCE_STACK_WORDS},
    {"rtc_task", gRtcTaskHandle, RTC_STACK_WORDS}
  };

  auto isTaskHandleInSnapshot = [&](TaskHandle_t h) -> bool {
    if (!h) return false;
    for (UBaseType_t i = 0; i < numTasks; i++) {
      if (taskArray[i].xHandle == h) return true;
    }
    return false;
  };
  
  // Enabled flags for each task — if false, handle may be stale (task self-deleted)
  const bool taskAlive[] = {
    espnowSnapshotValid,                                            // espnow_task
    espnowTxSnapshotValid,                                         // espnow_tx
    gCmdExecTaskHandle != nullptr,                                  // cmd_exec_task
    queueProcessorTask != nullptr,                                  // sensor_queue_task
    gInputRunning,                                                 // gamepad_task
    gThermalRunning,                                                 // thermal_task
    gImuRunning,                                                     // imu_task
    gTofRunning,                                                     // tof_task
    gFmRadioRunning,                                                 // fmradio_task
    gGpsRunning,                                                     // gps_task
    (gApdsColorRunning || gApdsProximityRunning || gApdsGestureRunning), // apds_task
    gPresenceRunning,                                                // presence_task
    gRtcRunning,                                                     // rtc_task
  };

  // First print known tasks
  int ktIdx = 0;
  for (const auto& kt : knownTasks) {
    bool alive = taskAlive[ktIdx++];
    if (!kt.handle || !alive) continue;
    if (!isTaskHandleInSnapshot(kt.handle)) continue;
    
    // BYTES, not words — kt.stackWords holds the byte count passed to
    // xTaskCreate, and the HWM is in bytes too. See System_TaskUtils.h.
    // The TX task can be deleted by closeespnow. Its watermark was sampled
    // while the TX lifecycle-owner mutex prevented deletion; never issue a
    // later FreeRTOS task query with the now-unpinned handle.
    UBaseType_t watermark = (ktIdx == 1)
        ? espnowWatermark
        : (ktIdx == 2) ? espnowTxWatermark
                       : uxTaskGetStackHighWaterMark(kt.handle);

    // Find the snapshot row by handle (names can be stale/duplicate): it
    // carries the measured stack/TCB blocks and the CPU counters. cpuPercent
    // is lifetime; dCpu is the per-interval delta (n/a until a baseline exists).
    uint32_t cpuPercent = 0;
    int dCpu = -1;
    uint32_t measuredStack = 0;
    uint32_t measuredTcb = 0;
    for (UBaseType_t i = 0; i < numTasks; i++) {
      if (taskArray[i].xHandle == kt.handle) {
        uint32_t curRun = (uint32_t)taskArray[i].ulRunTimeCounter;
        cpuPercent = (totalRuntime > 0) ? (uint32_t)((uint64_t)curRun * 100 / totalRuntime) : 0;  // uint64: curRun*100 overflows uint32 after ~43s uptime
        dCpu = deltaCpuPercent(kt.handle, curRun, totalDelta);
        measuredStack = taskMeasure[i].stackBytes;
        measuredTcb = taskMeasure[i].tcbBytes;
        break;
      }
    }
    char dc[16];  // sized for worst-case %4d%% so -Wformat-truncation stays quiet
    if (dCpu < 0) snprintf(dc, sizeof(dc), "%5s", "-");
    else          snprintf(dc, sizeof(dc), "%4d%%", dCpu);
    char tcb[12];
    fmtTcb(tcb, sizeof(tcb), measuredTcb);
    if (measuredTcb) { totalTCBOverhead += measuredTcb; tcbMeasured++; }

    // Same rule as every other row and as the boot memory report: the stack
    // size is the MEASURED allocator block or it is '?'. The compile-time
    // constant is NOT substituted here — that would fold an unmeasured figure
    // into a total the summary labels "(measured)". For a stack the allocator
    // does not own (PSRAM via xTaskCreateWithCaps, caller-owned buffer) the
    // constant still drives the WARNINGS pass below, which is a threshold
    // check, not an accounting.
    uint32_t freeBytes = (uint32_t)watermark;
    uint32_t allocBytes = measuredStack;
    if (allocBytes == 0) {
      stackUnmeasured++;
      BROADCAST_PRINTF("  %-16s     ?         ?      %4u         ?   %2u%%  %5s   %4s",
        kt.name,
        (unsigned)(freeBytes/1024),
        (unsigned)cpuPercent,
        dc,
        tcb);
      continue;
    }
    uint32_t usedBytes = (allocBytes > freeBytes) ? (allocBytes - freeBytes) : 0;
    uint32_t usedPercent = (usedBytes * 100) / allocBytes;

    BROADCAST_PRINTF("  %-16s  %4u      %4u      %4u      %3u%%   %2u%%  %5s   %4s",
      kt.name,
      (unsigned)(allocBytes/1024),
      (unsigned)(usedBytes/1024),
      (unsigned)(freeBytes/1024),
      (unsigned)usedPercent,
      (unsigned)cpuPercent,
      dc,
      tcb);

    totalStackAllocated += allocBytes;
    totalStackUsed += usedBytes;
  }
  
  // Print system tasks
  broadcastOutput("");
  for (UBaseType_t i = 0; i < numTasks; i++) {
    const char* name = taskArray[i].pcTaskName;
    
    // Guard against stale snapshot entries (task deleted after uxTaskGetSystemState)
    if (!name || (uintptr_t)name < 0x3F000000) continue;  // ESP32 valid memory starts at 0x3F...
    
    // Skip already reported sensor tasks (match by handle, not name, to avoid stale name ptrs)
    bool isKnown = false;
    for (const auto& kt : knownTasks) {
      if (taskArray[i].xHandle == kt.handle && kt.handle != nullptr) {
        isKnown = true;
        break;
      }
    }
    if (isKnown) continue;
    
    UBaseType_t watermark = taskArray[i].usStackHighWaterMark;
    uint32_t cpuPercent = (totalRuntime > 0) ? (uint32_t)((uint64_t)taskArray[i].ulRunTimeCounter * 100 / totalRuntime) : 0;  // uint64: *100 overflows uint32 after ~43s uptime
    int dCpu = deltaCpuPercent(taskArray[i].xHandle, (uint32_t)taskArray[i].ulRunTimeCounter, totalDelta);
    char dc[16];  // sized for worst-case %4d%% so -Wformat-truncation stays quiet
    if (dCpu < 0) snprintf(dc, sizeof(dc), "%5s", "-");
    else          snprintf(dc, sizeof(dc), "%4d%%", dCpu);
    char tcb[12];
    fmtTcb(tcb, sizeof(tcb), taskMeasure[i].tcbBytes);
    if (taskMeasure[i].tcbBytes) { totalTCBOverhead += taskMeasure[i].tcbBytes; tcbMeasured++; }

    // Allocated stack is the MEASURED block behind pxStackBase — BYTES
    // throughout (the watermark is bytes on this port, see System_TaskUtils.h).
    // A task whose stack is not a live internal-heap block (PSRAM stack,
    // caller-owned buffer, or torn down since the snapshot) prints '?' and is
    // not summed: the only real datum for it is the HWM.
    uint32_t freeBytes = (uint32_t)watermark;
    uint32_t allocBytes = taskMeasure[i].stackBytes;
    if (allocBytes == 0) {
      stackUnmeasured++;
      BROADCAST_PRINTF("  %-16s     ?         ?      %4u         ?   %2u%%  %5s   %4s",
        name,
        (unsigned)(freeBytes/1024),
        (unsigned)cpuPercent,
        dc,
        tcb);
      continue;
    }
    uint32_t usedBytes = (allocBytes > freeBytes) ? (allocBytes - freeBytes) : 0;
    uint32_t usedPercent = (usedBytes * 100) / allocBytes;

    BROADCAST_PRINTF("  %-16s  %4u      %4u      %4u      %3u%%   %2u%%  %5s   %4s",
      name,
      (unsigned)(allocBytes/1024),
      (unsigned)(usedBytes/1024),
      (unsigned)(freeBytes/1024),
      (unsigned)usedPercent,
      (unsigned)cpuPercent,
      dc,
      tcb);

    totalStackAllocated += allocBytes;
    totalStackUsed += usedBytes;
  }

  broadcastOutput("  ----------------  ---------  --------  --------  -----  ----  -----  ------");
  // TCB total widened %4u -> %6u: at 376 B per TCB a ~40-task build is five digits.
  BROADCAST_PRINTF("  TOTALS:           %5u     %5u      %5u                      %6u",
    (unsigned)(totalStackAllocated/1024),
    (unsigned)(totalStackUsed/1024),
    (unsigned)((totalStackAllocated - totalStackUsed)/1024),
    (unsigned)totalTCBOverhead);
  if (stackUnmeasured > 0) {
    BROADCAST_PRINTF("  (%u task stack(s) not a live internal-heap block - not in the totals)",
      stackUnmeasured);
  }

  // -- CPU (per-interval, since last report) --
  // Lifetime CPU% converges to a meaningless average over long uptimes; the
  // dCPU% column + these idle lines show what was actually busy this interval.
  broadcastOutput("");
  if (!sRunPrevValid || totalDelta == 0) {
    broadcastOutput("-- CPU (per-interval) --");
    broadcastOutput("  dCPU% baseline set; per-interval values appear from the next report.");
  } else {
    broadcastOutput("-- CPU (per-interval, since last report) --");
    bool printedIdle = false;
    for (UBaseType_t i = 0; i < numTasks; i++) {
      const char* nm = taskArray[i].pcTaskName;
      if (!nm || (uintptr_t)nm < 0x3F000000) continue;     // guard stale snapshot ptr
      if (strncmp(nm, "IDLE", 4) == 0) {                   // IDLE0 / IDLE1 (per core)
        int idlePct = deltaCpuPercent(taskArray[i].xHandle,
                                      (uint32_t)taskArray[i].ulRunTimeCounter, totalDelta);
        if (idlePct >= 0) {
          BROADCAST_PRINTF("  %-6s idle: %3d%% free this interval", nm, idlePct);
          printedIdle = true;
        }
      }
    }
    if (!printedIdle) broadcastOutput("  (idle-task headroom unavailable)");
  }

  // Memory accounting summary
  // Accounting summary packed: header+3 lines (1 msg), then frag+waste (1 msg) — was 6.
  BROADCAST_PRINTF(
    "\n-- MEMORY ACCOUNTING SUMMARY --\n"
    "  Task Stacks:          %6u KB  (measured)\n"
    "  Task Control Blocks:  %6u B   (%u of %u tasks measured)\n"
    "  Total Task Overhead:  %6u KB",
    (unsigned)(totalStackAllocated/1024),
    (unsigned)totalTCBOverhead, tcbMeasured, (unsigned)numTasks,
    (unsigned)((totalStackAllocated + totalTCBOverhead)/1024));
  unsigned fragPercent = 0;
  if (heapFree > 0) {
    fragPercent = (unsigned)((largestFreeBlock * 100) / heapFree);
  }
  BROADCAST_PRINTF(
    "  Heap Fragmentation:   %2u%% (largest block vs free)\n"
    "  Task Memory Waste:    %6u KB (allocated but unused stack)",
    fragPercent,
    (unsigned)((totalStackAllocated - totalStackUsed)/1024));
  
  // Warnings
  bool hasWarning = false;
  broadcastOutput("");
  broadcastOutput("-- WARNINGS & ALERTS --");
  
  int warnIdx = 0;
  for (const auto& kt : knownTasks) {
    bool alive = taskAlive[warnIdx++];
    if (!kt.handle || !alive) continue;
    if (!isTaskHandleInSnapshot(kt.handle)) continue;
    // Only call expensive FreeRTOS function when needed for warning check
    if (!isDebugFlagSet(DEBUG_PERFORMANCE)) continue;
    UBaseType_t watermark = (warnIdx == 1)
        ? espnowWatermark
        : (warnIdx == 2) ? espnowTxWatermark
                         : uxTaskGetStackHighWaterMark(kt.handle);
    uint32_t freePercent = (watermark * 100) / kt.stackWords;
    if (freePercent < 25) {
      BROADCAST_PRINTF("  [!] %-16s CRITICAL: Only %u%% stack free!", 
        kt.name, (unsigned)freePercent);
      hasWarning = true;
    }
  }
  
  if (heapFree < 40960) {
    BROADCAST_PRINTF("  [!] HEAP: Only %u KB free (< 40KB threshold)", 
      (unsigned)(heapFree/1024));
    hasWarning = true;
  }
  
  if (heapFree > 0 && largestFreeBlock < heapFree / 2) {
    BROADCAST_PRINTF("  [!] FRAGMENTATION: Largest block %u KB vs %u KB free",
      (unsigned)(largestFreeBlock/1024), (unsigned)(heapFree/1024));
    hasWarning = true;
  }
  
  if (!hasWarning) {
    broadcastOutput("  [OK] No critical warnings - all tasks healthy");
  }
  
  // Refresh the per-interval CPU baseline for the next report (keyed by handle).
  {
    UBaseType_t need = numTasks + 4;
    if (need > sRunPrevCap) {
      if (sRunPrev) { free(sRunPrev); sRunPrev = nullptr; sRunPrevCap = 0; }
      sRunPrev = (RunSample*)ps_alloc(need * sizeof(RunSample), AllocPref::PreferPSRAM, "task.runprev");
      if (sRunPrev) sRunPrevCap = need;
    }
    if (sRunPrev) {
      sRunPrevCount = 0;
      for (UBaseType_t i = 0; i < numTasks && sRunPrevCount < sRunPrevCap; i++) {
        sRunPrev[sRunPrevCount].handle  = taskArray[i].xHandle;
        sRunPrev[sRunPrevCount].prevRun = (uint32_t)taskArray[i].ulRunTimeCounter;
        sRunPrevCount++;
      }
      sPrevTotalRun = totalRuntime;
      sRunPrevValid = true;
    }
  }

  broadcastOutput("");
  broadcastOutput("========== END TASK STACK REPORT ==========");
  broadcastOutput("");

}
