// System_ESPNow_Deadline.h - millis()-based deadlines that survive the 49.7-day wrap
//
// A deadline is a uint32_t millis() value with 0 reserved for "no deadline".
// Compare through espnowDeadlinePending(), never with < or >=: an absolute
// comparison misreads a deadline that was set just before, or wrapped past,
// the uint32 rollover (prev-key window, sensor leases, pre-warm cooldown).
// Delays up to INT32_MAX ms (24.8 days) are represented exactly; longer ones
// are capped so they read as "far ahead" rather than "already passed".
//
// The rule itself lives in System_Deadline.h, which the rest of the firmware
// uses; the two names below are the ESP-NOW module's and forward to it.
#ifndef SYSTEM_ESPNOW_DEADLINE_H
#define SYSTEM_ESPNOW_DEADLINE_H

#include <stdint.h>

#include "System_Deadline.h"

// The deadline that is delayMs after nowMs; never returns 0.
inline uint32_t espnowDeadlineAfter(uint32_t nowMs, uint32_t delayMs) {
  return deadlineAfter(nowMs, delayMs);
}

// True while a set (non-zero) deadline lies ahead of nowMs.
inline bool espnowDeadlinePending(uint32_t deadlineMs, uint32_t nowMs) {
  return deadlinePending(deadlineMs, nowMs);
}

#endif  // SYSTEM_ESPNOW_DEADLINE_H
