#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <vector>

#define DEBUG_SYSTEM_H
#define SYSTEM_BUILDCONFIG_H
#define DEBUG_I2CF(...) ((void)0)
#define INFO_I2CF(...) ((void)0)
#define WARN_I2CF(...) ((void)0)
#define ERROR_I2CF(...) ((void)0)
#define private public
#include "System_I2C_Manager.h"
#undef private

using UBaseType_t = unsigned;
using TaskHandle_t = void*;
struct FakeSemaphore {
  bool held = false;
  bool refuseOnce = false;
  bool cache = false;
  unsigned takes = 0, gives = 0;
  std::function<void()> onTake;
};
TwoWire Wire, Wire1;
bool gI2CBusRunning = true;
static uint32_t nowUs = 1000000;
uint32_t micros() { nowUs += 10; return nowUs; }
uint32_t millis() { return nowUs / 1000; }
void delayMicroseconds(unsigned us) { nowUs += us; }
static void broadcastOutput(const char*) {}
static bool isHeldByCurrentTask(SemaphoreHandle_t mutex) { return mutex && mutex->held; }
static void assertBusesReleased();
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t) {
  assert(mutex && !mutex->held);
#if !TEST_BASELINE
  if (mutex->cache) assertBusesReleased();
#endif
  ++mutex->takes;
  if (mutex->onTake) {
    auto hook = std::move(mutex->onTake); mutex->onTake = {}; hook();
  }
  if (mutex->refuseOnce) { mutex->refuseOnce = false; return pdFALSE; }
  mutex->held = true;
  return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) {
  assert(mutex && mutex->held); mutex->held = false; ++mutex->gives; return pdTRUE;
}

I2CDeviceManager* I2CDeviceManager::instance = nullptr;
I2CDeviceManager* I2CDeviceManager::getInstance() { return instance; }
static unsigned recoveries = 0;
static void assertBusesReleased() {
  auto* manager = I2CDeviceManager::instance;
  for (auto* mutex : manager->busMutexes) assert(!mutex || !mutex->held);
}
void I2CDeviceManager::checkBusRecoveryNeeded() { assertBusesReleased(); ++recoveries; }
bool I2CDeviceManager::busRecoveryThrottled(uint8_t) const { assertBusesReleased(); return false; }
void I2CDeviceManager::performBusRecovery(uint8_t) { assertBusesReleased(); ++recoveries; }
static void logI2CRecovery(uint8_t, const char*, uint16_t) { assertBusesReleased(); }
static void logI2CError(uint8_t, const char*, uint8_t, uint16_t, bool) { assertBusesReleased(); }

#define INFO_TOF_LIFECYCLEF(...) ((void)0)
#define DEBUG_PERFORMANCEF(...) ((void)0)
#define DEBUG_MEMORY_HEAPF(...) ((void)0)
#define ERROR_TOFF(...) ((void)0)
static void diagnostic(const char*, ...) {}
#define DEBUG_TOF_POLLINGF(...) diagnostic(__VA_ARGS__)
#define DEBUG_TOF_VALUESF(...) diagnostic(__VA_ARGS__)
// INSERT_CONSTANTS
// INSERT_TYPES

static struct { int tofBus = 0, tofDevicePollMs = 100, i2cClockToFHz = 200000; } gSettings;
static bool gTofRunning = true, gTofConnected = true, gTofNear = false;
static TaskHandle_t gTofTaskHandle = reinterpret_cast<void*>(1);
static volatile UBaseType_t gTofWatermarkNow = 0, gTofWatermarkMin = ~0U;
static TofCache sTofCache;
static SemaphoreHandle_t sTofCacheMutex = nullptr;
static constexpr uint8_t I2C_ADDR_TOF = 0x29;
static constexpr unsigned TOF_STACK_WORDS = 3072;
static constexpr int DEBUG_PERFORMANCE = 1, SYSEVT_SENSOR_FAULT = 1, SYSEVT_TOF_OBJECT_DETECTED = 2;
static std::vector<std::string> events;
static bool isDebugFlagSet(int) { return false; }
static UBaseType_t uxTaskGetStackHighWaterMark(void*) { return TOF_STACK_WORDS; }
static bool checkTaskStackSafety(const char*, unsigned, bool*) { return false; }
static bool pollPaused(uint8_t) { return false; }
static void sensorStatusBumpWith(const char*) { events.push_back("status"); }
static void logSystemEvent(const char*, const char*) { events.push_back("log"); }
static void systemEventPost(int event, const char*, const char*) {
  assert(!sTofCacheMutex->held);
#if !TEST_BASELINE
  assertBusesReleased();
#endif
  events.push_back(event == SYSEVT_SENSOR_FAULT ? "fault" : "object");
}

using VL53L4CX_Error = int;
static constexpr int VL53L4CX_ERROR_NONE = 0, VL53L4CX_RANGESTATUS_SIGNAL_FAIL = 1,
  VL53L4CX_RANGESTATUS_SIGMA_FAIL = 2, VL53L4CX_RANGESTATUS_WRAP_TARGET_FAIL = 3,
  VL53L4CX_RANGESTATUS_XTALK_SIGNAL_FAIL = 4;
struct VL53L4CX_MultiRangingData_t {
  int NumberOfObjectsFound = 1;
  struct { int RangeMilliMeter = 250, RangeStatus = 0; uint32_t SignalRateRtnMegaCps = 65536; } RangeData[4];
};
static unsigned readyCalls, readCalls, rearmCalls, stopCalls, destructions, frees;
static int readyStatus, readStatus, rearmStatus;
static uint8_t readyValue = 1;
static std::vector<std::string> driverTrace;
static void hardwareCall(const char* label) {
  assert(I2CDeviceManager::instance->busMutexes[gSettings.tofBus]->held);
  assert(!sTofCacheMutex->held);
  driverTrace.push_back(label);
}
struct VL53L4CX {
  int VL53L4CX_GetMeasurementDataReady(uint8_t* ready) {
    hardwareCall("ready"); ++readyCalls; *ready = readyValue; return readyStatus;
  }
  int VL53L4CX_GetMultiRangingData(VL53L4CX_MultiRangingData_t* data) {
    hardwareCall("read"); ++readCalls; *data = {}; return readStatus;
  }
  int VL53L4CX_ClearInterruptAndStartMeasurement() {
    hardwareCall("rearm"); ++rearmCalls; return rearmStatus;
  }
  int VL53L4CX_StopMeasurement() { hardwareCall("stop"); ++stopCalls; return 0; }
  ~VL53L4CX() { assertBusesReleased(); assert(!sTofCacheMutex->held); ++destructions; }
};
static VL53L4CX* gVL53L4CX = nullptr;
static void ps_free(void* storage) { ++frees; std::free(storage); }
struct TaskExited {};
static void vTaskDelete(void* handle) {
  assert(!handle && !gTofTaskHandle && !gVL53L4CX && !gTofConnected);
  assert(!sTofCache.tofDataValid && sTofCache.tofTotalObjects == 0);
  throw TaskExited{};
}
static std::function<void()> afterPoll;
static void vTaskDelay(unsigned) {
  assertBusesReleased(); assert(!sTofCacheMutex->held);
  assert(afterPoll);
  auto hook = std::move(afterPoll); afterPoll = {}; hook();
  gTofRunning = false;
}

// INSERT_PRODUCTION

struct Fixture {
  I2CDeviceManager manager;
  FakeSemaphore mutexes[2], cacheMutex, registryMutex;
  I2CDevice::Health checkpoint{};
  TofCache published{};
  unsigned checkpointProbes = 0;
  std::vector<std::string> checkpointEvents;

  explicit Fixture(uint8_t bus) {
    assert(!gVL53L4CX);
    I2CDeviceManager::instance = &manager;
    Wire = {}; Wire1 = {};
    manager.managerMutex = &registryMutex;
    for (uint8_t i = 0; i < 2; ++i) {
      manager.busMutexes[i] = &mutexes[i]; manager.busInitialized[i] = true;
      manager.defaultClockHz[i] = 100000; manager.currentClockHz[i] = 100000;
      manager.wires[i]->clock = 100000; manager.wires[i]->lockHeld = &mutexes[i].held;
      manager.wires[i]->statuses.fill(0);
    }
    nowUs = 1000000; recoveries = 0;
    auto* device = manager.registerDevice(I2C_ADDR_TOF, "ToF", bus); assert(device);
    device->health.lastSuccessTime = 123; // Distinguish skipped work from fabricated success.
    gSettings.tofBus = bus; gTofRunning = gTofConnected = true; gTofNear = false;
    gTofTaskHandle = reinterpret_cast<void*>(1);
    sTofCacheMutex = &cacheMutex; sTofCache = {}; sTofCache.tofSeq = 7;
    cacheMutex.cache = true;
    sTofCache.tofDataValid = true; sTofCache.tofLastUpdate = 456;
    sTofCache.tofTotalObjects = 1; sTofCache.tofObjects[0].detected = true;
    sTofCache.tofObjects[0].valid = true; sTofCache.tofObjects[0].distance_mm = 1000;
    readyCalls = readCalls = rearmCalls = stopCalls = destructions = frees = 0;
    readyStatus = readStatus = rearmStatus = 0; readyValue = 1;
    events.clear(); driverTrace.clear();
    void* storage = std::malloc(sizeof(VL53L4CX)); assert(storage);
    gVL53L4CX = new (storage) VL53L4CX;
    afterPoll = [this, device, bus]() {
      checkpoint = device->health; published = sTofCache;
      checkpointProbes = manager.wires[bus]->endings; checkpointEvents = events;
    };
  }
  void run(unsigned expectedStopCalls = 1) {
    try { tofTask(nullptr); assert(false); } catch (const TaskExited&) {}
    assertBusesReleased(); assert(!cacheMutex.held);
    assert(!gTofRunning && !gTofConnected && !gTofTaskHandle && !gVL53L4CX);
    assert(destructions == 1 && frees == 1 && stopCalls == expectedStopCalls);
    for (const auto& object : sTofCache.tofObjects) assert(!object.detected && !object.valid);
    assert(manager.currentClockHz[gSettings.tofBus] == 100000);
    assert(manager.clockStackDepths[gSettings.tofBus] == 0);
  }
  ~Fixture() { assert(!gVL53L4CX); I2CDeviceManager::instance = nullptr; afterPoll = {}; }
};

int main() {
  for (uint8_t bus : {uint8_t(0), uint8_t(1)}) {
    {
      Fixture f(bus);
      f.mutexes[bus].onTake = []() { gTofRunning = false; };
      f.run();
#if TEST_BASELINE
      assert(!readyCalls && !readCalls && !rearmCalls);
      assert(f.checkpointProbes == 1 && f.checkpoint.totalErrors == 1 && f.checkpoint.nackCount == 1);
      assert(f.manager.devices[0].health.totalErrors == 1 && f.manager.devices[0].health.nackCount == 1);
      std::printf("REPRODUCED old stop-during-bus-wait: bus=%u, no sensor read, ACK probe, synthetic NACK=1\n", unsigned(bus));
#else
      assert(readyCalls == 1 && readCalls == 1 && rearmCalls == 1);
      assert(f.checkpointProbes == 0 && f.checkpoint.totalErrors == 0 && f.checkpoint.nackCount == 0);
      assert(f.published.tofSeq == 7 && f.published.tofLastUpdate == 456 && f.checkpointEvents.empty());
#endif
    }
#if !TEST_BASELINE
    {
      Fixture f(bus);
      gTofRunning = false;
      assert(!tofPoll());
      assert(!readyCalls && !f.mutexes[bus].takes && !f.manager.getMetrics(bus).totalTransactions);
      f.run();
    }
    {
      Fixture f(bus); f.run();
      assert(readyCalls == 1 && readCalls == 1 && rearmCalls == 1);
      assert(f.checkpointProbes == 0 && f.checkpoint.totalErrors == 0);
      assert(f.checkpoint.lastSuccessTime != 123);
      assert(f.published.tofSeq == 8 && f.published.tofObjects[0].distance_mm == 250);
      assert(f.checkpointEvents == std::vector<std::string>{"object"});
      assert(driverTrace == std::vector<std::string>({"ready", "read", "rearm", "stop"}));
    }
    for (bool stopInCache : {false, true}) {
      Fixture f(bus);
      if (stopInCache) f.cacheMutex.onTake = []() { gTofRunning = false; };
      else f.cacheMutex.refuseOnce = true;
      f.run();
      assert(readyCalls == 1 && readCalls == 1 && rearmCalls == 1);
      assert(f.checkpointProbes == 0 && !f.checkpoint.totalErrors);
      assert(f.published.tofSeq == 7 && f.published.tofLastUpdate == 456 && f.checkpointEvents.empty());
    }
    {
      Fixture f(bus); f.mutexes[bus].refuseOnce = true; f.run();
      assert(!readyCalls && !readCalls && !rearmCalls && !f.checkpointProbes);
      assert(f.manager.getMetrics(bus).mutexTimeouts == 1 && !f.checkpoint.totalErrors);
      assert(f.checkpoint.lastSuccessTime == 123 && f.published.tofSeq == 7);
    }
    {
      Fixture f(bus); readyValue = 0;
      for (unsigned poll = 1; poll <= 3; ++poll) {
        assert(tofPoll());
        assert(readyCalls == poll && !readCalls && !rearmCalls);
        assert(sTofCache.tofSeq == 7 && sTofCache.tofLastUpdate == 456);
        assertBusesReleased();
      }
      f.run();
      assert(readyCalls == 4 && !readCalls && !rearmCalls);
      assert(!f.checkpointProbes && !f.checkpoint.totalErrors);
      assert(f.published.tofSeq == 7 && f.published.tofLastUpdate == 456);
      assert(f.checkpointEvents.empty());
    }
    for (uint8_t wireStatus : {uint8_t(0), uint8_t(2), uint8_t(5), uint8_t(4)}) {
      Fixture f(bus); readyStatus = -1;
      f.manager.wires[bus]->statuses[I2C_ADDR_TOF] = wireStatus;
      f.run();
      assert(readyCalls == 1 && !readCalls && !rearmCalls && f.checkpointProbes == 1);
      assert(f.checkpoint.totalErrors == 1 && f.published.tofSeq == 7 && f.checkpointEvents.empty());
      assert(f.checkpoint.nackCount == (wireStatus == 0 || wireStatus == 2));
      assert(f.checkpoint.timeoutCount == (wireStatus == 5));
      assert(f.checkpoint.busErrorCount == (wireStatus == 4));
      assert(recoveries == (wireStatus == 4));
    }
    for (bool failRearm : {false, true}) {
      Fixture f(bus);
      if (failRearm) rearmStatus = -1; else readStatus = -1;
      // An actual device failure remains an error even if stop arrived while queued.
      f.mutexes[bus].onTake = []() { gTofRunning = false; };
      f.run();
      assert(readyCalls == 1 && readCalls == 1 && rearmCalls == unsigned(failRearm));
      assert(f.checkpointProbes == 1 && f.checkpoint.nackCount == 1 && f.checkpoint.totalErrors == 1);
      assert(f.published.tofSeq == 7 && f.checkpointEvents.empty());
    }
    for (bool stopDuringWait : {false, true}) {
      Fixture f(bus);
      auto* device = f.manager.getDevice(I2C_ADDR_TOF, bus);
      for (unsigned error = 0; error < 4; ++error) {
        device->recordError(I2CErrorType::NACK, 2);
      }
      assert(device->isDegraded() && device->health.consecutiveErrors == 4);
      // The real manager admits a retry after its 30-second recovery timeout.
      nowUs += 31000000;
      assert(!device->isDegraded());
      recoveries = 0;
      readStatus = -1;
      if (stopDuringWait) f.mutexes[bus].onTake = []() { gTofRunning = false; };
      // The fifth error degrades the device again, so the manager rejects the
      // best-effort hardware stop; object/cache cleanup must still finish.
      f.run(0);
      assert(readyCalls == 1 && readCalls == 1 && !rearmCalls && f.checkpointProbes == 1);
      assert(f.checkpoint.consecutiveErrors == 5 && f.checkpoint.totalErrors == 5);
      assert(f.checkpoint.nackCount == 5 && f.checkpoint.degraded && recoveries == 1);
      assert(f.published.tofSeq == 7 && f.published.tofLastUpdate == 456);
      if (stopDuringWait) assert(f.checkpointEvents.empty());
      else assert(f.checkpointEvents == std::vector<std::string>({"status", "log", "fault"}));
    }
#endif
  }
#if !TEST_BASELINE
  std::puts("PASS ToF production poll/task + manager health: stop interleavings, readiness, errors, publication and cleanup on both buses");
#endif
}
