// Exercises the production clock rules (System_ClockCore.h) as they are: the
// validity bounds, the source rank table behind Clock::step, civil time, the
// formatter family, the time timeset and rtcset read, and the timezone table.
// No Arduino/IDF replacements here.
#include "System_ClockCore.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <set>
#include <string>

static unsigned checks = 0;
#define CHECK(condition) \
  do { ++checks; if (!(condition)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); std::exit(1); } } while (0)
#define CHECK_TEXT(got, want) \
  do { ++checks; if (std::string(got) != (want)) { std::fprintf(stderr, "%s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, std::string(got).c_str(), want); std::exit(1); } } while (0)

using namespace clockcore;

// 2026-11-22 14:30:05 UTC.
static constexpr int64_t kNow = 1795357805LL;

static void validity() {
  CHECK(!isValidEpoch(0));
  CHECK(!isValidEpoch(5));
  CHECK(!isValidEpoch(200000));            // 55 hours of uptime without a time source
  CHECK(!isValidEpoch(kEpoch2020 - 1));
  CHECK(isValidEpoch(kEpoch2020));
  CHECK(isValidEpoch(kEpoch2100 + 5));     // the own clock has no upper bound
  CHECK(!isPlausibleEpoch(kEpoch2020 - 1));
  CHECK(isPlausibleEpoch(kEpoch2020));
  CHECK(isPlausibleEpoch(kEpoch2100 - 1));
  CHECK(!isPlausibleEpoch(kEpoch2100));
  CHECK(!isPlausibleEpoch(INT64_MAX));
  CHECK(!isPlausibleEpoch(-1));
  CHECK(kEpoch2020 == epochFromCivilUtc(2020, 1, 1, 0, 0, 0));
  CHECK(kEpoch2100 == epochFromCivilUtc(2100, 1, 1, 0, 0, 0));
}

static void rankTable() {
  // The owner's order, strongest first.
  const Source order[] = { Source::Ntp, Source::Manual, Source::MeshNtp, Source::Cm5,
                           Source::Rtc, Source::Ring, Source::Mesh, Source::None };
  for (size_t i = 0; i + 1 < sizeof(order) / sizeof(order[0]); ++i) {
    CHECK(sourceRank(order[i]) > sourceRank(order[i + 1]));
  }
  const Source real[] = { Source::Rtc, Source::Ntp, Source::Manual, Source::Ring,
                          Source::Cm5, Source::Mesh, Source::MeshNtp };

  // A clock that is not set takes every source, with a plausible time only.
  for (Source candidate : real) {
    CHECK(stepVerdict(candidate, kNow, Source::None, false) == StepVerdict::Allowed);
    CHECK(stepVerdict(candidate, 1000, Source::None, false) == StepVerdict::RefusedImplausible);
    CHECK(stepVerdict(candidate, kEpoch2100, Source::None, false) == StepVerdict::RefusedImplausible);
    CHECK(sourceMayCorrect(candidate, Source::None, false));
  }
  CHECK(stepVerdict(Source::None, kNow, Source::None, false) == StepVerdict::RefusedNoSource);
  CHECK(stepVerdict(Source::Count, kNow, Source::None, false) == StepVerdict::RefusedNoSource);
  CHECK(!sourceMayCorrect(Source::None, Source::None, false));

  // A clock that is set: equal or higher corrects, lower does not; manual always does.
  for (Source candidate : real) {
    for (Source current : order) {
      const bool bothMesh = sourceIsMesh(candidate) && sourceIsMesh(current);
      const bool expected = candidate == Source::Manual || bothMesh || sourceRank(candidate) >= sourceRank(current);
      CHECK((stepVerdict(candidate, kNow, current, true) == StepVerdict::Allowed) == expected);
      CHECK(sourceMayCorrect(candidate, current, true) == expected);
      if (!expected) CHECK(stepVerdict(candidate, kNow, current, true) == StepVerdict::RefusedOutranked);
      // An implausible time is refused before the rank is asked.
      CHECK(stepVerdict(candidate, 12, current, true) == StepVerdict::RefusedImplausible);
    }
  }

  // The cases the owner named.
  CHECK(stepVerdict(Source::Ntp, kNow, Source::Manual, true) == StepVerdict::Allowed);       // NTP overrides manual
  CHECK(stepVerdict(Source::Manual, kNow, Source::Ntp, true) == StepVerdict::Allowed);       // an operator command always sets
  CHECK(stepVerdict(Source::MeshNtp, kNow, Source::Cm5, true) == StepVerdict::Allowed);      // NTP-backed mesh over CM5
  CHECK(stepVerdict(Source::MeshNtp, kNow, Source::Rtc, true) == StepVerdict::Allowed);      // ... and over the chip
  CHECK(stepVerdict(Source::MeshNtp, kNow, Source::Manual, true) == StepVerdict::RefusedOutranked);
  CHECK(stepVerdict(Source::MeshNtp, kNow, Source::Ntp, true) == StepVerdict::RefusedOutranked);
  CHECK(stepVerdict(Source::Cm5, kNow, Source::MeshNtp, true) == StepVerdict::RefusedOutranked);
  CHECK(stepVerdict(Source::Rtc, kNow, Source::Cm5, true) == StepVerdict::RefusedOutranked);
  CHECK(stepVerdict(Source::Ring, kNow, Source::Rtc, true) == StepVerdict::RefusedOutranked);
  CHECK(stepVerdict(Source::Mesh, kNow, Source::Ring, true) == StepVerdict::RefusedOutranked);  // mesh without NTP is last
  CHECK(stepVerdict(Source::Mesh, kNow, Source::Mesh, true) == StepVerdict::Allowed);
  // A follower follows its master: mesh time corrects mesh time either way.
  CHECK(stepVerdict(Source::Mesh, kNow, Source::MeshNtp, true) == StepVerdict::Allowed);
  CHECK(stepVerdict(Source::MeshNtp, kNow, Source::Mesh, true) == StepVerdict::Allowed);
  CHECK(sourceIsMesh(Source::Mesh) && sourceIsMesh(Source::MeshNtp) && !sourceIsMesh(Source::Ntp) && !sourceIsMesh(Source::None));
  CHECK(stepVerdict(Source::Mesh, kNow, Source::None, true) == StepVerdict::Allowed);        // a carried clock
  CHECK(stepVerdict(Source::Rtc, kNow, Source::Rtc, true) == StepVerdict::Allowed);

  CHECK_TEXT(sourceName(Source::MeshNtp), "mesh-ntp");
  CHECK_TEXT(sourceName(Source::None), "none");
  std::set<std::string> names;
  for (Source s : real) names.insert(sourceName(s));
  CHECK(names.size() == 7);
}

static void civil() {
  const Civil c = civilFromEpoch(kNow);
  CHECK(c.year == 2026 && c.month == 11 && c.day == 22 && c.hour == 14 && c.minute == 30 && c.second == 5);
  CHECK(c.weekday == 0);  // a Sunday
  const Civil leap = civilFromEpoch(epochFromCivilUtc(2024, 2, 29, 23, 59, 59));
  CHECK(leap.year == 2024 && leap.month == 2 && leap.day == 29 && leap.hour == 23 && leap.second == 59);
  CHECK(leap.weekday == 4);  // a Thursday
  const Civil zero = civilFromEpoch(0);
  CHECK(zero.year == 1970 && zero.month == 1 && zero.day == 1 && zero.weekday == 4);

  // Every day from 2020 to 2100 and some seconds in it, against the C library.
  for (int64_t t = kEpoch2020; t < kEpoch2100; t += 86400 - 7) {
    const time_t tt = (time_t)t;
    struct tm utc;
    gmtime_r(&tt, &utc);
    const Civil got = civilFromEpoch(t);
    CHECK(got.year == utc.tm_year + 1900 && got.month == utc.tm_mon + 1 && got.day == utc.tm_mday &&
          got.hour == utc.tm_hour && got.minute == utc.tm_min && got.second == utc.tm_sec &&
          got.weekday == utc.tm_wday);
    CHECK(epochFromCivilUtc(got.year, got.month, got.day, got.hour, got.minute, got.second) == t);
  }

  // A fixed offset moves the date with it.
  const Civil east = civilFromEpochLocal(epochFromCivilUtc(2026, 12, 31, 23, 30, 0), 60);
  CHECK(east.year == 2027 && east.month == 1 && east.day == 1 && east.hour == 0 && east.minute == 30);
  const Civil west = civilFromEpochLocal(epochFromCivilUtc(2026, 3, 1, 2, 0, 0), -300);
  CHECK(west.month == 2 && west.day == 28 && west.hour == 21);

  CHECK(daysInMonth(2, 2024) == 29 && daysInMonth(2, 2026) == 28 && daysInMonth(2, 2100) == 28);
  CHECK(daysInMonth(4, 2026) == 30 && daysInMonth(12, 2026) == 31 && daysInMonth(13, 2026) == 0);
  CHECK(civilFieldsValid(2026, 11, 22, 14, 30, 5));
  CHECK(!civilFieldsValid(2026, 2, 31, 0, 0, 0));
  CHECK(!civilFieldsValid(2026, 11, 22, 24, 0, 0));
  CHECK(!civilFieldsValid(2026, 0, 1, 0, 0, 0));
}

static void formatters() {
  char buf[64];

  CHECK(formatIso8601Utc(kNow, buf, sizeof(buf)) == 20);
  CHECK_TEXT(buf, "2026-11-22T14:30:05Z");
  CHECK(formatIso8601Utc(kNow, buf, 21) == 20);
  std::snprintf(buf, sizeof(buf), "untouched");
  CHECK(formatIso8601Utc(kNow, buf, 20) == 0 && buf[0] == '\0');   // one short: nothing, not a cut string
  CHECK(formatIso8601Utc(100, buf, sizeof(buf)) == 0 && buf[0] == '\0');  // a clock that is not set
  CHECK(formatIso8601Utc(kNow, nullptr, 0) == 0);

  int64_t parsed = 0;
  CHECK(parseIso8601Utc("2026-11-22T14:30:05Z", &parsed) && parsed == kNow);
  CHECK(parseIso8601Utc("2020-01-01T00:00:00Z", &parsed) && parsed == kEpoch2020);
  CHECK(parseIso8601Utc("2026-11-22T14:30:05Z", nullptr));
  parsed = 7;
  for (const char* bad : { "", "2026-11-22T14:30:05", "2026-11-22 14:30:05Z", "2026-11-22T14:30:05Z ",
                           " 2026-11-22T14:30:05Z", "2026-02-31T00:00:00Z", "2026-11-22T24:00:00Z",
                           "2026-13-01T00:00:00Z", "2026-11-22T14:30:5Z", "2026-11-22t14:30:05z",
                           "20261122T143005Z", "1st Power Cycle" }) {
    CHECK(!parseIso8601Utc(bad, &parsed) && parsed == 7);
  }
  CHECK(!parseIso8601Utc(nullptr, &parsed));
  // What is written is read back.
  for (int64_t t = kEpoch2020; t < kEpoch2100; t += 86400 * 37 + 3601) {
    CHECK(formatIso8601Utc(t, buf, sizeof(buf)) == 20);
    CHECK(parseIso8601Utc(buf, &parsed) && parsed == t);
  }

  CHECK(formatCompactUtc(kNow, buf, sizeof(buf)) == 16);
  CHECK_TEXT(buf, "20261122T143005Z");
  CHECK(formatCompactUtc(0, buf, sizeof(buf)) == 0 && buf[0] == '\0');

  CHECK(formatDateTimeLocal(kNow, 0, true, false, buf, sizeof(buf)) == 19);
  CHECK_TEXT(buf, "2026-11-22 14:30:05");
  formatDateTimeLocal(kNow, 0, false, false, buf, sizeof(buf));
  CHECK_TEXT(buf, "2026-11-22 14:30");
  formatDateTimeLocal(kNow, 60, true, true, buf, sizeof(buf));
  CHECK_TEXT(buf, "2026-11-22 15:30:05 UTC+1:00");
  formatDateTimeLocal(kNow, -300, false, true, buf, sizeof(buf));
  CHECK_TEXT(buf, "2026-11-22 09:30 UTC-5:00");
  formatDateTimeLocal(kNow, 0, true, true, buf, sizeof(buf));
  CHECK_TEXT(buf, "2026-11-22 14:30:05 UTC");
  formatDateTimeLocal(kNow, 765, true, true, buf, sizeof(buf));
  CHECK_TEXT(buf, "2026-11-23 03:15:05 UTC+12:45");
  formatDateTimeLocal(kNow, 840, false, false, buf, sizeof(buf));
  CHECK_TEXT(buf, "2026-11-23 04:30");
  CHECK(formatDateTimeLocal(50, 60, true, true, buf, sizeof(buf)) == 0 && buf[0] == '\0');
  CHECK(formatDateTimeLocal(kNow, 60, true, true, buf, 10) == 0 && buf[0] == '\0');

  formatDateLocal(kNow, 600, buf, sizeof(buf));
  CHECK_TEXT(buf, "2026-11-23");      // the daily file rolls at local midnight
  formatDateLocal(kNow, -600, buf, sizeof(buf));
  CHECK_TEXT(buf, "2026-11-22");
  formatHHMMLocal(kNow, -210, buf, sizeof(buf));
  CHECK_TEXT(buf, "11:00");

  CHECK(formatFileStampLocal(kNow, 60, buf, sizeof(buf)) == 15);
  CHECK_TEXT(buf, "20261122_153005");
  CHECK(formatFileStampLocal(3, 60, buf, sizeof(buf)) == 0);

  formatLogPrefixLocal(kNow * 1000 + 123, 60, buf, sizeof(buf));
  CHECK_TEXT(buf, "[2026-11-22 15:30:05.123 +01:00] | ");
  formatLogPrefixLocal(kNow * 1000 + 7, -210, buf, sizeof(buf));
  CHECK_TEXT(buf, "[2026-11-22 11:00:05.007 -03:30] | ");
  formatLogPrefixLocal(kNow * 1000, 0, buf, sizeof(buf));
  CHECK_TEXT(buf, "[2026-11-22 14:30:05.000 +00:00] | ");
  CHECK(formatLogPrefixLocal(123456, 0, buf, sizeof(buf)) == 0 && buf[0] == '\0');  // uptime, not a time
  CHECK(formatLogPrefixLocal(-5, 0, buf, sizeof(buf)) == 0);

  formatTzOffsetLabel(0, buf, sizeof(buf));     CHECK_TEXT(buf, "UTC");
  formatTzOffsetLabel(330, buf, sizeof(buf));   CHECK_TEXT(buf, "UTC+5:30");
  formatTzOffsetLabel(-30, buf, sizeof(buf));   CHECK_TEXT(buf, "UTC-0:30");  // the sign of a sub-hour offset is kept
  formatTzOffsetLabel(-720, buf, sizeof(buf));  CHECK_TEXT(buf, "UTC-12:00");
  formatTzOffsetNumeric(-30, buf, sizeof(buf)); CHECK_TEXT(buf, "-00:30");
  formatTzOffsetNumeric(840, buf, sizeof(buf)); CHECK_TEXT(buf, "+14:00");
}

static void durations() {
  char buf[48];
  struct Case { uint64_t seconds; const char* compact; const char* longer; };
  const Case cases[] = {
    { 0, "0s", "0 seconds" },
    { 1, "1s", "1 second" },
    { 45, "45s", "45 seconds" },
    { 60, "1m", "1 minute" },
    { 61, "1m 1s", "1 minute 1 second" },
    { 185, "3m 5s", "3 minutes 5 seconds" },
    { 3600, "1h", "1 hour" },
    { 3659, "1h", "1 hour" },                    // seconds are dropped from an hour on
    { 8100, "2h 15m", "2 hours 15 minutes" },
    { 86400, "1d", "1 day" },
    { 86400 + 4 * 3600 + 59 * 60, "1d 4h", "1 day 4 hours" },
    { 3 * 86400 + 3600, "3d 1h", "3 days 1 hour" },
    { 4294967295ULL / 1000, "49d 17h", "49 days 17 hours" },   // the millis() wrap
    { 400ULL * 86400, "400d", "400 days" },
  };
  for (const Case& c : cases) {
    CHECK(formatDuration(c.seconds, DurationStyle::Compact, buf, sizeof(buf)) > 0);
    CHECK_TEXT(buf, c.compact);
    CHECK(formatDuration(c.seconds, DurationStyle::Long, buf, sizeof(buf)) > 0);
    CHECK_TEXT(buf, c.longer);
  }
  CHECK(formatDuration(185, DurationStyle::Compact, buf, 5) == 0 && buf[0] == '\0');
  CHECK(formatDuration(185, DurationStyle::Compact, buf, 6) == 5);

  struct Age { uint64_t seconds; const char* compact; const char* longer; };
  const Age ages[] = {
    { 0, "just now", "just now" },
    { 4, "just now", "just now" },
    { 5, "5s ago", "5 seconds ago" },
    { 59, "59s ago", "59 seconds ago" },
    { 60, "1m ago", "1 minute ago" },
    { 119, "1m ago", "1 minute ago" },
    { 3599, "59m ago", "59 minutes ago" },
    { 3600, "1h ago", "1 hour ago" },
    { 86399, "23h ago", "23 hours ago" },
    { 86400, "1d ago", "1 day ago" },
    { 10 * 86400 + 5, "10d ago", "10 days ago" },
  };
  for (const Age& a : ages) {
    formatAge(a.seconds, DurationStyle::Compact, buf, sizeof(buf));
    CHECK_TEXT(buf, a.compact);
    formatAge(a.seconds, DurationStyle::Long, buf, sizeof(buf));
    CHECK_TEXT(buf, a.longer);
  }
  CHECK(formatAge(60, DurationStyle::Long, buf, 4) == 0 && buf[0] == '\0');
}

static void typedTime() {
  TypedTime t = { 0, false };
  // No word: device-local. The same text is another instant in another zone.
  CHECK(parseTypedTime("2026-11-22 14:30:05", 0, &t) == TypedTimeError::None && t.epoch == kNow && !t.wasUtc);
  CHECK(parseTypedTime("2026-11-22 15:30:05", 60, &t) == TypedTimeError::None && t.epoch == kNow && !t.wasUtc);
  CHECK(parseTypedTime("2026-11-22 09:30:05", -300, &t) == TypedTimeError::None && t.epoch == kNow);
  CHECK(parseTypedTime("2026-11-22 15:30:05 local", 60, &t) == TypedTimeError::None && t.epoch == kNow && !t.wasUtc);
  CHECK(parseTypedTime("  2026-11-22 15:30:05   LOCAL  ", 60, &t) == TypedTimeError::None && t.epoch == kNow);
  // utc: the offset is not applied.
  CHECK(parseTypedTime("2026-11-22 14:30:05 utc", 60, &t) == TypedTimeError::None && t.epoch == kNow && t.wasUtc);
  CHECK(parseTypedTime("2026-11-22 14:30:05 UTC", -300, &t) == TypedTimeError::None && t.epoch == kNow && t.wasUtc);
  // A Unix time is UTC whatever the offset.
  CHECK(parseTypedTime("1795357805", 60, &t) == TypedTimeError::None && t.epoch == kNow && t.wasUtc);
  CHECK(parseTypedTime("1795357805 utc", -300, &t) == TypedTimeError::None && t.epoch == kNow && t.wasUtc);
  CHECK(parseTypedTime("1795357805 local", 60, &t) == TypedTimeError::UnixNotLocal);
  // The local reading crosses a date line.
  CHECK(parseTypedTime("2027-01-01 00:30:00", 60, &t) == TypedTimeError::None &&
        t.epoch == epochFromCivilUtc(2026, 12, 31, 23, 30, 0));

  t = { 42, true };
  CHECK(parseTypedTime("", 0, &t) == TypedTimeError::Empty);
  CHECK(parseTypedTime("   ", 0, &t) == TypedTimeError::Empty);
  CHECK(parseTypedTime(nullptr, 0, &t) == TypedTimeError::Empty);
  CHECK(parseTypedTime("utc", 0, &t) == TypedTimeError::Format);
  CHECK(parseTypedTime(" local", 0, &t) == TypedTimeError::Format);
  for (const char* bad : { "2026-11-22", "14:30:05", "2026-11-22T14:30:05", "2026-11-22 14:30", "2026-11-22 14:30:05Z",
                           "2026-11-22 14:30:05 gmt", "2026-11-22 14:30:05utc", "2026/11/22 14:30:05",
                           "26-11-22 14:30:05", "2026-11-22 14:30:05 utc local", "-5", "12a", "1e9" }) {
    CHECK(parseTypedTime(bad, 0, &t) == TypedTimeError::Format);
  }
  for (const char* bad : { "2026-02-31 00:00:00", "2026-13-01 00:00:00", "2026-00-10 00:00:00", "2026-11-00 00:00:00",
                           "2026-11-22 24:00:00", "2026-11-22 14:60:00", "2026-11-22 14:30:60", "2025-02-29 12:00:00 utc" }) {
    CHECK(parseTypedTime(bad, 0, &t) == TypedTimeError::NotADate);
  }
  CHECK(parseTypedTime("2024-02-29 12:00:00 utc", 0, &t) == TypedTimeError::None);
  t = { 42, true };
  for (const char* bad : { "100", "1577836799", "4102444800", "99999999999", "999999999999999999999999",
                           "2019-12-31 23:59:59 utc", "2100-01-01 00:00:00 utc", "1969-01-01 00:00:00", "9999-01-01 00:00:00" }) {
    CHECK(parseTypedTime(bad, 0, &t) == TypedTimeError::Range);
  }
  CHECK(t.epoch == 42 && t.wasUtc);  // an error leaves the result alone
  CHECK(parseTypedTime("1577836800", 0, &t) == TypedTimeError::None && t.epoch == kEpoch2020);
  CHECK(parseTypedTime("4102444799", 0, &t) == TypedTimeError::None && t.epoch == kEpoch2100 - 1);
  // The edges of the range move with the offset for a local reading.
  CHECK(parseTypedTime("2020-01-01 00:30:00", 60, &t) == TypedTimeError::Range);   // 2019-12-31 23:30 UTC
  CHECK(parseTypedTime("2019-12-31 23:30:00", -60, &t) == TypedTimeError::None);  // 2020-01-01 00:30 UTC
  CHECK(parseTypedTime("2026-11-22 14:30:05", 0, nullptr) == TypedTimeError::None);

  CHECK(typedTimeErrorText(TypedTimeError::None) == nullptr);
  for (TypedTimeError e : { TypedTimeError::Empty, TypedTimeError::Format, TypedTimeError::NotADate,
                            TypedTimeError::UnixNotLocal, TypedTimeError::Range }) {
    CHECK(std::string(typedTimeErrorText(e)).rfind("Error: ", 0) == 0);
  }
}

static void timezones() {
  // The offsets the owner asked for are there.
  for (int wanted : { 765, 780, 840, -720, 0, 330, 570 }) CHECK(timezoneIndexForOffset(wanted) >= 0);
  CHECK(timezoneIndexForOffset(17) == -1);
  CHECK(timezoneIndexForOffset(900) == -1);

  std::set<std::string> abbrevs;
  int lowest = 0, highest = 0;
  for (size_t i = 0; i < kTimezoneCount; ++i) {
    const TimezoneEntry& e = kTimezones[i];
    CHECK(e.abbrev && e.abbrev[0] && e.place && e.place[0]);
    CHECK(abbrevs.insert(e.abbrev).second);           // no abbreviation twice
    CHECK(e.offsetMinutes % 15 == 0);                 // the glasses take quarter hours
    CHECK(std::string(e.abbrev).find_first_of(",|") == std::string::npos);
    CHECK(std::string(e.place).find_first_of(",|:") == std::string::npos);
    CHECK(timezoneIndexByAbbrev(e.abbrev) == (int)i);
    if (e.offsetMinutes < lowest) lowest = e.offsetMinutes;
    if (e.offsetMinutes > highest) highest = e.offsetMinutes;
  }
  CHECK(lowest == kTzOffsetMinMinutes && highest == kTzOffsetMaxMinutes);
  CHECK(timezoneIndexByAbbrev("est") == -1 && timezoneIndexByAbbrev(nullptr) == -1);

  // An offset re-opens on its standard-time entry: EST, not CDT.
  CHECK_TEXT(kTimezones[timezoneIndexForOffset(-300)].abbrev, "EST");
  CHECK_TEXT(kTimezones[timezoneIndexForOffset(-240)].abbrev, "AST");
  CHECK_TEXT(kTimezones[timezoneIndexForOffset(60)].abbrev, "CET");
  CHECK_TEXT(kTimezones[timezoneIndexForOffset(780)].abbrev, "TOT");
  CHECK_TEXT(kTimezones[timezoneIndexForOffset(0)].abbrev, "UTC");
  CHECK_TEXT(kTimezones[timezoneIndexForOffset(825)].abbrev, "CHADT");  // summer time only: the first entry
  for (size_t i = 0; i < kTimezoneCount; ++i) {
    const int back = timezoneIndexForOffset(kTimezones[i].offsetMinutes);
    CHECK(back >= 0 && kTimezones[back].offsetMinutes == kTimezones[i].offsetMinutes);
    if (!kTimezones[i].daylight) CHECK(!kTimezones[back].daylight);
  }

  // The settings list: one line per offset, west to east, every entry named.
  static char options[kTimezoneOptionsCap];
  const size_t length = timezoneOptionsText(options, sizeof(options));
  CHECK(length > 0 && length == std::strlen(options));
  CHECK(length < kTimezoneOptionsCap / 2 + 600);  // room to grow
  const std::string text(options);
  CHECK(text.rfind("-720|UTC-12:00 (AoE Baker Island)", 0) == 0);
  CHECK(text.find(",-300|UTC-5:00 (EST New York / CDT Chicago),") != std::string::npos);
  CHECK(text.find(",0|UTC+0:00 (UTC / GMT London/Dublin),") != std::string::npos);
  CHECK(text.find(",765|UTC+12:45 (CHAST Chatham Islands),") != std::string::npos);
  CHECK(text.find(",780|UTC+13:00 (TOT Tonga / NZDT Auckland),") != std::string::npos);
  CHECK(text.find(",840|UTC+14:00 (LINT Kiritimati)") + std::strlen(",840|UTC+14:00 (LINT Kiritimati)") == text.size());
  std::set<int> offsets, listed;
  for (size_t i = 0; i < kTimezoneCount; ++i) {
    offsets.insert(kTimezones[i].offsetMinutes);
    CHECK(text.find(std::string(kTimezones[i].abbrev) + (std::string(kTimezones[i].abbrev) == kTimezones[i].place
                                                              ? "" : std::string(" ") + kTimezones[i].place)) != std::string::npos);
  }
  int previous = -100000;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find(',', at);
    if (end == std::string::npos) end = text.size();
    const std::string item = text.substr(at, end - at);
    const size_t bar = item.find('|');
    CHECK(bar != std::string::npos && bar > 0 && bar + 1 < item.size());
    const int value = std::atoi(item.substr(0, bar).c_str());
    CHECK(value > previous);
    previous = value;
    listed.insert(value);
    at = end + 1;
  }
  CHECK(listed == offsets);
  CHECK(timezoneOptionsText(options, 64) == 0 && options[0] == '\0');
  CHECK(std::string(kTimezoneDstNote) == "no automatic daylight saving");
}

int main() {
  validity();
  rankTable();
  civil();
  formatters();
  durations();
  typedTime();
  timezones();
  std::printf("clock core: %u checks passed\n", checks);
  return 0;
}
