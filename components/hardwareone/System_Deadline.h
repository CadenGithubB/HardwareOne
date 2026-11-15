// System_Deadline.h - millis() deadlines that survive the 49.7-day wrap
//
// A deadline is a uint32_t millis() value with 0 reserved for "no deadline".
// Ask deadlinePending() whether it still lies ahead. Never compare it with
// millis() through < or >=, and never take a bare signed difference:
//   - `millis() < until` reads a deadline set just before the wrap as passed
//     at once, and one left over from before the wrap as pending for days;
//   - `(int32_t)(until - millis()) > 0` without the zero test reads a deadline
//     that was never set as pending for as long as millis() is above 2^31
//     (uptime days 24.9 to 49.7).
// Delays up to INT32_MAX ms (24.8 days) are represented exactly; longer ones
// are capped so they read as "far ahead" rather than "already passed".
//
// The answer is good for 24.8 days after a deadline has passed. A deadline
// that stays in a variable longer than that is set back to 0 by its owner
// once it has passed (OLED_Utils.cpp does this for the dirty deadline).
//
// System_ESPNow_Deadline.h keeps the ESP-NOW names and forwards here.
#pragma once

#include <stdint.h>

// The deadline that is delayMs after nowMs; never returns 0.
inline uint32_t deadlineAfter(uint32_t nowMs, uint32_t delayMs) {
  if (delayMs > 0x7FFFFFFFu) delayMs = 0x7FFFFFFFu;
  const uint32_t deadlineMs = nowMs + delayMs;
  return deadlineMs ? deadlineMs : 1u;
}

// True while a set (non-zero) deadline lies ahead of nowMs.
inline bool deadlinePending(uint32_t deadlineMs, uint32_t nowMs) {
  return deadlineMs != 0 && (int32_t)(deadlineMs - nowMs) > 0;
}
