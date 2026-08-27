#include "System_MemTrackerCore.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

using TestRegistry = hw1_memtracker_detail::Registry<4>;

static void record(TestRegistry& registry, const char* tag, size_t bytes,
                   bool success, bool usedPS, bool fellBack) {
  assert(registry.record(tag, strlen(tag), bytes, success, usedPS, fellBack));
}

static MemTrackerSnapshot snapshot(const TestRegistry& registry,
                                   MemTrackerEntry* top = nullptr,
                                   size_t topCount = 0) {
  MemTrackerSnapshot result{};
  registry.snapshot(result, true, true, 3, 2, top, topCount);
  return result;
}

static void testAggregationAndTopK() {
  TestRegistry registry{};
  registry.reset(7, 1234);

  record(registry, "small", 10, true, false, false);
  record(registry, "large", 100, true, true, false);
  record(registry, "small", 20, true, false, true);
  record(registry, "failed", 999, false, false, false);

  MemTrackerEntry top[2]{};
  MemTrackerSnapshot result = snapshot(registry, top, 2);
  assert(result.initialized && result.enabled);
  assert(result.generation == 7 && result.resetAtMs == 1234);
  assert(result.entryCount == 3 && result.capacity == 4);
  assert(result.successCount == 3 && result.failureCount == 1);
  assert(result.fallbackCount == 1);
  assert(result.dramBytes == 30 && result.psramBytes == 100);
  assert(result.contentionDrops == 3 && result.invalidTagEvents == 2);
  assert(result.topCount == 2);
  assert(strcmp(top[0].tag, "large") == 0);
  assert(strcmp(top[1].tag, "small") == 0);
  assert(top[1].successCount == 2 && top[1].fallbackCount == 1);
}

static void testCapacityAndExistingTagAtCapacity() {
  TestRegistry registry{};
  registry.reset(1, 0);
  record(registry, "a", 1, true, false, false);
  record(registry, "b", 2, true, false, false);
  record(registry, "c", 3, true, true, false);
  record(registry, "d", 4, true, true, false);

  assert(!registry.record("overflow", strlen("overflow"), 5,
                          true, false, false));
  assert(registry.record("a", 1, 6, true, false, false));

  MemTrackerEntry top[4]{};
  MemTrackerSnapshot result = snapshot(registry, top, 4);
  assert(result.entryCount == 4);
  assert(result.overflowEvents == 1);
  // Aggregate traffic includes the unattributed full-table event.
  assert(result.successCount == 6);
  assert(result.dramBytes == 1 + 2 + 5 + 6);
  assert(result.psramBytes == 3 + 4);
  assert(result.topCount == 4);
  assert(strcmp(top[0].tag, "a") == 0 && top[0].dramBytes == 7);
}

static void testWideCountersAndReset() {
  TestRegistry registry{};
  registry.reset(41, 10);
  const size_t almostFourGiB = static_cast<size_t>(UINT32_MAX);
  record(registry, "wide", almostFourGiB, true, false, false);
  record(registry, "wide", almostFourGiB, true, false, false);

  MemTrackerSnapshot result = snapshot(registry);
  assert(result.dramBytes == static_cast<uint64_t>(UINT32_MAX) * 2u);
  assert(result.dramBytes > UINT32_MAX);

  registry.reset(42, 20);
  result = snapshot(registry);
  assert(result.generation == 42 && result.resetAtMs == 20);
  assert(result.entryCount == 0 && result.successCount == 0);
  assert(result.dramBytes == 0 && result.psramBytes == 0);
}

int main() {
  testAggregationAndTopK();
  testCapacityAndExistingTagAtCapacity();
  testWideCountersAndReset();
  puts("System_MemTrackerCore host tests passed");
  return 0;
}
