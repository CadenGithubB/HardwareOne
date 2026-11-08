// heap_low event policy (System_HeapLowCore.h): one post per low
// episode, a 60 s cooldown, and re-arm only after recovery past the margin.
// The catalog row itself is pinned by the event_catalog fixture tests.
#include "System_HeapLowCore.h"

#include <cstdio>
#include <cstdlib>

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::abort(); \
} } while (false)

namespace hl = heap_low_core;

static constexpr size_t kOk = 96 * 1024;
static constexpr size_t kBigBlock = 64 * 1024;

int main() {
  // Thresholds are the documented ones.
  static_assert(hl::kFreeBytes == 25600, "free line is the 25 KB [HEAP_PRESSURE] line");
  static_assert(hl::kLargestBytes == 16384, "largest-block line is 16 KB");
  static_assert(hl::kCooldownMs == 60000, "60 s cooldown");
  CHECK(!hl::isLow(hl::kFreeBytes, hl::kLargestBytes));
  CHECK(hl::isLow(hl::kFreeBytes - 1, kBigBlock));
  CHECK(hl::isLow(kOk, hl::kLargestBytes - 1));

  // Healthy samples never post.
  hl::Watch w;
  for (uint32_t t = 0; t < 10; ++t) CHECK(!hl::update(w, kOk, kBigBlock, t * 1000));

  // Entering low posts once; staying low (e.g. 21 KB with the G2 page open)
  // never posts again, however long it lasts.
  CHECK(hl::update(w, 21000, 12000, 20000));
  for (uint32_t t = 21; t < 1000; ++t) CHECK(!hl::update(w, 21000, 12000, t * 1000));

  // Recovering just above the line does not re-arm (no flapping posts) ...
  CHECK(!hl::update(w, hl::kFreeBytes + 10, kBigBlock, 1000000));
  CHECK(!hl::update(w, 21000, kBigBlock, 1001000));
  // ... recovering past the margin does, and the next episode posts.
  CHECK(!hl::update(w, hl::kFreeBytes + hl::kRearmMarginBytes, kBigBlock, 1002000));
  CHECK(hl::update(w, 21000, kBigBlock, 1003000));

  // A second episode inside the cooldown waits for it, then posts once.
  CHECK(!hl::update(w, kOk, kBigBlock, 1004000));
  CHECK(!hl::update(w, kOk, hl::kLargestBytes - 1, 1010000));   // 7 s after the post
  CHECK(!hl::update(w, kOk, hl::kLargestBytes - 1, 1062000));   // 59 s
  CHECK(hl::update(w, kOk, hl::kLargestBytes - 1, 1063000));    // 60 s
  CHECK(!hl::update(w, kOk, hl::kLargestBytes - 1, 1200000));

  // millis() wrap: the cooldown still measures elapsed time.
  hl::Watch wrap;
  CHECK(hl::update(wrap, 1000, 1000, 0xFFFFF000u));
  CHECK(!hl::update(wrap, kOk, kBigBlock, 0xFFFFF100u));
  CHECK(!hl::update(wrap, 1000, 1000, 0x00001000u));            // ~8 s later
  CHECK(hl::update(wrap, 1000, 1000, 0xFFFFF000u + 60000u));

  std::printf("heap_low policy: passed\n");
  return 0;
}
