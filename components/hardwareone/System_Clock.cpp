#include "System_Clock.h"

#include <cstdio>
#include <cstdlib>     // setenv — applyTimezone()
#include <cstring>
#include <sys/time.h>  // gettimeofday — epochMillis()
#include <esp_timer.h> // esp_timer_get_time — the projection reference

#include "System_Settings.h"    // gSettings.tzOffsetMinutes, rtcTimeHasBeenSet
#include "System_BuildConfig.h" // ENABLE_RTC_SENSOR gate for the write-back duty
#include "System_Events.h"      // systemEventPost + SYSEVT_TIME_SYNCED (X-macro enum — cannot be forward-declared)
#include "System_Debug.h"       // logSystemEvent, BROADCAST_PRINTF
#include "System_Automation.h"  // notifyAutomationScheduler (inline no-op stub when automations disabled)

// Duty targets that always link (their TUs are unconditional):
extern bool filesystemReady;
extern void writeBootAnchor();                          // System_User.cpp — upserts by anchor id
extern void resolvePendingUserCreationTimes();          // System_User.cpp
extern void logTimeSyncedMarkerIfReady(const char*);    // System_Debug.cpp — one-shot forensic marker
#if ENABLE_RTC_SENSOR
extern bool gRtcRunning;
extern bool gRtcConnected;
extern bool rtcSyncFromSystem();
#endif

namespace Clock {

// "Year >= 2020" sanity check. The system clock starts at 1970 (boot epoch)
// or maybe RTC-backed at a stale time. Anything before 2020 is treated as
// "not really synced yet" — same convention the scattered call sites used,
// so behavior is preserved.
static constexpr int kSyncedTmYearThreshold = 120;  // 1900 + 120 = 2020

bool isSynced() {
  time_t now = time(nullptr);
  if (now <= 0) return false;
  struct tm tminfo;
  if (!localtime_r(&now, &tminfo)) return false;
  return tminfo.tm_year >= kSyncedTmYearThreshold;
}

bool isValidEpoch(time_t t) {
  if (t <= 0) return false;
  struct tm tminfo;
  if (!gmtime_r(&t, &tminfo)) return false;
  return tminfo.tm_year >= kSyncedTmYearThreshold;
}

int64_t epochMillis() {
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) != 0) return 0;
  return (int64_t)tv.tv_sec * 1000LL + (int64_t)(tv.tv_usec / 1000);
}

int tzOffsetMinutes() {
  return gSettings.tzOffsetMinutes;
}

const char* formatTzOffsetLabel(char* out, size_t outSize) {
  if (!out || outSize == 0) return "";
  const int tzMin = gSettings.tzOffsetMinutes;
  if (tzMin == 0) {
    snprintf(out, outSize, "UTC");
    return out;
  }
  const int absMin = (tzMin < 0) ? -tzMin : tzMin;
  snprintf(out, outSize, "UTC%c%d:%02d", (tzMin < 0) ? '-' : '+',
           absMin / 60, absMin % 60);
  return out;
}

int tzOffsetQuarterHours() {
  // Integer division — fractional 15-minute offsets (e.g., +5:30 India = 330
  // minutes = 22 quarter-hours exactly, +5:45 Nepal = 345 = 23.0) work out
  // cleanly for every real-world IANA timezone since they're all whole
  // 15-minute increments. If someone configures a weird non-15-multiple,
  // the truncation matches what G2 firmware expects.
  return gSettings.tzOffsetMinutes / 15;
}

void applyTimezone() {
  // POSIX TZ carries the INVERTED sign of the stored offset ("UTC5" means 5
  // hours WEST of UTC = UTC-5), hence the negation. The string is EQUIVALENT to
  // Arduino's setTimeZone() (esp32-hal-time.c) but not byte-identical — that
  // helper appends a DST field, so it emits "UTC5DST5" where this emits "UTC5".
  // Same effective offset (std == DST when daylightOffset is 0), which is why
  // the configTime() call inside setupNTP() re-applies rather than fights it.
  // No DST rule is emitted: this firmware's timezone list offers standard and
  // daylight variants as separate picks, so the offset is always absolute.
  const long offset = -(long)tzOffsetMinutes() * 60;
  // Sign must come from the full offset, not from the hours division:
  // offset/3600 truncates toward zero, so a sub-hour EAST offset (+30 min
  // stored -> offset=-1800) yields hours==0 and the '-' silently vanishes,
  // flipping it to 30 min WEST. No real-world zone is <1h from UTC, but a
  // hand-typed `tzoffsetminutes 30` must still round-trip correctly.
  const char* sign = (offset < 0) ? "-" : "";
  const long absOff = labs(offset);
  char tz[24];
  if (absOff % 3600) {
    snprintf(tz, sizeof(tz), "UTC%s%ld:%02ld:%02ld", sign, absOff / 3600,
             (absOff % 3600) / 60, absOff % 60);
  } else {
    snprintf(tz, sizeof(tz), "UTC%s%ld", sign, absOff / 3600);
  }
  setenv("TZ", tz, 1);
  tzset();
}

size_t formatISO8601Local(time_t t, char* out, size_t outSize) {
  if (!out || outSize == 0) return 0;
  if (!isValidEpoch(t)) { out[0] = '\0'; return 0; }
  struct tm tminfo;
  if (!localtime_r(&t, &tminfo)) { out[0] = '\0'; return 0; }
  // strftime returns 0 on buffer overflow — propagate as "" so callers
  // see a clean failure rather than truncated output.
  size_t n = strftime(out, outSize, "%Y-%m-%dT%H:%M:%S", &tminfo);
  if (n == 0) out[0] = '\0';
  return n;
}

size_t formatFilenameLocal(time_t t, char* out, size_t outSize) {
  if (!out || outSize == 0) return 0;
  if (!isValidEpoch(t)) { out[0] = '\0'; return 0; }
  struct tm tminfo;
  if (!localtime_r(&t, &tminfo)) { out[0] = '\0'; return 0; }
  size_t n = strftime(out, outSize, "%Y%m%d_%H%M%S", &tminfo);
  if (n == 0) out[0] = '\0';
  return n;
}

size_t formatHHMMLocal(time_t t, char* out, size_t outSize) {
  if (!out || outSize == 0) return 0;
  if (!isValidEpoch(t)) { out[0] = '\0'; return 0; }
  struct tm tminfo;
  if (!localtime_r(&t, &tminfo)) { out[0] = '\0'; return 0; }
  size_t n = strftime(out, outSize, "%H:%M", &tminfo);
  if (n == 0) out[0] = '\0';
  return n;
}

// ============================================================================
// Clock-step chokepoint (see the header charter)
// ============================================================================

// All cross-task state lives under one mux: the pend flags are written from
// whatever task stepped the clock and drained on the main loop, and the
// projection reference is a 64-bit value on a 32-bit core (two stores —
// tearable without the lock).
static portMUX_TYPE sClockMux = portMUX_INITIALIZER_UNLOCKED;

static SyncSource sSource   = SYNC_NONE;
static long       sStepDelta = 0;      // seconds moved by the last step
static int64_t    sProjRefUs = 0;      // epoch-us minus esp_timer at last step

// Pend flags — set by clockStepped, drained by clockDutiesTick/Flush.
static constexpr uint8_t PEND_MARKER        = 1u << 0;  // forensic "Time Synced via X" log lines
static constexpr uint8_t PEND_ANCHOR        = 1u << 1;  // writeBootAnchor (upsert)
static constexpr uint8_t PEND_RESOLVE       = 1u << 2;  // resolvePendingUserCreationTimes
static constexpr uint8_t PEND_STEPLOG       = 1u << 3;  // valid→valid step > 2 min diagnostic
static constexpr uint8_t PEND_RTC_WRITEBACK = 1u << 4;  // push new time into the DS3231
static uint8_t sPending = 0;

SyncSource syncSource() { return sSource; }

const char* syncSourceName(SyncSource s) {
  switch (s) {
    case SYNC_RTC:    return "rtc";
    case SYNC_NTP:    return "ntp";
    case SYNC_MANUAL: return "manual";
    case SYNC_RING:   return "ring";
    case SYNC_CM5:    return "cm5";
    default:          return "none";
  }
}

void refreshProjection() {
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) != 0) return;
  const int64_t ref = (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec -
                      (int64_t)esp_timer_get_time();
  portENTER_CRITICAL(&sClockMux);
  sProjRefUs = ref;
  portEXIT_CRITICAL(&sClockMux);
}

int64_t projectedEpochUs() {
  portENTER_CRITICAL(&sClockMux);
  const int64_t ref = sProjRefUs;
  portEXIT_CRITICAL(&sClockMux);
  if (ref == 0) return 0;
  return ref + (int64_t)esp_timer_get_time();
}

void clockStepped(SyncSource src, time_t before) {
  refreshProjection();

  const time_t now = time(nullptr);
  const bool beforeValid = isValidEpoch(before);
  const bool nowValid    = isSynced();
  const bool edge        = !beforeValid && nowValid;
  const long delta       = (long)(now - before);

  // Cheap, any-task-safe, and wanted on every step: the scheduler compares
  // two flags and re-arms itself.
  notifyAutomationScheduler();

  if (edge) {
    // First valid clock this boot — the bus event convention (one post, on
    // the invalid→valid transition only) previously lived in the NTP path
    // with a hand-rolled 2024 threshold; isValidEpoch (2020) is now the one
    // vocabulary for "was the clock real".
    char ts[24] = "";
    struct tm tmNow;
    if (localtime_r(&now, &tmNow)) {
      strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &tmNow);
    }
    systemEventPost(SYSEVT_TIME_SYNCED, syncSourceName(src), ts);
  }

  uint8_t pend = 0;
  if (edge) pend |= PEND_MARKER | PEND_ANCHOR | PEND_RESOLVE;
  // A big valid→valid correction deserves a diagnostic AND a re-anchor —
  // the old anchor's epoch was wrong by the same amount (upsert-safe).
  if (!edge && beforeValid && (delta > 120 || delta < -120)) {
    pend |= PEND_STEPLOG | PEND_ANCHOR | PEND_RESOLVE;
  }
  if (src != SYNC_RTC && nowValid) pend |= PEND_RTC_WRITEBACK;

  // Labels and pend bits publish TOGETHER, unconditionally: the drain
  // snapshots them under the same mux, so a step that lands mid-drain can
  // never relabel duties that belong to an earlier step. ORing pend==0 is
  // a harmless no-op, and the ledger must update on every step regardless.
  portENTER_CRITICAL(&sClockMux);
  sSource    = src;
  sStepDelta = delta;
  sPending  |= pend;
  portEXIT_CRITICAL(&sClockMux);
}

// Shared drain body. Read-and-clear under the mux so a flush from cmd_exec
// can't race the main-loop tick into running a duty twice.
static void drainDuties() {
  if (!sPending || !filesystemReady) return;
  portENTER_CRITICAL(&sClockMux);
  const uint8_t pend  = sPending;
  const SyncSource src = sSource;   // snapshot labels with the claim —
  const long delta     = sStepDelta;  // see the publish note in clockStepped
  sPending = 0;
  portEXIT_CRITICAL(&sClockMux);
  if (!pend) return;

  const char* srcName = syncSourceName(src);

  if (pend & PEND_STEPLOG) {
    logSystemEvent("TIME", "clock stepped %+lds by %s while already set — check drift",
                   delta, srcName);
  }
  if (pend & PEND_MARKER)  logTimeSyncedMarkerIfReady(srcName);
  if (pend & PEND_ANCHOR)  writeBootAnchor();
  if (pend & PEND_RESOLVE) resolvePendingUserCreationTimes();

#if ENABLE_RTC_SENSOR
  if (pend & PEND_RTC_WRITEBACK) {
    if (gRtcRunning && gRtcConnected && rtcSyncFromSystem()) {
      // Every future hourly SNTP correction re-pends this; a per-boot
      // first-time broadcast keeps the log readable.
      static bool sRtcWritebackAnnounced = false;
      if (!sRtcWritebackAnnounced) {
        sRtcWritebackAnnounced = true;
        BROADCAST_PRINTF("[OK] RTC updated from %s time", srcName);
      }
      if (!gSettings.rtcTimeHasBeenSet) {
        setSetting(gSettings.rtcTimeHasBeenSet, true);
      }
    }
  }
#endif
}

void clockDutiesTick()  { drainDuties(); }
void clockDutiesFlush() { drainDuties(); }

} // namespace Clock

// ---------------------------------------------------------------------------
// Clock settings module — ALWAYS registered.
//
// tzOffsetMinutes previously lived in wifiSettingsEntries[], inside
// System_WiFi.cpp's file-wide #if ENABLE_WIFI. Its CLI command
// ("tzoffsetminutes") and every consumer (Clock::tzOffsetMinutes,
// i2csensor_ds3231, G2_Ring, System_R1_Protocol, WebPage_Sensors) are ungated,
// so on a radioless build the user could set the timezone and have it silently
// revert on the next boot — the registry walk that persists settings had no row
// to write. Timezone belongs to the clock, not the radio.
//
// ntpServer moves with it so the web "System Time" pane stays a single
// SchemaPanel (one module per panel, one Save button), but it keeps its
// ENABLE_WIFI gate: NTP genuinely needs the radio, and sizeof/sizeof adapts the
// entry count when the row compiles out.
// ---------------------------------------------------------------------------

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry clockSettingsEntries[] = {
#if ENABLE_WIFI
  { "ntpServer",          SETTING_STRING, &gSettings.ntpServer,         0, 0, "pool.ntp.org", 0, 0, "NTP Server", nullptr, false, nullptr, "ntpserver" },
#endif
  // Timezone offsets as "minutes|label" pairs — the schema renderer's enum
  // widget reads this and produces a select dropdown. CLI verb stays "tz".
  { "tzOffsetMinutes",    SETTING_INT,    &gSettings.tzOffsetMinutes,   0, 0, nullptr, -720, 840, "Timezone",
    "-720|UTC-12 (Baker Island),"
    "-660|UTC-11 (Samoa),"
    "-600|UTC-10 (Hawaii/HST),"
    "-540|UTC-9 (Alaska/AKST),"
    "-480|UTC-8 (Pacific/PST),"
    "-420|UTC-7 (Mountain/MST · Pacific/PDT),"
    "-360|UTC-6 (Central/CST · Mountain/MDT),"
    "-300|UTC-5 (Eastern/EST · Central/CDT),"
    "-240|UTC-4 (Atlantic/AST · Eastern/EDT),"
    "-180|UTC-3 (Argentina · Atlantic/ADT),"
    "-120|UTC-2 (Mid-Atlantic),"
    "-60|UTC-1 (Azores),"
    "0|UTC+0 (London/GMT · Dublin),"
    "60|UTC+1 (Berlin/Paris/CET · London/BST),"
    "120|UTC+2 (Cairo/Athens/EET · Paris/CEST),"
    "180|UTC+3 (Moscow/Baghdad),"
    "240|UTC+4 (Dubai/Baku),"
    "300|UTC+5 (Karachi/Tashkent),"
    "330|UTC+5:30 (Mumbai/Delhi/IST),"
    "360|UTC+6 (Dhaka/Almaty),"
    "420|UTC+7 (Bangkok/Jakarta),"
    "480|UTC+8 (Beijing/Singapore),"
    "540|UTC+9 (Tokyo/Seoul/JST),"
    "570|UTC+9:30 (Adelaide/ACST),"
    "600|UTC+10 (Sydney/AEST),"
    "660|UTC+11 (Solomon Islands),"
    "720|UTC+12 (Fiji/Auckland/NZST)",
    false, nullptr, "tzoffsetminutes" }
};

extern const SettingsModule clockSettingsModule = {
  "clock",
  "system.time",
  clockSettingsEntries,
  sizeof(clockSettingsEntries) / sizeof(clockSettingsEntries[0]),
  nullptr,  // always available — no connection precondition
  "System time: timezone and NTP server"
};

