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

static unsigned assertions = 0;
static void check(bool condition, const char* message) {
  ++assertions;
  if (!condition) throw std::runtime_error(message);
}
enum class AllocPref { PreferPSRAM, PreferInternal };
struct Stats { unsigned allocations = 0, constructions = 0, destructions = 0, frees = 0, begins = 0; };
struct Allocation { std::string tag; bool alive = false; };
static std::map<std::string, Stats> stats;
static std::unordered_map<void*, Allocation> allocations;
struct Scenario {
  bool bus = true, transaction = true, ping = true, defaultBegin = true;
  unsigned failAllocationAt = 0, allocationAttempts = 0, beginIndex = 0, taskExits = 0;
  unsigned resets = 0, pinConfigs = 0, encoderZeroes = 0, crystalEnables = 0;
  uint8_t resolvedBus = 255, transactionBus = 255;
  unsigned long now = 10000;
  std::vector<bool> beginResults;
};
static Scenario scenario;
struct TwoWire {};
static TwoWire wire;
static struct { int imuBus = 1, apdsBus = 1, servoBus = 1, inputBus = 1; } gSettings;
static constexpr uint8_t BNO055_ADDRESS_A = 0x28, BNO055_ADDRESS_B = 0x29;
static constexpr uint8_t I2C_ADDR_IMU = 0x28, I2C_ADDR_APDS = 0x39;
static constexpr uint8_t APDS9960_ADDRESS = 0x39, PCA9685_I2C_ADDRESS = 0x40;
static constexpr uint8_t I2C_ADDR_GAMEPAD = 0x50;
static constexpr int APDS9960_AGAIN_4X = 1, INPUT_PULLUP = 2;
static constexpr uint32_t GAMEPAD_BUTTON_MASK = 0x3f, SS_BUTTON_MASK = 0x1f;
static constexpr int MAX_SERVO_CHANNELS = 16;
static unsigned long gLastGamepadInitMs = 0, gLastAnoInitMs = 0;
static constexpr unsigned long kGamepadInitMinIntervalMs = 2000, kAnoInitMinIntervalMs = 2000;
static bool gImuConnected = false, gApdsConnected = false, gPwmDriverConnected = false;
static bool gInputConnected = false, gInputRunning = false, gAnoEncoderConnected = false;
static bool gAnoEncoderEnabled = false, gImuRunning = false;
static bool gImuInitRequested = true, gImuInitDone = true, gImuInitResult = true;
static struct { bool imuDataValid = true; unsigned imuSeq = 4; } gImuCache;
static struct { bool apdsDataValid = true; } gApdsCache;
static struct { bool dataValid = true; } gInputCache, gAnoEncoderCache;
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
class Adafruit_BNO055 {
 public:
  Adafruit_BNO055(int id, uint8_t address, TwoWire* bus) {
    constructed(this, "imu.obj");
    check(id == 55 && (address == BNO055_ADDRESS_A || address == BNO055_ADDRESS_B) && bus == &wire,
          "BNO constructor identity/address/wire unchanged");
  }
  ~Adafruit_BNO055() { destroyed(this); }
  bool begin() { return beginResult(this); }
  void setExtCrystalUse(bool enabled) { check(enabled, "BNO crystal remains enabled"); ++scenario.crystalEnables; }
};
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
  bool begin(uint8_t address) {
    check(address == I2C_ADDR_GAMEPAD || address == 0x49, "Seesaw begins with configured device address");
    return beginResult(this);
  }
  void SWReset() { ++scenario.resets; }
  uint32_t getVersion() { return 5743UL << 16; }
  void pinModeBulk(uint32_t mask, int mode) {
    check((mask == GAMEPAD_BUTTON_MASK || mask == SS_BUTTON_MASK) && mode == INPUT_PULLUP,
          "Seesaw button configuration unchanged");
    ++scenario.pinConfigs;
  }
  void setGPIOInterrupts(uint32_t, int enabled) { check(enabled == 1, "Seesaw interrupts remain enabled"); }
  void setEncoderPosition(int position, int encoder) {
    check(position == 0 && encoder == 0, "ANO encoder initializes at zero"); ++scenario.encoderZeroes;
  }
  void enableEncoderInterrupt(int encoder) { check(encoder == 0, "ANO encoder interrupt unchanged"); }
  int analogRead(int) { return 512; }
};
static Adafruit_BNO055* gBNO055 = nullptr;
static Adafruit_APDS9960* gAPDS9960 = nullptr;
static Adafruit_PWMServoDriver* gPwmDriver = nullptr;
static Adafruit_seesaw* gGamepadSeesaw = nullptr;
static Adafruit_seesaw* gAnoSeesaw = nullptr;
static void* ps_alloc(size_t size, AllocPref preference, const char* tag) {
  check(preference == AllocPref::PreferPSRAM, "parent allocation requests PSRAM preference");
  const std::map<std::string, size_t> expected = {
    {"imu.obj", sizeof(Adafruit_BNO055)}, {"apds.obj", sizeof(Adafruit_APDS9960)},
    {"servo.obj", sizeof(Adafruit_PWMServoDriver)}, {"input.gamepad.obj", sizeof(Adafruit_seesaw)},
    {"input.ano.obj", sizeof(Adafruit_seesaw)}};
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

struct I2CDevice { void resetGracePeriod() {} };
class I2CDeviceManager {
 public:
  static I2CDeviceManager* getInstance() { static I2CDeviceManager manager; return &manager; }
  TwoWire* getWire(uint8_t bus) { scenario.resolvedBus = bus; return scenario.bus ? &wire : nullptr; }
  I2CDevice* getDevice(uint8_t, uint8_t) { static I2CDevice device; return &device; }
};
static bool resolveBus(uint8_t* bus, TwoWire** output) {
  *bus = static_cast<uint8_t>(gSettings.inputBus); *output = scenario.bus ? &wire : nullptr;
  scenario.resolvedBus = *bus; return *output != nullptr;
}
static bool gamepadResolveBus(uint8_t* bus, TwoWire** output) { return resolveBus(bus, output); }
static bool anoResolveBus(uint8_t* bus, TwoWire** output) { return resolveBus(bus, output); }
static uint8_t anoI2cAddr() { return 0x49; }
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
#define SENSOR_TASK_EXIT(sensor) do { ++scenario.taskExits; return; } while (0)

// INSERT_PRODUCTION_SENSOR_CODE_HERE

static void fixtureCleanup() {
  // Gamepad/ANO (and successful servo) are intentionally retained by firmware.
  // The host fixture releases them only between isolated test cases.
  ps_delete(gBNO055); gBNO055 = nullptr;
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
  gLastGamepadInitMs = 0; gLastAnoInitMs = 0;
  gImuInitRequested = true; gImuInitDone = true; gImuInitResult = true;
  gImuCache.imuDataValid = true; gImuCache.imuSeq = 4;
  gApdsCache.apdsDataValid = true; gInputCache.dataValid = true; gAnoEncoderCache.dataValid = true;
}
static void testAllocationAndBusFailure() {
  const std::vector<bool(*)()> initializers = {imuInit, apdsInit, servoInit, gamepadInit,
                                             gamepadInitConnection, anoEncoderInit, anoEncoderInitConnection};
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
  resetScenario(); scenario.defaultBegin = false;
  check(!imuInit(), "BNO reports exhaustion of existing retries");
  check(gBNO055 == nullptr && stats["imu.obj"].constructions == 10 && stats["imu.obj"].frees == 10,
        "BNO preserves five attempts/two address tries and frees each failed parent");
  scenario.defaultBegin = true;
  check(imuInit() && gImuConnected, "BNO retries successfully after failure");
  auto* imu = gBNO055; const auto imuAllocations = stats["imu.obj"].allocations;
  check(imuInit() && gBNO055 == imu && stats["imu.obj"].allocations == imuAllocations,
        "already initialized BNO does not allocate again");
  stopImuBranch();
  check(!gBNO055 && !gImuConnected && !gImuCache.imuDataValid && gImuCache.imuSeq == 0 &&
        !gImuInitRequested && !gImuInitDone && !gImuInitResult,
        "actual BNO task shutdown frees parent and resets cache/init flags");
  resetScenario(); scenario.beginResults = {false, false, true};
  check(imuInit() && stats["imu.obj"].constructions == 3 && stats["imu.obj"].frees == 2,
        "BNO retains only the successful retry parent");
  resetScenario(); scenario.defaultBegin = false; scenario.failAllocationAt = 2;
  check(!imuInit() && stats["imu.obj"].frees == 1 && allocations.empty(),
        "BNO later allocation failure preserves prior-attempt cleanup");

  resetScenario(); scenario.defaultBegin = false;
  check(!apdsInit() && !gAPDS9960 && stats["apds.obj"].frees == 1, "APDS begin failure frees and nulls parent");
  scenario.defaultBegin = true;
  check(apdsInit() && gApdsConnected, "APDS retries with a new parent");
  auto* apds = gAPDS9960;
  check(apdsInit() && gAPDS9960 == apds && stats["apds.obj"].allocations == 2,
        "already initialized APDS retains object");
  stopApdsBranch();
  check(!gAPDS9960 && !gApdsConnected && !gApdsCache.apdsDataValid,
        "actual APDS shutdown frees parent and invalidates cache");

  resetScenario(); scenario.defaultBegin = false;
  check(!servoInit() && !gPwmDriver && stats["servo.obj"].frees == 1, "servo begin failure frees parent");
  scenario.defaultBegin = true;
  check(servoInit() && gPwmDriverConnected, "servo retries with new parent");
  for (const auto& profile : servoProfiles)
    check(!profile.configured && profile.minPulse == 500 && profile.maxPulse == 2500 &&
          profile.centerPulse == 1500 && profile.name[0] == '\0', "servo profile initialization unchanged");
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
    check(gamepadInitConnection() && gGamepadSeesaw == retained && gInputRunning &&
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
  for (bool failAfterReset : {false, true}) {
    resetScenario(); scenario.beginResults = failAfterReset ? std::vector<bool>{true, false} : std::vector<bool>{false};
    check(!anoEncoderInit() && gAnoSeesaw && !gAnoEncoderConnected,
          "failed ANO begin keeps parent for retry");
    auto* retained = gAnoSeesaw;
    check(anoEncoderInitConnection() && gAnoSeesaw == retained && gAnoEncoderConnected && gInputConnected &&
          stats["input.ano.obj"].allocations == 1 && scenario.encoderZeroes == 1,
          "ANO reconnect reuses failed-init parent and initializes encoder/proxy");
    stopAnoBranch();
    check(gAnoSeesaw == retained && !gAnoEncoderConnected && !gInputConnected &&
          !gAnoEncoderCache.dataValid && !gInputCache.dataValid && stats["input.ano.obj"].frees == 0,
          "actual ANO stop retains parent and invalidates own/proxy cache");
    check(!anoEncoderInitConnection(), "ANO reconnect preserves backoff window");
    scenario.now += 2001;
    check(anoEncoderInitConnection() && gAnoSeesaw == retained && stats["input.ano.obj"].allocations == 1,
          "ANO restarts after backoff without allocating another parent");
  }
  resetScenario(); scenario.transaction = false;
  check(!anoEncoderInit() && gAnoSeesaw && stats["input.ano.obj"].frees == 0,
        "ANO transaction rejection keeps preallocated parent");
}
int main() {
  try {
    testAllocationAndBusFailure(); testFreedFailureParents(); testRetainedGamepad(); testRetainedAno();
    fixtureCleanup();
    std::cout << "sensor parent extracted lifecycle tests passed (" << assertions
              << " assertions; mock driver/I2C/allocator/task boundaries)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "sensor parent lifecycle failure: " << error.what() << '\n';
    return 1;
  }
}
