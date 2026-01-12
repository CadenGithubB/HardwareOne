#include "i2csensor-seesaw.h"
#include "System_BuildConfig.h"
#include "System_MemoryMonitor.h"
#include "System_Utils.h"

#if ENABLE_GAMEPAD_SENSOR

#include <Adafruit_seesaw.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
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

// Task handle (owned by this module)
TaskHandle_t gamepadTaskHandle = nullptr;

// Seesaw gamepad object (owned by this module)
Adafruit_seesaw gGamepadSeesaw(&Wire1);

// Debug system provides DEBUG_SENSORSF macro and gDebugFlags via debug_system.h

// External dependencies for task
extern volatile bool gSensorPollingPaused;
extern SemaphoreHandle_t i2cMutex;
extern void drainDebugRing();
extern void sensorStatusBumpWith(const char* reason);

// ============================================================================
// Gamepad/Control Cache (owned by this module)
// ============================================================================
ControlCache gControlCache;

// Gamepad sensor state (definitions)
bool gamepadEnabled = false;
bool gamepadConnected = false;
unsigned long gamepadLastStopTime = 0;

// Gamepad timing and constants
unsigned long gLastGamepadInitMs = 0;
const unsigned long kGamepadInitMinIntervalMs = 2000;

// Gamepad watermark tracking
volatile UBaseType_t gGamepadWatermarkMin = (UBaseType_t)0xFFFFFFFF;
volatile UBaseType_t gGamepadWatermarkNow = (UBaseType_t)0;

// ============================================================================
// Gamepad Sensor Command Handlers
// ============================================================================

const char* cmd_gamepad(const String& cmd) {
  if (!gamepadConnected) {
    // Attempt on-demand init with retry/backoff
    if (!initGamepadConnection()) {
      return "[Gamepad] Error: Not connected - check wiring";
    }
  }
  readGamepad();
  return "[Gamepad] Data read complete";
}

const char* cmd_gamepadstart(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Use sensor queue system (consistent with other sensors)
  extern bool enqueueSensorStart(SensorType sensor);
  extern bool isInQueue(SensorType sensor);
  extern int getQueuePosition(SensorType sensor);
  extern bool ensureDebugBuffer();

  if (gamepadEnabled) {
    return "[Gamepad] Sensor already running";
  }

  if (isInQueue(SENSOR_GAMEPAD)) {
    if (!ensureDebugBuffer()) return "[Gamepad] Already queued";
    int pos = getQueuePosition(SENSOR_GAMEPAD);
    snprintf(getDebugBuffer(), 1024, "[Gamepad] Already queued (position %d)", pos);
    return getDebugBuffer();
  }

  if (enqueueSensorStart(SENSOR_GAMEPAD)) {
    sensorStatusBumpWith("gamepadstart@enqueue");
    if (!ensureDebugBuffer()) return "[Gamepad] Sensor queued for start";
    int pos = getQueuePosition(SENSOR_GAMEPAD);
    snprintf(getDebugBuffer(), 1024, "[Gamepad] Sensor queued for start (position %d)", pos);
    return getDebugBuffer();
  }

  return "[Gamepad] Error: Failed to enqueue start (queue full)";
}

const char* cmd_gamepadstop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Disable flag - task will see this and clean up itself
  gamepadEnabled = false;
  gamepadLastStopTime = millis();
  Serial.printf("[GAMEPAD] cmd_gamepadstop: Set gamepadEnabled=false\n");
  
  // Broadcast sensor status to ESP-NOW master
#if ENABLE_ESPNOW
  #include "System_ESPNow_Sensors.h"
  extern bool meshEnabled();
  extern Settings gSettings;
  if (meshEnabled() && gSettings.meshRole != MESH_ROLE_MASTER) {
    broadcastSensorStatus(REMOTE_SENSOR_GAMEPAD, false);
  }
#endif
  
  return "[Gamepad] Stop requested; cleanup will complete asynchronously";
}

const char* cmd_gamepadautostart(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  int spacePos = cmd.indexOf(' ');
  if (spacePos < 0) {
    return gSettings.gamepadAutoStart ? "[Gamepad] Auto-start: enabled" : "[Gamepad] Auto-start: disabled";
  }
  
  String arg = cmd.substring(spacePos + 1);
  arg.trim();
  arg.toLowerCase();
  
  if (arg == "on" || arg == "true" || arg == "1") {
    gSettings.gamepadAutoStart = true;
    writeSettingsJson();
    return "[Gamepad] Auto-start enabled";
  } else if (arg == "off" || arg == "false" || arg == "0") {
    gSettings.gamepadAutoStart = false;
    writeSettingsJson();
    return "[Gamepad] Auto-start disabled";
  } else {
    return "Usage: gamepadautostart [on|off]";
  }
}

// ============================================================================
// Gamepad Internal Start (called by queue processor)
// ============================================================================

const char* startGamepadInternal() {
  DEBUG_CLIF("[QUEUE] Processing Gamepad start from queue");

  // Check memory before creating gamepad task
  if (!checkMemoryAvailable("gamepad", nullptr)) {
    return "Insufficient memory for Gamepad sensor";
  }

  // Clean up any stale cache from previous run BEFORE starting
  if (gControlCache.mutex && xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    gControlCache.gamepadDataValid = false;
    gControlCache.gamepadButtons = 0;
    gControlCache.gamepadX = 0;
    gControlCache.gamepadY = 0;
    xSemaphoreGive(gControlCache.mutex);
  }

  // Initialize Seesaw
  if (!initGamepad()) {
    return "Failed to initialize Gamepad";
  }

  // Mark enabled BEFORE task creation to avoid startup race where the task can self-delete
  // if it runs before gamepadEnabled is set.
  bool prev = gamepadEnabled;
  gamepadEnabled = true;
  Serial.printf("[GAMEPAD] startGamepadInternal: Set gamepadEnabled=true (was %d), gamepadConnected=%d\n", prev, gamepadConnected);
  if (gamepadEnabled != prev) sensorStatusBumpWith("gamepadstart@enabled");
  
  // Broadcast sensor status to ESP-NOW master
#if ENABLE_ESPNOW
  broadcastSensorStatus(REMOTE_SENSOR_GAMEPAD, true);
#endif

  // Create dedicated gamepad task
  if (!createGamepadTask()) {
    // Roll back enable flag if task creation fails
    gamepadEnabled = false;
    return "Failed to create Gamepad task";
  }
  return "SUCCESS: Gamepad initialized with dedicated task";
}

// ============================================================================
// Gamepad Initialization and Reading Functions
// ============================================================================

bool initGamepad() {
  // If we've already got a connection, consider it initialized
  if (gamepadConnected) {
    Serial.println("[GAMEPAD] initGamepad: already connected, returning true");
    return true;
  }

  Serial.println("[GAMEPAD] initGamepad: starting initialization...");

  // Use device-aware transaction wrapper for safe mutex + clock management + health tracking
  bool initSuccess = i2cDeviceTransaction(I2C_ADDR_GAMEPAD, 100000, 3000, [&]() -> bool {
    // Wire1 already initialized in setup() - no need to call begin() again

    // Attempt to begin Seesaw on 0x50 using Wire1 bus
    if (!gGamepadSeesaw.begin(I2C_ADDR_GAMEPAD)) {
      Serial.println("[GAMEPAD] ERROR: Seesaw (Gamepad) not found at 0x50 on Wire1");
      return false;
    }

    // Verify product ID (upper 16 bits of getVersion()) should be 5743
    uint32_t version = ((gGamepadSeesaw.getVersion() >> 16) & 0xFFFF);
    Serial.printf("[GAMEPAD] Seesaw version: %lu (expected 5743)\n", (unsigned long)version);
    if (version != 5743) {
      Serial.printf("[GAMEPAD] WARNING: Seesaw product mismatch: got %lu, expected 5743 (Mini I2C Gamepad)\n", (unsigned long)version);
      // Not fatal: continue, as other seesaw variants may still be usable
    }

    // Configure gamepad button inputs with pullups and enable GPIO interrupts
    // Button bit definitions from Adafruit example
    const uint8_t BUTTON_X = 6;
    const uint8_t BUTTON_Y = 2;
    const uint8_t BUTTON_A = 5;
    const uint8_t BUTTON_B = 1;
    const uint8_t BUTTON_SELECT = 0;
    const uint8_t BUTTON_START = 16;

    uint32_t button_mask = (1UL << BUTTON_X) | (1UL << BUTTON_Y) | (1UL << BUTTON_START) | (1UL << BUTTON_A) | (1UL << BUTTON_B) | (1UL << BUTTON_SELECT);

    gGamepadSeesaw.pinModeBulk(button_mask, INPUT_PULLUP);
    gGamepadSeesaw.setGPIOInterrupts(button_mask, 1);

    Serial.println("[GAMEPAD] Seesaw hardware init complete inside lambda");
    return true;
  });

  Serial.printf("[GAMEPAD] i2cDeviceTransaction returned: %d\n", initSuccess);

  // Set gamepadConnected OUTSIDE the lambda (like IMU pattern)
  if (initSuccess) {
    gamepadConnected = true;
    i2cRegisterDevice(I2C_ADDR_GAMEPAD, "Gamepad");
    Serial.printf("[GAMEPAD] SUCCESS: gamepadConnected=%d gamepadEnabled=%d &enabled=%p &connected=%p &cache=%p\n", 
                  gamepadConnected, gamepadEnabled, (void*)&gamepadEnabled, (void*)&gamepadConnected, (void*)&gControlCache);
    broadcastOutput("Gamepad (Seesaw) initialized");
  } else {
    Serial.println("[GAMEPAD] FAILED: initGamepad returning false");
  }

  return initSuccess;
}

// Helper function for I2C ping
static bool i2cPing(TwoWire* bus, uint8_t addr) {
  (void)bus;
  return i2cPingAddress(addr, 100000, 200);
}

bool initGamepadConnection() {
  if (gamepadConnected) return true;
  unsigned long now = millis();
  if (now - gLastGamepadInitMs < kGamepadInitMinIntervalMs) {
    broadcastOutput("Gamepad: skipping re-init (backoff window)");
    return false;  // backoff window
  }
  gLastGamepadInitMs = now;
  broadcastOutput("Gamepad: attempting re-init");

  // Quick ping first to avoid costly begin() if device not present
  bool seen = false;
  for (int p = 0; p < 2; ++p) {
    if (i2cPing(&Wire1, 0x50)) {
      seen = true;
      break;
    }
    delay(5);
  }
  if (!seen) {
    WARN_SENSORSF("Gamepad: no ACK at 0x50");
    broadcastOutput("Gamepad: no ACK at 0x50");
    return false;
  }

  // Try up to 3 begin attempts with small delays
  for (int attempt = 1; attempt <= 3; ++attempt) {
    char msg[48];
    snprintf(msg, sizeof(msg), "Gamepad: re-init attempt %d", attempt);
    broadcastOutput(msg);
    bool began = i2cDeviceTransaction(I2C_ADDR_GAMEPAD, 100000, 500, [&]() -> bool {
      return gGamepadSeesaw.begin(I2C_ADDR_GAMEPAD);
    });
    if (began) {
      // Optional: soft reset for clean state
      // gGamepadSeesaw.swReset(); delay(2);

      // Validate by reading a couple of registers/values
      i2cDeviceTransactionVoid(I2C_ADDR_GAMEPAD, 100000, 500, [&]() {
        (void)gGamepadSeesaw.getVersion();
        (void)gGamepadSeesaw.analogRead(14);
        (void)gGamepadSeesaw.analogRead(15);
      });

      gamepadEnabled = true;
      gamepadConnected = true;
      Serial.printf("[GAMEPAD_DEBUG] initGamepadConnection: &enabled=%p &connected=%p &gControlCache=%p\n", (void*)&gamepadEnabled, (void*)&gamepadConnected, (void*)&gControlCache);
      
      // Register gamepad for I2C health tracking (may already be registered, that's OK)
      i2cRegisterDevice(I2C_ADDR_GAMEPAD, "Gamepad");
      INFO_SENSORSF("Gamepad connected on attempt %d", attempt);
      snprintf(msg, sizeof(msg), "Gamepad: re-init success (attempt %d)", attempt);
      broadcastOutput(msg);
      return true;
    }
    INFO_SENSORSF("Gamepad attempt %d failed, retrying", attempt);
    snprintf(msg, sizeof(msg), "Gamepad: attempt %d failed", attempt);
    broadcastOutput(msg);
    delay(15);
  }
  broadcastOutput("Gamepad: re-init failed after retries");
  return false;
}

void readGamepad() {
  if (!gamepadConnected) {
    broadcastOutput("Gamepad not connected. Check wiring.");
    return;
  }

  uint32_t buttons = 0;
  int16_t x = 0, y = 0;
  
  // Must use I2C transaction wrapper to prevent bus contention
  i2cDeviceTransactionVoid(I2C_ADDR_GAMEPAD, 100000, 200, [&]() {
    buttons = gGamepadSeesaw.digitalReadBulk(0xFFFFFFFF);
    x = gGamepadSeesaw.analogRead(14);
    y = gGamepadSeesaw.analogRead(15);
  });

  String gamepadData = "Buttons: 0x" + String(buttons, HEX) + ", X: " + String(x) + ", Y: " + String(y);
  broadcastOutput(gamepadData);
}

// ============================================================================
// Gamepad Modular Settings Registration (for safety and reliability)
// ============================================================================

// Gamepad settings entries - minimal but essential for safety
static const SettingEntry gamepadSettingEntries[] = {
  // Device-level settings (sensor hardware behavior)
  { "device.devicePollMs", SETTING_INT, &gSettings.gamepadDevicePollMs, 58, 0, nullptr, 10, 1000, "Poll Interval (ms)", nullptr },
  { "autoStart", SETTING_BOOL, &gSettings.gamepadAutoStart, 1, 0, nullptr, 0, 1, "Auto-start after boot", nullptr }
};

static bool isGamepadConnected() {
  return gamepadConnected;
}

extern const SettingsModule gamepadSettingsModule = {
  "gamepad",
  nullptr,
  gamepadSettingEntries,
  sizeof(gamepadSettingEntries) / sizeof(gamepadSettingEntries[0]),
  isGamepadConnected,
  "Seesaw gamepad settings"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================
// Gamepad Command Registry
// ============================================================================

const CommandEntry gamepadCommands[] = {
  { "gamepadstart", "Queue start of Gamepad task (seesaw)", false, cmd_gamepadstart },
  { "gamepadstop", "Stop Gamepad task (seesaw)", false, cmd_gamepadstop },
  { "gamepad", "Read Seesaw gamepad state (x/y/buttons).", false, cmd_gamepad },
  { "gamepadautostart", "Enable/disable gamepad auto-start after boot [on|off]", false, cmd_gamepadautostart, "Usage: gamepadautostart [on|off]" },
};

const size_t gamepadCommandsCount = sizeof(gamepadCommands) / sizeof(gamepadCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _gamepad_cmd_registrar(gamepadCommands, gamepadCommandsCount, "gamepad");

// ============================================================================
// Gamepad Task Implementation (moved from i2c_system.cpp for full modularization)
// ============================================================================

// ============================================================================
// Gamepad Task - FreeRTOS Task Function
// ============================================================================
// Purpose: Continuously reads button and joystick state from Seesaw gamepad
// Stack: 4096 words (~16KB) | Priority: 1 | Core: Any
// Lifecycle: Created by cmd_gamepadstart, deleted when gamepadEnabled=false
// Polling: Fixed 50ms interval | I2C Clock: 100kHz
//
// Cleanup Strategy:
//   1. Check gamepadEnabled flag at loop start
//   2. Acquire i2cMutex to prevent race conditions during cleanup
//   3. Invalidate cache (no sensor object to delete)
//   4. Release mutex and delete task
// ============================================================================

void gamepadTask(void* parameter) {
  INFO_SENSORSF("[Gamepad] Task started (handle=%p, stack=%u words)", 
                (void*)xTaskGetCurrentTaskHandle(), 
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  Serial.println("[MODULAR] gamepadTask() running from Sensor_Gamepad_Seesaw.cpp");
  Serial.printf("[GAMEPAD_TASK] Initial state: enabled=%d connected=%d\n", gamepadEnabled, gamepadConnected);
  unsigned long lastGamepadRead = 0;
  unsigned long lastStackLog = 0;
  unsigned long lastStateLog = 0;
  // Note: Failure tracking now handled by centralized I2CDevice health system
  // Use i2cShouldAutoDisable() instead of local counters

  // EWMA smoothing state
  int filtX = -1, filtY = -1;
  const float kAlpha = 0.7f;

  while (true) {
    // CRITICAL: Check enabled flag FIRST for graceful shutdown
    if (!gamepadEnabled) {
      // Perform safe cleanup before task deletion - RACE CONDITION FIX:
      // Take I2C mutex to ensure no other tasks are in active gamepad I2C transactions
      if (gamepadConnected) {
        // Wait for all active gamepad I2C transactions to complete before cleanup
        if (i2cMutex && xSemaphoreTake(i2cMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
          // Now safe to cleanup - no other tasks can access gamepad sensor
          gamepadConnected = false;
          
          // Invalidate cache - no locking needed since we hold i2cMutex
          gControlCache.gamepadDataValid = false;
          
          xSemaphoreGive(i2cMutex);
          
          // Brief delay to ensure cleanup propagates
          vTaskDelay(pdMS_TO_TICKS(50));
        } else {
          // Mutex timeout - force cleanup anyway to prevent deadlock
          gamepadConnected = false;
          gControlCache.gamepadDataValid = false;
          vTaskDelay(pdMS_TO_TICKS(100));
        }
      }
      
      // ALWAYS delete task when disabled (consistent with thermal/IMU/ToF/APDS/GPS)
      // NOTE: Do NOT clear gamepadTaskHandle here - let create function use eTaskGetState()
      // to detect stale handles. Clearing here creates a race condition window.
      Serial.println("[GAMEPAD_TASK] Task disabled - cleaning up and deleting");
      vTaskDelete(nullptr);
    }

    // Stack watermark tracking
    unsigned long nowMs = millis();
    if ((nowMs - lastStackLog) >= 10000) {
      lastStackLog = nowMs;
      if (isDebugFlagSet(DEBUG_PERFORMANCE)) {
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
        gGamepadWatermarkNow = watermark;
        if (watermark < gGamepadWatermarkMin) {
          gGamepadWatermarkMin = watermark;
        }
        DEBUG_PERFORMANCEF("[STACK] gamepad_task watermark_now=%u min=%u words", (unsigned)gGamepadWatermarkNow, (unsigned)gGamepadWatermarkMin);
      }
      if (isDebugFlagSet(DEBUG_MEMORY)) {
        DEBUG_MEMORYF("[HEAP] gamepad_task: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }

    // Periodic state logging (every 5 seconds)
    if ((nowMs - lastStateLog) >= 5000) {
      lastStateLog = nowMs;
      Serial.printf("[GAMEPAD_TASK] State: enabled=%d connected=%d paused=%d dataValid=%d\n", 
                    gamepadEnabled, gamepadConnected, gSensorPollingPaused, gControlCache.gamepadDataValid);
    }

    if (gamepadEnabled && gamepadConnected && !gSensorPollingPaused) {
      unsigned long gamepadPollMs = (gSettings.gamepadDevicePollMs > 0) ? (unsigned long)gSettings.gamepadDevicePollMs : 58;
      if ((nowMs - lastGamepadRead) >= gamepadPollMs) {
        bool readSuccess = false;
        uint32_t buttons = 0;
        int rawX = 0, rawY = 0;

        // Use task timeout wrapper to catch gamepad performance issues
        auto result = i2cTaskWithStandardTimeout(I2C_ADDR_GAMEPAD, 100000, [&]() -> bool {
          // Exceptions are disabled (-fno-exceptions), so rely on return value only.
          buttons = gGamepadSeesaw.digitalReadBulk(0xFFFFFFFF);
          rawX = 1023 - gGamepadSeesaw.analogRead(14);
          rawY = 1023 - gGamepadSeesaw.analogRead(15);
          return true;
        });

        // Honor the actual I2C read result
        readSuccess = (result == true);
        
        // Sanity check: reject garbage reads (e.g., 0x64000000 during I2C bus contention)
        // Valid button states have specific bit patterns - upper bits should be stable
        bool dataValid = true;
        if (readSuccess && (buttons & 0xFF000000) != 0) {
          // Upper byte should be 0 for valid reads - this is garbage data
          dataValid = false;
        }

        if (readSuccess && dataValid) {
          // Note: I2CDevice::recordSuccess() called automatically by transaction
          // which resets consecutiveErrors - no local counter needed
          
          // Track previous state for change detection
          static uint32_t lastButtons = 0xFFFFFFFF;
          static int lastFiltX = -1;
          static int lastFiltY = -1;
          static unsigned long lastESPNowSend = 0;
          
          // Debug: Log EVERY button state change immediately
          if (buttons != lastButtons) {
            uint32_t changed = buttons ^ lastButtons;
            uint32_t pressed = ~buttons & changed;   // Bits that went from 1 to 0 (pressed)
            uint32_t released = buttons & changed;   // Bits that went from 0 to 1 (released)
            
            Serial.printf("[GAMEPAD_PRESS] buttons=0x%08lX changed=0x%08lX pressed=0x%08lX released=0x%08lX\n",
                          (unsigned long)buttons, (unsigned long)changed, 
                          (unsigned long)pressed, (unsigned long)released);
            
            // Log individual button names for clarity
            if (pressed & GAMEPAD_BUTTON_A) Serial.println("[GAMEPAD_PRESS] A pressed");
            if (pressed & GAMEPAD_BUTTON_B) Serial.println("[GAMEPAD_PRESS] B pressed");
            if (pressed & GAMEPAD_BUTTON_X) Serial.println("[GAMEPAD_PRESS] X pressed");
            if (pressed & GAMEPAD_BUTTON_Y) Serial.println("[GAMEPAD_PRESS] Y pressed");
            if (pressed & GAMEPAD_BUTTON_START) Serial.println("[GAMEPAD_PRESS] START pressed");
            
            if (released & GAMEPAD_BUTTON_A) Serial.println("[GAMEPAD_PRESS] A released");
            if (released & GAMEPAD_BUTTON_B) Serial.println("[GAMEPAD_PRESS] B released");
            if (released & GAMEPAD_BUTTON_X) Serial.println("[GAMEPAD_PRESS] X released");
            if (released & GAMEPAD_BUTTON_Y) Serial.println("[GAMEPAD_PRESS] Y released");
            if (released & GAMEPAD_BUTTON_START) Serial.println("[GAMEPAD_PRESS] START released");
            
            lastButtons = buttons;
          }
          
          if (filtX < 0 || filtY < 0) {
            filtX = rawX;
            filtY = rawY;
          } else {
            filtX = (int)lroundf(kAlpha * rawX + (1.0f - kAlpha) * filtX);
            filtY = (int)lroundf(kAlpha * rawY + (1.0f - kAlpha) * filtY);
          }

          if (gControlCache.mutex && xSemaphoreTake(gControlCache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            gControlCache.gamepadButtons = buttons;
            gControlCache.gamepadX = filtX;
            gControlCache.gamepadY = filtY;
            gControlCache.gamepadLastUpdate = nowMs;
            gControlCache.gamepadDataValid = true;
            gControlCache.gamepadSeq++;
            xSemaphoreGive(gControlCache.mutex);
          }
          
          // Stream data to ESP-NOW master if enabled (worker devices only)
#if ENABLE_ESPNOW
          if (meshEnabled() && gSettings.meshRole != MESH_ROLE_MASTER) {
            // Detect if ANY input changed (buttons or joystick)
            bool buttonsChanged = (buttons != lastButtons);
            bool joystickMoved = (abs(filtX - lastFiltX) > 10 || abs(filtY - lastFiltY) > 10);
            bool inputChanged = buttonsChanged || joystickMoved;
            
            // Rate limit: minimum 100ms between sends to prevent network spam
            const unsigned long minSendInterval = 100;
            unsigned long timeSinceLastSend = nowMs - lastESPNowSend;
            bool canSend = (timeSinceLastSend >= minSendInterval);
            
            // Send if: (input changed AND rate limit allows) OR (been >1s since last send)
            // Only send if sensor broadcasting is enabled
            extern bool isSensorBroadcastEnabled();
            if (isSensorBroadcastEnabled() && ((inputChanged && canSend) || (timeSinceLastSend >= 1000))) {
              char gamepadJson[128];
              int jsonLen = snprintf(gamepadJson, sizeof(gamepadJson),
                                     "{\"val\":1,\"x\":%d,\"y\":%d,\"buttons\":%lu}",
                                     filtX, filtY, (unsigned long)buttons);
              if (jsonLen > 0 && jsonLen < 128) {
                // Bypass sendSensorDataUpdate's rate limiting by calling mesh directly
                extern void meshSendEnvelopeToPeers(const String& payload);
                JsonDocument doc;
                doc["type"] = MSG_TYPE_SENSOR_DATA;
                doc["sensor"] = "gamepad";
                JsonDocument dataDoc;
                deserializeJson(dataDoc, gamepadJson);
                doc["data"] = dataDoc.as<JsonObject>();
                String message;
                serializeJson(doc, message);
                meshSendEnvelopeToPeers(message);
                
                lastESPNowSend = nowMs;
                lastFiltX = filtX;
                lastFiltY = filtY;
              }
            }
          }
#endif
        } else if (!readSuccess) {
          // Actual I2C transaction failure
          // Note: I2CDevice::recordError() called automatically by transaction
          uint8_t errors = i2cGetConsecutiveErrors(I2C_ADDR_GAMEPAD);
          Serial.printf("[GAMEPAD_TASK] I2C read failure (consecutive: %u)\n", errors);
          
          if (i2cShouldAutoDisable(I2C_ADDR_GAMEPAD, 5)) {
            Serial.println("[GAMEPAD_TASK] Too many consecutive failures - auto-disabling");
            gamepadEnabled = false;
            gamepadConnected = false;
            DEBUG_FRAMEF("Gamepad auto-disabled: %u consecutive I2C failures", errors);
            sensorStatusBumpWith("gamepad@auto_disabled");
          }
        } else {
          // I2C succeeded but data validation failed (garbage data during bus contention)
          // Don't log every instance - this is common during thermal sensor reads
        }
        lastGamepadRead = nowMs;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      drainDebugRing();
    } else {
      vTaskDelay(pdMS_TO_TICKS(20));
      drainDebugRing();
    }
  }
}

// ============================================================================
// Gamepad OLED Mode
// ============================================================================
#include "i2csensor-seesaw-oled.h"

#endif // ENABLE_GAMEPAD_SENSOR
