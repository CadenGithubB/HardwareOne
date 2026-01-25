#include <esp_heap_caps.h>
#include <string.h>

#include "System_BuildConfig.h"
#include "System_Debug.h"
#include "System_MemoryMonitor.h"
#include "System_Settings.h"
#include "System_SensorStubs.h"
#include "System_Utils.h"

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
// These match the values defined in task_utils.cpp
#define GAMEPAD_STACK_WORDS  4096   // ~16KB
#define THERMAL_STACK_WORDS  4096   // ~16KB
#define IMU_STACK_WORDS      4096   // ~16KB
#define TOF_STACK_WORDS      3072   // ~12KB
#define FMRADIO_STACK_WORDS  4608   // ~18KB
#define PRESENCE_STACK_WORDS 3072   // ~12KB

// Memory requirements registry
// minHeapBytes = taskStackWords * 4 (bytes per word) + overhead buffer
// Overhead buffer accounts for task control block, queue allocations, etc.
static const MemoryRequirement gMemoryRequirements[] = {
  // Component       MinHeap   TaskStack  MinPSRAM
  { "gamepad",       20480,    GAMEPAD_STACK_WORDS,  0 },      // 16KB stack + 4KB overhead
  { "thermal",       40960,    THERMAL_STACK_WORDS,  0 },      // 16KB stack + 24KB overhead (frame processing)
  { "imu",           24576,    IMU_STACK_WORDS,      0 },      // 16KB stack + 8KB overhead
  { "tof",           16384,    TOF_STACK_WORDS,      0 },      // 12KB stack + 4KB overhead
  { "fmradio",       20480,    FMRADIO_STACK_WORDS,  0 },      // 18KB stack + 2KB overhead
  { "presence",      16384,    PRESENCE_STACK_WORDS, 0 },      // 12KB stack + 4KB overhead
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

void sampleMemoryState() {
  // Gather all memory stats in one go
  size_t freeHeap = ESP.getFreeHeap();
  size_t totalHeap = ESP.getHeapSize();
  size_t minFreeHeap = ESP.getMinFreeHeap();
  size_t maxAllocHeap = ESP.getMaxAllocHeap();
  size_t largestBlock = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  
  bool hasPsram = psramFound();
  size_t totalPsram = hasPsram ? ESP.getPsramSize() : 0;
  size_t freePsram = hasPsram ? ESP.getFreePsram() : 0;
  size_t largestPsram = hasPsram ? heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) : 0;
  
  // Calculate percentages
  int heapUsedPercent = ((totalHeap - freeHeap) * 100) / totalHeap;
  int psramUsedPercent = hasPsram ? (((totalPsram - freePsram) * 100) / totalPsram) : 0;
  
  // Heap pressure monitoring (consolidated - was separate in main loop)
  bool isNewLow = false;
  bool isPressured = false;
  if (freeHeap < gLowestHeapSeen) {
    gLowestHeapSeen = freeHeap;
    isNewLow = true;
  }
  if (freeHeap < HEAP_WARNING_THRESHOLD) {
    isPressured = true;
  }
  
  // Output comprehensive memory snapshot
  BROADCAST_PRINTF("[MEMSAMPLE] Heap: %lu/%lu KB (%d%% used) | Min: %lu KB | MaxAlloc: %lu KB | Largest: %lu KB",
                   (unsigned long)(freeHeap / 1024),
                   (unsigned long)(totalHeap / 1024),
                   heapUsedPercent,
                   (unsigned long)(minFreeHeap / 1024),
                   (unsigned long)(maxAllocHeap / 1024),
                   (unsigned long)(largestBlock / 1024));
  
  // Heap pressure warnings (consolidated from main loop)
  if (isNewLow) {
    DEBUG_MEMORYF("[HEAP_MONITOR] New low: %u bytes (min_ever=%u)", 
                  (unsigned)freeHeap, (unsigned)minFreeHeap);
  }
  if (isPressured) {
    DEBUG_MEMORYF("[HEAP_PRESSURE] WARNING: Free heap %u bytes (threshold=%u, min_ever=%u)",
                  (unsigned)freeHeap, (unsigned)HEAP_WARNING_THRESHOLD, (unsigned)minFreeHeap);
  }
  
  if (hasPsram) {
    BROADCAST_PRINTF("[MEMSAMPLE] PSRAM: %lu/%lu KB (%d%% used) | Largest: %lu KB",
                     (unsigned long)(freePsram / 1024),
                     (unsigned long)(totalPsram / 1024),
                     psramUsedPercent,
                     (unsigned long)(largestPsram / 1024));
  } else {
    broadcastOutput("[MEMSAMPLE] PSRAM: Not available");
  }
  
  // Show component memory requirements vs available (only for connected sensors)
  bool anyShown = false;
  for (size_t i = 0; i < gMemoryRequirementsCount; i++) {
    const MemoryRequirement* req = &gMemoryRequirements[i];
    
    // Skip sensors that aren't connected
    bool isConnected = false;
    if (strcmp(req->component, "gamepad") == 0) {
      isConnected = gamepadConnected;
    } else if (strcmp(req->component, "thermal") == 0) {
      isConnected = thermalConnected;
    } else if (strcmp(req->component, "imu") == 0) {
      isConnected = imuConnected;
    } else if (strcmp(req->component, "tof") == 0) {
      isConnected = tofConnected;
    } else if (strcmp(req->component, "fmradio") == 0) {
      isConnected = fmRadioConnected;
    }
    
    if (!isConnected) continue;
    
    if (!anyShown) {
      broadcastOutput("[MEMSAMPLE] Component Requirements:");
      anyShown = true;
    }
    
    bool canStart = checkMemoryAvailable(req->component, nullptr);
    const char* status = canStart ? "OK" : "INSUFFICIENT";
    
    BROADCAST_PRINTF("  %-10s: %lu KB heap (stack: %u words) - %s",
                     req->component,
                     (unsigned long)(req->minHeapBytes / 1024),
                     (unsigned)req->taskStackWords,
                     status);
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
  
  // Default: show memory sample
  sampleMemoryState();
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
