#ifndef SYSTEM_CLOCK_H
#define SYSTEM_CLOCK_H

#include <Arduino.h>
#include <stdint.h>
#include <time.h>

// =============================================================================
// Clock — time / sync state / timezone / formatting accessors
// =============================================================================
//
// Pre-consolidation footguns:
//   - "tm_year >= 120" (years since 2020) magic-number check repeated 6 places,
//     each one a "is the clock synced yet?" test rolled by hand
//   - Timezone offset stored as MINUTES (gSettings.tzOffsetMinutes) but G2's
//     TIME_SYNC frame wants QUARTER-HOURS. The memory file flags this swap
//     as an easy footgun. Explicit-unit accessors make the conversion
//     impossible to forget.
//   - "(millis() - timestamp) / 1000" repeated ~10 places for "age in seconds"
//   - strftime("%Y-%m-%dT%H:%M:%S") sprinkled across logging / filenames
//
// This namespace is the single source of truth. Unit names are in the
// function names: tzOffsetMinutes / tzOffsetQuarterHours, ageSeconds /
// ageMs. Callers literally cannot pick the wrong unit.
// =============================================================================

namespace Clock {

// ----- Wall-clock state ---------------------------------------------------

// Seconds since UNIX epoch. Wraps time(NULL). Returns 0 if not yet synced
// (i.e., when the system clock is still at boot epoch). Use with isSynced()
// when you need to distinguish "haven't synced" from "actually epoch=0".
inline time_t epochSeconds() { return time(nullptr); }

// True if the system clock has been synced to real wall-clock time. Uses
// the "tm_year >= 120" check (year >= 2020) which is the same heuristic the
// scattered call sites use — anything before 2020 is assumed to be the
// boot epoch (1970) or close to it.
bool isSynced();

// True if `t` looks like a real epoch (>= 2020-01-01). Same check as
// isSynced() but applied to an arbitrary timestamp — useful when validating
// data read from peers or settings files.
bool isValidEpoch(time_t t);

// Milliseconds since UNIX epoch (gettimeofday). Meaningful only when
// isSynced(); callers that can run pre-sync must gate on that themselves
// (e.g. the sensor-log CSV row stamp falls back to millis()). 64-bit —
// epoch-ms (~1.75e12) silently truncates in any 32-bit type.
int64_t epochMillis();

// ----- Timezone (unit names baked in) -------------------------------------

// Stored offset in MINUTES. This is gSettings.tzOffsetMinutes directly.
// Mirrors POSIX timezone convention: positive = east of UTC.
int tzOffsetMinutes();

// Stored offset converted to QUARTER-HOURS (15-minute increments). This
// is the encoding the G2 TIME_SYNC frame uses (1 byte signed quarter-hours
// fits the global -48..+56 range). Don't use this for anything that wants
// minutes — that's the footgun the explicit names defeat.
int tzOffsetQuarterHours();

// ----- Age / elapsed computations -----------------------------------------

// Seconds since `pastMs` (a millis() timestamp). Wraps the (millis() - ts)
// / 1000 pattern with a single rollover behavior — uses unsigned subtraction
// so millis() rollover (every 49.7 days) gives a sensible large age value
// rather than wrapping negative.
inline uint32_t ageSeconds(uint32_t pastMs) {
  return ((uint32_t)millis() - pastMs) / 1000;
}

// Milliseconds since `pastMs`. Same rollover behavior as ageSeconds. Use
// when you need sub-second precision (sensor freshness checks, ACK windows).
inline uint32_t ageMs(uint32_t pastMs) {
  return (uint32_t)millis() - pastMs;
}

// ----- Formatting ---------------------------------------------------------

// Format epoch `t` as ISO-8601 ("2026-05-23T15:04:05") into `out`. Uses
// LOCAL time (localtime_r) so the string reflects the device's configured
// timezone. Returns the number of bytes written (excluding NUL). Returns 0
// and writes "" if `t` is not a valid epoch — caller can detect with the
// return value rather than checking isValidEpoch up front.
size_t formatISO8601Local(time_t t, char* out, size_t outSize);

// Format epoch `t` as filename-safe "YYYYMMDD_HHMMSS" into `out`. Same
// local-time + invalid-handling behavior as formatISO8601Local. Used for
// log filenames, capture filenames, etc. where colons aren't allowed.
size_t formatFilenameLocal(time_t t, char* out, size_t outSize);

// Format epoch `t` as "HH:MM" into `out`. Local time. Convenient for status
// displays that just want a clock face.
size_t formatHHMMLocal(time_t t, char* out, size_t outSize);

} // namespace Clock

#endif // SYSTEM_CLOCK_H
