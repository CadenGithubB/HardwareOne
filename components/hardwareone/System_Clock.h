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

// 2100-01-01 — the shared upper bound for "could this possibly be a real
// wall-clock time on this device". One constant instead of the 4102444800
// literal scattered per call site.
constexpr time_t kEpoch2100 = 4102444800LL;

// isValidEpoch with the upper bound applied: [2020, 2100). Use when the
// value comes from an external device or user input, where garbage can be
// large as easily as small.
inline bool isPlausibleEpoch(time_t t) { return isValidEpoch(t) && t < kEpoch2100; }

// Format the CONFIGURED tz offset as "UTC+H:MM" / "UTC-H:MM" into `out`.
// Sign comes from the full offset, not the hours division — the hand-rolled
// versions this replaces printed negative sub-hour offsets with the sign
// lost (or fully inverted). Returns `out` for printf convenience.
const char* formatTzOffsetLabel(char* out, size_t outSize);

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

// Push the stored offset into the C library's TZ environment variable so
// localtime_r() and mktime() actually honor it. Call once at boot (from
// applySettings) and again whenever tzOffsetMinutes changes.
//
// Why this has to exist: TZ used to be set ONLY as a side effect of
// configTime() inside setupNTP(), which runs only once WiFi is up. On a boot
// that never joins WiFi — precisely when the DS3231 / R1-ring time sources
// matter — TZ stayed unset, so every localtime_r() in the firmware silently
// returned UTC while claiming to be local: dated capture folders, day-file
// rollover, log-line prefixes, the glasses' notification date, and the hour
// that TIME-triggered automations fire all shifted by the timezone offset
// relative to an otherwise identical WiFi boot. Setting TZ centrally makes
// "local" mean one thing on every boot path.
void applyTimezone();

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
// LOCAL time (localtime_r, honest only because applyTimezone() has run) so
// the string reflects the device's configured timezone. Returns the number
// of bytes written (excluding NUL). Returns 0 and writes "" if `t` is not a
// valid epoch — caller can detect with the return value rather than
// checking isValidEpoch up front.
size_t formatISO8601Local(time_t t, char* out, size_t outSize);

// Format epoch `t` as filename-safe "YYYYMMDD_HHMMSS" into `out`. Same
// local-time + invalid-handling behavior as formatISO8601Local. Used for
// log filenames, capture filenames, etc. where colons aren't allowed.
size_t formatFilenameLocal(time_t t, char* out, size_t outSize);

// Format epoch `t` as "HH:MM" into `out`. Local time. Convenient for status
// displays that just want a clock face.
size_t formatHHMMLocal(time_t t, char* out, size_t outSize);

// ----- Clock-step chokepoint ----------------------------------------------
//
// Six things can set this system's clock: NTP, the DS3231 RTC, `timeset`,
// the R1 ring (dark-boot adoption), the CM5 carrier's battery-backed RTC
// (SYNC_CM5, pushed over the UART host link), and lwIP's background SNTP
// daemon. Each
// step owes the same follow-up chores — post the TIME_SYNCED event on the
// first valid clock, write/refresh the boot anchor, resolve pending user
// timestamps, wake the automation scheduler, push the time into the RTC,
// log large corrections. Historically every source hand-rolled its own
// subset (timeset did none; the RTC paths did none; the ring path copied
// the NTP path and still missed one), so which chores ran depended on which
// source happened to win. clockStepped() is the single chokepoint: call it
// immediately after EVERY successful settimeofday(). Cheap chores run
// inline (any task except lwIP/BLE callbacks); filesystem-touching chores
// are pended and drained by clockDutiesTick() on the main loop (or
// clockDutiesFlush() synchronously from big-stack contexts).

enum SyncSource : uint8_t { SYNC_NONE = 0, SYNC_RTC, SYNC_NTP, SYNC_MANUAL, SYNC_RING, SYNC_CM5 };

// Last source that stepped the clock THIS boot. SYNC_NONE with isSynced()
// true means the time was carried across a soft reboot in the RTC domain.
SyncSource syncSource();
const char* syncSourceName(SyncSource s);  // "none|rtc|ntp|manual|ring|cm5"

// Call immediately AFTER a successful settimeofday(), passing the pre-step
// epoch. Inline-cheap; safe from any normal task. NOT for lwIP/BLE
// callbacks — those store what they know and let a drain call this.
void clockStepped(SyncSource src, time_t before);

// Main-loop drain for the filesystem-touching chores clockStepped() pends.
// Cheap no-op when nothing is pending or the filesystem isn't up yet.
void clockDutiesTick();

// Synchronous drain of the same chores — big-stack callers only (cmd_exec
// task / loopTask), for commands that want their side effects visible
// before returning.
void clockDutiesFlush();

// Projection of "now" in epoch-us from the last step's reference point.
// Sole intended consumer: the SNTP sync callback, which needs the PRE-step
// clock after lwIP has already stepped the real one. 0 = never seeded.
int64_t projectedEpochUs();
void refreshProjection();

} // namespace Clock

#endif // SYSTEM_CLOCK_H
