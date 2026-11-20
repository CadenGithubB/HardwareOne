// heap_low event policy (System_HeapLowCore.h, G-71): one post per low
// episode, a 60 s cooldown, and re-arm only after recovery past the margin.
// The catalog row itself is pinned by the event_catalog fixture tests.
// Also the shared fragmentation figure and its warning line (fragPercent,
// fragWarn), used by memsample and the DEBUG_MEMORY task report.
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

  // Fragmentation: the share of free bytes outside the largest block,
  // 100 - largest * 100 / free with the quotient rounded down.
  static_assert(hl::kFragWarnPercent == 50, "one fragmentation warning line, 50 %");
  static_assert(hl::fragPercent(1000, 500) == 50, "usable in constant expressions");
  CHECK(hl::fragPercent(0, 0) == 0);
  CHECK(hl::fragPercent(0, 4096) == 0);            // no free bytes: 0, no division
  CHECK(hl::fragPercent(4096, 4096) == 0);         // one block
  CHECK(hl::fragPercent(4096, 4097) == 0);         // largest read after free grew: clamp, no wrap
  CHECK(hl::fragPercent(4096, SIZE_MAX) == 0);
  CHECK(hl::fragPercent(100, 0) == 100);
  CHECK(hl::fragPercent(3, 1) == 67);              // quotient rounds down, as memreport prints
  CHECK(hl::fragPercent(1000, 500) == 50);
  CHECK(hl::fragPercent(1000, 499) == 51);
  // memreport INTERNAL rows from the bench: ESP32-S3 (two boards), then
  // ESP32-P4 idle (three samples). The figures match what memreport printed.
  CHECK(hl::fragPercent(201795, 126976) == 38);
  CHECK(hl::fragPercent(155987, 81920) == 48);
  CHECK(hl::fragPercent(253383, 217088) == 15);
  CHECK(hl::fragPercent(237611, 200704) == 16);
  CHECK(hl::fragPercent(236279, 196608) == 17);
  // Large sizes keep the same result. largest * 100 passes 32 bits at about
  // 42 MB; size_t is 64-bit on the host, so these pin the values and do not
  // exercise the 32-bit product the targets have (the header widens it).
  CHECK(hl::fragPercent((size_t)64 * 1024 * 1024, (size_t)48 * 1024 * 1024) == 25);
  CHECK(hl::fragPercent(0xFFFFFFFFu, 0x80000000u) == 50);

  // The warning is strictly above the line, on the same figure.
  CHECK(!hl::fragWarn(1000, 510));                 // 49 %
  CHECK(!hl::fragWarn(1000, 500));                 // 50 %, at the line
  CHECK(hl::fragWarn(1000, 499));                  // 51 %
  CHECK(!hl::fragWarn(0, 0));
  CHECK(!hl::fragWarn(4096, 8192));                // clamped case never warns
  CHECK(!hl::fragWarn(201795, 126976));            // bench ESP32-S3, 38 %
  CHECK(!hl::fragWarn(155987, 81920));             // bench ESP32-S3, 48 %
  CHECK(hl::fragWarn(70487, 31744));               // 55 %
  // The fragmentation line is not part of the heap_low event.
  CHECK(!hl::isLow(kOk, hl::kLargestBytes));       // 96 KB free, 16 KB largest: 84 %, not low
  CHECK(hl::fragWarn(kOk, hl::kLargestBytes));

  std::printf("heap_low policy: passed\n");
  return 0;
}
