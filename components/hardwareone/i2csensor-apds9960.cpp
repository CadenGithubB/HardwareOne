#include "System_BuildConfig.h"

#if ENABLE_APDS_SENSOR

#include <Arduino.h>
#include "Adafruit_APDS9960.h"

#include "i2csensor-apds9960.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_MemoryMonitor.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_Utils.h"

// Debug macros (use centralized versions from debug_system.h)

// External dependencies
extern void sensorStatusBumpWith(const char* reason);
extern volatile bool gSensorPollingPaused;
extern SemaphoreHandle_t i2cMutex;
extern void drainDebugRing();

// APDS sensor object (owned by this module)
Adafruit_APDS9960* gAPDS9960 = nullptr;

// ============================================================================
// APDS/Peripheral Sensor Cache (owned by this module)
// ============================================================================
PeripheralCache gPeripheralCache;

// APDS sensor state (definitions - matching pattern of thermal/tof/imu/gamepad sensors)
bool apdsColorEnabled = false;
bool apdsProximityEnabled = false;
bool apdsGestureEnabled = false;
bool apdsConnected = false;
TaskHandle_t apdsTaskHandle = nullptr;

// Helper: Create APDS task if not already running
static bool createAPDSTask() {
  extern BaseType_t xTaskCreateLogged(TaskFunction_t, const char*, uint32_t, void*, UBaseType_t, TaskHandle_t*, const char*);
  extern void apdsTask(void* parameter);
  
  // Check for stale task handle (task deleted itself but handle not cleared)
  if (apdsTaskHandle != nullptr) {
    eTaskState state = eTaskGetState(apdsTaskHandle);
    if (state == eDeleted || state == eInvalid) {
      apdsTaskHandle = nullptr;
    }
  }
  if (apdsTaskHandle == nullptr) {
    const uint32_t apdsStack = 3072;
    if (xTaskCreateLogged(apdsTask, "apds_task", apdsStack, nullptr, 1, &apdsTaskHandle, "apds") != pdPASS) {
      return false;
    }
    DEBUG_CLIF("APDS task created successfully");
  }
  return true;
}

// ============================================================================
// APDS Modular Settings Registration (for safety and reliability)
// ============================================================================

// APDS settings entries
static const SettingEntry apdsSettingEntries[] = {
  // Core settings
  { "autoStart", SETTING_BOOL, &gSettings.apdsAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr },
  // Device-level settings (sensor hardware behavior)
  { "device.devicePollMs", SETTING_INT, &gSettings.apdsDevicePollMs, 100, 0, nullptr, 50, 5000, "Poll Interval (ms)", nullptr }
};

static bool isAPDSConnected() {
  return apdsConnected;
}

extern const SettingsModule apdsSettingsModule = {
  "apds",
  nullptr,
  apdsSettingEntries,
  sizeof(apdsSettingEntries) / sizeof(apdsSettingEntries[0]),
  isAPDSConnected,
  "APDS9960 gesture/color/proximity sensor settings"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp


// ============================================================================
// APDS Sensor Command Handlers
// ============================================================================

const char* cmd_apdscolor(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  readAPDSColor();
  return "APDS color data read (check serial output)";
}

const char* cmd_apdsproximity(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  readAPDSProximity();
  return "APDS proximity data read (check serial output)";
}

const char* cmd_apdsgesture(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  readAPDSGesture();
  return "APDS gesture data read (check serial output)";
}

// Unified APDS start command using queue system (consistent with other sensors)
const char* cmd_apdsstart(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Use sensor queue system (consistent with Thermal, ToF, IMU, GPS, Gamepad)
  extern bool enqueueSensorStart(SensorType sensor);
  extern bool isInQueue(SensorType sensor);
  extern int getQueuePosition(SensorType sensor);
  extern bool ensureDebugBuffer();
  
  bool anyEnabled = apdsColorEnabled || apdsProximityEnabled || apdsGestureEnabled;
  if (anyEnabled) {
    return "[APDS] Error: Already running";
  }
  
  if (isInQueue(SENSOR_APDS)) {
    if (!ensureDebugBuffer()) return "[APDS] Already in queue";
    int pos = getQueuePosition(SENSOR_APDS);
    snprintf(getDebugBuffer(), 1024, "[APDS] Already in queue at position %d", pos);
    return getDebugBuffer();
  }
  
  if (enqueueSensorStart(SENSOR_APDS)) {
    sensorStatusBumpWith("apdsstart@enqueue");
    if (!ensureDebugBuffer()) return "[APDS] Sensor queued for start";
    int pos = getQueuePosition(SENSOR_APDS);
    snprintf(getDebugBuffer(), 1024, "[APDS] Sensor queued for start (position %d)", pos);
    return getDebugBuffer();
  }
  
  return "[APDS] Error: Failed to enqueue start (queue full)";
}

// Deprecated: Use 'apdsstart' then 'apdsmode color'
const char* cmd_apdscolorstart(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return "[APDS] Deprecated: Use 'apdsstart' to start sensor, then 'apdsmode color' to enable color sensing";
}

// Stop APDS sensor (all modes)
const char* cmd_apdsstop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  bool anyEnabled = apdsColorEnabled || apdsProximityEnabled || apdsGestureEnabled;
  if (!anyEnabled) {
    return "[APDS] Error: Not running";
  }
  
  // Disable all modes - task will see this and clean up
  apdsColorEnabled = false;
  apdsProximityEnabled = false;
  apdsGestureEnabled = false;
  
  sensorStatusBumpWith("apdsstop@CLI");
  return "[APDS] Sensor stop requested; cleanup will complete asynchronously";
}

// Deprecated: Use 'apdsstop' or 'apdsmode color off'
const char* cmd_apdscolorstop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return "[APDS] Deprecated: Use 'apdsstop' to stop sensor, or 'apdsmode color off' to disable color mode";
}

// Deprecated: Use 'apdsstart' then 'apdsmode proximity'
const char* cmd_apdsproximitystart(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return "[APDS] Deprecated: Use 'apdsstart' to start sensor, then 'apdsmode proximity' to enable proximity sensing";
}

// Deprecated: Use 'apdsstop' or 'apdsmode proximity off'
const char* cmd_apdsproximitystop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return "[APDS] Deprecated: Use 'apdsstop' to stop sensor, or 'apdsmode proximity off' to disable proximity mode";
}

// Deprecated: Use 'apdsstart' then 'apdsmode gesture'
const char* cmd_apdsgesturestart(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return "[APDS] Deprecated: Use 'apdsstart' to start sensor, then 'apdsmode gesture' to enable gesture sensing";
}

// Deprecated: Use 'apdsstop' or 'apdsmode gesture off'
const char* cmd_apdsgesturestop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return "[APDS] Deprecated: Use 'apdsstop' to stop sensor, or 'apdsmode gesture off' to disable gesture mode";
}

// Runtime mode control (once sensor is running)
const char* cmd_apdsmode(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!apdsConnected || gAPDS9960 == nullptr) {
    return "[APDS] Error: Sensor not initialized - use 'apdsstart' first";
  }
  
  String lc = cmd;
  lc.toLowerCase();
  lc.trim();
  
  // Parse: apdsmode <color|proximity|gesture> [on|off]
  int sp1 = lc.indexOf(' ');
  if (sp1 < 0) {
    if (!ensureDebugBuffer()) return "[APDS] Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024, "[APDS] Modes: color=%s proximity=%s gesture=%s",
             apdsColorEnabled ? "on" : "off",
             apdsProximityEnabled ? "on" : "off",
             apdsGestureEnabled ? "on" : "off");
    return getDebugBuffer();
  }
  
  int sp2 = lc.indexOf(' ', sp1 + 1);
  String mode = (sp2 > 0) ? lc.substring(sp1 + 1, sp2) : lc.substring(sp1 + 1);
  String state = (sp2 > 0) ? lc.substring(sp2 + 1) : "on";
  mode.trim();
  state.trim();
  
  bool enable = (state == "on" || state == "1" || state == "true");
  
  if (mode == "color") {
    gAPDS9960->enableColor(enable);
    apdsColorEnabled = enable;
    sensorStatusBumpWith(enable ? "apdsmode color on" : "apdsmode color off");
    return enable ? "[APDS] Color mode enabled" : "[APDS] Color mode disabled";
  } else if (mode == "proximity" || mode == "prox") {
    gAPDS9960->enableProximity(enable);
    apdsProximityEnabled = enable;
    sensorStatusBumpWith(enable ? "apdsmode proximity on" : "apdsmode proximity off");
    return enable ? "[APDS] Proximity mode enabled" : "[APDS] Proximity mode disabled";
  } else if (mode == "gesture") {
    if (enable) {
      gAPDS9960->enableProximity(true);
      gAPDS9960->enableGesture(true);
    } else {
      gAPDS9960->enableGesture(false);
      gAPDS9960->enableProximity(false);
    }
    apdsGestureEnabled = enable;
    sensorStatusBumpWith(enable ? "apdsmode gesture on" : "apdsmode gesture off");
    return enable ? "[APDS] Gesture mode enabled" : "[APDS] Gesture mode disabled";
  }
  
  return "[APDS] Error: Unknown mode - use 'color', 'proximity', or 'gesture'";
}

// ============================================================================
// APDS Sensor Initialization and Reading Functions
// ============================================================================

// Internal function called by queue processor
bool startAPDSSensorInternal() {
  // Check memory before creating task
  if (!checkMemoryAvailable("apds", nullptr)) {
    ERROR_SENSORSF("[APDS] Error: Insufficient memory for APDS sensor");
    return false;
  }

  // Clean up any stale cache from previous run BEFORE starting
  if (gPeripheralCache.mutex && xSemaphoreTake(gPeripheralCache.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    gPeripheralCache.apdsDataValid = false;
    gPeripheralCache.apdsRed = 0;
    gPeripheralCache.apdsGreen = 0;
    gPeripheralCache.apdsBlue = 0;
    gPeripheralCache.apdsClear = 0;
    gPeripheralCache.apdsProximity = 0;
    gPeripheralCache.apdsGesture = 0;
    xSemaphoreGive(gPeripheralCache.mutex);
  }
  INFO_SENSORSF("[APDS] Cleaned up stale cache from previous run");

  // Initialize APDS sensor synchronously
  if (!apdsConnected || gAPDS9960 == nullptr) {
    if (!initAPDS9960()) {
      ERROR_SENSORSF("[APDS] Error: Failed to initialize APDS9960 sensor");
      return false;
    }
  }

  // Enable color mode by default (user can change with apdsmode command)
  gAPDS9960->enableColor(true);
  apdsColorEnabled = true;
  INFO_SENSORSF("[APDS] Color mode enabled by default");

  // Create APDS task
  if (!createAPDSTask()) {
    ERROR_SENSORSF("[APDS] Error: Failed to create APDS task");
    apdsColorEnabled = false;
    return false;
  }

  sensorStatusBumpWith("APDS initialized");
  INFO_SENSORSF("[APDS] Sensor started successfully (color mode active)");
  return true;
}

bool initAPDS9960() {
  if (gAPDS9960 != nullptr) {
    return true;
  }
  
  // Use i2cTransaction wrapper for safe mutex + clock management
  return i2cTransaction(100000, 500, [&]() -> bool {
    gAPDS9960 = new Adafruit_APDS9960();
    if (!gAPDS9960) return false;
    
    if (!gAPDS9960->begin()) {
      delete gAPDS9960;
      gAPDS9960 = nullptr;
      return false;
    }
    
    apdsConnected = true;
    
    // Register APDS for I2C health tracking (address 0x39)
    i2cRegisterDevice(0x39, "APDS");
    return true;
  });
}

void readAPDSColor() {
  if (!apdsConnected || gAPDS9960 == nullptr) {
    broadcastOutput("APDS9960 sensor not connected or initialized");
    return;
  }

  if (!apdsColorEnabled) {
    broadcastOutput("Color sensing not enabled. Use 'apdscolorstart' first.");
    return;
  }

  // Create variables to store the color data in
  uint16_t red, green, blue, clear;

  // Wait for color data to be ready
  while (!gAPDS9960->colorDataReady()) {
    delay(5);
  }

  // Get the data and print the different channels
  gAPDS9960->getColorData(&red, &green, &blue, &clear);

  // Find closest matching color and set NeoPixel
  // RGB closestRGB;
  // String colorName = getClosestColorName(red, green, blue, closestRGB);
  // setLEDColor(closestRGB);

  String colorData = "Red: " + String(red) + ", Green: " + String(green) + ", Blue: " + String(blue) + ", Clear: " + String(clear);
  // colorData += " -> Detected: " + colorName + " (" + String(closestRGB.r) + "," + String(closestRGB.g) + "," + String(closestRGB.b) + ")";
  broadcastOutput(colorData);
}

void readAPDSProximity() {
  if (!apdsConnected || gAPDS9960 == nullptr) {
    broadcastOutput("APDS9960 sensor not connected or initialized");
    return;
  }

  if (!apdsProximityEnabled) {
    broadcastOutput("Proximity sensing not enabled. Use 'apdsproximitystart' first.");
    return;
  }

  uint8_t proximity = gAPDS9960->readProximity();
  String proximityData = "Proximity: " + String(proximity);
  broadcastOutput(proximityData);
}

void readAPDSGesture() {
  if (!apdsConnected || gAPDS9960 == nullptr) {
    broadcastOutput("APDS9960 sensor not connected or initialized");
    return;
  }

  if (!apdsGestureEnabled) {
    broadcastOutput("Gesture sensing not enabled. Use 'apdsgesturestart' first.");
    return;
  }

  uint8_t gesture = gAPDS9960->readGesture();
  if (gesture == APDS9960_DOWN) broadcastOutput("Gesture: DOWN");
  if (gesture == APDS9960_UP) broadcastOutput("Gesture: UP");
  if (gesture == APDS9960_LEFT) broadcastOutput("Gesture: LEFT");
  if (gesture == APDS9960_RIGHT) broadcastOutput("Gesture: RIGHT");
  if (gesture == 0) broadcastOutput("No gesture detected");
}

// ============================================================================
// APDS Command Registry
// ============================================================================

const CommandEntry apdsCommands[] = {
  // Primary commands (queue-based startup, consistent with other sensors)
  { "apdsstart", "Start APDS9960 sensor.", false, cmd_apdsstart },
  { "apdsstop", "Stop APDS9960 sensor.", false, cmd_apdsstop },
  { "apdsmode", "Control APDS modes: apdsmode <color|proximity|gesture> [on|off].", false, cmd_apdsmode },
  
  // Read commands
  { "apdscolor", "Read APDS9960 color values.", false, cmd_apdscolor },
  { "apdsproximity", "Read APDS9960 proximity value.", false, cmd_apdsproximity },
  { "apdsgesture", "Read APDS9960 gesture.", false, cmd_apdsgesture },
  
  // Deprecated commands (backward compatibility with deprecation warnings)
  { "apdscolorstart", "[DEPRECATED] Use 'apdsstart' + 'apdsmode color'.", false, cmd_apdscolorstart },
  { "apdscolorstop", "[DEPRECATED] Use 'apdsstop' or 'apdsmode color off'.", false, cmd_apdscolorstop },
  { "apdsproximitystart", "[DEPRECATED] Use 'apdsstart' + 'apdsmode proximity'.", false, cmd_apdsproximitystart },
  { "apdsproximitystop", "[DEPRECATED] Use 'apdsstop' or 'apdsmode proximity off'.", false, cmd_apdsproximitystop },
  { "apdsgesturestart", "[DEPRECATED] Use 'apdsstart' + 'apdsmode gesture'.", false, cmd_apdsgesturestart },
  { "apdsgesturestop", "[DEPRECATED] Use 'apdsstop' or 'apdsmode gesture off'.", false, cmd_apdsgesturestop },
};

const size_t apdsCommandsCount = sizeof(apdsCommands) / sizeof(apdsCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _apds_cmd_registrar(apdsCommands, apdsCommandsCount, "apds");

// ============================================================================
// APDS Task Implementation (moved from i2c_system.cpp for full modularization)
// ============================================================================

// ============================================================================
// APDS Task - FreeRTOS Task Function
// ============================================================================
// Purpose: Continuously reads color/proximity/gesture data from APDS9960 sensor
// Stack: 3072 words (~12KB) | Priority: 1 | Core: Any
// Lifecycle: Created by cmd_apdsstart, deleted when all modes disabled
// Polling: Fixed 100ms interval | I2C Clock: 100kHz
//
// Cleanup Strategy:
//   1. Check if all modes (color/proximity/gesture) are disabled
//   2. Acquire i2cMutex to prevent race conditions during cleanup
//   3. Delete sensor object and invalidate cache
//   4. Release mutex and delete task
// ============================================================================

void apdsTask(void* parameter) {
  INFO_SENSORSF("[APDS] Task started (handle=%p, stack=%u words)", 
                (void*)xTaskGetCurrentTaskHandle(), 
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  Serial.println("[MODULAR] apdsTask() running from Sensor_APDS_APDS9960.cpp");
  unsigned long lastApdsRead = 0;
  unsigned long lastStackLog = 0;
  // Note: Failure tracking now handled by centralized I2CDevice health system
  // Use i2cShouldAutoDisable() instead of local counters

  while (true) {
    // CRITICAL: Check if all modes disabled for graceful shutdown
    bool anyEnabled = apdsColorEnabled || apdsProximityEnabled || apdsGestureEnabled;
    if (!anyEnabled) {
      // Perform safe cleanup before task deletion - RACE CONDITION FIX:
      // Take I2C mutex to ensure no other tasks are in active APDS I2C transactions
      if (apdsConnected || gAPDS9960 != nullptr) {
        // Wait for all active APDS I2C transactions to complete before cleanup
        if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
          // Now safe to delete - no other tasks can access APDS sensor
          apdsConnected = false;
          if (gAPDS9960 != nullptr) {
            delete gAPDS9960;
            gAPDS9960 = nullptr;
          }
          
          // Invalidate cache - no locking needed since we hold i2cMutex
          gPeripheralCache.apdsDataValid = false;
          
          xSemaphoreGive(i2cMutex);
          
          // Brief delay to ensure cleanup propagates
          vTaskDelay(pdMS_TO_TICKS(50));
        } else {
          // Mutex timeout - force cleanup anyway to prevent deadlock
          apdsConnected = false;
          if (gAPDS9960 != nullptr) {
            delete gAPDS9960;
            gAPDS9960 = nullptr;
          }
          gPeripheralCache.apdsDataValid = false;
          vTaskDelay(pdMS_TO_TICKS(100));
        }
      }
      
      // ALWAYS delete task when disabled (consistent with thermal/IMU/ToF)
      // NOTE: Do NOT clear apdsTaskHandle here - let create function use eTaskGetState()
      // to detect stale handles. Clearing here creates a race condition window.
      vTaskDelete(nullptr);
    }

    // Stack watermark tracking
    unsigned long nowMs = millis();
    if ((nowMs - lastStackLog) >= 10000) {
      lastStackLog = nowMs;
      if (anyEnabled && isDebugFlagSet(DEBUG_PERFORMANCE)) {
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
        DEBUG_PERFORMANCEF("[STACK] apds_task watermark=%u words", (unsigned)watermark);
      }
      if (anyEnabled && isDebugFlagSet(DEBUG_MEMORY)) {
        DEBUG_MEMORYF("[HEAP] apds_task: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }
    
    if (anyEnabled && apdsConnected && !gSensorPollingPaused) {
      unsigned long apdsPollMs = (gSettings.apdsDevicePollMs > 0) ? (unsigned long)gSettings.apdsDevicePollMs : 100;
      
      if ((nowMs - lastApdsRead) >= apdsPollMs) {
        uint16_t red = 0, green = 0, blue = 0, clear = 0;
        uint8_t proximity = 0;
        uint8_t gesture = 0;
        
        // Use task timeout wrapper to catch APDS I2C performance issues
        bool result = i2cTaskWithStandardTimeout(I2C_ADDR_APDS, 100000, [&]() -> bool {
          if (apdsColorEnabled && gAPDS9960->colorDataReady()) {
            gAPDS9960->getColorData(&red, &green, &blue, &clear);
          }
          if (apdsProximityEnabled) {
            proximity = gAPDS9960->readProximity();
          }
          if (apdsGestureEnabled) {
            gesture = gAPDS9960->readGesture();
          }
          return true;
        });

        if (result) {
          // Note: I2CDevice::recordSuccess() called automatically by transaction
          // which resets consecutiveErrors - no local counter needed
          
          if (gPeripheralCache.mutex && xSemaphoreTake(gPeripheralCache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            gPeripheralCache.apdsRed = red;
            gPeripheralCache.apdsGreen = green;
            gPeripheralCache.apdsBlue = blue;
            gPeripheralCache.apdsClear = clear;
            gPeripheralCache.apdsProximity = proximity;
            gPeripheralCache.apdsGesture = gesture;
            gPeripheralCache.apdsLastUpdate = nowMs;
            gPeripheralCache.apdsDataValid = true;
            xSemaphoreGive(gPeripheralCache.mutex);
          }
        } else {
          // Note: I2CDevice::recordError() called automatically by transaction
          // Check centralized health tracking for auto-disable decision
          if (i2cShouldAutoDisable(I2C_ADDR_APDS, 5)) {
            uint8_t errors = i2cGetConsecutiveErrors(I2C_ADDR_APDS);
            apdsColorEnabled = false;
            apdsProximityEnabled = false;
            apdsGestureEnabled = false;
            apdsConnected = false;
            sensorStatusBumpWith("apds@auto_disabled");
            DEBUG_FRAMEF("APDS auto-disabled after %u consecutive I2C failures", errors);
            break;
          }
        }
        lastApdsRead = nowMs;
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));
    drainDebugRing();
  }
}

// ============================================================================
// APDS OLED Mode Registration
// ============================================================================

#if ENABLE_OLED_DISPLAY
#include "i2csensor-apds9960-oled.h"
#endif // ENABLE_OLED_DISPLAY

#endif // ENABLE_APDS_SENSOR
