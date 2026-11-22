#include "i2csensor_encoder.h"

#if ENABLE_ENCODER
#include <Arduino.h>
#include <Adafruit_seesaw.h>
#include <Wire.h>
#include <atomic>
#include <new>

#include "i2csensor_encoder_light.h"
#include "System_Command.h"
#include "System_CommandRegistry.h"
#include "System_Debug.h"
#include "System_Events.h"        // systemEventPost: the fault event of the auto-disable site
#include "System_I2C.h"
#include "System_MemUtil.h"       // ps_alloc: the seesaw object lives in PSRAM
#include "System_Mutex.h"         // SensorCacheGuard
#include "System_SeesawIdentity.h"
#include "System_Settings.h"
#include "System_Utils.h"

namespace {

constexpr uint32_t kInitClockHz = 100000;
constexpr uint32_t kFrameClockHz = 400000;
constexpr uint32_t kInitLockWaitMs = 3000;   // queue worker: a start may wait for the bus
constexpr uint32_t kStopLockWaitMs = 50;
constexpr const char* kDisplayName = "Encoder";

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

// Retained across stops and restarts, bound to the bus of the first open
// (bus changes need a reboot). PSRAM-preferred placement new.
EncoderPixels* sPixels = nullptr;
uint8_t sPixelsBus = 0;

std::atomic<bool> sRunning{false};
std::atomic<bool> sStopRequested{false};  // the auto-disable site asked once
uint8_t sBus = 0;                          // latched by the running open
uint8_t sAddress = kEncoderDefaultAddress;
const EncoderBoard* sBoard = nullptr;      // the board last detected

// What the light shows. Commands change it, the main loop's tick reads it;
// the lock is held for the state only, never across a bus transfer.
EncoderLight sLight;

// Whether the board is meant to be open: opened by hand, closed by hand, or
// (until either) its start-at-boot setting. A command reconnects only a
// board that is meant to be open.
enum class HandState : uint8_t { FollowAutoStart, Opened, Closed };
std::atomic<HandState> sHandState{HandState::FollowAutoStart};
EncoderReconnect sReconnect;

// Created on first use by whichever task gets there first (a function-local
// static initializes once, thread-safely).
SemaphoreHandle_t lightMutex() {
  static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
  return mutex;
}

bool boardWanted() {
  switch (sHandState.load()) {
    case HandState::Opened: return true;
    case HandState::Closed: return false;
    case HandState::FollowAutoStart: break;
  }
  return gSettings.encoderAutoStart;
}

uint8_t brightnessSetting() {
  const int value = gSettings.encoderBrightness;
  return (uint8_t)(value < 0 ? 0 : (value > 100 ? 100 : value));
}

// A command that needs the board and finds it not running. A board meant to
// be open starts a reconnect through the sensor queue at most every two
// seconds.
const char* notConnectedReply() {
  bool started = false;
  if (boardWanted() && sReconnect.due(millis())) {
    SensorStartRefusal why = SensorStartRefusal::None;
    started = requestSensorStart(I2C_DEVICE_ENCODER, "encoder@reconnect", &why) ||
              why == SensorStartRefusal::AlreadyQueued;
  }
  return encoderNotConnectedReply(started);
}

// A one-pixel target past the board's count: the reply names the count.
const char* pixelRangeReply(uint8_t pixel) {
  if (!ensureDebugBuffer()) {
    return "Error: The encoder board has no such pixel. Run 'ledstatus' to see its pixels.";
  }
  const unsigned count = sLight.pixelCount;
  const char* name = sBoard ? sBoard->name : "encoder board";
  if (count <= 1) {
    snprintf(getDebugBuffer(), 1024,
             "Error: The %s has 1 pixel, so encoder:%u is not one. Use encoder:1 or encoder.",
             name, (unsigned)pixel);
  } else {
    snprintf(getDebugBuffer(), 1024,
             "Error: The %s has %u pixels, so encoder:%u is not one. Use encoder:1 to encoder:%u, "
             "or encoder for all.", name, count, (unsigned)pixel, count);
  }
  return getDebugBuffer();
}

constexpr const char* kReplyStateBusy = "Error: The encoder light state is busy. Retry.";

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
// The address options: the 5752's jumpers (0x49..0x50) and the 4991's
// (0x36..0x3D).
// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
const SettingEntry settings[] = {
  { "encoderAutoStart", SETTING_BOOL, &gSettings.encoderAutoStart, 0, 0, nullptr, 0, 1,
    "Open after boot", nullptr, false, nullptr, "encoderautostart" },
  { "encoderBrightness", SETTING_INT, &gSettings.encoderBrightness, 50, 0, nullptr, 0, 100,
    "Brightness (%)", nullptr, false, nullptr, "ledbrightness encoder" },
  { "encoderAddress", SETTING_INT, &gSettings.encoderAddress, kEncoderDefaultAddress, 0, nullptr,
    0x36, 0x50, "I2C address (next open)",
    "73|0x49 (5752),74|0x4A,75|0x4B,76|0x4C,77|0x4D,78|0x4E,79|0x4F,80|0x50,"
    "54|0x36 (4991),55|0x37,56|0x38,57|0x39,58|0x3A,59|0x3B,60|0x3C,61|0x3D",
    false, nullptr, "encoderaddress" },
  { "encoderBus", SETTING_INT, &gSettings.encoderBus, 0, 0, nullptr, 0, 1,
    "I2C bus (reboot required)", "0|I2C1,1|I2C2", false, nullptr, "encoderbus" },
};

bool isConnected() { return sRunning.load(); }

// The board's own fuel gauge answers at 0x36 on its bus (FeatherS3[D]): an
// encoder there would share its address, so the device never opens it.
bool clashesWithFuelGauge(uint8_t bus, uint8_t address) {
#if BATTERY_BACKEND_FUEL_GAUGE
  return address == I2C_ADDR_FUEL_GAUGE && bus == (uint8_t)gSettings.fuelGaugeBus;
#else
  (void)bus;
  (void)address;
  return false;
#endif
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

const char* cmd_openencoder(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  sHandState = HandState::Opened;
  return cmd_sensorstart_queued(I2C_DEVICE_ENCODER, kDisplayName, "openencoder@enqueue",
                                "the board opens in the background - light it with 'ledcolor encoder <colour>'");
}

const char* cmd_closeencoder(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  sHandState = HandState::Closed;
  return cmd_sensorstop_queued(I2C_DEVICE_ENCODER, kDisplayName);
}

const char* cmd_encoderautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    return gSettings.encoderAutoStart ? "[Encoder] Open after boot: on" : "[Encoder] Open after boot: off";
  }
  const int parsed = parseBoolArg(arg);
  if (parsed < 0) return kCommandReplyNotOnOff;
  setSetting(gSettings.encoderAutoStart, parsed == 1);
  return parsed == 1 ? "[Encoder] Open after boot: on" : "[Encoder] Open after boot: off";
}

const char* cmd_encoderstatus(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  const bool running = sRunning.load();
  const EncoderBoard* board = sBoard;
  const uint8_t address = running ? sAddress : encoderDeviceAddress();
  const uint8_t bus = running ? sBus : encoderDeviceBus();
  const unsigned brightness = brightnessSetting();
  const bool autoStart = gSettings.encoderAutoStart;
  if (argWantsJson(argsInput)) {
    snprintf(getDebugBuffer(), 1024,
             "{\"schema\":1,\"connected\":%s,\"product\":%u,\"board\":\"%s\",\"pixels\":%u,"
             "\"bus\":%u,\"address\":%u,\"brightness\":%u,\"autoStart\":%s}",
             running ? "true" : "false", (unsigned)(board ? board->product : 0),
             board ? board->name : "", (unsigned)(board ? board->pixelCount : 0), (unsigned)bus,
             (unsigned)address, brightness, autoStart ? "true" : "false");
    return getDebugBuffer();
  }
  if (running && board) {
    snprintf(getDebugBuffer(), 1024,
             "[Encoder] Connected: Adafruit %u %s, %u pixel%s, I2C%u address 0x%02X. Brightness %u%%. "
             "Open after boot: %s. Light it with 'ledcolor encoder <colour>'; 'ledstatus' shows its pixels.",
             (unsigned)board->product, board->name, (unsigned)board->pixelCount,
             board->pixelCount == 1 ? "" : "s", (unsigned)bus + 1, (unsigned)address, brightness,
             autoStart ? "on" : "off");
  } else {
    snprintf(getDebugBuffer(), 1024,
             "[Encoder] Not connected (I2C%u address 0x%02X). Brightness %u%%. Open after boot: %s. "
             "Start it with 'openencoder'.",
             (unsigned)bus + 1, (unsigned)address, brightness, autoStart ? "on" : "off");
  }
  return getDebugBuffer();
}

const char* cmd_encoderaddress(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String text = argsInput;
  text.trim();
  if (text.isEmpty()) return handleSettingCommand(&settings[2], text);
  int address = 0;
  if (!parseAddressArg(text, 0x36, 0x50, address) || !encoderAddressValid(address)) {
    return "Error: The encoder address must be 0x49..0x50 (5752) or 0x36..0x3D (4991). "
           "Read it with 'i2cscan'.";
  }
  if (clashesWithFuelGauge(encoderDeviceBus(), (uint8_t)address)) {
    return "Error: 0x36 is this board's fuel gauge on that bus. Put the encoder on the other bus "
           "('encoderbus') or close its A0 jumper and use 0x37.";
  }
  setSetting(gSettings.encoderAddress, address);
  return "[Encoder] Address saved; it applies at the next 'openencoder'";
}

const char* cmd_encoderbus(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return i2cSetDeviceBusAndReport(gSettings.encoderBus, argsInput, "encoderBus");
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool encoderRunning() { return sRunning.load(); }

uint8_t encoderDeviceAddress() {
  const int address = gSettings.encoderAddress;
  return encoderAddressValid(address) ? (uint8_t)address : kEncoderDefaultAddress;
}

uint8_t encoderDeviceBus() {
  // The seesaw object keeps the bus it was made for.
  if (sPixels) return sPixelsBus;
  return gSettings.encoderBus == 1 ? 1 : 0;
}

bool encoderStartInternal() {
  if (sRunning.load()) return true;
  if (!lightMutex()) {
    ERROR_I2CF("[Encoder] Could not create its state lock");
    return false;
  }
  const uint8_t bus = encoderDeviceBus();
  const uint8_t address = encoderDeviceAddress();
  if (clashesWithFuelGauge(bus, address)) {
    ERROR_I2CF("[Encoder] 0x%02X on I2C%u is the fuel gauge's address; not opening it there",
               address, (unsigned)bus + 1);
    return false;
  }
  TwoWire* wire = i2c() ? i2c()->getWire(bus) : nullptr;
  if (!wire) {
    ERROR_I2CF("[Encoder] I2C%u is not initialized", (unsigned)bus + 1);
    return false;
  }
  if (!sPixels) {
    void* storage = ps_alloc(sizeof(EncoderPixels), AllocPref::PreferPsram, "encoder.obj");
    if (!storage) {
      ERROR_I2CF("[Encoder] Could not allocate the seesaw object");
      return false;
    }
    sPixels = new (storage) EncoderPixels(wire);
    sPixelsBus = bus;
  }

  uint16_t product = 0;
  bool answered = false;
  const EncoderBoard* board = nullptr;
  const bool ok = i2cDeviceTransaction(bus, address, kInitClockHz, kInitLockWaitMs, [&]() -> bool {
    // The product id first, with no reset: a board of another product (or a
    // device that is not a seesaw board) is left exactly as it was.
    if (!seesawReadProductIdOnHeldBus(wire, address, product)) return false;
    answered = true;
    board = encoderBoardForProduct(product);
    if (!board) return false;
    // Ours: the seesaw begin (reset, hardware id), then the pixel registers.
    return sPixels->begin(address) && sPixels->configure(*board);
  });
  if (!ok) {
    if (answered && !board) {
      ERROR_I2CF("[Encoder] The device at 0x%02X on I2C%u reports product %u, not an encoder board "
                 "(5752 or 4991); not claiming it", address, (unsigned)bus + 1, (unsigned)product);
    } else if (answered) {
      ERROR_I2CF("[Encoder] The %s at 0x%02X on I2C%u did not accept its setup", board->name, address,
                 (unsigned)bus + 1);
    } else {
      ERROR_I2CF("[Encoder] No encoder board answered at 0x%02X on I2C%u", address, (unsigned)bus + 1);
    }
    return false;
  }

  {
    SensorCacheGuard guard(lightMutex(), pdMS_TO_TICKS(100), "encoder.start");
    if (!guard.held) return false;
    sLight.connected(board->pixelCount);  // the next tick writes the current colours
  }
  sBus = bus;
  sAddress = address;
  sBoard = board;
  sStopRequested = false;
  sRunning = true;
  INFO_I2CF("[Encoder] Connected: %s (product %u, %u pixel%s) at 0x%02X on I2C%u", board->name,
            (unsigned)product, (unsigned)board->pixelCount, board->pixelCount == 1 ? "" : "s", address,
            (unsigned)bus + 1);
  sensorStatusBumpWith("encoder@started");
  return true;
}

void encoderStop() {
  if (!sRunning.exchange(false)) return;
  // Best effort: a closed board goes dark instead of keeping its last colour.
  // A board that stopped after failed writes does not answer anyway.
  if (sPixels) {
    const uint8_t count = sLight.pixelCount;
    (void)i2cDeviceTransaction(sBus, sAddress, kFrameClockHz, kStopLockWaitMs,
                               [&]() -> bool { return sPixels->writeFrame(EncoderFrame{}, count); });
  }
}

void encoderTick() {
  if (!sRunning.load() || !sPixels || sStopRequested.load()) return;
  const uint32_t nowMs = millis();
  EncoderFrame frame;
  uint8_t count = 0;
  {
    SensorCacheGuard guard(lightMutex(), 0, "encoder.tick");
    if (!guard.held) return;
    // The saved brightness, however it was set (ledbrightness encoder, or a
    // settings editor writing the row): a change is a changed frame.
    sLight.brightnessPercent = brightnessSetting();
    if (!sLight.next(nowMs, frame)) return;
    count = sLight.pixelCount;
  }
  bool ran = false;
  const bool written = i2cDeviceTransaction(sBus, sAddress, kFrameClockHz, kEncoderLockWaitMs,
                                            [&]() -> bool {
    ran = true;
    // Stopped meanwhile: the stop owns the pixels now.
    if (!sRunning.load()) return true;
    return sPixels->writeFrame(frame, count);
  });
  // Not run: a busy bus, or a device the manager skips as degraded.
  bool degraded = false;
  if (!written && !ran) {
    I2cDeviceManager* mgr = i2c();
    I2cDevice* device = mgr ? mgr->getDevice(sAddress, sBus) : nullptr;
    degraded = device && device->isDegraded();
  }
  const EncoderFrameResult result = encoderFrameResult(written, ran, degraded);
  bool failedTooOften = false;
  {
    SensorCacheGuard guard(lightMutex(), pdMS_TO_TICKS(5), "encoder.settle");
    if (guard.held) failedTooOften = sLight.settle(result, frame, nowMs);
  }
  if (failedTooOften && !sStopRequested.exchange(true)) {
    ERROR_I2CF("[Encoder] %u writes failed in a row - closing the board",
               (unsigned)kEncoderFailuresBeforeStop);
    noteSensorAutoDisabled(I2C_DEVICE_ENCODER);  // before the stop: see System_I2C.h
    requestSensorStop(I2C_DEVICE_ENCODER);
    logSystemEvent("SENSOR", "Encoder board auto-disabled after %u failed writes",
                   (unsigned)kEncoderFailuresBeforeStop);
    { char detail[24]; snprintf(detail, sizeof(detail), "%u failed writes", (unsigned)kEncoderFailuresBeforeStop);
      systemEventPost(SYSEVT_SENSOR_FAULT, "Encoder", detail); }
  }
}

// ---------------------------------------------------------------------------
// The LED commands' `encoder` target
// ---------------------------------------------------------------------------

const char* encoderLightSetColour(uint8_t pixel, RGB colour, const char* name) {
  if (!sRunning.load()) return notConnectedReply();
  SensorCacheGuard guard(lightMutex(), pdMS_TO_TICKS(50), "encoder.colour");
  if (!guard.held) return kReplyStateBusy;
  if (!sLight.setColour(pixel, colour, name)) return pixelRangeReply(pixel);
  return nullptr;
}

const char* encoderLightStartEffect(uint8_t pixel, int effect, RGB colour1, RGB colour2,
                                    uint32_t durationMs, const char* name) {
  if (!sRunning.load()) return notConnectedReply();
  SensorCacheGuard guard(lightMutex(), pdMS_TO_TICKS(50), "encoder.effect");
  if (!guard.held) return kReplyStateBusy;
  if (!sLight.startEffect(pixel, effect, colour1, colour2, durationMs, name, millis())) {
    return pixelRangeReply(pixel);
  }
  return nullptr;
}

void encoderLightSetBrightness(int percent) {
  // The next frame reads the setting (encoderTick), so it applies at once.
  setSetting(gSettings.encoderBrightness, percent);
}

bool encoderLightReadStatus(EncoderLightStatus& out) {
  out = EncoderLightStatus();
  out.connected = sRunning.load();
  const EncoderBoard* board = sBoard;
  out.product = board ? board->product : 0;
  out.boardName = board ? board->name : nullptr;
  out.bus = out.connected ? sBus : encoderDeviceBus();
  out.address = out.connected ? sAddress : encoderDeviceAddress();
  out.brightness = brightnessSetting();
  SensorCacheGuard guard(lightMutex(), pdMS_TO_TICKS(20), "encoder.status");
  if (!guard.held) return false;
  const uint32_t nowMs = millis();
  out.pixelCount = sLight.pixelCount;
  for (uint8_t i = 0; i < sLight.pixelCount && i < kEncoderMaxPixels; ++i) {
    out.pixels[i].rgb = sLight.pixels[i].steady;
    strlcpy(out.pixels[i].colour, sLight.pixels[i].colourName, sizeof(out.pixels[i].colour));
    strlcpy(out.pixels[i].effect, sLight.pixels[i].effectNow(nowMs), sizeof(out.pixels[i].effect));
  }
  return true;
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule encoderSettingsModule = {
  "encoder", "hardware.encoder", settings, sizeof(settings) / sizeof(settings[0]),
  isConnected, "Adafruit seesaw encoder board (5752 or 4991): its RGB pixels"
};

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry encoderCommands[] = {
  { "openencoder", "Open the encoder board (its pixels light with 'ledcolor encoder').", true, cmd_openencoder },
  { "closeencoder", "Close the encoder board (its pixels go dark).", true, cmd_closeencoder },
  { "encoderstatus", "Show the encoder board: connection, product, pixels, address, brightness. (add 'json' for JSON output)", false, cmd_encoderstatus, "Usage: encoderstatus [json]" },
  { "encoderautostart", "Open the encoder board after boot [on|off].", true, cmd_encoderautostart, "Usage: encoderautostart [on|off]" },
  { "encoderaddress", "Save the encoder board's I2C address (next open).", true, cmd_encoderaddress, "Usage: encoderaddress <0x49..0x50|0x36..0x3D>" },
  { "encoderbus", "Route the encoder board to bus <0|1> (reboot required).", true, cmd_encoderbus, "Usage: encoderbus <0|1>" },
};
const size_t encoderCommandsCount = sizeof(encoderCommands) / sizeof(encoderCommands[0]);

#endif  // ENABLE_ENCODER
