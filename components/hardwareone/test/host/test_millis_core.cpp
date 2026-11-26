// millisFromMicros (System_MillisCore.h) against the plain 64-bit division:
// every microsecond of the first second, the first ~6 days at a stride, both
// sides of every 2^32 boundary of the high word that a device can reach, and
// values spread over the whole 64-bit range.
#include "System_MillisCore.h"

#include <cstdint>
#include <cstdio>

static unsigned long sChecked = 0;
static unsigned long sFailed = 0;

static void check(uint64_t us) {
  ++sChecked;
  const uint32_t want = static_cast<uint32_t>(us / 1000ULL);
  const uint32_t got = millisFromMicros(us);
  if (got != want && sFailed++ < 10) {
    std::fprintf(stderr, "FAIL us=%llu got=%lu want=%lu\n",
                 static_cast<unsigned long long>(us), static_cast<unsigned long>(got),
                 static_cast<unsigned long>(want));
  }
}

int main() {
  for (uint64_t us = 0; us < 1000000ULL; ++us) check(us);
  for (uint64_t us = 0; us < 500000000000ULL; us += 99991ULL) check(us);
  // The high word reaches 1000 after ~49.7 days, where its remainder wraps.
  for (uint64_t hi = 0; hi < 4096; ++hi) {
    for (int64_t d = -2000; d <= 2000; ++d) {
      check((hi << 32) + static_cast<uint64_t>(d));
    }
  }
  uint64_t x = 0x9E3779B97F4A7C15ULL;
  for (int i = 0; i < 4000000; ++i) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    check(x);
  }
  check(UINT64_MAX);
  check(UINT64_MAX - 999);
  if (sFailed) {
    std::fprintf(stderr, "millis_core: %lu of %lu values differ\n", sFailed, sChecked);
    return 1;
  }
  std::printf("millis_core: %lu values match us / 1000\n", sChecked);
  return 0;
}
