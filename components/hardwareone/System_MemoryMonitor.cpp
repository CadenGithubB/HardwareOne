#include <esp_heap_caps.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "System_BuildConfig.h"
#include "System_Debug.h"
#include "System_MemoryMonitor.h"
#include "System_Settings.h"
#include "System_SensorStubs.h"
#include "System_Utils.h"
#include "System_TaskUtils.h"
#include "System_ESPNow.h"
#include "System_I2C.h"

// Sensor connection externs (stubs provide these when sensor disabled, actual sensor files when enabled)
#if ENABLE_IMU_SENSOR
extern bool imuConnected;
#endif
#if ENABLE_THERMAL_SENSOR
extern bool thermalConnected;
#endif
#if ENABLE_TOF_SENSOR
extern bool tofConnected;
#endif
#if ENABLE_GAMEPAD_SENSOR
extern bool gamepadConnected;
#endif
#if ENABLE_FM_RADIO
extern bool fmRadioConnected;
#endif

// External sensor enabled flags (for safe stale-handle detection)
extern bool gamepadEnabled;
extern bool thermalEnabled;
extern bool imuEnabled;
extern bool tofEnabled;
extern bool fmRadioEnabled;
extern bool gpsEnabled;
extern bool apdsColorEnabled;
extern bool apdsProximityEnabled;
extern bool apdsGestureEnabled;
extern bool presenceEnabled;
extern bool rtcEnabled;

// External task handles for stack monitoring
extern TaskHandle_t gCmdExecTaskHandle;
extern TaskHandle_t gamepadTaskHandle;
extern TaskHandle_t thermalTaskHandle;
extern TaskHandle_t imuTaskHandle;
extern TaskHandle_t tofTaskHandle;
extern TaskHandle_t fmRadioTaskHandle;
extern TaskHandle_t gpsTaskHandle;
extern TaskHandle_t apdsTaskHandle;
extern TaskHandle_t presenceTaskHandle;
extern TaskHandle_t rtcTaskHandle;

// External allocation tracker (defined in system_utils.cpp)
struct AllocEntry {
  char tag[24];
  size_t totalBytes;
  size_t psramBytes;
  size_t dramBytes;
  uint16_t count;
  bool isActive;
};
extern AllocEntry gAllocTracker[];
extern int gAllocTrackerCount;
extern bool gAllocTrackerEnabled;

// ============================================================================
// Memory Threshold Registry
// ============================================================================

// Task stack sizes (in words, 1 word = 4 bytes on ESP32)
// Centralized in System_TaskUtils.h

// Memory requirements registry
// minHeapBytes = taskStackWords * 4 (bytes per word) + overhead buffer
// Overhead buffer accounts for task control block, queue allocations, etc.
static const MemoryRequirement gMemoryRequirements[] = {
  // Component       MinHeap   TaskStack              MinPSRAM
  { "gamepad",       20480,    GAMEPAD_STACK_WORDS,   0 },       // 14KB stack + overhead
  { "thermal",       40960,    THERMAL_STACK_WORDS,   0 },       // 16KB stack + frame processing overhead
  { "imu",           24576,    IMU_STACK_WORDS,       0 },       // 16KB stack + overhead
  { "tof",           16384,    TOF_STACK_WORDS,       0 },       // 12KB stack + overhead
  { "fmradio",       20480,    FMRADIO_STACK_WORDS,   0 },       // 18KB stack + overhead
  { "presence",      16384,    PRESENCE_STACK_WORDS,  0 },       // 12KB stack + overhead
  { "apds",          16384,    APDS_STACK_WORDS,      0 },       // 12KB stack + overhead
  { "gps",           16384,    GPS_STACK_WORDS,       0 },       // 12KB stack + overhead
  { "rtc",           20480,    RTC_STACK_WORDS,       0 },       // 16KB stack + overhead
  { "espnow",        20480,    ESPNOW_HB_STACK_WORDS, 327680 }, // 16KB stack + overhead (~310KB PSRAM for state)
  { "bluetooth",     61440,    0,                     0 },       // ~60KB DRAM (BLE controller + host tasks)
};

static const size_t gMemoryRequirementsCount = sizeof(gMemoryRequirements) / sizeof(MemoryRequirement);

const MemoryRequirement* getMemoryRequirement(const char* component) {
  if (!component) return nullptr;
  
  for (size_t i = 0; i < gMemoryRequirementsCount; i++) {
    if (strcmp(gMemoryRequirements[i].component, component) == 0) {
      return &gMemoryRequirements[i];
    }
  }
  return nullptr;
}

bool checkMemoryAvailable(const char* component, String* outReason) {
  const MemoryRequirement* req = getMemoryRequirement(component);
  if (!req) {
    if (outReason) {
      char buf[96];
      snprintf(buf, sizeof(buf), "Unknown component: %s", component ? component : "(null)");
      outReason->reserve(sizeof(buf));
      *outReason = buf;
    }
    return false;
  }
  
  size_t freeHeap = ESP.getFreeHeap();
  size_t freePsram = ESP.getFreePsram();
  
  // Check heap requirement
  if (freeHeap < req->minHeapBytes) {
    if (outReason) {
      char buf[96];
      snprintf(buf, sizeof(buf), "Insufficient heap: need %uKB, have %uKB",
               (unsigned)(req->minHeapBytes / 1024),
               (unsigned)(freeHeap / 1024));
      outReason->reserve(sizeof(buf));
      *outReason = buf;
    }
    return false;
  }
  
  // Check PSRAM requirement (if any)
  if (req->minPsramBytes > 0 && freePsram < req->minPsramBytes) {
    if (outReason) {
      char buf[96];
      snprintf(buf, sizeof(buf), "Insufficient PSRAM: need %uKB, have %uKB",
               (unsigned)(req->minPsramBytes / 1024),
               (unsigned)(freePsram / 1024));
      outReason->reserve(sizeof(buf));
      *outReason = buf;
    }
    return false;
  }
  
  return true;
}

const MemoryRequirement* getAllMemoryRequirements(size_t& outCount) {
  outCount = gMemoryRequirementsCount;
  return gMemoryRequirements;
}

// ============================================================================
// Memory Sampling
// ============================================================================

// Last sample timestamp (for rate limiting periodic sampling)
static unsigned long gLastMemorySampleMs = 0;

// Heap pressure monitoring state (consolidated from main loop)
static size_t gLowestHeapSeen = UINT32_MAX;
static const size_t HEAP_WARNING_THRESHOLD = 40960;  // 40KB warning threshold

void sampleMemoryState(bool forceFullScan) {
  // ── Combined heap (DRAM + PSRAM when SPIRAM_USE_MALLOC) ──
  size_t freeHeap = ESP.getFreeHeap();
  size_t totalHeap = ESP.getHeapSize();
  size_t minFreeHeap = ESP.getMinFreeHeap();
  size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  int heapUsedPercent = totalHeap ? (int)(((totalHeap - freeHeap) * 100) / totalHeap) : 0;
  
  // ── PSRAM ──
  bool hasPsram = psramFound();
  size_t totalPsram = hasPsram ? ESP.getPsramSize() : 0;
  size_t freePsram = hasPsram ? ESP.getFreePsram() : 0;
  size_t largestPsram = hasPsram ? heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) : 0;
  int psramUsedPercent = (hasPsram && totalPsram) ? (int)(((totalPsram - freePsram) * 100) / totalPsram) : 0;
  
  // ── DRAM-specific (internal only) ──
  // Note: heap_caps_get_total_size() not available in ESP-IDF v5.3.1
  // Compute DRAM total as combined heap total minus PSRAM total
  size_t dramFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t dramTotal = totalHeap - totalPsram;
  size_t dramMinFree = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  size_t dramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int dramUsedPercent = dramTotal ? (int)(((dramTotal - dramFree) * 100) / dramTotal) : 0;
  
  // ── Heap pressure monitoring ──
  bool isNewLow = false;
  bool isPressured = false;
  if (dramFree < gLowestHeapSeen) {
    gLowestHeapSeen = dramFree;
    isNewLow = true;
  }
  if (dramFree < HEAP_WARNING_THRESHOLD) {
    isPressured = true;
  }
  
  // ── Output ──
  BROADCAST_PRINTF("[MEMSAMPLE] DRAM: %u/%u KB (%d%% used) | MinFree: %u KB | Largest: %u KB",
                   (unsigned)(dramFree / 1024), (unsigned)(dramTotal / 1024), dramUsedPercent,
                   (unsigned)(dramMinFree / 1024), (unsigned)(dramLargest / 1024));
  
  BROADCAST_PRINTF("[MEMSAMPLE] Heap(all): %u/%u KB (%d%% used) | MinFree: %u KB | Largest: %u KB",
                   (unsigned)(freeHeap / 1024), (unsigned)(totalHeap / 1024), heapUsedPercent,
                   (unsigned)(minFreeHeap / 1024), (unsigned)(largestBlock / 1024));
  
  if (hasPsram) {
    BROADCAST_PRINTF("[MEMSAMPLE] PSRAM: %u/%u KB (%d%% used) | Largest: %u KB",
                     (unsigned)(freePsram / 1024), (unsigned)(totalPsram / 1024),
                     psramUsedPercent, (unsigned)(largestPsram / 1024));
  } else {
    broadcastOutput("[MEMSAMPLE] PSRAM: Not available");
  }
  
  // ── DRAM fragmentation indicator ──
  if (dramFree > 0) {
    int fragPct = 100 - (int)((dramLargest * 100) / dramFree);
    if (fragPct > 30) {
      BROADCAST_PRINTF("[MEMSAMPLE] DRAM fragmentation: %d%% (largest_block=%u vs free=%u)",
                       fragPct, (unsigned)dramLargest, (unsigned)dramFree);
    }
  }
  
  if (isNewLow) {
    DEBUG_MEMORYF("[HEAP_MONITOR] New DRAM low: %u bytes (min_ever=%u)", 
                  (unsigned)dramFree, (unsigned)dramMinFree);
  }
  if (isPressured) {
    BROADCAST_PRINTF("[HEAP_PRESSURE] WARNING: DRAM free %u bytes (threshold=%u, min_ever=%u)",
                     (unsigned)dramFree, (unsigned)HEAP_WARNING_THRESHOLD, (unsigned)dramMinFree);
  }
  
  // ── Main loop (caller) stack watermark - always report since this is the tightest task ──
  UBaseType_t mainWatermark = uxTaskGetStackHighWaterMark(NULL);  // NULL = calling task
  BROADCAST_PRINTF("[MEMSAMPLE] MainLoop stack free=%u B%s",
                   (unsigned)(mainWatermark * 4),
                   (mainWatermark * 4 < 1024) ? " !! CRITICAL" :
                   (mainWatermark * 4 < 2048) ? " !! LOW" : "");
  
  // ── Debug queue pressure ──
  if (gDebugOutputQueue) {
    int dbgQueued = uxQueueMessagesWaiting(gDebugOutputQueue);
    int dbgFreePool = gDebugFreeQueue ? uxQueueMessagesWaiting(gDebugFreeQueue) : 0;
    int dbgTotal = gDebugQueueSize;
    int dbgPct = (dbgQueued * 100) / dbgTotal;
    unsigned long dbgDropped = gDebugDropped;
    BROADCAST_PRINTF("[MEMSAMPLE] DebugQ: %d/%d (%d%%) free_pool=%d dropped=%lu%s",
                     dbgQueued, dbgTotal, dbgPct, dbgFreePool, dbgDropped,
                     dbgPct > 75 ? " !! HIGH PRESSURE" : dbgDropped > 0 ? " (drops!)" : "");
  }
  
  // ── Per-task stack watermarks (heavy scan - run every 5th sample to reduce overhead) ──
  static uint8_t sTaskScanCounter = 0;
  bool doTaskScan = forceFullScan || (++sTaskScanCounter >= 5);  // ~every 150s at default 30s interval
  if (doTaskScan) {
    sTaskScanCounter = 0;
  }
  
  if (doTaskScan) {
    struct TaskEntry {
      const char* name;
      TaskHandle_t handle;
      uint32_t stackWords;
    };
    
    TaskHandle_t espnowHandle = getEspNowTaskHandle();
    
    const TaskEntry tasks[] = {
      {"espnow_task",        espnowHandle,          ESPNOW_HB_STACK_WORDS},
      {"cmd_exec_task",      gCmdExecTaskHandle,    CMD_EXEC_STACK_WORDS},
      {"sensor_queue_task",  queueProcessorTask,    SENSOR_QUEUE_STACK_WORDS},
      {"gamepad_task",       gamepadTaskHandle,     GAMEPAD_STACK_WORDS},
      {"thermal_task",  thermalTaskHandle,     THERMAL_STACK_WORDS},
      {"imu_task",      imuTaskHandle,         IMU_STACK_WORDS},
      {"tof_task",      tofTaskHandle,         TOF_STACK_WORDS},
      {"fmradio_task",  fmRadioTaskHandle,     FMRADIO_STACK_WORDS},
      {"gps_task",      gpsTaskHandle,         GPS_STACK_WORDS},
      {"apds_task",     apdsTaskHandle,        APDS_STACK_WORDS},
      {"presence_task", presenceTaskHandle,    PRESENCE_STACK_WORDS},
      {"rtc_task",      rtcTaskHandle,         RTC_STACK_WORDS},
    };
    
    // Enabled flags for each task — if false, handle may be stale (task self-deleted)
    const bool taskAlive[] = {
      espnowHandle != nullptr,                                        // espnow_task
      gCmdExecTaskHandle != nullptr,                                  // cmd_exec_task
      queueProcessorTask != nullptr,                                  // sensor_queue_task
      gamepadEnabled,                                                 // gamepad_task
      thermalEnabled,                                                 // thermal_task
      imuEnabled,                                                     // imu_task
      tofEnabled,                                                     // tof_task
      fmRadioEnabled,                                                 // fmradio_task
      gpsEnabled,                                                     // gps_task
      (apdsColorEnabled || apdsProximityEnabled || apdsGestureEnabled), // apds_task
      presenceEnabled,                                                // presence_task
      rtcEnabled,                                                     // rtc_task
    };
    
    bool anyTask = false;
    int taskIdx = 0;
    for (const auto& t : tasks) {
      bool alive = taskAlive[taskIdx++];
      if (!t.handle || !alive) continue;
      if (!anyTask) {
        broadcastOutput("[MEMSAMPLE] Task Stacks (name: used/total, watermark):");
        anyTask = true;
      }
      UBaseType_t watermark = uxTaskGetStackHighWaterMark(t.handle);
      uint32_t totalBytes = t.stackWords * 4;
      uint32_t usedBytes = totalBytes - (watermark * 4);
      uint32_t usedPct = totalBytes ? ((usedBytes * 100) / totalBytes) : 0;
      const char* warn = (watermark * 4 < 1024) ? " !! LOW" : ((watermark * 4 < 2048) ? " ! WARN" : "");
      BROADCAST_PRINTF("  %-14s %5u/%5u B (%2u%%) free=%5u B%s",
                       t.name, (unsigned)usedBytes, (unsigned)totalBytes,
                       (unsigned)usedPct, (unsigned)(watermark * 4), warn);
    }
  }
  
  // ── Allocation tracker summary (if enabled and has entries) ──
  if (gAllocTrackerEnabled && gAllocTrackerCount > 0) {
    size_t totalTracked = 0, totalDram = 0, totalPsramTracked = 0;
    int activeCount = 0;
    for (int i = 0; i < gAllocTrackerCount; i++) {
      if (!gAllocTracker[i].isActive) continue;
      activeCount++;
      totalTracked += gAllocTracker[i].totalBytes;
      totalDram += gAllocTracker[i].dramBytes;
      totalPsramTracked += gAllocTracker[i].psramBytes;
    }
    BROADCAST_PRINTF("[MEMSAMPLE] AllocTracker: %d entries, %u KB total (DRAM: %u KB, PSRAM: %u KB)",
                     activeCount, (unsigned)(totalTracked / 1024),
                     (unsigned)(totalDram / 1024), (unsigned)(totalPsramTracked / 1024));
    // Show top 5 by size (selection sort with temporary marking)
    bool wasActive[64];
    int limit = gAllocTrackerCount < 64 ? gAllocTrackerCount : 64;
    for (int i = 0; i < limit; i++) wasActive[i] = gAllocTracker[i].isActive;
    
    for (int shown = 0; shown < 5; shown++) {
      size_t maxBytes = 0;
      int maxIdx = -1;
      for (int i = 0; i < limit; i++) {
        if (!gAllocTracker[i].isActive) continue;
        if (gAllocTracker[i].totalBytes > maxBytes) {
          maxBytes = gAllocTracker[i].totalBytes;
          maxIdx = i;
        }
      }
      if (maxIdx < 0) break;
      BROADCAST_PRINTF("    %-20s %6u B (D:%u P:%u) x%u",
                       gAllocTracker[maxIdx].tag,
                       (unsigned)gAllocTracker[maxIdx].totalBytes,
                       (unsigned)gAllocTracker[maxIdx].dramBytes,
                       (unsigned)gAllocTracker[maxIdx].psramBytes,
                       (unsigned)gAllocTracker[maxIdx].count);
      gAllocTracker[maxIdx].isActive = false;
    }
    // Restore active flags
    for (int i = 0; i < limit; i++) gAllocTracker[i].isActive = wasActive[i];
  }
}

const char* cmd_memsample(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Check for allocation tracking subcommands
  String args = cmd;
  args.trim();
  
  if (args.startsWith("track ")) {
    String trackCmd = args.substring(6);
    trackCmd.trim();
    
    if (trackCmd == "on") {
      if (!gAllocTrackerEnabled) {
        gAllocTrackerCount = 0;
        memset(gAllocTracker, 0, 64 * sizeof(AllocEntry));
      }
      gAllocTrackerEnabled = true;
      return "Allocation tracking enabled (will track future ps_alloc calls)";
    } else if (trackCmd == "off") {
      gAllocTrackerEnabled = false;
      return "Allocation tracking disabled";
    } else if (trackCmd == "reset") {
      gAllocTrackerCount = 0;
      memset(gAllocTracker, 0, 64 * sizeof(AllocEntry));
      return "Allocation tracker reset";
    } else if (trackCmd == "status") {
      char statusBuf[256];
      snprintf(statusBuf, sizeof(statusBuf), 
               "Allocation tracking: %s | Tracked: %d allocations",
               gAllocTrackerEnabled ? "ENABLED" : "DISABLED",
               gAllocTrackerCount);
      if (gAllocTrackerCount > 0) {
        size_t total = 0;
        for (int i = 0; i < gAllocTrackerCount; i++) {
          total += gAllocTracker[i].totalBytes;
        }
        char totalBuf[128];
        snprintf(totalBuf, sizeof(totalBuf), " | Total: %lu bytes", (unsigned long)total);
        strncat(statusBuf, totalBuf, sizeof(statusBuf) - strlen(statusBuf) - 1);
      }
      broadcastOutput(statusBuf);
      return "[Memory] Tracking status displayed";
    } else {
      return "Usage: memsample track [on|off|reset|status]";
    }
  }
  
  // Default: show memory sample (force full task scan for manual CLI requests)
  sampleMemoryState(true);
  return "[Memory] Sample displayed";
}

void periodicMemorySample() {
  // Only sample if debug flag is enabled
  if (!isDebugFlagSet(DEBUG_MEMORY)) {
    return;
  }
  
  // Check if sampling is disabled (0 = disabled)
  extern Settings gSettings;
  if (gSettings.memorySampleIntervalSec <= 0) {
    return;
  }
  
  // Rate limit based on user-configurable interval
  unsigned long now = millis();
  unsigned long intervalMs = gSettings.memorySampleIntervalSec * 1000UL;
  if (now - gLastMemorySampleMs < intervalMs) {
    return;
  }
  
  gLastMemorySampleMs = now;
  sampleMemoryState();
}

// ============================================================================
// OLED Memory Stats Display (merged from oled_memory_stats.cpp)
// ============================================================================

#include "System_BuildConfig.h"

#if ENABLE_OLED_DISPLAY

// displayMemoryStats() moved to OLED_Mode_System.cpp

#endif // ENABLE_OLED_DISPLAY
