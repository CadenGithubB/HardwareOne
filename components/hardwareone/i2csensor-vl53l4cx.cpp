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
extern bool writeSettingsJson();

// I2C functions
extern uint32_t gWire1DefaultHz;
extern void i2cSetWire1Clock(uint32_t hz);

// Status and output
extern void sensorStatusBumpWith(const char* reason);
extern void broadcastOutput(const char* msg);
extern volatile bool gSensorPollingPaused;
extern SemaphoreHandle_t i2cMutex;
extern void drainDebugRing();

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

// Queue system functions now in i2c_system.h (included via Sensor_ToF_VL53L4CX.h)
extern void sensorStatusBumpWith(const char* cause);

// ============================================================================
// ToF Sensor Reading Functions (moved from .ino)
// ============================================================================

float readToFDistance() {
  if (!tofConnected || !tofEnabled || gVL53L4CX == nullptr) {
    if (!tofConnected) {
      broadcastOutput("ToF sensor not connected. Check wiring.");
    } else if (!tofEnabled) {
      broadcastOutput("ToF sensor not started. Use 'tofstart' first.");
    } else {
      broadcastOutput("ToF sensor initialization failed.");
    }
    return 999.9;
  }

  // Use i2cTransaction wrapper for safe mutex + clock management
  uint32_t clockHz = (gSettings.i2cClockToFHz > 0) ? (uint32_t)gSettings.i2cClockToFHz : 100000;
  float result = i2cTransaction(clockHz, 200, [&]() -> float {
    VL53L4CX_MultiRangingData_t MultiRangingData;
    VL53L4CX_MultiRangingData_t* pMultiRangingData = &MultiRangingData;
    uint8_t NewDataReady = 0;
    VL53L4CX_Error status;

    // Wait for data ready
    do {
      status = gVL53L4CX->VL53L4CX_GetMeasurementDataReady(&NewDataReady);
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
          String distanceData = "Distance: " + String(best_distance, 1) + " cm";
          broadcastOutput(distanceData);
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
  // Hold I2C clock steady for ToF while enabled (bounded 50k..400k)
  {
    uint32_t prevClock = gWire1DefaultHz;  // Save previous clock
    uint32_t tofHz = (gSettings.i2cClockToFHz > 0) ? gSettings.i2cClockToFHz : 200000;
    if (tofHz < 50000) tofHz = 50000;
    if (tofHz > 400000) tofHz = 400000;

    // Only change clock if different, add settling delay
    if (prevClock != tofHz) {
      i2cSetWire1Clock(tofHz);
      delay(100);  // Let I2C bus settle after clock change
    }
  }
  // tofEnabled already set to true at the beginning to prevent race condition
  sensorStatusBumpWith("tofstart@queue");
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
  if (isInQueue(SENSOR_TOF)) {
    int pos = getQueuePosition(SENSOR_TOF);
    BROADCAST_PRINTF("ToF sensor already queued (position %d)", pos);
    return "[ToF] Already queued";
  }

  // Enqueue the request to centralized queue
  if (enqueueSensorStart(SENSOR_TOF)) {
    sensorStatusBumpWith("tofstart@enqueue");
    int pos = getQueuePosition(SENSOR_TOF);
    BROADCAST_PRINTF("ToF sensor queued for start (position %d)", pos);
    return "[ToF] Sensor queued for start";
  } else {
    return "[ToF] Error: Failed to enqueue start (queue full)";
  }
}

const char* cmd_tofstop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Disable flag so the task stops polling and performs cleanup itself
  tofEnabled = false;
  tofLastStopTime = millis();
  
  // Broadcast sensor status to ESP-NOW master
#if ENABLE_ESPNOW
  broadcastSensorStatus(REMOTE_SENSOR_TOF, false);
#endif
  
  // Note: Status bump removed - was causing xQueueGenericSend crash
  // The ToF task will handle cleanup and status updates asynchronously

  // Return immediately; task will stop measurement and free resources safely
  return "[ToF] Stop requested; cleanup will complete asynchronously";
}

const char* cmd_toftransitionms(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: toftransitionms <0..5000>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 0 || v > 5000) return "[ToF] Error: Transition time must be 0-5000ms";
  gSettings.tofTransitionMs = v;
  writeSettingsJson();
  BROADCAST_PRINTF("tofTransitionMs set to %d", v);
  return "[ToF] Setting updated";
}

const char* cmd_tofuimaxdistancemm(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: tofuimaxdistancemm <100..10000>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 100 || v > 10000) return "[ToF] Error: Max distance must be 100-10000mm";
  gSettings.tofUiMaxDistanceMm = v;
  writeSettingsJson();
  BROADCAST_PRINTF("tofUiMaxDistanceMm set to %d", v);
  return "[ToF] Setting updated";
}

// ============================================================================
// ToF Sensor Initialization and Reading Functions
// ============================================================================

bool initToFSensor() {
  extern void i2cSetDefaultWire1Clock();
  
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
  
  return i2cTransaction(tofHz, 3000, [&]() -> bool {
    // Wire1 is configured centrally with runtime-configurable pins
    i2cSetDefaultWire1Clock();
    
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
    
    // Register ToF sensor for I2C health tracking
    i2cRegisterDevice(I2C_ADDR_TOF, "ToF");
    return true;
  });
}

bool readToFObjects() {
  // gDebugFlags now from debug_system.h
  extern void i2cSetWire1Clock(uint32_t hz);
  
  if (!tofConnected || !tofEnabled || gVL53L4CX == nullptr) {
    if (!tofConnected) {
      broadcastOutput("ToF sensor not connected. Check wiring.");
    } else if (!tofEnabled) {
      broadcastOutput("ToF sensor not started. Use 'tofstart' first.");
    } else {
      broadcastOutput("ToF sensor initialization failed.");
    }
    return false;
  }

  // Set I2C speed for ToF reads (sticky - stays until next sensor changes it)
  uint32_t tofHz = (gSettings.i2cClockToFHz > 0) ? (uint32_t)gSettings.i2cClockToFHz : 200000;
  i2cSetWire1Clock(tofHz);

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

      if (isDebugFlagSet(DEBUG_SENSORS_FRAME)) {
        DEBUG_FRAMEF("ToF obj[%d]: range=%dmm, status=%d, signal=%.3f (min=%.3f), isValid=%d, hasGoodSignal=%d",
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

    if (isDebugFlagSet(DEBUG_SENSORS_FRAME)) {
      DEBUG_FRAMEF("readToFObjects: found=%d, valid=%d, seq=%lu",
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
      if (isDebugFlagSet(DEBUG_SENSORS_FRAME)) {
        DEBUG_FRAMEF("buildToFDataJSON: tofDataValid=%s, tofEnabled=%d, tofConnected=%d, lastUpdate=%lu",
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
  gSettings.tofPollingMs = v;
  writeSettingsJson();
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
  gSettings.tofStabilityThreshold = v;
  writeSettingsJson();
  BROADCAST_PRINTF("tofStabilityThreshold set to %d", v);
  return "[ToF] Setting updated";
}

// ============================================================================
// ToF Command Registry
// ============================================================================

const char* cmd_tofdevicepollms(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  int sp1 = originalCmd.indexOf(' ');
  if (sp1 < 0) return "Usage: tofDevicePollMs <100..2000>";
  String valStr = originalCmd.substring(sp1 + 1);
  valStr.trim();
  int v = valStr.toInt();
  if (v < 100) v = 100;
  if (v > 2000) v = 2000;
  gSettings.tofDevicePollMs = v;
  writeSettingsJson();
  applySettings();
  snprintf(getDebugBuffer(), 1024, "tofDevicePollMs set to %d", v);
  return getDebugBuffer();
}

// External device-level command handler (defined in i2c_system.cpp)
// extern const char* cmd_tofdevicepollms(const String& cmd);

const CommandEntry tofCommands[] = {
  // Start/Stop/Read
  { "tofstart", "Start VL53L4CX ToF sensor.", false, cmd_tofstart },
  { "tofstop", "Stop VL53L4CX ToF sensor.", false, cmd_tofstop },
  { "tof", "Read ToF distance sensor.", false, cmd_tof },
  
  // UI Settings (client-side visualization)
  { "tofpollingms", "Set ToF UI polling interval (50..5000ms).", true, cmd_tofpollingms, "Usage: tofpollingms <50..5000>" },
  { "tofstabilitythreshold", "Set ToF stability threshold (0..50).", true, cmd_tofstabilitythreshold, "Usage: tofstabilitythreshold <0..50>" },
  { "toftransitionms", "Set ToF transition time.", true, cmd_toftransitionms, "Usage: toftransitionms <0..5000>" },
  { "tofuimaxdistancemm", "Set ToF UI max distance.", true, cmd_tofuimaxdistancemm, "Usage: tofuimaxdistancemm <100..10000>" },
  
  // Device-level settings (sensor hardware behavior)
  { "tofdevicepollms", "Set ToF device polling interval.", true, cmd_tofdevicepollms, "Usage: tofDevicePollMs <100..2000>" },
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
  Serial.println("[MODULAR] tofTask() running from Sensor_ToF_VL53L4CX.cpp");
  unsigned long lastToFRead = 0;
  unsigned long lastStackLog = 0;
  while (true) {
    // CRITICAL: Check enabled flag FIRST for graceful shutdown
    if (!tofEnabled) {
      // Perform safe cleanup before task deletion - RACE CONDITION FIX:
      // Take I2C mutex to ensure no other tasks are in active ToF I2C transactions
      if (tofConnected || gVL53L4CX != nullptr) {
        // Wait for all active ToF I2C transactions to complete before cleanup
        if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
          // Now safe to delete - no other tasks can access ToF sensor
          if (gVL53L4CX != nullptr) {
            (void)gVL53L4CX->VL53L4CX_StopMeasurement();
          }
          tofConnected = false;
          if (gVL53L4CX != nullptr) {
            delete gVL53L4CX;
            gVL53L4CX = nullptr;
          }
          
          // Invalidate cache - no locking needed since we hold i2cMutex
          gTofCache.tofDataValid = false;
          gTofCache.tofTotalObjects = 0;
          for (int j = 0; j < 4; j++) {
            gTofCache.tofObjects[j].detected = false;
            gTofCache.tofObjects[j].valid = false;
          }
          
          xSemaphoreGive(i2cMutex);
          
          // Brief delay to ensure cleanup propagates
          vTaskDelay(pdMS_TO_TICKS(50));
        } else {
          // Mutex timeout - force cleanup anyway to prevent deadlock
          WARN_SENSORSF("[ToF] Mutex timeout during cleanup, forcing cleanup");
          if (gVL53L4CX != nullptr) {
            (void)gVL53L4CX->VL53L4CX_StopMeasurement();
          }
          tofConnected = false;
          if (gVL53L4CX != nullptr) {
            delete gVL53L4CX;
            gVL53L4CX = nullptr;
          }
          gTofCache.tofDataValid = false;
          gTofCache.tofTotalObjects = 0;
          for (int j = 0; j < 4; j++) {
            gTofCache.tofObjects[j].detected = false;
            gTofCache.tofObjects[j].valid = false;
          }
          vTaskDelay(pdMS_TO_TICKS(100));
        }
      }
      
      // ALWAYS delete task when disabled (consistent with thermal/IMU)
      // NOTE: Do NOT clear tofTaskHandle here - let create function use eTaskGetState()
      // to detect stale handles. Clearing here creates a race condition window.
      INFO_SENSORSF("[ToF] Task cleanup complete, deleting task");
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
        
        // Device-aware I2C transaction with automatic health and timeout tracking (1000ms max)
        ok = i2cTaskWithStandardTimeout(I2C_ADDR_TOF, tofHz, [&]() -> bool {
          return readToFObjects();
        });
        
        lastToFRead = nowMs;
        
        // SAFE: Debug output AFTER transaction, with enabled check
        if (tofEnabled) {
          DEBUG_FRAMEF("ToF readObjects: %s", ok ? "ok" : "fail");
        }
        
        // Stream data to ESP-NOW master if enabled (worker devices only)
#if ENABLE_ESPNOW
        if (ok && meshEnabled() && gSettings.meshRole != MESH_ROLE_MASTER) {
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
  // UI Settings
  { "ui.pollingMs", SETTING_INT, &gSettings.tofPollingMs, 220, 0, nullptr, 50, 5000, "Polling (ms)", nullptr },
  { "ui.stabilityThreshold", SETTING_INT, &gSettings.tofStabilityThreshold, 3, 0, nullptr, 0, 50, "Stability Threshold", nullptr },
  { "ui.transitionMs", SETTING_INT, &gSettings.tofTransitionMs, 200, 0, nullptr, 0, 5000, "Transition (ms)", nullptr },
  { "ui.maxDistanceMm", SETTING_INT, &gSettings.tofUiMaxDistanceMm, 3400, 0, nullptr, 100, 10000, "Max Distance (mm)", nullptr },
  // Device Settings
  { "device.devicePollMs", SETTING_INT, &gSettings.tofDevicePollMs, 220, 0, nullptr, 100, 2000, "Poll Interval (ms)", nullptr },
  { "device.i2cClockHz", SETTING_INT, &gSettings.i2cClockToFHz, 200000, 0, nullptr, 50000, 400000, "I2C Clock (Hz)", nullptr }
};

static bool isToFConnected() {
  return tofConnected;
}

static const SettingsModule tofSettingsModule = {
  "tof",
  "tof_vl53l4cx",
  tofSettingEntries,
  sizeof(tofSettingEntries) / sizeof(tofSettingEntries[0]),
  isToFConnected,
  "VL53L4CX time-of-flight distance sensor settings"
};

// Auto-register on startup
static struct TofSettingsRegistrar {
  TofSettingsRegistrar() { registerSettingsModule(&tofSettingsModule); }
} _tofSettingsRegistrar;

// ============================================================================
// ToF OLED Mode (Display Function + Registration)
// ============================================================================
#include "i2csensor-vl53l4cx-oled.h"

#endif // ENABLE_TOF_SENSOR
