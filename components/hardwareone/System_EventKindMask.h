// System_EventKindMask.h - Bounds-safe operations for event-kind bit masks.
//
// The array reference is intentional: capacity is derived from the owning
// array at each call site, so a narrower temporary cannot be indexed using a
// wider global constant. This header stays dependency-light for host tests.
#ifndef SYSTEM_EVENTKINDMASK_H
#define SYSTEM_EVENTKINDMASK_H

#include <stddef.h>
#include <stdint.h>

template <size_t WordCount>
static constexpr size_t eventKindMaskBitCapacity(
    const uint32_t (&)[WordCount]) {
  return WordCount * 32u;
}

template <size_t WordCount>
static constexpr size_t eventKindMaskBitCapacity(
    const volatile uint32_t (&)[WordCount]) {
  return WordCount * 32u;
}

template <size_t WordCount>
static inline bool eventKindMaskTest(const uint32_t (&mask)[WordCount],
                                     size_t bit) {
  return bit < WordCount * 32u &&
         (mask[bit >> 5] & (uint32_t{1} << (bit & 31u))) != 0;
}

template <size_t WordCount>
static inline bool eventKindMaskTest(
    const volatile uint32_t (&mask)[WordCount], size_t bit) {
  return bit < WordCount * 32u &&
         (mask[bit >> 5] & (uint32_t{1} << (bit & 31u))) != 0;
}

template <size_t WordCount>
static inline bool eventKindMaskSet(uint32_t (&mask)[WordCount], size_t bit,
                                    bool on) {
  if (bit >= WordCount * 32u) return false;
  const uint32_t value = uint32_t{1} << (bit & 31u);
  if (on) mask[bit >> 5] |= value;
  else    mask[bit >> 5] &= ~value;
  return true;
}

template <size_t WordCount>
static inline bool eventKindMaskSet(volatile uint32_t (&mask)[WordCount],
                                    size_t bit, bool on) {
  if (bit >= WordCount * 32u) return false;
  const uint32_t value = uint32_t{1} << (bit & 31u);
  if (on) mask[bit >> 5] |= value;
  else    mask[bit >> 5] &= ~value;
  return true;
}

template <size_t WordCount>
static inline bool eventKindMaskToggle(uint32_t (&mask)[WordCount],
                                       size_t bit) {
  if (bit >= WordCount * 32u) return false;
  mask[bit >> 5] ^= uint32_t{1} << (bit & 31u);
  return true;
}

template <size_t WordCount>
static inline bool eventKindMaskToggle(volatile uint32_t (&mask)[WordCount],
                                       size_t bit) {
  if (bit >= WordCount * 32u) return false;
  mask[bit >> 5] ^= uint32_t{1} << (bit & 31u);
  return true;
}

#endif  // SYSTEM_EVENTKINDMASK_H
