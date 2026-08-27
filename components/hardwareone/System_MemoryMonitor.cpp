#include <esp_heap_caps.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "System_BuildConfig.h"
#include "System_Debug.h"
#include "System_MemoryMonitor.h"
#include "System_MemTracker.h"
#include "System_MemUtil.h"
#include "System_Settings.h"
#include "System_SensorStubs.h"
#include "System_Utils.h"
#include "System_TaskUtils.h"
#include "System_ESPNow.h"
#include "System_I2C.h"
#include "HAL_Input.h"          // For INPUT_TASK_NAME (gamepad vs ANO)

// Per-sensor enabled/connected flags + task handles come from the sensor
// headers (which gate their own decls on ENABLE_*_SENSOR). System_SensorStubs.h
// (included above) provides stubs for sensors compiled out.
#if ENABLE_IMU_SENSOR
#include "i2csensor_bno055.h"
#endif
#if ENABLE_THERMAL_SENSOR
#include "i2csensor_mlx90640.h"
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor_vl53l4cx.h"
#endif
#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor_seesaw.h"
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

// Command-exec task handle is NOT a sensor — declared separately.
extern TaskHandle_t gCmdExecTaskHandle;

// ============================================================================
// Memory Threshold Registry
// ============================================================================

// Task stack sizes are in BYTES (ESP-IDF's xTaskCreate deviates from vanilla
// FreeRTOS; StackType_t is uint8_t here). Centralized in System_TaskUtils.h —
// the *_STACK_WORDS names there are a misnomer, the values are byte counts.

// Memory requirements registry
// minHeapBytes = the task's stack (BYTES) + an overhead buffer for the task
// control block, queue allocations, etc.
// NOTE: the minHeap figures below were originally derived assuming stacks were
// words (i.e. 4x their real size), so they are ~4x more conservative than
// needed — e.g. gamepad gates on 20480 B for a 3584 B stack. Left as-is
// deliberately: they only gate feature start-up and erring high is safe.
// Re-tune them against measured usage if a feature is ever wrongly refused.
static const MemoryRequirement gMemoryRequirements[] = {
  // Component       MinHeap   TaskStack              MinPSRAM
  { "gamepad",       20480,    INPUT_STACK_WORDS,   0 },       // 14KB stack + overhead
  { "anoencoder",    20480,    INPUT_STACK_WORDS,   0 },       // 14KB stack + overhead (same seesaw lib + INPUT_STACK_WORDS as gamepad)
  { "thermal",       49152,    THERMAL_STACK_WORDS,   0 },       // 24KB stack + frame processing overhead
  { "imu",           24576,    IMU_STACK_WORDS,       0 },       // 16KB stack + overhead
  { "tof",           16384,    TOF_STACK_WORDS,       0 },       // 12KB stack + overhead
  { "fmradio",       20480,    FMRADIO_STACK_WORDS,   0 },       // 18KB stack + overhead
  { "presence",      16384,    PRESENCE_STACK_WORDS,  0 },       // 12KB stack + overhead
  { "apds",          16384,    APDS_STACK_WORDS,      0 },       // 12KB stack + overhead
  { "gps",           16384,    GPS_STACK_WORDS,       0 },       // 12KB stack + overhead
  { "rtc",           20480,    RTC_STACK_WORDS,       0 },       // 16KB stack + overhead
  { "espnow",        20480,    ESPNOW_HB_STACK_WORDS, 327680 }, // 16KB stack + overhead (~310KB PSRAM for state)
  // 84 KB. initG2Client is the most expensive operation in this firmware and was
  // COMPLETELY UNGATED until now -- the "bluetooth" row below is only consulted
  // from initBluetooth (server path), never from the client path.
  // MEASURED on feather_esp32_v2, one boot, probes 587 ms apart:
  //   pre-BLEDevice::init   148,115
  //   post-BLEDevice::init  104,843   -> BLE core   43,272
  //   initG2Client COMPLETE  67,135   -> G2 workers 37,708
  //   TOTAL                                        80,980
  // The worker figure reconciles with the four stacks it creates -- g2_page_swap_w
  // 8192 + g2_tap_disp 10240 + g2_session_w 10240 + r1_owner 6144 = 34,816, plus
  // 2,892 of TCBs/queues -- all MALLOC_CAP_INTERNAL|8BIT and unable to fall back
  // to PSRAM. The BLE-core figure independently matches openble's 42,944 (3 runs).
  //
  // 86016 = 80,980 + ~5 KB. NOTE this is deliberately a tight floor: it still
  // admits g2init on a WiFi+HTTP device (~95.8 KB free), which lands near 9 KB and
  // has been observed to fail afterwards at openg2. Raising this toward ~102,400
  // would refuse that combination outright.
  { "g2client",      86016,    0,                     0 },       // 84KB DRAM (BLE core 43K + 4 G2 worker stacks 38K)
  // 52 KB, against an HONEST INTERNAL|8BIT reading. MEASURED on feather_esp32_v2,
  // not estimated: BLE init costs 42,944 B (four runs, 4-byte spread) --
  //   g2init at boot   148,115 -> 104,843
  //   openble x3        107,383 ->  64,439 / 95,931 -> 52,987 / 84,559 -> 41,611
  // 53248 = 42,944 init + ~10 KB survivable floor.
  //
  // Why this exact value: openble/closeble leaks 11,192 B per cycle (the firmware
  // counts it itself, "~11KB this cycle"), and the largest free block drops
  // ~10 KB per cycle. By cycle 5 the gate sees free=51,163 but largest=8,192 --
  // below BTC_TASK's 8,704 B contiguous stack -- so init fails there. 53248
  // refuses at cycle 5, the first cycle that genuinely cannot complete. The
  // previous 46080 let cycle 5 through into that failure.
  //
  // Not a false-refusal risk: the check sits after deinitG2Client + deinitChecked,
  // and was MEASURED at 107,383 B free with G2 previously connected.
  { "bluetooth",     53248,    0,                     0 },       // 52KB DRAM (BLE controller + host tasks)
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
  
  // Internal 8-bit RAM only. ESP.getFreeHeap() is MALLOC_CAP_INTERNAL with no
  // MALLOC_CAP_8BIT, so on ESP32 classic it read ~25.8 KB high (the IRAM-only
  // heap nothing can malloc from) and every tier below was effectively that much
  // looser than its stated number. See hw1InternalFreeBytes() in System_MemUtil.h.
  // The rows were sized against the inflated reading, so any row NOT retuned
  // alongside this change becomes ~25.8 KB stricter in real terms.
  size_t freeHeap = hw1InternalFreeBytes();
  size_t freePsram = ESP.getFreePsram();  // correct as-is: MALLOC_CAP_SPIRAM
  
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
// 25 KB. Was 40960, which was implicitly calibrated against ESP.getFreeHeap()'s
// inflated reading (~25.8 KB high on ESP32 classic — see hw1InternalFreeBytes()
// in System_MemUtil.h). Once the samplers became honest, 40960 sat ABOVE normal
// operating level and warned every 10 s for as long as the G2 glasses were
// connected, which trains the reader to ignore it.
// Measured on feather_esp32_v2 (honest INTERNAL|8BIT figures):
//   boot, WiFi + HTTP, no BLE ......... 95,927 B
//   after openble (server mode) ....... 33,435 B
//   after g2init (client + G2) ........ 26,495 B
//   G2 connected, hijack page open .... 21,299 B  <- still warns at 25 KB, intended
static const size_t HEAP_WARNING_THRESHOLD = 25600;  // 25KB warning threshold

void sampleMemoryState(bool forceFullScan) {
  // ── Internal heap (NOT combined — PSRAM is reported separately below) ──
  // Internal 8-bit RAM only. Previously: ESP.* (MALLOC_CAP_INTERNAL, no 8BIT)
  // folded in the unusable IRAM-only heap, and largestBlock used bare
  // MALLOC_CAP_8BIT which ALSO matches PSRAM — so it reported the ~1.5 MB PSRAM
  // block as if it were internal. See hw1InternalFreeBytes() in System_MemUtil.h.
  size_t freeHeap = hw1InternalFreeBytes();
  size_t totalHeap = hw1InternalTotalBytes();
  size_t minFreeHeap = hw1InternalMinFreeBytes();
  size_t largestBlock = hw1InternalLargestBlock();
  int heapUsedPercent = (totalHeap && totalHeap > freeHeap)
                          ? (int)(((totalHeap - freeHeap) * 100) / totalHeap) : 0;
  
  // ── PSRAM ──
  bool hasPsram = psramFound();
  size_t totalPsram = hasPsram ? ESP.getPsramSize() : 0;
  size_t freePsram = hasPsram ? ESP.getFreePsram() : 0;
  size_t largestPsram = hasPsram ? heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) : 0;
  int psramUsedPercent = (hasPsram && totalPsram) ? (int)(((totalPsram - freePsram) * 100) / totalPsram) : 0;
  
  // ── DRAM-specific (internal only) ──
  // Was: dramTotal = totalHeap - totalPsram, with the free/min/largest trio
  // re-querying heap_caps_* directly. That subtraction was only correct while
  // totalHeap came from ESP.getHeapSize(), which under CONFIG_SPIRAM_USE_MALLOC
  // folds PSRAM into the total. v0.99.9 switched totalHeap to the honest
  // internal-only hw1InternalTotalBytes() but left the subtraction behind, so
  // it underflowed size_t on every PSRAM board: xiao_s3 printed
  // "DRAM: 101/4186839 KB" (= 2^32 - ~7.9 MB of PSRAM, / 1024).
  //
  // The trio was already querying MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT — exactly
  // what the hw1Internal* helpers above wrap — so this is the same pool the
  // freeHeap/totalHeap group reports. Reuse those values: one source of truth,
  // and four fewer walks of the heap under its lock per sample.
  size_t dramFree = freeHeap;
  size_t dramTotal = totalHeap;
  size_t dramMinFree = minFreeHeap;
  size_t dramLargest = largestBlock;
  int dramUsedPercent = (dramTotal && dramTotal > dramFree)
                          ? (int)(((dramTotal - dramFree) * 100) / dramTotal) : 0;
  
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
    DEBUG_MEMORY_HEAPF("[HEAP_MONITOR] New DRAM low: %u bytes (min_ever=%u)", 
                  (unsigned)dramFree, (unsigned)dramMinFree);
  }
  if (isPressured) {
    BROADCAST_PRINTF("[HEAP_PRESSURE] WARNING: DRAM free %u bytes (threshold=%u, min_ever=%u)",
                     (unsigned)dramFree, (unsigned)HEAP_WARNING_THRESHOLD, (unsigned)dramMinFree);
  }
  
  // ── Main loop (caller) stack watermark - always report since this is the tightest task ──
  // HWM is in BYTES on this port (StackType_t is uint8_t). The old "* 4" both
  // inflated the reported figure AND made these thresholds 4x LESS sensitive —
  // CRITICAL only fired below 256 B free instead of the intended 1024 B, so
  // genuinely tight stacks reported clean. See System_TaskUtils.h.
  UBaseType_t mainWatermark = uxTaskGetStackHighWaterMark(NULL);  // NULL = calling task
  BROADCAST_PRINTF("[MEMSAMPLE] MainLoop stack free=%u B%s",
                   (unsigned)mainWatermark,
                   (mainWatermark < 1024) ? " !! CRITICAL" :
                   (mainWatermark < 2048) ? " !! LOW" : "");
  
  // ── Debug queue pressure ──
  if (gDebugOutputQueue) {
    int dbgQueued = uxQueueMessagesWaiting(gDebugOutputQueue);
    int dbgFreePool = gDebugFreeQueue ? uxQueueMessagesWaiting(gDebugFreeQueue) : 0;
    int dbgTotal = gDebugPoolSize;
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
    
#if ENABLE_ESPNOW
    TaskHandle_t espnowHandle = nullptr;
    UBaseType_t espnowWatermark = 0;
    const bool espnowSnapshotValid =
        getEspNowTaskStackSnapshot(&espnowHandle, &espnowWatermark);
#else
    TaskHandle_t espnowHandle = nullptr;
    UBaseType_t espnowWatermark = 0;
    const bool espnowSnapshotValid = false;
#endif
    
    const TaskEntry tasks[] = {
      {"espnow_task",        espnowHandle,          ESPNOW_HB_STACK_WORDS},
      {"cmd_exec_task",      gCmdExecTaskHandle,    CMD_EXEC_STACK_WORDS},
      {"sensor_queue_task",  queueProcessorTask,    SENSOR_QUEUE_STACK_WORDS},
      {INPUT_TASK_NAME,      gInputTaskHandle,     INPUT_STACK_WORDS},
      {"thermal_task",  gThermalTaskHandle,     THERMAL_STACK_WORDS},
      {"imu_task",      gImuTaskHandle,         IMU_STACK_WORDS},
      {"tof_task",      gTofTaskHandle,         TOF_STACK_WORDS},
      {"fmradio_task",  gFmRadioTaskHandle,     FMRADIO_STACK_WORDS},
      {"gps_task",      gGpsTaskHandle,         GPS_STACK_WORDS},
      {"apds_task",     gApdsTaskHandle,        APDS_STACK_WORDS},
      {"presence_task", gPresenceTaskHandle,    PRESENCE_STACK_WORDS},
      {"rtc_task",      gRtcTaskHandle,         RTC_STACK_WORDS},
    };
    
    // Enabled flags for each task — if false, handle may be stale (task self-deleted)
    const bool taskAlive[] = {
      espnowSnapshotValid,                                            // espnow_task
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
    
    bool anyTask = false;
    int taskIdx = 0;
    for (const auto& t : tasks) {
      bool alive = taskAlive[taskIdx++];
      if (!t.handle || !alive) continue;
      if (!anyTask) {
        broadcastOutput("[MEMSAMPLE] Task Stacks (name: used/total, watermark):");
        anyTask = true;
      }
      // BYTES throughout: t.stackWords holds the byte count handed to
      // xTaskCreate, and the HWM is bytes too. The old "* 4" inflated every
      // absolute figure 4x (percentages stayed right, since both terms scaled)
      // AND made these warnings 4x less sensitive — LOW only fired below 256 B
      // free. See System_TaskUtils.h.
      UBaseType_t watermark = (taskIdx == 1)
          ? espnowWatermark
          : uxTaskGetStackHighWaterMark(t.handle);
      uint32_t totalBytes = t.stackWords;
      uint32_t freeBytes  = (uint32_t)watermark;
      uint32_t usedBytes = totalBytes - freeBytes;
      uint32_t usedPct = totalBytes ? ((usedBytes * 100) / totalBytes) : 0;
      const char* warn = (freeBytes < 1024) ? " !! LOW" : ((freeBytes < 2048) ? " ! WARN" : "");
      BROADCAST_PRINTF("  %-14s %5u/%5u B (%2u%%) free=%5u B%s",
                       t.name, (unsigned)usedBytes, (unsigned)totalBytes,
                       (unsigned)usedPct, (unsigned)freeBytes, warn);
    }
  }
  
  // ── Allocation traffic (cumulative since tracker reset, not live bytes) ──
  static constexpr size_t kMemSampleTopCount = 5;
  MemTrackerSnapshot tracker{};
  MemTrackerEntry top[kMemSampleTopCount]{};
  if (memTrackerSnapshot(tracker, top, kMemSampleTopCount,
                         pdMS_TO_TICKS(25)) &&
      (tracker.enabled || tracker.entryCount > 0 ||
       tracker.contentionDrops > 0 || tracker.invalidTagEvents > 0)) {
    BROADCAST_PRINTF("[MEMSAMPLE] AllocTracker: %s, %u/%u tags, %llu attempts",
                     tracker.enabled ? "ON" : "OFF",
                     (unsigned)tracker.entryCount, (unsigned)tracker.capacity,
                     (unsigned long long)(tracker.successCount +
                                          tracker.failureCount));
    BROADCAST_PRINTF("  cumulative requested: %llu B (DRAM:%llu PSRAM:%llu)",
                     (unsigned long long)(tracker.dramBytes + tracker.psramBytes),
                     (unsigned long long)tracker.dramBytes,
                     (unsigned long long)tracker.psramBytes);
    BROADCAST_PRINTF("  failures:%llu fallbacks:%llu drops(lock:%llu invalid:%llu full:%llu)",
                     (unsigned long long)tracker.failureCount,
                     (unsigned long long)tracker.fallbackCount,
                     (unsigned long long)tracker.contentionDrops,
                     (unsigned long long)tracker.invalidTagEvents,
                     (unsigned long long)tracker.overflowEvents);
    for (size_t i = 0; i < tracker.topCount; ++i) {
      BROADCAST_PRINTF("    %-31.31s %8llu B (D:%llu P:%llu) ok:%llu fail:%llu fb:%llu",
                       top[i].tag,
                       (unsigned long long)(top[i].dramBytes + top[i].psramBytes),
                       (unsigned long long)top[i].dramBytes,
                       (unsigned long long)top[i].psramBytes,
                       (unsigned long long)top[i].successCount,
                       (unsigned long long)top[i].failureCount,
                       (unsigned long long)top[i].fallbackCount);
    }
  }
}

const char* cmd_memsample(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Check for allocation tracking subcommands
  String args = argsInput;
  args.trim();
  
  if (args.startsWith("track ")) {
    String trackCmd = args.substring(6);
    trackCmd.trim();
    
    if (trackCmd == "on") {
      return memTrackerSetEnabled(true)
          ? "Allocation tracking enabled (cumulative window preserved)"
          : "Error: allocation tracker unavailable";
    } else if (trackCmd == "off") {
      return memTrackerSetEnabled(false)
          ? "Allocation tracking disabled (cumulative window preserved)"
          : "Error: allocation tracker unavailable";
    } else if (trackCmd == "reset") {
      return memTrackerReset()
          ? "Allocation tracker cumulative window reset"
          : "Error: allocation tracker unavailable";
    } else if (trackCmd == "status") {
      MemTrackerSnapshot tracker{};
      if (!memTrackerSnapshot(tracker)) {
        return "Error: allocation tracker unavailable";
      }
      char statusBuf[256];
      snprintf(statusBuf, sizeof(statusBuf),
               "Allocation tracking: %s | Tags: %u/%u | Attempts: %llu | Cumulative: %llu bytes",
               tracker.enabled ? "ENABLED" : "DISABLED",
               (unsigned)tracker.entryCount, (unsigned)tracker.capacity,
               (unsigned long long)(tracker.successCount +
                                    tracker.failureCount),
               (unsigned long long)(tracker.dramBytes + tracker.psramBytes));
      broadcastOutput(statusBuf);
      BROADCAST_PRINTF("[Memory] DRAM:%llu PSRAM:%llu failures:%llu fallbacks:%llu",
                       (unsigned long long)tracker.dramBytes,
                       (unsigned long long)tracker.psramBytes,
                       (unsigned long long)tracker.failureCount,
                       (unsigned long long)tracker.fallbackCount);
      BROADCAST_PRINTF("[Memory] Drops lock:%llu invalid:%llu full:%llu generation:%u",
                       (unsigned long long)tracker.contentionDrops,
                       (unsigned long long)tracker.invalidTagEvents,
                       (unsigned long long)tracker.overflowEvents,
                       (unsigned)tracker.generation);
      return "[Memory] Tracking status displayed";
    } else {
      return "Error: invalid arguments — Usage: memsample track [on|off|reset|status]";
    }
  }
  
  // Default: show memory sample (force full task scan for manual CLI requests)
  sampleMemoryState(true);
  return "[Memory] Sample displayed";
}

void periodicMemorySample() {
  // ── Tier 1 (always-on): cheap DRAM-pressure watch ────────────────────────
  // Runs regardless of DEBUG_MEMORY so a shipping device still warns *before*
  // it OOMs. Just an O(num_heaps) free-size query + a compare (no
  // largest-free-block walk — that stays in the verbose path below). Checked
  // at most once per second; the WARN line is rate-limited to one per 10 s so
  // it can never flood the output. Also keeps gLowestHeapSeen current between
  // verbose samples.
  {
    static unsigned long lastPressureCheckMs = 0;
    unsigned long nowMs = millis();
    if (lastPressureCheckMs == 0 || (nowMs - lastPressureCheckMs) >= 1000UL) {
      lastPressureCheckMs = nowMs;
      size_t dramFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
      if (dramFree < gLowestHeapSeen) gLowestHeapSeen = dramFree;
      if (dramFree < HEAP_WARNING_THRESHOLD) {
        static unsigned long lastPressureWarnMs = 0;
        if (lastPressureWarnMs == 0 || (nowMs - lastPressureWarnMs) >= 10000UL) {
          lastPressureWarnMs = nowMs;
          BROADCAST_PRINTF("[HEAP_PRESSURE] WARNING: DRAM free %u B (threshold=%u, min_ever=%u)",
                           (unsigned)dramFree, (unsigned)HEAP_WARNING_THRESHOLD, (unsigned)gLowestHeapSeen);
        }
      }
    }
  }

  // ── Tier 2 (DEBUG_MEMORY): full verbose sample ───────────────────────────
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
