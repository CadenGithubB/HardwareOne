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

// External dependencies provided by System_I2C.h:
// sensorStatusBumpWith, gSensorPollingPaused, drainDebugRing

// APDS sensor object (owned by this module)
Adafruit_APDS9960* gAPDS9960 = nullptr;

// ============================================================================
// APDS/Peripheral Sensor Cache (owned by this module)
// ============================================================================
APDSCache gAPDSCache;

// APDS sensor state (definitions - matching pattern of thermal/tof/imu/gamepad sensors)
bool gApdsEnabled = false;
bool gApdsColorEnabled = false;
bool gApdsProximityEnabled = false;
bool gApdsGestureEnabled = false;
bool gApdsConnected = false;
unsigned long gApdsLastStopTime = 0;
TaskHandle_t gApdsTaskHandle = nullptr;

// Task creation now handled by createAPDSTask() in System_TaskUtils.cpp

// ============================================================================
// APDS Modular Settings Registration (for safety and reliability)
// ============================================================================

// APDS settings entries
// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry apdsSettingEntries[] = {
  { "apdsAutoStart", SETTING_BOOL, &gSettings.apdsAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr, false, nullptr, nullptr },
  { "apdsDevicePollMs", SETTING_INT, &gSettings.apdsDevicePollMs, 200, 0, nullptr, 50, 5000, "Poll Interval (ms)", nullptr, false, nullptr, nullptr }
};

static bool isAPDSConnected() {
  return gApdsConnected;
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule apdsSettingsModule = {
  "apds",
  "hardware.sensors.apds",
  apdsSettingEntries,
  sizeof(apdsSettingEntries) / sizeof(apdsSettingEntries[0]),
  isAPDSConnected,
  "APDS9960 gesture/color/proximity sensor settings"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp


// ============================================================================
// APDS Sensor Command Handlers
// ============================================================================

const char* cmd_apdscolor(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  apdsColorPoll();
  return "APDS color data read (check serial output)";
}

const char* cmd_apdsproximity(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  apdsProximityPoll();
  return "APDS proximity data read (check serial output)";
}

const char* cmd_apdsgesture(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  apdsGesturePoll();
  return "APDS gesture data read (check serial output)";
}

const char* cmd_apdsread(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  bool anyEnabled = gApdsColorEnabled || gApdsProximityEnabled || gApdsGestureEnabled;
  if (!anyEnabled) {
    return "[APDS] Not running. Use 'openapds' to start.";
  }

  if (!ensureDebugBuffer()) return "[APDS] Error: buffer unavailable";
  char* buf = getDebugBuffer();
  int remaining = 1024;
  int n = 0;

  n = snprintf(buf, remaining, "APDS9960 Status:\n");
  buf += n; remaining -= n;

  n = snprintf(buf, remaining, "  Color: %s  Proximity: %s  Gesture: %s\n",
               gApdsColorEnabled ? "ON" : "OFF",
               gApdsProximityEnabled ? "ON" : "OFF",
               gApdsGestureEnabled ? "ON" : "OFF");
  buf += n; remaining -= n;

  if (gApdsColorEnabled) {
    n = snprintf(buf, remaining, "  RGBC: R=%u G=%u B=%u C=%u\n",
                 gAPDSCache.red, gAPDSCache.green, gAPDSCache.blue, gAPDSCache.clear);
    buf += n; remaining -= n;
  }
  if (gApdsProximityEnabled) {
    n = snprintf(buf, remaining, "  Proximity: %u\n", gAPDSCache.proximity);
    buf += n; remaining -= n;
  }

  return getDebugBuffer();
}

// Unified APDS start command using queue system (consistent with other sensors)
const char* cmd_apdsstart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  bool anyEnabled = gApdsColorEnabled || gApdsProximityEnabled || gApdsGestureEnabled;
  if (anyEnabled) {
    return "[APDS] Error: Already running";
  }
  
  if (isInQueue(I2C_DEVICE_APDS)) {
    if (!ensureDebugBuffer()) return "[APDS] Already in queue";
    int pos = getQueuePosition(I2C_DEVICE_APDS);
    snprintf(getDebugBuffer(), 1024, "[APDS] Already in queue at position %d", pos);
    return getDebugBuffer();
  }

  if (!i2cPingAddress(I2C_ADDR_APDS, 100000, 50)) {
    return "[APDS] Not detected on I2C bus";
  }

  if (enqueueDeviceStart(I2C_DEVICE_APDS)) {
    sensorStatusBumpWith("openapds@enqueue");
    if (!ensureDebugBuffer()) return "[APDS] Sensor queued for open";
    int pos = getQueuePosition(I2C_DEVICE_APDS);
    snprintf(getDebugBuffer(), 1024, "[APDS] Sensor queued for open (position %d)", pos);
    return getDebugBuffer();
  }
  
  return "[APDS] Error: Failed to enqueue open (queue full)";
}

// Stop APDS sensor (all modes)
const char* cmd_apdsstop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  bool anyEnabled = gApdsColorEnabled || gApdsProximityEnabled || gApdsGestureEnabled;
  if (!anyEnabled) {
    return "[APDS] Error: Not running";
  }
  
  handleDeviceStopped(I2C_DEVICE_APDS);
  sensorStatusBumpWith("closeapds@CLI");
  return "[APDS] Sensor close requested; cleanup will complete asynchronously";
}

// Runtime mode control (once sensor is running)
const char* cmd_apdsmode(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gApdsConnected || gAPDS9960 == nullptr) {
    return "[APDS] Error: Sensor not initialized - use 'openapds' first";
  }
  
  // Parse: apdsmode <color|proximity|gesture> [on|off]
  CommandArgs a(argsInput);

  if (a.count() == 0) {
    if (!ensureDebugBuffer()) return "[APDS] Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024, "[APDS] Modes: color=%s proximity=%s gesture=%s",
             gApdsColorEnabled ? "on" : "off",
             gApdsProximityEnabled ? "on" : "off",
             gApdsGestureEnabled ? "on" : "off");
    return getDebugBuffer();
  }

  String mode = a.arg(0);
  mode.toLowerCase();
  bool enable = a.argBool(1, true);  // default to "on" if not specified
  
  if (mode == "color") {
    gAPDS9960->enableColor(enable);
    gApdsColorEnabled = enable;
    sensorStatusBumpWith(enable ? "apdsmode color on" : "apdsmode color off");
    return enable ? "[APDS] Color mode enabled" : "[APDS] Color mode disabled";
  } else if (mode == "proximity" || mode == "prox") {
    gAPDS9960->enableProximity(enable);
    gApdsProximityEnabled = enable;
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
    gApdsGestureEnabled = enable;
    sensorStatusBumpWith(enable ? "apdsmode gesture on" : "apdsmode gesture off");
    return enable ? "[APDS] Gesture mode enabled" : "[APDS] Gesture mode disabled";
  }
  
  return "[APDS] Error: Unknown mode - use 'color', 'proximity', or 'gesture'";
}

// ============================================================================
// APDS Sensor Initialization and Reading Functions
// ============================================================================

// Internal function called by queue processor
bool apdsStartInternal() {
  // Check memory before creating task
  if (!checkMemoryAvailable("apds", nullptr)) {
    ERROR_APDSF("Error: Insufficient memory for APDS sensor");
    return false;
  }

  // Create cache mutex if not already created
  if (!gAPDSCache.mutex) {
    gAPDSCache.mutex = xSemaphoreCreateMutex();
    if (!gAPDSCache.mutex) {
      ERROR_APDSF("Failed to create cache mutex");
      return false;
    }
    DEBUG_APDS_LIFECYCLEF("[APDS] Cache mutex created");
  }

  // Clean up any stale cache from previous run BEFORE starting
  if (gAPDSCache.mutex && xSemaphoreTake(gAPDSCache.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    gAPDSCache.apdsDataValid = false;
    gAPDSCache.apdsRed = 0;
    gAPDSCache.apdsGreen = 0;
    gAPDSCache.apdsBlue = 0;
    gAPDSCache.apdsClear = 0;
    gAPDSCache.apdsProximity = 0;
    gAPDSCache.apdsGesture = 0;
    xSemaphoreGive(gAPDSCache.mutex);
  }
  INFO_APDSF("Cleaned up stale cache from previous run");

  // Initialize APDS sensor synchronously
  if (!gApdsConnected || gAPDS9960 == nullptr) {
    if (!apdsInit()) {
      ERROR_APDSF("Error: Failed to initialize APDS9960 sensor");
      return false;
    }
  }

  // Enable color mode by default (user can change with apdsmode command)
  gAPDS9960->enableColor(true);
  gApdsColorEnabled = true;
  INFO_APDSF("Color mode enabled by default");

  // Create APDS task
  if (!createAPDSTask()) {
    ERROR_APDSF("Error: Failed to create APDS task");
    gApdsColorEnabled = false;
    return false;
  }

  sensorStatusBumpWith("APDS initialized");
  INFO_APDSF("Sensor started successfully (color mode active)");
  return true;
}

bool apdsInit() {
  if (gAPDS9960 != nullptr) {
    return true;
  }
  
  // Use i2cTransaction wrapper for safe mutex + clock management
  return i2cDeviceTransaction(I2C_ADDR_APDS, 100000, 500, [&]() -> bool {
    gAPDS9960 = new Adafruit_APDS9960();
    if (!gAPDS9960) return false;
    
    if (!gAPDS9960->begin()) {
      delete gAPDS9960;
      gAPDS9960 = nullptr;
      return false;
    }
    
    gApdsConnected = true;
    
    return true;
  });
}

void apdsColorPoll() {
  if (!gApdsConnected || gAPDS9960 == nullptr) {
    broadcastOutput("APDS9960 sensor not connected or initialized");
    return;
  }

  if (!gApdsColorEnabled) {
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

  BROADCAST_PRINTF("Red: %d, Green: %d, Blue: %d, Clear: %d", red, green, blue, clear);
}

void apdsProximityPoll() {
  if (!gApdsConnected || gAPDS9960 == nullptr) {
    broadcastOutput("APDS9960 sensor not connected or initialized");
    return;
  }

  if (!gApdsProximityEnabled) {
    broadcastOutput("Proximity sensing not enabled. Use 'apdsproximitystart' first.");
    return;
  }

  uint8_t proximity = gAPDS9960->readProximity();
  BROADCAST_PRINTF("Proximity: %d", proximity);
}

void apdsGesturePoll() {
  if (!gApdsConnected || gAPDS9960 == nullptr) {
    broadcastOutput("APDS9960 sensor not connected or initialized");
    return;
  }

  if (!gApdsGestureEnabled) {
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

const char* cmd_apdsautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) {
    return gSettings.apdsAutoStart ? "[APDS] Auto-start: enabled" : "[APDS] Auto-start: disabled";
  }
  arg.toLowerCase();
  if (arg == "on" || arg == "true" || arg == "1") {
    setSetting(gSettings.apdsAutoStart, true);
    return "[APDS] Auto-start enabled";
  } else if (arg == "off" || arg == "false" || arg == "0") {
    setSetting(gSettings.apdsAutoStart, false);
    return "[APDS] Auto-start disabled";
  }
  return "Usage: apdsautostart [on|off]";
}

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry apdsCommands[] = {
  // Primary commands (3-level voice: "sensor" -> "gesture" -> "open/close")
  { "openapds", "Start APDS9960 sensor.", false, cmd_apdsstart, nullptr, "sensor", "gesture", "open" },
  { "closeapds", "Stop APDS9960 sensor.", false, cmd_apdsstop, nullptr, "sensor", "gesture", "close" },
  { "apdsread", "Read APDS9960 sensor status and data.", false, cmd_apdsread },
  { "apdsmode", "Control APDS modes: apdsmode <color|proximity|gesture> [on|off].", false, cmd_apdsmode },
  
  // Read commands (per-mode)
  { "apdscolor", "Read APDS9960 color values.", false, cmd_apdscolor },
  { "apdsproximity", "Read APDS9960 proximity value.", false, cmd_apdsproximity },
  { "apdsgesture", "Read APDS9960 gesture.", false, cmd_apdsgesture },
  
  // Auto-start
  { "apdsautostart", "Enable/disable APDS auto-start after boot [on|off]", false, cmd_apdsautostart, "Usage: apdsautostart [on|off]" },
};

const size_t apdsCommandsCount = sizeof(apdsCommands) / sizeof(apdsCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

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
//   2. Acquire bus mutex via I2CDeviceManager to prevent race conditions during cleanup
//   3. Delete sensor object and invalidate cache
//   4. Release mutex and delete task
// ============================================================================

void apdsTask(void* parameter) {
  INFO_APDSF("Task started (handle=%p, stack=%u words)", 
                (void*)xTaskGetCurrentTaskHandle(), 
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  INFO_APDSF("[MODULAR] apdsTask() running from Sensor_APDS_APDS9960.cpp");
  unsigned long lastApdsRead = 0;
  unsigned long lastStackLog = 0;
  // Note: Failure tracking now handled by centralized I2CDevice health system
  // Use i2cShouldAutoDisable() instead of local counters

  while (true) {
    // CRITICAL: Check if all modes disabled for graceful shutdown
    bool anyEnabled = gApdsColorEnabled || gApdsProximityEnabled || gApdsGestureEnabled;
    if (!anyEnabled) {
      gApdsConnected = false;
      if (gAPDS9960 != nullptr) {
        delete gAPDS9960;
        gAPDS9960 = nullptr;
      }
      gAPDSCache.apdsDataValid = false;
      SENSOR_TASK_EXIT(APDS);
    }

    // Stack watermark tracking + safety bailout
    unsigned long nowMs = millis();
    if ((nowMs - lastStackLog) >= 10000) {
      lastStackLog = nowMs;
      if (checkTaskStackSafety("apds", APDS_STACK_WORDS, &gApdsColorEnabled)) {
        gApdsProximityEnabled = false;
        gApdsGestureEnabled = false;
        break;
      }
      if (anyEnabled && isDebugFlagSet(DEBUG_PERFORMANCE)) {
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
        DEBUG_PERFORMANCEF("[STACK] apds_task watermark=%u words", (unsigned)watermark);
      }
      if (anyEnabled && isDebugFlagSet(DEBUG_MEMORY)) {
        DEBUG_MEMORY_HEAPF("[HEAP] apds_task: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }
    
    if (anyEnabled && gApdsConnected && !gSensorPollingPaused) {
      unsigned long apdsPollMs = (gSettings.apdsDevicePollMs > 0) ? (unsigned long)gSettings.apdsDevicePollMs : 200;
      
      if ((nowMs - lastApdsRead) >= apdsPollMs) {
        uint16_t red = 0, green = 0, blue = 0, clear = 0;
        uint8_t proximity = 0;
        uint8_t gesture = 0;
        
        // APDS reads ~5ms at 100kHz; fail fast and retry next poll rather than blocking 1000ms
        bool result = i2cTaskWithTimeout(I2C_ADDR_APDS, 100000, 100, [&]() -> bool {
          if (gApdsColorEnabled && gAPDS9960->colorDataReady()) {
            gAPDS9960->getColorData(&red, &green, &blue, &clear);
          }
          if (gApdsProximityEnabled) {
            proximity = gAPDS9960->readProximity();
          }
          if (gApdsGestureEnabled) {
            gesture = gAPDS9960->readGesture();
          }
          return true;
        });

        if (result) {
          // Note: I2CDevice::recordSuccess() called automatically by transaction
          // which resets consecutiveErrors - no local counter needed
          
          if (gAPDSCache.mutex && xSemaphoreTake(gAPDSCache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            gAPDSCache.apdsRed = red;
            gAPDSCache.apdsGreen = green;
            gAPDSCache.apdsBlue = blue;
            gAPDSCache.apdsClear = clear;
            gAPDSCache.apdsProximity = proximity;
            gAPDSCache.apdsGesture = gesture;
            gAPDSCache.apdsLastUpdate = nowMs;
            gAPDSCache.apdsDataValid = true;
            xSemaphoreGive(gAPDSCache.mutex);
          }
        } else {
          // Note: I2CDevice::recordError() called automatically by transaction
          // Check centralized health tracking for auto-disable decision
          if (i2cShouldAutoDisable(I2C_ADDR_APDS)) {
            uint8_t errors = i2cGetConsecutiveErrors(I2C_ADDR_APDS);
            gApdsColorEnabled = false;
            gApdsProximityEnabled = false;
            gApdsGestureEnabled = false;
            gApdsConnected = false;
            sensorStatusBumpWith("apds@auto_disabled");
            DEBUG_APDS_LIFECYCLEF("APDS auto-disabled after %u consecutive I2C failures", errors);
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

// ============================================================================
// APDS Accessor Functions (for MQTT and other modules)
// ============================================================================

uint8_t apdsGetProximity() {
  if (!gApdsConnected || !gAPDSCache.apdsDataValid) return 0;
  return gAPDSCache.apdsProximity;
}

uint16_t apdsGetColorR() {
  if (!gApdsConnected || !gAPDSCache.apdsDataValid) return 0;
  return gAPDSCache.apdsRed;
}

uint16_t apdsGetColorG() {
  if (!gApdsConnected || !gAPDSCache.apdsDataValid) return 0;
  return gAPDSCache.apdsGreen;
}

uint16_t apdsGetColorB() {
  if (!gApdsConnected || !gAPDSCache.apdsDataValid) return 0;
  return gAPDSCache.apdsBlue;
}

uint16_t apdsGetColorC() {
  if (!gApdsConnected || !gAPDSCache.apdsDataValid) return 0;
  return gAPDSCache.apdsClear;
}

#endif // ENABLE_APDS_SENSOR
