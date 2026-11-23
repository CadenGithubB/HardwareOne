#ifndef SYSTEM_CLOCK_H
#define SYSTEM_CLOCK_H

#include <Arduino.h>
#include <stdint.h>
#include <time.h>

#include "System_ClockCore.h"  // the rules themselves: validity, rank table, formatters, timezone table

// =============================================================================
// Clock — time / sync state / timezone / formatting accessors
// =============================================================================
//
// System_NTP owns network synchronization and hands replies to clockStepped().
// This module owns clock behavior shared by NTP, RTC, manual, and peer sources.
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

// Seconds since UNIX epoch. Wraps time(NULL). A clock that was never set
// counts up from 1970, so the value is small but not 0: ask isSynced() (or
// isValidEpoch of the value) before storing or showing it as a date.
inline time_t epochSeconds() { return time(nullptr); }

// ----- The validity vocabulary --------------------------------------------
//
// Three questions, and no other way to ask them (no `time(nullptr) > 0`, no
// year literal, no tm_year test):
//
//   isSynced()          is THIS device's clock set?  = isValidEpoch(now)
//   isValidEpoch(t)     is `t` a real time (2020 or later)? For the device's
//                       own clock and values it wrote itself.
//   isPlausibleEpoch(t) is `t` within [2020, 2100)? For a time from outside:
//                       a peer, a chip, a typed value, where garbage is as
//                       easily large as small.
//
// A time that fails the question is stored as 0 / JSON null, never as the
// 1970-based number.

// 2020-01-01 00:00:00 UTC, the lower bound of all three questions.
constexpr time_t kEpoch2020 = (time_t)clockcore::kEpoch2020;
// 2100-01-01 00:00:00 UTC, the upper bound of isPlausibleEpoch.
constexpr time_t kEpoch2100 = (time_t)clockcore::kEpoch2100;

bool isSynced();
bool isValidEpoch(time_t t);
inline bool isPlausibleEpoch(time_t t) { return clockcore::isPlausibleEpoch((int64_t)t); }

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

// What a stored or sent `<what>EpochMs` field holds for "now": the epoch in
// milliseconds when the clock is set, 0 when it is not (written as JSON null
// or left out; never the 1970-based number, never uptime).
int64_t epochMillisOrZero();

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
// configTime() inside setupNtp(), which runs only once WiFi is up. On a boot
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
//
// The one formatter family (System_ClockCore.h holds the rules and the host
// tests run them). Stored and sent text is UTC; shown text is device-local,
// 24 hour. "Local" is the configured fixed offset (tzOffsetMinutes), computed
// here without localtime_r, so these are safe from any task. Each returns the
// number of characters written (excluding NUL), and 0 with "" in `out` when
// `t` is not a valid epoch or the text does not fit: no cut strings.

// Stored / sent: "2026-11-22T14:30:05Z" (UTC, 20 characters).
size_t formatIso8601Utc(time_t t, char* out, size_t outSize);
// Reads exactly that shape back. False leaves *out untouched.
bool parseIso8601Utc(const char* text, time_t* out);
// Names and ids: "20261122T143005Z" (UTC, 16 characters).
size_t formatCompactUtc(time_t t, char* out, size_t outSize);

// Shown: "2026-11-22 14:30:05" (or "... 14:30" without seconds), device-local;
// with `tzLabel` the offset follows: "2026-11-22 14:30:05 UTC+1:00". Put the
// label on a full date and time that stands alone (a command reply, a status
// line); leave it off inside a list whose heading names the zone.
size_t formatDateTimeLocal(time_t t, char* out, size_t outSize, bool withSeconds = true, bool tzLabel = false);
// "2026-11-22": the local date. Daily files are named by it and roll at
// local midnight.
size_t formatDateLocal(time_t t, char* out, size_t outSize);
// "14:30": a clock face, device-local.
size_t formatHHMMLocal(time_t t, char* out, size_t outSize);
// File names: "20261122_143005", device-local, filename-safe.
size_t formatFileStampLocal(time_t t, char* out, size_t outSize);
// The prefix of a stored log line, device-local WITH the UTC offset:
// "[2026-11-22 14:30:05.123 +01:00] | ". `epochMs` is Clock::epochMillis().
size_t formatLogPrefixLocal(int64_t epochMs, char* out, size_t outSize);

// "2026-05-23T15:04:05": local, T-separated, no zone. Kept for the callers
// that print it beside formatTzOffsetLabel; new code uses formatDateTimeLocal.
size_t formatIso8601Local(time_t t, char* out, size_t outSize);

// Lengths of time and ages, one wording for the whole firmware:
//   formatDuration(185, DurationStyle::Compact) -> "3m 5s"
//   formatDuration(185, DurationStyle::Long)    -> "3 minutes 5 seconds"
//   formatAge(185, DurationStyle::Compact)      -> "3m ago"   ("just now" under 5 s)
using DurationStyle = clockcore::DurationStyle;
inline size_t formatDuration(uint64_t seconds, DurationStyle style, char* out, size_t outSize) {
  return clockcore::formatDuration(seconds, style, out, outSize);
}
inline size_t formatAge(uint64_t seconds, DurationStyle style, char* out, size_t outSize) {
  return clockcore::formatAge(seconds, style, out, outSize);
}

// ----- The timezone table --------------------------------------------------
//
// One table (clockcore::kTimezones) feeds the setup wizard and the settings
// list. The device keeps a fixed offset: there is no automatic daylight
// saving (clockcore::kTimezoneDstNote), a zone's summer time is its own entry.

// The settings list built from the table: "minutes|label" pairs, one per
// offset. Built once into PSRAM; "" when that allocation fails.
const char* timezoneOptionsText();

// ----- Clock-step chokepoint ----------------------------------------------
//
// Seven things can set this system's clock: NTP, the DS3231 RTC, `timeset`
// and `rtcset`, the R1 ring (dark-boot adoption), the CM5 carrier's
// battery-backed RTC (SYNC_CM5, pushed over the UART host link), the ESP-NOW
// mesh master (SYNC_MESH / SYNC_MESH_NTP, taken only from the master named on
// the device; System_ESPNow_TimeCore.h), and lwIP's background SNTP daemon.
//
// Clock::step() is the one way to set the clock: it checks that the time is
// plausible, asks the rank table whether this source may replace the one that
// set the clock, calls settimeofday() and runs clockStepped(). No other code
// calls settimeofday(). The one exception is lwIP's SNTP client, which sets
// the clock inside lwIP; System_NTP reports that step through clockStepped()
// from its main-loop drain.
//
// Each step owes the same follow-up chores — post the TIME_SYNCED event on the
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

// SYNC_MESH: the mesh master's time, when the master's own clock is not from
// NTP. SYNC_MESH_NTP: the same, when it is (the TIME_SYNC frame's source says
// ntp). The numbers are stored and sent (power events, sensor arrival
// records): new sources go at the end.
enum SyncSource : uint8_t { SYNC_NONE = 0, SYNC_RTC, SYNC_NTP, SYNC_MANUAL, SYNC_RING, SYNC_CM5, SYNC_MESH, SYNC_MESH_NTP };

static_assert((int)SYNC_NONE == (int)clockcore::Source::None && (int)SYNC_RTC == (int)clockcore::Source::Rtc &&
              (int)SYNC_NTP == (int)clockcore::Source::Ntp && (int)SYNC_MANUAL == (int)clockcore::Source::Manual &&
              (int)SYNC_RING == (int)clockcore::Source::Ring && (int)SYNC_CM5 == (int)clockcore::Source::Cm5 &&
              (int)SYNC_MESH == (int)clockcore::Source::Mesh && (int)SYNC_MESH_NTP == (int)clockcore::Source::MeshNtp,
              "Clock::SyncSource mirrors clockcore::Source");

// The source the clock's time is from: the last one that set it THIS boot.
// SYNC_NONE with isSynced() true means the time was carried across a soft
// reboot in the RTC domain. SYNC_NTP from the moment lwIP's SNTP client has
// set the clock, before the main loop has reported that step (sntpStepPending).
SyncSource syncSource();

// What set the clock before mesh time took it over (SYNC_NONE when the clock
// was not set then, or was only carried over a restart, and whenever the
// clock is not on mesh time). The master's time is followed down to this
// source and no lower: see clockcore::sourceMayCorrect.
SyncSource meshFloorSource();
const char* syncSourceName(SyncSource s);  // "none|rtc|ntp|manual|ring|cm5|mesh|mesh-ntp"

// True for a source whose time may be written into the RTC chip. Not the
// chip itself, not mesh time (after the next boot it would read back as
// "rtc": as a master the device would pass the time on as a source of its
// own), and not a clock carried over a soft restart (SYNC_NONE while set: it
// may have come from the mesh).
constexpr bool sourceWritesRtc(SyncSource s) { return s != SYNC_NONE && s != SYNC_RTC && s != SYNC_MESH && s != SYNC_MESH_NTP; }

// ----- Setting the clock ---------------------------------------------------
//
// The rank table (clockcore::sourceRank, owner decision 2026-11-22),
// strongest first:
//
//   ntp > manual > mesh-ntp > cm5 > rtc > ring > mesh > (carried over a restart)
//
//   - A clock that is not set takes any source.
//   - A clock that is set takes a source that ranks equal or higher than the
//     one that set it; a lower one is refused (RefusedOutranked).
//   - An operator command (SYNC_MANUAL: timeset, rtcset) always sets the
//     clock; NTP replaces that time at its next reply.
//   - Mesh time corrects mesh time, with or without NTP behind it, as long as
//     it ranks no lower than what the device had before mesh time took over
//     (meshFloorSource): plain mesh time does not replace an NTP-backed
//     master's time on a device whose own RTC or CM5 set the clock first.
//   - The time must be within 2020-2099, whoever offers it.
enum class StepResult : uint8_t {
  Stepped = 0,
  RefusedImplausible,  // not within 2020-2099
  RefusedOutranked,    // the clock is set from a source that ranks higher
  RefusedNoSource,     // SYNC_NONE sets nothing
  Failed,              // settimeofday() failed
};

// Sets the clock to `epoch` (+ `usec`) from `src` when the rank table allows
// it, then runs clockStepped(). The verdict, settimeofday() and the ledger
// are one unit under one mutex: two sources on two tasks cannot both pass the
// verdict against the same earlier state. Any normal task (the callers that
// own filesystem chores drain them: see clockDutiesTick / clockDutiesFlush);
// not from lwIP or BLE callbacks, not from an ISR.
StepResult step(SyncSource src, time_t epoch, uint32_t usec = 0);

// lwIP's SNTP client sets the clock itself, on its own task. Its two halves:
//
//   sntpStepPending()   from the SNTP callback, right after lwIP set the
//                       clock: from here on the clock's time is NTP's
//                       (syncSource() says so, and step() ranks a candidate
//                       against NTP), although the chores have not run.
//   sntpStepReport()    from the main-loop drain (ntpSyncDrainTick, the first
//                       clock tick of a lap): the chores of that step, with
//                       the clock from before it.
//
// Without the first half a source that came between the two (the CM5 tick
// earlier in the same lap, a timeset on the command task) was ranked against
// the stale ledger, overwrote the fresh NTP time and was then relabelled
// "ntp" by the report. When an operator command did replace the NTP time
// between the two halves, the report runs the chores under that command's
// name: the clock shows its time.
void sntpStepPending();
void sntpStepReport(time_t before);

// The same answer without setting anything.
StepResult stepVerdict(SyncSource src, time_t epoch);

// Why a step was refused, as the text after "Error: " in a reply, e.g.
// "the clock is set from ntp, which outranks rtc". "" for Stepped.
const char* stepRefusalText(StepResult result, SyncSource src, char* out, size_t outSize);

// Counts the clock steps of this boot (every clockStepped call). A reader
// that decided something from the clock's state compares it before acting:
// a changed number means another source set the clock meanwhile.
uint32_t stepSerial();

// The last step of this boot: which source, and how many seconds ago.
// False when nothing has set the clock this boot.
bool lastStep(SyncSource* src, uint32_t* agoSeconds);

// The chores after a step, passing the pre-step epoch. Clock::step() and
// sntpStepReport() call it under the step mutex; nothing else does.
// Inline-cheap; safe from any normal task. NOT for lwIP/BLE callbacks.
void clockStepped(SyncSource src, time_t before);

// Main-loop drain for the filesystem-touching chores clockStepped() pends.
// Cheap no-op when nothing is pending or the filesystem isn't up yet.
void clockDutiesTick();

// Synchronous drain of the same chores — big-stack callers only (cmd_exec
// task / loopTask), for commands that want their side effects visible
// before returning.
void clockDutiesFlush();

// ----- Confirmed clock (what the R1 ring is given) -------------------------
//
// SNTP replies are not authenticated, and the ring files its daily pages
// under the clock it is given. confirmedNow() is time(nullptr) unless an SNTP
// reply moved a clock that was already set this boot by more than a day and
// nothing has confirmed the new time since; then it is the earlier clock,
// carried forward by the monotonic timer. `timeset`, the RTC, the CM5, an
// SNTP reply that agrees with the earlier clock, or a restart end the hold.
// Rules and limits: System_ClockConfirmPolicy.h. Any task; not from an ISR.
time_t confirmedNow();
bool confirmedClockHeld();

// Projection of "now" in epoch-us from the last step's reference point.
// Sole intended consumer: the SNTP sync callback, which needs the PRE-step
// clock after lwIP has already stepped the real one. 0 = never seeded.
int64_t projectedEpochUs();
void refreshProjection();

} // namespace Clock

#endif // SYSTEM_CLOCK_H
