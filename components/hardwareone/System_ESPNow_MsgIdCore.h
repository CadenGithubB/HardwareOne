#pragma once
// This device's ESP-NOW message numbers (the header msgId), with no Arduino or
// IDF dependency so the host suite runs it (test_espnow_msgid_dedup.py).
//
// msgId 0 is never sent: a receiver skips the duplicate check for msgId 0, so
// a 0 would bypass loop suppression. The counter starts at a random nonzero
// value each time ESP-NOW starts (ESP-NOW 1.0, E21): a restarted sender no
// longer repeats the low numbers its peers still hold in their duplicate ring
// (5 s), which dropped its first frames after a restart. On wrap it goes from
// 0xFFFFFFFF to 1.
#include <stdint.h>

namespace espnowmsgid {

// The first msgId of a start, from a random 32-bit value: any value but 0.
inline uint32_t seed(uint32_t random) { return random != 0 ? random : 1u; }

// Hands out the counter's value and moves it on, skipping 0. Called from every
// sending task (espnow_task, cmd_exec, the sensor sender): one compare and swap,
// so two cores never take the same number. RELAXED is enough: msgIds only need
// to differ, not to order other memory.
inline uint32_t take(uint32_t* counter) {
  uint32_t cur = __atomic_load_n(counter, __ATOMIC_RELAXED);
  uint32_t id = 0, next = 0;
  do {
    id = cur != 0 ? cur : 1u;  // a counter of 0 (never seeded) hands out 1
    next = id + 1u;
    if (next == 0) next = 1u;
  } while (!__atomic_compare_exchange_n(counter, &cur, next, /*weak=*/true,
                                        __ATOMIC_RELAXED, __ATOMIC_RELAXED));
  return id;
}

}  // namespace espnowmsgid
