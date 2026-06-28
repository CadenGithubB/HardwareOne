#include "i2csensor_ano_encoder.h"
#include "System_BuildConfig.h"
#include "System_MemoryMonitor.h"
#include "System_MemUtil.h"
#include "System_Utils.h"

#if ENABLE_ANO_ENCODER

#include <Adafruit_seesaw.h>
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
#include "i2csensor_seesaw.h"  // For InputCache struct + gGamepad* extern declarations

// ============================================================================
// Module state
// ============================================================================
TaskHandle_t gAnoEncoderTaskHandle = nullptr;
Adafruit_seesaw* gAnoSeesaw = nullptr;
AnoEncoderCache  gAnoEncoderCache;

bool gAnoEncoderEnabled = false;
bool gAnoEncoderConnected = false;

// Gamepad-shaped proxies: the OLED input pipeline reads these. When ANO is the
// active driver (this .cpp compiled, i2csensor_seesaw.cpp excluded by CMake),
// the ANO task populates gInputCache with synthesized state so processGamepad-
// MenuInput keeps working unchanged. gInputEnabled/gInputConnected mirror
// gAnoEncoderEnabled/gAnoEncoderConnected.
bool gInputEnabled = false;
bool gInputConnected = false;
InputCache gInputCache;

static unsigned long gLastAnoInitMs = 0;
static const unsigned long kAnoInitMinIntervalMs = 2000;

// Seesaw GPIO pin assignments for the Adafruit ANO breakout. These come from
// Adafruit's own example sketch — the ATtiny817 on the breakout exposes the
// 5 switches on pins 1..5 and the rotary on encoder #0.
static const uint8_t SS_PIN_SELECT = 1;  // IN / center press
static const uint8_t SS_PIN_UP     = 2;
static const uint8_t SS_PIN_LEFT   = 3;
static const uint8_t SS_PIN_DOWN   = 4;
static const uint8_t SS_PIN_RIGHT  = 5;
static const uint32_t SS_BUTTON_MASK =
    (1UL << SS_PIN_SELECT) | (1UL << SS_PIN_UP) | (1UL << SS_PIN_LEFT) |
    (1UL << SS_PIN_DOWN)   | (1UL << SS_PIN_RIGHT);

// Translate the raw seesaw GPIO bulk read (active-low) into our internal
// active-high ANO_BTN_* bit layout. Keeping a separate layout means the
// consumer-facing bits stay stable even if a future breakout revision
// remaps seesaw pins.
static inline uint32_t ssBulkToAnoBits(uint32_t bulk) {
  // bulk: bit N = 1 if seesaw pin N is HIGH (button NOT pressed)
  // Invert and remap to ANO_BTN_* positions.
  //
  // Mounting-orientation compensation: gSettings.anoEncoderSwap{UpDown,LeftRight}
  // pick which physical seesaw pin sources each logical ANO_BTN_* bit. Applying
  // the swap here — the single boundary where chip pins become logical bits —
  // means everything downstream (chord state machine, cache, OLED nav, web
  // card, MQTT, ESP-NOW peers) sees the user-perceived labels automatically.
  const uint8_t upPin    = gSettings.anoEncoderSwapUpDown    ? SS_PIN_DOWN  : SS_PIN_UP;
  const uint8_t downPin  = gSettings.anoEncoderSwapUpDown    ? SS_PIN_UP    : SS_PIN_DOWN;
  const uint8_t leftPin  = gSettings.anoEncoderSwapLeftRight ? SS_PIN_RIGHT : SS_PIN_LEFT;
  const uint8_t rightPin = gSettings.anoEncoderSwapLeftRight ? SS_PIN_LEFT  : SS_PIN_RIGHT;

  uint32_t pressed = ~bulk;
  uint32_t out = 0;
  if (pressed & (1UL << SS_PIN_SELECT)) out |= ANO_BTN_IN;
  if (pressed & (1UL << upPin))         out |= ANO_BTN_UP;
  if (pressed & (1UL << downPin))       out |= ANO_BTN_DOWN;
  if (pressed & (1UL << leftPin))       out |= ANO_BTN_LEFT;
  if (pressed & (1UL << rightPin))      out |= ANO_BTN_RIGHT;
  return out;
}

// Format an ANO_BTN_* bitmask as a human-readable button list. Returns the
// passed-in buffer for inline use. Output looks like "IN+UP" or "RIGHT+START"
// or "(none)" for an empty mask. Used by the debug logs in inputTask().
static const char* anoBtnNames(uint32_t mask, char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return "";
  buf[0] = '\0';
  size_t off = 0;
  auto append = [&](const char* s) {
    size_t n = strlen(s);
    if (off == 0) {
      if (off + n < bufSize) { memcpy(buf + off, s, n); off += n; buf[off] = '\0'; }
    } else {
      if (off + 1 + n < bufSize) { buf[off++] = '+'; memcpy(buf + off, s, n); off += n; buf[off] = '\0'; }
    }
  };
  if (mask & ANO_BTN_IN)     append("IN");
  if (mask & ANO_BTN_UP)     append("UP");
  if (mask & ANO_BTN_DOWN)   append("DOWN");
  if (mask & ANO_BTN_LEFT)   append("LEFT");
  if (mask & ANO_BTN_RIGHT)  append("RIGHT");
  if (mask & ANO_VIRT_START) append("START");
  if (off == 0 && bufSize >= 7) { memcpy(buf, "(none)", 7); }
  return buf;
}

// ============================================================================
// Bus resolution
// ============================================================================
static bool anoResolveBus(uint8_t* outBus, TwoWire** outWire) {
  const uint8_t bus = (uint8_t)gSettings.inputBus;
  TwoWire* w = i2c() ? i2c()->getWire(bus) : nullptr;
  if (!w) return false;
  *outBus = bus;
  *outWire = w;
  return true;
}

static inline uint8_t anoI2cAddr() {
  // Runtime override (set via CLI or web) for trial-and-error addressing.
  // Falls back to the documented default if user hasn't touched it.
  int v = gSettings.anoEncoderI2cAddr;
  return (v > 0 && v < 0x80) ? (uint8_t)v : I2C_ADDR_ANO_ENCODER;
}

static inline void anoLogHeap(const char* tag) {
  if (!isDebugFlagSet(DEBUG_MEMORY)) return;
  size_t freeHeap = ESP.getFreeHeap();
  size_t minFree  = ESP.getMinFreeHeap();
  size_t largest  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  DEBUG_MEMORYF("[ANO_MEM] %s heap_free=%u heap_min=%u largest=%u",
                tag ? tag : "?",
                (unsigned)freeHeap, (unsigned)minFree, (unsigned)largest);
}

// ============================================================================
// Initialization
// ============================================================================
bool anoEncoderInit() {
  if (gAnoEncoderConnected && gAnoSeesaw) return true;

  uint8_t bus; TwoWire* wire;
  if (!anoResolveBus(&bus, &wire)) {
    ERROR_ANO_ENCODERF("ANO encoder bus %d not initialized", gSettings.inputBus);
    return false;
  }

  if (!gAnoSeesaw) {
    gAnoSeesaw = new Adafruit_seesaw(wire);
    if (!gAnoSeesaw) {
      ERROR_ANO_ENCODERF("Failed to allocate Adafruit_seesaw for ANO");
      return false;
    }
  }

  const uint8_t addr = anoI2cAddr();
  bool ok = i2cDeviceTransaction(bus, addr, 100000, 3000, [&]() -> bool {
    if (!gAnoSeesaw->begin(addr)) {
      ERROR_ANO_ENCODERF("ANO seesaw not found at 0x%02X on bus %u", addr, bus);
      return false;
    }

    gAnoSeesaw->SWReset();
    delay(10);

    if (!gAnoSeesaw->begin(addr)) {
      ERROR_ANO_ENCODERF("ANO seesaw not responding after soft reset");
      return false;
    }

    uint32_t version = (gAnoSeesaw->getVersion() >> 16) & 0xFFFF;
    INFO_ANO_ENCODER_LIFECYCLEF("ANO seesaw version: %lu", (unsigned long)version);

    // Configure buttons as pullups + enable interrupts (matches gamepad init).
    gAnoSeesaw->pinModeBulk(SS_BUTTON_MASK, INPUT_PULLUP);
    gAnoSeesaw->setGPIOInterrupts(SS_BUTTON_MASK, 1);

    // Zero the encoder so first read returns a known reference.
    gAnoSeesaw->setEncoderPosition(0, 0);
    gAnoSeesaw->enableEncoderInterrupt(0);
    return true;
  });

  if (ok) {
    gAnoEncoderConnected = true;
    gInputConnected = true;  // proxy in sync
    broadcastOutput("ANO encoder initialized");
  } else {
    ERROR_ANO_ENCODERF("ANO encoder init failed");
  }
  return ok;
}

bool anoEncoderInitConnection() {
  if (gAnoEncoderConnected && gAnoSeesaw) return true;
  unsigned long now = millis();
  if (now - gLastAnoInitMs < kAnoInitMinIntervalMs) return false;
  gLastAnoInitMs = now;
  return anoEncoderInit();
}

bool inputStartInternal() {
  DEBUG_CLIF("[QUEUE] Processing ANO encoder start from queue");
  anoLogHeap("start.begin");

  String memReason;
  if (!checkMemoryAvailable("anoencoder", &memReason)) {
    ERROR_ANO_ENCODERF("Insufficient memory for ANO encoder: %s", memReason.c_str());
    return false;
  }

  if (!gAnoEncoderCache.mutex) {
    gAnoEncoderCache.mutex = xSemaphoreCreateMutex();
    if (!gAnoEncoderCache.mutex) {
      ERROR_ANO_ENCODERF("Failed to create ANO cache mutex");
      return false;
    }
  }
  // Sibling mutex for the gamepad-shaped proxy cache the OLED reads.
  if (!gInputCache.mutex) {
    gInputCache.mutex = xSemaphoreCreateMutex();
  }

  {
    SensorCacheGuard g(gAnoEncoderCache.mutex, pdMS_TO_TICKS(100), "ano.cleanStaleCache");
    if (g.held) {
      gAnoEncoderCache.dataValid = false;
      gAnoEncoderCache.buttons = 0;
      gAnoEncoderCache.encoderDelta = 0;
      gAnoEncoderCache.encoderPosition = 0;
      gAnoEncoderCache.buttonPressedAccum = 0;
    }
  }

  if (!anoEncoderInit()) {
    anoLogHeap("start.init_fail");
    return false;
  }

  bool prev = gInputEnabled;
  gAnoEncoderEnabled = true;
  gInputEnabled = true;       // keep proxy in sync for OLED-side checks
  DEBUG_ANO_ENCODER_LIFECYCLEF("[ANO] inputStartInternal: gInputEnabled=true (was %d) gInputConnected=%d",
                               prev, gInputConnected);
  // Bump the sensor-status sequence so the SSE event fires and the web UI
  // refreshes its connected/enabled indicators immediately. Without this the
  // dot can stay red until the next polling tick.
  if (gInputEnabled != prev) sensorStatusBumpWith("openinput@enabled");

  // Broadcast sensor status to ESP-NOW master so paired peers see the change.
#if ENABLE_ESPNOW
  broadcastSensorStatus(REMOTE_SENSOR_INPUT, true);
#endif

  if (!createInputTask()) {  // Reuse the existing gamepad task slot — only one input device runs.
    ERROR_ANO_ENCODERF("Failed to create ANO encoder task");
    return false;
  }
  anoLogHeap("start.after_task");
  return true;
}

// ============================================================================
// Axis state (per-mode)
// ============================================================================
void anoEncoderResetAxisForMode(uint8_t defaultAxis) {
  if (!gAnoEncoderCache.mutex) return;
  SensorCacheGuard g(gAnoEncoderCache.mutex, pdMS_TO_TICKS(10), "ano.resetAxis");
  if (g.held) {
    gAnoEncoderCache.currentAxis =
        (defaultAxis == ANO_AXIS_HORIZONTAL) ? ANO_AXIS_HORIZONTAL : ANO_AXIS_VERTICAL;
  }
}

int anoEncoderConsumeOneDetent() {
  if (!gAnoEncoderCache.mutex) return 0;
  SensorCacheGuard g(gAnoEncoderCache.mutex, pdMS_TO_TICKS(5), "ano.consumeDetent");
  if (!g.held) return 0;
  if (gAnoEncoderCache.encoderDelta > 0) { gAnoEncoderCache.encoderDelta--; return 1; }
  if (gAnoEncoderCache.encoderDelta < 0) { gAnoEncoderCache.encoderDelta++; return -1; }
  return 0;
}

int anoEncoderBuildDataJSON(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;
  SensorCacheGuard g(gAnoEncoderCache.mutex, pdMS_TO_TICKS(50), "ano.buildJSON");
  if (!g.held) return 0;
  return snprintf(buf, bufSize,
                  "{\"val\":1,\"pos\":%ld,\"axis\":%u,\"buttons\":%lu}",
                  (long)gAnoEncoderCache.encoderPosition,
                  (unsigned)gAnoEncoderCache.currentAxis,
                  (unsigned long)gAnoEncoderCache.buttons);
}

// ============================================================================
// Command handlers
// ============================================================================
const char* cmd_anoencoder(const String& argsInput) {
  if (argWantsJson(argsInput)) {
    static char jbuf[160];
    snprintf(jbuf, sizeof(jbuf),
      "{\"schema\":1,\"connected\":%s,\"position\":%ld,\"axis\":%u,\"buttons\":%lu}",
      gAnoEncoderConnected ? "true" : "false",
      (long)gAnoEncoderCache.encoderPosition, (unsigned)gAnoEncoderCache.currentAxis,
      (unsigned long)gAnoEncoderCache.buttons);
    return jbuf;
  }
  if (!gAnoEncoderConnected) {
    if (!anoEncoderInitConnection()) return "[ANO] Error: Not connected - check wiring";
  }
  static char buf[80];
  snprintf(buf, sizeof(buf), "[ANO] pos=%ld axis=%u buttons=0x%08lX",
           (long)gAnoEncoderCache.encoderPosition,
           (unsigned)gAnoEncoderCache.currentAxis,
           (unsigned long)gAnoEncoderCache.buttons);
  return buf;
}

const char* cmd_anoencoderi2caddr(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  if (a.count() == 0) {
    static char buf[48];
    snprintf(buf, sizeof(buf), "[ANO] I2C address: 0x%02X", gSettings.anoEncoderI2cAddr);
    return buf;
  }
  int v = a.argInt(0, 0);
  if (v < 1 || v > 127) return "Usage: anoencoderi2caddr <1-127> (decimal) — try i2cscan first";
  setSetting(gSettings.anoEncoderI2cAddr, v);
  return "[ANO] I2C address updated (reboot required)";
}

const char* cmd_anoencoderinvert(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim(); arg.toLowerCase();
  if (arg.length() == 0) {
    return gSettings.anoEncoderInvert ? "[ANO] Rotation: inverted" : "[ANO] Rotation: normal";
  }
  if (arg == "on" || arg == "true" || arg == "1") {
    setSetting(gSettings.anoEncoderInvert, true);
    return "[ANO] Rotation inverted";
  }
  if (arg == "off" || arg == "false" || arg == "0") {
    setSetting(gSettings.anoEncoderInvert, false);
    return "[ANO] Rotation normal";
  }
  return "Usage: anoencoderinvert [on|off]";
}

// Helper: parse on/off/toggle into a bool, applied to the given setting field.
// Returns the new value via outNewValue; returns true on success, false on
// usage error.
static bool parseOnOffToggleArg(const String& argsInput, bool currentValue,
                                bool* outNewValue) {
  String arg = argsInput; arg.trim(); arg.toLowerCase();
  if (arg.length() == 0) { *outNewValue = currentValue; return true; }  // status query
  if (arg == "on"  || arg == "true"  || arg == "1") { *outNewValue = true;  return true; }
  if (arg == "off" || arg == "false" || arg == "0") { *outNewValue = false; return true; }
  if (arg == "toggle")                              { *outNewValue = !currentValue; return true; }
  return false;
}

const char* cmd_anoencoderswapud(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  bool target = false;
  if (!parseOnOffToggleArg(argsInput, gSettings.anoEncoderSwapUpDown, &target)) {
    return "Usage: anoencoderswapud [on|off|toggle]";
  }
  String arg = argsInput; arg.trim();
  if (arg.length() != 0) setSetting(gSettings.anoEncoderSwapUpDown, target);
  return target ? "[ANO] UP/DOWN buttons: swapped" : "[ANO] UP/DOWN buttons: normal";
}

const char* cmd_anoencoderswaplr(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  bool target = false;
  if (!parseOnOffToggleArg(argsInput, gSettings.anoEncoderSwapLeftRight, &target)) {
    return "Usage: anoencoderswaplr [on|off|toggle]";
  }
  String arg = argsInput; arg.trim();
  if (arg.length() != 0) setSetting(gSettings.anoEncoderSwapLeftRight, target);
  return target ? "[ANO] LEFT/RIGHT buttons: swapped" : "[ANO] LEFT/RIGHT buttons: normal";
}

// ============================================================================
// Settings module — only ANO-specific entries. The shared poll/autostart/bus
// settings are in inputSettingsModule (HAL_Input.cpp).
// ============================================================================
static const SettingEntry anoEncoderSettingEntries[] = {
  { "anoEncoderI2cAddr",       SETTING_INT,  &gSettings.anoEncoderI2cAddr,       I2C_ADDR_ANO_ENCODER, 0, nullptr, 1, 127, "I2C Address",           nullptr, false, nullptr, "anoencoderi2caddr" },
  { "anoEncoderInvert",        SETTING_BOOL, &gSettings.anoEncoderInvert,        0,                    0, nullptr, 0, 1,   "Invert rotation",       nullptr, false, nullptr, "anoencoderinvert" },
  { "anoEncoderSwapUpDown",    SETTING_BOOL, &gSettings.anoEncoderSwapUpDown,    1,                    0, nullptr, 0, 1,   "Swap UP/DOWN buttons",  nullptr, false, nullptr, "anoencoderswapud" },
  { "anoEncoderSwapLeftRight", SETTING_BOOL, &gSettings.anoEncoderSwapLeftRight, 1,                    0, nullptr, 0, 1,   "Swap LEFT/RIGHT buttons", nullptr, false, nullptr, "anoencoderswaplr" },
};

static bool isAnoEncoderConnected() { return gAnoEncoderConnected; }

extern const SettingsModule anoEncoderSettingsModule = {
  "anoencoder",
  "hardware.sensors.anoencoder",
  anoEncoderSettingEntries,
  sizeof(anoEncoderSettingEntries) / sizeof(anoEncoderSettingEntries[0]),
  isAnoEncoderConnected,
  "Adafruit ANO Rotary Encoder breakout (driver-specific settings)"
};

// ============================================================================
// Command registry — only ANO-specific debug + tuning commands. The unified
// open/close/autostart/poll commands live in HAL_Input.cpp (inputCommands).
// ============================================================================
const CommandEntry anoEncoderCommands[] = {
  { "anoencoderread",    "Read ANO encoder state. (add 'json' for JSON output)",                  false, cmd_anoencoder },
  { "anoencoderi2caddr", "Set ANO I2C address [1-127]",              true,  cmd_anoencoderi2caddr, "Usage: anoencoderi2caddr <1-127>" },
  { "anoencoderinvert",  "Invert rotation direction [on|off]",       false, cmd_anoencoderinvert,  "Usage: anoencoderinvert [on|off]" },
  { "anoencoderswapud",  "Swap UP/DOWN buttons [on|off|toggle]",     false, cmd_anoencoderswapud,  "Usage: anoencoderswapud [on|off|toggle]" },
  { "anoencoderswaplr",  "Swap LEFT/RIGHT buttons [on|off|toggle]",  false, cmd_anoencoderswaplr,  "Usage: anoencoderswaplr [on|off|toggle]" },
};
const size_t anoEncoderCommandsCount = sizeof(anoEncoderCommands) / sizeof(anoEncoderCommands[0]);

// ============================================================================
// Polling task — driven by createInputTask() (shared slot)
// ============================================================================
// Reads buttons + encoder, runs the RIGHT/RIGHT+IN chord state machine, and
// accumulates detents into encoderDelta. The OLED reader consumes one detent
// per frame so fast spins still feel responsive without losing clicks.

void inputTask(void* parameter) {
  INFO_ANO_ENCODER_LIFECYCLEF("ANO encoder task started (handle=%p)", (void*)xTaskGetCurrentTaskHandle());
  anoLogHeap("task.entry");

  unsigned long lastRead = 0;
  unsigned long lastStackLog = 0;
  uint8_t consecutiveInvalidReads = 0;
  static const uint8_t INVALID_READ_AUTO_DISABLE_THRESHOLD = 20;

  // Chord state machine: tracks RIGHT-held window so we can disambiguate
  // "RIGHT tapped alone (axis toggle)" from "RIGHT held while IN pressed (START)".
  bool prevRightHeld = false;
  bool rightHadChord = false;
  uint32_t prevButtons = 0;  // active-high (our bit layout)
  int32_t  prevEncoderPos = 0;

  // POLL-log de-noise state. The driver polls at ~30 ms regardless (needed for
  // accuracy on fast spins / quick taps), but emitting an [ANO_POLL] line on
  // every tick floods the serial log when nothing's actually happening. So we
  // log only on STATE CHANGE (bulk or encPos differs from what we last logged)
  // PLUS a once-per-second heartbeat so a stuck task is still visible. Initial
  // sentinel values guarantee the first iteration is treated as a change.
  uint32_t      lastLoggedBulk   = 0xFFFFFFFEu;
  int32_t       lastLoggedEncPos = INT32_MIN;
  unsigned long lastPollLogMs    = 0;
  const unsigned long POLL_LOG_HEARTBEAT_MS = 1000;

  while (true) {
    if (!gAnoEncoderEnabled) {
      gAnoEncoderConnected = false;
      gAnoEncoderCache.dataValid = false;
      gInputEnabled = false;
      gInputConnected = false;
      gInputCache.dataValid = false;
      SENSOR_TASK_EXIT(INPUT);
    }

    unsigned long nowMs = millis();
    if ((nowMs - lastStackLog) >= 30000) {
      lastStackLog = nowMs;
      if (checkTaskStackSafety("anoencoder", INPUT_STACK_WORDS, &gAnoEncoderEnabled)) break;
    }

    if (gAnoEncoderEnabled && gAnoEncoderConnected && !pollPaused((uint8_t)gSettings.inputBus)) {
      unsigned long pollMs = (gSettings.inputDevicePollMs > 0)
                              ? (unsigned long)gSettings.inputDevicePollMs : 30;
      if ((nowMs - lastRead) >= pollMs) {
        bool readOk = false;
        uint32_t bulk = 0;
        int32_t encPos = prevEncoderPos;

        const uint8_t bus = (uint8_t)gSettings.inputBus;
        const uint8_t addr = anoI2cAddr();
        auto result = gAnoSeesaw && i2cDeviceTransaction(bus, addr, 400000, 80, [&]() -> bool {
          bulk = gAnoSeesaw->digitalReadBulk(SS_BUTTON_MASK);
          encPos = gAnoSeesaw->getEncoderPosition(0);
          return true;
        });
        readOk = (result == true);

        // Validate: if every button bit reads pressed, the bus is likely stuck low.
        uint32_t btns = ssBulkToAnoBits(bulk);
        bool dataValid = true;
        if (readOk && (bulk & SS_BUTTON_MASK) == 0) dataValid = false;  // All-zero bulk = bus fault

        // Raw read log (POLLING flag). Gated on (a) state change since the
        // last log line OR (b) heartbeat — once per POLL_LOG_HEARTBEAT_MS to
        // confirm the task is still alive even when nothing's happening. This
        // collapses ~30× log volume during idle while still showing every
        // edge and every detent the moment it appears. The poll itself is
        // unchanged — same cadence, same accuracy.
        const bool pollChanged = (bulk != lastLoggedBulk) || (encPos != lastLoggedEncPos);
        const bool pollHeartbeat = (nowMs - lastPollLogMs) >= POLL_LOG_HEARTBEAT_MS;
        if (pollChanged || pollHeartbeat) {
          DEBUG_ANO_ENCODER_POLLINGF("[ANO_POLL] bulk=0x%08lX btns=0x%02lX encPos=%ld readOk=%d valid=%d%s",
                                     (unsigned long)bulk, (unsigned long)btns,
                                     (long)encPos, readOk ? 1 : 0, dataValid ? 1 : 0,
                                     pollChanged ? "" : " (hb)");
          lastLoggedBulk = bulk;
          lastLoggedEncPos = encPos;
          lastPollLogMs = nowMs;
        }

        if (readOk && dataValid) {
          consecutiveInvalidReads = 0;

          // Edge detection: bits that went 0→1 are new presses.
          uint32_t newlyPressed = btns & ~prevButtons;
          uint32_t newlyReleased = prevButtons & ~btns;

          // Log press/release edges (VALUES flag). Empty masks produce no log.
          if (newlyPressed) {
            char nameBuf[48];
            DEBUG_ANO_ENCODER_VALUESF("[ANO_VAL] press   raw=0x%02lX (%s)",
                                      (unsigned long)newlyPressed,
                                      anoBtnNames(newlyPressed, nameBuf, sizeof(nameBuf)));
          }
          if (newlyReleased) {
            char nameBuf[48];
            DEBUG_ANO_ENCODER_VALUESF("[ANO_VAL] release raw=0x%02lX (%s)",
                                      (unsigned long)newlyReleased,
                                      anoBtnNames(newlyReleased, nameBuf, sizeof(nameBuf)));
          }

          // ----- Chord: RIGHT held + IN press → synthesize START -----
          // Earlier revisions also used RIGHT-tap to toggle the rotary axis,
          // and stripped RIGHT from the latched newlyPressed because it was
          // "just a modifier." Both of those made the RIGHT button unusable
          // as a normal directional input — every press silently mutated
          // wheel behavior and never reached the menu nav pipeline. Modes
          // that need horizontal axis call anoEncoderResetAxisForMode() at
          // entry; the user shouldn't have to discover a hidden gesture.
          //
          // The chord stays because RIGHT+IN is unusual enough that users
          // won't trigger it accidentally, and it preserves START button
          // access (system menu / data-source toggle) on a 5-button device.
          bool rightHeldNow = (btns & ANO_BTN_RIGHT) != 0;
          if (rightHeldNow && (newlyPressed & ANO_BTN_IN)) {
            // Convert IN-press into START. Drop the IN bit (so it doesn't
            // also count as a regular IN-press) and add the virtual bit.
            newlyPressed &= ~ANO_BTN_IN;
            newlyPressed |= ANO_VIRT_START;
            DEBUG_ANO_ENCODER_VALUESF("[ANO_VAL] chord RIGHT+IN → synthesize START");
          }
          prevRightHeld = rightHeldNow;
          (void)rightHadChord;  // retained for binary compatibility; no longer used

          // ----- Encoder delta accumulation -----
          int32_t rawDelta = encPos - prevEncoderPos;
          prevEncoderPos = encPos;
          int32_t detentDelta = rawDelta;
          // Respect optional inversion (some users mount the encoder backwards).
          if (gSettings.anoEncoderInvert) detentDelta = -detentDelta;

          // Log rotation events with raw + inverted delta + final value (VALUES).
          if (rawDelta != 0) {
            DEBUG_ANO_ENCODER_VALUESF("[ANO_VAL] rotate raw=%ld → detent=%ld (invert=%d) pos=%ld",
                                      (long)rawDelta, (long)detentDelta,
                                      gSettings.anoEncoderInvert ? 1 : 0, (long)encPos);
          }

          // ----- Publish to native ANO cache -----
          {
            SensorCacheGuard g(gAnoEncoderCache.mutex, pdMS_TO_TICKS(50), "ano.pollWrite");
            if (g.held) {
              bool changed = (gAnoEncoderCache.buttons != btns) ||
                             (detentDelta != 0);
              uint32_t prevAccum = gAnoEncoderCache.buttonPressedAccum;
              int32_t  prevPendingDetents = gAnoEncoderCache.encoderDelta;
              gAnoEncoderCache.buttons = btns;
              gAnoEncoderCache.buttonPressedAccum |= newlyPressed;
              gAnoEncoderCache.encoderPosition = encPos;
              gAnoEncoderCache.encoderDelta += detentDelta;
              gAnoEncoderCache.lastUpdate = nowMs;
              gAnoEncoderCache.dataValid = true;
              if (changed) gAnoEncoderCache.seq++;
              // Log cache mutations (VALUES). Only on actual change to avoid noise.
              if (changed) {
                DEBUG_ANO_ENCODER_VALUESF("[ANO_VAL] cache  buttons=0x%02lX accum=0x%02lX→0x%02lX pendingDetents=%ld→%ld seq=%lu",
                                          (unsigned long)btns,
                                          (unsigned long)prevAccum,
                                          (unsigned long)gAnoEncoderCache.buttonPressedAccum,
                                          (long)prevPendingDetents,
                                          (long)gAnoEncoderCache.encoderDelta,
                                          (unsigned long)gAnoEncoderCache.seq);
              }
            }
          }

          // ----- Publish to gamepad-shaped proxy cache for the OLED -----
          // Active-low: gamepad code does `~buttons` to get active-high pressed
          // state. We store ~btns so the inversion yields the ANO_BTN_* bits.
          // Joystick X/Y stay at CENTER — encoder→nav-event happens via the
          // ANO-specific branch in processOLEDInput, not joystick math.
          {
            SensorCacheGuard g(gInputCache.mutex, pdMS_TO_TICKS(50), "ano.proxyWrite");
            if (g.held) {
              gInputCache.buttons = ~btns;
              gInputCache.buttonPressedAccum |= newlyPressed;
              gInputCache.joyX = JOYSTICK_CENTER;
              gInputCache.joyY = JOYSTICK_CENTER;
              gInputCache.lastUpdate = nowMs;
              gInputCache.dataValid = true;
              // Bump seq ONLY on a real input edge (button change or detent),
              // matching the native ANO cache (gAnoEncoderCache.seq above) and
              // the seesaw path. The proxy write runs every poll (~30 ms), so an
              // unconditional seq++ made gInputCache.seq advance ~33x/s forever
              // — which silently defeats BOTH power-save and OLED idle-logout,
              // since each treats any seq change as "real user activity" and so
              // would never go idle on an ANO-encoder board. prevButtons is still
              // the previous poll here (updated below); detentDelta is this poll.
              if ((btns != prevButtons) || (detentDelta != 0)) gInputCache.seq++;
            }
          }

          prevButtons = btns;
        } else if (!readOk) {
          uint8_t errors = i2cGetConsecutiveErrors(anoI2cAddr());
          WARN_ANO_ENCODERF("[ANO_TASK] I2C read failure (consecutive: %u)", errors);
          if (i2cShouldAutoDisable(anoI2cAddr())) {
            ERROR_ANO_ENCODERF("[ANO_TASK] Too many failures - auto-disabling");
            handleDeviceStopped(I2C_DEVICE_INPUT);
          }
        } else {
          consecutiveInvalidReads++;
          if (consecutiveInvalidReads >= INVALID_READ_AUTO_DISABLE_THRESHOLD) {
            ERROR_ANO_ENCODERF("[ANO_TASK] %u invalid reads - auto-disabling", consecutiveInvalidReads);
            handleDeviceStopped(I2C_DEVICE_INPUT);
          }
        }
        lastRead = nowMs;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      drainDebugRing();
    } else {
      vTaskDelay(pdMS_TO_TICKS(20));
      drainDebugRing();
    }
  }
}

#endif // ENABLE_ANO_ENCODER
