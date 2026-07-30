#include "System_BuildConfig.h"
#include "System_Events.h"  // systemEventPost — event register producer

#if ENABLE_PRESENCE_SENSOR

#include <Arduino.h>
#include <Wire.h>

#include "i2csensor_sths34pf80.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_ESPNow.h"
#include "System_ESPNow_Sensors.h"
#include "System_I2C.h"
#include "System_MemoryMonitor.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_Utils.h"

// External dependencies provided by System_I2C.h:
// sensorStatusBumpWith, gSensorPollingPaused, drainDebugRing

// ============================================================================
// STHS34PF80 Register Definitions
// ============================================================================

// Device identification
#define STHS34PF80_WHO_AM_I       0x0F
#define STHS34PF80_WHO_AM_I_VALUE 0xD3

// Control registers
#define STHS34PF80_CTRL1          0x20
#define STHS34PF80_CTRL2          0x21
#define STHS34PF80_CTRL3          0x22

// Status register
#define STHS34PF80_STATUS         0x23

// Output data registers
#define STHS34PF80_TOBJECT_L      0x26
#define STHS34PF80_TOBJECT_H      0x27
#define STHS34PF80_TAMBIENT_L     0x28
#define STHS34PF80_TAMBIENT_H     0x29
#define STHS34PF80_TOBJ_COMP_L    0x38
#define STHS34PF80_TOBJ_COMP_H    0x39
#define STHS34PF80_TPRESENCE_L    0x3A
#define STHS34PF80_TPRESENCE_H    0x3B
#define STHS34PF80_TMOTION_L      0x3C
#define STHS34PF80_TMOTION_H      0x3D
#define STHS34PF80_TAMB_SHOCK_L   0x3E
#define STHS34PF80_TAMB_SHOCK_H   0x3F

// Function status register
#define STHS34PF80_FUNC_STATUS    0x25

// ODR values for CTRL1
#define STHS34PF80_ODR_OFF        0x00
#define STHS34PF80_ODR_0_25HZ     0x01
#define STHS34PF80_ODR_0_5HZ      0x02
#define STHS34PF80_ODR_1HZ        0x03
#define STHS34PF80_ODR_2HZ        0x04
#define STHS34PF80_ODR_4HZ        0x05
#define STHS34PF80_ODR_8HZ        0x06
#define STHS34PF80_ODR_15HZ       0x07
#define STHS34PF80_ODR_30HZ       0x08

// ============================================================================
// Presence Sensor Cache (owned by this module)
// ============================================================================
PresenceCache gPresenceCache;

// Presence sensor state
bool gPresenceRunning = false;
bool gPresenceConnected = false;
unsigned long gPresenceLastStopTime = 0;
TaskHandle_t gPresenceTaskHandle = nullptr;

// Task creation now handled by createPresenceTask() in System_TaskUtils.cpp

// ============================================================================
// STHS34PF80 Modular Settings Registration
// ============================================================================

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry presenceSettingEntries[] = {
  { "presenceEnabled", SETTING_BOOL, &gSettings.presenceEnabled, 1, 0, nullptr, 0, 1, "Enabled", nullptr, false, nullptr, "presenceenabled" },
  { "presenceAutoStart", SETTING_BOOL, &gSettings.presenceAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr, false, nullptr, "presenceautostart" },
  { "presenceDevicePollMs", SETTING_INT, &gSettings.presenceDevicePollMs, 100, 0, nullptr, 50, 5000, "Poll Interval (ms)", nullptr, false, nullptr, "presencedevicepollms" },
};

static bool isPresenceConnected() {
  return gPresenceConnected;
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule presenceSettingsModule = {
  "presence",
  "hardware.sensors.presence",
  presenceSettingEntries,
  sizeof(presenceSettingEntries) / sizeof(presenceSettingEntries[0]),
  isPresenceConnected,
  "STHS34PF80 IR presence and motion sensor"
};

// ============================================================================
// Low-level I2C Helper Functions
// ============================================================================
// Resolves the STHS34PF80's bus + TwoWire* from settings (gSettings.presenceBus).
// Returns false (outputs unchanged) when the bus isn't initialized — every
// register helper below fails closed in that case rather than silently
// targeting Wire1 with the wrong bus mutex. Same pattern as the OLED + RTC
// migrations.
static bool presenceResolveBus(uint8_t* outBus, TwoWire** outWire) {
  const uint8_t bus = (uint8_t)gSettings.presenceBus;
  TwoWire* w = i2c() ? i2c()->getWire(bus) : nullptr;
  if (!w) return false;
  *outBus = bus;
  *outWire = w;
  return true;
}

static bool writeRegister(uint8_t reg, uint8_t value) {
  uint8_t bus; TwoWire* w;
  if (!presenceResolveBus(&bus, &w)) return false;
  return i2cDeviceTransaction(bus, I2C_ADDR_PRESENCE, 100000, 200, [&]() -> bool {
    w->beginTransmission(I2C_ADDR_PRESENCE);
    w->write(reg);
    w->write(value);
    return (w->endTransmission() == 0);
  });
}

static bool readRegister(uint8_t reg, uint8_t* value) {
  uint8_t bus; TwoWire* w;
  if (!presenceResolveBus(&bus, &w)) return false;
  return i2cDeviceTransaction(bus, I2C_ADDR_PRESENCE, 100000, 200, [&]() -> bool {
    w->beginTransmission(I2C_ADDR_PRESENCE);
    w->write(reg);
    if (w->endTransmission(false) != 0) return false;

    if (w->requestFrom(I2C_ADDR_PRESENCE, (uint8_t)1) != 1) return false;
    *value = w->read();
    return true;
  });
}

static bool readRegisters(uint8_t reg, uint8_t* buffer, uint8_t len) {
  uint8_t bus; TwoWire* w;
  if (!presenceResolveBus(&bus, &w)) return false;
  return i2cDeviceTransaction(bus, I2C_ADDR_PRESENCE, 100000, 200, [&]() -> bool {
    w->beginTransmission(I2C_ADDR_PRESENCE);
    w->write(reg);
    if (w->endTransmission(false) != 0) return false;

    if (w->requestFrom(I2C_ADDR_PRESENCE, len) != len) return false;
    for (uint8_t i = 0; i < len; i++) {
      buffer[i] = w->read();
    }
    return true;
  });
}

static int16_t readInt16(uint8_t regL) {
  uint8_t buf[2];
  if (!readRegisters(regL, buf, 2)) return 0;
  return (int16_t)(buf[0] | (buf[1] << 8));
}

// ============================================================================
// Presence Sensor Command Handlers
// ============================================================================

const char* cmd_presencestart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (gPresenceRunning) {
    return "Error: [PRESENCE] Already running";
  }
  
  if (isInQueue(I2C_DEVICE_PRESENCE)) {
    if (!ensureDebugBuffer()) return "[PRESENCE] Already in queue";
    int pos = getQueuePosition(I2C_DEVICE_PRESENCE);
    snprintf(getDebugBuffer(), 1024, "[PRESENCE] Already in queue at position %d", pos);
    return getDebugBuffer();
  }

  if (!i2cPingAddress(I2C_ADDR_PRESENCE, 100000, 50, (uint8_t)gSettings.presenceBus)) {
    return "Error: [Presence] Not detected on I2C bus";
  }

  if (enqueueDeviceStart(I2C_DEVICE_PRESENCE)) {
    sensorStatusBumpWith("openpresence@enqueue");
    cliHint("the sensor opens in the background - read it with 'presenceread' once it is up");
    if (!ensureDebugBuffer()) return "[PRESENCE] Sensor queued for open";
    int pos = getQueuePosition(I2C_DEVICE_PRESENCE);
    snprintf(getDebugBuffer(), 1024, "[PRESENCE] Sensor queued for open (position %d)", pos);
    return getDebugBuffer();
  }
  
  return "Error: [PRESENCE] Failed to enqueue open (queue full)";
}

const char* cmd_presencestop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gPresenceRunning) {
    return "Error: [PRESENCE] Not running";
  }
  
  handleDeviceStopped(I2C_DEVICE_PRESENCE);
  sensorStatusBumpWith("closepresence@CLI");
  return "[PRESENCE] Sensor close requested; cleanup will complete asynchronously";
}

const char* cmd_presenceread(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    if (!ensureDebugBuffer()) return SENSOR_JSON_NOBUF;
    int n = presenceBuildDataJSON(getDebugBuffer(), 1024);  // shared builder (also feeds sensors json)
    return (n > 0) ? getDebugBuffer() : SENSOR_JSON_UNAVAILABLE;
  }

  if (!gPresenceConnected || !gPresenceRunning) {
    return "Error: [PRESENCE] Sensor not running - use 'openpresence' first";
  }
  
  if (!ensureDebugBuffer()) return "Error: [PRESENCE] Debug buffer unavailable";
  
  {
    SensorCacheGuard g(gPresenceCache.mutex, pdMS_TO_TICKS(100), "presence.cmdRead");
    if (g.held) {
      snprintf(getDebugBuffer(), 1024,
        "[PRESENCE] Ambient: %.2f°C | Presence: %d %s | Motion: %d %s | TShock: %d %s",
        gPresenceCache.ambientTemp,
        gPresenceCache.presenceValue,
        gPresenceCache.presenceDetected ? "[DETECTED]" : "",
        gPresenceCache.motionValue,
        gPresenceCache.motionDetected ? "[DETECTED]" : "",
        gPresenceCache.tempShockValue,
        gPresenceCache.tempShockDetected ? "[DETECTED]" : "");
      return getDebugBuffer();
    }
  }

  return "Error: [PRESENCE] Could not read cache";
}

const char* cmd_presencestatus(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    if (!ensureDebugBuffer()) return SENSOR_JSON_NOBUF;
    int n = presenceBuildDataJSON(getDebugBuffer(), 1024);
    return (n > 0) ? getDebugBuffer() : SENSOR_JSON_UNAVAILABLE;
  }

  if (!ensureDebugBuffer()) return "Error: [PRESENCE] Debug buffer unavailable";
  
  snprintf(getDebugBuffer(), 1024,
    "[PRESENCE] Status: connected=%d enabled=%d taskHandle=%p dataValid=%d",
    gPresenceConnected, gPresenceRunning, (void*)gPresenceTaskHandle,
    gPresenceCache.dataValid);
  return getDebugBuffer();
}

// ============================================================================
// Presence Sensor Initialization and Reading Functions
// ============================================================================

bool presenceStartInternal() {
  // Check memory before creating task
  if (!checkMemoryAvailable("presence", nullptr)) {
    ERROR_PRESENCEF("Error: Insufficient memory for presence sensor");
    return false;
  }

  // Create cache mutex if not already created
  if (!gPresenceCache.mutex) {
    gPresenceCache.mutex = xSemaphoreCreateMutex();
    if (!gPresenceCache.mutex) {
      ERROR_PRESENCEF("Error: Failed to create cache mutex");
      return false;
    }
    DEBUG_PRESENCE_LIFECYCLEF("[PRESENCE] Cache mutex created");
  }

  // Clean up stale cache
  {
    SensorCacheGuard g(gPresenceCache.mutex, pdMS_TO_TICKS(100), "presence.cleanStaleCache");
    if (g.held) {
      gPresenceCache.dataValid = false;
      gPresenceCache.ambientTemp = 0.0f;
      gPresenceCache.objectTemp = 0;
      gPresenceCache.compObjectTemp = 0.0f;
      gPresenceCache.presenceValue = 0;
      gPresenceCache.motionValue = 0;
      gPresenceCache.tempShockValue = 0;
      gPresenceCache.presenceDetected = false;
      gPresenceCache.motionDetected = false;
      gPresenceCache.tempShockDetected = false;
    }
  }
  INFO_PRESENCE_LIFECYCLEF("Cleaned up stale cache");

  // Initialize sensor synchronously
  if (!gPresenceConnected) {
    if (!presenceInit()) {
      ERROR_PRESENCEF("Error: Failed to initialize STHS34PF80 sensor");
      return false;
    }
  }

  // Set enabled BEFORE creating task - task checks gPresenceRunning on first iteration
  // and will immediately delete itself if it's still false
  gPresenceRunning = true;

  // Create task
  if (!createPresenceTask()) {
    gPresenceRunning = false;
    ERROR_PRESENCEF("Error: Failed to create presence task");
    return false;
  }
  sensorStatusBumpWith("PRESENCE initialized");
  INFO_PRESENCE_LIFECYCLEF("Sensor started successfully");
  return true;
}

bool presenceInit() {
  if (gPresenceConnected) {
    return true;
  }
  
  return i2cTaskWithTimeout(I2C_ADDR_PRESENCE, 100000, 500, [&]() -> bool {
    // Check WHO_AM_I
    uint8_t whoami;
    if (!readRegister(STHS34PF80_WHO_AM_I, &whoami)) {
      ERROR_PRESENCEF("Failed to read WHO_AM_I");
      return false;
    }
    
    if (whoami != STHS34PF80_WHO_AM_I_VALUE) {
      ERROR_PRESENCEF("Wrong WHO_AM_I: 0x%02X (expected 0x%02X)", whoami, STHS34PF80_WHO_AM_I_VALUE);
      return false;
    }
    
    INFO_PRESENCE_LIFECYCLEF("WHO_AM_I verified: 0x%02X", whoami);
    
    // Configure CTRL1: Set ODR to 8Hz, BDU enabled
    // Bits [6:4] = ODR, Bit 3 = BDU
    uint8_t ctrl1 = (STHS34PF80_ODR_8HZ << 4) | 0x08;
    if (!writeRegister(STHS34PF80_CTRL1, ctrl1)) {
      ERROR_PRESENCEF("Failed to configure CTRL1");
      return false;
    }
    
    // Configure CTRL2: Enable presence, motion, and ambient shock detection
    // Defaults are usually fine, but ensure FUNC_CFG_ACCESS is 0
    if (!writeRegister(STHS34PF80_CTRL2, 0x00)) {
      ERROR_PRESENCEF("Failed to configure CTRL2");
      return false;
    }
    
    gPresenceConnected = true;
    
    // Register for I2C health tracking (use manager directly with correct clock/timeout)
    I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
    if (mgr) mgr->registerDevice(I2C_ADDR_PRESENCE, "STHS34PF80", 100000, 200);
    return true;
  });
}

// Event-latch state for presence/motion detected/cleared. File-scope so a
// sensor restart resets it — function-local statics survived auto-disable and
// replayed a stale "cleared" edge with a fresh timestamp at the next start.
static bool sPresenceEvtPresent = false;
static uint8_t sPresenceEvtClearPolls = 0;
static bool gMotionActive = false;

bool presencePoll() {
  if (!gPresenceConnected) return false;
  
  // Read status first
  uint8_t status;
  if (!readRegister(STHS34PF80_STATUS, &status)) {
    return false;
  }
  
  // Check if data is ready (bit 2 = DRDY)
  if (!(status & 0x04)) {
    return true;  // No new data, but I2C transaction succeeded
  }
  
  // Read function status for detection flags
  uint8_t funcStatus;
  if (!readRegister(STHS34PF80_FUNC_STATUS, &funcStatus)) {
    return false;
  }
  
  // Read ambient temperature (LSB = 0.0625°C)
  int16_t ambientRaw = readInt16(STHS34PF80_TAMBIENT_L);
  float ambient = ambientRaw / 100.0f;
  
  // Read object temperature (raw)
  int16_t objectRaw = readInt16(STHS34PF80_TOBJECT_L);
  
  // Read compensated object temperature
  int16_t compObjRaw = readInt16(STHS34PF80_TOBJ_COMP_L);
  float compObj = compObjRaw / 100.0f;
  
  // Read presence value
  int16_t presenceVal = readInt16(STHS34PF80_TPRESENCE_L);
  
  // Read motion value
  int16_t motionVal = readInt16(STHS34PF80_TMOTION_L);
  
  // Read temperature shock value
  int16_t tempShockVal = readInt16(STHS34PF80_TAMB_SHOCK_L);
  
  // Extract detection flags from FUNC_STATUS
  // Bit 0 = PRES_FLAG, Bit 1 = MOT_FLAG, Bit 2 = TAMB_SHOCK_FLAG
  bool presence = (funcStatus & 0x04) != 0;
  bool motion = (funcStatus & 0x02) != 0;
  bool tempShock = (funcStatus & 0x01) != 0;
  
  // Update cache
  {
    SensorCacheGuard g(gPresenceCache.mutex, pdMS_TO_TICKS(50), "presence.pollWrite");
    if (g.held) {
      gPresenceCache.ambientTemp = ambient;
      gPresenceCache.objectTemp = objectRaw;
      gPresenceCache.compObjectTemp = compObj;
      gPresenceCache.presenceValue = presenceVal;
      gPresenceCache.motionValue = motionVal;
      gPresenceCache.tempShockValue = tempShockVal;
      gPresenceCache.presenceDetected = presence;
      gPresenceCache.motionDetected = motion;
      gPresenceCache.tempShockDetected = tempShock;
      gPresenceCache.lastUpdate = millis();
      gPresenceCache.dataValid = true;
    }
  }

  // Bus events: rising edge posts immediately (HW level-flag holds while
  // someone is present, so this fires once per arrival). Falling edge is
  // held down for 10 consecutive clear polls (~1-2s) so brief dropouts at
  // the detection margin don't flap cleared/detected pairs into the ring.
  {
    if (presence) {
      sPresenceEvtClearPolls = 0;
      if (!sPresenceEvtPresent) {
        sPresenceEvtPresent = true;
        char sv[12];
        snprintf(sv, sizeof(sv), "%d", (int)presenceVal);
        systemEventPost(SYSEVT_PRESENCE_DETECTED, sv);
      }
    } else if (sPresenceEvtPresent) {
      if (++sPresenceEvtClearPolls >= 10) {
        sPresenceEvtPresent = false;
        sPresenceEvtClearPolls = 0;
        systemEventPost(SYSEVT_PRESENCE_CLEARED);
      }
    }
  }

  // Motion algorithm flag (FUNC_STATUS bit 1) is distinct from presence.
  // Edge-latch it so we post once per rise/fall, not every poll.
  if (motion != gMotionActive) {
    gMotionActive = motion;
    systemEventPost(SYSEVT_MOTION_DETECTED, motion ? "detected" : "cleared");
  }

  return true;
}

// ============================================================================
// Presence Command Registry
// ============================================================================

const char* cmd_presenceautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim();
  if (arg.length() == 0) {
    return gSettings.presenceAutoStart ? "[Presence] Auto-start: enabled" : "[Presence] Auto-start: disabled";
  }
  arg.toLowerCase();
  if (arg == "on" || arg == "true" || arg == "1") {
    setSetting(gSettings.presenceAutoStart, true);
    return "[Presence] Auto-start enabled";
  } else if (arg == "off" || arg == "false" || arg == "0") {
    setSetting(gSettings.presenceAutoStart, false);
    return "[Presence] Auto-start disabled";
  }
  return "Error: invalid arguments — Usage: presenceautostart [on|off]";
}

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry presenceCommands[] = {
  // 3-level voice: "sensor" -> "presence" -> "open/close"
  { "openpresence", "Start STHS34PF80 IR presence/motion sensor.", false, cmd_presencestart, nullptr, "sensor", "presence", "open" },
  { "closepresence", "Stop STHS34PF80 sensor.", false, cmd_presencestop, nullptr, "sensor", "presence", "close" },
  { "presenceread", "Read STHS34PF80 presence/motion/temperature data. (add 'json' for JSON output)", false, cmd_presenceread },
  { "presencestatus", "Show STHS34PF80 sensor status. (add 'json' for JSON output)", false, cmd_presencestatus },
  
  // Auto-start
  { "presenceautostart", "Enable/disable presence auto-start after boot [on|off]", false, cmd_presenceautostart, "Usage: presenceautostart [on|off]" },
};

const size_t presenceCommandsCount = sizeof(presenceCommands) / sizeof(presenceCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// JSON building for ESP-NOW streaming
// ============================================================================

int presenceBuildDataJSON(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;
  
  SensorCacheGuard g(gPresenceCache.mutex, pdMS_TO_TICKS(50), "presence.buildJSON");
  if (!g.held) {
    // Cache-lock timeout: not-ready envelope (was: return 0 / no output).
    int pos = sensorEnvelopeBegin(buf, bufSize, false, gPresenceConnected, 0);
    if (pos == 0) return 0;
    int n = snprintf(buf + pos, bufSize - pos,
                     ",\"ambient\":0,\"presence\":0,\"presenceDetected\":false,"
                     "\"motion\":0,\"motionDetected\":false,\"tempShock\":0,\"tempShockDetected\":false}");
    if (n < 0 || (size_t)n >= bufSize - pos) return 0;
    return pos + n;
  }

  int pos = sensorEnvelopeBegin(buf, bufSize, gPresenceCache.dataValid, gPresenceConnected, gPresenceCache.lastUpdate);
  if (pos == 0) return 0;
  int n = snprintf(buf + pos, bufSize - pos,
                   ",\"ambient\":%.2f,\"presence\":%d,\"presenceDetected\":%s,"
                   "\"motion\":%d,\"motionDetected\":%s,\"tempShock\":%d,\"tempShockDetected\":%s}",
                   gPresenceCache.ambientTemp,
                   gPresenceCache.presenceValue, gPresenceCache.presenceDetected ? "true" : "false",
                   gPresenceCache.motionValue, gPresenceCache.motionDetected ? "true" : "false",
                   gPresenceCache.tempShockValue, gPresenceCache.tempShockDetected ? "true" : "false");
  if (n < 0 || (size_t)n >= bufSize - pos) return 0;
  return pos + n;
}

// ============================================================================
// Presence Task Implementation
// ============================================================================

void presenceTask(void* parameter) {
  sPresenceEvtPresent = false;
  sPresenceEvtClearPolls = 0;
  gMotionActive = false;
  INFO_PRESENCE_LIFECYCLEF("Task started (handle=%p, stack=%u words)", 
                (void*)xTaskGetCurrentTaskHandle(), 
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  INFO_PRESENCE_LIFECYCLEF("[MODULAR] presenceTask() running from i2csensor_sths34pf80.cpp");
  
  unsigned long lastPresenceRead = 0;
  unsigned long lastStackLog = 0;

  while (true) {
    // Check if sensor disabled for graceful shutdown
    if (!gPresenceRunning) {
      gPresenceConnected = false;
      gPresenceCache.dataValid = false;
      SENSOR_TASK_EXIT(PRESENCE);
    }

    // Stack watermark tracking + safety bailout
    unsigned long nowMs = millis();
    if ((nowMs - lastStackLog) >= 10000) {
      lastStackLog = nowMs;
      // 'continue' (not 'break') so the top-of-loop shutdown runs the clean
      // SENSOR_TASK_EXIT (vTaskDelete) path instead of returning from the task
      // function with a near-overflowed stack (IllegalInstruction panic).
      if (checkTaskStackSafety("presence", PRESENCE_STACK_WORDS, &gPresenceRunning)) continue;
      if (gPresenceRunning && isDebugFlagSet(DEBUG_PERFORMANCE)) {
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
        DEBUG_PERFORMANCEF("[STACK] presence_task watermark=%u words", (unsigned)watermark);
      }
      if (gPresenceRunning && isDebugFlagSet(DEBUG_MEMORY)) {
        DEBUG_MEMORY_HEAPF("[HEAP] presence_task: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }
    
    // NOTE: hybrid driver — init() is bus-aware (5-arg), but this poll uses the
    // legacy 4-arg i2cTaskWithTimeout(I2C_ADDR_PRESENCE, ...) which routes to
    // bus 0 (Wire1). Gate on the bus the poll ACTUALLY uses (0), not the
    // presenceBus setting it doesn't honor here. Revisit if the poll is migrated
    // to the 5-arg bus-aware form.
    if (gPresenceRunning && gPresenceConnected && !pollPaused(0 /* poll uses legacy bus 0 */)) {
      unsigned long presencePollMs = (gSettings.presenceDevicePollMs > 0) ? (unsigned long)gSettings.presenceDevicePollMs : 200;
      
      if ((nowMs - lastPresenceRead) >= presencePollMs) {
        bool ok = i2cTaskWithTimeout(I2C_ADDR_PRESENCE, 100000, 100, [&]() -> bool {
          return presencePoll();
        });
        
        if (ok) {
          // ESP-NOW broadcaster reads presenceBuildDataJSON() on demand from gPresenceCache.
        } else {
          if (i2cShouldAutoDisable(I2C_ADDR_PRESENCE)) {
            uint8_t errors = i2cGetConsecutiveErrors(I2C_ADDR_PRESENCE);
            gPresenceRunning = false;
            gPresenceConnected = false;
            sensorStatusBumpWith("presence@auto_disabled");
            DEBUG_PRESENCE_LIFECYCLEF("Presence auto-disabled after %u consecutive I2C failures", errors);
            logSystemEvent("SENSOR", "Presence auto-disabled after %u consecutive I2C failures", errors);
            { char det[24]; snprintf(det, sizeof(det), "%u I2C errors", errors);
              systemEventPost(SYSEVT_SENSOR_FAULT, "Presence", det); }
            // 'continue' (not 'break') — see the same note on the stack-safety
            // bailout above. `break` here left the while(true) loop and RETURNED
            // from the task entry point, which FreeRTOS turns into a jump to
            // address 0 (InstrFetchProhibited panic + reboot) rather than a task
            // exit. It also skipped the cache-invalidate the shutdown path
            // performs. gPresenceRunning is cleared above, so the top-of-loop
            // check now runs the clean SENSOR_TASK_EXIT.
            continue;
          }
        }
        lastPresenceRead = nowMs;
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));
    drainDebugRing();
  }
}

// ============================================================================
// Presence OLED Mode Registration
// ============================================================================

#if ENABLE_OLED_DISPLAY
#include "i2csensor_sths34pf80_oled.h"
#endif // ENABLE_OLED_DISPLAY

#endif // ENABLE_PRESENCE_SENSOR
