#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

// Compile the actual manager types. Private access supplies only the RTOS
// mutex and a legacy duplicate queue case; queue behavior comes from firmware.
#define DEBUG_SYSTEM_H
#define SYSTEM_BUILDCONFIG_H
#define DEBUG_I2CF(...) do { } while (0)
#define private public
#include "System_I2C_Manager.h"
#undef private

struct FakeSemaphore {
  bool available = true;
  bool held = false;
  unsigned takes = 0;
  unsigned gives = 0;
};

static constexpr TickType_t portMAX_DELAY = UINT32_MAX;
TwoWire Wire;
TwoWire Wire1;
bool gI2CBusRunning = true;
static uint32_t now = 100;

BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait) {
  assert(mutex && !mutex->held && wait == portMAX_DELAY);
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
uint32_t millis() { return now; }

// INSERT_PRODUCTION_DEFINITIONS

struct Fixture {
  I2CDeviceManager manager;
  FakeSemaphore mutex;

  Fixture() {
    gI2CBusRunning = true;
    now = 100;
    manager.queueMutex = &mutex;
    assert(manager.getQueueDepth() == 0);
    for (int i = 0; i < I2C_DEVICE_COUNT; ++i) {
      assert(!manager.hasPendingDeviceStop(static_cast<I2CDeviceType>(i)));
    }
  }
  ~Fixture() { assert(!mutex.held); }

  I2CDeviceRequest take(I2CDeviceRequestAction action, I2CDeviceType device) {
    I2CDeviceRequest request{};
    assert(manager.dequeueDeviceRequest(&request));
    assert(request.action == action && request.device == device);
    return request;
  }

  void empty() {
    I2CDeviceRequest request{I2C_DEVICE_RTC, 1234, I2CDeviceRequestAction::Stop};
    assert(!manager.dequeueDeviceRequest(&request));
    assert(request.device == I2C_DEVICE_RTC && request.queuedAt == 1234);
    assert(request.action == I2CDeviceRequestAction::Stop);
    assert(manager.getQueueDepth() == 0);
  }
};

static void testAdmissionAndUnavailableQueue() {
  Fixture f;
  assert(f.manager.enqueueDeviceStart(I2C_DEVICE_GPS));
  now = 101;
  assert(f.manager.enqueueDeviceStart(I2C_DEVICE_GPS));
  assert(f.manager.getQueueDepth() == 1);
  assert(f.manager.getQueuePosition(I2C_DEVICE_GPS) == 1);
  assert(f.take(I2CDeviceRequestAction::Start, I2C_DEVICE_GPS).queuedAt == 100);
  f.empty();

  const unsigned before = f.mutex.takes;
  for (int invalid : {-1, int(I2C_DEVICE_COUNT), 999}) {
    const auto device = static_cast<I2CDeviceType>(invalid);
    assert(!f.manager.enqueueDeviceStart(device));
    assert(!f.manager.enqueueDeviceStop(device));
    assert(!f.manager.hasPendingDeviceStop(device));
    assert(!f.manager.isInQueue(device));
    assert(f.manager.getQueuePosition(device) == -1);
  }
  assert(!f.manager.dequeueDeviceRequest(nullptr));
  assert(f.mutex.takes == before);

  f.manager.queueMutex = nullptr;
  assert(!f.manager.enqueueDeviceStart(I2C_DEVICE_GPS));
  assert(!f.manager.enqueueDeviceStop(I2C_DEVICE_GPS));
  assert(!f.manager.hasPendingDeviceStop(I2C_DEVICE_GPS));
  assert(!f.manager.isInQueue(I2C_DEVICE_GPS));
  assert(f.manager.getQueuePosition(I2C_DEVICE_GPS) == -1);
  f.empty();
}

static void testCancellationAndStopPriority() {
  Fixture f;
  assert(f.manager.enqueueDeviceStart(I2C_DEVICE_TOF));
  assert(f.manager.enqueueDeviceStart(I2C_DEVICE_GPS));
  assert(f.manager.enqueueDeviceStart(I2C_DEVICE_IMU));
  now = 200;
  assert(f.manager.enqueueDeviceStop(I2C_DEVICE_GPS));
  assert(!f.manager.isInQueue(I2C_DEVICE_GPS));
  assert(f.manager.getQueuePosition(I2C_DEVICE_GPS) == -1);
  assert(f.manager.getQueuePosition(I2C_DEVICE_IMU) == 2);
  assert(f.manager.getQueueDepth() == 2);
  assert(f.manager.hasPendingDeviceStop(I2C_DEVICE_GPS));
  assert(!f.manager.enqueueDeviceStart(I2C_DEVICE_GPS));
  now = 300;
  assert(f.manager.enqueueDeviceStop(I2C_DEVICE_GPS));
  assert(f.take(I2CDeviceRequestAction::Stop, I2C_DEVICE_GPS).queuedAt == 200);
  assert(!f.manager.hasPendingDeviceStop(I2C_DEVICE_GPS));
  f.take(I2CDeviceRequestAction::Start, I2C_DEVICE_TOF);
  f.take(I2CDeviceRequestAction::Start, I2C_DEVICE_IMU);
  f.empty();

  // A close arriving after dequeue remains visible to the worker before init.
  assert(f.manager.enqueueDeviceStart(I2C_DEVICE_PRESENCE));
  f.take(I2CDeviceRequestAction::Start, I2C_DEVICE_PRESENCE);
  assert(f.manager.enqueueDeviceStop(I2C_DEVICE_PRESENCE));
  assert(f.manager.hasPendingDeviceStop(I2C_DEVICE_PRESENCE));
  assert(!f.manager.enqueueDeviceStart(I2C_DEVICE_PRESENCE));
  f.take(I2CDeviceRequestAction::Stop, I2C_DEVICE_PRESENCE);
  assert(f.manager.enqueueDeviceStart(I2C_DEVICE_PRESENCE));
  f.take(I2CDeviceRequestAction::Start, I2C_DEVICE_PRESENCE);
  f.empty();
}

static void testFullRingAndDisabledBus() {
  Fixture f;
  const int capacity = I2CDeviceManager::DEVICE_QUEUE_SIZE - 1;
  for (int i = 0; i < capacity; ++i) {
    assert(f.manager.enqueueDeviceStart(static_cast<I2CDeviceType>(i)));
  }
  assert(f.manager.getQueueDepth() == capacity);
  assert(!f.manager.enqueueDeviceStart(I2C_DEVICE_RTC));
  assert(f.manager.enqueueDeviceStart(I2C_DEVICE_THERMAL)); // duplicate in full ring
  assert(f.manager.getQueueDepth() == capacity);

  // Neither a full start ring nor a disabled bus can discard a stop.
  gI2CBusRunning = false;
  assert(!f.manager.enqueueDeviceStart(I2C_DEVICE_RTC));
  assert(f.manager.enqueueDeviceStop(I2C_DEVICE_RTC));
  assert(f.manager.enqueueDeviceStop(I2C_DEVICE_PRESENCE));
  assert(f.manager.enqueueDeviceStop(I2C_DEVICE_IMU));
  assert(f.manager.getQueueDepth() == capacity - 1);
  f.take(I2CDeviceRequestAction::Stop, I2C_DEVICE_IMU);
  f.take(I2CDeviceRequestAction::Stop, I2C_DEVICE_RTC);
  f.take(I2CDeviceRequestAction::Stop, I2C_DEVICE_PRESENCE);
  for (int i = 0; i < capacity; ++i) {
    if (i != I2C_DEVICE_IMU) f.take(I2CDeviceRequestAction::Start, static_cast<I2CDeviceType>(i));
  }
  f.empty();
}

static void testWrappedCancellation() {
  Fixture f;
  // Move both cursors near the ring end using production enqueue/dequeue.
  for (int i = 0; i < I2CDeviceManager::DEVICE_QUEUE_SIZE - 2; ++i) {
    assert(f.manager.enqueueDeviceStart(I2C_DEVICE_THERMAL));
    f.take(I2CDeviceRequestAction::Start, I2C_DEVICE_THERMAL);
  }
  for (auto device : {I2C_DEVICE_TOF, I2C_DEVICE_GPS, I2C_DEVICE_IMU, I2C_DEVICE_INPUT}) {
    assert(f.manager.enqueueDeviceStart(device));
  }
  assert(f.manager.queueTail < f.manager.queueHead);
  assert(f.manager.enqueueDeviceStop(I2C_DEVICE_GPS));
  assert(f.manager.getQueueDepth() == 3);
  assert(f.manager.getQueuePosition(I2C_DEVICE_TOF) == 1);
  assert(f.manager.getQueuePosition(I2C_DEVICE_IMU) == 2);
  assert(f.manager.getQueuePosition(I2C_DEVICE_INPUT) == 3);
  f.take(I2CDeviceRequestAction::Stop, I2C_DEVICE_GPS);
  for (auto device : {I2C_DEVICE_TOF, I2C_DEVICE_IMU, I2C_DEVICE_INPUT}) {
    f.take(I2CDeviceRequestAction::Start, device);
  }
  f.empty();

  // Defend against duplicate requests admitted by older callers: cancellation
  // removes all occurrences without disturbing the other device's order.
  f.manager.queueHead = 6;
  f.manager.queueTail = 3;
  const std::array<I2CDeviceType, 5> devices = {
    I2C_DEVICE_GPS, I2C_DEVICE_TOF, I2C_DEVICE_GPS, I2C_DEVICE_IMU, I2C_DEVICE_GPS
  };
  for (size_t i = 0; i < devices.size(); ++i) {
    f.manager.deviceQueue[(6 + i) % I2CDeviceManager::DEVICE_QUEUE_SIZE] =
      {devices[i], now, I2CDeviceRequestAction::Start};
  }
  assert(f.manager.enqueueDeviceStop(I2C_DEVICE_GPS));
  assert(f.manager.getQueueDepth() == 2);
  f.take(I2CDeviceRequestAction::Stop, I2C_DEVICE_GPS);
  f.take(I2CDeviceRequestAction::Start, I2C_DEVICE_TOF);
  f.take(I2CDeviceRequestAction::Start, I2C_DEVICE_IMU);
  f.empty();
}

static void testMutexRefusalPreservesRequests() {
  Fixture f;
  assert(f.manager.enqueueDeviceStart(I2C_DEVICE_GPS));
  assert(f.manager.enqueueDeviceStop(I2C_DEVICE_RTC));
  const auto queued = f.manager.deviceQueue[f.manager.queueHead];
  const int head = f.manager.queueHead, tail = f.manager.queueTail;
  f.mutex.available = false;
  assert(!f.manager.enqueueDeviceStart(I2C_DEVICE_TOF));
  assert(!f.manager.enqueueDeviceStop(I2C_DEVICE_GPS));
  assert(!f.manager.hasPendingDeviceStop(I2C_DEVICE_RTC));
  assert(!f.manager.isInQueue(I2C_DEVICE_GPS));
  assert(f.manager.getQueuePosition(I2C_DEVICE_GPS) == -1);
  I2CDeviceRequest request{};
  assert(!f.manager.dequeueDeviceRequest(&request));
  assert(f.manager.getQueueDepth() == 0);
  assert(f.manager.queueHead == head && f.manager.queueTail == tail);
  assert(f.manager.deviceQueue[head].device == queued.device);
  assert(f.manager.pendingDeviceStops[I2C_DEVICE_RTC]);
  f.mutex.available = true;
  f.take(I2CDeviceRequestAction::Stop, I2C_DEVICE_RTC);
  f.take(I2CDeviceRequestAction::Start, I2C_DEVICE_GPS);
  f.empty();
}

int main() {
  testAdmissionAndUnavailableQueue();
  testCancellationAndStopPriority();
  testFullRingAndDisabledBus();
  testWrappedCancellation();
  testMutexRefusalPreservesRequests();
  std::puts("I2C device queue tests passed (production admission, cancellation, stop priority and wrap)");
}
