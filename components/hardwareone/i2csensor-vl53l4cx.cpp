#include "i2csensor-vl53l4cx.h"
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
extern Settings gSettings;

// I2C functions - clock now managed by transaction wrapper

// External dependencies provided by System_I2C.h:
// sensorStatusBumpWith, gSensorPollingPaused, i2cMutex, drainDebugRing

// ============================================================================
// ToF Sensor Cache (owned by this module)
// ============================================================================
TofCache gTofCache;

// Macro for validation
#define RETURN_VALID_IF_VALIDATE_CSTR() \
  do { \
    extern bool gCLIValidateOnly; \
    if (gCLIValidateOnly) return "VALID"; \
  } while(0)

// Debug macros (use centralized versions from debug_system.h)
#define MIN_RESTART_DELAY_MS 2000

// ToF sensor state (definitions)
bool tofEnabled = false;
bool tofConnected = false;
uint32_t tofLastStopTime = 0;
TaskHandle_t tofTaskHandle = nullptr;

// ToF watermark tracking
volatile UBaseType_t gTofWatermarkMin = (UBaseType_t)0xFFFFFFFF;
volatile UBaseType_t gTofWatermarkNow = (UBaseType_t)0;

// Forward declarations (implementations in main .ino)
extern bool initToFSensor();
extern void i2cSetDefaultWire1Clock();
extern bool createToFTask();

// Queue system functions now in System_I2C.h

// ============================================================================
// ToF Sensor Reading Functions (moved from .ino)
// ============================================================================

float readToFDistance() {
  if (!tofConnected || !tofEnabled || gVL53L4CX == nullptr) {
    if (!tofConnected) {
      broadcastOutput("ToF sensor not connected. Check wiring.");
    } else if (!tofEnabled) {
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

const char* cmd_tof(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

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
bool startToFSensorInternal() {
  // Check if too soon after stop (prevent rapid restart crashes)
  if (tofLastStopTime > 0) {
    unsigned long timeSinceStop = millis() - tofLastStopTime;
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

  // Clean up any stale cache from previous run BEFORE starting
  // CRITICAL: Cache wasn't invalidated during stop to avoid dying-task crashes
  gTofCache.tofDataValid = false;
  gTofCache.tofTotalObjects = 0;
  for (int j = 0; j < 4; j++) {
    gTofCache.tofObjects[j].detected = false;
    gTofCache.tofObjects[j].valid = false;
  }
  DEBUG_CLIF("[TOF_INTERNAL] Cleaned up stale cache from previous run");

  // Set tofEnabled FIRST to prevent race condition with task cleanup code
  tofEnabled = true;
  INFO_SENSORSF("Set tofEnabled=1 BEFORE init to prevent race condition");

  // Initialize ToF sensor synchronously (like thermal sensor)
  if (!tofConnected || gVL53L4CX == nullptr) {
    // Try initialization with retry
    bool initSuccess = false;
    for (int attempt = 0; attempt < 2 && !initSuccess; attempt++) {
      if (attempt > 0) {
        delay(200);  // Brief delay between attempts
      }
      initSuccess = initToFSensor();
    }

    if (!initSuccess) {
      // Ensure ToF stays disabled on init failure
      tofEnabled = false;
      tofConnected = false;
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
  // tofEnabled already set to true at the beginning to prevent race condition
  sensorStatusBumpWith("opentof@queue");
  DEBUG_CLIF("SUCCESS: ToF sensor started successfully");
  
  // Broadcast sensor status to ESP-NOW master
#if ENABLE_ESPNOW
  broadcastSensorStatus(REMOTE_SENSOR_TOF, true);
#endif
  
  return true;
}

// Public command - uses centralized queue
const char* cmd_tofstart(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Check if already enabled or queued
  if (tofEnabled) {
    return "[ToF] Sensor already running";
  }
  if (isInQueue(I2C_DEVICE_TOF)) {
    int pos = getQueuePosition(I2C_DEVICE_TOF);
    BROADCAST_PRINTF("ToF sensor already queued (position %d)", pos);
    return "[ToF] Already queued";
  }

  // Enqueue the request to centralized queue
  if (enqueueDeviceStart(I2C_DEVICE_TOF)) {
    sensorStatusBumpWith("opentof@enqueue");
    int pos = getQueuePosition(I2C_DEVICE_TOF);
    BROADCAST_PRINTF("ToF sensor queued for open (position %d)", pos);
    return "[ToF] Sensor queued for open";
  } else {
    return "[ToF] Error: Failed to enqueue open (queue full)";
  }
}

const char* cmd_tofstop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  handleDeviceStopped(I2C_DEVICE_TOF);
  return "[ToF] Close requested; cleanup will complete asynchronously";
}

const char* cmd_toftransitionms(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: toftransitionms <0..5000>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 0 || v > 5000) return "[ToF] Error: Transition time must be 0-5000ms";
  setSetting(gSettings.tofTransitionMs, v);
  BROADCAST_PRINTF("tofTransitionMs set to %d", v);
  return "[ToF] Setting updated";
}

const char* cmd_tofmaxdistancemm(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: tofmaxdistancemm <100..10000>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 100 || v > 10000) return "[ToF] Error: Max distance must be 100-10000mm";
  setSetting(gSettings.tofUiMaxDistanceMm, v);
  BROADCAST_PRINTF("tofUiMaxDistanceMm set to %d", v);
  return "[ToF] Setting updated";
}

// ============================================================================
// ToF Sensor Initialization and Reading Functions
// ============================================================================

bool initToFSensor() {
  if (gVL53L4CX != nullptr) {
    // Sensor object exists - clean it up and reinitialize to ensure fresh state
    INFO_SENSORSF("Cleaning up existing sensor object before reinit");
    (void)gVL53L4CX->VL53L4CX_StopMeasurement();
    delete gVL53L4CX;
    gVL53L4CX = nullptr;
    tofConnected = false;
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
    
    tofConnected = true;
    // Note: tofEnabled is set by cmd_tofstart(), not here, to ensure proper status bump
    
    return true;
  });
}

bool readToFObjects() {
  // gDebugFlags now from debug_system.h
  
  if (!tofConnected || !tofEnabled || gVL53L4CX == nullptr) {
    if (!tofConnected) {
      broadcastOutput("ToF sensor not connected. Check wiring.");
    } else if (!tofEnabled) {
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

    if (!gTofCache.mutex || xSemaphoreTake(gTofCache.mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
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

      if (isDebugFlagSet(DEBUG_TOF_FRAME)) {
        DEBUG_TOF_FRAMEF("ToF obj[%d]: range=%dmm, status=%d, signal=%.3f (min=%.3f), isValid=%d, hasGoodSignal=%d",
                     j, range_mm, range_status, signal_rate, minSignalRate, isValid ? 1 : 0, hasGoodSignal ? 1 : 0);
      }

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

        validObjectIndex++;
      }
    }

    gTofCache.tofTotalObjects = validObjectIndex;
    gTofCache.tofLastUpdate = millis();
    gTofCache.tofDataValid = true;
    gTofCache.tofSeq++;

    if (isDebugFlagSet(DEBUG_TOF_FRAME)) {
      DEBUG_TOF_FRAMEF("readToFObjects: found=%d, valid=%d, seq=%lu",
                   no_of_object_found, validObjectIndex,
                   (unsigned long)gTofCache.tofSeq);
    }

    xSemaphoreGive(gTofCache.mutex);

    gVL53L4CX->VL53L4CX_ClearInterruptAndStartMeasurement();

    return true;
  }

  return false;
}

// ============================================================================
// JSON Building
// ============================================================================

// Build ToF JSON directly into buffer using snprintf (zero String allocations)
int buildToFDataJSON(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;

  int pos = 0;

  if (gTofCache.mutex && xSemaphoreTake(gTofCache.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {  // 100ms timeout for HTTP response
    if (!gTofCache.tofDataValid) {
      if (isDebugFlagSet(DEBUG_TOF_FRAME)) {
        DEBUG_TOF_FRAMEF("buildToFDataJSON: tofDataValid=%s, tofEnabled=%d, tofConnected=%d, lastUpdate=%lu",
                     "false", tofEnabled ? 1 : 0, tofConnected ? 1 : 0, gTofCache.tofLastUpdate);
      }
      pos = snprintf(buf, bufSize, "{\"error\":\"ToF sensor not ready\"}");
      xSemaphoreGive(gTofCache.mutex);
      return pos;
    }

    // Build JSON response from cached data
    pos = snprintf(buf, bufSize, "{\"objects\":[");

    for (int j = 0; j < 4; j++) {
      if (j > 0) {
        pos += snprintf(buf + pos, bufSize - pos, ",");
      }

      // Build each object's JSON
      if (gTofCache.tofObjects[j].detected) {
        pos += snprintf(buf + pos, bufSize - pos,
                        "{\"id\":%d,\"detected\":true,\"distance_mm\":%d,\"distance_cm\":%.1f,\"status\":%d,\"valid\":%s}",
                        j + 1,
                        gTofCache.tofObjects[j].distance_mm,
                        gTofCache.tofObjects[j].distance_cm,
                        gTofCache.tofObjects[j].status,
                        gTofCache.tofObjects[j].valid ? "true" : "false");
      } else {
        pos += snprintf(buf + pos, bufSize - pos,
                        "{\"id\":%d,\"detected\":false,\"distance_mm\":null,\"distance_cm\":null,\"status\":null,\"valid\":false}",
                        j + 1);
      }
    }

    // Add footer with metadata
    pos += snprintf(buf + pos, bufSize - pos,
                    "],\"total_objects\":%d,\"seq\":%lu,\"timestamp\":%lu}",
                    gTofCache.tofTotalObjects,
                    (unsigned long)gTofCache.tofSeq,
                    gTofCache.tofLastUpdate);

    xSemaphoreGive(gTofCache.mutex);
  } else {
    // Timeout - return error response
    pos = snprintf(buf, bufSize, "{\"error\":\"ToF cache timeout\"}");
  }

  return pos;
}

// ============================================================================
// ToF tuning commands (migrated from .ino)
// ============================================================================

const char* cmd_tofpollingms(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: tofpollingms <50..5000>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 50 || v > 5000) return "[ToF] Error: Polling interval must be 50-5000ms";
  setSetting(gSettings.tofPollingMs, v);
  BROADCAST_PRINTF("tofPollingMs set to %d", v);
  return "[ToF] Setting updated";
}

const char* cmd_tofstabilitythreshold(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: tofstabilitythreshold <0..50>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 0 || v > 50) return "[ToF] Error: Stability threshold must be 0-50";
  setSetting(gSettings.tofStabilityThreshold, v);
  BROADCAST_PRINTF("tofStabilityThreshold set to %d", v);
  return "[ToF] Setting updated";
}

// ============================================================================
// ToF Command Registry
// ============================================================================

const char* cmd_tofdevicepollms(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) return "Usage: tofDevicePollMs <100..2000>";
  int v = valStr.toInt();
  if (v < 100) v = 100;
  if (v > 2000) v = 2000;
  setSetting(gSettings.tofDevicePollMs, v);
  snprintf(getDebugBuffer(), 1024, "tofDevicePollMs set to %d", v);
  return getDebugBuffer();
}

// External device-level command handler (defined in i2c_system.cpp)
// extern const char* cmd_tofdevicepollms(const String& cmd);

const char* cmd_tofautostart(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = args; arg.trim();
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
  return "Usage: tofautostart [on|off]";
}

const CommandEntry tofCommands[] = {
  // Start/Stop/Read (3-level voice: "sensor" -> "time of flight" -> "open/close")
  { "opentof", "Start VL53L4CX ToF sensor.", false, cmd_tofstart, nullptr, "sensor", "time of flight", "open" },
  { "closetof", "Stop VL53L4CX ToF sensor.", false, cmd_tofstop, nullptr, "sensor", "time of flight", "close" },
  { "tofread", "Read ToF distance sensor.", false, cmd_tof },
  
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
static CommandModuleRegistrar _tof_cmd_registrar(tofCommands, tofCommandsCount, "tof");

// ============================================================================
// ToF Task Implementation (moved from i2c_system.cpp for full modularization)
// ============================================================================

// ============================================================================
// ToF Task - FreeRTOS Task Function
// ============================================================================
// Purpose: Continuously reads distance measurements from VL53L4CX ToF sensor
// Stack: 3072 words (~12KB) | Priority: 1 | Core: Any
// Lifecycle: Created by cmd_tofstart, deleted when tofEnabled=false
// Polling: Configurable via tofPollingMs (default 100ms) | I2C Clock: 50-400kHz
//
// Cleanup Strategy:
//   1. Check tofEnabled flag at loop start
//   2. Acquire i2cMutex to prevent race conditions during cleanup
//   3. Stop measurement, delete sensor object, and invalidate cache
//   4. Release mutex and delete task
// ============================================================================

void tofTask(void* parameter) {
  INFO_SENSORSF("[ToF] Task started (handle=%p, stack=%u words)", 
                (void*)xTaskGetCurrentTaskHandle(), 
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  INFO_SENSORSF("[MODULAR] tofTask() running from Sensor_ToF_VL53L4CX.cpp");
  unsigned long lastToFRead = 0;
  unsigned long lastStackLog = 0;
  while (true) {
    // CRITICAL: Check enabled flag FIRST for graceful shutdown
    if (!tofEnabled) {
      if (gVL53L4CX != nullptr) {
        (void)gVL53L4CX->VL53L4CX_StopMeasurement();
        delete gVL53L4CX;
        gVL53L4CX = nullptr;
      }
      tofConnected = false;
      gTofCache.tofDataValid = false;
      gTofCache.tofTotalObjects = 0;
      for (int j = 0; j < 4; j++) {
        gTofCache.tofObjects[j].detected = false;
        gTofCache.tofObjects[j].valid = false;
      }
      INFO_SENSORSF("[ToF] Task disabled - cleaning up and deleting");
      // NOTE: Do NOT clear tofTaskHandle here - let create function use eTaskGetState()
      // to detect stale handles. Clearing here creates a race condition window.
      vTaskDelete(nullptr);
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
      if (checkTaskStackSafety("tof", TOF_STACK_WORDS, &tofEnabled)) break;
      // CRITICAL: Check enabled flag again before debug output (prevent crash during shutdown)
      if (tofEnabled) {
        DEBUG_PERFORMANCEF("[STACK] tof_task watermark_now=%u min=%u words", (unsigned)gTofWatermarkNow, (unsigned)gTofWatermarkMin);
        DEBUG_MEMORYF("[HEAP] tof_task: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }
    if (tofEnabled && tofConnected && gVL53L4CX != nullptr && !gSensorPollingPaused) {
      unsigned long tofPollMs = (gSettings.tofDevicePollMs > 0) ? (unsigned long)gSettings.tofDevicePollMs : 100;
      unsigned long nowMs = millis();
      if (nowMs - lastToFRead >= tofPollMs) {
        uint32_t tofHz = (gSettings.i2cClockToFHz > 0) ? (uint32_t)gSettings.i2cClockToFHz : 200000;
        bool ok = false;
        
        // ToF busy-waits up to 250ms for data ready; 500ms timeout gives headroom without over-blocking
        ok = i2cTaskWithTimeout(I2C_ADDR_TOF, tofHz, 500, [&]() -> bool {
          return readToFObjects();
        });
        
        lastToFRead = nowMs;
        
        // Auto-disable if too many consecutive failures
        if (!ok) {
          if (i2cShouldAutoDisable(I2C_ADDR_TOF, 5)) {
            ERROR_SENSORSF("Too many consecutive ToF failures - auto-disabling");
            tofEnabled = false;
            sensorStatusBumpWith("tof@auto_disabled");
          }
        }
        
        // SAFE: Debug output AFTER transaction, with enabled check
        if (tofEnabled) {
          DEBUG_TOF_FRAMEF("ToF readObjects: %s", ok ? "ok" : "fail");
        }
        
        // Stream data to ESP-NOW master if enabled (worker devices only)
#if ENABLE_ESPNOW
        // Check mesh mode (worker role) OR bond mode (worker role)
        bool shouldStream = false;
        if (meshEnabled() && gSettings.meshRole != MESH_ROLE_MASTER) {
          shouldStream = true;
        }
#if ENABLE_BONDED_MODE
        if (gSettings.bondModeEnabled && gSettings.bondRole == 0) {
          shouldStream = true;  // Bond mode worker
        }
#endif
        
        if (ok && shouldStream) {
          // Build ToF JSON from cache
          char tofJson[1024];
          int jsonLen = buildToFDataJSON(tofJson, sizeof(tofJson));
          if (jsonLen > 0) {
            sendSensorDataUpdate(REMOTE_SENSOR_TOF, String(tofJson));
          }
        }
#endif
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

// ============================================================================
// ToF Settings Module (for modular settings registry)
// ============================================================================

static const SettingEntry tofSettingEntries[] = {
  { "tofAutoStart",          SETTING_BOOL, &gSettings.tofAutoStart,          0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr },
  { "tofPollingMs",          SETTING_INT,  &gSettings.tofPollingMs,          220, 0, nullptr, 50, 5000, "Polling (ms)", nullptr },
  { "tofStabilityThreshold", SETTING_INT,  &gSettings.tofStabilityThreshold, 3, 0, nullptr, 0, 50, "Stability Threshold", nullptr },
  { "tofTransitionMs",       SETTING_INT,  &gSettings.tofTransitionMs,       200, 0, nullptr, 0, 5000, "Transition (ms)", nullptr },
  { "tofMaxDistanceMm",      SETTING_INT,  &gSettings.tofUiMaxDistanceMm,    3400, 0, nullptr, 100, 10000, "Max Distance (mm)", nullptr },
  { "tofDevicePollMs",       SETTING_INT,  &gSettings.tofDevicePollMs,       220, 0, nullptr, 100, 2000, "Poll Interval (ms)", nullptr },
  { "tofI2cClockHz",         SETTING_INT,  &gSettings.i2cClockToFHz,         200000, 0, nullptr, 50000, 400000, "I2C Clock (Hz)", nullptr }
};

static bool isToFConnected() {
  return tofConnected;
}

extern const SettingsModule tofSettingsModule = {
  "tof",
  "tof_vl53l4cx",
  tofSettingEntries,
  sizeof(tofSettingEntries) / sizeof(tofSettingEntries[0]),
  isToFConnected,
  "VL53L4CX time-of-flight distance sensor settings"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================
// ToF OLED Mode (Display Function + Registration)
// ============================================================================
#if DISPLAY_TYPE > 0
#include "i2csensor-vl53l4cx-oled.h"
#endif

#endif // ENABLE_TOF_SENSOR
