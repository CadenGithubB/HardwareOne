#include <assert.h>
#include <stdint.h>

using SemaphoreHandle_t = void*;
static constexpr int pdTRUE = 1;
#define pdMS_TO_TICKS(ms) (ms)
static bool lockAvailable = true;
static int takes = 0, gives = 0;
static int xSemaphoreTake(SemaphoreHandle_t, unsigned) { ++takes; return lockAvailable ? pdTRUE : 0; }
static void xSemaphoreGive(SemaphoreHandle_t) { ++gives; }
static bool gInputRunning = true;
static struct {
  SemaphoreHandle_t mutex = reinterpret_cast<void*>(1);
  uint32_t buttons = UINT32_MAX;
  uint32_t buttonPressedAccum = 0;
  bool dataValid = true;
} gInputCache;

// This header is extracted from the actual production HAL_Input.cpp function
// by the test runner; the test never reimplements the snapshot operation.
#include "input_snapshot_under_test.h"

int main() {
  uint32_t previous = UINT32_MAX;
  bool initialized = false;
  // A raw switch held when a wizard opens establishes a baseline, not a click.
  gInputCache.buttons = ~1u;
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  assert(initialized && previous == ~1u);
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  gInputCache.buttons = UINT32_MAX;
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  gInputCache.buttons = ~1u;
  gInputCache.buttonPressedAccum = 1u;
  assert(inputConsumeButtonPresses(previous, initialized) == 1u);
  assert(gInputCache.buttonPressedAccum == 0);
  assert(inputConsumeButtonPresses(previous, initialized) == 0);

  // A complete press/release shorter than a display frame must survive, then
  // be consumed exactly once even though the current raw state is released.
  gInputCache.buttons = UINT32_MAX;
  gInputCache.buttonPressedAccum = 2u | 32u;
  assert(inputConsumeButtonPresses(previous, initialized) == (2u | 32u));
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  initialized = false;
  gInputCache.buttonPressedAccum = 1u;
  assert(inputConsumeButtonPresses(previous, initialized) == 1u);
  assert(inputConsumeButtonPresses(previous, initialized) == 0);

  // The three auxiliary keys share this same latch. Simultaneously held
  // X/Y/START keys each emit one press, then release independently; a new
  // X press must remain visible while Y and START stay held.
  constexpr uint32_t auxiliaryMask = 4u | 8u | 16u;
  gInputCache.buttons = ~auxiliaryMask;
  gInputCache.buttonPressedAccum = auxiliaryMask;
  assert(inputConsumeButtonPresses(previous, initialized) == auxiliaryMask);
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  gInputCache.buttons = ~(8u | 16u);
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  gInputCache.buttons = ~auxiliaryMask;
  gInputCache.buttonPressedAccum = 4u;
  assert(inputConsumeButtonPresses(previous, initialized) == 4u);
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  gInputCache.buttons = UINT32_MAX;
  assert(inputConsumeButtonPresses(previous, initialized) == 0);

  // Contention and invalid/stopped caches must not consume pending edges or
  // corrupt the reader's prior state. All successful takes release the lock.
  gInputCache.buttonPressedAccum = 8u;
  const uint32_t savedPrevious = previous;
  lockAvailable = false;
  const int savedGives = gives;
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  assert(gives == savedGives && previous == savedPrevious);
  assert(gInputCache.buttonPressedAccum == 8u);
  lockAvailable = true;
  gInputCache.dataValid = false;
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  gInputCache.dataValid = true;
  gInputRunning = false;
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  assert(gInputCache.buttonPressedAccum == 8u);
  gInputRunning = true;
  assert(inputConsumeButtonPresses(previous, initialized) == 8u);
  assert(takes == gives + 1);  // Only the simulated failed take lacked a give.
  gInputCache.mutex = nullptr;
  const int savedTakes = takes;
  assert(inputConsumeButtonPresses(previous, initialized) == 0);
  assert(takes == savedTakes);
}
