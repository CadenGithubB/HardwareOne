#include <assert.h>
#include <stdint.h>

#include "System_VFSCapacityCache.h"

using VFS::CapacitySnapshot;
using VFS::detail::CapacitySnapshotCache;

static CapacitySnapshot sample(uint64_t total, uint64_t used,
                               uint32_t sampledAtMs) {
  CapacitySnapshot s{};
  s.totalBytes = total;
  s.usedBytes = used;
  s.freeBytes = total > used ? total - used : 0;
  s.sampledAtMs = sampledAtMs;
  return s;
}

static void test_zero_age_is_always_fresh() {
  CapacitySnapshotCache cache;
  const CapacitySnapshot first = sample(1000, 250, 100);
  assert(cache.publish(0, cache.generation(0), first));

  CapacitySnapshot out{};
  assert(!cache.read(0, 100, 0, &out));
  assert(!cache.read(0, 101, 0, &out));
}

static void test_expiry_and_millis_wrap() {
  CapacitySnapshotCache cache;
  const CapacitySnapshot first = sample(1000, 250, 100);
  assert(cache.publish(0, cache.generation(0), first));

  CapacitySnapshot out{};
  assert(cache.read(0, 1099, 1000, &out));
  assert(out.totalBytes == 1000);
  assert(out.usedBytes == 250);
  assert(out.freeBytes == 750);
  assert(!cache.read(0, 1100, 1000, &out));

  cache.invalidate(0);
  const CapacitySnapshot wrapped =
      sample(2000, 500, UINT32_MAX - 5u);
  assert(cache.publish(0, cache.generation(0), wrapped));
  assert(cache.read(0, 3, 10, &out));   // unsigned age is 9 ms
  assert(!cache.read(0, 4, 10, &out));  // exact max age is expired
}

static void test_invalidation_fences_slow_publication() {
  CapacitySnapshotCache cache;
  const uint32_t beforeWalk = cache.generation(0);
  const CapacitySnapshot stale = sample(1000, 300, 10);

  // Model an invalidation landing while LittleFS is being walked outside the
  // cache lock. The old generation must not be allowed to repopulate the slot.
  cache.invalidate(0);
  assert(!cache.publish(0, beforeWalk, stale));

  CapacitySnapshot out{};
  assert(!cache.read(0, 10, 1000, &out));

  const CapacitySnapshot fresh = sample(1000, 400, 11);
  assert(cache.publish(0, cache.generation(0), fresh));
  assert(cache.read(0, 11, 1000, &out));
  assert(out.usedBytes == 400);
}

static void test_tiers_are_independent() {
  CapacitySnapshotCache cache;
  const CapacitySnapshot internal = sample(1000, 100, 50);
  const CapacitySnapshot sd = sample(8000, 2000, 50);
  assert(cache.publish(0, cache.generation(0), internal));
  assert(cache.publish(1, cache.generation(1), sd));

  cache.invalidate(1);
  CapacitySnapshot out{};
  assert(cache.read(0, 51, 1000, &out));
  assert(out.totalBytes == 1000);
  assert(!cache.read(1, 51, 1000, &out));

  // Invalid tier operations fail closed and cannot disturb either real slot.
  assert(!cache.publish(2, 0, sd));
  cache.invalidate(2);
  assert(cache.read(0, 51, 1000, &out));
}

int main() {
  test_zero_age_is_always_fresh();
  test_expiry_and_millis_wrap();
  test_invalidation_fences_slow_publication();
  test_tiers_are_independent();
  return 0;
}
