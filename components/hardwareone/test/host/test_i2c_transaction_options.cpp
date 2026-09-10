#include <assert.h>
#include <stdio.h>

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>

// System_I2C_Manager.h's transaction template is production code. Only its
// hardware boundary is replaced here: a deterministic semaphore, clock, and
// time source. Exposing private state lets the test initialize that boundary
// without compiling the rest of the ESP-IDF firmware.
#define DEBUG_SYSTEM_H
#define SYSTEM_BUILDCONFIG_H
#define DEBUG_I2CF(...) do { } while (0)
#define private public
#include "../../System_I2C_Manager.h"
#undef private

struct FakeSemaphore {
  BaseType_t takeResult = pdTRUE;
  TickType_t lastWaitTicks = 0;
  unsigned takeCount = 0;
  unsigned giveCount = 0;
};

TwoWire Wire;
TwoWire Wire1;

namespace {
uint32_t fakeMicros;
uint32_t requestedClocks[8];
size_t requestedClockCount;
uint32_t metricsClockHz;
}

uint32_t micros() {
  fakeMicros += 10;
  return fakeMicros;
}

uint32_t millis() {
  return fakeMicros / 1000;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t waitTicks) {
  assert(semaphore != nullptr);
  semaphore->lastWaitTicks = waitTicks;
  semaphore->takeCount++;
  return semaphore->takeResult;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
  assert(semaphore != nullptr);
  semaphore->giveCount++;
  return pdTRUE;
}

I2CDeviceManager* I2CDeviceManager::instance = nullptr;

I2CDeviceManager::I2CDeviceManager()
    : deviceCount(0), managerMutex(nullptr), queueHead(0), queueTail(0),
      queueMutex(nullptr), pollingPaused(false) {
  for (uint8_t bus = 0; bus < NUM_BUSES; ++bus) {
    busMutexes[bus] = nullptr;
    wires[bus] = nullptr;
    currentClockHz[bus] = 0;
    defaultClockHz[bus] = 100000;
    busInitialized[bus] = false;
    std::memset(&busMetrics[bus], 0, sizeof(busMetrics[bus]));
    lastRecoveryMs[bus] = 0;
    std::memset(clockStacks[bus], 0, sizeof(clockStacks[bus]));
    clockStackDepths[bus] = 0;
  }
  std::memset(deviceQueue, 0, sizeof(deviceQueue));
}

I2CDeviceManager* I2CDeviceManager::getInstance() {
  return instance;
}

bool I2CDeviceManager::clockStackPush(uint8_t bus, uint32_t hz) {
  if (clockStackDepths[bus] >= CLOCK_STACK_MAX) return false;
  clockStacks[bus][clockStackDepths[bus]++] = hz;
  return true;
}

void I2CDeviceManager::clockStackPop(uint8_t bus) {
  assert(clockStackDepths[bus] > 0);
  --clockStackDepths[bus];
}

uint32_t I2CDeviceManager::clockStackTopOrDefault(uint8_t bus) {
  if (clockStackDepths[bus] == 0) return defaultClockHz[bus];
  return clockStacks[bus][clockStackDepths[bus] - 1];
}

void I2CDeviceManager::setBusClock(uint8_t bus, uint32_t hz) {
  assert(bus < NUM_BUSES);
  assert(requestedClockCount < (sizeof(requestedClocks) / sizeof(requestedClocks[0])));
  requestedClocks[requestedClockCount++] = hz;
  currentClockHz[bus] = hz;
}

void I2CDeviceManager::updateMetrics(uint8_t, uint32_t, uint32_t,
                                     uint32_t clockHz) {
  metricsClockHz = clockHz;
}

uint8_t I2CDeviceManager::probeDeviceStatus(uint8_t, uint8_t) {
  return 0;
}

I2CDevice::I2CDevice()
    : address(0), bus(0), name(nullptr), health{} {}

void I2CDevice::init(uint8_t addr, const char* deviceName, uint8_t busIdx) {
  address = addr;
  bus = busIdx;
  name = deviceName;
  health = {};
}

void I2CDevice::recordSuccess() {
  health.lastSuccessTime++;
}

void I2CDevice::recordError(I2CErrorType errorType, uint8_t) {
  health.lastErrorType = errorType;
  health.totalErrors++;
}

bool I2CDevice::isDegraded() const {
  return health.degraded;
}

static void resetObservations(FakeSemaphore& semaphore) {
  semaphore.lastWaitTicks = 0;
  semaphore.takeCount = 0;
  semaphore.giveCount = 0;
  requestedClockCount = 0;
  metricsClockHz = 0;
}

int main() {
  I2CDeviceManager manager;
  I2CDeviceManager::instance = &manager;

  FakeSemaphore busMutex;
  manager.busMutexes[0] = &busMutex;
  manager.wires[0] = &Wire1;
  manager.busInitialized[0] = true;
  manager.defaultClockHz[0] = 100000;

  // Model the real boot-time duplicate-registration case: this address is
  // already present before a helper executes its first transaction.
  I2CDevice& registered = manager.devices[0];
  manager.deviceCount = 1;
  registered.init(0x50, "SeesawGamepad", 0);

  bool operationRan = false;
  resetObservations(busMutex);
  int result = registered.transaction(
      [&operationRan]() {
        operationRan = true;
        return 37;
      },
      I2CTransactionOptions{100000, 80}, I2CDevice::Mode::STANDARD);

  assert(result == 37);
  assert(operationRan);
  assert(busMutex.takeCount == 1);
  assert(busMutex.lastWaitTicks == 80);
  assert(busMutex.giveCount == 1);
  assert(requestedClockCount == 2);
  assert(requestedClocks[0] == 100000);
  assert(requestedClocks[1] == 100000);
  assert(metricsClockHz == 100000);

  // A second call on the same registered object must use this call's values,
  // not the first call's values and not the registry's stale profile.
  resetObservations(busMutex);
  registered.transaction(
      []() {}, I2CTransactionOptions{400000, 15}, I2CDevice::Mode::STANDARD);

  assert(busMutex.takeCount == 1);
  assert(busMutex.lastWaitTicks == 15);
  assert(busMutex.giveCount == 1);
  assert(requestedClockCount == 2);
  assert(requestedClocks[0] == 400000);
  assert(requestedClocks[1] == 100000);
  assert(metricsClockHz == 400000);

  // Execution policy is per call; registry identity remains unchanged even
  // after calls with different policy.
  assert(registered.address == 0x50);
  assert(registered.bus == 0);
  assert(std::strcmp(registered.name, "SeesawGamepad") == 0);

  puts("I2C per-call transaction-option tests passed");
  return 0;
}
