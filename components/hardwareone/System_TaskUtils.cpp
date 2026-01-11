#include "System_TaskUtils.h"
#include "System_Filesystem.h"    // For filesystemReady, isFsLockedByCurrentTask
#include "System_Mutex.h"  // For isFsLockedByCurrentTask
#include "System_Debug.h"  // For DEBUG_CLIF macro
#include "System_MemUtil.h"      // For ps_alloc, AllocPref
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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

// Forward declaration (implemented in HardwareOnev2.1.ino)
bool appendLineWithCap(const char* path, const String& line, size_t capBytes);

// External timestamp function
extern void getTimestampPrefixMsCached(char* buf, size_t bufSize);

// External task handles (defined in .ino)
extern TaskHandle_t gamepadTaskHandle;
extern TaskHandle_t thermalTaskHandle;
extern TaskHandle_t imuTaskHandle;
extern TaskHandle_t tofTaskHandle;
extern TaskHandle_t fmRadioTaskHandle;

// External task functions (defined in sensor modules)
extern void gamepadTask(void* parameter);
extern void thermalTask(void* parameter);
extern void imuTask(void* parameter);
extern void tofTask(void* parameter);
extern void fmRadioTask(void* parameter);

// External sensor state flags (defined in .ino or sensor modules)
extern bool gamepadEnabled;
extern bool gamepadConnected;

// ============================================================================
// Task Creation with Memory Logging
// ============================================================================

BaseType_t xTaskCreateLogged(TaskFunction_t pxTaskCode,
                              const char* pcName,
                              const uint32_t usStackDepth,
                              void* pvParameters,
                              UBaseType_t uxPriority,
                              TaskHandle_t* pxCreatedTask,
                              const char* tag) {
  // Measure before
  size_t heapBefore = ESP.getFreeHeap();
  size_t psTot = ESP.getPsramSize();
  size_t psBefore = (psTot > 0) ? ESP.getFreePsram() : 0;

  BaseType_t res = xTaskCreate(pxTaskCode, pcName, usStackDepth, pvParameters, uxPriority, pxCreatedTask);

  // Optionally log (only when FS is ready and not inside FS critical section)
  if (filesystemReady && !isFsLockedByCurrentTask()) {
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
    line += " stackWords=";
    line += String((unsigned long)usStackDepth);
    line += " stackBytes=";
    line += String((unsigned long)(usStackDepth * 4));
    line += " result=";
    line += (res == pdPASS ? "ok" : "fail");
    line += " heapBefore=";
    line += String(heapBefore);
    line += " heapAfter=";
    line += String(heapAfter);
    line += " heapDelta=";
    line += String(heapDelta);
    if (psTot > 0) {
      line += " psBefore=";
      line += String(psBefore);
      line += " psAfter=";
      line += String(psAfter);
      line += " psDelta=";
      line += String(psDelta);
    }
    // Memory allocation logging removed - LOG_ALLOC_FILE is obsolete
  }

  return res;
}

// ============================================================================
// Sensor Task Creation Helpers
// ============================================================================

bool createGamepadTask() {
  // Check for stale task handle (task deleted itself but handle not cleared)
  if (gamepadTaskHandle != nullptr) {
    eTaskState state = eTaskGetState(gamepadTaskHandle);
    if (state == eDeleted || state == eInvalid) {
      gamepadTaskHandle = nullptr;
    }
  }
  if (gamepadTaskHandle == nullptr) {
    BaseType_t result = xTaskCreateLogged(
      gamepadTask,
      "GamepadTask",
      4096,  // 4KB stack (similar to other sensor tasks)
      nullptr,
      1,  // Priority 1 (same as other sensor tasks)
      &gamepadTaskHandle,
      "gamepad");

    if (result != pdPASS) {
      gamepadEnabled = false;
      gamepadConnected = false;
      return false;
    }
  }
  return true;  // Task created successfully or already exists
}

bool createThermalTask() {
  // Check for stale task handle (task deleted itself but handle not cleared)
  if (thermalTaskHandle != nullptr) {
    eTaskState state = eTaskGetState(thermalTaskHandle);
    if (state == eDeleted || state == eInvalid) {
      thermalTaskHandle = nullptr;
    }
  }
  if (thermalTaskHandle == nullptr) {
    const uint32_t thermalStack = 4096;  // words; ~16KB (reduced from 24KB - frame buffers moved to PSRAM)
    if (xTaskCreateLogged(thermalTask, "thermal_task", thermalStack, nullptr, 1, &thermalTaskHandle, "thermal") != pdPASS) {
      return false;
    }
  }
  return true;  // Task created successfully or already exists
}

bool createIMUTask() {
  // Check for stale task handle (task deleted itself but handle not cleared)
  if (imuTaskHandle != nullptr) {
    eTaskState state = eTaskGetState(imuTaskHandle);
    if (state == eDeleted || state == eInvalid) {
      imuTaskHandle = nullptr;
    }
  }
  if (imuTaskHandle == nullptr) {
    const uint32_t imuStack = 4096;  // words; ~16KB (reduced from 24KB - peak usage 11KB)
    if (xTaskCreateLogged(imuTask, "imu_task", imuStack, nullptr, 1, &imuTaskHandle, "imu") != pdPASS) {
      return false;
    }
    DEBUG_CLIF("imustart: IMU task created successfully");
  }
  return true;  // Task created successfully or already exists
}

bool createToFTask() {
  // Check for stale task handle (task deleted itself but handle not cleared)
  if (tofTaskHandle != nullptr) {
    eTaskState state = eTaskGetState(tofTaskHandle);
    if (state == eDeleted || state == eInvalid) {
      tofTaskHandle = nullptr;
    }
  }
  if (tofTaskHandle == nullptr) {
    const uint32_t tofStack = 3072;  // words; ~12KB
    if (xTaskCreateLogged(tofTask, "tof_task", tofStack, nullptr, 1, &tofTaskHandle, "tof") != pdPASS) {
      DEBUG_CLIF("tofstart: FAILED to create ToF task");
      return false;
    }
  }
  return true;  // Task created successfully or already exists
}

bool createFMRadioTask() {
  // Check for stale task handle (task deleted itself but handle not cleared)
  if (fmRadioTaskHandle != nullptr) {
    eTaskState state = eTaskGetState(fmRadioTaskHandle);
    if (state == eDeleted || state == eInvalid) {
      fmRadioTaskHandle = nullptr;
    }
  }
  if (fmRadioTaskHandle == nullptr) {
    const uint32_t fmRadioStack = 4608;  // words; ~18KB
    if (xTaskCreateLogged(fmRadioTask, "fmradio_task", fmRadioStack, nullptr, 1, &fmRadioTaskHandle, "fmradio") != pdPASS) {
      DEBUG_CLIF("fmradiostart: FAILED to create FM Radio task");
      return false;
    }
    DEBUG_CLIF("fmradiostart: FM Radio task created successfully (handle=%p)", fmRadioTaskHandle);
  }
  return true;  // Task created successfully or already exists
}

// ============================================================================
// Automated Stack Watermark Monitoring
// ============================================================================

// Report stack usage for a single task
void reportTaskStack(TaskHandle_t handle, const char* name, uint32_t allocatedWords) {
  if (!handle) return;
  
  // Only call expensive FreeRTOS function when debug enabled
  if (!isDebugFlagSet(DEBUG_PERFORMANCE)) return;
  
  UBaseType_t watermark = uxTaskGetStackHighWaterMark(handle);
  uint32_t allocatedBytes = allocatedWords * 4;
  uint32_t usedBytes = allocatedBytes - (watermark * 4);
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
  line += String(watermark * 4);
  line += "B (";
  line += String(freePercent);
  line += "%) watermark=";
  line += String(watermark);
  line += "words";
  
  // Memory allocation logging removed - LOG_ALLOC_FILE is obsolete
  
  // Also print to serial for immediate visibility
  if (gOutputFlags & OUTPUT_SERIAL) {
    Serial.println(line);
  }
}

// Report all sensor task stacks with comprehensive memory pressure stats
void reportAllTaskStacks() {
  extern void broadcastOutput(const char* s);
  
  broadcastOutput("");
  broadcastOutput("╔══════════════════════════════════════════════════════════════════════════════╗");
  broadcastOutput("║                    COMPREHENSIVE TASK PRESSURE REPORT                      ║");
  broadcastOutput("╚══════════════════════════════════════════════════════════════════════════════╝");
  
  // Memory stats
  size_t heapFree = ESP.getFreeHeap();
  size_t heapTotal = ESP.getHeapSize();
  size_t heapMin = ESP.getMinFreeHeap();
  size_t heapUsed = heapTotal - heapFree;
  size_t psramFree = ESP.getFreePsram();
  size_t psramTotal = ESP.getPsramSize();
  size_t psramUsed = psramTotal - psramFree;
  
  // Try to get largest free block (heap fragmentation indicator).
  // Use INTERNAL|8BIT so this reflects internal heap only (PSRAM is tracked separately).
  size_t largestFreeBlock = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  
  broadcastOutput("");
  broadcastOutput("┌─────────────────────── MEMORY OVERVIEW ────────────────────┐");
  BROADCAST_PRINTF("│ HEAP (Internal DRAM):                                          │");
  BROADCAST_PRINTF("│   Total:      %6u KB                                         │", (unsigned)(heapTotal/1024));
  BROADCAST_PRINTF("│   Used:       %6u KB (%2u%%)                                  │", 
    (unsigned)(heapUsed/1024), (unsigned)((heapUsed * 100) / heapTotal));
  BROADCAST_PRINTF("│   Free:       %6u KB (%2u%%)                                  │", 
    (unsigned)(heapFree/1024), (unsigned)((heapFree * 100) / heapTotal));
  BROADCAST_PRINTF("│   Min Free:   %6u KB (lowest ever)                          │", (unsigned)(heapMin/1024));
  BROADCAST_PRINTF("│   Largest Block: %6u KB (fragmentation indicator)           │", (unsigned)(largestFreeBlock/1024));
  BROADCAST_PRINTF("│                                                                │");
  BROADCAST_PRINTF("│ PSRAM (External):                                              │");
  BROADCAST_PRINTF("│   Total:      %6u KB                                         │", (unsigned)(psramTotal/1024));
  if (psramTotal > 0) {
    BROADCAST_PRINTF("│   Used:       %6u KB (%2u%%)                                  │", 
      (unsigned)(psramUsed/1024), (unsigned)((psramUsed * 100) / psramTotal));
    BROADCAST_PRINTF("│   Free:       %6u KB (%2u%%)                                  │", 
      (unsigned)(psramFree/1024), (unsigned)((psramFree * 100) / psramTotal));
  } else {
    BROADCAST_PRINTF("│   Used:           0 KB ( 0%%)                                  │");
    BROADCAST_PRINTF("│   Free:           0 KB ( 0%%)                                  │");
  }
  broadcastOutput("└────────────────────────────────────────────────────────────────┘");
  // Get all tasks from system
  UBaseType_t taskCount = uxTaskGetNumberOfTasks();
  static TaskStatus_t* taskArray = nullptr;
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
      if (taskArray) {
        free(taskArray);
        taskArray = nullptr;
        taskCap = 0;
      }
      taskArray = (TaskStatus_t*)ps_alloc(needed * sizeof(TaskStatus_t), AllocPref::PreferPSRAM, "task.pressure");
      if (taskArray) {
        taskCap = needed;
      }
    }
    if (!taskArray) {
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
    broadcastOutput("┌─────────────────────── TASK BREAKDOWN ───────────────────────┐");
    broadcastOutput("│  Per-task statistics not available (FreeRTOS trace disabled). │");
    broadcastOutput("└────────────────────────────────────────────────────────────────┘");
    return;
  }

  broadcastOutput("");
  BROADCAST_PRINTF("┌─────────────────────── TASK BREAKDOWN (%u tasks) ───────────────────────┐", (unsigned)numTasks);
  broadcastOutput("│                                                                            │");
  broadcastOutput("│  Name              Stack(KB)  Used(KB)  Free(KB)  Used%  CPU%  TCB(B)    │");
  broadcastOutput("│  ────────────────  ─────────  ────────  ────────  ─────  ────  ─────    │");
  
  uint32_t totalStackAllocated = 0;
  uint32_t totalStackUsed = 0;
  uint32_t totalTCBOverhead = 0;
  const uint32_t TCB_SIZE = 104;  // ESP32 FreeRTOS TCB is ~104 bytes
  
  // Known sensor tasks with their stack sizes
  struct KnownTask {
    const char* name;
    TaskHandle_t handle;
    uint32_t stackWords;
  };
  
  static const KnownTask knownTasks[] = {
    {"GamepadTask", gamepadTaskHandle, 4096},
    {"thermal_task", thermalTaskHandle, 4096},
    {"imu_task", imuTaskHandle, 4096},
    {"tof_task", tofTaskHandle, 3072},
    {"fmradio_task", fmRadioTaskHandle, 4608}
  };

  auto isTaskHandleInSnapshot = [&](TaskHandle_t h) -> bool {
    if (!h) return false;
    for (UBaseType_t i = 0; i < numTasks; i++) {
      if (taskArray[i].xHandle == h) return true;
    }
    return false;
  };
  
  // First print known tasks
  for (const auto& kt : knownTasks) {
    if (!kt.handle) continue;
    if (!isTaskHandleInSnapshot(kt.handle)) continue;
    
    UBaseType_t watermark = uxTaskGetStackHighWaterMark(kt.handle);
    uint32_t allocBytes = kt.stackWords * 4;
    uint32_t usedBytes = allocBytes - (watermark * 4);
    uint32_t freeBytes = watermark * 4;
    uint32_t usedPercent = (usedBytes * 100) / allocBytes;
    
    // Find CPU usage
    uint32_t cpuPercent = 0;
    for (UBaseType_t i = 0; i < numTasks; i++) {
      if (strcmp(taskArray[i].pcTaskName, kt.name) == 0) {
        cpuPercent = (totalRuntime > 0) ? ((taskArray[i].ulRunTimeCounter * 100) / totalRuntime) : 0;
        break;
      }
    }
    
    BROADCAST_PRINTF("│  %-16s  %4u      %4u      %4u      %3u%%   %2u%%   %3u      │",
      kt.name,
      (unsigned)(allocBytes/1024),
      (unsigned)(usedBytes/1024),
      (unsigned)(freeBytes/1024),
      (unsigned)usedPercent,
      (unsigned)cpuPercent,
      (unsigned)TCB_SIZE);
    
    totalStackAllocated += allocBytes;
    totalStackUsed += usedBytes;
    totalTCBOverhead += TCB_SIZE;
  }
  
  // Print system tasks
  broadcastOutput("│                                                                            │");
  for (UBaseType_t i = 0; i < numTasks; i++) {
    const char* name = taskArray[i].pcTaskName;
    
    // Skip already reported sensor tasks
    bool isKnown = false;
    for (const auto& kt : knownTasks) {
      if (strcmp(name, kt.name) == 0) {
        isKnown = true;
        break;
      }
    }
    if (isKnown) continue;
    
    UBaseType_t watermark = taskArray[i].usStackHighWaterMark;
    
    // Estimate allocated stack (we don't know exact size for system tasks)
    // Use watermark + safety margin as conservative estimate
    uint32_t estimatedStackWords = watermark + 1024;  // Assume 4KB safety margin
    uint32_t allocBytes = estimatedStackWords * 4;
    uint32_t usedBytes = allocBytes - (watermark * 4);
    uint32_t freeBytes = watermark * 4;
    uint32_t usedPercent = (usedBytes * 100) / allocBytes;
    uint32_t cpuPercent = (totalRuntime > 0) ? ((taskArray[i].ulRunTimeCounter * 100) / totalRuntime) : 0;
    
    BROADCAST_PRINTF("│  %-16s ~%4u     ~%4u      %4u     ~%3u%%   %2u%%   %3u      │",
      name,
      (unsigned)(allocBytes/1024),
      (unsigned)(usedBytes/1024),
      (unsigned)(freeBytes/1024),
      (unsigned)usedPercent,
      (unsigned)cpuPercent,
      (unsigned)TCB_SIZE);
    
    totalStackAllocated += allocBytes;
    totalStackUsed += usedBytes;
    totalTCBOverhead += TCB_SIZE;
  }
  
  broadcastOutput("│  ────────────────  ─────────  ────────  ────────  ─────  ────  ─────    │");
  BROADCAST_PRINTF("│  TOTALS:           %5u     %5u      %5u                  %4u      │",
    (unsigned)(totalStackAllocated/1024),
    (unsigned)(totalStackUsed/1024),
    (unsigned)((totalStackAllocated - totalStackUsed)/1024),
    (unsigned)totalTCBOverhead);
  broadcastOutput("└────────────────────────────────────────────────────────────────────────────┘");
  
  // Memory accounting summary
  broadcastOutput("");
  broadcastOutput("┌──────────────────── MEMORY ACCOUNTING SUMMARY ─────────────────────┐");
  BROADCAST_PRINTF("│ Task Stacks:          %6u KB                                      │", (unsigned)(totalStackAllocated/1024));
  BROADCAST_PRINTF("│ Task Control Blocks:  %6u B  (%u tasks × %u bytes)            │", 
    (unsigned)totalTCBOverhead, (unsigned)taskCount, (unsigned)TCB_SIZE);
  BROADCAST_PRINTF("│ Total Task Overhead:  %6u KB                                      │", 
    (unsigned)((totalStackAllocated + totalTCBOverhead)/1024));
  BROADCAST_PRINTF("│                                                                     │");
  unsigned fragPercent = 0;
  if (heapFree > 0) {
    fragPercent = (unsigned)((largestFreeBlock * 100) / heapFree);
  }
  BROADCAST_PRINTF("│ Heap Fragmentation:   %2u%% (largest block vs free)                 │",
    fragPercent);
  BROADCAST_PRINTF("│ Task Memory Waste:    %6u KB (allocated but unused stack)         │",
    (unsigned)((totalStackAllocated - totalStackUsed)/1024));
  broadcastOutput("└─────────────────────────────────────────────────────────────────────┘");
  
  // Warnings
  bool hasWarning = false;
  broadcastOutput("");
  broadcastOutput("┌────────────────────────── WARNINGS & ALERTS ──────────────────────────┐");
  
  for (const auto& kt : knownTasks) {
    if (!kt.handle) continue;
    if (!isTaskHandleInSnapshot(kt.handle)) continue;
    // Only call expensive FreeRTOS function when needed for warning check
    if (!isDebugFlagSet(DEBUG_PERFORMANCE)) continue;
    UBaseType_t watermark = uxTaskGetStackHighWaterMark(kt.handle);
    uint32_t freePercent = (watermark * 100) / kt.stackWords;
    if (freePercent < 25) {
      BROADCAST_PRINTF("│ ⚠ %-16s CRITICAL: Only %u%% stack free!                      │", 
        kt.name, (unsigned)freePercent);
      hasWarning = true;
    }
  }
  
  if (heapFree < 40960) {
    BROADCAST_PRINTF("│ ⚠ HEAP: Only %u KB free (< 40KB threshold)                           │", 
      (unsigned)(heapFree/1024));
    hasWarning = true;
  }
  
  if (heapFree > 0 && largestFreeBlock < heapFree / 2) {
    BROADCAST_PRINTF("│ ⚠ FRAGMENTATION: Largest block %u KB vs %u KB free                    │",
      (unsigned)(largestFreeBlock/1024), (unsigned)(heapFree/1024));
    hasWarning = true;
  }
  
  if (!hasWarning) {
    broadcastOutput("│ ✓ No critical warnings - all tasks healthy                             │");
  }
  
  broadcastOutput("└─────────────────────────────────────────────────────────────────────┘");
  broadcastOutput("");
  
}
