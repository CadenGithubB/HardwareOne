#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../System_EventCatalog.h"
#include "../../System_EventKindMask.h"

static constexpr size_t kFinalKind =
    static_cast<size_t>(SYSEVT_COUNT) - 1u;

static void testBoundaryBits() {
  uint32_t mask[8]{};
  assert(eventKindMaskBitCapacity(mask) == 256);

  assert(eventKindMaskSet(mask, 127, true));
  assert(eventKindMaskSet(mask, 128, true));
  assert(eventKindMaskSet(mask, kFinalKind, true));
  assert(eventKindMaskTest(mask, 127));
  assert(eventKindMaskTest(mask, 128));
  assert(eventKindMaskTest(mask, kFinalKind));

  const uint32_t (&constMask)[8] = mask;
  assert(eventKindMaskTest(constMask, 128));

  volatile uint32_t volatileMask[8]{};
  assert(eventKindMaskSet(volatileMask, kFinalKind, true));
  assert(eventKindMaskTest(volatileMask, kFinalKind));
  assert(eventKindMaskToggle(volatileMask, kFinalKind));
  assert(!eventKindMaskTest(volatileMask, kFinalKind));
}

static void testOutOfBoundsDoesNotMutate() {
  uint32_t mask[8]{};
  assert(eventKindMaskSet(mask, 31, true));
  uint32_t before[8];
  memcpy(before, mask, sizeof(mask));

  assert(!eventKindMaskSet(mask, 256, true));
  assert(!eventKindMaskToggle(mask, 256));
  assert(!eventKindMaskTest(mask, 256));
  assert(!eventKindMaskSet(mask, SIZE_MAX, true));
  assert(!eventKindMaskToggle(mask, SIZE_MAX));
  assert(!eventKindMaskTest(mask, SIZE_MAX));
  assert(memcmp(before, mask, sizeof(mask)) == 0);

  uint32_t narrow[4]{};
  assert(!eventKindMaskSet(narrow, 128, true));
  assert(!eventKindMaskToggle(narrow, 128));
  assert(!eventKindMaskTest(narrow, 128));
}

static void testLowTogglePreservesHighKinds() {
  uint32_t mask[8]{};
  assert(eventKindMaskSet(mask, 3, true));
  assert(eventKindMaskSet(mask, 128, true));
  assert(eventKindMaskSet(mask, kFinalKind, true));

  assert(eventKindMaskToggle(mask, 3));
  assert(!eventKindMaskTest(mask, 3));
  assert(eventKindMaskTest(mask, 128));
  assert(eventKindMaskTest(mask, kFinalKind));
}

int main() {
  testBoundaryBits();
  testOutOfBoundsDoesNotMutate();
  testLowTogglePreservesHighKinds();
  puts("event-kind mask tests passed");
  return 0;
}
