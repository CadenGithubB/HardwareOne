// Template for test_tof_psram_lifecycle.py. Production definitions replace the
// markers; this file is not a standalone target or a copied driver algorithm.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

static unsigned assertions = 0;
static void check(bool condition, const char* description) {
  ++assertions;
  if (!condition) throw std::runtime_error(description);
}
enum class AllocPref { PreferPSRAM, PreferInternal };
using VL53L4CX_Error = int;
using UBaseType_t = unsigned;
static constexpr int VL53L4CX_ERROR_NONE = 0;
static constexpr uint8_t I2C_ADDR_TOF = 0x29;
static constexpr uint8_t VL53L4CX_DEFAULT_DEVICE_ADDRESS = 0x52;
static constexpr int VL53L4CX_DISTANCEMODE_LONG = 3;
static constexpr int DEBUG_PERFORMANCE = 1, TOF_STACK_WORDS = 3072;
static constexpr int SYSEVT_SENSOR_FAULT = 1;
struct Scenario {
  bool managerPresent = true, wirePresent = true, ping = true;
  bool transaction = true, allocation = true, inTransaction = false;
  bool driverAlive = false;
  int beginStatus = 0, initStatus = 0, startStatus = 0;
  unsigned constructions = 0, destructions = 0, allocations = 0, frees = 0;
  unsigned stops = 0, taskExits = 0, pinCalls = 0;
  uint8_t wireBus = 255, pingBus = 255, transactionBus = 255;
  uint32_t pingHz = 0, transactionHz = 0;
  unsigned pingWait = 0, transactionWait = 0;
  void* storage = nullptr;
  std::vector<std::string> events;
};
static Scenario scenario;
struct TwoWire {};
static TwoWire wire;
struct {
  int i2cClockToFHz = 200000, tofBus = 1, tofDevicePollMs = 100;
} gSettings;

class VL53L4CX {
 public:
  VL53L4CX() {
    check(this == scenario.storage, "driver is constructed in ps_alloc storage");
    check(!scenario.driverAlive, "previous driver destroyed before construction");
    check(representativeState_[0] == 0, "placement construction initializes driver state");
    scenario.driverAlive = true; ++scenario.constructions;
    scenario.events.push_back("construct");
  }
  ~VL53L4CX() {
    check(scenario.driverAlive && this == scenario.storage,
          "driver destructor runs once on owned storage");
    scenario.driverAlive = false; ++scenario.destructions;
    scenario.events.push_back("destroy");
  }
  void setI2cDevice(TwoWire* value) {
    check(value == &wire, "driver receives configured bus wire");
    scenario.events.push_back("wire");
  }
  void setXShutPin(int) { ++scenario.pinCalls; scenario.events.push_back("pin"); }
  VL53L4CX_Error begin() { scenario.events.push_back("begin"); return scenario.beginStatus; }
  void VL53L4CX_Off() { scenario.events.push_back("off"); }
  VL53L4CX_Error InitSensor(uint8_t address) {
    check(address == VL53L4CX_DEFAULT_DEVICE_ADDRESS, "sensor init address unchanged");
    scenario.events.push_back("init"); return scenario.initStatus;
  }
  VL53L4CX_Error VL53L4CX_SetDistanceMode(int mode) {
    check(mode == VL53L4CX_DISTANCEMODE_LONG, "distance mode unchanged");
    scenario.events.push_back("mode"); return 0;
  }
  VL53L4CX_Error VL53L4CX_SetMeasurementTimingBudgetMicroSeconds(unsigned budget) {
    check(budget == 200000, "measurement timing budget unchanged");
    scenario.events.push_back("budget"); return 0;
  }
  VL53L4CX_Error VL53L4CX_StartMeasurement() {
    scenario.events.push_back("start"); return scenario.startStatus;
  }
  VL53L4CX_Error VL53L4CX_StopMeasurement() {
    check(scenario.driverAlive, "measurement stops before driver destruction");
    ++scenario.stops; scenario.events.push_back("stop"); return 0;
  }
 private:
  // This mock checks lifetime, not the target ABI's real 9,496-byte size.
  unsigned char representativeState_[32] = {};
};
static VL53L4CX* gVL53L4CX = nullptr;
static bool gTofConnected = false, gTofRunning = false, gTofNear = false;
static volatile UBaseType_t gTofWatermarkNow = 0, gTofWatermarkMin = ~0U;
struct ObjectCache { bool detected = true, valid = true; };
static struct {
  bool tofDataValid = true;
  int tofTotalObjects = 4;
  ObjectCache tofObjects[4];
} gTofCache;

static void* ps_alloc(size_t size, AllocPref preference, const char* tag) {
  check(scenario.inTransaction, "object allocation remains inside I2C transaction");
  check(size == sizeof(VL53L4CX), "allocation uses sizeof driver, not stale constant");
  check(preference == AllocPref::PreferPSRAM && std::strcmp(tag, "tof.obj") == 0,
        "object allocation preserves PSRAM preference and tracking tag");
  check(scenario.storage == nullptr, "previous raw storage released before allocation");
  ++scenario.allocations; scenario.events.push_back("allocate");
  if (!scenario.allocation) return nullptr;
  scenario.storage = std::malloc(size);
  check(scenario.storage != nullptr, "host fixture allocation succeeds");
  return scenario.storage;
}
static void ps_free(void* ptr) {
  check(ptr != nullptr && ptr == scenario.storage, "only matching object storage is freed");
  check(!scenario.driverAlive, "destructor runs before releasing object storage");
  ++scenario.frees; scenario.events.push_back("free");
  std::free(ptr); scenario.storage = nullptr;
}

// INSERT_PRODUCTION_PS_DELETE_HERE

class I2CDeviceManager {
 public:
  static I2CDeviceManager* getInstance() {
    static I2CDeviceManager manager;
    return scenario.managerPresent ? &manager : nullptr;
  }
  TwoWire* getWire(uint8_t bus) {
    scenario.wireBus = bus;
    return scenario.wirePresent ? &wire : nullptr;
  }
};
static void delay(unsigned) {}
static bool i2cPingAddress(uint8_t address, uint32_t hz, unsigned wait, uint8_t bus) {
  check(address == I2C_ADDR_TOF, "probe targets ToF address");
  scenario.pingHz = hz; scenario.pingWait = wait; scenario.pingBus = bus;
  return scenario.ping;
}
template <class F>
static bool i2cDeviceTransaction(uint8_t bus, uint8_t address, uint32_t hz,
                                 unsigned wait, F&& operation) {
  check(address == I2C_ADDR_TOF, "transaction targets ToF address");
  scenario.transactionHz = hz; scenario.transactionWait = wait; scenario.transactionBus = bus;
  if (!scenario.transaction) return false;
  scenario.inTransaction = true;
  const bool result = operation();
  scenario.inTransaction = false;
  return result;
}
#define INFO_TOF_LIFECYCLEF(...) ((void)0)
#define DEBUG_PERFORMANCEF(...) ((void)0)
#define DEBUG_MEMORY_HEAPF(...) ((void)0)
#define ERROR_TOFF(...) ((void)0)
#define DEBUG_TOF_POLLINGF(...) ((void)0)
#define SENSOR_TASK_EXIT(sensor) do { ++scenario.taskExits; scenario.events.push_back("task-exit"); return; } while (0)
static bool isDebugFlagSet(int) { return false; }
static UBaseType_t uxTaskGetStackHighWaterMark(void*) { return 1024; }
static unsigned long millis() { return 100; }
static bool checkTaskStackSafety(const char*, unsigned, bool*) { return false; }
static bool pollPaused(uint8_t) { return false; }
static bool tofPoll() { return true; }
template <class F>
static bool i2cTaskWithTimeout(uint8_t, uint8_t, uint32_t, unsigned, F&& operation) { return operation(); }
static bool i2cShouldAutoDisable(uint8_t, uint8_t) { return false; }
static void sensorStatusBumpWith(const char*) {}
static void logSystemEvent(const char*, const char*) {}
static void systemEventPost(int, const char*, const char*) {}
static unsigned pdMS_TO_TICKS(unsigned value) { return value; }
static void vTaskDelay(unsigned) {
  throw std::runtime_error("shutdown test unexpectedly entered the running task loop");
}

// INSERT_PRODUCTION_TOF_FUNCTIONS_HERE

static void resetScenario() {
  check(gVL53L4CX == nullptr && scenario.storage == nullptr && !scenario.driverAlive,
        "previous fixture released driver ownership");
  scenario = Scenario{};
  gTofConnected = false; gTofRunning = false; gTofNear = false;
  gTofCache.tofDataValid = true; gTofCache.tofTotalObjects = 4;
  for (auto& object : gTofCache.tofObjects) { object.detected = true; object.valid = true; }
  gSettings.i2cClockToFHz = 200000; gSettings.tofBus = 1;
}
static void expectReleased() {
  check(gVL53L4CX == nullptr && scenario.storage == nullptr && !scenario.driverAlive,
        "driver pointer and owned storage cleared");
  check(scenario.constructions == scenario.destructions && scenario.destructions == scenario.frees,
        "every constructed driver has one destructor and one free");
}
static void stopTask() {
  gTofRunning = false;
  tofTask(nullptr);
  expectReleased();
  check(!gTofConnected && !gTofCache.tofDataValid && gTofCache.tofTotalObjects == 0,
        "task stop invalidates connection and cache");
  for (const auto& object : gTofCache.tofObjects)
    check(!object.detected && !object.valid, "task stop clears every cached detection");
}
static size_t eventAt(const char* event) {
  const auto found = std::find(scenario.events.begin(), scenario.events.end(), event);
  check(found != scenario.events.end(), "expected driver lifecycle event is present");
  return static_cast<size_t>(found - scenario.events.begin());
}
static void testSuccessAndTaskStop() {
  resetScenario();
  check(tofInit(), "ToF initializes successfully");
  check(gVL53L4CX != nullptr && gTofConnected && !gTofRunning,
        "init connects but does not change caller-owned running flag");
  check(scenario.constructions == 1 && scenario.destructions == 0,
        "successful init retains one live driver");
  check(scenario.wireBus == 1 && scenario.pingBus == 1 && scenario.transactionBus == 1,
        "configured bus reaches wire, probe, and transaction");
  check(scenario.pingWait == 200 && scenario.transactionWait == 3000,
        "existing probe and transaction waits unchanged");
  check(eventAt("allocate") < eventAt("construct") && eventAt("construct") < eventAt("wire") &&
        eventAt("wire") < eventAt("begin") && eventAt("begin") < eventAt("off") &&
        eventAt("off") < eventAt("init") && eventAt("init") < eventAt("mode") &&
        eventAt("mode") < eventAt("budget") && eventAt("budget") < eventAt("start"),
        "driver initialization ordering unchanged");
#ifdef A1
  check(scenario.pinCalls == 1, "A1 board config applies optional XSHUT pin");
#else
  check(scenario.pinCalls == 0, "board without A1 skips optional XSHUT pin");
#endif
  stopTask();
  check(scenario.stops == 1 && scenario.taskExits == 1, "task stop stops measurement and exits once");
  check(eventAt("stop") < eventAt("destroy") && eventAt("destroy") < eventAt("free") &&
        eventAt("free") < eventAt("task-exit"), "task teardown stops, destructs, frees, then exits");
  stopTask();
  check(scenario.stops == 1 && scenario.frees == 1 && scenario.taskExits == 2,
        "null-object shutdown does not double destroy or free");
  ps_delete<VL53L4CX>(nullptr);
  check(scenario.frees == 1, "production ps_delete null path is a no-op");
}
static void testInitFailures() {
  for (int failure = 0; failure < 4; ++failure) {
    resetScenario();
    if (failure == 0) scenario.allocation = false;
    if (failure == 1) scenario.beginStatus = -1;
    if (failure == 2) scenario.initStatus = -1;
    if (failure == 3) scenario.startStatus = -1;
    check(!tofInit(), "each allocation/driver failure rejects initialization");
    expectReleased();
    check(!gTofConnected && !gTofRunning, "failed fresh init does not connect or start");
    check(scenario.allocations == 1, "failed initialization makes exactly one allocation attempt");
    check(scenario.constructions == (failure == 0 ? 0U : 1U),
          "allocation failure does not construct or destruct uninitialized storage");
    if (failure == 1)
      check(std::find(scenario.events.begin(), scenario.events.end(), "init") == scenario.events.end(),
            "begin failure prevents later sensor init");
    if (failure == 2)
      check(std::find(scenario.events.begin(), scenario.events.end(), "start") == scenario.events.end(),
            "sensor init failure prevents measurement start");
    scenario.allocation = true; scenario.beginStatus = 0; scenario.initStatus = 0; scenario.startStatus = 0;
    check(tofInit(), "retry succeeds after every failed init stage");
    stopTask();
  }
}
static void testReinitialization() {
  resetScenario();
  check(tofInit(), "reinit fixture starts with live object");
  scenario.events.clear(); gTofRunning = true;
  check(tofInit(), "reinit replaces existing object");
  check(gTofRunning && gTofConnected && scenario.constructions == 2 && scenario.destructions == 1,
        "reinit retains only the new object without changing running state");
  check(eventAt("stop") < eventAt("destroy") && eventAt("destroy") < eventAt("free") &&
        eventAt("free") < eventAt("allocate") && eventAt("allocate") < eventAt("construct"),
        "reinit releases previous object before allocating replacement");
  scenario.allocation = false;
  check(!tofInit(), "replacement allocation failure is reported");
  expectReleased();
  check(!gTofConnected && gTofRunning, "failed replacement clears connection but preserves caller running flag");
  stopTask();
}
static void testPreallocationGatesAndClock() {
  for (int gate = 0; gate < 4; ++gate) {
    resetScenario();
    if (gate == 0) scenario.managerPresent = false;
    if (gate == 1) scenario.wirePresent = false;
    if (gate == 2) scenario.ping = false;
    if (gate == 3) scenario.transaction = false;
    check(!tofInit(), "missing manager/wire/probe/transaction prevents init");
    check(scenario.allocations == 0 && scenario.constructions == 0,
          "unavailable I2C path never allocates driver");
    expectReleased();
  }
  for (const auto& clock : std::vector<std::pair<int, uint32_t>>{
           {-1, 50000}, {0, 50000}, {1, 50000}, {100000, 100000}, {800000, 400000}}) {
    resetScenario(); gSettings.i2cClockToFHz = clock.first;
    check(tofInit(), "clock-boundary fixture initializes");
    check(scenario.pingHz == clock.second && scenario.transactionHz == clock.second,
          "existing init clock default/clamping remains unchanged");
    stopTask();
  }
}
int main() {
  try {
    testSuccessAndTaskStop(); testInitFailures(); testReinitialization();
    testPreallocationGatesAndClock();
    expectReleased();
    std::cout << "ToF extracted lifecycle tests passed (" << assertions << " assertions; A1="
#ifdef A1
              << "defined"
#else
              << "absent"
#endif
              << "; mocked driver/I2C/allocator/task boundaries)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "ToF lifecycle test failure: " << error.what() << '\n';
    return 1;
  }
}
