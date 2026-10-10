#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

// Compile the actual manager header. Private access initializes the RTOS
// boundary and lets us observe registry/health state without a test registry.
#define DEBUG_SYSTEM_H
#define SYSTEM_BUILDCONFIG_H
#define DEBUG_I2CF(...) do { } while (0)
#define private public
#include "System_I2C_Manager.h"
#undef private

struct FakeSemaphore {
  bool available = true;
  bool held = false;
  TickType_t wait = 0;
  unsigned takes = 0;
  unsigned gives = 0;
};

TwoWire Wire;
TwoWire Wire1;
bool gI2CBusRunning = true;
static unsigned managerLookups = 0;

BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait) {
  assert(mutex && !mutex->held);
  mutex->wait = wait;
  ++mutex->takes;
  mutex->held = mutex->available;
  return mutex->held ? pdTRUE : pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) {
  assert(mutex && mutex->held);
  mutex->held = false;
  ++mutex->gives;
  return pdTRUE;
}
void delayMicroseconds(unsigned) {}
void broadcastOutput(const char*) {}
uint32_t millis() { return 123; }

I2CDeviceManager* I2CDeviceManager::instance = nullptr;
I2CDeviceManager* I2CDeviceManager::getInstance() {
  ++managerLookups;
  return instance;
}

// INSERT_PRODUCTION_DEFINITIONS

struct Fixture {
  I2CDeviceManager manager;
  FakeSemaphore mutexes[2];
  std::array<unsigned char, sizeof(manager.devices)> registry;
  std::array<unsigned char, sizeof(manager.busMetrics)> metrics;

  Fixture() {
    Wire = TwoWire{};
    Wire1 = TwoWire{};
    I2CDeviceManager::instance = &manager;
    gI2CBusRunning = true;
    managerLookups = 0;
    // The production constructor supplies bus 0 -> Wire1, bus 1 -> Wire.
    for (uint8_t bus = 0; bus < 2; ++bus) {
      manager.busMutexes[bus] = &mutexes[bus];
      manager.busInitialized[bus] = true;
      manager.defaultClockHz[bus] = bus == 0 ? 400000 : 250000;
      manager.currentClockHz[bus] = manager.defaultClockHz[bus];
      manager.wires[bus]->clock = manager.defaultClockHz[bus];
      manager.wires[bus]->lockHeld = &mutexes[bus].held;
      manager.wires[bus]->statuses.fill(2);  // Unattached addresses NACK.
      manager.busMetrics[bus].totalTransactions = 13 + bus;
      manager.busMetrics[bus].mutexTimeouts = 2 + bus;
    }
    // Probing a known address must neither erase its history nor treat a NACK
    // as a failed driver transaction. Unknown addresses must not register.
    manager.deviceCount = 1;
    manager.devices[0].init(0x50, "SeesawGamepad", 0);
    manager.devices[0].health.totalErrors = 9;
    manager.devices[0].health.degraded = true;
    std::memcpy(registry.data(), manager.devices, registry.size());
    std::memcpy(metrics.data(), manager.busMetrics, metrics.size());
  }

  ~Fixture() {
    assert(manager.deviceCount == 1);
    assert(std::memcmp(registry.data(), manager.devices, registry.size()) == 0);
    assert(std::memcmp(metrics.data(), manager.busMetrics, metrics.size()) == 0);
    assert(!mutexes[0].held && !mutexes[1].held);
    assert(Wire.reads == 0 && Wire.writes == 0);
    assert(Wire1.reads == 0 && Wire1.writes == 0);
    I2CDeviceManager::instance = nullptr;
  }

  void noTransport() const {
    assert(Wire.addresses.empty() && Wire1.addresses.empty());
    assert(Wire.clocks.empty() && Wire1.clocks.empty());
  }
};

int main() {
  {
    Fixture f;
    // Both an unprimed register device and a write-only device are discoverable
    // by their address ACK. The fake refuses every unsolicited data read.
    Wire1.statuses[0x50] = 0;
    Wire.statuses[0x3D] = 0;
    assert(i2cPingAddress(0x50, 100000, 37));  // Default bus is primary.
    assert(i2cPingAddress(0x3D, 100000, 81, 1));
    assert((Wire1.addresses == std::vector<uint8_t>{0x50, 0x50}));
    assert((Wire.addresses == std::vector<uint8_t>{0x3D, 0x3D}));
    assert((Wire1.clocks == std::vector<uint32_t>{100000, 400000}));
    assert((Wire.clocks == std::vector<uint32_t>{100000, 250000}));
    assert(f.mutexes[0].wait == pdMS_TO_TICKS(37));
    assert(f.mutexes[1].wait == pdMS_TO_TICKS(81));
    assert(Wire1.endings == 2 && Wire.endings == 2);
    assert(f.mutexes[0].takes == 1 && f.mutexes[0].gives == 1);
    assert(f.mutexes[1].takes == 1 && f.mutexes[1].gives == 1);
  }
  for (uint8_t status : {uint8_t(0), uint8_t(2), uint8_t(3), uint8_t(4), uint8_t(5)}) {
    Fixture f;
    Wire1.statuses[0x50] = status;
    assert(i2cProbeAddress(0x50, 0, 18, 0) == status);
    assert(i2cPingAddress(0x50, 100000, 18, 0) == (status == 0));
    // A zero requested clock uses the shared probe fallback. Every transport
    // result, including timeout/bus error, restores clock and releases lock.
    assert(Wire1.clocks.front() == 100000);
    assert(Wire1.clock == 400000 && f.manager.currentClockHz[0] == 400000);
    assert(f.manager.clockStackDepths[0] == 0);
    assert(f.mutexes[0].takes == 2 && f.mutexes[0].gives == 2);
    assert(f.mutexes[1].takes == 0);
    assert(Wire1.endings == (status == 0 ? 4U : 2U));
  }
  for (uint8_t secondStatus : {uint8_t(2), uint8_t(4), uint8_t(5)}) {
    Fixture f;
    // A transient address ACK must not become a discovered device. Confirm
    // using another address-only transaction, preserving its failure status.
    Wire.responses = {0, secondStatus};
    assert(i2cProbeAddress(0x0B, 100000, 17, 1) == secondStatus);
    assert((Wire.addresses == std::vector<uint8_t>{0x0B, 0x0B}));
    assert(Wire.endings == 2);
    assert(f.mutexes[1].takes == 1 && f.mutexes[1].gives == 1);
    assert((Wire.clocks == std::vector<uint32_t>{100000, 250000}));
    assert(Wire.clock == 250000 && f.manager.currentClockHz[1] == 250000);
    assert(f.manager.clockStackDepths[1] == 0);
  }
  {
    Fixture f;
    f.mutexes[1].available = false;
    assert(i2cProbeAddress(0x5A, 100000, 7, 1) == 4);
    assert(f.mutexes[1].takes == 1 && f.mutexes[1].gives == 0);
    assert(f.mutexes[1].wait == pdMS_TO_TICKS(7));
    f.noTransport();
  }
  for (unsigned unavailable = 0; unavailable < 4; ++unavailable) {
    Fixture f;
    uint8_t bus = 1;
    if (unavailable == 0) bus = 255;
    if (unavailable == 1) f.manager.busInitialized[bus] = false;
    if (unavailable == 2) f.manager.wires[bus] = nullptr;
    if (unavailable == 3) f.manager.busMutexes[bus] = nullptr;
    assert(i2cProbeAddress(0x50, 100000, 9, bus) == 4);
    assert(f.mutexes[0].takes == 0 && f.mutexes[1].takes == 0);
    f.noTransport();
  }
  {
    Fixture f;
    f.manager.clockStackDepths[0] = I2CDeviceManager::CLOCK_STACK_MAX;
    assert(i2cProbeAddress(0x50, 100000, 9) == 4);
    assert(f.mutexes[0].takes == 1 && f.mutexes[0].gives == 1);
    assert(f.manager.clockStackDepths[0] == I2CDeviceManager::CLOCK_STACK_MAX);
    f.noTransport();
  }
  {
    Fixture f;
    // Clock restoration follows the manager's existing stack rather than
    // forcing a scanner-owned speed onto a later transaction.
    f.manager.clockStacks[1][0] = 200000;
    f.manager.clockStackDepths[1] = 1;
    f.manager.currentClockHz[1] = Wire.clock = 200000;
    assert(i2cProbeAddress(0x50, 100000, 9, 1) == 2);
    assert(Wire.clock == 200000 && f.manager.currentClockHz[1] == 200000);
    assert(f.manager.clockStackDepths[1] == 1);
  }
  {
    Fixture f;
    gI2CBusRunning = false;
    assert(i2cProbeAddress(0x50, 100000, 9) == 4);
    assert(!i2cPingAddress(0x50, 100000, 9));
    assert(managerLookups == 0);
    assert(f.mutexes[0].takes == 0 && f.mutexes[1].takes == 0);
    f.noTransport();
    gI2CBusRunning = true;
    I2CDeviceManager::instance = nullptr;
    assert(i2cProbeAddress(0x50, 100000, 9) == 4);
    assert(!i2cPingAddress(0x50, 100000, 9));
    f.noTransport();
  }
  puts("I2C discovery probe boundary tests passed");
}
