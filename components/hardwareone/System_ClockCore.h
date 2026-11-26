// System_ClockCore.h - the rules of time that need no device
//
// What System_Clock decides and formats, as pure functions on caller-owned
// values: no heap, no OS, no Arduino, no settings. System_Clock.cpp wraps
// these with the live clock and the configured timezone; a host test
// (test_clock_core.cpp, archived 2026-11-26) can run them as they are.
//
//   1. Validity: one vocabulary for "is this a real wall-clock time".
//   2. Sources and the rank table: which source may set a clock that another
//      source has already set (Clock::step).
//   3. Civil time: epoch <-> date and time fields, UTC or a fixed offset.
//   4. The formatter family: ISO UTC, compact UTC, local date and time, file
//      stamp, log prefix, durations and ages.
//   5. The parser timeset and rtcset share.
//   6. The timezone table that feeds the setup wizard and the settings list.
//
// Times are seconds since 1970 UTC as int64_t unless a name says Ms. Offsets
// are minutes east of UTC (gSettings.tzOffsetMinutes). Every formatter writes
// a NUL-terminated string, returns the number of characters written, and
// returns 0 with "" in `out` when the value cannot be shown or does not fit.
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace clockcore {

// ---------------------------------------------------------------------------
// 1. Validity
// ---------------------------------------------------------------------------

// 2020-01-01 00:00:00 UTC. A clock that was never set counts up from 1970, so
// anything earlier is "not set". The one lower bound of the firmware.
constexpr int64_t kEpoch2020 = 1577836800LL;
// 2100-01-01 00:00:00 UTC. The upper bound for a time that comes from outside
// (a peer, a chip, a typed value), where garbage is as easily large as small.
constexpr int64_t kEpoch2100 = 4102444800LL;

// True from 2020 on: the device's own clock reads as set.
constexpr bool isValidEpoch(int64_t t) { return t >= kEpoch2020; }
// True within [2020, 2100): a time from outside may be taken.
constexpr bool isPlausibleEpoch(int64_t t) { return t >= kEpoch2020 && t < kEpoch2100; }

// ---------------------------------------------------------------------------
// 2. Sources and the rank table
// ---------------------------------------------------------------------------

// What set the clock. The values are those of Clock::SyncSource
// (System_Clock.h asserts it); None while set means the time was carried over
// a soft restart.
enum class Source : uint8_t {
  None = 0,
  Rtc,      // the DS3231 chip
  Ntp,      // this device's own SNTP client
  Manual,   // an operator command: timeset, rtcset
  Ring,     // the R1 ring
  Cm5,      // the CM5 carrier over the UART host link
  Mesh,     // the mesh master, whose own time is not from NTP
  MeshNtp,  // the mesh master, whose own time is from NTP
  Count
};

// The rank table (owner decision 2026-11-22), strongest first:
//
//   7  ntp       this device's own NTP
//   6  manual    timeset / rtcset
//   5  mesh+ntp  the mesh master, when the master's clock is NTP-backed
//   4  cm5
//   3  rtc       DS3231
//   2  ring
//   1  mesh      mesh time without an NTP source
//   0  none      not set this boot (a clock carried over a restart)
constexpr uint8_t sourceRank(Source s) {
  return s == Source::Ntp ? 7
       : s == Source::Manual ? 6
       : s == Source::MeshNtp ? 5
       : s == Source::Cm5 ? 4
       : s == Source::Rtc ? 3
       : s == Source::Ring ? 2
       : s == Source::Mesh ? 1 : 0;
}

inline const char* sourceName(Source s) {
  switch (s) {
    case Source::Rtc:     return "rtc";
    case Source::Ntp:     return "ntp";
    case Source::Manual:  return "manual";
    case Source::Ring:    return "ring";
    case Source::Cm5:     return "cm5";
    case Source::Mesh:    return "mesh";
    case Source::MeshNtp: return "mesh-ntp";
    default:              return "none";
  }
}

enum class StepVerdict : uint8_t {
  Allowed = 0,
  RefusedImplausible,  // the time offered is not within 2020-2099
  RefusedOutranked,    // the clock is set from a source that ranks higher
  RefusedNoSource,     // Source::None sets nothing
};

// May `candidate` set the clock to `candidateEpoch`?
//
//   - The time must be plausible, whoever offers it.
//   - A clock that is not set takes any source.
//   - A clock that is set takes a source that ranks equal or higher than the
//     one that set it (`current`): a lower source never corrects a higher one.
//     NTP therefore overrides a manual time at its next reply, and nothing
//     but NTP or another operator command replaces a manual time.
//   - Manual is one exception: an operator command always sets the clock.
//     It is a deliberate act, and it is how a clock that an NTP reply moved
//     wrongly is put right (System_ClockConfirmPolicy.h).
//   - Mesh time is the other: it corrects mesh time, with or without NTP
//     behind either, down to the device's own source. A follower follows the
//     master it names, and that master's own source may change (its NTP gone,
//     a timeset on it), so the two mesh ranks order mesh time against the
//     OTHER sources, not against each other. But the master's time never
//     ranks below what the device had itself: `floor` is the source that set
//     the clock before mesh time took it over (None when the clock was not
//     set, or only carried over a restart). A device whose RTC chip set its
//     clock follows an NTP-backed master (mesh-ntp outranks rtc) and stops
//     following when that master's time is no longer from NTP (plain mesh
//     does not outrank rtc); a device with nothing of its own follows either.
constexpr bool sourceIsMesh(Source s) { return s == Source::Mesh || s == Source::MeshNtp; }

// May `candidate` correct a clock that `current` set? (The question without
// a time; mesh time asks it before it looks at the frame's time.) `floor`
// matters only while both are mesh time: see above.
constexpr bool sourceMayCorrect(Source candidate, Source current, bool clockSet, Source floor = Source::None) {
  return candidate != Source::None && candidate < Source::Count &&
         (!clockSet || candidate == Source::Manual ||
          (sourceIsMesh(candidate) && sourceIsMesh(current)
               ? sourceRank(candidate) >= sourceRank(floor)
               : sourceRank(candidate) >= sourceRank(current)));
}

// The floor after `candidate` has set a clock that `current` had set with
// `floor` (clockSet: the clock was set before the step). Mesh time taking
// over from another source remembers it; mesh time following mesh time keeps
// what is remembered; any other source clears it.
constexpr Source floorAfterStep(Source candidate, Source current, bool clockSet, Source floor) {
  return !sourceIsMesh(candidate) ? Source::None
       : !clockSet ? Source::None
       : sourceIsMesh(current) ? floor
       : current;
}

constexpr StepVerdict stepVerdict(Source candidate, int64_t candidateEpoch,
                                  Source current, bool clockSet, Source floor = Source::None) {
  return candidate == Source::None || candidate >= Source::Count ? StepVerdict::RefusedNoSource
       : !isPlausibleEpoch(candidateEpoch) ? StepVerdict::RefusedImplausible
       : sourceMayCorrect(candidate, current, clockSet, floor) ? StepVerdict::Allowed
       : StepVerdict::RefusedOutranked;
}

// The ledger: which source the clock's time is from. System_Clock keeps one
// under its lock; the functions are the whole of its bookkeeping.
//
// lwIP's SNTP client sets the clock on its own task, and the main loop
// reports that step a little later. Between the two the clock already shows
// NTP's time: ledgerCurrent says Ntp from the callback on (sntpPending), so a
// source that comes in between is ranked against NTP and not against the
// stale `source`. An operator command is the one source that may still
// replace the time then (sntpSuperseded); the report then runs under that
// command's name instead of relabelling its time as NTP's.
struct Ledger {
  Source source = Source::None;   // the last source that set the clock this boot
  Source floor = Source::None;    // what the clock had before mesh time took it over
  bool sntpPending = false;       // lwIP set the clock; the report has not run
  bool sntpSuperseded = false;    // ... and another source has set it since
};

// The source the clock's time is from right now.
constexpr Source ledgerCurrent(const Ledger& l) {
  return (l.sntpPending && !l.sntpSuperseded) ? Source::Ntp : l.source;
}

// May `candidate` set the clock to `candidateEpoch` now?
constexpr StepVerdict ledgerVerdict(const Ledger& l, Source candidate, int64_t candidateEpoch, bool clockSet) {
  return stepVerdict(candidate, candidateEpoch, ledgerCurrent(l), clockSet, l.floor);
}

// `src` has set the clock (clockSet: it was set before). Also the bookkeeping
// of a reported SNTP step, with src = the name ledgerOnSntpReport returned.
inline void ledgerOnStep(Ledger& l, Source src, bool clockSet) {
  l.floor = floorAfterStep(src, ledgerCurrent(l), clockSet, l.floor);
  l.source = src;
}

// A source other than lwIP has set the clock (Clock::step, after
// ledgerOnStep): an SNTP step that still waits for its report is replaced.
inline void ledgerAfterOwnStep(Ledger& l) {
  if (l.sntpPending) l.sntpSuperseded = true;
}

// The SNTP callback: lwIP has just set the clock.
inline void ledgerOnSntpPending(Ledger& l) {
  l.sntpPending = true;
  l.sntpSuperseded = false;
}

// The main-loop report of an SNTP step: the name its chores run under. Ntp,
// or the source that replaced NTP's time since. The caller then runs the
// step's chores, which end in ledgerOnStep with that name.
inline Source ledgerOnSntpReport(Ledger& l) {
  const Source name = (l.sntpPending && l.sntpSuperseded) ? l.source : Source::Ntp;
  l.sntpPending = false;
  l.sntpSuperseded = false;
  return name;
}

// ---------------------------------------------------------------------------
// 3. Civil time
// ---------------------------------------------------------------------------

struct Civil {
  int year;     // e.g. 2026
  int month;    // 1..12
  int day;      // 1..31
  int hour;     // 0..23
  int minute;   // 0..59
  int second;   // 0..59
  int weekday;  // 0 = Sunday .. 6 = Saturday
};

constexpr bool isLeapYear(int year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

constexpr int daysInMonth(int month, int year) {
  return month == 2 ? (isLeapYear(year) ? 29 : 28)
       : (month == 4 || month == 6 || month == 9 || month == 11) ? 30
       : (month >= 1 && month <= 12) ? 31 : 0;
}

// Days since 1970-01-01 of a proleptic Gregorian date (Howard Hinnant's
// days_from_civil). No timegm(), no TZ: the process-wide TZ is never touched.
constexpr int64_t daysFromCivil(int year, int month, int day) {
  const int y = year - (month <= 2 ? 1 : 0);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (unsigned)((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097LL + (int64_t)doe - 719468LL;
}

// True for a date and time that exist (31 February does not).
constexpr bool civilFieldsValid(int year, int month, int day, int hour, int minute, int second) {
  return year >= 1970 && year <= 9999 && month >= 1 && month <= 12 &&
         day >= 1 && day <= daysInMonth(month, year) &&
         hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59 && second >= 0 && second <= 59;
}

// UTC fields -> epoch seconds. The fields are taken as given (check them with
// civilFieldsValid first when they come from outside).
constexpr int64_t epochFromCivilUtc(int year, int month, int day, int hour, int minute, int second) {
  return daysFromCivil(year, month, day) * 86400LL + (int64_t)hour * 3600 + (int64_t)minute * 60 + second;
}

// Epoch seconds -> UTC fields (civil_from_days). Any epoch from 1970 on.
inline Civil civilFromEpoch(int64_t epoch) {
  Civil c = {};
  if (epoch < 0) epoch = 0;
  const int64_t days = epoch / 86400;
  const int64_t rem = epoch % 86400;
  c.hour = (int)(rem / 3600);
  c.minute = (int)((rem % 3600) / 60);
  c.second = (int)(rem % 60);
  c.weekday = (int)((days + 4) % 7);  // 1970-01-01 was a Thursday
  const int64_t z = days + 719468;
  const int64_t era = z / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  c.day = (int)(doy - (153 * mp + 2) / 5 + 1);
  c.month = (int)(mp < 10 ? mp + 3 : mp - 9);
  c.year = (int)(yoe + era * 400) + (c.month <= 2 ? 1 : 0);
  return c;
}

// Epoch seconds -> the fields a clock `tzOffsetMinutes` east of UTC shows.
inline Civil civilFromEpochLocal(int64_t epoch, int tzOffsetMinutes) {
  return civilFromEpoch(epoch + (int64_t)tzOffsetMinutes * 60);
}

// ---------------------------------------------------------------------------
// 4. Formatters
// ---------------------------------------------------------------------------

namespace detail {
// snprintf's result as "characters written, or 0 when it did not fit".
inline size_t fitted(int n, char* out, size_t cap) {
  if (n > 0 && (size_t)n < cap) return (size_t)n;
  if (out && cap) out[0] = '\0';
  return 0;
}
inline bool usable(char* out, size_t cap) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  return true;
}
}  // namespace detail

// "UTC", "UTC+1:00", "UTC-3:30": the label for a fixed offset.
inline size_t formatTzOffsetLabel(int tzOffsetMinutes, char* out, size_t cap) {
  if (!detail::usable(out, cap)) return 0;
  if (tzOffsetMinutes == 0) return detail::fitted(snprintf(out, cap, "UTC"), out, cap);
  const int a = tzOffsetMinutes < 0 ? -tzOffsetMinutes : tzOffsetMinutes;
  return detail::fitted(snprintf(out, cap, "UTC%c%d:%02d", tzOffsetMinutes < 0 ? '-' : '+', a / 60, a % 60),
                        out, cap);
}

// "+01:00", "-03:30", "+00:00": the numeric offset of ISO 8601, fixed width.
inline size_t formatTzOffsetNumeric(int tzOffsetMinutes, char* out, size_t cap) {
  if (!detail::usable(out, cap)) return 0;
  const int a = tzOffsetMinutes < 0 ? -tzOffsetMinutes : tzOffsetMinutes;
  return detail::fitted(snprintf(out, cap, "%c%02d:%02d", tzOffsetMinutes < 0 ? '-' : '+', a / 60, a % 60),
                        out, cap);
}

// Stored and sent text: "2026-11-22T14:30:05Z" (20 characters). UTC.
inline size_t formatIso8601Utc(int64_t epoch, char* out, size_t cap) {
  if (!detail::usable(out, cap) || !isValidEpoch(epoch)) return 0;
  const Civil c = civilFromEpoch(epoch);
  return detail::fitted(snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02dZ",
                                 c.year, c.month, c.day, c.hour, c.minute, c.second), out, cap);
}

// Reads exactly what formatIso8601Utc writes: "YYYY-MM-DDTHH:MM:SSZ", a date
// that exists, nothing before or after. False leaves *epochOut untouched.
inline bool parseIso8601Utc(const char* text, int64_t* epochOut) {
  static const char kShape[] = "dddd-dd-ddTdd:dd:ddZ";
  if (!text) return false;
  for (size_t i = 0; i < sizeof(kShape) - 1; ++i) {
    const char ch = text[i];
    if (kShape[i] == 'd' ? (ch < '0' || ch > '9') : ch != kShape[i]) return false;
  }
  if (text[sizeof(kShape) - 1] != '\0') return false;
  const auto two = [text](size_t at) { return (text[at] - '0') * 10 + (text[at + 1] - '0'); };
  const int year = two(0) * 100 + two(2);
  const int month = two(5), day = two(8), hour = two(11), minute = two(14), second = two(17);
  if (!civilFieldsValid(year, month, day, hour, minute, second)) return false;
  if (epochOut) *epochOut = epochFromCivilUtc(year, month, day, hour, minute, second);
  return true;
}

// Names and ids: "20261122T143005Z" (16 characters). UTC.
inline size_t formatCompactUtc(int64_t epoch, char* out, size_t cap) {
  if (!detail::usable(out, cap) || !isValidEpoch(epoch)) return 0;
  const Civil c = civilFromEpoch(epoch);
  return detail::fitted(snprintf(out, cap, "%04d%02d%02dT%02d%02d%02dZ",
                                 c.year, c.month, c.day, c.hour, c.minute, c.second), out, cap);
}

// Shown text: "2026-11-22 14:30" or "2026-11-22 14:30:05", device-local,
// 24 hour; with `tzLabel` the offset follows: "2026-11-22 14:30:05 UTC+1:00".
inline size_t formatDateTimeLocal(int64_t epoch, int tzOffsetMinutes, bool withSeconds, bool tzLabel,
                                  char* out, size_t cap) {
  if (!detail::usable(out, cap) || !isValidEpoch(epoch)) return 0;
  const Civil c = civilFromEpochLocal(epoch, tzOffsetMinutes);
  char label[12] = "";
  if (tzLabel) formatTzOffsetLabel(tzOffsetMinutes, label, sizeof(label));
  const int n = withSeconds
      ? snprintf(out, cap, "%04d-%02d-%02d %02d:%02d:%02d%s%s", c.year, c.month, c.day,
                 c.hour, c.minute, c.second, tzLabel ? " " : "", label)
      : snprintf(out, cap, "%04d-%02d-%02d %02d:%02d%s%s", c.year, c.month, c.day,
                 c.hour, c.minute, tzLabel ? " " : "", label);
  return detail::fitted(n, out, cap);
}

// "2026-11-22": the local date, the name of a daily file (it rolls at local
// midnight, owner decision 2026-11-22).
inline size_t formatDateLocal(int64_t epoch, int tzOffsetMinutes, char* out, size_t cap) {
  if (!detail::usable(out, cap) || !isValidEpoch(epoch)) return 0;
  const Civil c = civilFromEpochLocal(epoch, tzOffsetMinutes);
  return detail::fitted(snprintf(out, cap, "%04d-%02d-%02d", c.year, c.month, c.day), out, cap);
}

// "14:30": a clock face, device-local.
inline size_t formatHHMMLocal(int64_t epoch, int tzOffsetMinutes, char* out, size_t cap) {
  if (!detail::usable(out, cap) || !isValidEpoch(epoch)) return 0;
  const Civil c = civilFromEpochLocal(epoch, tzOffsetMinutes);
  return detail::fitted(snprintf(out, cap, "%02d:%02d", c.hour, c.minute), out, cap);
}

// File names: "20261122_143005" (15 characters), device-local, no character
// a filesystem refuses.
inline size_t formatFileStampLocal(int64_t epoch, int tzOffsetMinutes, char* out, size_t cap) {
  if (!detail::usable(out, cap) || !isValidEpoch(epoch)) return 0;
  const Civil c = civilFromEpochLocal(epoch, tzOffsetMinutes);
  return detail::fitted(snprintf(out, cap, "%04d%02d%02d_%02d%02d%02d",
                                 c.year, c.month, c.day, c.hour, c.minute, c.second), out, cap);
}

// The prefix of a stored log line: "[2026-11-22 14:30:05.123 +01:00] | ".
// Device-local with the offset in the line (owner decision 2026-11-22), so a
// line still names one instant after the timezone setting has changed.
inline size_t formatLogPrefixLocal(int64_t epochMs, int tzOffsetMinutes, char* out, size_t cap) {
  if (!detail::usable(out, cap) || epochMs < 0 || !isValidEpoch(epochMs / 1000)) return 0;
  const Civil c = civilFromEpochLocal(epochMs / 1000, tzOffsetMinutes);
  char offset[8];
  formatTzOffsetNumeric(tzOffsetMinutes, offset, sizeof(offset));
  return detail::fitted(snprintf(out, cap, "[%04d-%02d-%02d %02d:%02d:%02d.%03d %s] | ",
                                 c.year, c.month, c.day, c.hour, c.minute, c.second,
                                 (int)(epochMs % 1000), offset), out, cap);
}

// How a length of time is worded.
//   Compact: "45s", "3m 5s", "2h 15m", "3d 4h"       (lists, status lines)
//   Long:    "45 seconds", "3 minutes 5 seconds", "2 hours 15 minutes",
//            "1 day 4 hours"                           (sentences)
// The two largest units that are not zero-led: seconds are dropped from an
// hour on, minutes from a day on. Zero is "0s" / "0 seconds".
enum class DurationStyle : uint8_t { Compact = 0, Long };

inline size_t formatDuration(uint64_t seconds, DurationStyle style, char* out, size_t cap) {
  if (!detail::usable(out, cap)) return 0;
  const unsigned long long d = seconds / 86400, h = (seconds % 86400) / 3600,
                           m = (seconds % 3600) / 60, s = seconds % 60;
  unsigned long long major = s, minor = 0;
  const char* majorShort = "s";
  const char* minorShort = "";
  const char* majorLong = "second";
  const char* minorLong = "";
  bool two = false;
  if (d) { major = d; minor = h; majorShort = "d"; minorShort = "h"; majorLong = "day"; minorLong = "hour"; two = true; }
  else if (h) { major = h; minor = m; majorShort = "h"; minorShort = "m"; majorLong = "hour"; minorLong = "minute"; two = true; }
  else if (m) { major = m; minor = s; majorShort = "m"; minorShort = "s"; majorLong = "minute"; minorLong = "second"; two = true; }
  int n = 0;
  if (style == DurationStyle::Compact) {
    n = (two && minor) ? snprintf(out, cap, "%llu%s %llu%s", major, majorShort, minor, minorShort)
                       : snprintf(out, cap, "%llu%s", major, majorShort);
  } else {
    n = (two && minor) ? snprintf(out, cap, "%llu %s%s %llu %s%s", major, majorLong, major == 1 ? "" : "s",
                                  minor, minorLong, minor == 1 ? "" : "s")
                       : snprintf(out, cap, "%llu %s%s", major, majorLong, major == 1 ? "" : "s");
  }
  return detail::fitted(n, out, cap);
}

// How long ago: "just now" under kAgeJustNowSeconds, then the largest unit
// alone: "45s ago", "3m ago", "2h ago", "3d ago" (Compact) or "45 seconds
// ago", "1 minute ago" (Long).
constexpr uint64_t kAgeJustNowSeconds = 5;

inline size_t formatAge(uint64_t seconds, DurationStyle style, char* out, size_t cap) {
  if (!detail::usable(out, cap)) return 0;
  if (seconds < kAgeJustNowSeconds) return detail::fitted(snprintf(out, cap, "just now"), out, cap);
  unsigned long long value = seconds;
  const char* unitShort = "s";
  const char* unitLong = "second";
  if (seconds >= 86400) { value = seconds / 86400; unitShort = "d"; unitLong = "day"; }
  else if (seconds >= 3600) { value = seconds / 3600; unitShort = "h"; unitLong = "hour"; }
  else if (seconds >= 60) { value = seconds / 60; unitShort = "m"; unitLong = "minute"; }
  const int n = style == DurationStyle::Compact
      ? snprintf(out, cap, "%llu%s ago", value, unitShort)
      : snprintf(out, cap, "%llu %s%s ago", value, unitLong, value == 1 ? "" : "s");
  return detail::fitted(n, out, cap);
}

// ---------------------------------------------------------------------------
// 5. The time an operator types (timeset, rtcset)
// ---------------------------------------------------------------------------
//
//   <unix seconds>                       always UTC
//   YYYY-MM-DD HH:MM:SS                  device-local time
//   YYYY-MM-DD HH:MM:SS local            the same, said out loud
//   YYYY-MM-DD HH:MM:SS utc              UTC
//
// One reading for both commands. (Before 2026-11-22 timeset read the text as
// local and rtcset read the same text as UTC.) The word is not case
// sensitive. The result must lie within 2020-2099.

enum class TypedTimeError : uint8_t {
  None = 0,
  Empty,        // nothing typed
  Format,       // not one of the four shapes
  NotADate,     // a month, day or time of day that does not exist
  UnixNotLocal, // "<unix seconds> local": a Unix time has no zone to shift
  Range,        // outside 2020-2099
};

struct TypedTime {
  int64_t epoch;   // UTC seconds
  bool    wasUtc;  // true: a Unix time or the word utc; false: read as local
};

// The reply for an error, the same for both commands. nullptr for None.
inline const char* typedTimeErrorText(TypedTimeError e) {
  switch (e) {
    case TypedTimeError::None:         return nullptr;
    case TypedTimeError::Empty:
      return "Error: no time given. Use YYYY-MM-DD HH:MM:SS [utc|local] (local when no word is given) or a Unix timestamp.";
    case TypedTimeError::Format:
      return "Error: not a time this command reads. Use YYYY-MM-DD HH:MM:SS [utc|local], for example 2026-11-17 14:30:00, or a Unix timestamp.";
    case TypedTimeError::NotADate:
      return "Error: that date or time of day does not exist. Use YYYY-MM-DD HH:MM:SS, for example 2026-11-17 14:30:00.";
    case TypedTimeError::UnixNotLocal:
      return "Error: a Unix timestamp is always UTC. Leave out the word local, or type the date and time.";
    case TypedTimeError::Range:
      return "Error: the time must be within the years 2020 to 2099.";
  }
  return "Error: not a time this command reads.";
}

inline TypedTimeError parseTypedTime(const char* text, int tzOffsetMinutes, TypedTime* out) {
  if (!text) return TypedTimeError::Empty;
  while (*text == ' ' || *text == '\t') ++text;
  size_t len = strlen(text);
  while (len && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\r' || text[len - 1] == '\n')) --len;
  if (len == 0) return TypedTimeError::Empty;

  // An optional last word: utc or local.
  bool wordUtc = false, wordLocal = false;
  const auto endsWithWord = [&](const char* word) {
    const size_t w = strlen(word);
    if (len <= w) return false;
    const char before = text[len - w - 1];
    if (before != ' ' && before != '\t') return false;
    for (size_t i = 0; i < w; ++i) {
      char ch = text[len - w + i];
      if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
      if (ch != word[i]) return false;
    }
    return true;
  };
  if (endsWithWord("utc")) { wordUtc = true; len -= 3; }
  else if (endsWithWord("local")) { wordLocal = true; len -= 5; }
  while (len && (text[len - 1] == ' ' || text[len - 1] == '\t')) --len;
  if (len == 0) return TypedTimeError::Format;

  bool allDigits = true;
  for (size_t i = 0; i < len; ++i) {
    if (text[i] < '0' || text[i] > '9') { allDigits = false; break; }
  }

  TypedTime result = { 0, true };
  if (allDigits) {
    if (wordLocal) return TypedTimeError::UnixNotLocal;
    if (len > 11) return TypedTimeError::Range;  // more digits than any time before 2100 has
    int64_t value = 0;
    for (size_t i = 0; i < len; ++i) value = value * 10 + (text[i] - '0');
    result.epoch = value;
    result.wasUtc = true;
  } else {
    static const char kShape[] = "dddd-dd-dd dd:dd:dd";
    if (len != sizeof(kShape) - 1) return TypedTimeError::Format;
    for (size_t i = 0; i < len; ++i) {
      const char ch = text[i];
      if (kShape[i] == 'd' ? (ch < '0' || ch > '9') : ch != kShape[i]) return TypedTimeError::Format;
    }
    const auto two = [text](size_t at) { return (text[at] - '0') * 10 + (text[at + 1] - '0'); };
    const int year = two(0) * 100 + two(2);
    const int month = two(5), day = two(8), hour = two(11), minute = two(14), second = two(17);
    if (month < 1 || month > 12 || day < 1 || day > daysInMonth(month, year) ||
        hour > 23 || minute > 59 || second > 59) {
      return TypedTimeError::NotADate;
    }
    if (year < 1970) return TypedTimeError::Range;
    result.epoch = epochFromCivilUtc(year, month, day, hour, minute, second);
    result.wasUtc = wordUtc;
    if (!wordUtc) result.epoch -= (int64_t)tzOffsetMinutes * 60;
  }
  if (!isPlausibleEpoch(result.epoch)) return TypedTimeError::Range;
  if (out) *out = result;
  return TypedTimeError::None;
}

// ---------------------------------------------------------------------------
// 6. The timezone table
// ---------------------------------------------------------------------------
//
// The device keeps one fixed offset (tzOffsetMinutes). It does not change by
// itself in spring and autumn: there is NO AUTOMATIC DAYLIGHT SAVING, so a
// zone's summer time is an entry of its own that the user picks when the
// clocks change. This one table feeds the setup wizard (which steps through
// the entries) and the settings list (one line per offset, built by
// timezoneOptionsText).

struct TimezoneEntry {
  const char* abbrev;        // "EST"
  const char* place;         // "New York"
  int16_t     offsetMinutes; // minutes east of UTC
  bool        daylight;      // a zone's summer (daylight saving) time
};

// Shown with the timezone wherever there is room for a sentence.
inline constexpr char kTimezoneDstNote[] = "no automatic daylight saving";

// The wizard steps through these in this order.
inline constexpr TimezoneEntry kTimezones[] = {
  // UTC
  { "UTC",   "UTC",                      0, false },
  // North America, summer time
  { "EDT",   "New York",              -240, true  },
  { "CDT",   "Chicago",               -300, true  },
  { "MDT",   "Denver",                -360, true  },
  { "PDT",   "Los Angeles",           -420, true  },
  { "AKDT",  "Anchorage",             -480, true  },
  { "ADT",   "Halifax",               -180, true  },
  // North America, standard time
  { "AST",   "Halifax",               -240, false },
  { "EST",   "New York",              -300, false },
  { "CST",   "Chicago",               -360, false },
  { "MST",   "Denver/Phoenix",        -420, false },
  { "PST",   "Los Angeles",           -480, false },
  { "AKST",  "Anchorage",             -540, false },
  { "HST",   "Honolulu",              -600, false },
  { "SST",   "Pago Pago",             -660, false },
  { "AoE",   "Baker Island",          -720, false },
  // South America, Atlantic
  { "ART",   "Buenos Aires",          -180, false },
  { "FNT",   "Fernando de Noronha",   -120, false },
  { "AZOT",  "Azores",                 -60, false },
  // Europe, Africa
  { "GMT",   "London/Dublin",            0, false },
  { "BST",   "London",                  60, true  },
  { "CET",   "Paris/Berlin",            60, false },
  { "CEST",  "Paris/Berlin",           120, true  },
  { "EET",   "Athens/Cairo",           120, false },
  { "EEST",  "Athens",                 180, true  },
  { "MSK",   "Moscow/Istanbul",        180, false },
  // Asia
  { "GST",   "Dubai",                  240, false },
  { "PKT",   "Karachi",                300, false },
  { "IST",   "Mumbai/Delhi",           330, false },
  { "BTT",   "Dhaka/Thimphu",          360, false },
  { "ICT",   "Bangkok/Jakarta",        420, false },
  { "SGT",   "Singapore/Beijing",      480, false },
  { "JST",   "Tokyo/Seoul",            540, false },
  // Australia, Pacific
  { "ACST",  "Adelaide",               570, false },
  { "AEST",  "Sydney",                 600, false },
  { "ACDT",  "Adelaide",               630, true  },
  { "AEDT",  "Sydney",                 660, true  },
  { "SBT",   "Solomon Islands",        660, false },
  { "NZST",  "Auckland",               720, false },
  { "CHAST", "Chatham Islands",        765, false },
  { "NZDT",  "Auckland",               780, true  },
  { "TOT",   "Tonga",                  780, false },
  { "CHADT", "Chatham Islands",        825, true  },
  { "LINT",  "Kiritimati",             840, false },
};
inline constexpr size_t kTimezoneCount = sizeof(kTimezones) / sizeof(kTimezones[0]);

// The limits of the setting: the smallest and the largest offset in use.
constexpr int kTzOffsetMinMinutes = -720;
constexpr int kTzOffsetMaxMinutes = 840;

// The entry a stored offset re-opens on. Only the offset is stored, and two
// zones can share one (EST and CDT are both UTC-5:00), so the choice is fixed:
// the standard-time entry when there is one, else the first with that
// offset. -1 when no entry has the offset (a hand-typed one).
inline int timezoneIndexForOffset(int offsetMinutes) {
  int first = -1;
  for (size_t i = 0; i < kTimezoneCount; ++i) {
    if (kTimezones[i].offsetMinutes != offsetMinutes) continue;
    if (!kTimezones[i].daylight) return (int)i;
    if (first < 0) first = (int)i;
  }
  return first;
}

// The entry with this abbreviation (exact case), or -1.
inline int timezoneIndexByAbbrev(const char* abbrev) {
  if (!abbrev) return -1;
  for (size_t i = 0; i < kTimezoneCount; ++i) {
    if (strcmp(kTimezones[i].abbrev, abbrev) == 0) return (int)i;
  }
  return -1;
}

// The settings list: "minutes|label" pairs separated by commas, one per
// offset in the table, west to east:
//   "-300|UTC-5:00 (EST New York / CDT Chicago)"
// The standard-time zone is named first. Returns the length written; 0 with
// "" when `cap` is too small (kTimezoneOptionsCap is enough).
constexpr size_t kTimezoneOptionsCap = 2048;

inline size_t timezoneOptionsText(char* out, size_t cap) {
  if (!detail::usable(out, cap)) return 0;
  size_t used = 0;
  int last = -100000;
  for (;;) {
    // The next offset above the last one written.
    int next = 100000;
    for (size_t i = 0; i < kTimezoneCount; ++i) {
      const int o = kTimezones[i].offsetMinutes;
      if (o > last && o < next) next = o;
    }
    if (next == 100000) break;
    last = next;
    const int a = next < 0 ? -next : next;
    int n = next == 0 ? snprintf(out + used, cap - used, "%s0|UTC+0:00 (", used ? "," : "")
                      : snprintf(out + used, cap - used, "%s%d|UTC%c%d:%02d (", used ? "," : "", next,
                                 next < 0 ? '-' : '+', a / 60, a % 60);
    if (n < 0 || (size_t)n >= cap - used) { out[0] = '\0'; return 0; }
    used += (size_t)n;
    bool firstName = true;
    for (int pass = 0; pass < 2; ++pass) {  // standard time first, then summer time
      for (size_t i = 0; i < kTimezoneCount; ++i) {
        const TimezoneEntry& e = kTimezones[i];
        if (e.offsetMinutes != next || e.daylight != (pass == 1)) continue;
        // "UTC UTC" would say nothing: the UTC entry is its abbreviation alone.
        n = strcmp(e.abbrev, e.place) == 0
            ? snprintf(out + used, cap - used, "%s%s", firstName ? "" : " / ", e.abbrev)
            : snprintf(out + used, cap - used, "%s%s %s", firstName ? "" : " / ", e.abbrev, e.place);
        if (n < 0 || (size_t)n >= cap - used) { out[0] = '\0'; return 0; }
        used += (size_t)n;
        firstName = false;
      }
    }
    n = snprintf(out + used, cap - used, ")");
    if (n < 0 || (size_t)n >= cap - used) { out[0] = '\0'; return 0; }
    used += (size_t)n;
  }
  return used;
}

}  // namespace clockcore
