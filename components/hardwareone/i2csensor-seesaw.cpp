#include "i2csensor-seesaw.h"
#include "System_BuildConfig.h"
#include "System_MemoryMonitor.h"
#include "System_MemUtil.h"
#include "System_Utils.h"

#if ENABLE_GAMEPAD_SENSOR

#include <Adafruit_seesaw.h>
#include <Adafruit_SSD1306.h>
#include <Arduino.h>
#include <Wire.h>
 #include <esp_heap_caps.h>

#include "OLED_Display.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_ESPNow.h"
#include "System_ESPNow_Sensors.h"
#include "System_I2C.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"

// Task handle (owned by this module)
TaskHandle_t gamepadTaskHandle = nullptr;

// Seesaw gamepad object (owned by this module)
Adafruit_seesaw gGamepadSeesaw(&Wire1);

// Debug system provides DEBUG_SENSORSF macro and gDebugFlags via debug_system.h

// External dependencies for task
// sensorStatusBumpWith, gSensorPollingPaused, i2cMutex, drainDebugRing provided by System_I2C.h

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

 static inline void gamepadLogHeap(const char* tag) {
   if (!isDebugFlagSet(DEBUG_MEMORY)) return;
   size_t freeHeap = ESP.getFreeHeap();
   size_t minFree = ESP.getMinFreeHeap();
   size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
   DEBUG_MEMORYF("[GAMEPAD_MEM] %s heap_free=%u heap_min=%u largest=%u",
                 tag ? tag : "?",
                 (unsigned)freeHeap,
                 (unsigned)minFree,
                 (unsigned)largest);
 }

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

// Centralized start handler (defined in System_I2C.cpp)
extern const char* cmd_gamepadstart_queued(const String& cmd);

const char* cmd_gamepadstop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  INFO_SENSORSF("[GAMEPAD] cmd_gamepadstop: Stop requested");
  handleDeviceStopped(I2C_DEVICE_GAMEPAD);
  return "[Gamepad] Stop requested; cleanup will complete asynchronously";
}

const char* cmd_gamepadautostart(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = args;
  arg.trim();
  
  if (arg.length() == 0) {
    return gSettings.gamepadAutoStart ? "[Gamepad] Auto-start: enabled" : "[Gamepad] Auto-start: disabled";
  }
  
  arg.toLowerCase();
  
  if (arg == "on" || arg == "true" || arg == "1") {
    setSetting(gSettings.gamepadAutoStart, true);
    return "[Gamepad] Auto-start enabled";
  } else if (arg == "off" || arg == "false" || arg == "0") {
    setSetting(gSettings.gamepadAutoStart, false);
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

   gamepadLogHeap("start.begin");

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
    gamepadLogHeap("start.init_fail");
    return "Failed to initialize Gamepad";
  }

   gamepadLogHeap("start.after_init");

  // Mark enabled BEFORE task creation to avoid startup race where the task can self-delete
  // if it runs before gamepadEnabled is set.
  bool prev = gamepadEnabled;
  gamepadEnabled = true;
  DEBUG_SENSORSF("[GAMEPAD] startGamepadInternal: Set gamepadEnabled=true (was %d), gamepadConnected=%d", prev, gamepadConnected);
  if (gamepadEnabled != prev) sensorStatusBumpWith("opengamepad@enabled");
  
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
  gamepadLogHeap("start.after_task");
  return "SUCCESS: Gamepad initialized with dedicated task";
}

// ============================================================================
// Gamepad Initialization and Reading Functions
// ============================================================================

bool initGamepad() {
  // If we've already got a connection, consider it initialized
  if (gamepadConnected) {
    DEBUG_SENSORSF("[GAMEPAD] initGamepad: already connected, returning true");
    return true;
  }

  INFO_SENSORSF("[GAMEPAD] initGamepad: starting initialization...");

  // Use device-aware transaction wrapper for safe mutex + clock management + health tracking
  bool initSuccess = i2cDeviceTransaction(I2C_ADDR_GAMEPAD, 100000, 3000, [&]() -> bool {
    // Wire1 already initialized in setup() - no need to call begin() again

    // Attempt to begin Seesaw on 0x50 using Wire1 bus
    if (!gGamepadSeesaw.begin(I2C_ADDR_GAMEPAD)) {
      ERROR_SENSORSF("[GAMEPAD] Seesaw (Gamepad) not found at 0x50 on Wire1");
      return false;
    }

    // Soft reset to ensure clean state - fixes stuck button reads
    DEBUG_SENSORSF("[GAMEPAD] Performing soft reset...");
    gGamepadSeesaw.SWReset();
    delay(10);  // Allow reset to complete
    
    // Re-begin after reset
    if (!gGamepadSeesaw.begin(I2C_ADDR_GAMEPAD)) {
      ERROR_SENSORSF("[GAMEPAD] Seesaw not responding after soft reset");
      return false;
    }

    // Verify product ID (upper 16 bits of getVersion()) should be 5743
    uint32_t version = ((gGamepadSeesaw.getVersion() >> 16) & 0xFFFF);
    INFO_SENSORSF("[GAMEPAD] Seesaw version: %lu (expected 5743)", (unsigned long)version);
    if (version != 5743) {
      WARN_SENSORSF("[GAMEPAD] Seesaw product mismatch: got %lu, expected 5743 (Mini I2C Gamepad)", (unsigned long)version);
      // Not fatal: continue, as other seesaw variants may still be usable
    }

    // Configure gamepad button inputs with pullups and enable GPIO interrupts
    // Use GAMEPAD_BUTTON_MASK from header for consistency
    gGamepadSeesaw.pinModeBulk(GAMEPAD_BUTTON_MASK, INPUT_PULLUP);
    gGamepadSeesaw.setGPIOInterrupts(GAMEPAD_BUTTON_MASK, 1);

    DEBUG_SENSORSF("[GAMEPAD] Seesaw hardware init complete inside lambda");
    return true;
  });

  DEBUG_SENSORSF("[GAMEPAD] i2cDeviceTransaction returned: %d", initSuccess);

  // Set gamepadConnected OUTSIDE the lambda (like IMU pattern)
  if (initSuccess) {
    gamepadConnected = true;
    DEBUG_SENSORSF("[GAMEPAD] SUCCESS: gamepadConnected=%d gamepadEnabled=%d &enabled=%p &connected=%p &cache=%p", 
                  gamepadConnected, gamepadEnabled, (void*)&gamepadEnabled, (void*)&gamepadConnected, (void*)&gControlCache);
    broadcastOutput("Gamepad (Seesaw) initialized");
  } else {
    ERROR_SENSORSF("[GAMEPAD] FAILED: initGamepad returning false");
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
      // Soft reset for clean state, then reconfigure
      i2cDeviceTransactionVoid(I2C_ADDR_GAMEPAD, 100000, 500, [&]() {
        gGamepadSeesaw.SWReset();
      });
      delay(10);
      
      // Re-begin and configure pins after reset
      bool reinit = i2cDeviceTransaction(I2C_ADDR_GAMEPAD, 100000, 500, [&]() -> bool {
        if (!gGamepadSeesaw.begin(I2C_ADDR_GAMEPAD)) return false;
        gGamepadSeesaw.pinModeBulk(GAMEPAD_BUTTON_MASK, INPUT_PULLUP);
        gGamepadSeesaw.setGPIOInterrupts(GAMEPAD_BUTTON_MASK, 1);
        return true;
      });
      if (!reinit) {
        WARN_SENSORSF("[GAMEPAD] Re-init after soft reset failed");
        continue;  // Try next attempt
      }

      // Validate by reading a couple of registers/values
      i2cDeviceTransactionVoid(I2C_ADDR_GAMEPAD, 100000, 500, [&]() {
        (void)gGamepadSeesaw.getVersion();
        (void)gGamepadSeesaw.analogRead(14);
        (void)gGamepadSeesaw.analogRead(15);
      });

      gamepadEnabled = true;
      gamepadConnected = true;
      DEBUG_SENSORSF("[GAMEPAD_DEBUG] initGamepadConnection: &enabled=%p &connected=%p &gControlCache=%p", (void*)&gamepadEnabled, (void*)&gamepadConnected, (void*)&gControlCache);
      
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
    buttons = gGamepadSeesaw.digitalReadBulk(GAMEPAD_BUTTON_MASK);
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
  { "gamepadDevicePollMs", SETTING_INT,  &gSettings.gamepadDevicePollMs, 58, 0, nullptr, 10, 1000, "Poll Interval (ms)", nullptr },
  { "gamepadAutoStart",    SETTING_BOOL, &gSettings.gamepadAutoStart,    1, 0, nullptr, 0, 1, "Auto-start after boot", nullptr }
};

static bool isGamepadConnected() {
  return gamepadConnected;
}

extern const SettingsModule gamepadSettingsModule = {
  "gamepad",
  "gamepad",
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
  { "opengamepad", "Start Seesaw gamepad sensor.", false, cmd_gamepadstart_queued, nullptr, "sensor", "gamepad", "open" },
  { "closegamepad", "Stop Seesaw gamepad sensor.", false, cmd_gamepadstop, nullptr, "sensor", "gamepad", "close" },
  { "gamepadread", "Read Seesaw gamepad state (x/y/buttons).", false, cmd_gamepad },
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
  INFO_SENSORSF("[MODULAR] gamepadTask() running from Sensor_Gamepad_Seesaw.cpp");
  DEBUG_SENSORSF("[GAMEPAD_TASK] Initial state: enabled=%d connected=%d", gamepadEnabled, gamepadConnected);
  gamepadLogHeap("task.entry");
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
      gamepadConnected = false;
      gControlCache.gamepadDataValid = false;
      INFO_SENSORSF("[GAMEPAD_TASK] Task disabled - cleaning up and deleting");
      // NOTE: Do NOT clear gamepadTaskHandle here - let create function use eTaskGetState()
      // to detect stale handles. Clearing here creates a race condition window.
      vTaskDelete(nullptr);
    }

    // Stack watermark tracking + safety bailout
    unsigned long nowMs = millis();
    if ((nowMs - lastStackLog) >= 30000) {
      lastStackLog = nowMs;
      if (checkTaskStackSafety("gamepad", GAMEPAD_STACK_WORDS, &gamepadEnabled)) break;
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

    // Periodic state logging (every 60 seconds)
    if ((nowMs - lastStateLog) >= 60000) {
      lastStateLog = nowMs;
      DEBUG_SENSORSF("[GAMEPAD_TASK] State: enabled=%d connected=%d paused=%d dataValid=%d", 
                    gamepadEnabled, gamepadConnected, gSensorPollingPaused, gControlCache.gamepadDataValid);
    }

    if (gamepadEnabled && gamepadConnected && !gSensorPollingPaused) {
      unsigned long gamepadPollMs = (gSettings.gamepadDevicePollMs > 0) ? (unsigned long)gSettings.gamepadDevicePollMs : 58;
      if ((nowMs - lastGamepadRead) >= gamepadPollMs) {
        bool readSuccess = false;
        uint32_t buttons = 0;
        int rawX = 0, rawY = 0;

        // Seesaw ATSAMD09 supports 400kHz I2C - faster transactions reduce bus hold time.
        // 200ms timeout allows waiting out an OLED display push (~20ms at 400kHz).
        auto result = i2cTaskWithTimeout(I2C_ADDR_GAMEPAD, 400000, 200, [&]() -> bool {
          // Exceptions are disabled (-fno-exceptions), so rely on return value only.
          // Read ONLY button pins, not all 32 GPIO pins - prevents garbage from unconfigured pins
          buttons = gGamepadSeesaw.digitalReadBulk(GAMEPAD_BUTTON_MASK);
          rawX = 1023 - gGamepadSeesaw.analogRead(14);
          rawY = 1023 - gGamepadSeesaw.analogRead(15);
          return true;
        });

        // Honor the actual I2C read result
        readSuccess = (result == true);
        
        // Sanity check: reject garbage reads during I2C bus contention
        // With GAMEPAD_BUTTON_MASK, only bits 0,1,2,5,6,16 should ever be set
        // Any other bits set indicates garbage data
        bool dataValid = true;
        if (readSuccess && (buttons & ~GAMEPAD_BUTTON_MASK) != 0) {
          // Bits outside button mask are set - this is garbage data
          dataValid = false;
        }
        // Reject all-zeros: I2C bus failure (SDA stuck low) reads 0 for all bits,
        // which with active-low buttons means "all pressed" - impossible in reality
        if (readSuccess && (buttons & GAMEPAD_BUTTON_MASK) == 0) {
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
          
          // Button debounce: require 2 consecutive identical reads before accepting a change.
          // This eliminates ghost presses from single I2C bit flips (~58ms added latency).
          static uint32_t pendingButtons = 0xFFFFFFFF;
          static int pendingCount = 0;
          if (buttons != lastButtons) {
            // New value differs from accepted state - check debounce
            if (buttons == pendingButtons) {
              pendingCount++;
            } else {
              pendingButtons = buttons;
              pendingCount = 1;
            }
            if (pendingCount < 2) {
              // Not yet stable - keep old button state, still update joystick below
              buttons = lastButtons;
            }
          } else {
            // Matches accepted state - reset pending
            pendingButtons = buttons;
            pendingCount = 0;
          }
          
          // Debug: Log EVERY button state change immediately
          if (buttons != lastButtons) {
            uint32_t changed = buttons ^ lastButtons;
            uint32_t pressed = ~buttons & changed;   // Bits that went from 1 to 0 (pressed)
            uint32_t released = buttons & changed;   // Bits that went from 0 to 1 (released)
            
            DEBUG_GAMEPAD_DATAF("[GAMEPAD_PRESS] buttons=0x%08lX changed=0x%08lX pressed=0x%08lX released=0x%08lX",
                          (unsigned long)buttons, (unsigned long)changed, 
                          (unsigned long)pressed, (unsigned long)released);
            
            // Log individual button names for clarity
            if (pressed & GAMEPAD_BUTTON_A) DEBUG_GAMEPAD_DATAF("[GAMEPAD_PRESS] A pressed");
            if (pressed & GAMEPAD_BUTTON_B) DEBUG_GAMEPAD_DATAF("[GAMEPAD_PRESS] B pressed");
            if (pressed & GAMEPAD_BUTTON_X) DEBUG_GAMEPAD_DATAF("[GAMEPAD_PRESS] X pressed");
            if (pressed & GAMEPAD_BUTTON_Y) DEBUG_GAMEPAD_DATAF("[GAMEPAD_PRESS] Y pressed");
            if (pressed & GAMEPAD_BUTTON_START) DEBUG_GAMEPAD_DATAF("[GAMEPAD_PRESS] START pressed");
            
            if (released & GAMEPAD_BUTTON_A) DEBUG_GAMEPAD_DATAF("[GAMEPAD_PRESS] A released");
            if (released & GAMEPAD_BUTTON_B) DEBUG_GAMEPAD_DATAF("[GAMEPAD_PRESS] B released");
            if (released & GAMEPAD_BUTTON_X) DEBUG_GAMEPAD_DATAF("[GAMEPAD_PRESS] X released");
            if (released & GAMEPAD_BUTTON_Y) DEBUG_GAMEPAD_DATAF("[GAMEPAD_PRESS] Y released");
            if (released & GAMEPAD_BUTTON_START) DEBUG_GAMEPAD_DATAF("[GAMEPAD_PRESS] START released");
            
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
            // Only increment seq when data actually changes to avoid unnecessary OLED re-renders
            // Use threshold of 2 for joystick to ignore ADC noise/rounding jitter
            bool changed = (gControlCache.gamepadButtons != buttons ||
                            abs(gControlCache.gamepadX - filtX) > 1 ||
                            abs(gControlCache.gamepadY - filtY) > 1);
            gControlCache.gamepadButtons = buttons;  // Only changes after debounce
            gControlCache.gamepadX = filtX;
            gControlCache.gamepadY = filtY;
            gControlCache.gamepadLastUpdate = nowMs;
            gControlCache.gamepadDataValid = true;
            if (changed) gControlCache.gamepadSeq++;
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
                size_t heapBefore = ESP.getFreeHeap();
                size_t largestBefore = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
                {
                  // Send gamepad data via V3 binary protocol
                  extern bool v3_broadcast_sensor_data(RemoteSensorType sensorType, const char* jsonData, uint16_t jsonLen);
                  v3_broadcast_sensor_data(REMOTE_SENSOR_GAMEPAD, gamepadJson, jsonLen);
                }
                if (isDebugFlagSet(DEBUG_MEMORY)) {
                  size_t heapAfter = ESP.getFreeHeap();
                  size_t largestAfter = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
                  long heapDelta = (long)heapBefore - (long)heapAfter;
                  long largestDelta = (long)largestBefore - (long)largestAfter;
                  DEBUG_MEMORYF("[GAMEPAD_MEM] espnow_send heap_delta=%ld largest_delta=%ld", heapDelta, largestDelta);
                }
                
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
          WARN_SENSORSF("[GAMEPAD_TASK] I2C read failure (consecutive: %u)", errors);
          
          if (i2cShouldAutoDisable(I2C_ADDR_GAMEPAD, 5)) {
            ERROR_SENSORSF("[GAMEPAD_TASK] Too many consecutive failures - auto-disabling");
            gamepadEnabled = false;
            gamepadConnected = false;
            DEBUG_GAMEPAD_FRAMEF("Gamepad auto-disabled: %u consecutive I2C failures", errors);
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
// Gamepad Accessor Functions (for MQTT and other modules)
// ============================================================================

int getGamepadX() {
  if (!gamepadConnected || !gControlCache.gamepadDataValid) return 0;
  return gControlCache.gamepadX;
}

int getGamepadY() {
  if (!gamepadConnected || !gControlCache.gamepadDataValid) return 0;
  return gControlCache.gamepadY;
}

uint32_t getGamepadButtons() {
  if (!gamepadConnected || !gControlCache.gamepadDataValid) return 0;
  return gControlCache.gamepadButtons;
}

// ============================================================================
// Gamepad OLED Mode
// ============================================================================
#if DISPLAY_TYPE > 0
#include "i2csensor-seesaw-oled.h"
#endif

#endif // ENABLE_GAMEPAD_SENSOR
