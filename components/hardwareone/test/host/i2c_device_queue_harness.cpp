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
bool gI2cBusRunning = true;
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
  I2cDeviceManager manager;
  FakeSemaphore mutex;

  Fixture() {
    gI2cBusRunning = true;
    now = 100;
    manager.queueMutex = &mutex;
    assert(manager.getQueueDepth() == 0);
    for (int i = 0; i < I2C_DEVICE_COUNT; ++i) {
      assert(!manager.hasPendingDeviceStop(static_cast<I2cDeviceType>(i)));
    }
  }
  ~Fixture() { assert(!mutex.held); }

  I2cDeviceRequest take(I2cDeviceRequestAction action, I2cDeviceType device, uint8_t n = 1) {
    I2cDeviceRequest request{};
    assert(manager.dequeueDeviceRequest(&request));
    assert(request.action == action && request.device == device && request.n == n);
    return request;
  }

  void empty() {
    I2cDeviceRequest request{I2C_DEVICE_RTC, 1234, I2cDeviceRequestAction::Stop, 7};
    assert(!manager.dequeueDeviceRequest(&request));
    assert(request.device == I2C_DEVICE_RTC && request.queuedAt == 1234 && request.n == 7);
    assert(request.action == I2cDeviceRequestAction::Stop);
    assert(manager.getQueueDepth() == 0);
  }

  // Fill the rest of the ring by hand. Admission keeps one start per instance
  // slot and the ring is one longer than the slot count, so admission alone
  // can never fill it; a full ring is still refused.
  void fillByHand() {
    while ((manager.queueTail + 1) % I2cDeviceManager::DEVICE_QUEUE_SIZE != manager.queueHead) {
      manager.deviceQueue[manager.queueTail] = {I2C_DEVICE_COUNT, now, I2cDeviceRequestAction::Start, 0};
      manager.queueTail = (manager.queueTail + 1) % I2cDeviceManager::DEVICE_QUEUE_SIZE;
    }
  }
};

static void testAdmissionAndUnavailableQueue() {
  Fixture f;
  assert(f.manager.enqueueDeviceStart(I2C_DEVICE_GPS));
  now = 101;
  assert(f.manager.enqueueDeviceStart(I2C_DEVICE_GPS));
  assert(f.manager.getQueueDepth() == 1);
  assert(f.manager.getQueuePosition(I2C_DEVICE_GPS) == 1);
  assert(f.take(I2cDeviceRequestAction::Start, I2C_DEVICE_GPS).queuedAt == 100);
  f.empty();

  const unsigned before = f.mutex.takes;
  for (int invalid : {-1, int(I2C_DEVICE_COUNT), 999}) {
    const auto device = static_cast<I2cDeviceType>(invalid);
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
  assert(f.take(I2cDeviceRequestAction::Stop, I2C_DEVICE_GPS).queuedAt == 200);
  assert(!f.manager.hasPendingDeviceStop(I2C_DEVICE_GPS));
  f.take(I2cDeviceRequestAction::Start, I2C_DEVICE_TOF);
  f.take(I2cDeviceRequestAction::Start, I2C_DEVICE_IMU);
  f.empty();

  // A close arriving after dequeue remains visible to the worker before init.
  assert(f.manager.enqueueDeviceStart(I2C_DEVICE_PRESENCE));
  f.take(I2cDeviceRequestAction::Start, I2C_DEVICE_PRESENCE);
  assert(f.manager.enqueueDeviceStop(I2C_DEVICE_PRESENCE));
  assert(f.manager.hasPendingDeviceStop(I2C_DEVICE_PRESENCE));
  assert(!f.manager.enqueueDeviceStart(I2C_DEVICE_PRESENCE));
  f.take(I2cDeviceRequestAction::Stop, I2C_DEVICE_PRESENCE);
  assert(f.manager.enqueueDeviceStart(I2C_DEVICE_PRESENCE));
  f.take(I2cDeviceRequestAction::Start, I2C_DEVICE_PRESENCE);
  f.empty();
}

static void testFullRingAndDisabledBus() {
  Fixture f;
  const int capacity = I2cDeviceManager::DEVICE_QUEUE_SIZE - 1;
  // One start per instance slot always fits.
  static_assert(I2cDeviceManager::DEVICE_QUEUE_SIZE - 1 >= kI2cSlotCount, "ring holds every slot");
  std::vector<I2cDeviceType> admitted;  // every type but RTC and presence
  for (int i = 0; i < I2C_DEVICE_COUNT; ++i) {
    if (i == I2C_DEVICE_RTC || i == I2C_DEVICE_PRESENCE) continue;
    admitted.push_back(static_cast<I2cDeviceType>(i));
    assert(f.manager.enqueueDeviceStart(admitted.back()));
  }
  const int devices = int(admitted.size());
  f.fillByHand();
  assert(f.manager.getQueueDepth() == capacity);
  assert(!f.manager.enqueueDeviceStart(I2C_DEVICE_RTC));
  assert(f.manager.enqueueDeviceStart(I2C_DEVICE_THERMAL)); // duplicate in full ring
  assert(f.manager.getQueueDepth() == capacity);

  // Neither a full start ring nor a disabled bus can discard a stop.
  gI2cBusRunning = false;
  assert(!f.manager.enqueueDeviceStart(I2C_DEVICE_RTC));
  assert(f.manager.enqueueDeviceStop(I2C_DEVICE_RTC));
  assert(f.manager.enqueueDeviceStop(I2C_DEVICE_PRESENCE));
  assert(f.manager.enqueueDeviceStop(I2C_DEVICE_IMU));
  assert(f.manager.getQueueDepth() == capacity - 1);
  f.take(I2cDeviceRequestAction::Stop, I2C_DEVICE_IMU);
  f.take(I2cDeviceRequestAction::Stop, I2C_DEVICE_RTC);
  f.take(I2cDeviceRequestAction::Stop, I2C_DEVICE_PRESENCE);
  for (auto device : admitted) {
    if (device != I2C_DEVICE_IMU) f.take(I2cDeviceRequestAction::Start, device);
  }
  for (int i = devices - 1; i < capacity - 1; ++i) {
    f.take(I2cDeviceRequestAction::Start, I2C_DEVICE_COUNT, 0);  // the hand-filled entries
  }
  f.empty();
}

static void testWrappedCancellation() {
  Fixture f;
  // Move both cursors near the ring end using production enqueue/dequeue.
  for (int i = 0; i < I2cDeviceManager::DEVICE_QUEUE_SIZE - 2; ++i) {
    assert(f.manager.enqueueDeviceStart(I2C_DEVICE_THERMAL));
    f.take(I2cDeviceRequestAction::Start, I2C_DEVICE_THERMAL);
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
  f.take(I2cDeviceRequestAction::Stop, I2C_DEVICE_GPS);
  for (auto device : {I2C_DEVICE_TOF, I2C_DEVICE_IMU, I2C_DEVICE_INPUT}) {
    f.take(I2cDeviceRequestAction::Start, device);
  }
  f.empty();

  // Defend against duplicate requests admitted by older callers: cancellation
  // removes all occurrences without disturbing the other device's order.
  const std::array<I2cDeviceType, 5> devices = {
    I2C_DEVICE_GPS, I2C_DEVICE_TOF, I2C_DEVICE_GPS, I2C_DEVICE_IMU, I2C_DEVICE_GPS
  };
  const int head = I2cDeviceManager::DEVICE_QUEUE_SIZE - 2;  // wraps after two entries
  f.manager.queueHead = head;
  f.manager.queueTail = (head + int(devices.size())) % I2cDeviceManager::DEVICE_QUEUE_SIZE;
  for (size_t i = 0; i < devices.size(); ++i) {
    f.manager.deviceQueue[(head + i) % I2cDeviceManager::DEVICE_QUEUE_SIZE] =
      {devices[i], now, I2cDeviceRequestAction::Start, 1};
  }
  assert(f.manager.enqueueDeviceStop(I2C_DEVICE_GPS));
  assert(f.manager.getQueueDepth() == 2);
  f.take(I2cDeviceRequestAction::Stop, I2C_DEVICE_GPS);
  f.take(I2cDeviceRequestAction::Start, I2C_DEVICE_TOF);
  f.take(I2cDeviceRequestAction::Start, I2C_DEVICE_IMU);
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
  I2cDeviceRequest request{};
  assert(!f.manager.dequeueDeviceRequest(&request));
  assert(f.manager.getQueueDepth() == 0);
  assert(f.manager.queueHead == head && f.manager.queueTail == tail);
  assert(f.manager.deviceQueue[head].device == queued.device);
  assert(f.manager.pendingDeviceStops[I2C_DEVICE_RTC]);
  f.mutex.available = true;
  f.take(I2cDeviceRequestAction::Stop, I2C_DEVICE_RTC);
  f.take(I2cDeviceRequestAction::Start, I2C_DEVICE_GPS);
  f.empty();
}

#ifdef HW1_HOST_I2C_TEST_ENCODER_MAX
// Several instances of one type (System_I2CInstanceCore.h; built with the
// encoder at four): admission, cancellation and stops compare (device, n).
static void testInstancesQueueIndependently() {
  const auto enc = I2C_DEVICE_ENCODER;
  static_assert(I2cDeviceManager::DEVICE_QUEUE_SIZE == kI2cSlotCount + 1, "one start per slot");
  Fixture f;
  const unsigned before = f.mutex.takes;
  for (uint8_t bad : {0, 5, 255}) {
    assert(!f.manager.enqueueDeviceStart(enc, bad));
    assert(!f.manager.enqueueDeviceStop(enc, bad));
    assert(!f.manager.hasPendingDeviceStop(enc, bad));
    assert(!f.manager.isInQueue(enc, bad));
    assert(f.manager.getQueuePosition(enc, bad) == -1);
  }
  assert(!f.manager.enqueueDeviceStart(I2C_DEVICE_IMU, 2));  // a type with one instance
  assert(f.mutex.takes == before);

  // Two instances queue side by side; each is admitted once.
  assert(f.manager.enqueueDeviceStart(enc, 2));
  assert(f.manager.enqueueDeviceStart(enc));
  assert(f.manager.enqueueDeviceStart(enc, 2));
  assert(f.manager.getQueueDepth() == 2);
  assert(f.manager.getQueuePosition(enc, 2) == 1 && f.manager.getQueuePosition(enc, 1) == 2);
  assert(f.manager.isInQueue(enc, 1) && f.manager.isInQueue(enc, 2) && !f.manager.isInQueue(enc, 3));

  // A stop of instance 2 cancels only instance 2's start and refuses only its
  // next start; instance 1's start stays queued.
  now = 300;
  assert(f.manager.enqueueDeviceStop(enc, 2));
  assert(!f.manager.isInQueue(enc, 2) && f.manager.isInQueue(enc, 1));
  assert(f.manager.hasPendingDeviceStop(enc, 2) && !f.manager.hasPendingDeviceStop(enc, 1));
  assert(!f.manager.enqueueDeviceStart(enc, 2));
  assert(f.manager.enqueueDeviceStart(enc, 3));
  assert(f.take(I2cDeviceRequestAction::Stop, enc, 2).queuedAt == 300);
  f.take(I2cDeviceRequestAction::Start, enc, 1);
  f.take(I2cDeviceRequestAction::Start, enc, 3);
  f.empty();

  // Stops of every instance are kept; instance 1's (its type slot) go first.
  assert(f.manager.enqueueDeviceStop(enc, 4));
  assert(f.manager.enqueueDeviceStop(I2C_DEVICE_GPS));
  assert(f.manager.enqueueDeviceStop(enc));
  f.take(I2cDeviceRequestAction::Stop, I2C_DEVICE_GPS);
  f.take(I2cDeviceRequestAction::Stop, enc, 1);
  f.take(I2cDeviceRequestAction::Stop, enc, 4);
  f.empty();

  // Every slot fits at once, and the ring still has its empty entry.
  for (uint8_t slot = 0; slot < kI2cSlotCount; ++slot) {
    I2cDeviceType t;
    uint8_t n;
    assert(i2cSlotToRef(slot, t, n));
    assert(f.manager.enqueueDeviceStart(t, n));
  }
  assert(f.manager.getQueueDepth() == kI2cSlotCount);
  for (uint8_t slot = 0; slot < kI2cSlotCount; ++slot) {
    I2cDeviceType t;
    uint8_t n;
    assert(i2cSlotToRef(slot, t, n));
    f.take(I2cDeviceRequestAction::Start, t, n);
  }
  f.empty();
}
#endif

int main() {
  testAdmissionAndUnavailableQueue();
  testCancellationAndStopPriority();
  testFullRingAndDisabledBus();
  testWrappedCancellation();
  testMutexRefusalPreservesRequests();
#ifdef HW1_HOST_I2C_TEST_ENCODER_MAX
  testInstancesQueueIndependently();
  std::puts("I2C device queue tests passed (encoder with four instances: independent admission, stops and slots)");
#else
  std::puts("I2C device queue tests passed (production admission, cancellation, stop priority and wrap)");
#endif
}
