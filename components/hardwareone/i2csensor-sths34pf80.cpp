#include "System_BuildConfig.h"

#if ENABLE_PRESENCE_SENSOR

#include <Arduino.h>
#include <Wire.h>

#include "i2csensor-sths34pf80.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_MemoryMonitor.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_Utils.h"

// External dependencies
extern void sensorStatusBumpWith(const char* reason);
extern volatile bool gSensorPollingPaused;
extern SemaphoreHandle_t i2cMutex;
extern void drainDebugRing();

// ============================================================================
// STHS34PF80 Register Definitions
// ============================================================================
#define STHS34PF80_ADDR           0x5A

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
bool presenceEnabled = false;
bool presenceConnected = false;
TaskHandle_t presenceTaskHandle = nullptr;

// Helper: Create presence task if not already running
static bool createPresenceTask() {
  extern BaseType_t xTaskCreateLogged(TaskFunction_t, const char*, uint32_t, void*, UBaseType_t, TaskHandle_t*, const char*);
  extern void presenceTask(void* parameter);
  
  // Check for stale task handle
  if (presenceTaskHandle != nullptr) {
    eTaskState state = eTaskGetState(presenceTaskHandle);
    if (state == eDeleted || state == eInvalid) {
      presenceTaskHandle = nullptr;
    }
  }
  if (presenceTaskHandle == nullptr) {
    const uint32_t presenceStack = 3072;
    if (xTaskCreateLogged(presenceTask, "presence_task", presenceStack, nullptr, 1, &presenceTaskHandle, "presence") != pdPASS) {
      return false;
    }
    DEBUG_SENSORSF("Presence task created successfully");
  }
  return true;
}

// ============================================================================
// STHS34PF80 Modular Settings Registration
// ============================================================================

static const SettingEntry presenceSettingEntries[] = {
  { "presenceAutoStart",    SETTING_BOOL, &gSettings.presenceAutoStart,    0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr },
  { "presenceDevicePollMs", SETTING_INT,  &gSettings.presenceDevicePollMs, 100, 0, nullptr, 50, 5000, "Poll Interval (ms)", nullptr }
};

static bool isPresenceConnected() {
  return presenceConnected;
}

extern const SettingsModule presenceSettingsModule = {
  "presence",
  nullptr,
  presenceSettingEntries,
  sizeof(presenceSettingEntries) / sizeof(presenceSettingEntries[0]),
  isPresenceConnected,
  "STHS34PF80 IR presence/motion sensor settings"
};

// ============================================================================
// Low-level I2C Helper Functions
// ============================================================================

static bool writeRegister(uint8_t reg, uint8_t value) {
  extern TwoWire Wire1;
  Wire1.beginTransmission(STHS34PF80_ADDR);
  Wire1.write(reg);
  Wire1.write(value);
  return (Wire1.endTransmission() == 0);
}

static bool readRegister(uint8_t reg, uint8_t* value) {
  extern TwoWire Wire1;
  Wire1.beginTransmission(STHS34PF80_ADDR);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0) return false;
  
  if (Wire1.requestFrom(STHS34PF80_ADDR, (uint8_t)1) != 1) return false;
  *value = Wire1.read();
  return true;
}

static bool readRegisters(uint8_t reg, uint8_t* buffer, uint8_t len) {
  extern TwoWire Wire1;
  Wire1.beginTransmission(STHS34PF80_ADDR);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0) return false;
  
  if (Wire1.requestFrom(STHS34PF80_ADDR, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) {
    buffer[i] = Wire1.read();
  }
  return true;
}

static int16_t readInt16(uint8_t regL) {
  uint8_t buf[2];
  if (!readRegisters(regL, buf, 2)) return 0;
  return (int16_t)(buf[0] | (buf[1] << 8));
}

// ============================================================================
// Presence Sensor Command Handlers
// ============================================================================

const char* cmd_presencestart(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  extern bool enqueueSensorStart(SensorType sensor);
  extern bool isInQueue(SensorType sensor);
  extern int getQueuePosition(SensorType sensor);
  extern bool ensureDebugBuffer();
  
  if (presenceEnabled) {
    return "[PRESENCE] Error: Already running";
  }
  
  if (isInQueue(SENSOR_PRESENCE)) {
    if (!ensureDebugBuffer()) return "[PRESENCE] Already in queue";
    int pos = getQueuePosition(SENSOR_PRESENCE);
    snprintf(getDebugBuffer(), 1024, "[PRESENCE] Already in queue at position %d", pos);
    return getDebugBuffer();
  }
  
  if (enqueueSensorStart(SENSOR_PRESENCE)) {
    sensorStatusBumpWith("presencestart@enqueue");
    if (!ensureDebugBuffer()) return "[PRESENCE] Sensor queued for start";
    int pos = getQueuePosition(SENSOR_PRESENCE);
    snprintf(getDebugBuffer(), 1024, "[PRESENCE] Sensor queued for start (position %d)", pos);
    return getDebugBuffer();
  }
  
  return "[PRESENCE] Error: Failed to enqueue start (queue full)";
}

const char* cmd_presencestop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!presenceEnabled) {
    return "[PRESENCE] Error: Not running";
  }
  
  presenceEnabled = false;
  sensorStatusBumpWith("presencestop@CLI");
  return "[PRESENCE] Sensor stop requested; cleanup will complete asynchronously";
}

const char* cmd_presenceread(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!presenceConnected || !presenceEnabled) {
    return "[PRESENCE] Error: Sensor not running - use 'presencestart' first";
  }
  
  if (!ensureDebugBuffer()) return "[PRESENCE] Error: Debug buffer unavailable";
  
  if (gPresenceCache.mutex && xSemaphoreTake(gPresenceCache.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    snprintf(getDebugBuffer(), 1024,
      "[PRESENCE] Ambient: %.2f°C | Presence: %d %s | Motion: %d %s | TShock: %d %s",
      gPresenceCache.ambientTemp,
      gPresenceCache.presenceValue,
      gPresenceCache.presenceDetected ? "[DETECTED]" : "",
      gPresenceCache.motionValue,
      gPresenceCache.motionDetected ? "[DETECTED]" : "",
      gPresenceCache.tempShockValue,
      gPresenceCache.tempShockDetected ? "[DETECTED]" : "");
    xSemaphoreGive(gPresenceCache.mutex);
    return getDebugBuffer();
  }
  
  return "[PRESENCE] Error: Could not read cache";
}

const char* cmd_presencestatus(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "[PRESENCE] Error: Debug buffer unavailable";
  
  snprintf(getDebugBuffer(), 1024,
    "[PRESENCE] Status: connected=%d enabled=%d taskHandle=%p dataValid=%d",
    presenceConnected, presenceEnabled, (void*)presenceTaskHandle,
    gPresenceCache.dataValid);
  return getDebugBuffer();
}

// ============================================================================
// Presence Sensor Initialization and Reading Functions
// ============================================================================

bool startPresenceSensorInternal() {
  // Check memory before creating task
  if (!checkMemoryAvailable("presence", nullptr)) {
    ERROR_SENSORSF("[PRESENCE] Error: Insufficient memory for presence sensor");
    return false;
  }

  // Create cache mutex if not already created
  if (!gPresenceCache.mutex) {
    gPresenceCache.mutex = xSemaphoreCreateMutex();
    if (!gPresenceCache.mutex) {
      ERROR_SENSORSF("[PRESENCE] Error: Failed to create cache mutex");
      return false;
    }
    DEBUG_SENSORSF("[PRESENCE] Cache mutex created");
  }

  // Clean up stale cache
  if (gPresenceCache.mutex && xSemaphoreTake(gPresenceCache.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
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
    xSemaphoreGive(gPresenceCache.mutex);
  }
  INFO_SENSORSF("[PRESENCE] Cleaned up stale cache");

  // Initialize sensor synchronously
  if (!presenceConnected) {
    if (!initPresenceSensor()) {
      ERROR_SENSORSF("[PRESENCE] Error: Failed to initialize STHS34PF80 sensor");
      return false;
    }
  }

  // Create task
  if (!createPresenceTask()) {
    ERROR_SENSORSF("[PRESENCE] Error: Failed to create presence task");
    return false;
  }

  presenceEnabled = true;
  sensorStatusBumpWith("PRESENCE initialized");
  INFO_SENSORSF("[PRESENCE] Sensor started successfully");
  return true;
}

bool initPresenceSensor() {
  if (presenceConnected) {
    return true;
  }
  
  return i2cTaskWithTimeout(I2C_ADDR_PRESENCE, 100000, 500, [&]() -> bool {
    // Check WHO_AM_I
    uint8_t whoami;
    if (!readRegister(STHS34PF80_WHO_AM_I, &whoami)) {
      ERROR_SENSORSF("[PRESENCE] Failed to read WHO_AM_I");
      return false;
    }
    
    if (whoami != STHS34PF80_WHO_AM_I_VALUE) {
      ERROR_SENSORSF("[PRESENCE] Wrong WHO_AM_I: 0x%02X (expected 0x%02X)", whoami, STHS34PF80_WHO_AM_I_VALUE);
      return false;
    }
    
    INFO_SENSORSF("[PRESENCE] WHO_AM_I verified: 0x%02X", whoami);
    
    // Configure CTRL1: Set ODR to 8Hz, BDU enabled
    // Bits [6:4] = ODR, Bit 3 = BDU
    uint8_t ctrl1 = (STHS34PF80_ODR_8HZ << 4) | 0x08;
    if (!writeRegister(STHS34PF80_CTRL1, ctrl1)) {
      ERROR_SENSORSF("[PRESENCE] Failed to configure CTRL1");
      return false;
    }
    
    // Configure CTRL2: Enable presence, motion, and ambient shock detection
    // Defaults are usually fine, but ensure FUNC_CFG_ACCESS is 0
    if (!writeRegister(STHS34PF80_CTRL2, 0x00)) {
      ERROR_SENSORSF("[PRESENCE] Failed to configure CTRL2");
      return false;
    }
    
    presenceConnected = true;
    
    // Register for I2C health tracking
    i2cRegisterDevice(I2C_ADDR_PRESENCE, "STHS34PF80");
    return true;
  });
}

bool readPresenceData() {
  if (!presenceConnected) return false;
  
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
  if (gPresenceCache.mutex && xSemaphoreTake(gPresenceCache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
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
    xSemaphoreGive(gPresenceCache.mutex);
  }
  
  return true;
}

// ============================================================================
// Presence Command Registry
// ============================================================================

const CommandEntry presenceCommands[] = {
  { "presencestart", "Start STHS34PF80 IR presence/motion sensor.", false, cmd_presencestart },
  { "presencestop", "Stop STHS34PF80 sensor.", false, cmd_presencestop },
  { "presenceread", "Read STHS34PF80 presence/motion/temperature data.", false, cmd_presenceread },
  { "presencestatus", "Show STHS34PF80 sensor status.", false, cmd_presencestatus },
};

const size_t presenceCommandsCount = sizeof(presenceCommands) / sizeof(presenceCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _presence_cmd_registrar(presenceCommands, presenceCommandsCount, "presence");

// ============================================================================
// Presence Task Implementation
// ============================================================================

void presenceTask(void* parameter) {
  INFO_SENSORSF("[PRESENCE] Task started (handle=%p, stack=%u words)", 
                (void*)xTaskGetCurrentTaskHandle(), 
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  Serial.println("[MODULAR] presenceTask() running from i2csensor-sths34pf80.cpp");
  
  unsigned long lastPresenceRead = 0;
  unsigned long lastStackLog = 0;

  while (true) {
    // Check if sensor disabled for graceful shutdown
    if (!presenceEnabled) {
      if (presenceConnected) {
        if (i2cMutex && xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
          presenceConnected = false;
          gPresenceCache.dataValid = false;
          xSemaphoreGiveRecursive(i2cMutex);
          vTaskDelay(pdMS_TO_TICKS(50));
        } else {
          presenceConnected = false;
          gPresenceCache.dataValid = false;
          vTaskDelay(pdMS_TO_TICKS(100));
        }
      }
      vTaskDelete(nullptr);
    }

    // Stack watermark tracking
    unsigned long nowMs = millis();
    if ((nowMs - lastStackLog) >= 10000) {
      lastStackLog = nowMs;
      if (presenceEnabled && isDebugFlagSet(DEBUG_PERFORMANCE)) {
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
        DEBUG_PERFORMANCEF("[STACK] presence_task watermark=%u words", (unsigned)watermark);
      }
      if (presenceEnabled && isDebugFlagSet(DEBUG_MEMORY)) {
        DEBUG_MEMORYF("[HEAP] presence_task: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }
    
    if (presenceEnabled && presenceConnected && !gSensorPollingPaused) {
      unsigned long presencePollMs = (gSettings.presenceDevicePollMs > 0) ? (unsigned long)gSettings.presenceDevicePollMs : 100;
      
      if ((nowMs - lastPresenceRead) >= presencePollMs) {
        // LOW PRIORITY: Try to get bus with short timeout - yield to gamepad/other high-pri devices
        if (i2cMutex && xSemaphoreTakeRecursive(i2cMutex, pdMS_TO_TICKS(10)) != pdTRUE) {
          // Bus is busy - skip this cycle, let high-priority tasks (gamepad) go first
          vTaskDelay(pdMS_TO_TICKS(20));
          continue;
        }
        xSemaphoreGiveRecursive(i2cMutex);  // Release - transaction wrapper will re-acquire
        
        bool ok = i2cTaskWithTimeout(I2C_ADDR_PRESENCE, 100000, 100, [&]() -> bool {
          return readPresenceData();
        });
        
        if (!ok) {
          if (i2cShouldAutoDisable(I2C_ADDR_PRESENCE, 5)) {
            uint8_t errors = i2cGetConsecutiveErrors(I2C_ADDR_PRESENCE);
            presenceEnabled = false;
            presenceConnected = false;
            sensorStatusBumpWith("presence@auto_disabled");
            DEBUG_SENSORSF("Presence auto-disabled after %u consecutive I2C failures", errors);
            break;
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
#include "i2csensor-sths34pf80-oled.h"
#endif // ENABLE_OLED_DISPLAY

#endif // ENABLE_PRESENCE_SENSOR
