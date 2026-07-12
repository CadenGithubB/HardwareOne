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
TaskHandle_t gInputTaskHandle = nullptr;

// Seesaw gamepad object — heap-allocated at gamepadInit with the TwoWire*
// for the gamepad's configured bus. Was a stack global with hardcoded
// `&Wire1` pre-dual-bus; the library binds the TwoWire at construction so
// we defer until gamepadBus is resolved. All callsites use `->` and check
// for nullptr.
Adafruit_seesaw* gGamepadSeesaw = nullptr;

// Resolve the gamepad's bus + TwoWire* from settings. Returns false if the
// bus isn't initialized (e.g., gamepadBus=1 but i2c2BusEnabled=false).
static bool gamepadResolveBus(uint8_t* outBus, TwoWire** outWire) {
  const uint8_t bus = (uint8_t)gSettings.inputBus;
  TwoWire* w = i2c() ? i2c()->getWire(bus) : nullptr;
  if (!w) return false;
  *outBus = bus;
  *outWire = w;
  return true;
}

// Debug system provides DEBUG_INPUTF / INFO_INPUTF and gDebugFlags via System_Debug.h

// External dependencies for task
// sensorStatusBumpWith, gSensorPollingPaused, drainDebugRing provided by System_I2C.h

// ============================================================================
// Gamepad/Control Cache (owned by this module)
// ============================================================================
InputCache gInputCache;

// Gamepad sensor state (definitions)
bool gInputEnabled = false;
bool gInputConnected = false;
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
  if (argWantsJson(argsInput)) {
    if (!ensureDebugBuffer()) return "{\"valid\":false,\"error\":\"buffer\"}";
    int n = gamepadBuildDataJSON(getDebugBuffer(), 1024);  // shared builder (also feeds sensors json)
    return (n > 0) ? getDebugBuffer() : "{\"valid\":false}";
  }
  if (!gInputConnected) {
    // Attempt on-demand init with retry/backoff
    if (!gamepadInitConnection()) {
      cliHint("to start the gamepad, run 'openinput'; if it is still missing, confirm the address with 'i2cscan'");
      return "Error: [Gamepad] Not connected - check wiring";
    }
  }
  gamepadPoll();
  return "[Gamepad] Data read complete";
}

// cmd_openinput / cmd_closeinput / cmd_inputautostart / cmd_inputdevicepollms
// all live in HAL_Input.cpp — they work for either driver. Only driver-specific
// CLI (gamepadread for raw debug dump) stays here.

// ============================================================================
// Input device Internal Start (called by queue processor under ENABLE_GAMEPAD_SENSOR)
// ============================================================================

bool inputStartInternal() {
  DEBUG_CLIF("[QUEUE] Processing Gamepad start from queue");

   gamepadLogHeap("start.begin");

  // Check memory before creating gamepad task
  if (!checkMemoryAvailable("gamepad", nullptr)) {
    ERROR_INPUTF("Insufficient memory for Gamepad sensor");
    return false;
  }

  // Create cache mutex if not already created
  if (!gInputCache.mutex) {
    gInputCache.mutex = xSemaphoreCreateMutex();
    if (!gInputCache.mutex) {
      ERROR_INPUTF("Failed to create cache mutex");
      return false;
    }
    DEBUG_INPUT_LIFECYCLEF("[GAMEPAD] Cache mutex created");
  }

  // Clean up any stale cache from previous run BEFORE starting
  {
    SensorCacheGuard g(gInputCache.mutex, pdMS_TO_TICKS(100), "gamepad.cleanStaleCache");
    if (g.held) {
      gInputCache.dataValid = false;
      gInputCache.buttons = 0;
      gInputCache.joyX = 0;
      gInputCache.joyY = 0;
    }
  }

  // Initialize Seesaw
  if (!gamepadInit()) {
    gamepadLogHeap("start.init_fail");
    ERROR_INPUTF("Failed to initialize Gamepad");
    return false;
  }

   gamepadLogHeap("start.after_init");

  // Mark enabled BEFORE task creation to avoid startup race where the task can self-delete
  // if it runs before gInputEnabled is set.
  bool prev = gInputEnabled;
  gInputEnabled = true;
  DEBUG_INPUT_LIFECYCLEF("[GAMEPAD] inputStartInternal: Set gInputEnabled=true (was %d), gInputConnected=%d", prev, gInputConnected);
  if (gInputEnabled != prev) sensorStatusBumpWith("opengamepad@enabled");

  // Broadcast sensor status to ESP-NOW master
#if ENABLE_ESPNOW
  broadcastSensorStatus(REMOTE_SENSOR_INPUT, true);
#endif

  // Create dedicated gamepad task
  if (!createInputTask()) {
    ERROR_INPUTF("Failed to create Gamepad task");
    return false;
  }
  gamepadLogHeap("start.after_task");
  return true;
}

// ============================================================================
// Gamepad Initialization and Reading Functions
// ============================================================================

bool gamepadInit() {
  // If we've already got a connection AND the seesaw object exists, done.
  if (gInputConnected && gGamepadSeesaw) {
    DEBUG_INPUT_LIFECYCLEF("[GAMEPAD] gamepadInit: already connected, returning true");
    return true;
  }

  INFO_INPUT_LIFECYCLEF("gamepadInit: starting initialization...");

  // Resolve which bus the gamepad lives on (gSettings.inputBus). The
  // Adafruit_seesaw library binds the TwoWire at construction, so we must
  // allocate the object here with the right pointer — there's no setBus()
  // after the fact. Bail closed if the bus isn't initialized.
  uint8_t gpBus; TwoWire* gpWire;
  if (!gamepadResolveBus(&gpBus, &gpWire)) {
    ERROR_INPUTF("gamepad bus %d not initialized — check i2c2BusEnabled if gamepadBus=1",
                   gSettings.inputBus);
    return false;
  }

  // Allocate the seesaw library object on the heap if we haven't yet.
  // Kept across re-init attempts so the library's internal state survives.
  if (!gGamepadSeesaw) {
    gGamepadSeesaw = new Adafruit_seesaw(gpWire);
    if (!gGamepadSeesaw) {
      ERROR_INPUTF("Failed to allocate Adafruit_seesaw");
      return false;
    }
    DEBUG_INPUT_LIFECYCLEF("[GAMEPAD] Allocated Adafruit_seesaw at %p on bus %u",
                             (void*)gGamepadSeesaw, gpBus);
  }

  // Use bus-aware device transaction wrapper for safe mutex + clock + health tracking
  bool initSuccess = i2cDeviceTransaction(gpBus, I2C_ADDR_GAMEPAD, 100000, 3000, [&]() -> bool {
    // Library's wire is already bound from construction above

    if (!gGamepadSeesaw->begin(I2C_ADDR_GAMEPAD)) {
      ERROR_INPUTF("Seesaw (Gamepad) not found at 0x%02X on bus %u", I2C_ADDR_GAMEPAD, gpBus);
      return false;
    }

    // Soft reset to ensure clean state - fixes stuck button reads
    DEBUG_INPUT_LIFECYCLEF("[GAMEPAD] Performing soft reset...");
    gGamepadSeesaw->SWReset();
    delay(10);  // Allow reset to complete

    // Re-begin after reset
    if (!gGamepadSeesaw->begin(I2C_ADDR_GAMEPAD)) {
      ERROR_INPUTF("Seesaw not responding after soft reset");
      return false;
    }

    // Verify product ID (upper 16 bits of getVersion()) should be 5743
    uint32_t version = ((gGamepadSeesaw->getVersion() >> 16) & 0xFFFF);
    INFO_INPUT_LIFECYCLEF("Seesaw version: %lu (expected 5743)", (unsigned long)version);
    if (version != 5743) {
      WARN_INPUTF("Seesaw product mismatch: got %lu, expected 5743 (Mini I2C Gamepad)", (unsigned long)version);
      // Not fatal: continue, as other seesaw variants may still be usable
    }

    // Configure gamepad button inputs with pullups and enable GPIO interrupts
    // Use GAMEPAD_BUTTON_MASK from header for consistency
    gGamepadSeesaw->pinModeBulk(GAMEPAD_BUTTON_MASK, INPUT_PULLUP);
    gGamepadSeesaw->setGPIOInterrupts(GAMEPAD_BUTTON_MASK, 1);

    DEBUG_INPUT_LIFECYCLEF("[GAMEPAD] Seesaw hardware init complete inside lambda");
    return true;
  });

  DEBUG_INPUT_LIFECYCLEF("[GAMEPAD] i2cDeviceTransaction returned: %d", initSuccess);

  // Set gInputConnected OUTSIDE the lambda (like IMU pattern)
  if (initSuccess) {
    gInputConnected = true;
    DEBUG_INPUT_LIFECYCLEF("[GAMEPAD] SUCCESS: gInputConnected=%d gInputEnabled=%d &enabled=%p &connected=%p &cache=%p",
                  gInputConnected, gInputEnabled, (void*)&gInputEnabled, (void*)&gInputConnected, (void*)&gInputCache);
    broadcastOutput("Gamepad (Seesaw) initialized");
  } else {
    ERROR_INPUTF("FAILED: gamepadInit returning false");
  }

  return initSuccess;
}

// Helper function for I2C ping — bus-aware.
// The pre-dual-bus signature took TwoWire* which it then ignored (always
// pinged via Wire1). Now takes a bus index and routes through the bus-aware
// i2cPingAddress, so the ping uses the right mutex + Wire.
static bool i2cPing(uint8_t bus, uint8_t addr) {
  return i2cPingAddress(addr, 100000, 200, bus);
}

bool gamepadInitConnection() {
  if (gInputConnected && gGamepadSeesaw) return true;
  unsigned long now = millis();
  if (now - gLastGamepadInitMs < kGamepadInitMinIntervalMs) {
    broadcastOutput("Gamepad: skipping re-init (backoff window)");
    return false;  // backoff window
  }
  gLastGamepadInitMs = now;
  broadcastOutput("Gamepad: attempting re-init");

  // Resolve bus + allocate library object if not done yet.
  uint8_t gpBus; TwoWire* gpWire;
  if (!gamepadResolveBus(&gpBus, &gpWire)) {
    WARN_INPUTF("Gamepad: bus %d not initialized", gSettings.inputBus);
    return false;
  }
  if (!gGamepadSeesaw) {
    gGamepadSeesaw = new Adafruit_seesaw(gpWire);
    if (!gGamepadSeesaw) {
      WARN_INPUTF("Gamepad: failed to alloc Adafruit_seesaw");
      return false;
    }
  }

  // Quick ping first to avoid costly begin() if device not present
  bool seen = false;
  for (int p = 0; p < 2; ++p) {
    if (i2cPing(gpBus, I2C_ADDR_GAMEPAD)) {
      seen = true;
      break;
    }
    delay(5);
  }
  if (!seen) {
    WARN_INPUTF("Gamepad: no ACK at 0x%02X on bus %u", I2C_ADDR_GAMEPAD, gpBus);
    broadcastOutput("Gamepad: no ACK at 0x50");
    return false;
  }

  // Try up to 3 begin attempts with small delays
  for (int attempt = 1; attempt <= 3; ++attempt) {
    char msg[48];
    snprintf(msg, sizeof(msg), "Gamepad: re-init attempt %d", attempt);
    broadcastOutput(msg);
    bool began = i2cDeviceTransaction(gpBus, I2C_ADDR_GAMEPAD, 100000, 500, [&]() -> bool {
      return gGamepadSeesaw->begin(I2C_ADDR_GAMEPAD);
    });
    if (began) {
      // Soft reset for clean state, then reconfigure
      i2cDeviceTransactionVoid(gpBus, I2C_ADDR_GAMEPAD, 100000, 500, [&]() {
        gGamepadSeesaw->SWReset();
      });
      delay(10);

      // Re-begin and configure pins after reset
      bool reinit = i2cDeviceTransaction(gpBus, I2C_ADDR_GAMEPAD, 100000, 500, [&]() -> bool {
        if (!gGamepadSeesaw->begin(I2C_ADDR_GAMEPAD)) return false;
        gGamepadSeesaw->pinModeBulk(GAMEPAD_BUTTON_MASK, INPUT_PULLUP);
        gGamepadSeesaw->setGPIOInterrupts(GAMEPAD_BUTTON_MASK, 1);
        return true;
      });
      if (!reinit) {
        WARN_INPUTF("Re-init after soft reset failed");
        continue;  // Try next attempt
      }

      // Validate by reading a couple of registers/values
      i2cDeviceTransactionVoid(gpBus, I2C_ADDR_GAMEPAD, 100000, 500, [&]() {
        (void)gGamepadSeesaw->getVersion();
        (void)gGamepadSeesaw->analogRead(14);
        (void)gGamepadSeesaw->analogRead(15);
      });

      gInputEnabled = true;
      gInputConnected = true;
      DEBUG_INPUT_LIFECYCLEF("[GAMEPAD_DEBUG] gamepadInitConnection: &enabled=%p &connected=%p &gInputCache=%p", (void*)&gInputEnabled, (void*)&gInputConnected, (void*)&gInputCache);
      
      INFO_INPUT_LIFECYCLEF("Gamepad connected on attempt %d", attempt);
      snprintf(msg, sizeof(msg), "Gamepad: re-init success (attempt %d)", attempt);
      broadcastOutput(msg);
      return true;
    }
    INFO_INPUT_LIFECYCLEF("Gamepad attempt %d failed, retrying", attempt);
    snprintf(msg, sizeof(msg), "Gamepad: attempt %d failed", attempt);
    broadcastOutput(msg);
    delay(15);
  }
  broadcastOutput("Gamepad: re-init failed after retries");
  return false;
}

void gamepadPoll() {
  if (!gInputConnected || !gGamepadSeesaw) {
    broadcastOutput("Gamepad not connected. Check wiring.");
    return;
  }

  uint32_t buttons = 0;
  int16_t x = 0, y = 0;

  // Bus-aware transaction wrapper to prevent bus contention. The gamepad's
  // bus is fixed at gamepadInit time and stored in gSettings.inputBus.
  const uint8_t gpBus = (uint8_t)gSettings.inputBus;
  i2cDeviceTransactionVoid(gpBus, I2C_ADDR_GAMEPAD, 100000, 200, [&]() {
    buttons = gGamepadSeesaw->digitalReadBulk(GAMEPAD_BUTTON_MASK);
    x = gGamepadSeesaw->analogRead(14);
    y = gGamepadSeesaw->analogRead(15);
  });

  BROADCAST_PRINTF("Buttons: 0x%lX, X: %d, Y: %d", (unsigned long)buttons, x, y);
}

// ============================================================================
// Gamepad-specific Command Registry — only commands that dump raw seesaw
// state (debug). Open/close/autostart/poll moved to HAL_Input.cpp.
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry gamepadCommands[] = {
  { "gamepadread", "Read Seesaw gamepad state (x/y/buttons). (add 'json' for JSON output)", false, cmd_gamepad },
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
// Lifecycle: Created by cmd_gamepadstart, deleted when gInputEnabled=false
// Polling: Fixed 50ms interval | I2C Clock: 100kHz
//
// Cleanup Strategy:
//   1. Check gInputEnabled flag at loop start
//   2. Acquire bus mutex via I2CDeviceManager to prevent race conditions during cleanup
//   3. Invalidate cache (no sensor object to delete)
//   4. Release mutex and delete task
// ============================================================================

// Build gamepad JSON directly into buffer from cache. Safe to call from any task —
// only reads gInputCache under its own mutex. Returns bytes written or 0.
int gamepadBuildDataJSON(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;

  SensorCacheGuard g(gInputCache.mutex, pdMS_TO_TICKS(50), "gamepad.buildJSON");
  if (!g.held) {
    // Cache-lock timeout: not-ready envelope (was: return 0 / no output).
    int pos = sensorEnvelopeBegin(buf, bufSize, false, gInputConnected, 0);
    if (pos == 0) return 0;
    int n = snprintf(buf + pos, bufSize - pos, ",\"x\":0,\"y\":0,\"buttons\":0}");
    if (n < 0 || (size_t)n >= bufSize - pos) return 0;
    return pos + n;
  }

  int pos = sensorEnvelopeBegin(buf, bufSize, gInputCache.dataValid, gInputConnected, gInputCache.lastUpdate);
  if (pos == 0) return 0;
  int n = snprintf(buf + pos, bufSize - pos,
                   ",\"x\":%d,\"y\":%d,\"buttons\":%lu}",
                   gInputCache.joyX, gInputCache.joyY,
                   (unsigned long)gInputCache.buttons);
  if (n < 0 || (size_t)n >= bufSize - pos) return 0;
  return pos + n;
}

void inputTask(void* parameter) {
  INFO_INPUT_LIFECYCLEF("Task started (handle=%p, stack=%u words)", 
                (void*)xTaskGetCurrentTaskHandle(), 
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  INFO_INPUT_LIFECYCLEF("[MODULAR] inputTask() running from i2csensor_seesaw.cpp");
  DEBUG_INPUT_LIFECYCLEF("[GAMEPAD_TASK] Initial state: enabled=%d connected=%d", gInputEnabled, gInputConnected);
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
    if (!gInputEnabled) {
      gInputConnected = false;
      gInputCache.dataValid = false;
      SENSOR_TASK_EXIT(INPUT);
    }

    // Stack watermark tracking + safety bailout
    unsigned long nowMs = millis();
    if ((nowMs - lastStackLog) >= 30000) {
      lastStackLog = nowMs;
      // 'continue' (not 'break') so the top-of-loop shutdown runs the clean
      // SENSOR_TASK_EXIT (vTaskDelete) path instead of returning from the task
      // function with a near-overflowed stack (IllegalInstruction panic).
      if (checkTaskStackSafety("gamepad", INPUT_STACK_WORDS, &gInputEnabled)) continue;
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
      DEBUG_INPUT_POLLINGF("[GAMEPAD_TASK] State: enabled=%d connected=%d paused=%d dataValid=%d",
                    gInputEnabled, gInputConnected, gSensorPollingPaused, gInputCache.dataValid);
    }

    if (gInputEnabled && gInputConnected && !pollPaused((uint8_t)gSettings.inputBus)) {
      unsigned long gamepadPollMs = (gSettings.inputDevicePollMs > 0) ? (unsigned long)gSettings.inputDevicePollMs : 90;
      if ((nowMs - lastGamepadRead) >= gamepadPollMs) {
        bool readSuccess = false;
        uint32_t buttons = 0;
        uint32_t intflags = 0;  // Hardware edge-latch: pins that had any transition since last read
        int rawX = 0, rawY = 0;

        // Poll at 100kHz (NOT the Seesaw's max 400kHz). The fast poll runs
        // 40+ I2C transactions/sec on this bus; at 400kHz, under BLE radio
        // coexistence (the controller is pinned to CPU0 with the I2C ISR), a
        // marginal transaction can NACK/timeout and the legacy I2C driver
        // re-arms the command from inside the ISR — an interrupt storm that
        // trips the Int WDT. 100kHz (matching gamepad init) gives the timing
        // margin to avoid that; the slower transaction is still <1ms.
        // 80ms timeout: must be < the gamepad bus's setTimeOut(100ms) so Wire
        // aborts cleanly first. Bus-aware: mutex + clock route to the gamepad's bus.
        const uint8_t gpBus = (uint8_t)gSettings.inputBus;
        auto result = gGamepadSeesaw && i2cDeviceTransaction(gpBus, I2C_ADDR_GAMEPAD, 100000, 80, [&]() -> bool {
          // Exceptions are disabled (-fno-exceptions), so rely on return value only.
          // Read ONLY button pins, not all 32 GPIO pins - prevents garbage from unconfigured pins
          buttons = gGamepadSeesaw->digitalReadBulk(GAMEPAD_BUTTON_MASK);
          // Read and clear the hardware interrupt flag register. The ATSAMD09 latches every
          // edge event between polls (read-to-clear), so a press+release inside one 90ms window
          // still shows up here even though BULK already shows the button as released.
          intflags = gGamepadSeesaw->digitalReadBulkIntFlag(GAMEPAD_BUTTON_MASK);
          rawX = 1023 - gGamepadSeesaw->analogRead(14);
          rawY = 1023 - gGamepadSeesaw->analogRead(15);
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
          
          // Track previous buttons for debounce / edge detection
          static uint32_t lastButtons = 0xFFFFFFFF;
          
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
            
            DEBUG_INPUT_VALUESF("[GAMEPAD_PRESS] buttons=0x%08lX changed=0x%08lX pressed=0x%08lX released=0x%08lX",
                          (unsigned long)buttons, (unsigned long)changed, 
                          (unsigned long)pressed, (unsigned long)released);
            
            // Log individual button names for clarity
            if (pressed & GAMEPAD_BUTTON_A) DEBUG_INPUT_VALUESF("[GAMEPAD_PRESS] A pressed");
            if (pressed & GAMEPAD_BUTTON_B) DEBUG_INPUT_VALUESF("[GAMEPAD_PRESS] B pressed");
            if (pressed & GAMEPAD_BUTTON_X) DEBUG_INPUT_VALUESF("[GAMEPAD_PRESS] X pressed");
            if (pressed & GAMEPAD_BUTTON_Y) DEBUG_INPUT_VALUESF("[GAMEPAD_PRESS] Y pressed");
            if (pressed & GAMEPAD_BUTTON_START) DEBUG_INPUT_VALUESF("[GAMEPAD_PRESS] START pressed");
            
            if (released & GAMEPAD_BUTTON_A) DEBUG_INPUT_VALUESF("[GAMEPAD_PRESS] A released");
            if (released & GAMEPAD_BUTTON_B) DEBUG_INPUT_VALUESF("[GAMEPAD_PRESS] B released");
            if (released & GAMEPAD_BUTTON_X) DEBUG_INPUT_VALUESF("[GAMEPAD_PRESS] X released");
            if (released & GAMEPAD_BUTTON_Y) DEBUG_INPUT_VALUESF("[GAMEPAD_PRESS] Y released");
            if (released & GAMEPAD_BUTTON_START) DEBUG_INPUT_VALUESF("[GAMEPAD_PRESS] START released");
            
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
            SensorCacheGuard g(gInputCache.mutex, pdMS_TO_TICKS(50), "gamepad.pollWrite");
            if (g.held) {
            // Only increment seq when data actually changes to avoid unnecessary OLED re-renders
            // Use threshold of 2 for joystick to ignore ADC noise/rounding jitter
            bool changed = (gInputCache.buttons != buttons ||
                            abs(gInputCache.joyX - filtX) > 1 ||
                            abs(gInputCache.joyY - filtY) > 1);
            // Latch press edges into the accumulator so the UI never misses a tap.
            // Active-low: 1 = unpressed, 0 = pressed.
            //
            // Two sources of press evidence:
            //
            // 1. Debounce-confirmed press: button state changed and debounce accepted it.
            //    Covers normal (held) presses where the button is still down this poll.
            uint32_t confirmedPressed = gInputCache.buttons & ~buttons;
            //
            // 2. INTFLAG-based press: hardware edge-latch fired this poll interval.
            //    Covers quick taps released before the second confirmation poll — the
            //    debounce filter would discard these entirely, but INTFLAG saw the edge.
            //    Condition: intflag set AND (currently pressed OR was previously unpressed).
            //    The "was previously unpressed" arm catches press+release within one interval.
            //    The "currently pressed" arm catches first-detected holds (belt-and-suspenders).
            uint32_t intflagPressed = intflags & (~buttons | lastButtons);
            //
            gInputCache.buttonPressedAccum |= confirmedPressed | intflagPressed;
            gInputCache.buttons = buttons;  // Only changes after debounce
            gInputCache.joyX = filtX;
            gInputCache.joyY = filtY;
            gInputCache.lastUpdate = nowMs;
            gInputCache.dataValid = true;
            if (changed) gInputCache.seq++;
            }
          }  // gamepad guard releases here

          // ESP-NOW broadcaster reads gamepadBuildDataJSON() on demand from gInputCache
          // (paced per-sensor in gSensorSpecs — 100ms for snappy button-press response).
        } else if (!readSuccess) {
          // Actual I2C transaction failure
          // Note: I2CDevice::recordError() called automatically by transaction
          uint8_t errors = i2cGetConsecutiveErrors(I2C_ADDR_GAMEPAD);
          WARN_INPUTF("[GAMEPAD_TASK] I2C read failure (consecutive: %u)", errors);
          
          if (i2cShouldAutoDisable(I2C_ADDR_GAMEPAD)) {
            ERROR_INPUTF("[GAMEPAD_TASK] Too many consecutive failures - auto-disabling");
            handleDeviceStopped(I2C_DEVICE_INPUT);
            DEBUG_INPUT_LIFECYCLEF("Gamepad auto-disabled: %u consecutive I2C failures", errors);
            sensorStatusBumpWith("gamepad@auto_disabled");
            logSystemEvent("SENSOR", "Gamepad auto-disabled after %u consecutive I2C failures", errors);
          }
        } else {
          // I2C succeeded but data validation failed (garbage data during bus contention)
          consecutiveInvalidReads++;
          if (consecutiveInvalidReads == 10) {
            WARN_INPUTF("[GAMEPAD_TASK] 10 consecutive invalid reads - device may be disconnected");
          }
          if (consecutiveInvalidReads >= INVALID_READ_AUTO_DISABLE_THRESHOLD) {
            ERROR_INPUTF("[GAMEPAD_TASK] %u consecutive invalid reads - auto-disabling gamepad", consecutiveInvalidReads);
            handleDeviceStopped(I2C_DEVICE_INPUT);
            sensorStatusBumpWith("gamepad@invalid_data_disabled");
            logSystemEvent("SENSOR", "Gamepad auto-disabled after %u consecutive invalid reads (device likely disconnected)", consecutiveInvalidReads);
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
  if (!gInputConnected || !gInputCache.dataValid) return 0;
  return gInputCache.joyX;
}

int gamepadGetY() {
  if (!gInputConnected || !gInputCache.dataValid) return 0;
  return gInputCache.joyY;
}

uint32_t gamepadGetButtons() {
  if (!gInputConnected || !gInputCache.dataValid) return 0;
  return gInputCache.buttons;
}

// ============================================================================
// Gamepad OLED Mode
// ============================================================================
#if DISPLAY_TYPE > 0
#include "i2csensor_seesaw_oled.h"
#endif

#endif // ENABLE_GAMEPAD_SENSOR
