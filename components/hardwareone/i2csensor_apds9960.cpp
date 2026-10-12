#include "System_BuildConfig.h"
#include "System_Events.h"  // systemEventPost — event register producer

#if ENABLE_APDS_SENSOR

#include <Arduino.h>
#include <freertos/semphr.h>
#include <new>
#include "Adafruit_APDS9960.h"

#include "i2csensor_apds9960.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_MemUtil.h"
#include "System_MemoryMonitor.h"
#include "System_Mutex.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_Utils.h"

// Debug macros (use centralized versions from debug_system.h)

// External dependencies provided by System_I2C.h:
// sensorStatusBumpWith, gSensorPollingPaused, drainDebugRing

// APDS sensor object (owned by this module)
static Adafruit_APDS9960* gAPDS9960 = nullptr;

// ============================================================================
// APDS/Peripheral Sensor Cache (owned by this module)
// ============================================================================
// The mutex is retained across stops; readers never own the live storage.
static APDSCache sApdsCache{};
static SemaphoreHandle_t sApdsCacheMutex = nullptr;

bool apdsCacheSnapshot(APDSCache& out, uint32_t timeoutMs) {
  out = {};
  SensorCacheGuard guard(sApdsCacheMutex, pdMS_TO_TICKS(timeoutMs), "apds.snapshot");
  if (!guard.held) return false;
  out = sApdsCache;
  return true;
}

size_t apdsCacheStorageBytes() {
  return sizeof(sApdsCache) + sizeof(sApdsCacheMutex);
}

static bool apdsInvalidateCache() {
  SensorCacheGuard guard(sApdsCacheMutex, pdMS_TO_TICKS(50), "apds.invalidate");
  if (!guard.held) return false;
  sApdsCache.apdsDataValid = false;
  sApdsCache.apdsColorValid = false;
  sApdsCache.apdsProximityValid = false;
  sApdsCache.apdsGestureValid = false;
  sApdsCache.apdsLastGesture = 0;
  sApdsCache.apdsLastGestureMs = 0;
  return true;
}

// Register writes from the command task. The cache lock also pins the
// driver's lifetime (apdsReleaseDriver deletes it only under that lock), and
// the bus transaction keeps the writes from interleaving with the polling
// task's transfers. False: lock or bus unavailable, or the driver is gone.
template <typename Write>
static bool apdsDeviceWrite(Write&& write) {
  SensorCacheGuard guard(sApdsCacheMutex, pdMS_TO_TICKS(100), "apds.modeWrite");
  if (!guard.held || !gAPDS9960) return false;
  bool written = false;
  i2cDeviceTransactionVoid((uint8_t)gSettings.apdsBus, I2C_ADDR_APDS, 100000, 500, [&]() {
    write(*gAPDS9960);
    written = true;
  });
  return written;
}

// Deletes the driver under the cache lock (see apdsDeviceWrite). A bus
// transaction cannot pin it: transactions skip a degraded device, so a
// shutdown after an auto-disable would never get to run its delete.
static bool apdsReleaseDriver(TickType_t wait) {
  SensorCacheGuard guard(sApdsCacheMutex, wait, "apds.release");
  if (!guard.held) return false;
  if (gAPDS9960) {
    ps_delete(gAPDS9960);
    gAPDS9960 = nullptr;
  }
  return true;
}

// APDS sensor state (definitions - matching pattern of thermal/tof/imu/gamepad sensors)
bool gApdsRunning = false;
bool gApdsColorRunning = false;
bool gApdsProximityRunning = false;
bool gApdsGestureRunning = false;
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
  { "apdsEnabled", SETTING_BOOL, &gSettings.apdsEnabled, 1, 0, nullptr, 0, 1, "Enabled", nullptr, false, nullptr, "apdsenabled" },
  { "apdsAutoStart", SETTING_BOOL, &gSettings.apdsAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr, false, nullptr, "apdsautostart" },
  { "apdsDevicePollMs", SETTING_INT, &gSettings.apdsDevicePollMs, 200, 0, nullptr, 50, 5000, "Poll Interval (ms)", nullptr, false, nullptr, "apdsdevicepollms" },
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
  "APDS9960 gesture, colour, and proximity sensor"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp


// ============================================================================
// APDS Sensor Command Handlers
// ============================================================================

// Shared data builder — feeds `apdsread json`, `sensors json`, and any other
// consumer. Mirrors the other sensors' *BuildDataJSON contract.
int apdsBuildDataJSON(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;

  // Read the cached RGBC/proximity values UNDER the cache mutex (the poll task
  // writes them un-atomically). `valid` = real data validity (apdsDataValid).
  // The color/prox/gesture mode flags are device-state, not measurement — they
  // live in the sensors-json discovery layer, not in the reading body.
  APDSCache reading;
  const bool available = apdsCacheSnapshot(reading, 50);
  if (!available) {
    int pos = sensorEnvelopeBegin(buf, bufSize, false, gApdsConnected, 0);
    if (pos == 0) return 0;
    int n = snprintf(buf + pos, bufSize - pos, ",\"r\":0,\"g\":0,\"b\":0,\"c\":0,\"proximity\":0}");
    if (n < 0 || (size_t)n >= bufSize - pos) return 0;
    return pos + n;
  }

  int pos = sensorEnvelopeBegin(buf, bufSize, reading.apdsDataValid, gApdsConnected, reading.apdsLastUpdate);
  if (pos == 0) return 0;
  int n = snprintf(buf + pos, bufSize - pos,
                   ",\"r\":%u,\"g\":%u,\"b\":%u,\"c\":%u,\"proximity\":%u}",
                   reading.apdsRed, reading.apdsGreen, reading.apdsBlue, reading.apdsClear,
                   reading.apdsProximity);
  if (n < 0 || (size_t)n >= bufSize - pos) return 0;
  return pos + n;
}

// Direction name for a decoded gesture code, or nullptr for an unknown code.
static const char* apdsGestureName(uint8_t gesture) {
  switch (gesture) {
    case APDS9960_UP: return "UP";
    case APDS9960_DOWN: return "DOWN";
    case APDS9960_LEFT: return "LEFT";
    case APDS9960_RIGHT: return "RIGHT";
    default: return nullptr;
  }
}

// Read commands share the polling task's snapshot. In particular, they never
// consume gesture FIFO data or wait for color conversion on the command task.
static const char* apdsReadSnapshot(APDSCache& reading) {
  if (!gApdsRunning || !gApdsConnected) {
    return "Error: [APDS] Not running. Use 'openapds' to start.";
  }
  if (!apdsCacheSnapshot(reading)) return "Error: [APDS] Could not read cache; retry shortly";
  return nullptr;
}

const char* cmd_apdscolor(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  APDSCache reading;
  if (const char* error = apdsReadSnapshot(reading)) return error;
  if (!gApdsColorRunning) return "Error: [APDS] Color mode is off. Use 'apdsmode color on'.";
  if (!reading.apdsDataValid || !reading.apdsColorValid) return "Error: [APDS] No cached color reading yet; retry shortly";
  if (!ensureDebugBuffer()) return "Error: [APDS] buffer unavailable";
  snprintf(getDebugBuffer(), 1024, "Red: %u, Green: %u, Blue: %u, Clear: %u (cached)",
           reading.apdsRed, reading.apdsGreen, reading.apdsBlue, reading.apdsClear);
  return getDebugBuffer();
}

const char* cmd_apdsproximity(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  APDSCache reading;
  if (const char* error = apdsReadSnapshot(reading)) return error;
  if (!gApdsProximityRunning) return "Error: [APDS] Proximity mode is off. Use 'apdsmode proximity on'.";
  if (!reading.apdsDataValid || !reading.apdsProximityValid) return "Error: [APDS] No cached proximity reading yet; retry shortly";
  if (!ensureDebugBuffer()) return "Error: [APDS] buffer unavailable";
  snprintf(getDebugBuffer(), 1024, "Proximity: %u (cached)", reading.apdsProximity);
  return getDebugBuffer();
}

const char* cmd_apdsgesture(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  APDSCache reading;
  if (const char* error = apdsReadSnapshot(reading)) return error;
  if (!gApdsGestureRunning) return "Error: [APDS] Gesture mode is off. Use 'apdsmode gesture on'.";
  if (!reading.apdsDataValid || !reading.apdsGestureValid) return "Error: [APDS] No cached gesture reading yet; retry shortly";
  // Report the latched last swipe: one poll sees only one FIFO batch, so the
  // latest sample alone almost never holds a completed gesture.
  if (reading.apdsLastGesture == 0) return "No gesture since start";
  const char* dir = apdsGestureName(reading.apdsLastGesture);
  if (!dir) return "Error: [APDS] Unknown cached gesture";
  if (!ensureDebugBuffer()) return "Error: [APDS] Debug buffer unavailable";
  const unsigned long ageMs = millis() - reading.apdsLastGestureMs;
  snprintf(getDebugBuffer(), 1024, "Gesture: %s (%lu.%lu s ago)", dir, ageMs / 1000, (ageMs % 1000) / 100);
  return getDebugBuffer();
}

const char* cmd_apdsread(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    if (!ensureDebugBuffer()) return SENSOR_JSON_NOBUF;
    int n = apdsBuildDataJSON(getDebugBuffer(), 1024);
    return (n > 0) ? getDebugBuffer() : SENSOR_JSON_UNAVAILABLE;
  }

  APDSCache reading;
  if (const char* error = apdsReadSnapshot(reading)) return error;
  if (!reading.apdsDataValid) return "Error: [APDS] No cached reading yet; retry shortly";
  if (!ensureDebugBuffer()) return "Error: [APDS] buffer unavailable";
  char* buf = getDebugBuffer();
  int remaining = 1024;
  int n = 0;

  n = snprintf(buf, remaining, "APDS9960 Status:\n");
  buf += n; remaining -= n;

  n = snprintf(buf, remaining, "  Color: %s  Proximity: %s  Gesture: %s\n",
               gApdsColorRunning ? "ON" : "OFF",
               gApdsProximityRunning ? "ON" : "OFF",
               gApdsGestureRunning ? "ON" : "OFF");
  buf += n; remaining -= n;

  if (gApdsColorRunning) {
    n = reading.apdsColorValid
        ? snprintf(buf, remaining, "  RGBC: R=%u G=%u B=%u C=%u\n",
                   reading.apdsRed, reading.apdsGreen, reading.apdsBlue, reading.apdsClear)
        : snprintf(buf, remaining, "  RGBC: waiting for sample\n");
    buf += n; remaining -= n;
  }
  if (gApdsProximityRunning) {
    n = reading.apdsProximityValid
        ? snprintf(buf, remaining, "  Proximity: %u\n", reading.apdsProximity)
        : snprintf(buf, remaining, "  Proximity: waiting for sample\n");
    buf += n; remaining -= n;
  }

  return getDebugBuffer();
}

// Unified APDS start command using queue system (consistent with other sensors)
const char* cmd_apdsstart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return cmd_sensorstart_queued(I2C_DEVICE_APDS, "APDS", "openapds@enqueue");
}

// Stop APDS sensor (all modes)
const char* cmd_apdsstop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!requestSensorStop(I2C_DEVICE_APDS)) return "Error: [APDS] Could not queue stop";
  return "[APDS] Stop requested; cleanup will complete asynchronously";
}

// Runtime mode control (once sensor is running)
const char* cmd_apdsmode(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gApdsRunning || !(gApdsColorRunning || gApdsProximityRunning || gApdsGestureRunning) ||
      !gApdsConnected || gAPDS9960 == nullptr) {
    return "Error: [APDS] Sensor not initialized - use 'openapds' first";
  }
  
  // Parse: apdsmode <color|proximity|gesture> [on|off]
  CommandArgs a(argsInput);

  if (a.count() == 0) {
    if (!ensureDebugBuffer()) return "Error: [APDS] Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024, "[APDS] Modes: color=%s proximity=%s gesture=%s",
             gApdsColorRunning ? "on" : "off",
             gApdsProximityRunning ? "on" : "off",
             gApdsGestureRunning ? "on" : "off");
    return getDebugBuffer();
  }

  String mode = a.arg(0);
  mode.toLowerCase();
  bool enable = a.argBool(1, true);  // default to "on" if not specified
  
  // Writes go through apdsDeviceWrite: one bus transaction, driver pinned.
  static const char* const kModeWriteFailed = "Error: [APDS] Sensor busy or stopping; mode unchanged";
  if (mode == "color") {
    if (!apdsDeviceWrite([&](Adafruit_APDS9960& d) { d.enableColor(enable); })) return kModeWriteFailed;
    gApdsColorRunning = enable;
    sensorStatusBumpWith(enable ? "apdsmode color on" : "apdsmode color off");
    return enable ? "[APDS] Color mode enabled" : "[APDS] Color mode disabled";
  } else if (mode == "proximity" || mode == "prox") {
    if (!apdsDeviceWrite([&](Adafruit_APDS9960& d) { d.enableProximity(enable); })) return kModeWriteFailed;
    gApdsProximityRunning = enable;
    sensorStatusBumpWith(enable ? "apdsmode proximity on" : "apdsmode proximity off");
    return enable ? "[APDS] Proximity mode enabled" : "[APDS] Proximity mode disabled";
  } else if (mode == "gesture") {
    const bool written = apdsDeviceWrite([&](Adafruit_APDS9960& d) {
      if (enable) {
        d.enableProximity(true);
        d.enableGesture(true);
      } else {
        d.enableGesture(false);
        d.enableProximity(false);
      }
    });
    if (!written) return kModeWriteFailed;
    gApdsGestureRunning = enable;
    sensorStatusBumpWith(enable ? "apdsmode gesture on" : "apdsmode gesture off");
    return enable ? "[APDS] Gesture mode enabled" : "[APDS] Gesture mode disabled";
  }
  
  return "Error: [APDS] Unknown mode - use 'color', 'proximity', or 'gesture'";
}

// ============================================================================
// APDS Sensor Initialization and Reading Functions
// ============================================================================

// Internal function called by queue processor
bool apdsStartInternal() {
  if (gApdsRunning && (gApdsColorRunning || gApdsProximityRunning || gApdsGestureRunning)) return true;
  // A stopped task owns its resources until cleanup publishes a null handle.
  if (gApdsTaskHandle != nullptr) return false;

  // Check memory before creating task
  if (!checkMemoryAvailable("apds", nullptr)) {
    ERROR_APDSF("Error: Insufficient memory for APDS sensor");
    return false;
  }

  // Create cache mutex if not already created
  if (!sApdsCacheMutex) {
    sApdsCacheMutex = xSemaphoreCreateMutex();
    if (!sApdsCacheMutex) {
      ERROR_APDSF("Failed to create cache mutex");
      return false;
    }
    DEBUG_APDS_LIFECYCLEF("[APDS] Cache mutex created");
  }

  // Clean up any stale cache from previous run BEFORE starting
  {
    SensorCacheGuard g(sApdsCacheMutex, pdMS_TO_TICKS(100), "apds.cleanStaleCache");
    if (!g.held) return false;
    sApdsCache.apdsDataValid = false;
    sApdsCache.apdsColorValid = false;
    sApdsCache.apdsProximityValid = false;
    sApdsCache.apdsGestureValid = false;
    sApdsCache.apdsRed = 0;
    sApdsCache.apdsGreen = 0;
    sApdsCache.apdsBlue = 0;
    sApdsCache.apdsClear = 0;
    sApdsCache.apdsProximity = 0;
    sApdsCache.apdsGesture = 0;
    sApdsCache.apdsLastGesture = 0;
    sApdsCache.apdsLastGestureMs = 0;
  }
  INFO_APDS_LIFECYCLEF("Cleaned up stale cache from previous run");

  // Initialize APDS sensor synchronously
  if (!gApdsConnected || gAPDS9960 == nullptr) {
    if (!apdsInit()) {
      ERROR_APDSF("Error: Failed to initialize APDS9960 sensor");
      return false;
    }
  }

  // Enable color mode by default (user can change with apdsmode command). The
  // write shares the bus with every other device, so it runs as a transaction.
  bool colorEnabled = false;
  i2cDeviceTransactionVoid((uint8_t)gSettings.apdsBus, I2C_ADDR_APDS, 100000, 500, [&]() {
    gAPDS9960->enableColor(true);
    colorEnabled = true;
  });
  if (!colorEnabled) {
    ERROR_APDSF("Error: Could not enable APDS color mode (bus unavailable)");
    gApdsConnected = false;
    // No mode flag is set yet, so no command can hold the driver.
    ps_delete(gAPDS9960);
    gAPDS9960 = nullptr;
    return false;
  }
  gApdsRunning = true;
  gApdsColorRunning = true;
  INFO_APDS_LIFECYCLEF("Color mode enabled by default");

  // Create APDS task
  if (!createAPDSTask()) {
    ERROR_APDSF("Error: Failed to create APDS task");
    gApdsColorRunning = false;
    gApdsProximityRunning = false;
    gApdsGestureRunning = false;
    gApdsRunning = false;
    gApdsConnected = false;
    i2cDeviceTransactionVoid((uint8_t)gSettings.apdsBus, I2C_ADDR_APDS, 100000, 500, [&]() {
      gAPDS9960->enableColor(false);
    });
    // A mode write that passed its checks before the flags dropped holds the
    // cache lock; delete under it. Without the lock the worker still owns it.
    if (!apdsReleaseDriver(pdMS_TO_TICKS(600))) {
      ps_delete(gAPDS9960);
      gAPDS9960 = nullptr;
    }
    return false;
  }

  sensorStatusBumpWith("APDS initialized");
  INFO_APDS_LIFECYCLEF("Sensor started successfully (color mode active)");
  return true;
}

bool apdsInit() {
  if (gAPDS9960 != nullptr) {
    return true;
  }

  const uint8_t apdsBus = (uint8_t)gSettings.apdsBus;
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  TwoWire* apdsWire = mgr ? mgr->getWire(apdsBus) : nullptr;
  if (!apdsWire) return false;
  
  // Use i2cTransaction wrapper for safe mutex + clock management
  return i2cDeviceTransaction(apdsBus, I2C_ADDR_APDS, 100000, 500, [&]() -> bool {
    // Only the task-context wrapper moves; the library owns its I2C transport.
    void* apdsObjBuf = ps_alloc(sizeof(Adafruit_APDS9960), AllocPref::PreferPSRAM, "apds.obj");
    if (!apdsObjBuf) return false;
    gAPDS9960 = new (apdsObjBuf) Adafruit_APDS9960();
    
    if (!gAPDS9960->begin(10, APDS9960_AGAIN_4X, APDS9960_ADDRESS, apdsWire)) {
      ps_delete(gAPDS9960);
      gAPDS9960 = nullptr;
      return false;
    }
    
    gApdsConnected = true;
    
    return true;
  });
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
  return "Error: invalid arguments — Usage: apdsautostart [on|off]";
}

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
// Bus routing for this device. Lives here rather than in System_I2C.cpp so it
// compiles out with the driver — see i2cSetDeviceBusAndReport in System_I2C.h.
static const char* cmd_apdsbus(const String& a) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return i2cSetDeviceBusAndReport(gSettings.apdsBus, a, "apdsBus");
}

const CommandEntry apdsCommands[] = {
  // Primary commands (3-level voice: "sensor" -> "gesture" -> "open/close")
  { "openapds", "Start APDS9960 sensor.", false, cmd_apdsstart },
  { "closeapds", "Stop APDS9960 sensor.", false, cmd_apdsstop },
  { "apdsread", "Read APDS9960 sensor status and data. (add 'json' for JSON output)", false, cmd_apdsread },
  { "apdsmode", "Control APDS modes: apdsmode <color|proximity|gesture> [on|off].", false, cmd_apdsmode, "Usage: apdsmode <color|proximity|gesture> [<on|off>]" },
  
  // Read commands (per-mode)
  { "apdscolor", "Show latest cached APDS9960 color values.", false, cmd_apdscolor },
  { "apdsproximity", "Show latest cached APDS9960 proximity value.", false, cmd_apdsproximity },
  { "apdsgesture", "Show latest sampled APDS9960 gesture (does not consume events).", false, cmd_apdsgesture },
  
  // Auto-start
  { "apdsautostart", "Enable/disable APDS auto-start after boot [on|off]", false, cmd_apdsautostart, "Usage: apdsautostart [on|off]" },
  { "apdsbus",     "Route APDS9960 gesture to bus: <0|1> (reboot required)", true, cmd_apdsbus, "Usage: apdsBus <0|1>" },
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
// Stack: 3072 BYTES (3 KB) | Priority: 1 | Core: Any
// Lifecycle: Created by cmd_apdsstart, deleted when all modes disabled
// Polling: apdsDevicePollMs (default 200 ms); gesture FIFO at least every 100 ms
// I2C Clock: 100kHz
//
// Cleanup Strategy:
//   1. Check if all modes (color/proximity/gesture) are disabled
//   2. Invalidate the cache, then delete the sensor object under the cache
//      lock that mode writes hold across their bus transaction
//   3. Delete task
// ============================================================================

// Proximity presence latch. Hysteresis (>200 arrive / <100 leave) plus
// file-scope reset on task start keeps brief dropouts from flapping edges.
static bool gApdsPresent = false;

// One polling step, run by apdsTask while a mode is on. Colour and proximity
// follow the user's interval; gesture mode drains the FIFO at least every
// 100 ms, because the vendored decoder pairs the two halves of a swipe only
// within 300 ms of motion and apdsDevicePollMs allows up to 5000. A gesture
// read is at most three short transfers. Returns false after an auto-disable,
// so the caller loops back to the clean shutdown.
static bool apdsPollDevice(unsigned long nowMs, unsigned long pollMs,
                           unsigned long& lastSampleMs, unsigned long& lastGestureMs) {
  const unsigned long gesturePollMs = pollMs < 100 ? pollMs : 100;
  const bool sampleDue = (nowMs - lastSampleMs) >= pollMs;
  const bool gestureDue = gApdsGestureRunning && (nowMs - lastGestureMs) >= gesturePollMs;
  if (!sampleDue && !gestureDue) return true;

  const uint8_t apdsBus = (uint8_t)gSettings.apdsBus;
  uint16_t red = 0, green = 0, blue = 0, clear = 0;
  uint8_t proximity = 0;
  uint8_t gesture = 0;
  bool colorRead = false, proximityRead = false, gestureRead = false;

  // Wait at most 100ms for bus admission. Gesture sampling consumes one
  // available FIFO batch; it never waits for the next half of a swipe.
  const bool result = i2cTaskWithTimeout(apdsBus, I2C_ADDR_APDS, 100000, 100, [&]() -> bool {
    if (sampleDue && gApdsColorRunning && gAPDS9960->colorDataReady()) {
      gAPDS9960->getColorData(&red, &green, &blue, &clear);
      colorRead = true;
    }
    if (sampleDue && gApdsProximityRunning) {
      proximity = gAPDS9960->readProximity();
      proximityRead = true;
    }
    if (gestureDue && gApdsGestureRunning) {
      gesture = gAPDS9960->readGesture();
      gestureRead = true;
    }
    return true;
  });

  if (result) {
    // Note: I2CDevice::recordSuccess() called automatically by transaction
    // which resets consecutiveErrors - no local counter needed
    {
      SensorCacheGuard g(sApdsCacheMutex, pdMS_TO_TICKS(50), "apds.pollWrite");
      if (g.held) {
        // A mode without a new sample keeps its last one (colour "not ready"
        // is no new sample); a mode that is off reads as zero and invalid.
        if (colorRead) {
          sApdsCache.apdsRed = red;
          sApdsCache.apdsGreen = green;
          sApdsCache.apdsBlue = blue;
          sApdsCache.apdsClear = clear;
          sApdsCache.apdsColorValid = true;
        } else if (!gApdsColorRunning) {
          sApdsCache.apdsRed = sApdsCache.apdsGreen = sApdsCache.apdsBlue = sApdsCache.apdsClear = 0;
          sApdsCache.apdsColorValid = false;
        }
        if (proximityRead) {
          sApdsCache.apdsProximity = proximity;
          sApdsCache.apdsProximityValid = true;
        } else if (!gApdsProximityRunning) {
          sApdsCache.apdsProximity = 0;
          sApdsCache.apdsProximityValid = false;
        }
        if (gestureRead) {
          sApdsCache.apdsGesture = gesture;
          sApdsCache.apdsGestureValid = true;
          if (gesture != 0) {  // Latched for 'apdsgesture'.
            sApdsCache.apdsLastGesture = gesture;
            sApdsCache.apdsLastGestureMs = nowMs;
          }
        } else if (!gApdsGestureRunning) {
          sApdsCache.apdsGesture = 0;
          sApdsCache.apdsGestureValid = false;
        }
        sApdsCache.apdsLastUpdate = nowMs;
        sApdsCache.apdsDataValid = sApdsCache.apdsColorValid || sApdsCache.apdsProximityValid ||
                                   sApdsCache.apdsGestureValid;
      }
    }
    // Bus event per detected swipe. The driver returns nonzero only on
    // the poll where a gesture completed, so post-on-read is edge-safe.
    if (gestureRead && gesture != 0) {
      const char* dir = apdsGestureName(gesture);
      systemEventPost(SYSEVT_GESTURE, dir ? dir : "?");
    }
    // Proximity presence with hysteresis: arrive above 200, leave below
    // 100 (of 255). Edge-latched so we post once per crossing; only a pass
    // that sampled proximity can move it.
    if (proximityRead) {
      if (!gApdsPresent && proximity > 200) {
        gApdsPresent = true;
        systemEventPost(SYSEVT_PRESENCE_DETECTED, "proximity");
      } else if (gApdsPresent && proximity < 100) {
        gApdsPresent = false;
        systemEventPost(SYSEVT_PRESENCE_CLEARED, "proximity");
      }
    }
  } else if (i2cShouldAutoDisable(I2C_ADDR_APDS, apdsBus)) {
    // Note: I2CDevice::recordError() called automatically by transaction;
    // the centralized health tracking decides on auto-disable.
    uint8_t errors = i2cGetConsecutiveErrors(I2C_ADDR_APDS, apdsBus);
    gApdsColorRunning = false;
    gApdsProximityRunning = false;
    gApdsGestureRunning = false;
    gApdsConnected = false;
    sensorStatusBumpWith("apds@auto_disabled");
    DEBUG_APDS_LIFECYCLEF("APDS auto-disabled after %u consecutive I2C failures", errors);
    logSystemEvent("SENSOR", "APDS auto-disabled after %u consecutive I2C failures", errors);
    { char det[24]; snprintf(det, sizeof(det), "%u I2C errors", errors);
      systemEventPost(SYSEVT_SENSOR_FAULT, "APDS", det); }
    return false;  // Timestamps stay; the shutdown path runs next.
  }
  if (sampleDue) lastSampleMs = nowMs;
  if (gestureDue) lastGestureMs = nowMs;
  return true;
}

void apdsTask(void* parameter) {
  gApdsPresent = false;
  INFO_APDS_LIFECYCLEF("Task started (handle=%p, stack=%u words)",
                (void*)xTaskGetCurrentTaskHandle(),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  INFO_APDS_LIFECYCLEF("[MODULAR] apdsTask() running from i2csensor_apds9960.cpp");
  unsigned long lastApdsRead = 0;
  unsigned long lastGestureRead = 0;
  unsigned long lastStackLog = 0;
  // Note: Failure tracking now handled by centralized I2CDevice health system
  // Use i2cShouldAutoDisable() instead of local counters

  while (true) {
    // CRITICAL: Check if all modes disabled for graceful shutdown
    bool anyEnabled = gApdsRunning && (gApdsColorRunning || gApdsProximityRunning || gApdsGestureRunning);
    if (!anyEnabled) {
      gApdsRunning = false;
      gApdsColorRunning = false;
      gApdsProximityRunning = false;
      gApdsGestureRunning = false;
      gApdsConnected = false;
      if (!apdsInvalidateCache()) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      // Mode writes hold the cache lock across their bus transaction, so the
      // driver is deleted only under that lock.
      if (!apdsReleaseDriver(pdMS_TO_TICKS(50))) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      SENSOR_TASK_EXIT(APDS, gApdsTaskHandle);
    }

    // Stack watermark tracking + safety bailout
    unsigned long nowMs = millis();
    if ((nowMs - lastStackLog) >= 10000) {
      lastStackLog = nowMs;
      // 'continue' (not 'break') so the top-of-loop shutdown runs the clean
      // SENSOR_TASK_EXIT (vTaskDelete) path instead of returning from the task
      // function with a near-overflowed stack (IllegalInstruction panic).
      if (checkTaskStackSafety("apds", APDS_STACK_WORDS, &gApdsColorRunning)) {
        gApdsProximityRunning = false;
        gApdsGestureRunning = false;
        continue;
      }
      if (anyEnabled && isDebugFlagSet(DEBUG_PERFORMANCE)) {
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
        DEBUG_PERFORMANCEF("[STACK] apds_task watermark=%u words", (unsigned)watermark);
      }
      if (anyEnabled && isDebugFlagSet(DEBUG_MEMORY)) {
        DEBUG_MEMORY_HEAPF("[HEAP] apds_task: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }
    
    const uint8_t apdsBus = (uint8_t)gSettings.apdsBus;
    if (anyEnabled && gApdsConnected && !pollPaused(apdsBus)) {
      const unsigned long apdsPollMs = (gSettings.apdsDevicePollMs > 0) ? (unsigned long)gSettings.apdsDevicePollMs : 200;
      // 'continue' (not 'break') after an auto-disable — see the same note on
      // the stack-safety bailout above. `break` here left the while(true) loop
      // and RETURNED from the task entry point, which FreeRTOS turns into a
      // jump to address 0 (InstrFetchProhibited panic + reboot) rather than a
      // task exit. The poll clears all three running flags, so the
      // top-of-loop check now runs the clean SENSOR_TASK_EXIT.
      if (!apdsPollDevice(nowMs, apdsPollMs, lastApdsRead, lastGestureRead)) continue;
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));
    drainDebugRing();
  }
}

// ============================================================================
// APDS OLED Mode Registration
// ============================================================================

#if ENABLE_OLED_DISPLAY
#include "i2csensor_apds9960_oled.h"
#endif // ENABLE_OLED_DISPLAY

// ============================================================================
// APDS Accessor Functions (for MQTT and other modules)
// ============================================================================

uint8_t apdsGetProximity() {
  APDSCache reading;
  if (!gApdsConnected || !apdsCacheSnapshot(reading) || !reading.apdsDataValid) return 0;
  return reading.apdsProximity;
}

uint16_t apdsGetColorR() {
  APDSCache reading;
  if (!gApdsConnected || !apdsCacheSnapshot(reading) || !reading.apdsDataValid) return 0;
  return reading.apdsRed;
}

uint16_t apdsGetColorG() {
  APDSCache reading;
  if (!gApdsConnected || !apdsCacheSnapshot(reading) || !reading.apdsDataValid) return 0;
  return reading.apdsGreen;
}

uint16_t apdsGetColorB() {
  APDSCache reading;
  if (!gApdsConnected || !apdsCacheSnapshot(reading) || !reading.apdsDataValid) return 0;
  return reading.apdsBlue;
}

uint16_t apdsGetColorC() {
  APDSCache reading;
  if (!gApdsConnected || !apdsCacheSnapshot(reading) || !reading.apdsDataValid) return 0;
  return reading.apdsClear;
}

#endif // ENABLE_APDS_SENSOR
