#include "i2csensor_vl53l4cx.h"
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_BuildConfig.h"
#include "System_MemoryMonitor.h"
#include "System_Utils.h"

#if ENABLE_TOF_SENSOR

#include <Arduino.h>
#include <vl53l4cx_class.h>
#include <Wire.h>

#include "OLED_Display.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#if ENABLE_ESPNOW
  #include "System_ESPNow.h"
  #include "System_ESPNow_Sensors.h"
#endif

// VL53L4CX ToF sensor object (owned by this module)
VL53L4CX* gVL53L4CX = nullptr;
extern TwoWire Wire1;

// Settings and debug

// I2C functions - clock now managed by transaction wrapper

// External dependencies provided by System_I2C.h:
// sensorStatusBumpWith, gSensorPollingPaused, drainDebugRing

// ============================================================================
// ToF Sensor Cache (owned by this module)
// ============================================================================
TofCache gTofCache;

// Debug macros (use centralized versions from debug_system.h)
// MIN_RESTART_DELAY_MS defined in System_I2C.h

// ToF sensor state (definitions)
bool gTofEnabled = false;
bool gTofConnected = false;
uint32_t gTofLastStopTime = 0;
TaskHandle_t gTofTaskHandle = nullptr;

// ToF watermark tracking
volatile UBaseType_t gTofWatermarkMin = (UBaseType_t)0xFFFFFFFF;
volatile UBaseType_t gTofWatermarkNow = (UBaseType_t)0;

// Forward declarations (implementations in main .ino)
extern bool tofInit();
extern void i2cSetDefaultWire1Clock();
extern bool createToFTask();

// Queue system functions now in System_I2C.h

// ============================================================================
// ToF Sensor Reading Functions (moved from .ino)
// ============================================================================

float readToFDistance() {
  if (!gTofConnected || !gTofEnabled || gVL53L4CX == nullptr) {
    if (!gTofConnected) {
      broadcastOutput("ToF sensor not connected. Check wiring.");
      cliHint("to start the sensor once it is wired, run 'opentof'");
    } else if (!gTofEnabled) {
      broadcastOutput("ToF sensor not started. Use 'opentof' first.");
    } else {
      broadcastOutput("ToF sensor initialization failed.");
    }
    return 999.9;
  }

  // Use i2cTransaction wrapper for safe mutex + clock management
  uint32_t clockHz = (gSettings.i2cClockToFHz > 0) ? (uint32_t)gSettings.i2cClockToFHz : 100000;
  float result = i2cDeviceTransaction(I2C_ADDR_TOF, clockHz, 200, [&]() -> float {
    VL53L4CX_MultiRangingData_t MultiRangingData;
    VL53L4CX_MultiRangingData_t* pMultiRangingData = &MultiRangingData;
    uint8_t NewDataReady = 0;
    VL53L4CX_Error status;

    // Wait for data ready with timeout (matches 200ms measurement timing budget)
    unsigned long startTime = millis();
    do {
      status = gVL53L4CX->VL53L4CX_GetMeasurementDataReady(&NewDataReady);
      if (status != VL53L4CX_ERROR_NONE) return 999.9f;
      if (millis() - startTime > 250) return 999.9f;
    } while (!NewDataReady);

    if ((!status) && (NewDataReady != 0)) {
      status = gVL53L4CX->VL53L4CX_GetMultiRangingData(pMultiRangingData);

      if (!status) {
        int no_of_object_found = pMultiRangingData->NumberOfObjectsFound;

        // Find best valid measurement
        float best_distance = 999.9;
        bool found_valid = false;

        for (int j = 0; j < no_of_object_found; j++) {
          if (pMultiRangingData->RangeData[j].RangeStatus == VL53L4CX_RANGESTATUS_RANGE_VALID) {
            float distance_cm = pMultiRangingData->RangeData[j].RangeMilliMeter / 10.0;

            // Use closest valid object
            if (distance_cm < best_distance) {
              best_distance = distance_cm;
              found_valid = true;
            }
          }
        }

        // Clear interrupt and restart
        gVL53L4CX->VL53L4CX_ClearInterruptAndStartMeasurement();

        if (found_valid) {
          BROADCAST_PRINTF("Distance: %.1f cm", best_distance);
          return best_distance;
        }
      }

      // Clear interrupt even on error
      gVL53L4CX->VL53L4CX_ClearInterruptAndStartMeasurement();
    }

    broadcastOutput("No valid distance measurement");
    return 999.9;  // No valid measurement
  });

  return result;
}

// ============================================================================
// ToF Sensor Command Handlers
// ============================================================================

const char* cmd_tof(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    if (!ensureDebugBuffer()) return "{\"valid\":false,\"error\":\"buffer\"}";
    int n = tofBuildDataJSON(getDebugBuffer(), 1024);  // shared builder (also feeds sensors json / MQTT)
    return (n > 0) ? getDebugBuffer() : "{\"valid\":false}";
  }

  float distance = readToFDistance();
  if (distance < 999.0) {
    BROADCAST_PRINTF("Distance: %.1f cm", distance);
    return "[ToF] Reading complete";
  } else {
    // readToFDistance() already output error message
    return "ERROR";
  }
}

// Internal function called by queue processor
bool tofStartInternal() {
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
  if (!gTofCache.mutex) {
    gTofCache.mutex = xSemaphoreCreateMutex();
    if (!gTofCache.mutex) {
      ERROR_TOFF("Failed to create cache mutex");
      return false;
    }
    DEBUG_TOF_LIFECYCLEF("[TOF] Cache mutex created");
  }

  // Clean up any stale cache from previous run BEFORE starting
  // CRITICAL: Cache wasn't invalidated during stop to avoid dying-task crashes
  {
    SensorCacheGuard g(gTofCache.mutex, pdMS_TO_TICKS(100), "tof.cleanStaleCache");
    if (g.held) {
      gTofCache.tofDataValid = false;
      gTofCache.tofTotalObjects = 0;
      for (int j = 0; j < 4; j++) {
        gTofCache.tofObjects[j].detected = false;
        gTofCache.tofObjects[j].valid = false;
        gTofCache.tofObjects[j].distance_mm = 0;
        gTofCache.tofObjects[j].distance_cm = 0.0;
        gTofCache.tofObjects[j].status = 0;
        gTofCache.tofObjects[j].smoothed_distance_mm = 0.0;
        gTofCache.tofObjects[j].smoothed_distance_cm = 0.0;
        gTofCache.tofObjects[j].hasHistory = false;
      }
      DEBUG_CLIF("[TOF_INTERNAL] Cleaned up stale cache from previous run");
    }
  }

  // Set gTofEnabled FIRST to prevent race condition with task cleanup code
  gTofEnabled = true;
  INFO_TOF_LIFECYCLEF("Set gTofEnabled=1 BEFORE init to prevent race condition");

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
      gTofEnabled = false;
      gTofConnected = false;
      DEBUG_CLIF("Failed to initialize VL53L4CX ToF sensor (tried 2x)");
      return false;
    }
  }

  // Create ToF task lazily
  if (!createToFTask()) {
    DEBUG_CLIF("Failed to create ToF task");
    return false;
  }
  // Clock is now managed automatically by i2cTaskWithStandardTimeout wrapper
  // Device registration specifies ToF's clock speed (50-400kHz)
  // gTofEnabled already set to true at the beginning to prevent race condition
  sensorStatusBumpWith("opentof@queue");
  DEBUG_CLIF("SUCCESS: ToF sensor started successfully");
  
  // Broadcast sensor status to ESP-NOW master
#if ENABLE_ESPNOW
  broadcastSensorStatus(REMOTE_SENSOR_TOF, true);
#endif
  
  return true;
}

// Public command - uses centralized queue
const char* cmd_tofstart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Check if already enabled or queued
  if (gTofEnabled) {
    return "[ToF] Sensor already running";
  }
  if (isInQueue(I2C_DEVICE_TOF)) {
    int pos = getQueuePosition(I2C_DEVICE_TOF);
    BROADCAST_PRINTF("ToF sensor already queued (position %d)", pos);
    return "[ToF] Already queued";
  }

  if (!i2cPingAddress(I2C_ADDR_TOF, 100000, 50)) {
    return "Error: [ToF] Not detected on I2C bus";
  }

  // Enqueue the request to centralized queue
  if (enqueueDeviceStart(I2C_DEVICE_TOF)) {
    sensorStatusBumpWith("opentof@enqueue");
    int pos = getQueuePosition(I2C_DEVICE_TOF);
    BROADCAST_PRINTF("ToF sensor queued for open (position %d)", pos);
    cliHint("the sensor opens in the background - read a distance with 'tofread' once it is up");
    return "[ToF] Sensor queued for open";
  } else {
    return "Error: [ToF] Failed to enqueue open (queue full)";
  }
}

const char* cmd_tofstop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  handleDeviceStopped(I2C_DEVICE_TOF);
  return "[ToF] Close requested; cleanup will complete asynchronously";
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
    (void)gVL53L4CX->VL53L4CX_StopMeasurement();
    delete gVL53L4CX;
    gVL53L4CX = nullptr;
    gTofConnected = false;
    // Fall through to reinitialize
  }
  
  // Use i2cTransaction wrapper for safe mutex + clock management
  
  uint32_t tofHz = (gSettings.i2cClockToFHz > 0) ? (uint32_t)gSettings.i2cClockToFHz : 50000;
  if (tofHz < 50000) tofHz = 50000;
  if (tofHz > 400000) tofHz = 400000;

  delay(200);
  if (!i2cPingAddress(I2C_ADDR_TOF, tofHz, 200)) {
    return false;
  }
  
  return i2cDeviceTransaction(I2C_ADDR_TOF, tofHz, 3000, [&]() -> bool {
    // Wire1 is configured centrally with runtime-configurable pins
    
    // Allocate sensor object
    gVL53L4CX = new VL53L4CX();
    if (!gVL53L4CX) return false;
    
    // Configure and start
    gVL53L4CX->setI2cDevice(&Wire1);
    // XSHUT pin is optional and board-specific; guard usage on A1 definition
#ifdef A1
    gVL53L4CX->setXShutPin(A1);
#endif
    VL53L4CX_Error status = gVL53L4CX->begin();
    if (status != VL53L4CX_ERROR_NONE) {
      delete gVL53L4CX;
      gVL53L4CX = nullptr;
      return false;
    }
    
    gVL53L4CX->VL53L4CX_Off();
    status = gVL53L4CX->InitSensor(VL53L4CX_DEFAULT_DEVICE_ADDRESS);
    if (status != VL53L4CX_ERROR_NONE) {
      delete gVL53L4CX;
      gVL53L4CX = nullptr;
      return false;
    }
    
    (void)gVL53L4CX->VL53L4CX_SetDistanceMode(VL53L4CX_DISTANCEMODE_LONG);
    (void)gVL53L4CX->VL53L4CX_SetMeasurementTimingBudgetMicroSeconds(200000);
    status = gVL53L4CX->VL53L4CX_StartMeasurement();
    if (status != VL53L4CX_ERROR_NONE) {
      delete gVL53L4CX;
      gVL53L4CX = nullptr;
      return false;
    }
    
    gTofConnected = true;
    // Note: gTofEnabled is set by cmd_tofstart(), not here, to ensure proper status bump
    
    return true;
  });
}

// Object-in-close-range latch. Hysteresis (enter <300mm / leave >400mm) plus
// reset on task start keeps a hovering object from re-posting every poll.
static bool gTofNear = false;

bool tofPoll() {
  // gDebugFlags now from debug_system.h
  
  if (!gTofConnected || !gTofEnabled || gVL53L4CX == nullptr) {
    if (!gTofConnected) {
      broadcastOutput("ToF sensor not connected. Check wiring.");
    } else if (!gTofEnabled) {
      broadcastOutput("ToF sensor not started. Use 'opentof' first.");
    } else {
      broadcastOutput("ToF sensor initialization failed.");
    }
    return false;
  }

  // Clock is managed by i2cTaskWithStandardTimeout wrapper - no manual changes needed

  VL53L4CX_MultiRangingData_t MultiRangingData;
  VL53L4CX_MultiRangingData_t* pMultiRangingData = &MultiRangingData;
  uint8_t NewDataReady = 0;
  VL53L4CX_Error status;

  // Wait for data ready with optimized timeout for 200ms timing budget
  unsigned long startTime = millis();
  do {
    status = gVL53L4CX->VL53L4CX_GetMeasurementDataReady(&NewDataReady);
    if (millis() - startTime > 250) {
      return false;
    }
    if (status != VL53L4CX_ERROR_NONE) {
      return false;
    }
  } while (!NewDataReady);

  if ((!status) && (NewDataReady != 0)) {
    status = gVL53L4CX->VL53L4CX_GetMultiRangingData(pMultiRangingData);

    if (status != VL53L4CX_ERROR_NONE) {
      return false;
    }

    int no_of_object_found = pMultiRangingData->NumberOfObjectsFound;
    float nearestMm = 1e9f;  // smallest committed object distance this poll (survives guard scope)

    // Pattern 3: take, do all cache writes, release BEFORE the
    // VL53L4CX_ClearInterruptAndStartMeasurement hardware call. Explicit
    // { } block scopes the guard so the dtor releases before line 1 of
    // the post-block hardware call.
    {
      SensorCacheGuard tofGuard(gTofCache.mutex, pdMS_TO_TICKS(50), "tof.pollWrite");
      if (!tofGuard.held) {
        return false;
      }

      gTofCache.tofTotalObjects = no_of_object_found;

    for (int j = 0; j < 4; j++) {
      gTofCache.tofObjects[j].detected = false;
      gTofCache.tofObjects[j].distance_mm = 0;
      gTofCache.tofObjects[j].distance_cm = 0.0;
      gTofCache.tofObjects[j].status = 0;
      gTofCache.tofObjects[j].valid = false;
    }

    int validObjectIndex = 0;
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

      DEBUG_TOF_VALUESF("ToF obj[%d]: range=%dmm, status=%d, signal=%.3f (min=%.3f), isValid=%d, hasGoodSignal=%d",
                   j, range_mm, range_status, signal_rate, minSignalRate, isValid ? 1 : 0, hasGoodSignal ? 1 : 0);

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

        if (gTofCache.tofObjects[validObjectIndex].hasHistory) {
          smoothed_mm = alpha * range_mm + (1.0 - alpha) * gTofCache.tofObjects[validObjectIndex].smoothed_distance_mm;
          smoothed_cm = alpha * distance_cm + (1.0 - alpha) * gTofCache.tofObjects[validObjectIndex].smoothed_distance_cm;
        } else {
          smoothed_mm = range_mm;
          smoothed_cm = distance_cm;
          gTofCache.tofObjects[validObjectIndex].hasHistory = true;
        }

        gTofCache.tofObjects[validObjectIndex].detected = true;
        gTofCache.tofObjects[validObjectIndex].distance_mm = (int)smoothed_mm;
        gTofCache.tofObjects[validObjectIndex].distance_cm = smoothed_cm;
        gTofCache.tofObjects[validObjectIndex].smoothed_distance_mm = smoothed_mm;
        gTofCache.tofObjects[validObjectIndex].smoothed_distance_cm = smoothed_cm;
        gTofCache.tofObjects[validObjectIndex].status = range_status;
        gTofCache.tofObjects[validObjectIndex].valid = true;

        if (smoothed_mm < nearestMm) nearestMm = smoothed_mm;
        validObjectIndex++;
      }
    }

    gTofCache.tofTotalObjects = validObjectIndex;
    gTofCache.tofLastUpdate = millis();
    gTofCache.tofDataValid = true;
    gTofCache.tofSeq++;

    DEBUG_TOF_POLLINGF("tofPoll: found=%d, valid=%d, seq=%lu",
                 no_of_object_found, validObjectIndex,
                 (unsigned long)gTofCache.tofSeq);
    }  // tofGuard releases here, before the hardware call

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

    gVL53L4CX->VL53L4CX_ClearInterruptAndStartMeasurement();

    return true;
  }

  return false;
}

// ============================================================================
// JSON Building
// ============================================================================

// Build ToF JSON directly into buffer using snprintf (zero String allocations)
int tofBuildDataJSON(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;

  int pos = 0;

  SensorCacheGuard g(gTofCache.mutex, pdMS_TO_TICKS(CACHE_MUTEX_TIMEOUT_MS), "tof.buildJSON");
  if (g.held) {
    if (!gTofCache.tofDataValid) {
      DEBUG_TOF_POLLINGF("tofBuildDataJSON: tofDataValid=%s, gTofEnabled=%d, gTofConnected=%d, lastUpdate=%lu",
                   "false", gTofEnabled ? 1 : 0, gTofConnected ? 1 : 0, gTofCache.tofLastUpdate);
      // Not-ready reading: uniform envelope with valid=false + empty object list
      // (replaces the old {"error":"ToF sensor not ready"} shape).
      pos = sensorEnvelopeBegin(buf, bufSize, false, gTofConnected, gTofCache.tofLastUpdate);
      if (pos == 0) return 0;
      pos += snprintf(buf + pos, bufSize - pos, ",\"objects\":[]}");
      return pos;
    }

    // Valid reading: shared envelope opening, then ToF's own object list + counts.
    // ts now carries the timestamp, so the old top-level "timestamp" key is gone.
    pos = sensorEnvelopeBegin(buf, bufSize, true, gTofConnected, gTofCache.tofLastUpdate);
    if (pos == 0) return 0;
    pos += snprintf(buf + pos, bufSize - pos, ",\"objects\":[");

    // Emit ONLY detected objects (variable-length). Presence in the array IS the
    // detection, so per-object "detected" is redundant and dropped; "distance_cm"
    // (=mm/10) and the top-level "total_objects"/"seq" are dropped too — the `id`
    // (1-4) identifies which target slot.
    bool first = true;
    for (int j = 0; j < 4; j++) {
      if (pos < 0 || (size_t)pos >= bufSize) break;  // buffer full — stop before bufSize-pos underflows
      if (!gTofCache.tofObjects[j].detected) continue;
      pos += snprintf(buf + pos, bufSize - pos,
                      "%s{\"id\":%d,\"distance_mm\":%d,\"status\":%d,\"valid\":%s}",
                      first ? "" : ",", j + 1,
                      gTofCache.tofObjects[j].distance_mm,
                      gTofCache.tofObjects[j].status,
                      gTofCache.tofObjects[j].valid ? "true" : "false");
      first = false;
    }

    if (pos >= 0 && (size_t)pos < bufSize) {
      pos += snprintf(buf + pos, bufSize - pos, "]}");
    }
  } else {
    // Cache timeout: can't read the cache under lock, so ts=0 (unknown) + valid=false
    // (replaces the old {"error":"ToF cache timeout"} shape).
    pos = sensorEnvelopeBegin(buf, bufSize, false, gTofConnected, 0);
    if (pos == 0) return 0;
    pos += snprintf(buf + pos, bufSize - pos, ",\"objects\":[],\"total_objects\":0}");
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

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry tofCommands[] = {
  // Start/Stop/Read (3-level voice: "sensor" -> "time of flight" -> "open/close")
  { "opentof", "Start VL53L4CX ToF sensor.", false, cmd_tofstart, nullptr, "sensor", "time of flight", "open" },
  { "closetof", "Stop VL53L4CX ToF sensor.", false, cmd_tofstop, nullptr, "sensor", "time of flight", "close" },
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
// Stack: 3072 BYTES (3 KB) | Priority: 1 | Core: Any
// Lifecycle: Created by cmd_tofstart, deleted when gTofEnabled=false
// Polling: Configurable via tofPollingMs (default 100ms) | I2C Clock: 50-400kHz
//
// Cleanup Strategy:
//   1. Check gTofEnabled flag at loop start
//   2. Acquire bus mutex via I2CDeviceManager to prevent race conditions during cleanup
//   3. Stop measurement, delete sensor object, and invalidate cache
//   4. Release mutex and delete task
// ============================================================================

void tofTask(void* parameter) {
  INFO_TOF_LIFECYCLEF("Task started (handle=%p, stack=%u words)", 
                (void*)xTaskGetCurrentTaskHandle(), 
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  INFO_TOF_LIFECYCLEF("[MODULAR] tofTask() running from i2csensor_vl53l4cx.cpp");
  gTofNear = false;
  unsigned long lastToFRead = 0;
  unsigned long lastStackLog = 0;
  while (true) {
    // CRITICAL: Check enabled flag FIRST for graceful shutdown
    if (!gTofEnabled) {
      if (gVL53L4CX != nullptr) {
        (void)gVL53L4CX->VL53L4CX_StopMeasurement();
        delete gVL53L4CX;
        gVL53L4CX = nullptr;
      }
      gTofConnected = false;
      gTofCache.tofDataValid = false;
      gTofCache.tofTotalObjects = 0;
      for (int j = 0; j < 4; j++) {
        gTofCache.tofObjects[j].detected = false;
        gTofCache.tofObjects[j].valid = false;
      }
      SENSOR_TASK_EXIT(TOF);
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
      if (checkTaskStackSafety("tof", TOF_STACK_WORDS, &gTofEnabled)) continue;
      // CRITICAL: Check enabled flag again before debug output (prevent crash during shutdown)
      if (gTofEnabled) {
        DEBUG_PERFORMANCEF("[STACK] tof_task watermark_now=%u min=%u words", (unsigned)gTofWatermarkNow, (unsigned)gTofWatermarkMin);
        DEBUG_MEMORY_HEAPF("[HEAP] tof_task: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }
    if (gTofEnabled && gTofConnected && gVL53L4CX != nullptr && !pollPaused(0 /* legacy Wire1 = bus 0 */)) {
      unsigned long tofPollMs = (gSettings.tofDevicePollMs > 0) ? (unsigned long)gSettings.tofDevicePollMs : 100;
      unsigned long nowMs = millis();
      if (nowMs - lastToFRead >= tofPollMs) {
        uint32_t tofHz = (gSettings.i2cClockToFHz > 0) ? (uint32_t)gSettings.i2cClockToFHz : 200000;
        bool ok = false;
        
        // ToF busy-waits up to 250ms for data ready; 500ms timeout gives headroom without over-blocking
        ok = i2cTaskWithTimeout(I2C_ADDR_TOF, tofHz, 500, [&]() -> bool {
          return tofPoll();
        });
        
        lastToFRead = nowMs;
        
        // Auto-disable if too many consecutive failures
        if (!ok) {
          if (i2cShouldAutoDisable(I2C_ADDR_TOF)) {
            ERROR_TOFF("Too many consecutive ToF failures - auto-disabling");
            gTofEnabled = false;
            sensorStatusBumpWith("tof@auto_disabled");
            logSystemEvent("SENSOR", "ToF auto-disabled after too many consecutive I2C failures");
            systemEventPost(SYSEVT_SENSOR_FAULT, "ToF", "consecutive I2C failures");
          }
        }
        
        // SAFE: Debug output AFTER transaction, with enabled check
        if (gTofEnabled) {
          DEBUG_TOF_POLLINGF("ToF readObjects: %s", ok ? "ok" : "fail");
        }
        
        // ESP-NOW broadcaster reads tofBuildDataJSON() on demand from gTofCache.
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
  { "tofAutoStart", SETTING_BOOL, &gSettings.tofAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr, false, nullptr, nullptr },
  { "tofPollingMs", SETTING_INT, &gSettings.tofPollingMs, 220, 0, nullptr, 50, 5000, "Polling (ms)", nullptr, false, nullptr, nullptr },
  { "tofStabilityThreshold", SETTING_INT, &gSettings.tofStabilityThreshold, 3, 0, nullptr, 0, 50, "Stability Threshold", nullptr, false, nullptr, nullptr },
  { "tofTransitionMs", SETTING_INT, &gSettings.tofTransitionMs, 200, 0, nullptr, 0, 5000, "Transition (ms)", nullptr, false, nullptr, nullptr },
  { "tofMaxDistanceMm", SETTING_INT, &gSettings.tofUiMaxDistanceMm, 3400, 0, nullptr, 100, 10000, "Max Distance (mm)", nullptr, false, nullptr, nullptr },
  { "tofDevicePollMs", SETTING_INT, &gSettings.tofDevicePollMs, 220, 0, nullptr, 100, 2000, "Poll Interval (ms)", nullptr, false, nullptr, nullptr },
  { "tofI2cClockHz", SETTING_INT, &gSettings.i2cClockToFHz, 200000, 0, nullptr, 50000, 400000, "I2C Clock (Hz)", nullptr, false, nullptr, "tofi2cclockhz" }
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
