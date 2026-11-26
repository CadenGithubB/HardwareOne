// System_MillisCore.h - millis() from the microsecond timer, dependency-free
// so the host tests can check it against the plain division.
#ifndef SYSTEM_MILLIS_CORE_H
#define SYSTEM_MILLIS_CORE_H

#include <stdint.h>

// Exactly (uint32_t)(us / 1000) for every us. The ESP32-S3 has a 32-bit
// divider but no 64-bit one, so the plain form is a libgcc routine; this is
// long division in 16-bit digits, three hardware divides. The high word's
// quotient only adds multiples of 2^32 and drops out of the 32-bit result.
static inline uint32_t millisFromMicros(uint64_t us) {
  const uint32_t hi = (uint32_t)(us >> 32);
  const uint32_t lo = (uint32_t)us;
  const uint32_t mid = ((hi % 1000u) << 16) | (lo >> 16);
  const uint32_t low = ((mid % 1000u) << 16) | (lo & 0xFFFFu);
  return ((mid / 1000u) << 16) + low / 1000u;
}

#endif  // SYSTEM_MILLIS_CORE_H
