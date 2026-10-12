#include "i2csensor_vl53l4cx.h"
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_BuildConfig.h"
#include "System_MemoryMonitor.h"
#include "System_Utils.h"

#if ENABLE_TOF_SENSOR

#include <Arduino.h>
#include <freertos/semphr.h>
#include <vl53l4cx_class.h>
#include <Wire.h>
#include <new>

#include "OLED_Display.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_MemUtil.h"
#include "System_Mutex.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"

// VL53L4CX ToF sensor object (owned by this module)
static VL53L4CX* gVL53L4CX = nullptr;

// Settings and debug

// I2C functions - clock now managed by transaction wrapper

// External dependencies provided by System_I2C.h:
// sensorStatusBumpWith, gSensorPollingPaused, drainDebugRing

// ============================================================================
// ToF Sensor Cache (owned by this module)
// ============================================================================
// The mutex is retained across stops; readers never own the live storage.
static TofCache sTofCache{};
static SemaphoreHandle_t sTofCacheMutex = nullptr;

bool tofCacheSnapshot(TofCache& out, uint32_t timeoutMs) {
  out = {};
  SensorCacheGuard guard(sTofCacheMutex, pdMS_TO_TICKS(timeoutMs), "tof.snapshot");
  if (!guard.held) return false;
  out = sTofCache;
  return true;
}

size_t tofCacheStorageBytes() {
  return sizeof(sTofCache) + sizeof(sTofCacheMutex);
}

static bool tofInvalidateCache() {
  SensorCacheGuard guard(sTofCacheMutex, pdMS_TO_TICKS(50), "tof.invalidate");
  if (!guard.held) return false;
  sTofCache.tofDataValid = false;
  sTofCache.tofTotalObjects = 0;
  for (auto& object : sTofCache.tofObjects) {
    object.detected = false;
    object.valid = false;
  }
  return true;
}

// Debug macros (use centralized versions from debug_system.h)
// MIN_RESTART_DELAY_MS defined in System_I2C.h

// ToF sensor state (definitions)
bool gTofRunning = false;
bool gTofConnected = false;
uint32_t gTofLastStopTime = 0;
TaskHandle_t gTofTaskHandle = nullptr;

// ToF watermark tracking
volatile UBaseType_t gTofWatermarkMin = (UBaseType_t)0xFFFFFFFF;
volatile UBaseType_t gTofWatermarkNow = (UBaseType_t)0;

// Forward declarations (implementations in main .ino)
extern bool tofInit();
extern bool createToFTask();

// Queue system functions now in System_I2C.h

// ============================================================================
// ToF Sensor Command Handlers
// ============================================================================

const char* cmd_tof(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    if (!ensureDebugBuffer()) return SENSOR_JSON_NOBUF;
    int n = tofBuildDataJSON(getDebugBuffer(), 1024);  // shared builder (also feeds sensors json / MQTT)
    return (n > 0) ? getDebugBuffer() : SENSOR_JSON_UNAVAILABLE;
  }

  if (!gTofRunning) return "Error: [ToF] Sensor stopped. Use 'opentof' first.";
  if (!gTofConnected) return "Error: [ToF] Sensor not connected. Check wiring.";

  TofCache reading;
  if (!tofCacheSnapshot(reading, CACHE_MUTEX_TIMEOUT_MS)) {
    return "Error: [ToF] Reading cache busy or unavailable. Try again.";
  }
  if (!reading.tofDataValid) return "Error: [ToF] Reading not yet available.";

  // Use the same filtered/smoothed published distances as JSON. Reading a
  // snapshot neither waits for a new hardware sample nor clears its interrupt.
  int nearestMm = 0;
  for (const auto& object : reading.tofObjects) {
    if (!object.detected || !object.valid || object.distance_mm <= 0) continue;
    if (nearestMm == 0 || object.distance_mm < nearestMm) nearestMm = object.distance_mm;
  }
  if (nearestMm == 0) return "Error: [ToF] No valid detected object in the latest reading.";

  BROADCAST_PRINTF("Distance: %.1f cm", nearestMm / 10.0f);
  return "[ToF] Reading complete";
}

// Internal function called by queue processor
bool tofStartInternal() {
  if (gTofRunning) return true;
  // A stopped task owns its resources until cleanup publishes a null handle.
  if (gTofTaskHandle != nullptr) return false;

  // Check if too soon after stop (prevent rapid restart crashes)
  if (gTofLastStopTime > 0) {
    unsigned long timeSinceStop = millis() - gTofLastStopTime;
    if (timeSinceStop < MIN_RESTART_DELAY_MS) {
      DEBUG_CLIF("ToF sensor stopped recently, waiting before restart");
      return false;
    }
  }

  // Check memory before creating task
  if (!checkMemoryAvailable("tof", nullptr)) {
    DEBUG_CLIF("Insufficient memory for ToF sensor");
    return false;
  }

  // Create cache mutex if not already created
  if (!sTofCacheMutex) {
    sTofCacheMutex = xSemaphoreCreateMutex();
    if (!sTofCacheMutex) {
      ERROR_TOFF("Failed to create cache mutex");
      return false;
    }
    DEBUG_TOF_LIFECYCLEF("[TOF] Cache mutex created");
  }

  // Clean up any stale cache from previous run BEFORE starting
  {
    SensorCacheGuard g(sTofCacheMutex, pdMS_TO_TICKS(100), "tof.cleanStaleCache");
    if (!g.held) return false;
    sTofCache.tofDataValid = false;
    sTofCache.tofTotalObjects = 0;
    for (int j = 0; j < 4; j++) {
      sTofCache.tofObjects[j].detected = false;
      sTofCache.tofObjects[j].valid = false;
      sTofCache.tofObjects[j].distance_mm = 0;
      sTofCache.tofObjects[j].distance_cm = 0.0;
      sTofCache.tofObjects[j].status = 0;
      sTofCache.tofObjects[j].smoothed_distance_mm = 0.0;
      sTofCache.tofObjects[j].smoothed_distance_cm = 0.0;
      sTofCache.tofObjects[j].hasHistory = false;
    }
  }

  // Set gTofRunning FIRST to prevent race condition with task cleanup code
  gTofRunning = true;
  INFO_TOF_LIFECYCLEF("Set gTofRunning=1 BEFORE init to prevent race condition");

  // Initialize ToF sensor synchronously (like thermal sensor)
  if (!gTofConnected || gVL53L4CX == nullptr) {
    // Try initialization with retry
    bool initSuccess = false;
    for (int attempt = 0; attempt < 2 && !initSuccess; attempt++) {
      if (attempt > 0) {
        delay(200);  // Brief delay between attempts
      }
      initSuccess = tofInit();
    }

    if (!initSuccess) {
      // Ensure ToF stays disabled on init failure
      gTofRunning = false;
      gTofConnected = false;
      DEBUG_CLIF("Failed to initialize VL53L4CX ToF sensor (tried 2x)");
      return false;
    }
  }

  // Create ToF task lazily
  if (!createToFTask()) {
    DEBUG_CLIF("Failed to create ToF task");
    gTofRunning = false;
    gTofConnected = false;
    if (gVL53L4CX != nullptr) {
      i2cDeviceTransactionVoid((uint8_t)gSettings.tofBus, I2C_ADDR_TOF, 100000, 500, [&]() {
        (void)gVL53L4CX->VL53L4CX_StopMeasurement();
      });
      ps_delete(gVL53L4CX);
      gVL53L4CX = nullptr;
    }
    return false;
  }
  // Clock is now managed automatically by i2cTaskWithStandardTimeout wrapper
  // Device registration specifies ToF's clock speed (50-400kHz)
  // gTofRunning already set to true at the beginning to prevent race condition
  sensorStatusBumpWith("opentof@queue");
  DEBUG_CLIF("SUCCESS: ToF sensor started successfully");
  
  
  return true;
}

// Public command - uses centralized queue
const char* cmd_tofstart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cmd_sensorstart_queued(I2C_DEVICE_TOF, "ToF", "opentof@enqueue",
      "the sensor opens in the background - read a distance with 'tofread' once it is up");
}

const char* cmd_tofstop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!requestSensorStop(I2C_DEVICE_TOF)) return "Error: [ToF] Could not queue stop";
  return "[ToF] Stop requested; cleanup will complete asynchronously";
}

const char* cmd_toftransitionms(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: toftransitionms <0..5000>";
  int v = _arg.toInt();
  if (v < 0 || v > 5000) return "Error: [ToF] Transition time must be 0-5000ms";
  setSetting(gSettings.tofTransitionMs, v);
  BROADCAST_PRINTF("tofTransitionMs set to %d", v);
  return "[ToF] Setting updated";
}

const char* cmd_tofmaxdistancemm(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: tofmaxdistancemm <100..10000>";
  int v = _arg.toInt();
  if (v < 100 || v > 10000) return "Error: [ToF] Max distance must be 100-10000mm";
  setSetting(gSettings.tofUiMaxDistanceMm, v);
  BROADCAST_PRINTF("tofUiMaxDistanceMm set to %d", v);
  return "[ToF] Setting updated";
}

// ============================================================================
// ToF Sensor Initialization and Reading Functions
// ============================================================================

bool tofInit() {
  if (gVL53L4CX != nullptr) {
    // Sensor object exists - clean it up and reinitialize to ensure fresh state
    INFO_TOF_LIFECYCLEF("Cleaning up existing sensor object before reinit");
    i2cDeviceTransactionVoid((uint8_t)gSettings.tofBus, I2C_ADDR_TOF, 100000, 500, [&]() {
      (void)gVL53L4CX->VL53L4CX_StopMeasurement();
    });
    ps_delete(gVL53L4CX);
    gVL53L4CX = nullptr;
    gTofConnected = false;
    // Fall through to reinitialize
  }
  
  // Use i2cTransaction wrapper for safe mutex + clock management
  
  uint32_t tofHz = (gSettings.i2cClockToFHz > 0) ? (uint32_t)gSettings.i2cClockToFHz : 50000;
  if (tofHz < 50000) tofHz = 50000;
  if (tofHz > 400000) tofHz = 400000;

  const uint8_t tofBus = (uint8_t)gSettings.tofBus;
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  TwoWire* tofWire = mgr ? mgr->getWire(tofBus) : nullptr;
  if (!tofWire) return false;

  delay(200);
  if (!i2cPingAddress(I2C_ADDR_TOF, tofHz, 200, tofBus)) {
    return false;
  }
  
  return i2cDeviceTransaction(tofBus, I2C_ADDR_TOF, tofHz, 3000, [&]() -> bool {
    // The large inline device/calibration state is used by task-context code;
    // Wire copies I2C bytes through its own buffers, not DMA from this object.
    // Prefer PSRAM, retaining the standard internal fallback/bypass policy.
    // Pair placement-new with ps_delete on every teardown path below.
    void* tofObjBuf = ps_alloc(sizeof(VL53L4CX), AllocPref::PreferPSRAM, "tof.obj");
    if (!tofObjBuf) return false;
    gVL53L4CX = new (tofObjBuf) VL53L4CX();
    
    // Configure and start
    gVL53L4CX->setI2cDevice(tofWire);
    // XSHUT pin is optional and board-specific; guard usage on A1 definition
#ifdef A1
    gVL53L4CX->setXShutPin(A1);
#endif
    VL53L4CX_Error status = gVL53L4CX->begin();
    if (status != VL53L4CX_ERROR_NONE) {
      ps_delete(gVL53L4CX);
      gVL53L4CX = nullptr;
      return false;
    }
    
    gVL53L4CX->VL53L4CX_Off();
    status = gVL53L4CX->InitSensor(VL53L4CX_DEFAULT_DEVICE_ADDRESS);
    if (status != VL53L4CX_ERROR_NONE) {
      ps_delete(gVL53L4CX);
      gVL53L4CX = nullptr;
      return false;
    }
    
    (void)gVL53L4CX->VL53L4CX_SetDistanceMode(VL53L4CX_DISTANCEMODE_LONG);
    (void)gVL53L4CX->VL53L4CX_SetMeasurementTimingBudgetMicroSeconds(200000);
    status = gVL53L4CX->VL53L4CX_StartMeasurement();
    if (status != VL53L4CX_ERROR_NONE) {
      ps_delete(gVL53L4CX);
      gVL53L4CX = nullptr;
      return false;
    }
    
    gTofConnected = true;
    // Note: gTofRunning is set by cmd_tofstart(), not here, to ensure proper status bump
    
    return true;
  });
}

// Object-in-close-range latch. Hysteresis (enter <300mm / leave >400mm) plus
// reset on task start keeps a hovering object from re-posting every poll.
static bool gTofNear = false;

// Return bus-operation success, not sample freshness. The task owns the driver
// until this call returns, so an asynchronous stop can let one admitted read
// finish safely; it must not turn cancellation/cache contention into I2C faults.
bool tofPoll() {
  if (!gTofConnected || !gTofRunning || gVL53L4CX == nullptr) return false;

  VL53L4CX_MultiRangingData_t MultiRangingData{};
  VL53L4CX_MultiRangingData_t* pMultiRangingData = &MultiRangingData;
  bool dataReady = false;
  const uint8_t tofBus = (uint8_t)gSettings.tofBus;
  const uint32_t tofHz = (gSettings.i2cClockToFHz > 0) ? (uint32_t)gSettings.i2cClockToFHz : 200000;
  const bool transactionOk = i2cTaskWithTimeout(tofBus, I2C_ADDR_TOF, tofHz, 500, [&]() -> bool {
    // Inspect readiness once. A conversion still in progress is a successful
    // status read; release the shared bus and try again on the next task poll.
    uint8_t ready = 0;
    if (gVL53L4CX->VL53L4CX_GetMeasurementDataReady(&ready) != VL53L4CX_ERROR_NONE) return false;
    if (!ready) return true;
    if (gVL53L4CX->VL53L4CX_GetMultiRangingData(pMultiRangingData) != VL53L4CX_ERROR_NONE) return false;
    if (gVL53L4CX->VL53L4CX_ClearInterruptAndStartMeasurement() != VL53L4CX_ERROR_NONE) return false;
    dataReady = true;
    return true;
  });
  if (!transactionOk || !dataReady || !gTofRunning) return transactionOk;

  int no_of_object_found = pMultiRangingData->NumberOfObjectsFound;
  float nearestMm = 1e9f;  // smallest committed object distance this poll (survives guard scope)
  struct ObjectDiagnostic {
    int rangeMm, status;
    float signalRate, minSignalRate;
    bool valid, goodSignal;
  } diagnostics[4]{};
  int validObjectIndex = 0;
  uint32_t publishedSeq = 0;

  // The bus is released before filtering or waiting for the cache. A busy
  // consumer can delay publication without holding up other I2C devices.
  {
    SensorCacheGuard tofGuard(sTofCacheMutex, pdMS_TO_TICKS(50), "tof.pollWrite");
    if (!tofGuard.held || !gTofRunning) {
      return true;  // The bus operation succeeded; no reading was published.
    }

    sTofCache.tofTotalObjects = no_of_object_found;

    for (int j = 0; j < 4; j++) {
      sTofCache.tofObjects[j].detected = false;
      sTofCache.tofObjects[j].distance_mm = 0;
      sTofCache.tofObjects[j].distance_cm = 0.0;
      sTofCache.tofObjects[j].status = 0;
      sTofCache.tofObjects[j].valid = false;
    }

    for (int j = 0; j < no_of_object_found && j < 4; j++) {
      int range_mm = pMultiRangingData->RangeData[j].RangeMilliMeter;
      int range_status = pMultiRangingData->RangeData[j].RangeStatus;

      float signal_rate = (float)pMultiRangingData->RangeData[j].SignalRateRtnMegaCps / 65536.0;

      bool isValid = (range_status != VL53L4CX_RANGESTATUS_SIGNAL_FAIL &&
                      range_status != VL53L4CX_RANGESTATUS_SIGMA_FAIL &&
                      range_status != VL53L4CX_RANGESTATUS_WRAP_TARGET_FAIL &&
                      range_status != VL53L4CX_RANGESTATUS_XTALK_SIGNAL_FAIL);

      float minSignalRate;
      if (range_mm < 1000) {
        minSignalRate = 0.1;
      } else if (range_mm < 3000) {
        minSignalRate = 0.05;
      } else {
        minSignalRate = 0.02;
      }

      bool hasGoodSignal = (signal_rate > minSignalRate);

      diagnostics[j] = {range_mm, range_status, signal_rate, minSignalRate, isValid, hasGoodSignal};

      if (isValid && hasGoodSignal && range_mm > 0 && range_mm <= 6000 && validObjectIndex < 4) {
        float distance_cm = range_mm / 10.0;

        float alpha;
        if (range_mm > 3000) {
          alpha = 0.15;
        } else if (range_mm > 1000) {
          alpha = 0.25;
        } else {
          alpha = 0.4;
        }
        float smoothed_mm, smoothed_cm;

        if (sTofCache.tofObjects[validObjectIndex].hasHistory) {
          smoothed_mm = alpha * range_mm + (1.0 - alpha) * sTofCache.tofObjects[validObjectIndex].smoothed_distance_mm;
          smoothed_cm = alpha * distance_cm + (1.0 - alpha) * sTofCache.tofObjects[validObjectIndex].smoothed_distance_cm;
        } else {
          smoothed_mm = range_mm;
          smoothed_cm = distance_cm;
          sTofCache.tofObjects[validObjectIndex].hasHistory = true;
        }

        sTofCache.tofObjects[validObjectIndex].detected = true;
        sTofCache.tofObjects[validObjectIndex].distance_mm = (int)smoothed_mm;
        sTofCache.tofObjects[validObjectIndex].distance_cm = smoothed_cm;
        sTofCache.tofObjects[validObjectIndex].smoothed_distance_mm = smoothed_mm;
        sTofCache.tofObjects[validObjectIndex].smoothed_distance_cm = smoothed_cm;
        sTofCache.tofObjects[validObjectIndex].status = range_status;
        sTofCache.tofObjects[validObjectIndex].valid = true;

        if (smoothed_mm < nearestMm) nearestMm = smoothed_mm;
        validObjectIndex++;
      }
    }

    sTofCache.tofTotalObjects = validObjectIndex;
    sTofCache.tofLastUpdate = millis();
    sTofCache.tofDataValid = true;
    sTofCache.tofSeq++;

    publishedSeq = sTofCache.tofSeq;
  }  // tofGuard releases here, before diagnostics and events

  for (int j = 0; j < no_of_object_found && j < 4; ++j) {
    const auto& d = diagnostics[j];
    DEBUG_TOF_VALUESF("ToF obj[%d]: range=%dmm, status=%d, signal=%.3f (min=%.3f), isValid=%d, hasGoodSignal=%d",
                     j, d.rangeMm, d.status, d.signalRate, d.minSignalRate,
                     d.valid ? 1 : 0, d.goodSignal ? 1 : 0);
  }
  DEBUG_TOF_POLLINGF("tofPoll: found=%d, valid=%d, seq=%lu",
                     no_of_object_found, validObjectIndex, (unsigned long)publishedSeq);

  // Object presence, edge-latched with hysteresis: enter close range below
  // 300mm, leave above 400mm. Posts once per crossing, never every poll.
  {
    int reportMm = (nearestMm >= 1e9f) ? 9999 : (int)nearestMm;
    char mm[12];
    snprintf(mm, sizeof(mm), "%dmm", reportMm);
    if (!gTofNear && nearestMm < 300.0f) {
      gTofNear = true;
      systemEventPost(SYSEVT_TOF_OBJECT_DETECTED, "near", mm);
    } else if (gTofNear && nearestMm > 400.0f) {
      gTofNear = false;
      systemEventPost(SYSEVT_TOF_OBJECT_DETECTED, "far", mm);
    }
  }

  return true;
}

// ============================================================================
// JSON Building
// ============================================================================

// Build ToF JSON directly into buffer using snprintf (zero String allocations)
int tofBuildDataJSON(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;

  buf[0] = '\0';
  int pos = 0;

  TofCache reading;
  const bool available = tofCacheSnapshot(reading, CACHE_MUTEX_TIMEOUT_MS);
  if (available) {
    if (!reading.tofDataValid) {
      DEBUG_TOF_POLLINGF("tofBuildDataJSON: tofDataValid=%s, gTofRunning=%d, gTofConnected=%d, lastUpdate=%lu",
                   "false", gTofRunning ? 1 : 0, gTofConnected ? 1 : 0, reading.tofLastUpdate);
      // Not-ready reading: uniform envelope with valid=false + empty object list
      // (replaces the old {"error":"ToF sensor not ready"} shape).
      pos = sensorEnvelopeBegin(buf, bufSize, false, gTofConnected, reading.tofLastUpdate);
      if (pos == 0) { buf[0] = '\0'; return 0; }
      int n = snprintf(buf + pos, bufSize - pos, ",\"objects\":[]}");
      if (n < 0 || (size_t)n >= bufSize - pos) { buf[0] = '\0'; return 0; }
      pos += n;
      return pos;
    }

    // Valid reading: shared envelope opening, then ToF's own object list + counts.
    // ts now carries the timestamp, so the old top-level "timestamp" key is gone.
    pos = sensorEnvelopeBegin(buf, bufSize, true, gTofConnected, reading.tofLastUpdate);
    if (pos == 0) { buf[0] = '\0'; return 0; }
    int n = snprintf(buf + pos, bufSize - pos, ",\"objects\":[");
    if (n < 0 || (size_t)n >= bufSize - pos) { buf[0] = '\0'; return 0; }
    pos += n;

    // Emit ONLY detected objects (variable-length). Presence in the array IS the
    // detection, so per-object "detected" is redundant and dropped; "distance_cm"
    // (=mm/10) and the top-level "total_objects"/"seq" are dropped too — the `id`
    // (1-4) identifies which target slot.
    bool first = true;
    for (int j = 0; j < 4; j++) {
      if (!reading.tofObjects[j].detected) continue;
      int n = snprintf(buf + pos, bufSize - pos,
                      "%s{\"id\":%d,\"distance_mm\":%d,\"status\":%d,\"valid\":%s}",
                      first ? "" : ",", j + 1,
                      reading.tofObjects[j].distance_mm,
                      reading.tofObjects[j].status,
                      reading.tofObjects[j].valid ? "true" : "false");
      if (n < 0 || (size_t)n >= bufSize - pos) { buf[0] = '\0'; return 0; }
      pos += n;
      first = false;
    }

    {
      int n = snprintf(buf + pos, bufSize - pos, "]}");
      if (n < 0 || (size_t)n >= bufSize - pos) { buf[0] = '\0'; return 0; }
      pos += n;
    }
  } else {
    // Cache timeout: can't read the cache under lock, so ts=0 (unknown) + valid=false
    // (replaces the old {"error":"ToF cache timeout"} shape).
    pos = sensorEnvelopeBegin(buf, bufSize, false, gTofConnected, 0);
    if (pos == 0) { buf[0] = '\0'; return 0; }
    int n = snprintf(buf + pos, bufSize - pos, ",\"objects\":[],\"total_objects\":0}");
    if (n < 0 || (size_t)n >= bufSize - pos) { buf[0] = '\0'; return 0; }
    pos += n;
  }

  return pos;
}

// ============================================================================
// ToF tuning commands (migrated from .ino)
// ============================================================================

const char* cmd_tofpollingms(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: tofpollingms <50..5000>";
  int v = _arg.toInt();
  if (v < 50 || v > 5000) return "Error: [ToF] Polling interval must be 50-5000ms";
  setSetting(gSettings.tofPollingMs, v);
  BROADCAST_PRINTF("tofPollingMs set to %d", v);
  return "[ToF] Setting updated";
}

const char* cmd_tofstabilitythreshold(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: tofstabilitythreshold <0..50>";
  int v = _arg.toInt();
  if (v < 0 || v > 50) return "Error: [ToF] Stability threshold must be 0-50";
  setSetting(gSettings.tofStabilityThreshold, v);
  BROADCAST_PRINTF("tofStabilityThreshold set to %d", v);
  return "[ToF] Setting updated";
}

// ============================================================================
// ToF Command Registry
// ============================================================================

const char* cmd_tofdevicepollms(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: tofDevicePollMs <100..2000>";
  int v = valStr.toInt();
  if (v < 100) v = 100;
  if (v > 2000) v = 2000;
  setSetting(gSettings.tofDevicePollMs, v);
  snprintf(getDebugBuffer(), 1024, "tofDevicePollMs set to %d", v);
  return getDebugBuffer();
}

// External device-level command handler (defined in i2c_system.cpp)
// extern const char* cmd_tofdevicepollms(const String& argsInput);

const char* cmd_tofautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) {
    return gSettings.tofAutoStart ? "[ToF] Auto-start: enabled" : "[ToF] Auto-start: disabled";
  }
  arg.toLowerCase();
  if (arg == "on" || arg == "true" || arg == "1") {
    setSetting(gSettings.tofAutoStart, true);
    return "[ToF] Auto-start enabled";
  } else if (arg == "off" || arg == "false" || arg == "0") {
    setSetting(gSettings.tofAutoStart, false);
    return "[ToF] Auto-start disabled";
  }
  return "Error: invalid arguments — Usage: tofautostart [on|off]";
}

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
// Bus routing for this device. Lives here rather than in System_I2C.cpp so it
// compiles out with the driver — see i2cSetDeviceBusAndReport in System_I2C.h.
static const char* cmd_tofbus(const String& a) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return i2cSetDeviceBusAndReport(gSettings.tofBus, a, "tofBus");
}

const CommandEntry tofCommands[] = {
  // Start/Stop/Read (3-level voice: "sensor" -> "time of flight" -> "open/close")
  { "opentof", "Start VL53L4CX ToF sensor.", false, cmd_tofstart },
  { "closetof", "Stop VL53L4CX ToF sensor.", false, cmd_tofstop },
  { "tofread", "Read ToF distance sensor. (add 'json' for JSON output)", false, cmd_tof },
  
  // UI Settings (client-side visualization)
  { "tofpollingms", "ToF UI polling: <50..5000>", true, cmd_tofpollingms, "Usage: tofpollingms <50..5000>" },
  { "tofstabilitythreshold", "ToF stability threshold: <0..50>", true, cmd_tofstabilitythreshold, "Usage: tofstabilitythreshold <0..50>" },
  { "toftransitionms", "ToF transition time: <0..5000>", true, cmd_toftransitionms, "Usage: toftransitionms <0..5000>" },
  { "tofmaxdistancemm", "ToF max distance: <100..10000>", true, cmd_tofmaxdistancemm, "Usage: tofmaxdistancemm <100..10000>" },
  
  // Device-level settings (sensor hardware behavior)
  { "tofdevicepollms", "ToF device poll: <100..2000>", true, cmd_tofdevicepollms, "Usage: tofDevicePollMs <100..2000>" },
  
  // Auto-start
  { "tofautostart", "Enable/disable ToF auto-start after boot [on|off]", false, cmd_tofautostart, "Usage: tofautostart [on|off]" },
  { "tofbus",      "Route VL53L4CX ToF to bus: <0|1> (reboot required)", true, cmd_tofbus,      "Usage: tofBus <0|1>" },
};

const size_t tofCommandsCount = sizeof(tofCommands) / sizeof(tofCommands[0]);

// ============================================================================
// Command Registration (Sensor-Specific)
// ============================================================================
// Direct static registration to avoid macro issues
// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// ToF Task Implementation (moved from i2c_system.cpp for full modularization)
// ============================================================================

// ============================================================================
// ToF Task - FreeRTOS Task Function
// ============================================================================
// Purpose: Continuously reads distance measurements from VL53L4CX ToF sensor
// Stack: TOF_STACK_WORDS bytes (4 KB) | Priority: 1 | Core: I2C_SENSOR_CORE
// Lifecycle: Created by cmd_tofstart, deleted when gTofRunning=false
// Polling: Configurable via tofPollingMs (default 100ms) | I2C Clock: 50-400kHz
//
// Cleanup Strategy:
//   1. Check gTofRunning flag at loop start
//   2. Acquire bus mutex via I2CDeviceManager to prevent race conditions during cleanup
//   3. Stop measurement under the bus lock, release it, then delete the object
//   4. Clear connected state and delete the task
// ============================================================================

void tofTask(void* parameter) {
  INFO_TOF_LIFECYCLEF("Task started (handle=%p, stack=%u bytes)",
                (void*)xTaskGetCurrentTaskHandle(),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  INFO_TOF_LIFECYCLEF("[MODULAR] tofTask() running from i2csensor_vl53l4cx.cpp");
  gTofNear = false;
  unsigned long lastToFRead = 0;
  unsigned long lastStackLog = 0;
  while (true) {
    // CRITICAL: Check enabled flag FIRST for graceful shutdown
    if (!gTofRunning) {
      if (!tofInvalidateCache()) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      if (gVL53L4CX != nullptr) {
        // Polling pauses do not exclude other bus users. Keep the hardware
        // stop inside the manager transaction, but always release our object.
        i2cDeviceTransactionVoid((uint8_t)gSettings.tofBus, I2C_ADDR_TOF, 100000, 500, [&]() {
          (void)gVL53L4CX->VL53L4CX_StopMeasurement();
        });
        ps_delete(gVL53L4CX);
        gVL53L4CX = nullptr;
      }
      gTofConnected = false;
      SENSOR_TASK_EXIT(TOF, gTofTaskHandle);
    }
    
    // Update watermark diagnostics (only when enabled)
    if (isDebugFlagSet(DEBUG_PERFORMANCE)) {
      UBaseType_t wm = uxTaskGetStackHighWaterMark(NULL);
      gTofWatermarkNow = wm;
      if (wm < gTofWatermarkMin) gTofWatermarkMin = wm;
    }
    unsigned long nowLog = millis();
    if (nowLog - lastStackLog >= 5000UL) {
      lastStackLog = nowLog;
      // 'continue' (not 'break') so the top-of-loop shutdown runs the clean
      // SENSOR_TASK_EXIT (vTaskDelete) path instead of returning from the task
      // function with a near-overflowed stack (IllegalInstruction panic).
      if (checkTaskStackSafety("tof", TOF_STACK_WORDS, &gTofRunning)) continue;
      // CRITICAL: Check enabled flag again before debug output (prevent crash during shutdown)
      if (gTofRunning) {
        DEBUG_PERFORMANCEF("[STACK] tof_task watermark_now=%u min=%u bytes", (unsigned)gTofWatermarkNow, (unsigned)gTofWatermarkMin);
        DEBUG_MEMORY_HEAPF("[HEAP] tof_task: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }
    const uint8_t tofBus = (uint8_t)gSettings.tofBus;
    if (gTofRunning && gTofConnected && gVL53L4CX != nullptr && !pollPaused(tofBus)) {
      unsigned long tofPollMs = (gSettings.tofDevicePollMs > 0) ? (unsigned long)gSettings.tofDevicePollMs : 100;
      unsigned long nowMs = millis();
      if (nowMs - lastToFRead >= tofPollMs) {
        // The owner separates bus access from cache publication, as presencePoll does.
        bool ok = tofPoll();
        
        lastToFRead = nowMs;
        
        // A stop is normal cleanup, even if an in-flight read also failed.
        if (!ok && gTofRunning) {
          if (i2cShouldAutoDisable(I2C_ADDR_TOF, (uint8_t)gSettings.tofBus)) {
            ERROR_TOFF("Too many consecutive ToF failures - auto-disabling");
            gTofRunning = false;
            sensorStatusBumpWith("tof@auto_disabled");
            logSystemEvent("SENSOR", "ToF auto-disabled after too many consecutive I2C failures");
            systemEventPost(SYSEVT_SENSOR_FAULT, "ToF", "consecutive I2C failures");
          }
        }
        
        // SAFE: Debug output AFTER transaction, with enabled check
        if (gTofRunning) {
          DEBUG_TOF_POLLINGF("ToF readObjects: %s", ok ? "ok" : "fail");
        }
        
        // ESP-NOW broadcaster reads tofBuildDataJSON() on demand from sTofCache.
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    } else {
      // Enabled-but-not-ready, disconnected, or polling paused: yield instead of
      // busy-spinning the core. Without this else the while(true) loop had no delay
      // on the gated path and pinned the core at 100% during any pollPause window
      // (matches imuTask's 50 ms idle delay).
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

// ============================================================================
// ToF Settings Module (for modular settings registry)
// ============================================================================

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry tofSettingEntries[] = {
  { "tofEnabled", SETTING_BOOL, &gSettings.tofEnabled, 1, 0, nullptr, 0, 1, "Enabled", nullptr, false, nullptr, "tofenabled" },
  { "tofAutoStart", SETTING_BOOL, &gSettings.tofAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr, false, nullptr, "tofautostart" },
  { "tofPollingMs", SETTING_INT, &gSettings.tofPollingMs, 220, 0, nullptr, 50, 5000, "Polling (ms)", nullptr, false, nullptr, "tofpollingms" },
  { "tofStabilityThreshold", SETTING_INT, &gSettings.tofStabilityThreshold, 3, 0, nullptr, 0, 50, "Stability Threshold", nullptr, false, nullptr, "tofstabilitythreshold" },
  { "tofTransitionMs", SETTING_INT, &gSettings.tofTransitionMs, 200, 0, nullptr, 0, 5000, "Transition (ms)", nullptr, false, nullptr, "toftransitionms" },
  { "tofMaxDistanceMm", SETTING_INT, &gSettings.tofUiMaxDistanceMm, 3400, 0, nullptr, 100, 10000, "Max Distance (mm)", nullptr, false, nullptr, "tofmaxdistancemm" },
  { "tofDevicePollMs", SETTING_INT, &gSettings.tofDevicePollMs, 220, 0, nullptr, 100, 2000, "Poll Interval (ms)", nullptr, false, nullptr, "tofdevicepollms" },
  { "tofI2cClockHz", SETTING_INT, &gSettings.i2cClockToFHz, 200000, 0, nullptr, 50000, 400000, "I2C Clock (Hz)", nullptr, false, nullptr, "tofi2cclockhz" },
};

static bool isToFConnected() {
  return gTofConnected;
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule tofSettingsModule = {
  "tof",
  "hardware.sensors.tof",
  tofSettingEntries,
  sizeof(tofSettingEntries) / sizeof(tofSettingEntries[0]),
  isToFConnected,
  "VL53L4CX time-of-flight distance sensor"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================
// ToF OLED Mode (Display Function + Registration)
// ============================================================================
#if DISPLAY_TYPE > 0
#include "i2csensor_vl53l4cx_oled.h"
#endif

#endif // ENABLE_TOF_SENSOR
