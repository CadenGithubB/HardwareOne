#include "i2csensor_seesaw.h"
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
TaskHandle_t gGamepadTaskHandle = nullptr;

// Seesaw gamepad object (owned by this module)
Adafruit_seesaw gGamepadSeesaw(&Wire1);

// Debug system provides DEBUG_GAMEPADF / INFO_GAMEPADF and gDebugFlags via System_Debug.h

// External dependencies for task
// sensorStatusBumpWith, gSensorPollingPaused, drainDebugRing provided by System_I2C.h

// ============================================================================
// Gamepad/Control Cache (owned by this module)
// ============================================================================
GamepadCache gGamepadCache;

// Gamepad sensor state (definitions)
bool gGamepadEnabled = false;
bool gGamepadConnected = false;
unsigned long gGamepadLastStopTime = 0;

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

const char* cmd_gamepad(const String& argsInput) {
  if (!gGamepadConnected) {
    // Attempt on-demand init with retry/backoff
    if (!gamepadInitConnection()) {
      return "[Gamepad] Error: Not connected - check wiring";
    }
  }
  gamepadPoll();
  return "[Gamepad] Data read complete";
}

// Centralized start handler (defined in System_I2C.cpp)
extern const char* cmd_gamepadstart_queued(const String& argsInput);

const char* cmd_gamepadstop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  INFO_GAMEPAD_LIFECYCLEF("cmd_gamepadstop: Stop requested");
  handleDeviceStopped(I2C_DEVICE_GAMEPAD);
  return "[Gamepad] Stop requested; cleanup will complete asynchronously";
}

const char* cmd_gamepadautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
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

const char* cmd_gamepaddevicepollms(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  if (a.count() == 0) {
    static char buf[48];
    snprintf(buf, sizeof(buf), "[Gamepad] Poll interval: %d ms", gSettings.gamepadDevicePollMs);
    return buf;
  }
  int ms = a.argInt(0, 0);
  if (ms < 10 || ms > 1000) return "Usage: gamepaddevicepollms <10-1000>";
  setSetting(gSettings.gamepadDevicePollMs, ms);
  return "[Gamepad] Poll interval updated";
}

// ============================================================================
// Gamepad Internal Start (called by queue processor)
// ============================================================================

bool gamepadStartInternal() {
  DEBUG_CLIF("[QUEUE] Processing Gamepad start from queue");

   gamepadLogHeap("start.begin");

  // Check memory before creating gamepad task
  if (!checkMemoryAvailable("gamepad", nullptr)) {
    ERROR_GAMEPADF("Insufficient memory for Gamepad sensor");
    return false;
  }

  // Create cache mutex if not already created
  if (!gGamepadCache.mutex) {
    gGamepadCache.mutex = xSemaphoreCreateMutex();
    if (!gGamepadCache.mutex) {
      ERROR_GAMEPADF("Failed to create cache mutex");
      return false;
    }
    DEBUG_GAMEPAD_LIFECYCLEF("[GAMEPAD] Cache mutex created");
  }

  // Clean up any stale cache from previous run BEFORE starting
  {
    SensorCacheGuard g(gGamepadCache.mutex, pdMS_TO_TICKS(100), "gamepad.cleanStaleCache");
    if (g.held) {
      gGamepadCache.gamepadDataValid = false;
      gGamepadCache.gamepadButtons = 0;
      gGamepadCache.gamepadX = 0;
      gGamepadCache.gamepadY = 0;
    }
  }

  // Initialize Seesaw
  if (!gamepadInit()) {
    gamepadLogHeap("start.init_fail");
    ERROR_GAMEPADF("Failed to initialize Gamepad");
    return false;
  }

   gamepadLogHeap("start.after_init");

  // Mark enabled BEFORE task creation to avoid startup race where the task can self-delete
  // if it runs before gGamepadEnabled is set.
  bool prev = gGamepadEnabled;
  gGamepadEnabled = true;
  DEBUG_GAMEPAD_LIFECYCLEF("[GAMEPAD] gamepadStartInternal: Set gGamepadEnabled=true (was %d), gGamepadConnected=%d", prev, gGamepadConnected);
  if (gGamepadEnabled != prev) sensorStatusBumpWith("opengamepad@enabled");

  // Broadcast sensor status to ESP-NOW master
#if ENABLE_ESPNOW
  broadcastSensorStatus(REMOTE_SENSOR_GAMEPAD, true);
#endif

  // Create dedicated gamepad task
  if (!createGamepadTask()) {
    ERROR_GAMEPADF("Failed to create Gamepad task");
    return false;
  }
  gamepadLogHeap("start.after_task");
  return true;
}

// ============================================================================
// Gamepad Initialization and Reading Functions
// ============================================================================

bool gamepadInit() {
  // If we've already got a connection, consider it initialized
  if (gGamepadConnected) {
    DEBUG_GAMEPAD_LIFECYCLEF("[GAMEPAD] gamepadInit: already connected, returning true");
    return true;
  }

  INFO_GAMEPAD_LIFECYCLEF("gamepadInit: starting initialization...");

  // Use device-aware transaction wrapper for safe mutex + clock management + health tracking
  bool initSuccess = i2cDeviceTransaction(I2C_ADDR_GAMEPAD, 100000, 3000, [&]() -> bool {
    // Wire1 already initialized in setup() - no need to call begin() again

    if (!gGamepadSeesaw.begin(I2C_ADDR_GAMEPAD)) {
      ERROR_GAMEPADF("Seesaw (Gamepad) not found at 0x%02X on Wire1", I2C_ADDR_GAMEPAD);
      return false;
    }

    // Soft reset to ensure clean state - fixes stuck button reads
    DEBUG_GAMEPAD_LIFECYCLEF("[GAMEPAD] Performing soft reset...");
    gGamepadSeesaw.SWReset();
    delay(10);  // Allow reset to complete
    
    // Re-begin after reset
    if (!gGamepadSeesaw.begin(I2C_ADDR_GAMEPAD)) {
      ERROR_GAMEPADF("Seesaw not responding after soft reset");
      return false;
    }

    // Verify product ID (upper 16 bits of getVersion()) should be 5743
    uint32_t version = ((gGamepadSeesaw.getVersion() >> 16) & 0xFFFF);
    INFO_GAMEPAD_LIFECYCLEF("Seesaw version: %lu (expected 5743)", (unsigned long)version);
    if (version != 5743) {
      WARN_GAMEPADF("Seesaw product mismatch: got %lu, expected 5743 (Mini I2C Gamepad)", (unsigned long)version);
      // Not fatal: continue, as other seesaw variants may still be usable
    }

    // Configure gamepad button inputs with pullups and enable GPIO interrupts
    // Use GAMEPAD_BUTTON_MASK from header for consistency
    gGamepadSeesaw.pinModeBulk(GAMEPAD_BUTTON_MASK, INPUT_PULLUP);
    gGamepadSeesaw.setGPIOInterrupts(GAMEPAD_BUTTON_MASK, 1);

    DEBUG_GAMEPAD_LIFECYCLEF("[GAMEPAD] Seesaw hardware init complete inside lambda");
    return true;
  });

  DEBUG_GAMEPAD_LIFECYCLEF("[GAMEPAD] i2cDeviceTransaction returned: %d", initSuccess);

  // Set gGamepadConnected OUTSIDE the lambda (like IMU pattern)
  if (initSuccess) {
    gGamepadConnected = true;
    DEBUG_GAMEPAD_LIFECYCLEF("[GAMEPAD] SUCCESS: gGamepadConnected=%d gGamepadEnabled=%d &enabled=%p &connected=%p &cache=%p",
                  gGamepadConnected, gGamepadEnabled, (void*)&gGamepadEnabled, (void*)&gGamepadConnected, (void*)&gGamepadCache);
    broadcastOutput("Gamepad (Seesaw) initialized");
  } else {
    ERROR_GAMEPADF("FAILED: gamepadInit returning false");
  }

  return initSuccess;
}

// Helper function for I2C ping
static bool i2cPing(TwoWire* bus, uint8_t addr) {
  (void)bus;
  return i2cPingAddress(addr, 100000, 200);
}

bool gamepadInitConnection() {
  if (gGamepadConnected) return true;
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
    if (i2cPing(&Wire1, I2C_ADDR_GAMEPAD)) {
      seen = true;
      break;
    }
    delay(5);
  }
  if (!seen) {
    WARN_GAMEPADF("Gamepad: no ACK at 0x%02X", I2C_ADDR_GAMEPAD);
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
        WARN_GAMEPADF("Re-init after soft reset failed");
        continue;  // Try next attempt
      }

      // Validate by reading a couple of registers/values
      i2cDeviceTransactionVoid(I2C_ADDR_GAMEPAD, 100000, 500, [&]() {
        (void)gGamepadSeesaw.getVersion();
        (void)gGamepadSeesaw.analogRead(14);
        (void)gGamepadSeesaw.analogRead(15);
      });

      gGamepadEnabled = true;
      gGamepadConnected = true;
      DEBUG_GAMEPAD_LIFECYCLEF("[GAMEPAD_DEBUG] gamepadInitConnection: &enabled=%p &connected=%p &gGamepadCache=%p", (void*)&gGamepadEnabled, (void*)&gGamepadConnected, (void*)&gGamepadCache);
      
      INFO_GAMEPAD_LIFECYCLEF("Gamepad connected on attempt %d", attempt);
      snprintf(msg, sizeof(msg), "Gamepad: re-init success (attempt %d)", attempt);
      broadcastOutput(msg);
      return true;
    }
    INFO_GAMEPAD_LIFECYCLEF("Gamepad attempt %d failed, retrying", attempt);
    snprintf(msg, sizeof(msg), "Gamepad: attempt %d failed", attempt);
    broadcastOutput(msg);
    delay(15);
  }
  broadcastOutput("Gamepad: re-init failed after retries");
  return false;
}

void gamepadPoll() {
  if (!gGamepadConnected) {
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

  BROADCAST_PRINTF("Buttons: 0x%lX, X: %d, Y: %d", (unsigned long)buttons, x, y);
}

// ============================================================================
// Gamepad Modular Settings Registration (for safety and reliability)
// ============================================================================

// Gamepad settings entries - minimal but essential for safety
// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry gamepadSettingEntries[] = {
  { "gamepadDevicePollMs", SETTING_INT, &gSettings.gamepadDevicePollMs, 90, 0, nullptr, 10, 1000, "Poll Interval (ms)", nullptr, false, nullptr, nullptr },
  { "gamepadAutoStart", SETTING_BOOL, &gSettings.gamepadAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr, false, nullptr, nullptr }
};

static bool isGamepadConnected() {
  return gGamepadConnected;
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule gamepadSettingsModule = {
  "gamepad",
  "hardware.sensors.gamepad",
  gamepadSettingEntries,
  sizeof(gamepadSettingEntries) / sizeof(gamepadSettingEntries[0]),
  isGamepadConnected,
  "Seesaw mini gamepad"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================
// Gamepad Command Registry
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry gamepadCommands[] = {
  { "opengamepad", "Start Seesaw gamepad sensor.", false, cmd_gamepadstart_queued, nullptr, "sensor", "gamepad", "open" },
  { "closegamepad", "Stop Seesaw gamepad sensor.", false, cmd_gamepadstop, nullptr, "sensor", "gamepad", "close" },
  { "gamepadread", "Read Seesaw gamepad state (x/y/buttons).", false, cmd_gamepad },
  { "gamepadautostart", "Enable/disable gamepad auto-start after boot [on|off]", false, cmd_gamepadautostart, "Usage: gamepadautostart [on|off]" },
  { "gamepaddevicepollms", "Set gamepad poll interval ms [10-1000]", true, cmd_gamepaddevicepollms },
};

const size_t gamepadCommandsCount = sizeof(gamepadCommands) / sizeof(gamepadCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// Gamepad Task Implementation (moved from i2c_system.cpp for full modularization)
// ============================================================================

// ============================================================================
// Gamepad Task - FreeRTOS Task Function
// ============================================================================
// Purpose: Continuously reads button and joystick state from Seesaw gamepad
// Stack: 4096 words (~16KB) | Priority: 1 | Core: Any
// Lifecycle: Created by cmd_gamepadstart, deleted when gGamepadEnabled=false
// Polling: Fixed 50ms interval | I2C Clock: 100kHz
//
// Cleanup Strategy:
//   1. Check gGamepadEnabled flag at loop start
//   2. Acquire bus mutex via I2CDeviceManager to prevent race conditions during cleanup
//   3. Invalidate cache (no sensor object to delete)
//   4. Release mutex and delete task
// ============================================================================

void gamepadTask(void* parameter) {
  INFO_GAMEPAD_LIFECYCLEF("Task started (handle=%p, stack=%u words)", 
                (void*)xTaskGetCurrentTaskHandle(), 
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  INFO_GAMEPAD_LIFECYCLEF("[MODULAR] gamepadTask() running from Sensor_Gamepad_Seesaw.cpp");
  DEBUG_GAMEPAD_LIFECYCLEF("[GAMEPAD_TASK] Initial state: enabled=%d connected=%d", gGamepadEnabled, gGamepadConnected);
  gamepadLogHeap("task.entry");
  unsigned long lastGamepadRead = 0;
  unsigned long lastStackLog = 0;
  unsigned long lastStateLog = 0;
  // Note: I2C transaction failure tracking handled by centralized I2CDevice health system.
  // Invalid-data tracking (I2C succeeds but returns garbage) needs a local counter
  // because the health system only tracks transport-level failures.
  uint8_t consecutiveInvalidReads = 0;
  static const uint8_t INVALID_READ_AUTO_DISABLE_THRESHOLD = 20;  // ~1.2s at 58ms poll

  // EWMA smoothing state
  int filtX = -1, filtY = -1;
  const float kAlpha = 0.7f;

  while (true) {
    // CRITICAL: Check enabled flag FIRST for graceful shutdown
    if (!gGamepadEnabled) {
      gGamepadConnected = false;
      gGamepadCache.gamepadDataValid = false;
      SENSOR_TASK_EXIT(GAMEPAD);
    }

    // Stack watermark tracking + safety bailout
    unsigned long nowMs = millis();
    if ((nowMs - lastStackLog) >= 30000) {
      lastStackLog = nowMs;
      if (checkTaskStackSafety("gamepad", GAMEPAD_STACK_WORDS, &gGamepadEnabled)) break;
      if (isDebugFlagSet(DEBUG_PERFORMANCE)) {
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
        gGamepadWatermarkNow = watermark;
        if (watermark < gGamepadWatermarkMin) {
          gGamepadWatermarkMin = watermark;
        }
        DEBUG_PERFORMANCEF("[STACK] gamepad_task watermark_now=%u min=%u words", (unsigned)gGamepadWatermarkNow, (unsigned)gGamepadWatermarkMin);
      }
      if (isDebugFlagSet(DEBUG_MEMORY)) {
        DEBUG_MEMORY_HEAPF("[HEAP] gamepad_task: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }

    // Periodic state logging (every 60 seconds)
    if ((nowMs - lastStateLog) >= 60000) {
      lastStateLog = nowMs;
      DEBUG_GAMEPAD_POLLINGF("[GAMEPAD_TASK] State: enabled=%d connected=%d paused=%d dataValid=%d",
                    gGamepadEnabled, gGamepadConnected, gSensorPollingPaused, gGamepadCache.gamepadDataValid);
    }

    if (gGamepadEnabled && gGamepadConnected && !gSensorPollingPaused) {
      unsigned long gamepadPollMs = (gSettings.gamepadDevicePollMs > 0) ? (unsigned long)gSettings.gamepadDevicePollMs : 90;
      if ((nowMs - lastGamepadRead) >= gamepadPollMs) {
        bool readSuccess = false;
        uint32_t buttons = 0;
        uint32_t intflags = 0;  // Hardware edge-latch: pins that had any transition since last read
        int rawX = 0, rawY = 0;

        // Seesaw ATSAMD09 supports 400kHz I2C - faster transactions reduce bus hold time.
        // 80ms timeout: must be < Wire1.setTimeOut(100ms) so Wire aborts cleanly first.
        // 3 reads × 80ms = 240ms worst case, well under CONFIG_ESP_INT_WDT_TIMEOUT_MS (1500ms).
        auto result = i2cTaskWithTimeout(I2C_ADDR_GAMEPAD, 400000, 80, [&]() -> bool {
          // Exceptions are disabled (-fno-exceptions), so rely on return value only.
          // Read ONLY button pins, not all 32 GPIO pins - prevents garbage from unconfigured pins
          buttons = gGamepadSeesaw.digitalReadBulk(GAMEPAD_BUTTON_MASK);
          // Read and clear the hardware interrupt flag register. The ATSAMD09 latches every
          // edge event between polls (read-to-clear), so a press+release inside one 90ms window
          // still shows up here even though BULK already shows the button as released.
          intflags = gGamepadSeesaw.digitalReadBulkIntFlag(GAMEPAD_BUTTON_MASK);
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
          consecutiveInvalidReads = 0;  // Reset invalid-data counter on good read
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
            
            DEBUG_GAMEPAD_VALUESF("[GAMEPAD_PRESS] buttons=0x%08lX changed=0x%08lX pressed=0x%08lX released=0x%08lX",
                          (unsigned long)buttons, (unsigned long)changed, 
                          (unsigned long)pressed, (unsigned long)released);
            
            // Log individual button names for clarity
            if (pressed & GAMEPAD_BUTTON_A) DEBUG_GAMEPAD_VALUESF("[GAMEPAD_PRESS] A pressed");
            if (pressed & GAMEPAD_BUTTON_B) DEBUG_GAMEPAD_VALUESF("[GAMEPAD_PRESS] B pressed");
            if (pressed & GAMEPAD_BUTTON_X) DEBUG_GAMEPAD_VALUESF("[GAMEPAD_PRESS] X pressed");
            if (pressed & GAMEPAD_BUTTON_Y) DEBUG_GAMEPAD_VALUESF("[GAMEPAD_PRESS] Y pressed");
            if (pressed & GAMEPAD_BUTTON_START) DEBUG_GAMEPAD_VALUESF("[GAMEPAD_PRESS] START pressed");
            
            if (released & GAMEPAD_BUTTON_A) DEBUG_GAMEPAD_VALUESF("[GAMEPAD_PRESS] A released");
            if (released & GAMEPAD_BUTTON_B) DEBUG_GAMEPAD_VALUESF("[GAMEPAD_PRESS] B released");
            if (released & GAMEPAD_BUTTON_X) DEBUG_GAMEPAD_VALUESF("[GAMEPAD_PRESS] X released");
            if (released & GAMEPAD_BUTTON_Y) DEBUG_GAMEPAD_VALUESF("[GAMEPAD_PRESS] Y released");
            if (released & GAMEPAD_BUTTON_START) DEBUG_GAMEPAD_VALUESF("[GAMEPAD_PRESS] START released");
            
            lastButtons = buttons;
          }
          
          if (filtX < 0 || filtY < 0) {
            filtX = rawX;
            filtY = rawY;
          } else {
            filtX = (int)lroundf(kAlpha * rawX + (1.0f - kAlpha) * filtX);
            filtY = (int)lroundf(kAlpha * rawY + (1.0f - kAlpha) * filtY);
          }

          {
            SensorCacheGuard g(gGamepadCache.mutex, pdMS_TO_TICKS(50), "gamepad.pollWrite");
            if (g.held) {
            // Only increment seq when data actually changes to avoid unnecessary OLED re-renders
            // Use threshold of 2 for joystick to ignore ADC noise/rounding jitter
            bool changed = (gGamepadCache.gamepadButtons != buttons ||
                            abs(gGamepadCache.gamepadX - filtX) > 1 ||
                            abs(gGamepadCache.gamepadY - filtY) > 1);
            // Latch press edges into the accumulator so the UI never misses a tap.
            // Active-low: 1 = unpressed, 0 = pressed.
            //
            // Two sources of press evidence:
            //
            // 1. Debounce-confirmed press: button state changed and debounce accepted it.
            //    Covers normal (held) presses where the button is still down this poll.
            uint32_t confirmedPressed = gGamepadCache.gamepadButtons & ~buttons;
            //
            // 2. INTFLAG-based press: hardware edge-latch fired this poll interval.
            //    Covers quick taps released before the second confirmation poll — the
            //    debounce filter would discard these entirely, but INTFLAG saw the edge.
            //    Condition: intflag set AND (currently pressed OR was previously unpressed).
            //    The "was previously unpressed" arm catches press+release within one interval.
            //    The "currently pressed" arm catches first-detected holds (belt-and-suspenders).
            uint32_t intflagPressed = intflags & (~buttons | lastButtons);
            //
            gGamepadCache.buttonPressedAccum |= confirmedPressed | intflagPressed;
            gGamepadCache.gamepadButtons = buttons;  // Only changes after debounce
            gGamepadCache.gamepadX = filtX;
            gGamepadCache.gamepadY = filtY;
            gGamepadCache.gamepadLastUpdate = nowMs;
            gGamepadCache.gamepadDataValid = true;
            if (changed) gGamepadCache.gamepadSeq++;
            }
          }  // gamepad guard releases here, before the ESP-NOW streaming below

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
                  // Send gamepad data via bond or mesh (sendSensorDataUpdate handles routing)
                  extern void sendSensorDataUpdate(RemoteSensorType sensorType, const char* jsonData, size_t jsonLen);
                  sendSensorDataUpdate(REMOTE_SENSOR_GAMEPAD, gamepadJson, jsonLen);
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
          WARN_GAMEPADF("[GAMEPAD_TASK] I2C read failure (consecutive: %u)", errors);
          
          if (i2cShouldAutoDisable(I2C_ADDR_GAMEPAD)) {
            ERROR_GAMEPADF("[GAMEPAD_TASK] Too many consecutive failures - auto-disabling");
            handleDeviceStopped(I2C_DEVICE_GAMEPAD);
            DEBUG_GAMEPAD_LIFECYCLEF("Gamepad auto-disabled: %u consecutive I2C failures", errors);
            sensorStatusBumpWith("gamepad@auto_disabled");
          }
        } else {
          // I2C succeeded but data validation failed (garbage data during bus contention)
          consecutiveInvalidReads++;
          if (consecutiveInvalidReads == 10) {
            WARN_GAMEPADF("[GAMEPAD_TASK] 10 consecutive invalid reads - device may be disconnected");
          }
          if (consecutiveInvalidReads >= INVALID_READ_AUTO_DISABLE_THRESHOLD) {
            ERROR_GAMEPADF("[GAMEPAD_TASK] %u consecutive invalid reads - auto-disabling gamepad", consecutiveInvalidReads);
            handleDeviceStopped(I2C_DEVICE_GAMEPAD);
            sensorStatusBumpWith("gamepad@invalid_data_disabled");
          }
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

int gamepadGetX() {
  if (!gGamepadConnected || !gGamepadCache.gamepadDataValid) return 0;
  return gGamepadCache.gamepadX;
}

int gamepadGetY() {
  if (!gGamepadConnected || !gGamepadCache.gamepadDataValid) return 0;
  return gGamepadCache.gamepadY;
}

uint32_t gamepadGetButtons() {
  if (!gGamepadConnected || !gGamepadCache.gamepadDataValid) return 0;
  return gGamepadCache.gamepadButtons;
}

// ============================================================================
// Gamepad OLED Mode
// ============================================================================
#if DISPLAY_TYPE > 0
#include "i2csensor_seesaw_oled.h"
#endif

#endif // ENABLE_GAMEPAD_SENSOR
