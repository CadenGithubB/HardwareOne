// System_AutomationId.h - the range of an automation id
//
// automations.json stores an id as a number, and every reader takes it as a
// signed 32-bit `long`: as<long>(), String::toInt, parseIntArg. A number above
// 2^31 - 1 reads there as 0 or as 2147483647, so such an automation cannot be
// addressed by id, is not rescheduled after it fires, and the next one like it
// is skipped as its duplicate. Ids used to be millis(), which passes 2^31
// after 24.9 days of uptime.
//
// New ids are made here and stay in 1..kAutomationIdMax (0 means "no id" to
// the scheduler's tables). A stored id above the range is given a new one by
// the sanitizer; the scheduler asks automationIdTextAboveRange() to find it.
#pragma once

#include <stdint.h>

constexpr uint32_t kAutomationIdMax = 0x7FFFFFFFu;

// The first candidate for a new id: the uptime in ms, folded into the range.
inline uint32_t automationIdSeed(uint32_t nowMs) {
  const uint32_t id = nowMs & kAutomationIdMax;
  return id ? id : 1u;
}

// The next candidate after `id` turned out to be taken: `stepBy` further on,
// still inside the range.
inline uint32_t automationIdAdvance(uint32_t id, uint32_t stepBy) {
  const uint32_t next = (id + stepBy) & kAutomationIdMax;
  return next ? next : 1u;
}

// True when the text of a stored id is a whole number above kAutomationIdMax
// that still fits 32 bits: an id made from millis() after 24.9 days of uptime,
// or given as `id=`. Anything else (in range, negative, a fraction, not a
// number, beyond 32 bits) is not this case; the sanitizer would not touch it.
inline bool automationIdTextAboveRange(const char* text) {
  if (!text) return false;
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
  uint64_t value = 0;
  int digits = 0;
  for (; *text >= '0' && *text <= '9'; ++text) {
    if (++digits > 10) return false;  // more digits than a 32-bit number has
    value = value * 10u + (uint32_t)(*text - '0');
  }
  while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
  if (digits == 0 || *text != '\0') return false;
  return value > kAutomationIdMax && value <= 0xFFFFFFFFull;
}
