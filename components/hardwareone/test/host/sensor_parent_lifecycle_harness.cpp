// Source-extracted production code replaces the markers. Only platform and
// driver boundaries are mocked; this template does not implement init loops.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "System_SeesawIdentity.h"  // the real product ids and the claim check
#include "i2csensor_ano_encoder_core.h"  // the ANO's pins and address rule

static unsigned assertions = 0;
static void check(bool condition, const char* message) {
  ++assertions;
  if (!condition) throw std::runtime_error(message);
}
enum class AllocPref { PreferPsram, PreferInternal };
struct Stats { unsigned allocations = 0, constructions = 0, destructions = 0, frees = 0, begins = 0; };
struct Allocation { std::string tag; bool alive = false; };
static std::map<std::string, Stats> stats;
static std::unordered_map<void*, Allocation> allocations;
struct Scenario {
  bool bus = true, transaction = true, ping = true, defaultBegin = true;
  unsigned failAllocationAt = 0, allocationAttempts = 0, beginIndex = 0, taskExits = 0;
  unsigned resets = 0, pinConfigs = 0, encoderZeroes = 0, backendStops = 0, rebinds = 0, flagReads = 0;
  bool addressTaken = false;     // another device of the build holds the ANO's address
  uint8_t resolvedBus = 255, transactionBus = 255;
  unsigned long now = 10000;
  std::vector<bool> beginResults;
  uint16_t productOverride = 0;  // a board of another product at the address (0 = the expected one)
  bool idAnswers = true;         // false: nothing answers the product-id read
  unsigned idReads = 0;
  uint8_t lastIdAddress = 0;     // the last product-id read: where, and what it reported
  uint16_t lastIdProduct = 0;
};
static Scenario scenario;
struct TwoWire {};
static TwoWire wire;
static struct { int imuBus = 1, apdsBus = 1, servoBus = 1, inputBus = 1; } gSettings;
static constexpr uint8_t I2C_ADDR_APDS = 0x39;
static constexpr uint8_t APDS9960_ADDRESS = 0x39, PCA9685_I2C_ADDRESS = 0x40;
static constexpr uint8_t I2C_ADDR_GAMEPAD = 0x50;
static constexpr int APDS9960_AGAIN_4X = 1, INPUT_PULLUP = 2;
static constexpr uint32_t GAMEPAD_BUTTON_MASK = 0x3f;
static constexpr int MAX_SERVO_CHANNELS = 16;
static unsigned long gLastGamepadInitMs = 0;
static constexpr unsigned long kGamepadInitMinIntervalMs = 2000;
static bool gImuConnected = false, gApdsConnected = false, gPwmDriverConnected = false;
static bool gInputConnected = false, gInputRunning = false, gAnoEncoderConnected = false;
static bool gAnoEncoderEnabled = false, gImuRunning = false;
static bool gApdsRunning = false, gApdsColorRunning = false, gApdsProximityRunning = false, gApdsGestureRunning = false;
static bool gImuInitRequested = true, gImuInitDone = true, gImuInitResult = true;
static struct { bool imuDataValid = true; unsigned imuSeq = 4; } sImuCache;
static struct {
  bool apdsDataValid = true;
  bool apdsColorValid = true, apdsProximityValid = true, apdsGestureValid = true;
  uint8_t apdsLastGesture = 3;
  unsigned long apdsLastGestureMs = 1234;
} sApdsCache;
static struct { bool dataValid = true; } gInputCache, sAnoEncoderCache;
// Synchronization is a boundary mock here; the snapshot suite executes the
// production guard and invalidation under admission failure separately.
static int sImuCacheMutex=1, sApdsCacheMutex=1, sAnoCacheMutex=1;
struct SensorCacheGuard { bool held=true; SensorCacheGuard(int,unsigned,const char*) {} };
using TickType_t = unsigned;
static unsigned pdMS_TO_TICKS(unsigned ms) { return ms; }
static void vTaskDelay(unsigned) { throw std::runtime_error("unexpected cache admission failure"); }
static bool inputCacheInvalidate() { gInputCache.dataValid=false; return true; }
struct ServoProfile {
  bool configured = true;
  int minPulse = 0, maxPulse = 0, centerPulse = 0;
  char name[16] = "previous";
};
static ServoProfile servoProfiles[MAX_SERVO_CHANNELS];

static void constructed(void* object, const char* tag = nullptr) {
  auto found = allocations.find(object);
  check(found != allocations.end() && !found->second.alive,
        "parent placement construction uses owned unconstructed allocation");
  if (tag) check(found->second.tag == tag, "constructor receives correct tagged allocation");
  found->second.alive = true;
  ++stats[found->second.tag].constructions;
}
static void destroyed(void* object) {
  auto found = allocations.find(object);
  check(found != allocations.end() && found->second.alive, "parent destructor runs once while storage is live");
  found->second.alive = false;
  ++stats[found->second.tag].destructions;
}
static bool beginResult(void* object) {
  auto found = allocations.find(object);
  check(found != allocations.end() && found->second.alive, "driver begin receives constructed parent");
  ++stats[found->second.tag].begins;
  if (scenario.beginIndex < scenario.beginResults.size())
    return scenario.beginResults[scenario.beginIndex++];
  ++scenario.beginIndex;
  return scenario.defaultBegin;
}
// The IMU's backend (HAL_MotionBackend.h): no object, only a release.
static void motionBackendStop() { ++scenario.backendStops; }
class Adafruit_APDS9960 {
 public:
  Adafruit_APDS9960() { constructed(this, "apds.obj"); }
  ~Adafruit_APDS9960() { destroyed(this); }
  bool begin(int integration, int gain, uint8_t address, TwoWire* bus) {
    check(integration == 10 && gain == APDS9960_AGAIN_4X && address == APDS9960_ADDRESS && bus == &wire,
          "APDS begin parameters and wire unchanged");
    return beginResult(this);
  }
};
class Adafruit_PWMServoDriver {
 public:
  Adafruit_PWMServoDriver(uint8_t address, TwoWire& bus) {
    constructed(this, "servo.obj");
    check(address == PCA9685_I2C_ADDRESS && &bus == &wire, "servo constructor address/wire unchanged");
  }
  ~Adafruit_PWMServoDriver() { destroyed(this); }
  bool begin() { return beginResult(this); }
  void setPWMFreq(int hz) { check(hz == 50, "servo PWM remains 50Hz"); }
};
class Adafruit_seesaw {
 public:
  explicit Adafruit_seesaw(TwoWire* bus) {
    constructed(this);
    check(bus == &wire, "Seesaw constructor retains configured wire");
  }
  ~Adafruit_seesaw() { destroyed(this); }
  // A board is begun (the library resets it) or reset only after the product
  // id read at its address reported this driver's own board.
  bool begin(uint8_t address) {
    check(address == I2C_ADDR_GAMEPAD || address == 0x49, "Seesaw begins with configured device address");
    checkClaimed(address);
    address_ = address;
    return beginResult(this);
  }
  void SWReset() { checkClaimed(address_); ++scenario.resets; }
  // The gamepad answers at 0x50 and the ANO at 0x49, each with its product id.
  uint32_t getVersion() {
    const uint16_t product = scenario.productOverride ? scenario.productOverride
        : (address_ == I2C_ADDR_GAMEPAD ? kSeesawProductGamepad : kSeesawProductAnoEncoder);
    return ((uint32_t)product << 16) | 0x1234u;
  }
  void pinModeBulk(uint32_t mask, int mode) {
    check((mask == GAMEPAD_BUTTON_MASK || mask == kAnoPinMask) && mode == INPUT_PULLUP,
          "Seesaw button configuration unchanged");
    ++scenario.pinConfigs;
  }
  void setGPIOInterrupts(uint32_t, int enabled) { check(enabled == 1, "Seesaw interrupts remain enabled"); }
  void setEncoderPosition(int position, int encoder) {
    check(position == 0 && encoder == 0, "ANO encoder initializes at zero"); ++scenario.encoderZeroes;
  }
  void enableEncoderInterrupt(int encoder) { check(encoder == 0, "ANO encoder interrupt unchanged"); }
  int analogRead(int) { return 512; }
 private:
  static void checkClaimed(uint8_t address) {
    const uint16_t own = address == I2C_ADDR_GAMEPAD ? kSeesawProductGamepad : kSeesawProductAnoEncoder;
    check(scenario.lastIdAddress == address && scenario.lastIdProduct == own,
          "seesaw begin/reset only after its product id read named the driver's own board");
  }
  uint8_t address_ = 0;
};
// The ANO driver's subclass: rebinding to another bus and the change-flag read.
class AnoSeesaw : public Adafruit_seesaw {
 public:
  explicit AnoSeesaw(TwoWire* bus) : Adafruit_seesaw(bus) {}
  void bindWire(TwoWire* bus) { check(bus == &wire, "ANO rebinds to the configured wire"); ++scenario.rebinds; }
  bool readIntFlags(uint32_t& flags) { flags = 0; ++scenario.flagReads; return true; }
};
// The product-id read the drivers make on the held bus, before any begin.
static bool seesawReadProductIdOnHeldBus(TwoWire* bus, uint8_t address, uint16_t& product) {
  check(bus == &wire, "product id read uses the configured wire");
  ++scenario.idReads;
  scenario.lastIdAddress = address;
  scenario.lastIdProduct = 0;
  if (!scenario.idAnswers) return false;
  product = scenario.productOverride ? scenario.productOverride
      : (address == I2C_ADDR_GAMEPAD ? kSeesawProductGamepad : kSeesawProductAnoEncoder);
  scenario.lastIdProduct = product;
  return true;
}
static Adafruit_APDS9960* gAPDS9960 = nullptr;
static Adafruit_PWMServoDriver* gPwmDriver = nullptr;
static Adafruit_seesaw* gGamepadSeesaw = nullptr;
static AnoSeesaw* gAnoSeesaw = nullptr;
static uint8_t sAnoSeesawBus = 0xFF, sAnoBus = 0, sAnoAddress = 0;
static void* ps_alloc(size_t size, AllocPref preference, const char* tag) {
  check(preference == AllocPref::PreferPsram, "parent allocation requests PSRAM preference");
  const std::map<std::string, size_t> expected = {
    {"apds.obj", sizeof(Adafruit_APDS9960)},
    {"servo.obj", sizeof(Adafruit_PWMServoDriver)}, {"input.gamepad.obj", sizeof(Adafruit_seesaw)},
    {"input.ano.obj", sizeof(AnoSeesaw)}};
  check(expected.count(tag) && expected.at(tag) == size, "parent allocation tag and sizeof match driver");
  ++stats[tag].allocations;
  if (++scenario.allocationAttempts == scenario.failAllocationAt) return nullptr;
  void* storage = std::malloc(size);
  check(storage != nullptr, "host fixture has allocation memory");
  allocations[storage] = {tag, false};
  return storage;
}
static void ps_free(void* object) {
  const auto found = allocations.find(object);
  check(found != allocations.end() && !found->second.alive,
        "parent free matches allocation and follows destructor");
  ++stats[found->second.tag].frees;
  allocations.erase(found); std::free(object);
}

// INSERT_PRODUCTION_PS_DELETE_HERE

struct I2cDevice { void resetGracePeriod() {} };
class I2cDeviceManager {
 public:
  static I2cDeviceManager* getInstance() { static I2cDeviceManager manager; return &manager; }
  TwoWire* getWire(uint8_t bus) { scenario.resolvedBus = bus; return scenario.bus ? &wire : nullptr; }
  I2cDevice* getDevice(uint8_t, uint8_t) { static I2cDevice device; return &device; }
};
static bool resolveBus(uint8_t* bus, TwoWire** output) {
  *bus = static_cast<uint8_t>(gSettings.inputBus); *output = scenario.bus ? &wire : nullptr;
  scenario.resolvedBus = *bus; return *output != nullptr;
}
static bool gamepadResolveBus(uint8_t* bus, TwoWire** output) { return resolveBus(bus, output); }
static bool anoResolveBus(uint8_t* bus, TwoWire** output) { return resolveBus(bus, output); }
static uint8_t anoEncoderAddress() { return 0x49; }
enum I2cDeviceType { I2C_DEVICE_INPUT = 3 };
static bool i2cAddressTakenBy(uint8_t, uint8_t address, I2cDeviceType type, uint8_t n, char* why, size_t len) {
  check(address == 0x49 && type == I2C_DEVICE_INPUT && n == 1 && why && len > 0,
        "ANO asks for its own address as input instance 1");
  if (scenario.addressTaken) std::snprintf(why, len, "Error: 0x49 on bus 1 is in use by Encoder (instance 1).");
  return scenario.addressTaken;
}
static bool i2cPingAddress(uint8_t, uint32_t, unsigned, uint8_t) { return scenario.ping; }
static bool i2cPing(uint8_t bus, uint8_t address) { return i2cPingAddress(address, 100000, 200, bus); }
template<class F>
static bool i2cDeviceTransaction(uint8_t bus, uint8_t, uint32_t, unsigned, F&& operation) {
  scenario.transactionBus = bus;
  return scenario.transaction ? operation() : false;
}
template<class F>
static void i2cDeviceTransactionVoid(uint8_t bus, uint8_t address, uint32_t hz, unsigned wait, F&& operation) {
  i2cDeviceTransaction(bus, address, hz, wait, [&]() { operation(); return true; });
}
static void delay(unsigned ms) { scenario.now += ms; }
static unsigned long millis() { return scenario.now; }
template<class... T> static void testLog(const char*, T&&...) {}
static void broadcastOutput(const char*) {}
#define INFO_IMU_LIFECYCLEF(...) testLog(__VA_ARGS__)
#define WARN_IMUF(...) testLog(__VA_ARGS__)
#define ERROR_IMUF(...) testLog(__VA_ARGS__)
#define DEBUG_IMU_LIFECYCLEF(...) testLog(__VA_ARGS__)
#define DEBUG_INPUT_LIFECYCLEF(...) testLog(__VA_ARGS__)
#define INFO_INPUT_LIFECYCLEF(...) testLog(__VA_ARGS__)
#define ERROR_INPUTF(...) testLog(__VA_ARGS__)
#define WARN_INPUTF(...) testLog(__VA_ARGS__)
#define ERROR_ANO_ENCODERF(...) testLog(__VA_ARGS__)
#define INFO_ANO_ENCODER_LIFECYCLEF(...) testLog(__VA_ARGS__)
#define SENSOR_TASK_EXIT(sensor, ...) do { ++scenario.taskExits; return; } while (0)

// INSERT_PRODUCTION_SENSOR_CODE_HERE

static void fixtureCleanup() {
  // Gamepad/ANO (and successful servo) are intentionally retained by firmware.
  // The host fixture releases them only between isolated test cases.
  ps_delete(gAPDS9960); gAPDS9960 = nullptr;
  ps_delete(gPwmDriver); gPwmDriver = nullptr;
  ps_delete(gGamepadSeesaw); gGamepadSeesaw = nullptr;
  ps_delete(gAnoSeesaw); gAnoSeesaw = nullptr;
  check(allocations.empty(), "fixture owns no forgotten parent allocation");
  for (const auto& entry : stats)
    check(entry.second.constructions == entry.second.destructions && entry.second.destructions == entry.second.frees,
          "every constructed fixture parent has exactly one destructor and free");
}
static void resetScenario() {
  fixtureCleanup(); stats.clear(); scenario = Scenario{};
  gImuConnected = false; gApdsConnected = false; gPwmDriverConnected = false;
  gInputConnected = false; gInputRunning = false; gAnoEncoderConnected = false;
  gImuRunning = false; gAnoEncoderEnabled = false;
  gLastGamepadInitMs = 0; sAnoSeesawBus = 0xFF; sAnoBus = 0; sAnoAddress = 0; gSettings.inputBus = 1;
  gImuInitRequested = true; gImuInitDone = true; gImuInitResult = true;
  sImuCache.imuDataValid = true; sImuCache.imuSeq = 4;
  sApdsCache.apdsDataValid = true; gInputCache.dataValid = true; sAnoEncoderCache.dataValid = true;
  sApdsCache.apdsColorValid = sApdsCache.apdsProximityValid = sApdsCache.apdsGestureValid = true;
}
static void testAllocationAndBusFailure() {
  const std::vector<bool(*)()> initializers = {apdsInit, servoInit, gamepadInit,
                                             gamepadInitConnection, anoEncoderInit};
  for (auto initialize : initializers) {
    resetScenario(); scenario.failAllocationAt = 1;
    check(!initialize(), "parent allocation failure is reported");
    check(allocations.empty(), "allocation failure does not construct invalid parent");
    check(!gImuConnected && !gApdsConnected && !gPwmDriverConnected && !gInputConnected && !gAnoEncoderConnected,
          "allocation failure does not mark any sensor connected");
    resetScenario(); scenario.bus = false;
    check(!initialize() && scenario.allocationAttempts == 0, "unavailable bus rejects init before allocation");
  }
}
static void testFreedFailureParents() {
  resetScenario(); gImuConnected = true;
  stopImuBranch();
  check(!gImuConnected && !sImuCache.imuDataValid && sImuCache.imuSeq == 0 && scenario.backendStops == 1 &&
        !gImuInitRequested && !gImuInitDone && !gImuInitResult && scenario.taskExits == 1,
        "actual IMU task shutdown releases the backend once and resets cache/init flags");

  resetScenario(); scenario.defaultBegin = false;
  check(!apdsInit() && !gAPDS9960 && stats["apds.obj"].frees == 1, "APDS begin failure frees and nulls parent");
  scenario.defaultBegin = true;
  check(apdsInit() && gApdsConnected, "APDS retries with a new parent");
  auto* apds = gAPDS9960;
  check(apdsInit() && gAPDS9960 == apds && stats["apds.obj"].allocations == 2,
        "already initialized APDS retains object");
  stopApdsBranch();
  check(!gAPDS9960 && !gApdsConnected && !sApdsCache.apdsDataValid,
        "actual APDS shutdown frees parent and invalidates cache");
  check(!sApdsCache.apdsColorValid && !sApdsCache.apdsProximityValid && !sApdsCache.apdsGestureValid,
        "APDS shutdown invalidates every sampled field");

  resetScenario(); scenario.defaultBegin = false;
  for (auto& profile : servoProfiles) profile = {};
  servoProfiles[0].minPulse = 700;
  servoProfiles[0].maxPulse = 2300;
  servoProfiles[0].centerPulse = 1500;
  for (int i = 1; i < MAX_SERVO_CHANNELS; ++i) servoProfiles[i].configured = false;
  check(!servoInit() && !gPwmDriver && stats["servo.obj"].frees == 1, "servo begin failure frees parent");
  scenario.defaultBegin = true;
  check(servoInit() && gPwmDriverConnected, "servo retries with new parent");
  check(servoProfiles[0].configured && servoProfiles[0].minPulse == 700 &&
        servoProfiles[0].maxPulse == 2300 && servoProfiles[0].centerPulse == 1500 &&
        std::strcmp(servoProfiles[0].name, "previous") == 0,
        "servo retry preserves a configured profile");
  for (int i = 1; i < MAX_SERVO_CHANNELS; ++i) {
    const auto& profile = servoProfiles[i];
    check(!profile.configured && profile.minPulse == 500 && profile.maxPulse == 2500 &&
          profile.centerPulse == 1500 && profile.name[0] == '\0', "unconfigured servo defaults unchanged");
  }
  auto* servo = gPwmDriver;
  check(servoInit() && gPwmDriver == servo && stats["servo.obj"].allocations == 2,
        "connected servo retains its parent");
  resetScenario(); scenario.transaction = false;
  check(!servoInit() && !gPwmDriver && stats["servo.obj"].frees == 1,
        "servo transaction admission failure frees preallocated parent");
}
static void testRetainedGamepad() {
  for (bool failAfterReset : {false, true}) {
    resetScenario(); scenario.beginResults = failAfterReset ? std::vector<bool>{true, false} : std::vector<bool>{false};
    check(!gamepadInit() && gGamepadSeesaw && !gInputConnected, "failed gamepad begin keeps parent for retry");
    auto* retained = gGamepadSeesaw;
    check(stats["input.gamepad.obj"].allocations == 1 && stats["input.gamepad.obj"].frees == 0,
          "failed gamepad attempt does not destroy retained parent");
    check(gamepadInit() && gGamepadSeesaw == retained && gInputConnected,
          "gamepad retry reuses same constructed parent");
    stopGamepadBranch();
    check(gGamepadSeesaw == retained && !gInputConnected && !gInputCache.dataValid,
          "actual gamepad stop keeps parent and invalidates cache");
    check(gamepadInitConnection() && gGamepadSeesaw == retained && !gInputRunning &&
          stats["input.gamepad.obj"].allocations == 1,
          "reconnect path reuses parent retained by ordinary init/stop");
  }
  resetScenario(); scenario.ping = false;
  check(!gamepadInitConnection() && gGamepadSeesaw, "no-ACK reconnect retains allocated parent");
  auto* retained = gGamepadSeesaw;
  check(!gamepadInitConnection() && stats["input.gamepad.obj"].allocations == 1,
        "gamepad backoff does not allocate another parent");
  scenario.now += 2001; scenario.ping = true;
  check(gamepadInitConnection() && gGamepadSeesaw == retained && stats["input.gamepad.obj"].allocations == 1,
        "gamepad reconnect success reuses no-ACK parent");
  resetScenario(); scenario.transaction = false;
  check(!gamepadInit() && gGamepadSeesaw && stats["input.gamepad.obj"].frees == 0,
        "gamepad transaction rejection keeps preallocated parent");
}
static void testRetainedAno() {
  resetScenario(); scenario.beginResults = std::vector<bool>{false};
  check(!anoEncoderInit() && gAnoSeesaw && !gAnoEncoderConnected,
        "failed ANO begin keeps parent for retry");
  auto* retained = gAnoSeesaw;
  check(anoEncoderInit() && gAnoSeesaw == retained && gAnoEncoderConnected && gInputConnected &&
        stats["input.ano.obj"].allocations == 1 && stats["input.ano.obj"].begins == 2 &&
        scenario.encoderZeroes == 1 && scenario.flagReads == 1 && sAnoBus == 1 && sAnoAddress == 0x49,
        "ANO retry reuses the failed-init parent, begins once, clears stale flags and latches bus/address");
  stopAnoBranch();
  check(gAnoSeesaw == retained && !gAnoEncoderConnected && !gInputConnected &&
        !sAnoEncoderCache.dataValid && !gInputCache.dataValid && stats["input.ano.obj"].frees == 0,
        "actual ANO stop retains parent and invalidates own/proxy cache");
  // Reopened on the other bus: the retained object moves to that bus's Wire.
  gSettings.inputBus = 0;
  check(anoEncoderInit() && gAnoSeesaw == retained && stats["input.ano.obj"].allocations == 1 &&
        scenario.rebinds == 1 && scenario.transactionBus == 0 && sAnoBus == 0,
        "ANO reopened on another bus rebinds the retained parent to that bus");
  stopAnoBranch();
  check(anoEncoderInit() && scenario.rebinds == 1, "ANO reopened on the same bus does not rebind");

  resetScenario(); scenario.transaction = false;
  check(!anoEncoderInit() && gAnoSeesaw && stats["input.ano.obj"].frees == 0,
        "ANO transaction rejection keeps preallocated parent");
  // An address another device of the build holds: refused before any I2C.
  resetScenario(); scenario.addressTaken = true;
  check(!anoEncoderInit() && !gAnoSeesaw && scenario.idReads == 0 && scenario.allocationAttempts == 0,
        "ANO refuses an address in use before allocating or touching the bus");
}
// A seesaw of another product at the address is not claimed, and is never
// begun or reset (the mock's begin/SWReset check that): 0x49 is the default of
// the ANO and the quad encoder board and the gamepad's alternate. The
// reconnect paths check too.
static void testOtherSeesawProductsRefused() {
  for (uint16_t other : {kSeesawProductQuadEncoder, kSeesawProductRotaryEncoder, kSeesawProductAnoEncoder}) {
    resetScenario(); scenario.productOverride = other;
    check(!gamepadInit() && !gInputConnected && scenario.pinConfigs == 0 && scenario.resets == 0 &&
          scenario.idReads == 1, "gamepad driver refuses a seesaw that reports another product id");
    scenario.now += 2001;
    check(!gamepadInitConnection() && !gInputConnected && scenario.resets == 0 && scenario.idReads == 2,
          "gamepad reconnect refuses another product once, without a reset or a retry");
  }
  for (uint16_t other : {kSeesawProductQuadEncoder, kSeesawProductRotaryEncoder, kSeesawProductGamepad}) {
    resetScenario(); scenario.productOverride = other;
    check(!anoEncoderInit() && !gAnoEncoderConnected && scenario.encoderZeroes == 0 && scenario.resets == 0,
          "ANO driver refuses a seesaw that reports another product id");
    check(!anoEncoderInit() && !gAnoEncoderConnected && scenario.resets == 0 && scenario.idReads == 2,
          "ANO retry refuses another product again, without a reset");
  }
  // Nothing (or a device that is not a seesaw board) answers the id read.
  resetScenario(); scenario.idAnswers = false;
  check(!gamepadInit() && !anoEncoderInit() && scenario.resets == 0 && stats["input.gamepad.obj"].begins == 0 &&
        stats["input.ano.obj"].begins == 0, "a failed product id read begins and resets nothing");
}
int main() {
  try {
    testAllocationAndBusFailure(); testFreedFailureParents(); testRetainedGamepad(); testRetainedAno();
    testOtherSeesawProductsRefused();
    fixtureCleanup();
    std::cout << "sensor parent extracted lifecycle tests passed (" << assertions
              << " assertions; mock driver/I2C/allocator/task boundaries)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "sensor parent lifecycle failure: " << error.what() << '\n';
    return 1;
  }
}
