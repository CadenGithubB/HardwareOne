#include "i2csensor_ano_encoder.h"
#include "HAL_InputInternal.h"
#include "System_Mutex.h"
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_BuildConfig.h"
#include "System_MemoryMonitor.h"
#include "System_MemUtil.h"
#include "System_Utils.h"

#if ENABLE_ANO_ENCODER

#include <Adafruit_seesaw.h>
#include <Arduino.h>
#include <Wire.h>
#include <new>
#include <esp_heap_caps.h>

#include "OLED_Display.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_SeesawIdentity.h"
#include "System_SensorJsonKeys.h"  // inter-device sensor JSON key names (receivers read the same)
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "i2csensor_seesaw.h"  // For InputCache struct + gGamepad* extern declarations
#include "System_CommandRegistry.h"

// ============================================================================
// Module state
// ============================================================================
TaskHandle_t gAnoEncoderTaskHandle = nullptr;

// The seesaw object with checked reads (the library's digitalReadBulk and
// getEncoderPosition ignore a failed read and return whatever the buffer
// held). Caller holds the bus for every method.
class AnoSeesaw : public Adafruit_seesaw {
 public:
  explicit AnoSeesaw(TwoWire* wire) : Adafruit_seesaw(wire) {}
  // Points the object at another bus; the next begin() builds its I2C device
  // on it.
  void bindWire(TwoWire* wire) { _i2cbus = wire; }
  bool readButtons(uint32_t& bulk) { return readWord(SEESAW_GPIO_BASE, SEESAW_GPIO_BULK, bulk); }
  // Read-to-clear: the pins that changed since the last read.
  bool readIntFlags(uint32_t& flags) { return readWord(SEESAW_GPIO_BASE, SEESAW_GPIO_INTFLAG, flags); }
  // Read-to-clear: the wheel's detents since the last read.
  bool readDelta(int32_t& delta) {
    uint32_t word = 0;
    const bool ok = readWord(SEESAW_ENCODER_BASE, SEESAW_ENCODER_DELTA, word);
    delta = ok ? (int32_t)word : 0;
    return ok;
  }

 private:
  bool readWord(uint8_t base, uint8_t reg, uint32_t& value) {
    uint8_t buf[4] = {0, 0, 0, 0};
    value = 0;
    if (!read(base, reg, buf, sizeof(buf))) return false;
    value = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];
    return true;
  }
};

// Retained across stops/retries; PSRAM-preferred placement-new needs ps_delete
// if an explicit teardown is introduced. Wire/BusIO storage stays separate.
static AnoSeesaw* gAnoSeesaw = nullptr;
// The bus the object's I2C device is on, and the bus and address the board
// was opened at (latched at open: a setting changed while it runs applies at
// the next openinput).
static uint8_t sAnoSeesawBus = 0xFF;
static uint8_t sAnoBus = 0;
static uint8_t sAnoAddress = kAnoAddressDefault;
static AnoEncoderCache sAnoEncoderCache;
static SemaphoreHandle_t sAnoCacheMutex = nullptr;

bool anoEncoderCacheSnapshot(AnoEncoderCache& out, uint32_t timeoutMs) {
  out = {};
  SensorCacheGuard guard(sAnoCacheMutex, pdMS_TO_TICKS(timeoutMs), "ano.snapshot");
  if (!guard.held) return false;
  out = sAnoEncoderCache;
  return true;
}

static bool invalidateAnoCache() {
  SensorCacheGuard guard(sAnoCacheMutex, pdMS_TO_TICKS(100), "ano.invalidate");
  if (!guard.held) return false;
  sAnoEncoderCache.dataValid = false;
  return true;
}

bool gAnoEncoderEnabled = false;
bool gAnoEncoderConnected = false;

// Gamepad-shaped proxies: the OLED input pipeline reads these. When ANO is the
// active driver (this .cpp compiled, i2csensor_seesaw.cpp excluded by CMake),
// the ANO task populates the shared input reading with synthesized state so processGamepad-
// MenuInput keeps working unchanged. gInputRunning/gInputConnected mirror
// gAnoEncoderEnabled/gAnoEncoderConnected.
bool gInputRunning = false;
bool gInputConnected = false;

// Format an ANO_BTN_* bitmask as a human-readable button list. Returns the
// passed-in buffer for inline use. Output looks like "IN+UP" or "RIGHT+START"
// or "(none)" for an empty mask. Used by the debug logs, the button event and
// the read reply.
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
  if (mask & ANO_BTN_IN)      append("IN");
  if (mask & ANO_BTN_UP)      append("UP");
  if (mask & ANO_BTN_DOWN)    append("DOWN");
  if (mask & ANO_BTN_LEFT)    append("LEFT");
  if (mask & ANO_BTN_RIGHT)   append("RIGHT");
  if (mask & ANO_VIRT_START)  append("START");
  if (mask & ANO_VIRT_SELECT) append("SELECT");
  if (off == 0 && bufSize >= 7) { memcpy(buf, "(none)", 7); }
  return buf;
}

// ============================================================================
// Bus and address
// ============================================================================
static bool anoResolveBus(uint8_t* outBus, TwoWire** outWire) {
  const uint8_t bus = (uint8_t)gSettings.inputBus;
  TwoWire* w = i2c() ? i2c()->getWire(bus) : nullptr;
  if (!w) return false;
  *outBus = bus;
  *outWire = w;
  return true;
}

uint8_t anoEncoderAddress() { return anoAddressFromSetting(gSettings.anoEncoderI2cAddr); }

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
// Runs on the sensor queue's worker (inputStartInternal). The bus and the
// address are read from the settings here and kept for the poll until the
// next open.
bool anoEncoderInit() {
  if (gAnoEncoderConnected && gAnoSeesaw) return true;

  uint8_t bus; TwoWire* wire;
  if (!anoResolveBus(&bus, &wire)) {
    ERROR_ANO_ENCODERF("ANO encoder bus %d not initialized", gSettings.inputBus);
    return false;
  }
  const uint8_t addr = anoEncoderAddress();
  {
    char why[112];
    if (i2cAddressTakenBy(bus, addr, I2C_DEVICE_INPUT, 1, why, sizeof(why))) {
      ERROR_ANO_ENCODERF("%s Not opening the ANO encoder there.", why);
      return false;
    }
  }

  if (!gAnoSeesaw) {
    void* anoObjBuf = ps_alloc(sizeof(AnoSeesaw), AllocPref::PreferPsram, "input.ano.obj");
    if (!anoObjBuf) {
      ERROR_ANO_ENCODERF("Failed to allocate the ANO seesaw object");
      return false;
    }
    gAnoSeesaw = new (anoObjBuf) AnoSeesaw(wire);
    sAnoSeesawBus = bus;
  } else if (sAnoSeesawBus != bus) {
    // The bus changed since the object was made: its begin() below builds
    // its I2C device on the new bus's Wire.
    gAnoSeesaw->bindWire(wire);
    sAnoSeesawBus = bus;
  }

  bool ok = i2cDeviceTransaction(bus, addr, 100000, 1000, [&]() -> bool {
    // The product id first, before any begin or reset: 0x49 is also the
    // encoder board's default and the gamepad's alternate, and a board of
    // another product is left exactly as it was.
    uint16_t product = 0;
    if (!seesawReadProductIdOnHeldBus(wire, addr, product)) {
      ERROR_ANO_ENCODERF("ANO seesaw not found at 0x%02X on bus %u", addr, bus);
      return false;
    }
    INFO_ANO_ENCODER_LIFECYCLEF("ANO seesaw product id: %u", (unsigned)product);
    if (product != kSeesawProductAnoEncoder) {
      ERROR_ANO_ENCODERF("Seesaw at 0x%02X reports product %u, not the ANO encoder (%u); not claiming it",
                         addr, (unsigned)product, (unsigned)kSeesawProductAnoEncoder);
      return false;
    }

    // One begin: it resets the board and waits for it to answer again.
    if (!gAnoSeesaw->begin(addr)) {
      ERROR_ANO_ENCODERF("ANO seesaw at 0x%02X on bus %u did not come back after its reset", addr, bus);
      return false;
    }

    // Buttons as pull-ups with change flags (read each poll, so short taps
    // between two polls are seen); the wheel from zero.
    gAnoSeesaw->pinModeBulk(kAnoPinMask, INPUT_PULLUP);
    gAnoSeesaw->setGPIOInterrupts(kAnoPinMask, 1);
    gAnoSeesaw->setEncoderPosition(0, 0);
    gAnoSeesaw->enableEncoderInterrupt(0);
    // Clear the flags the configuration raised, so the first poll sees none.
    uint32_t staleFlags = 0;
    return gAnoSeesaw->readIntFlags(staleFlags);
  });

  if (ok) {
    sAnoBus = bus;
    sAnoAddress = addr;
    gAnoEncoderConnected = true;
    gInputConnected = true;  // proxy in sync
    broadcastOutput("ANO encoder initialized");
  } else {
    ERROR_ANO_ENCODERF("ANO encoder init failed");
  }
  return ok;
}

bool inputStartInternal() {
  if (gInputRunning) return true;
  // Do not reinitialize resources while the previous input task is exiting.
  if (gInputTaskHandle != nullptr) return false;

  DEBUG_CLIF("[QUEUE] Processing ANO encoder start from queue");
  anoLogHeap("start.begin");

  String memReason;
  if (!checkMemoryAvailable("anoencoder", &memReason)) {
    ERROR_ANO_ENCODERF("Insufficient memory for ANO encoder: %s", memReason.c_str());
    return false;
  }

  if (!sAnoCacheMutex) {
    sAnoCacheMutex = xSemaphoreCreateMutex();
    if (!sAnoCacheMutex) {
      ERROR_ANO_ENCODERF("Failed to create ANO cache mutex");
      return false;
    }
  }
  if (!inputCacheInit()) {
    ERROR_ANO_ENCODERF("Failed to create shared input cache mutex");
    return false;
  }

  {
    SensorCacheGuard g(sAnoCacheMutex, pdMS_TO_TICKS(100), "ano.cleanStaleCache");
    if (!g.held) return false;
    {
      sAnoEncoderCache.dataValid = false;
      sAnoEncoderCache.buttons = 0;
      sAnoEncoderCache.encoderDelta = 0;
      sAnoEncoderCache.encoderPosition = 0;
    }
  }

  if (!anoEncoderInit()) {
    anoLogHeap("start.init_fail");
    return false;
  }

  bool prev = gInputRunning;
  gAnoEncoderEnabled = true;
  gInputRunning = true;       // keep proxy in sync for OLED-side checks
  DEBUG_ANO_ENCODER_LIFECYCLEF("[ANO] inputStartInternal: gInputRunning=true (was %d) gInputConnected=%d",
                               prev, gInputConnected);
  // Bump the sensor-status sequence so the SSE event fires and the web UI
  // refreshes its connected/enabled indicators immediately. Without this the
  // dot can stay red until the next polling tick.
  if (gInputRunning != prev) sensorStatusBumpWith("openinput@enabled");


  if (!createInputTask()) {  // Reuse the existing gamepad task slot — only one input device runs.
    gAnoEncoderEnabled = false;
    gInputRunning = false;
    gAnoEncoderConnected = false;
    gInputConnected = false;
    (void)invalidateAnoCache();
    (void)inputCacheInvalidate();
    sensorStatusBumpWith("openinput@task_failed");  // Undo the enable bump above.
    ERROR_ANO_ENCODERF("Failed to create ANO encoder task");
    return false;
  }
  anoLogHeap("start.after_task");
  return true;
}

int anoEncoderConsumeOneDetent() {
  if (!sAnoCacheMutex) return 0;
  SensorCacheGuard g(sAnoCacheMutex, pdMS_TO_TICKS(5), "ano.consumeDetent");
  if (!g.held) return 0;
  if (sAnoEncoderCache.encoderDelta > 0) { sAnoEncoderCache.encoderDelta--; return 1; }
  if (sAnoEncoderCache.encoderDelta < 0) { sAnoEncoderCache.encoderDelta++; return -1; }
  return 0;
}

// The reading: the shared envelope plus pos, axis and buttons. `axis` stays
// in the wire format and is always 0 (the wheel is vertical); `buttons` is
// the device-native ANO_BTN_* layout.
int anoEncoderBuildDataJson(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;
  AnoEncoderCache reading;
  if (!anoEncoderCacheSnapshot(reading, 50)) {
    // Cache-lock timeout: not-ready envelope (was: return 0 / no output).
    int pos = sensorEnvelopeBegin(buf, bufSize, false, gAnoEncoderConnected, 0);
    if (pos == 0) return 0;
    int n = snprintf(buf + pos, bufSize - pos, "," HW1_SK_Q(HW1_SK_POS) "0," HW1_SK_Q(HW1_SK_AXIS) "0," HW1_SK_Q(HW1_SK_BUTTONS) "0}");
    if (n < 0 || (size_t)n >= bufSize - pos) return 0;
    return pos + n;
  }

  int pos = sensorEnvelopeBegin(buf, bufSize, reading.dataValid, gAnoEncoderConnected, reading.lastUpdate);
  if (pos == 0) return 0;
  int n = snprintf(buf + pos, bufSize - pos,
                   "," HW1_SK_Q(HW1_SK_POS) "%ld," HW1_SK_Q(HW1_SK_AXIS) "0," HW1_SK_Q(HW1_SK_BUTTONS) "%lu}",
                   (long)reading.encoderPosition,
                   (unsigned long)reading.buttons);
  if (n < 0 || (size_t)n >= bufSize - pos) return 0;
  return pos + n;
}

// ============================================================================
// Command handlers
// ============================================================================
static bool anoRunning() { return gAnoEncoderEnabled && gAnoEncoderConnected; }

// anoencoderread [json]: the reading. The device is opened through the queue
// (openinput / inputautostart), never by this command.
const char* cmd_anoencoder(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (argWantsJson(argsInput)) {
    if (!ensureDebugBuffer()) return SENSOR_JSON_NOBUF;
    int n = anoEncoderBuildDataJson(getDebugBuffer(), 1024);  // shared builder (also feeds web + ESP-NOW)
    return (n > 0) ? getDebugBuffer() : SENSOR_JSON_UNAVAILABLE;
  }
  if (!anoRunning()) {
    return "Error: The ANO encoder is not open. Start it with 'openinput'; 'anoencoderstatus' "
           "shows its bus and address.";
  }
  AnoEncoderCache reading;
  if (!anoEncoderCacheSnapshot(reading, 50)) return "Error: The ANO reading is busy. Retry the command.";
  // The cache holds a reading only after the polling task's first good poll.
  if (!reading.dataValid) return "Error: The ANO encoder has no reading yet. Retry in a moment.";
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  char names[48];
  snprintf(getDebugBuffer(), 1024, "[ANO] pos=%ld buttons=0x%02lX (%s)", (long)reading.encoderPosition,
           (unsigned long)reading.buttons, anoBtnNames(reading.buttons, names, sizeof(names)));
  return getDebugBuffer();
}

static constexpr const char* kUsageStatus = "anoencoderstatus [json]";

// anoencoderstatus [json]: where the board is looked for, whether it is open,
// and its settings (schema 1).
const char* cmd_anoencoderstatus(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const I2cInstanceArg arg =
      i2cParseInstanceArg(I2C_DEVICE_INPUT, argsInput.c_str(), I2cInstanceForm::Status);
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  if (arg.error != I2cInstanceError::None) {
    return i2cInstanceErrorText(I2C_DEVICE_INPUT, arg, kUsageStatus, getDebugBuffer(), 1024);
  }
  const bool running = anoRunning();
  const unsigned bus = running ? sAnoBus : (unsigned)gSettings.inputBus;
  const unsigned address = running ? sAnoAddress : anoEncoderAddress();
  const int pollMs = gSettings.inputDevicePollMs;
  const bool invert = gSettings.anoEncoderInvert;
  const bool swapUd = gSettings.anoEncoderSwapUpDown;
  const bool swapLr = gSettings.anoEncoderSwapLeftRight;
  const bool autoStart = gSettings.inputAutoStart;
  if (argWantsJson(argsInput)) {
    snprintf(getDebugBuffer(), 1024,
             "{\"schema\":1,\"connected\":%s,\"product\":%u,\"bus\":%u,\"address\":%u,\"pollMs\":%d,"
             "\"invert\":%s,\"swapUpDown\":%s,\"swapLeftRight\":%s,\"autoStart\":%s}",
             running ? "true" : "false", running ? (unsigned)kSeesawProductAnoEncoder : 0u, bus, address,
             pollMs, invert ? "true" : "false", swapUd ? "true" : "false", swapLr ? "true" : "false",
             autoStart ? "true" : "false");
    return getDebugBuffer();
  }
  snprintf(getDebugBuffer(), 1024,
           "[ANO] %s (I2C%u address 0x%02X). Poll %d ms. Rotation %s; UP/DOWN %s; LEFT/RIGHT %s. "
           "Open after boot: %s. %s",
           running ? "Connected: Adafruit 5740 ANO encoder" : "Not connected", bus + 1, address, pollMs,
           invert ? "inverted" : "normal", swapUd ? "swapped" : "normal", swapLr ? "swapped" : "normal",
           autoStart ? "on" : "off",
           running ? "Read it with 'anoencoderread'." : "Start it with 'openinput'.");
  return getDebugBuffer();
}

// anoencoderi2caddr [addr]: 0x49..0x50 in hex or decimal. Refused when
// another device of this build holds it on the input bus; it applies at the
// next openinput.
const char* cmd_anoencoderi2caddr(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String text = argsInput;
  text.trim();
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  if (text.length() == 0) {
    snprintf(getDebugBuffer(), 1024, "[ANO] I2C address: 0x%02X on I2C%u%s", (unsigned)anoEncoderAddress(),
             (unsigned)gSettings.inputBus + 1,
             anoRunning() && sAnoAddress != anoEncoderAddress() ? " (open at the old address until the next 'openinput')" : "");
    return getDebugBuffer();
  }
  int address = 0;
  if (!parseAddressArg(text, kAnoAddressMin, kAnoAddressMax, address)) {
    return "Error: The ANO address must be 0x49..0x50 (its A0-A2 jumpers). Read it with 'i2cscan'.";
  }
  char why[112];
  if (i2cAddressTakenBy((uint8_t)gSettings.inputBus, (uint8_t)address, I2C_DEVICE_INPUT, 1, why, sizeof(why))) {
    snprintf(getDebugBuffer(), 1024,
             "%s Use another address (its A0-A2 jumpers) or put the ANO on the other bus ('inputbus').", why);
    return getDebugBuffer();
  }
  setSetting(gSettings.anoEncoderI2cAddr, address);
  snprintf(getDebugBuffer(), 1024, "[ANO] Address 0x%02X saved; it applies at the next 'openinput'",
           (unsigned)address);
  return getDebugBuffer();
}

// One on/off (or toggle) setting of the ANO: no value shows it.
static const char* anoBoolSetting(bool& field, const String& argsInput, bool allowToggle,
                                  const char* onReply, const char* offReply) {
  String arg = argsInput;
  arg.trim();
  if (arg.length() > 0) {
    bool value = false;
    if (allowToggle && arg.equalsIgnoreCase("toggle")) {
      value = !field;
    } else {
      const int parsed = parseBoolArg(arg);
      if (parsed < 0) return allowToggle ? "Error: Not an on/off value. Use on, off or toggle." : kCommandReplyNotOnOff;
      value = parsed == 1;
    }
    setSetting(field, value);
  }
  return field ? onReply : offReply;
}

const char* cmd_anoencoderinvert(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return anoBoolSetting(gSettings.anoEncoderInvert, argsInput, false, "[ANO] Rotation: inverted",
                        "[ANO] Rotation: normal");
}

const char* cmd_anoencoderswapud(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return anoBoolSetting(gSettings.anoEncoderSwapUpDown, argsInput, true, "[ANO] UP/DOWN buttons: swapped",
                        "[ANO] UP/DOWN buttons: normal");
}

const char* cmd_anoencoderswaplr(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return anoBoolSetting(gSettings.anoEncoderSwapLeftRight, argsInput, true,
                        "[ANO] LEFT/RIGHT buttons: swapped", "[ANO] LEFT/RIGHT buttons: normal");
}

// ============================================================================
// Settings module — only ANO-specific entries. The shared poll/autostart
// settings are in inputSettingsModule (HAL_Input.cpp), the bus (inputBus) in
// the I2C module.
// ============================================================================
static const char kAnoAddressOptions[] = ANO_ADDRESS_OPTIONS;
static const SettingEntry anoEncoderSettingEntries[] = {
  { "anoEncoderI2cAddr",       SETTING_INT,  &gSettings.anoEncoderI2cAddr,       kAnoAddressDefault, 0, nullptr, kAnoAddressMin, kAnoAddressMax, "I2C address (next openinput)", kAnoAddressOptions, false, nullptr, "anoencoderi2caddr" },
  { "anoEncoderInvert",        SETTING_BOOL, &gSettings.anoEncoderInvert,        0,                  0, nullptr, 0, 1,   "Invert rotation",       nullptr, false, nullptr, "anoencoderinvert" },
  { "anoEncoderSwapUpDown",    SETTING_BOOL, &gSettings.anoEncoderSwapUpDown,    1,                  0, nullptr, 0, 1,   "Swap UP/DOWN buttons",  nullptr, false, nullptr, "anoencoderswapud" },
  { "anoEncoderSwapLeftRight", SETTING_BOOL, &gSettings.anoEncoderSwapLeftRight, 1,                  0, nullptr, 0, 1,   "Swap LEFT/RIGHT buttons", nullptr, false, nullptr, "anoencoderswaplr" },
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
// Command registry — only ANO-specific commands. The unified
// open/close/autostart/poll commands live in HAL_Input.cpp (inputCommands).
// ============================================================================
const CommandEntry anoEncoderCommands[] = {
  { "anoencoderread",    "Read the ANO encoder: wheel position and buttons. (add 'json' for JSON output)", false, cmd_anoencoder },
  { "anoencoderstatus",  "ANO encoder status: bus, address, settings. (add 'json' for JSON output)", false, cmd_anoencoderstatus, "Usage: anoencoderstatus [json]" },
  { "anoencoderi2caddr", "Set the ANO I2C address [0x49-0x50] (next openinput)", true,  cmd_anoencoderi2caddr, "Usage: anoencoderi2caddr <0x49..0x50>" },
  { "anoencoderinvert",  "Invert rotation direction [on|off]",       true, cmd_anoencoderinvert,  "Usage: anoencoderinvert [on|off]" },
  { "anoencoderswapud",  "Swap UP/DOWN buttons [on|off|toggle]",     true, cmd_anoencoderswapud,  "Usage: anoencoderswapud [on|off|toggle]" },
  { "anoencoderswaplr",  "Swap LEFT/RIGHT buttons [on|off|toggle]",  true, cmd_anoencoderswaplr,  "Usage: anoencoderswaplr [on|off|toggle]" },
};
const size_t anoEncoderCommandsCount = sizeof(anoEncoderCommands) / sizeof(anoEncoderCommands[0]);

// ============================================================================
// Polling task — driven by createInputTask() (shared slot)
// ============================================================================
// Each poll is one checked transaction: buttons, interrupt flags, then the
// wheel delta (read-to-clear, so last). anoPoll() (i2csensor_ano_encoder_core.h)
// turns the reads into the reading, the button events and the navigation's
// edges and level (RIGHT tap = SELECT at release, RIGHT+IN = START only).
// The OLED drains the accumulated detents each frame.

void inputTask(void* parameter) {
  INFO_ANO_ENCODER_LIFECYCLEF("ANO encoder task started (handle=%p)", (void*)xTaskGetCurrentTaskHandle());
  anoLogHeap("task.entry");

  unsigned long lastRead = 0;
  unsigned long lastStackLog = 0;
  uint8_t consecutiveInvalidReads = 0;
  static const uint8_t INVALID_READ_AUTO_DISABLE_THRESHOLD = 20;

  AnoPollState pollState;
  int32_t position = 0;  // the reading's wheel count since this open

  // POLL-log de-noise state: an [ANO_POLL] line on a change of the raw reads
  // or the wheel, plus a once-per-second heartbeat so a stuck task is still
  // visible.
  uint32_t      lastLoggedBulk  = 0xFFFFFFFEu;
  unsigned long lastPollLogMs   = 0;
  const unsigned long POLL_LOG_HEARTBEAT_MS = 1000;

  while (true) {
    if (!gAnoEncoderEnabled) {
      gAnoEncoderConnected = false;
      if (!invalidateAnoCache()) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
      gInputRunning = false;
      gInputConnected = false;
      if (!inputCacheInvalidate()) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
      SENSOR_TASK_EXIT(INPUT, gInputTaskHandle);
    }

    unsigned long nowMs = millis();
    if ((nowMs - lastStackLog) >= 30000) {
      lastStackLog = nowMs;
      // 'continue' (not 'break') so the top-of-loop shutdown runs the clean
      // SENSOR_TASK_EXIT (vTaskDelete) path instead of returning from the task
      // function with a near-overflowed stack (IllegalInstruction panic).
      if (checkTaskStackSafety("anoencoder", INPUT_STACK_WORDS, &gAnoEncoderEnabled)) continue;
    }

    const uint8_t bus = sAnoBus;
    const uint8_t addr = sAnoAddress;
    if (gAnoEncoderEnabled && gAnoEncoderConnected && !pollPaused(bus)) {
      unsigned long pollMs = (gSettings.inputDevicePollMs > 0)
                              ? (unsigned long)gSettings.inputDevicePollMs : 90;
      if ((nowMs - lastRead) >= pollMs) {
        AnoPollReads reads;
        AnoSeesaw* seesaw = gAnoSeesaw;
        // The I2C manager records the result: a NACK or an unplugged board is
        // a failed transaction, which its health records and auto-disable see.
        const bool readOk = seesaw && i2cDeviceTransaction(bus, addr, 400000, 80, [&]() -> bool {
          reads.buttonsOk = seesaw->readButtons(reads.bulk);
          reads.flagsOk = reads.buttonsOk && seesaw->readIntFlags(reads.intFlags);
          reads.deltaOk = reads.flagsOk && seesaw->readDelta(reads.delta);
          return reads.deltaOk;
        });

        const AnoPollResult r = anoPoll(pollState, reads, gSettings.anoEncoderSwapUpDown,
                                        gSettings.anoEncoderSwapLeftRight, gSettings.anoEncoderInvert);

        const bool pollChanged = (reads.bulk != lastLoggedBulk) || reads.intFlags || r.rawDelta;
        const bool pollHeartbeat = (nowMs - lastPollLogMs) >= POLL_LOG_HEARTBEAT_MS;
        if (pollChanged || pollHeartbeat) {
          DEBUG_ANO_ENCODER_POLLINGF("[ANO_POLL] bulk=0x%08lX flags=0x%08lX delta=%ld readOk=%d valid=%d%s",
                                     (unsigned long)reads.bulk, (unsigned long)reads.intFlags,
                                     (long)reads.delta, readOk ? 1 : 0, r.buttonsValid ? 1 : 0,
                                     pollChanged ? "" : " (hb)");
          lastLoggedBulk = reads.bulk;
          lastPollLogMs = nowMs;
        }

        // Turns read off the chip count even when a button part failed.
        position += r.rawDelta;
        if (r.rawDelta != 0) {
          DEBUG_ANO_ENCODER_VALUESF("[ANO_VAL] rotate raw=%ld -> detent=%ld (invert=%d) pos=%ld",
                                    (long)r.rawDelta, (long)r.detents,
                                    gSettings.anoEncoderInvert ? 1 : 0, (long)position);
        }

        if (r.buttonsValid) {
          consecutiveInvalidReads = 0;
          if (r.presses) {
            char nameBuf[48];
            DEBUG_ANO_ENCODER_VALUESF("[ANO_VAL] press   raw=0x%02lX (%s)", (unsigned long)r.presses,
                                      anoBtnNames(r.presses, nameBuf, sizeof(nameBuf)));
            // Bus event: physical presses (taps included); releases are nav noise.
            systemEventPost(SYSEVT_BUTTON, anoBtnNames(r.presses, nameBuf, sizeof(nameBuf)));
          }
          if (r.edges) {
            char nameBuf[48];
            DEBUG_ANO_ENCODER_VALUESF("[ANO_VAL] edges   0x%05lX (%s)", (unsigned long)r.edges,
                                      anoBtnNames(r.edges, nameBuf, sizeof(nameBuf)));
          }
        }

        if (r.buttonsValid || r.detents != 0) {
          // ----- Native ANO reading -----
          {
            SensorCacheGuard g(sAnoCacheMutex, pdMS_TO_TICKS(50), "ano.pollWrite");
            if (g.held) {
              const bool changed = (sAnoEncoderCache.buttons != r.buttons) || (r.detents != 0);
              sAnoEncoderCache.buttons = r.buttons;
              sAnoEncoderCache.encoderPosition = position;
              sAnoEncoderCache.encoderDelta += r.detents;
              sAnoEncoderCache.lastUpdate = nowMs;
              sAnoEncoderCache.dataValid = sAnoEncoderCache.dataValid || r.buttonsValid;
              if (changed) sAnoEncoderCache.seq++;
            }
          }

          // ----- Gamepad-shaped proxy for the OLED -----
          // Active-low like the gamepad: the physical buttons (~buttons), so
          // the sensor log and the G2 page see RIGHT and the chord's IN. The
          // navigation hides r.navHidden itself (RIGHT, whose SELECT comes at
          // release, and the chord's IN), published under the same lock. The
          // edges carry the presses, taps, START and SELECT. Joystick X/Y stay
          // at CENTER: the wheel reaches the OLED as detents, not joystick math.
          inputPublishState(~r.buttons, JOYSTICK_CENTER, JOYSTICK_CENTER, r.edges,
                            r.edges != 0 || r.presses != 0 || r.detents != 0, nowMs,
                            r.navHidden);
        }

        if (!readOk) {
          uint8_t errors = i2cGetConsecutiveErrors(addr, bus);
          WARN_ANO_ENCODERF("[ANO_TASK] I2C read failure (consecutive: %u)", errors);
          if (i2cShouldAutoDisable(addr, bus)) {
            ERROR_ANO_ENCODERF("[ANO_TASK] Too many failures - auto-disabling");
            noteSensorAutoDisabled(I2C_DEVICE_INPUT);  // before the stop: see System_I2C.h
            requestSensorStop(I2C_DEVICE_INPUT);
            logSystemEvent("SENSOR", "ANO Encoder auto-disabled after %u consecutive I2C failures", errors);
            { char det[24]; snprintf(det, sizeof(det), "%u I2C errors", errors);
              systemEventPost(SYSEVT_SENSOR_FAULT, "ANO", det); }
          }
        } else if (!r.buttonsValid) {
          // Read fine, but all five switches pressed: a bus held low.
          consecutiveInvalidReads++;
          if (consecutiveInvalidReads >= INVALID_READ_AUTO_DISABLE_THRESHOLD) {
            ERROR_ANO_ENCODERF("[ANO_TASK] %u invalid reads - auto-disabling", consecutiveInvalidReads);
            noteSensorAutoDisabled(I2C_DEVICE_INPUT);  // before the stop: see System_I2C.h
            requestSensorStop(I2C_DEVICE_INPUT);
            logSystemEvent("SENSOR", "ANO Encoder auto-disabled after %u consecutive invalid reads (device likely disconnected)", consecutiveInvalidReads);
            { char det[24]; snprintf(det, sizeof(det), "%u invalid reads", (unsigned)consecutiveInvalidReads);
              systemEventPost(SYSEVT_SENSOR_FAULT, "ANO", det); }
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
