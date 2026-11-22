#include "i2csensor_encoder.h"

#if ENABLE_ENCODER
#include <esp_attr.h>  // EXT_RAM_BSS_ATTR

// Every instance's saved settings, instance 1 included, in PSRAM (internal
// SRAM is scarce). Zero until the settings load: the boot applies the rows'
// defaults (below) first.
EXT_RAM_BSS_ATTR EncoderInstanceSettings gEncoderInst[kEncoderMaxInstances];
#include <Arduino.h>
#include <Adafruit_seesaw.h>
#include <Wire.h>
#include <atomic>
#include <new>

#include "System_Command.h"
#include "System_CommandRegistry.h"
#include "System_Debug.h"
#include "System_Events.h"           // systemEventPost: the fault event of the auto-disable site
#include "System_I2C.h"
#include "System_I2CInstanceCore.h"  // the instance argument (G1-G3), names
#include "System_MemUtil.h"          // ps_alloc: the seesaw objects live in PSRAM
#include "System_Mutex.h"            // SensorCacheGuard
#include "System_SeesawIdentity.h"
#include "System_Settings.h"
#include "System_Utils.h"

static_assert(kI2cTypes[I2C_DEVICE_ENCODER].maxInstances == kEncoderMaxInstances,
              "the device's instances are the encoder type's (System_I2CInstanceCore.h)");

namespace {

constexpr uint32_t kInitClockHz = 100000;
constexpr uint32_t kFrameClockHz = 400000;
constexpr uint32_t kInitLockWaitMs = 3000;   // queue worker: a start may wait for the bus
constexpr uint32_t kStopLockWaitMs = 50;

// The board's NeoPixel registers, written directly: one checked write of
// every pixel and a checked show is a frame, and the library's own pixel
// buffer (a heap allocation) is never made.
class EncoderPixels : public Adafruit_seesaw {
 public:
  explicit EncoderPixels(TwoWire* wire) : Adafruit_seesaw(wire) {}

  // Caller holds the bus. GRB at 800 kHz on the board's pixel pin.
  bool configure(const EncoderBoard& board) {
    const uint16_t bytes = (uint16_t)(3 * board.pixelCount);
    uint8_t length[2] = { (uint8_t)(bytes >> 8), (uint8_t)(bytes & 0xFF) };
    return write8(SEESAW_NEOPIXEL_BASE, SEESAW_NEOPIXEL_SPEED, 1) &&
           write(SEESAW_NEOPIXEL_BASE, SEESAW_NEOPIXEL_BUF_LENGTH, length, sizeof(length)) &&
           write8(SEESAW_NEOPIXEL_BASE, SEESAW_NEOPIXEL_PIN, board.pixelPin);
  }

  // Caller holds the bus.
  bool writeFrame(const EncoderFrame& frame, uint8_t count) {
    uint8_t bytes[2 + 3 * kEncoderMaxPixels] = {};  // buffer offset 0, then GRB per pixel
    if (count > kEncoderMaxPixels) count = kEncoderMaxPixels;
    for (uint8_t pixel = 0; pixel < count; ++pixel) {
      bytes[2 + 3 * pixel] = frame.pixel[pixel].g;
      bytes[3 + 3 * pixel] = frame.pixel[pixel].r;
      bytes[4 + 3 * pixel] = frame.pixel[pixel].b;
    }
    return write(SEESAW_NEOPIXEL_BASE, SEESAW_NEOPIXEL_BUF, bytes, (uint8_t)(2 + 3 * count)) &&
           write(SEESAW_NEOPIXEL_BASE, SEESAW_NEOPIXEL_SHOW, nullptr, 0);
  }
};

// Whether a board is meant to be open: opened by hand, closed by hand, or
// (until either) its start-at-boot setting. A command reconnects only a
// board that is meant to be open.
enum class HandState : uint8_t { FollowAutoStart, Opened, Closed };

// One board: instance n is encoders()[n - 1].
struct EncoderInstance {
  // Retained across stops and restarts, bound to the bus of the first open
  // (bus changes need a reboot). PSRAM-preferred placement new.
  EncoderPixels* pixels = nullptr;
  uint8_t pixelsBus = 0;

  std::atomic<bool> running{false};
  std::atomic<bool> stopRequested{false};  // the auto-disable site asked once
  uint8_t bus = 0;                         // latched by the running open
  uint8_t address = 0;
  const EncoderBoard* board = nullptr;     // the board last detected

  // What the light shows. Commands change it, the main loop's tick reads it;
  // the lock is held for the state only, never across a bus transfer.
  EncoderLight light;

  std::atomic<HandState> handState{HandState::FollowAutoStart};
  EncoderReconnect reconnect;

  // The light's state lock, created on first use (lightMutex()).
  std::atomic<SemaphoreHandle_t> mutex{nullptr};
};

// The instances live in PSRAM and are constructed once, on first use, by a
// function-local static (initialized once, thread-safely). An object with
// initial values is not placed in the external bss directly: that section is
// zero-filled at boot, so constant-initialized values would be lost.
EXT_RAM_BSS_ATTR alignas(EncoderInstance) uint8_t
    sEncoderStorage[sizeof(EncoderInstance) * kEncoderMaxInstances];

EncoderInstance* encoders() {
  static EncoderInstance* const all = [] {
    EncoderInstance* first = reinterpret_cast<EncoderInstance*>(sEncoderStorage);
    for (uint8_t i = 0; i < kEncoderMaxInstances; ++i) new (&first[i]) EncoderInstance();
    return first;
  }();
  return all;
}

// Instance n, or nullptr for an n the device does not have.
EncoderInstance* encoderAt(uint8_t n) {
  return (n >= 1 && n <= kEncoderMaxInstances) ? &encoders()[n - 1] : nullptr;
}

// Created on first use by whichever task gets there first; a task that lost
// the race deletes its own and uses the winner's. An instance that is never
// used never takes a lock from the internal heap.
SemaphoreHandle_t lightMutex(EncoderInstance& e) {
  SemaphoreHandle_t mutex = e.mutex.load();
  if (mutex) return mutex;
  SemaphoreHandle_t created = xSemaphoreCreateMutex();
  if (!created) return nullptr;
  SemaphoreHandle_t expected = nullptr;
  if (e.mutex.compare_exchange_strong(expected, created)) return created;
  vSemaphoreDelete(created);
  return expected;
}

// "Encoder" / "Encoder 2" (a stable text, System_I2C.cpp).
const char* displayName(uint8_t n) { return deviceTypeDisplayName(I2C_DEVICE_ENCODER, n); }

// "encoder" / "encoder2": the instance's LED target and id.
const char* targetId(uint8_t n, char* buf, size_t len) {
  return i2cInstanceId(I2C_DEVICE_ENCODER, n, buf, len);
}

// " 2" after a command word for instance 2, "" for instance 1.
const char* instanceSuffix(uint8_t n, char* buf, size_t len) {
  if (n == 1) buf[0] = '\0';
  else snprintf(buf, len, " %u", (unsigned)n);
  return buf;
}

EncoderInstanceSettings& saved(uint8_t n) { return gEncoderInst[n - 1]; }

bool boardWanted(EncoderInstance& e, uint8_t n) {
  switch (e.handState.load()) {
    case HandState::Opened: return true;
    case HandState::Closed: return false;
    case HandState::FollowAutoStart: break;
  }
  return saved(n).autoStart;
}

uint8_t brightnessSetting(uint8_t n) {
  const int value = saved(n).brightness;
  return (uint8_t)(value < 0 ? 0 : (value > 100 ? 100 : value));
}

// A command that needs the board and finds it not running. A board meant to
// be open (and with an address) starts a reconnect through the sensor queue
// at most every two seconds.
const char* notConnectedReply(EncoderInstance& e, uint8_t n) {
  bool started = false;
  if (boardWanted(e, n) && encoderInstanceAddress(n) != 0 && e.reconnect.due(millis())) {
    SensorStartRefusal why = SensorStartRefusal::None;
    started = requestSensorStart(I2C_DEVICE_ENCODER, "encoder@reconnect", &why, n) ||
              why == SensorStartRefusal::AlreadyQueued;
  }
  if (n == 1 || !ensureDebugBuffer()) return encoderNotConnectedReply(started);
  if (started) {
    snprintf(getDebugBuffer(), 1024,
             "Error: %s is not connected. Reconnecting now; retry in a few seconds.", displayName(n));
  } else {
    snprintf(getDebugBuffer(), 1024,
             "Error: %s is not connected. Check its cable and address with 'i2cscan', "
             "then run 'openencoder %u'.", displayName(n), (unsigned)n);
  }
  return getDebugBuffer();
}

// A one-pixel target past the board's count: the reply names the count.
const char* pixelRangeReply(EncoderInstance& e, uint8_t pixel, uint8_t n) {
  if (!ensureDebugBuffer()) {
    return "Error: The encoder board has no such pixel. Run 'ledstatus' to see its pixels.";
  }
  const unsigned count = e.light.pixelCount;
  const char* name = e.board ? e.board->name : "encoder board";
  char id[16];
  targetId(n, id, sizeof(id));
  if (count <= 1) {
    snprintf(getDebugBuffer(), 1024,
             "Error: The %s has 1 pixel, so %s:%u is not one. Use %s:1 or %s.",
             name, id, (unsigned)pixel, id, id);
  } else {
    snprintf(getDebugBuffer(), 1024,
             "Error: The %s has %u pixels, so %s:%u is not one. Use %s:1 to %s:%u, "
             "or %s for all.", name, count, id, (unsigned)pixel, id, id, count, id);
  }
  return getDebugBuffer();
}

constexpr const char* kReplyStateBusy = "Error: The encoder light state is busy. Retry.";
constexpr const char* kReplyNoInstance = "Error: No such encoder instance.";

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
// Instance 1's rows keep their keys and commands; instances 2..4 get flat,
// numbered rows from ENCODER_INSTANCE_ROWS (i2csensor_encoder_light.h):
// encoder2Address with "encoderaddress 2", and so on. Every instance has the
// same four fields in the same order, so instance n's row of a field is
// settings[(n - 1) * kEncoderSettingFields + field].
enum EncoderSettingField : uint8_t { kFieldAutoStart = 0, kFieldBrightness, kFieldAddress, kFieldBus };

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
constexpr SettingEntry settings[] = {
  { "encoderAutoStart", SETTING_BOOL, &gEncoderInst[0].autoStart, 0, 0, nullptr, 0, 1,
    "Open after boot", nullptr, false, nullptr, "encoderautostart" },
  { "encoderBrightness", SETTING_INT, &gEncoderInst[0].brightness, kEncoderDefaultBrightness, 0, nullptr,
    0, 100, "Brightness (%)", nullptr, false, nullptr, "ledbrightness encoder" },
  { "encoderAddress", SETTING_INT, &gEncoderInst[0].address, kEncoderDefaultAddress, 0, nullptr,
    0x36, 0x50, "I2C address (next open)", kEncoderAddressOptions, false, nullptr, "encoderaddress" },
  { "encoderBus", SETTING_INT, &gEncoderInst[0].bus, 0, 0, nullptr, 0, 1,
    "I2C bus (reboot required)", kEncoderBusOptions, false, nullptr, "encoderbus" },
  ENCODER_INSTANCE_ROWS(2),
  ENCODER_INSTANCE_ROWS(3),
  ENCODER_INSTANCE_ROWS(4),
};
constexpr size_t kSettingsCount = sizeof(settings) / sizeof(settings[0]);

// The table, checked at build time (as the generated debug rows are,
// System_Settings.cpp).
static_assert(kEncoderMaxInstances == 4, "one ENCODER_INSTANCE_ROWS line per instance 2..max");
static_assert(kSettingsCount == (size_t)kEncoderMaxInstances * kEncoderSettingFields,
              "every instance has its four rows (generated rows = (max - 1) x fields)");

constexpr bool settingTextEqual(const char* a, const char* b) {
  if (!a || !b) return a == b;
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}

// Each storage field is named by exactly its own row: instance n's field f is
// row (n - 1) * fields + f, with the field's type.
constexpr bool settingsPointAtTheirFields() {
  for (uint8_t i = 0; i < kEncoderMaxInstances; ++i) {
    const SettingEntry* row = &settings[i * kEncoderSettingFields];
    if (row[kFieldAutoStart].valuePtr != &gEncoderInst[i].autoStart ||
        row[kFieldAutoStart].type != SETTING_BOOL) return false;
    if (row[kFieldBrightness].valuePtr != &gEncoderInst[i].brightness ||
        row[kFieldBrightness].type != SETTING_INT) return false;
    if (row[kFieldAddress].valuePtr != &gEncoderInst[i].address ||
        row[kFieldAddress].type != SETTING_INT) return false;
    if (row[kFieldBus].valuePtr != &gEncoderInst[i].bus || row[kFieldBus].type != SETTING_INT) return false;
  }
  return true;
}
static_assert(settingsPointAtTheirFields(), "a row points at another instance's or field's storage");

// No two rows share a jsonKey or a cmdKey, and no name column is empty.
constexpr bool settingsNamedOnce() {
  for (size_t i = 0; i < kSettingsCount; ++i) {
    const SettingEntry& a = settings[i];
    if (!a.jsonKey || !a.jsonKey[0] || !a.label || !a.label[0] || !a.cmdKey || !a.cmdKey[0]) return false;
    for (size_t j = i + 1; j < kSettingsCount; ++j) {
      if (settingTextEqual(a.jsonKey, settings[j].jsonKey)) return false;
      if (settingTextEqual(a.cmdKey, settings[j].cmdKey)) return false;
    }
  }
  return true;
}
static_assert(settingsNamedOnce(), "encoder settings rows need unique jsonKey/cmdKey and non-empty columns");

constexpr const SettingEntry* settingRow(uint8_t n, EncoderSettingField field) {
  return &settings[(n - 1) * kEncoderSettingFields + field];
}

bool isConnected() { return encoderAnyRunning(); }

// The reply when (bus, address) is already used by another device of this
// build (System_I2C.h, i2cAddressTakenBy): the board's own fuel gauge at 0x36
// (FeatherS3[D]), an OLED, the APDS9960, the gamepad, another encoder
// instance, ... `hint` says what to change. nullptr when it is free.
const char* addressClashReply(uint8_t bus, uint8_t address, uint8_t n, const char* hint) {
  char why[112];
  if (!i2cAddressTakenBy(bus, address, I2C_DEVICE_ENCODER, n, why, sizeof(why))) return nullptr;
  if (!ensureDebugBuffer()) return "Error: That address is in use on that bus.";
  snprintf(getDebugBuffer(), 1024, "%s %s", why, hint);
  return getDebugBuffer();
}

// ---------------------------------------------------------------------------
// Commands, with the instance argument (System_I2CInstanceCore.h, design
// section 3): open/close take [n] (G1), status takes [n] and json anywhere
// (G2), a setter's one token is instance 1's value and two tokens are the
// instance and its value (G3).
// ---------------------------------------------------------------------------

// The reply for an instance argument that does not parse, or nullptr.
const char* instanceArgError(const I2cInstanceArg& arg, const char* usage) {
  if (arg.error == I2cInstanceError::None) return nullptr;
  if (!ensureDebugBuffer()) return "Error: Not an encoder instance.";
  return i2cInstanceErrorText(I2C_DEVICE_ENCODER, arg, usage, getDebugBuffer(), 1024);
}

// A setter's one token that is no valid value but names an instance n >= 2:
// "Error: '2' is not on or off. To set instance 2: encoderautostart 2 on".
// nullptr when the token names no instance.
const char* notValueButInstance(const I2cInstanceArg& arg, const char* expected, const char* command,
                                const char* example) {
  if (!i2cInstanceTokenIsInstance(I2C_DEVICE_ENCODER, arg.rest, arg.restLen)) return nullptr;
  if (!ensureDebugBuffer()) return "Error: Not a valid value.";
  return i2cInstanceNotValueText(arg.rest, arg.restLen, expected, command, example, getDebugBuffer(), 1024);
}

// The value text of a setter (the arguments after the instance, if any).
String setterValue(const I2cInstanceArg& arg) {
  return String(arg.rest).substring(0, arg.restLen);
}

constexpr const char* kUsageOpen = "openencoder [n]";
constexpr const char* kUsageClose = "closeencoder [n]";
constexpr const char* kUsageStatus = "encoderstatus [n] [json]";
constexpr const char* kUsageAutoStart = "encoderautostart [n] <on|off>";
constexpr const char* kUsageAddress = "encoderaddress [n] <0x49..0x50|0x36..0x3D>";
constexpr const char* kUsageBus = "encoderbus [n] <0|1>";

const char* cmd_openencoder(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const I2cInstanceArg arg =
      i2cParseInstanceArg(I2C_DEVICE_ENCODER, argsInput.c_str(), I2cInstanceForm::OpenClose);
  if (const char* error = instanceArgError(arg, kUsageOpen)) return error;
  const uint8_t n = arg.n;
  if (encoderInstanceAddress(n) == 0) {
    if (!ensureDebugBuffer()) return "Error: That encoder has no address. Set one with 'encoderaddress'.";
    return i2cInstanceNoAddressText(I2C_DEVICE_ENCODER, n, "encoderaddress", getDebugBuffer(), 1024);
  }
  EncoderInstance* e = encoderAt(n);
  if (!e) return kReplyNoInstance;
  e->handState = HandState::Opened;
  if (n == 1) {
    return cmd_sensorstart_queued(I2C_DEVICE_ENCODER, displayName(n), "openencoder@enqueue",
                                  "the board opens in the background - light it with 'ledcolor encoder <colour>'");
  }
  EXT_RAM_BSS_ATTR static char hint[88];
  snprintf(hint, sizeof(hint), "the board opens in the background - light it with 'ledcolor encoder%u <colour>'",
           (unsigned)n);
  return cmd_sensorstart_queued(I2C_DEVICE_ENCODER, displayName(n), "openencoder@enqueue", hint, n);
}

const char* cmd_closeencoder(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const I2cInstanceArg arg =
      i2cParseInstanceArg(I2C_DEVICE_ENCODER, argsInput.c_str(), I2cInstanceForm::OpenClose);
  if (const char* error = instanceArgError(arg, kUsageClose)) return error;
  EncoderInstance* e = encoderAt(arg.n);
  if (!e) return kReplyNoInstance;
  e->handState = HandState::Closed;
  return cmd_sensorstop_queued(I2C_DEVICE_ENCODER, displayName(arg.n), arg.n);
}

const char* autoStartReply(uint8_t n) {
  if (n == 1) {
    return saved(1).autoStart ? "[Encoder] Open after boot: on" : "[Encoder] Open after boot: off";
  }
  if (!ensureDebugBuffer()) return saved(n).autoStart ? "Open after boot: on" : "Open after boot: off";
  snprintf(getDebugBuffer(), 1024, "[%s] Open after boot: %s", displayName(n),
           saved(n).autoStart ? "on" : "off");
  return getDebugBuffer();
}

const char* cmd_encoderautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const I2cInstanceArg arg =
      i2cParseInstanceArg(I2C_DEVICE_ENCODER, argsInput.c_str(), I2cInstanceForm::Setter);
  if (const char* error = instanceArgError(arg, kUsageAutoStart)) return error;
  if (arg.restLen == 0) return autoStartReply(1);  // no value: instance 1's, as before
  const int parsed = parseBoolArg(setterValue(arg));
  if (parsed < 0) {
    if (arg.n == 1) {
      if (const char* hint = notValueButInstance(arg, "on or off", "encoderautostart", "on")) return hint;
    }
    return kCommandReplyNotOnOff;
  }
  setSetting(saved(arg.n).autoStart, parsed == 1);
  return autoStartReply(arg.n);
}

const char* cmd_encoderstatus(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const I2cInstanceArg arg =
      i2cParseInstanceArg(I2C_DEVICE_ENCODER, argsInput.c_str(), I2cInstanceForm::Status);
  if (const char* error = instanceArgError(arg, kUsageStatus)) return error;
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  const uint8_t n = arg.n;
  EncoderInstance* found = encoderAt(n);
  if (!found) return kReplyNoInstance;
  EncoderInstance& e = *found;
  const bool running = e.running.load();
  const EncoderBoard* board = e.board;
  const uint8_t address = running ? e.address : encoderInstanceAddress(n);
  const uint8_t bus = running ? e.bus : encoderInstanceBus(n);
  const unsigned brightness = brightnessSetting(n);
  const bool autoStart = saved(n).autoStart;
  if (argWantsJson(argsInput)) {
    // Instance 1's object is unchanged; instance n >= 2 adds "instance" (and
    // its address is 0 while none is set).
    char instanceKey[20] = "";
    if (n != 1) snprintf(instanceKey, sizeof(instanceKey), ",\"instance\":%u", (unsigned)n);
    snprintf(getDebugBuffer(), 1024,
             "{\"schema\":1%s,\"connected\":%s,\"product\":%u,\"board\":\"%s\",\"pixels\":%u,"
             "\"bus\":%u,\"address\":%u,\"brightness\":%u,\"autoStart\":%s}",
             instanceKey, running ? "true" : "false", (unsigned)(board ? board->product : 0),
             board ? board->name : "", (unsigned)(board ? board->pixelCount : 0), (unsigned)bus,
             (unsigned)address, brightness, autoStart ? "true" : "false");
    return getDebugBuffer();
  }
  char id[16];
  char suffix[6];
  targetId(n, id, sizeof(id));
  instanceSuffix(n, suffix, sizeof(suffix));
  if (running && board) {
    snprintf(getDebugBuffer(), 1024,
             "[%s] Connected: Adafruit %u %s, %u pixel%s, I2C%u address 0x%02X. Brightness %u%%. "
             "Open after boot: %s. Light it with 'ledcolor %s <colour>'; 'ledstatus' shows its pixels.",
             displayName(n), (unsigned)board->product, board->name, (unsigned)board->pixelCount,
             board->pixelCount == 1 ? "" : "s", (unsigned)bus + 1, (unsigned)address, brightness,
             autoStart ? "on" : "off", id);
  } else if (address == 0) {
    snprintf(getDebugBuffer(), 1024,
             "[%s] No address set (I2C%u). Brightness %u%%. Open after boot: %s. "
             "Set one with 'encoderaddress%s <addr>', then 'openencoder%s'.",
             displayName(n), (unsigned)bus + 1, brightness, autoStart ? "on" : "off", suffix, suffix);
  } else {
    snprintf(getDebugBuffer(), 1024,
             "[%s] Not connected (I2C%u address 0x%02X). Brightness %u%%. Open after boot: %s. "
             "Start it with 'openencoder%s'.",
             displayName(n), (unsigned)bus + 1, (unsigned)address, brightness, autoStart ? "on" : "off",
             suffix);
  }
  return getDebugBuffer();
}

const char* cmd_encoderaddress(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const I2cInstanceArg arg =
      i2cParseInstanceArg(I2C_DEVICE_ENCODER, argsInput.c_str(), I2cInstanceForm::Setter);
  if (const char* error = instanceArgError(arg, kUsageAddress)) return error;
  if (arg.restLen == 0) return handleSettingCommand(settingRow(1, kFieldAddress), String());  // show instance 1
  const uint8_t n = arg.n;
  const String text = setterValue(arg);
  int address = 0;
  // Instances 2..4 are unset again with 0.
  const bool unset = n != 1 && parseAddressArg(text, 0, 0, address);
  if (!unset && (!parseAddressArg(text, 0x36, 0x50, address) || !encoderAddressValid(address))) {
    if (n == 1) {
      if (const char* hint = notValueButInstance(arg, "an encoder address", "encoderaddress", "0x37")) {
        return hint;
      }
      return "Error: The encoder address must be 0x49..0x50 (5752) or 0x36..0x3D (4991). "
             "Read it with 'i2cscan'.";
    }
    return "Error: The encoder address must be 0x49..0x50 (5752) or 0x36..0x3D (4991), or 0 to "
           "unset it. Read it with 'i2cscan'.";
  }
  if (!unset) {
    if (const char* clash = addressClashReply(encoderInstanceBus(n), (uint8_t)address, n,
                                              "Use another address (its A0-A2 jumpers) or put the "
                                              "encoder on the other bus ('encoderbus').")) {
      return clash;
    }
  }
  setSetting(saved(n).address, address);
  if (n == 1) return "[Encoder] Address saved; it applies at the next 'openencoder'";
  if (!ensureDebugBuffer()) return unset ? "Address cleared" : "Address saved";
  if (unset) {
    snprintf(getDebugBuffer(), 1024, "[%s] Address cleared; it is not opened until one is set",
             displayName(n));
  } else {
    snprintf(getDebugBuffer(), 1024, "[%s] Address saved; it applies at the next 'openencoder %u'",
             displayName(n), (unsigned)n);
  }
  return getDebugBuffer();
}

const char* cmd_encoderbus(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const I2cInstanceArg arg =
      i2cParseInstanceArg(I2C_DEVICE_ENCODER, argsInput.c_str(), I2cInstanceForm::Setter);
  if (const char* error = instanceArgError(arg, kUsageBus)) return error;
  const uint8_t n = arg.n;
  // A bus where the saved address is already used is refused; the setter
  // reports every other case (usage, not a number, out of range).
  const String text = setterValue(arg);
  int bus = 0;
  if (text.length() > 0 && parseIntArg(text, bus) && bus >= 0 && bus < (int)I2cDeviceManager::NUM_BUSES) {
    const uint8_t address = encoderInstanceAddress(n);
    if (address != 0) {
      if (const char* clash = addressClashReply((uint8_t)bus, address, n, "Change 'encoderaddress' first.")) {
        return clash;
      }
    }
  } else if (n == 1 && text.length() > 0) {
    if (const char* hint = notValueButInstance(arg, "a bus (0 or 1)", "encoderbus", "1")) return hint;
  }
  return i2cSetDeviceBusAndReport(saved(n).bus, text, settingRow(n, kFieldBus)->jsonKey);
}

// One instance's frame: each open board keeps its own pacing, and a busy bus
// skips only that board's frame.
void tickInstance(EncoderInstance& e, uint8_t n) {
  if (!e.running.load() || !e.pixels || e.stopRequested.load()) return;
  const uint32_t nowMs = millis();
  EncoderFrame frame;
  uint8_t count = 0;
  {
    SensorCacheGuard guard(lightMutex(e), 0, "encoder.tick");
    if (!guard.held) return;
    // The saved brightness, however it was set (ledbrightness encoder, or a
    // settings editor writing the row): a change is a changed frame.
    e.light.brightnessPercent = brightnessSetting(n);
    if (!e.light.next(nowMs, frame)) return;
    count = e.light.pixelCount;
  }
  bool ran = false;
  EncoderPixels* pixels = e.pixels;
  const bool written = i2cDeviceTransaction(e.bus, e.address, kFrameClockHz, kEncoderLockWaitMs,
                                            [&]() -> bool {
    ran = true;
    // Stopped meanwhile: the stop owns the pixels now.
    if (!e.running.load()) return true;
    return pixels->writeFrame(frame, count);
  });
  // Not run: a busy bus, or a device the manager skips as degraded.
  bool degraded = false;
  if (!written && !ran) {
    I2cDeviceManager* mgr = i2c();
    I2cDevice* device = mgr ? mgr->getDevice(e.address, e.bus) : nullptr;
    degraded = device && device->isDegraded();
  }
  const EncoderFrameResult result = encoderFrameResult(written, ran, degraded);
  bool failedTooOften = false;
  {
    SensorCacheGuard guard(lightMutex(e), pdMS_TO_TICKS(5), "encoder.settle");
    if (guard.held) failedTooOften = e.light.settle(result, frame, nowMs);
  }
  if (failedTooOften && !e.stopRequested.exchange(true)) {
    const char* name = displayName(n);
    ERROR_I2CF("[%s] %u writes failed in a row - closing the board", name,
               (unsigned)kEncoderFailuresBeforeStop);
    noteSensorAutoDisabled(I2C_DEVICE_ENCODER, n);  // before the stop: see System_I2C.h
    requestSensorStop(I2C_DEVICE_ENCODER, n);
    logSystemEvent("SENSOR", "%s board auto-disabled after %u failed writes", name,
                   (unsigned)kEncoderFailuresBeforeStop);
    { char detail[24]; snprintf(detail, sizeof(detail), "%u failed writes", (unsigned)kEncoderFailuresBeforeStop);
      systemEventPost(SYSEVT_SENSOR_FAULT, name, detail); }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool encoderRunning(uint8_t n) {
  EncoderInstance* e = encoderAt(n);
  return e && e->running.load();
}

bool encoderAnyRunning() {
  for (uint8_t n = 1; n <= kEncoderMaxInstances; ++n) {
    if (encoderRunning(n)) return true;
  }
  return false;
}

bool encoderAutoStartOf(uint8_t n) {
  return n >= 1 && n <= kEncoderMaxInstances && saved(n).autoStart;
}

uint8_t encoderInstanceAddress(uint8_t n) {
  if (n < 1 || n > kEncoderMaxInstances) return 0;
  return encoderSavedAddress(saved(n).address, n);
}

uint8_t encoderInstanceBus(uint8_t n) {
  EncoderInstance* e = encoderAt(n);
  if (!e) return 0;
  // The seesaw object keeps the bus it was made for.
  if (e->pixels) return e->pixelsBus;
  return encoderSavedBus(saved(n).bus);
}

bool encoderStartInternal(uint8_t n) {
  EncoderInstance* found = encoderAt(n);
  if (!found) return false;
  EncoderInstance& e = *found;
  const char* name = displayName(n);
  if (e.running.load()) return true;
  if (!lightMutex(e)) {
    ERROR_I2CF("[%s] Could not create its state lock", name);
    return false;
  }
  const uint8_t bus = encoderInstanceBus(n);
  const uint8_t address = encoderInstanceAddress(n);
  if (address == 0) {
    ERROR_I2CF("[%s] No address set; not opening it", name);
    return false;
  }
  {
    char why[112];
    if (i2cAddressTakenBy(bus, address, I2C_DEVICE_ENCODER, n, why, sizeof(why))) {
      ERROR_I2CF("[%s] %s Not opening it there.", name, why);
      return false;
    }
  }
  TwoWire* wire = i2c() ? i2c()->getWire(bus) : nullptr;
  if (!wire) {
    ERROR_I2CF("[%s] I2C%u is not initialized", name, (unsigned)bus + 1);
    return false;
  }
  if (!e.pixels) {
    void* storage = ps_alloc(sizeof(EncoderPixels), AllocPref::PreferPsram, "encoder.obj");
    if (!storage) {
      ERROR_I2CF("[%s] Could not allocate the seesaw object", name);
      return false;
    }
    e.pixels = new (storage) EncoderPixels(wire);
    e.pixelsBus = bus;
  }

  uint16_t product = 0;
  bool answered = false;
  const EncoderBoard* board = nullptr;
  EncoderPixels* pixels = e.pixels;
  const bool ok = i2cDeviceTransaction(bus, address, kInitClockHz, kInitLockWaitMs, [&]() -> bool {
    // The product id first, with no reset: a board of another product (or a
    // device that is not a seesaw board) is left exactly as it was.
    if (!seesawReadProductIdOnHeldBus(wire, address, product)) return false;
    answered = true;
    board = encoderBoardForProduct(product);
    if (!board) return false;
    // Ours: the seesaw begin (reset, hardware id), then the pixel registers.
    return pixels->begin(address) && pixels->configure(*board);
  });
  if (!ok) {
    if (answered && !board) {
      ERROR_I2CF("[%s] The device at 0x%02X on I2C%u reports product %u, not an encoder board "
                 "(5752 or 4991); not claiming it", name, address, (unsigned)bus + 1, (unsigned)product);
    } else if (answered) {
      ERROR_I2CF("[%s] The %s at 0x%02X on I2C%u did not accept its setup", name, board->name, address,
                 (unsigned)bus + 1);
    } else {
      ERROR_I2CF("[%s] No encoder board answered at 0x%02X on I2C%u", name, address, (unsigned)bus + 1);
    }
    return false;
  }

  {
    SensorCacheGuard guard(lightMutex(e), pdMS_TO_TICKS(100), "encoder.start");
    if (!guard.held) return false;
    e.light.connected(board->pixelCount);  // the next tick writes the current colours
  }
  e.bus = bus;
  e.address = address;
  e.board = board;
  e.stopRequested = false;
  e.running = true;
  INFO_I2CF("[%s] Connected: %s (product %u, %u pixel%s) at 0x%02X on I2C%u", name, board->name,
            (unsigned)product, (unsigned)board->pixelCount, board->pixelCount == 1 ? "" : "s", address,
            (unsigned)bus + 1);
  sensorStatusBumpWith("encoder@started");
  return true;
}

void encoderStop(uint8_t n) {
  EncoderInstance* found = encoderAt(n);
  if (!found || !found->running.exchange(false)) return;
  EncoderInstance& e = *found;
  // Best effort: a closed board goes dark instead of keeping its last colour.
  // A board that stopped after failed writes does not answer anyway.
  if (e.pixels) {
    const uint8_t count = e.light.pixelCount;
    EncoderPixels* pixels = e.pixels;
    (void)i2cDeviceTransaction(e.bus, e.address, kFrameClockHz, kStopLockWaitMs,
                               [&]() -> bool { return pixels->writeFrame(EncoderFrame{}, count); });
  }
}

void encoderTick() {
  EncoderInstance* all = encoders();
  for (uint8_t n = 1; n <= kEncoderMaxInstances; ++n) tickInstance(all[n - 1], n);
}

// ---------------------------------------------------------------------------
// The LED commands' `encoder` target
// ---------------------------------------------------------------------------

const char* encoderLightSetColour(uint8_t pixel, RGB colour, const char* name, uint8_t n) {
  EncoderInstance* e = encoderAt(n);
  if (!e) return kReplyNoInstance;
  if (!e->running.load()) return notConnectedReply(*e, n);
  SensorCacheGuard guard(lightMutex(*e), pdMS_TO_TICKS(50), "encoder.colour");
  if (!guard.held) return kReplyStateBusy;
  if (!e->light.setColour(pixel, colour, name)) return pixelRangeReply(*e, pixel, n);
  return nullptr;
}

const char* encoderLightStartEffect(uint8_t pixel, int effect, RGB colour1, RGB colour2,
                                    uint32_t durationMs, const char* name, uint8_t n) {
  EncoderInstance* e = encoderAt(n);
  if (!e) return kReplyNoInstance;
  if (!e->running.load()) return notConnectedReply(*e, n);
  SensorCacheGuard guard(lightMutex(*e), pdMS_TO_TICKS(50), "encoder.effect");
  if (!guard.held) return kReplyStateBusy;
  if (!e->light.startEffect(pixel, effect, colour1, colour2, durationMs, name, millis())) {
    return pixelRangeReply(*e, pixel, n);
  }
  return nullptr;
}

void encoderLightSetBrightness(int percent, uint8_t n) {
  if (n < 1 || n > kEncoderMaxInstances) return;
  // The next frame reads the setting (encoderTick), so it applies at once.
  setSetting(saved(n).brightness, percent);
}

bool encoderLightReadStatus(EncoderLightStatus& out, uint8_t n) {
  out = EncoderLightStatus();
  EncoderInstance* found = encoderAt(n);
  if (!found) return false;
  EncoderInstance& e = *found;
  out.connected = e.running.load();
  const EncoderBoard* board = e.board;
  out.product = board ? board->product : 0;
  out.boardName = board ? board->name : nullptr;
  out.bus = out.connected ? e.bus : encoderInstanceBus(n);
  out.address = out.connected ? e.address : encoderInstanceAddress(n);
  out.brightness = brightnessSetting(n);
  SensorCacheGuard guard(lightMutex(e), pdMS_TO_TICKS(20), "encoder.status");
  if (!guard.held) return false;
  const uint32_t nowMs = millis();
  out.pixelCount = e.light.pixelCount;
  for (uint8_t i = 0; i < e.light.pixelCount && i < kEncoderMaxPixels; ++i) {
    out.pixels[i].rgb = e.light.pixels[i].steady;
    strlcpy(out.pixels[i].colour, e.light.pixels[i].colourName, sizeof(out.pixels[i].colour));
    strlcpy(out.pixels[i].effect, e.light.pixels[i].effectNow(nowMs), sizeof(out.pixels[i].effect));
  }
  return true;
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule encoderSettingsModule = {
  "encoder", "hardware.encoder", settings, kSettingsCount,
  isConnected, "Adafruit seesaw encoder boards (5752 or 4991): their RGB pixels"
};

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry encoderCommands[] = {
  { "openencoder", "Open an encoder board (its pixels light with 'ledcolor encoder'); n is instance 2..4.", true, cmd_openencoder, "Usage: openencoder [n]" },
  { "closeencoder", "Close an encoder board (its pixels go dark); n is instance 2..4.", true, cmd_closeencoder, "Usage: closeencoder [n]" },
  { "encoderstatus", "Show an encoder board: connection, product, pixels, address, brightness. (add 'json' for JSON output)", false, cmd_encoderstatus, "Usage: encoderstatus [n] [json]" },
  { "encoderautostart", "Open an encoder board after boot [n] [on|off].", true, cmd_encoderautostart, "Usage: encoderautostart [n] [on|off]" },
  { "encoderaddress", "Save an encoder board's I2C address (next open); 0 unsets instance 2..4.", true, cmd_encoderaddress, "Usage: encoderaddress [n] <0x49..0x50|0x36..0x3D>" },
  { "encoderbus", "Route an encoder board to bus <0|1> (reboot required).", true, cmd_encoderbus, "Usage: encoderbus [n] <0|1>" },
};
const size_t encoderCommandsCount = sizeof(encoderCommands) / sizeof(encoderCommands[0]);

#endif  // ENABLE_ENCODER
