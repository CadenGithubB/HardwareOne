#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

using SemaphoreHandle_t = void*;
static constexpr int pdTRUE = 1;
#define pdMS_TO_TICKS(ms) (ms)
static bool lockAvailable = true;
static int takes = 0, gives = 0;
static int xSemaphoreTake(SemaphoreHandle_t, unsigned) { ++takes; return lockAvailable ? pdTRUE : 0; }
static void xSemaphoreGive(SemaphoreHandle_t) { ++gives; }
static bool gInputRunning = true;
using StaticSemaphore_t = int;
static SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* storage) { return storage; }
#define ENABLE_OLED_INPUT 1
class SensorCacheGuard {
 public:
  bool held;
  SemaphoreHandle_t mutex;
  SensorCacheGuard(SemaphoreHandle_t value, unsigned wait, const char*)
      : held(value && xSemaphoreTake(value, wait) == pdTRUE), mutex(value) {}
  ~SensorCacheGuard() { if (held) xSemaphoreGive(mutex); }
};

// This header is extracted from the actual production HAL_Input.cpp function
// by the test runner; the test never reimplements the snapshot operation.
#include "input_snapshot_under_test.h"

int main() {
  sInputCache.buttons = UINT32_MAX;
  sInputCache.dataValid = true;
  uint32_t previous = UINT32_MAX;
  bool initialized = false;
  // A raw switch held when a wizard opens establishes a baseline, not a click.
  sInputCache.buttons = ~1u;
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  assert(initialized && previous == ~1u);
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  sInputCache.buttons = UINT32_MAX;
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  sInputCache.buttons = ~1u;
  sInputCache.buttonPressedAccum = 1u;
  assert(inputConsumeButtonPresses(previous, initialized) == 1u);
  assert(sInputCache.buttonPressedAccum == 0);
  assert(inputConsumeButtonPresses(previous, initialized) == 0);

  // A complete press/release shorter than a display frame must survive, then
  // be consumed exactly once even though the current raw state is released.
  sInputCache.buttons = UINT32_MAX;
  sInputCache.buttonPressedAccum = 2u | 32u;
  assert(inputConsumeButtonPresses(previous, initialized) == (2u | 32u));
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  initialized = false;
  sInputCache.buttonPressedAccum = 1u;
  assert(inputConsumeButtonPresses(previous, initialized) == 1u);
  assert(inputConsumeButtonPresses(previous, initialized) == 0);

  // The three auxiliary keys share this same latch. Simultaneously held
  // X/Y/START keys each emit one press, then release independently; a new
  // X press must remain visible while Y and START stay held.
  constexpr uint32_t auxiliaryMask = 4u | 8u | 16u;
  sInputCache.buttons = ~auxiliaryMask;
  sInputCache.buttonPressedAccum = auxiliaryMask;
  assert(inputConsumeButtonPresses(previous, initialized) == auxiliaryMask);
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  sInputCache.buttons = ~(8u | 16u);
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  sInputCache.buttons = ~auxiliaryMask;
  sInputCache.buttonPressedAccum = 4u;
  assert(inputConsumeButtonPresses(previous, initialized) == 4u);
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  sInputCache.buttons = UINT32_MAX;
  assert(inputConsumeButtonPresses(previous, initialized) == 0);

  // Contention and invalid/stopped caches must not consume pending edges or
  // corrupt the reader's prior state. All successful takes release the lock.
  sInputCache.buttonPressedAccum = 8u;
  const uint32_t savedPrevious = previous;
  lockAvailable = false;
  const int savedGives = gives;
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  assert(gives == savedGives && previous == savedPrevious);
  assert(sInputCache.buttonPressedAccum == 8u);
  lockAvailable = true;
  sInputCache.dataValid = false;
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  sInputCache.dataValid = true;
  gInputRunning = false;
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  assert(sInputCache.buttonPressedAccum == 8u);
  gInputRunning = true;
  assert(inputConsumeButtonPresses(previous, initialized) == 8u);
  assert(takes == gives + 1);  // Only the simulated failed take lacked a give.
  // Production publisher preserves short press evidence even with released raw
  // buttons, while joystick noise of one unit does not advance activity.
  assert(inputCacheReset(UINT32_MAX, 512, 512, true));
  sInputCache.dataValid = true;
  const uint32_t seq = sInputCache.seq;
  assert(inputPublishGamepadState(UINT32_MAX, 513, 512, 2u, 40));
  assert(sInputCache.seq == seq && sInputCache.buttonPressedAccum == 2u);
  InputCache peek;
  assert(inputCacheSnapshot(peek, 10));
  assert(peek.buttonPressedAccum == 2u && sInputCache.buttonPressedAccum == 2u);
  assert(inputPublishGamepadState(~1u, 515, 512, 0, 41));
  assert(sInputCache.seq == seq + 1 && sInputCache.buttonPressedAccum == 3u);
  uint32_t rememberedSeq = 77;
  lockAvailable = false;
  assert(!inputActivitySequence(rememberedSeq, 0) && rememberedSeq == 77);
  assert(!inputCacheSnapshot(peek, 0) && !peek.dataValid && !peek.buttonPressedAccum);
  assert(!inputCacheInvalidate() && sInputCache.dataValid);
  lockAvailable = true;
  assert(inputCacheInvalidate() && !sInputCache.dataValid);
  sInputCacheMutex = nullptr;
  const int savedTakes = takes;
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  assert(takes == savedTakes);
}
