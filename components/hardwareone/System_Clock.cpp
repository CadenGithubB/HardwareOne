#include "System_Clock.h"

#include <cstdio>
#include <cstdlib>     // setenv — applyTimezone()
#include <cstring>
#include <sys/time.h>  // gettimeofday — epochMillis()
#include <esp_timer.h> // esp_timer_get_time — the projection reference
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>  // the step mutex

#include "System_ClockConfirmPolicy.h" // which clock steps the ring is given
#include "System_Settings.h"    // gSettings.tzOffsetMinutes, rtcTimeHasBeenSet
#include "System_BuildConfig.h" // ENABLE_RTC_SENSOR gate for the write-back duty
#include "System_Events.h"      // systemEventPost + SYSEVT_TIME_SYNCED (X-macro enum — cannot be forward-declared)
#include "System_Debug.h"       // logSystemEvent, BROADCAST_PRINTF
#include "System_Automation.h"  // notifyAutomationScheduler (inline no-op stub when automations disabled)
#include "System_QuickNote.h"   // quickNoteClockSet (inline no-op stub without QuickNote)
#include "System_Filesystem.h"
#include "System_Logging.h"
#include "System_MemUtil.h"     // ps_alloc: the timezone list
#include "System_User.h"
#include "i2csensor_ds3231.h"

namespace Clock {

// The validity vocabulary (header). A clock that was never set counts up
// from 1970; anything before 2020-01-01 UTC is "not set". Both questions are
// the same UTC comparison (isSynced used to ask in local time, so the edge
// moved with the timezone).
bool isSynced() {
  return isValidEpoch(time(nullptr));
}

bool isValidEpoch(time_t t) {
  return clockcore::isValidEpoch((int64_t)t);
}

int64_t epochMillis() {
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) != 0) return 0;
  return (int64_t)tv.tv_sec * 1000LL + (int64_t)(tv.tv_usec / 1000);
}

int64_t epochMillisOrZero() {
  return isSynced() ? epochMillis() : 0;
}

int tzOffsetMinutes() {
  return gSettings.tzOffsetMinutes;
}

const char* formatTzOffsetLabel(char* out, size_t outSize) {
  if (!out || outSize == 0) return "";
  clockcore::formatTzOffsetLabel(gSettings.tzOffsetMinutes, out, outSize);
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
  // the configTime() call inside setupNtp() re-applies rather than fights it.
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

size_t formatIso8601Local(time_t t, char* out, size_t outSize) {
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

// The formatter family: the rules are clockcore's, the offset is the
// configured one. No localtime_r: the fields come from the epoch and the
// offset, which is what the fixed-offset TZ string makes localtime_r do.
size_t formatIso8601Utc(time_t t, char* out, size_t outSize) {
  return clockcore::formatIso8601Utc((int64_t)t, out, outSize);
}

bool parseIso8601Utc(const char* text, time_t* out) {
  int64_t epoch = 0;
  if (!clockcore::parseIso8601Utc(text, &epoch)) return false;
  if (out) *out = (time_t)epoch;
  return true;
}

size_t formatCompactUtc(time_t t, char* out, size_t outSize) {
  return clockcore::formatCompactUtc((int64_t)t, out, outSize);
}

size_t formatDateTimeLocal(time_t t, char* out, size_t outSize, bool withSeconds, bool tzLabel) {
  return clockcore::formatDateTimeLocal((int64_t)t, tzOffsetMinutes(), withSeconds, tzLabel, out, outSize);
}

size_t formatDateLocal(time_t t, char* out, size_t outSize) {
  return clockcore::formatDateLocal((int64_t)t, tzOffsetMinutes(), out, outSize);
}

size_t formatHHMMLocal(time_t t, char* out, size_t outSize) {
  return clockcore::formatHHMMLocal((int64_t)t, tzOffsetMinutes(), out, outSize);
}

size_t formatFileStampLocal(time_t t, char* out, size_t outSize) {
  return clockcore::formatFileStampLocal((int64_t)t, tzOffsetMinutes(), out, outSize);
}

size_t formatLogPrefixLocal(int64_t epochMs, char* out, size_t outSize) {
  return clockcore::formatLogPrefixLocal(epochMs, tzOffsetMinutes(), out, outSize);
}

// The settings list of timezones, from the one table. Built once; the buffer
// lives for the rest of the boot (a setting's options are read at any time).
const char* timezoneOptionsText() {
  static char* sOptions = nullptr;
  static portMUX_TYPE sOptionsLock = portMUX_INITIALIZER_UNLOCKED;
  portENTER_CRITICAL(&sOptionsLock);
  const char* have = sOptions;
  portEXIT_CRITICAL(&sOptionsLock);
  if (have) return have;

  char* built = (char*)ps_alloc(clockcore::kTimezoneOptionsCap, AllocPref::PreferPsram, "clock.tz.options");
  if (!built) return "";
  if (clockcore::timezoneOptionsText(built, clockcore::kTimezoneOptionsCap) == 0) {
    ps_free(built);
    return "";
  }
  portENTER_CRITICAL(&sOptionsLock);
  if (!sOptions) { sOptions = built; built = nullptr; }  // another task built it meanwhile: keep the first
  have = sOptions;
  portEXIT_CRITICAL(&sOptionsLock);
  if (built) ps_free(built);
  return have;
}

// ============================================================================
// Clock-step chokepoint (see the header charter)
// ============================================================================

// All cross-task state lives under one mux: the pend flags are written from
// whatever task stepped the clock and drained on the main loop, and the
// projection reference is a 64-bit value on a 32-bit core (two stores —
// tearable without the lock).
static portMUX_TYPE sClockMux = portMUX_INITIALIZER_UNLOCKED;

// Which source the clock's time is from (clockcore::Ledger has the rules):
// the last source that set it, the mesh floor, and an SNTP step that lwIP
// has made and the main loop has not reported yet. Under sClockMux.
static clockcore::Ledger sLedger;
// The last step's label for the duties drain (they belong to that step).
static SyncSource sSource   = SYNC_NONE;
static long       sStepDelta = 0;      // seconds moved by the last step
static uint32_t   sStepSerial = 0;     // steps this boot (stepSerial)
static uint32_t   sLastStepMs = 0;     // millis() of the last step
static int64_t    sProjRefUs = 0;      // epoch-us minus esp_timer at last step

// Pend flags — set by clockStepped, drained by clockDutiesTick/Flush.
static constexpr uint8_t kPendMarker        = 1u << 0;  // forensic "Time Synced via X" log lines
static constexpr uint8_t kPendAnchor        = 1u << 1;  // writeBootAnchor (upsert)
static constexpr uint8_t kPendResolve       = 1u << 2;  // resolvePendingUserCreationTimes
static constexpr uint8_t kPendSteplog       = 1u << 3;  // valid→valid step > 2 min diagnostic
static constexpr uint8_t kPendRtcWriteback = 1u << 4;  // push new time into the DS3231
static constexpr uint8_t kPendHoldStart    = 1u << 5;  // an SNTP jump was kept from the ring
static constexpr uint8_t kPendHoldEnd      = 1u << 6;  // ... and a later step settled it
static uint8_t sPending = 0;

// The reference for the confirmed clock (System_ClockConfirmPolicy.h).
static ClockConfirm::State sConfirm;

static bool wallAndMonotonicUs(int64_t& wallUs, int64_t& monoUs) {
  struct timeval tv;
  if (gettimeofday(&tv, nullptr) != 0) return false;
  monoUs = (int64_t)esp_timer_get_time();
  wallUs = (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
  return true;
}

static ClockConfirm::State confirmState() {
  portENTER_CRITICAL(&sClockMux);
  const ClockConfirm::State state = sConfirm;
  portEXIT_CRITICAL(&sClockMux);
  return state;
}

time_t confirmedNow() {
  int64_t wallUs = 0, monoUs = 0;
  if (!wallAndMonotonicUs(wallUs, monoUs)) return time(nullptr);
  return (time_t)(ClockConfirm::nowUs(confirmState(), wallUs, monoUs) / 1000000LL);
}

bool confirmedClockHeld() {
  int64_t wallUs = 0, monoUs = 0;
  return wallAndMonotonicUs(wallUs, monoUs) && ClockConfirm::disagrees(confirmState(), wallUs, monoUs);
}

SyncSource syncSource() {
  portENTER_CRITICAL(&sClockMux);
  const SyncSource source = (SyncSource)clockcore::ledgerCurrent(sLedger);
  portEXIT_CRITICAL(&sClockMux);
  return source;
}

SyncSource meshFloorSource() {
  portENTER_CRITICAL(&sClockMux);
  const SyncSource floor = (SyncSource)sLedger.floor;
  portEXIT_CRITICAL(&sClockMux);
  return floor;
}

uint32_t stepSerial() {
  portENTER_CRITICAL(&sClockMux);
  const uint32_t serial = sStepSerial;
  portEXIT_CRITICAL(&sClockMux);
  return serial;
}

bool lastStep(SyncSource* src, uint32_t* agoSeconds) {
  portENTER_CRITICAL(&sClockMux);
  const uint32_t serial = sStepSerial;
  const SyncSource source = sSource;
  const uint32_t atMs = sLastStepMs;
  portEXIT_CRITICAL(&sClockMux);
  if (serial == 0) return false;
  if (src) *src = source;
  if (agoSeconds) *agoSeconds = ageSeconds(atMs);
  return true;
}

const char* syncSourceName(SyncSource s) {
  switch (s) {
    case SYNC_RTC:    return "rtc";
    case SYNC_NTP:    return "ntp";
    case SYNC_MANUAL: return "manual";
    case SYNC_RING:   return "ring";
    case SYNC_CM5:    return "cm5";
    case SYNC_MESH:   return "mesh";
    case SYNC_MESH_NTP: return "mesh-ntp";
    default:          return "none";
  }
}

// ============================================================================
// Clock::step: the one way to set the clock (see the header)
// ============================================================================

static StepResult stepResultFor(clockcore::StepVerdict verdict) {
  switch (verdict) {
    case clockcore::StepVerdict::Allowed:            return StepResult::Stepped;
    case clockcore::StepVerdict::RefusedImplausible: return StepResult::RefusedImplausible;
    case clockcore::StepVerdict::RefusedOutranked:   return StepResult::RefusedOutranked;
    case clockcore::StepVerdict::RefusedNoSource:    return StepResult::RefusedNoSource;
  }
  return StepResult::RefusedNoSource;
}

// One mutex around "verdict, settimeofday, ledger" (step) and around the
// report of an SNTP step: a FreeRTOS mutex, since settimeofday() must not run
// inside a critical section. Made at first use; without one (out of memory)
// the steps run unserialised, as they did before.
static SemaphoreHandle_t stepMutex() {
  static SemaphoreHandle_t sMutex = nullptr;
  portENTER_CRITICAL(&sClockMux);
  SemaphoreHandle_t have = sMutex;
  portEXIT_CRITICAL(&sClockMux);
  if (have) return have;
  SemaphoreHandle_t made = xSemaphoreCreateMutex();
  if (!made) return nullptr;
  portENTER_CRITICAL(&sClockMux);
  if (!sMutex) { sMutex = made; made = nullptr; }  // another task made it meanwhile: keep the first
  have = sMutex;
  portEXIT_CRITICAL(&sClockMux);
  if (made) vSemaphoreDelete(made);
  return have;
}

struct StepLock {
  SemaphoreHandle_t mutex;
  StepLock() : mutex(stepMutex()) { if (mutex) xSemaphoreTake(mutex, portMAX_DELAY); }
  ~StepLock() { if (mutex) xSemaphoreGive(mutex); }
  StepLock(const StepLock&) = delete;
  StepLock& operator=(const StepLock&) = delete;
};

// The verdict against the clock as it is now: the source its time is from
// (NTP while an SNTP step waits for its report) and the mesh floor.
static StepResult verdictNow(SyncSource src, time_t epoch, time_t now) {
  portENTER_CRITICAL(&sClockMux);
  const clockcore::Ledger ledger = sLedger;
  portEXIT_CRITICAL(&sClockMux);
  return stepResultFor(clockcore::ledgerVerdict(ledger, (clockcore::Source)src, (int64_t)epoch, isValidEpoch(now)));
}

StepResult stepVerdict(SyncSource src, time_t epoch) {
  return verdictNow(src, epoch, time(nullptr));
}

StepResult step(SyncSource src, time_t epoch, uint32_t usec) {
  StepLock lock;
  const time_t before = time(nullptr);
  const StepResult verdict = verdictNow(src, epoch, before);
  if (verdict != StepResult::Stepped) return verdict;
  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = (suseconds_t)(usec < 1000000u ? usec : 999999u);
  if (settimeofday(&tv, nullptr) != 0) return StepResult::Failed;
  clockStepped(src, before);
  // An SNTP step that still waits for its report has been replaced (only an
  // operator command gets here over NTP): the report must not relabel this
  // time as NTP's.
  portENTER_CRITICAL(&sClockMux);
  clockcore::ledgerAfterOwnStep(sLedger);
  portEXIT_CRITICAL(&sClockMux);
  return StepResult::Stepped;
}

void sntpStepPending() {
  portENTER_CRITICAL(&sClockMux);
  clockcore::ledgerOnSntpPending(sLedger);  // lwIP has just set the clock: it shows NTP's time
  portEXIT_CRITICAL(&sClockMux);
}

void sntpStepReport(time_t before) {
  StepLock lock;
  portENTER_CRITICAL(&sClockMux);
  const SyncSource name = (SyncSource)clockcore::ledgerOnSntpReport(sLedger);
  // The ledger names the step in the same breath as it stops being pending
  // (clockStepped repeats this with the same result).
  clockcore::ledgerOnStep(sLedger, (clockcore::Source)name, isValidEpoch(before));
  portEXIT_CRITICAL(&sClockMux);
  // Not NTP when an operator command set the clock after lwIP did: the clock
  // shows that command's time. The chores of the step from `before` are
  // still owed (this may be the first valid clock of the boot); they run
  // under that name.
  clockStepped(name, before);
}

const char* stepRefusalText(StepResult result, SyncSource src, char* out, size_t outSize) {
  if (!out || outSize == 0) return "";
  out[0] = '\0';
  switch (result) {
    case StepResult::Stepped:
      break;
    case StepResult::RefusedImplausible:
      snprintf(out, outSize, "the %s time is not within the years 2020 to 2099", syncSourceName(src));
      break;
    case StepResult::RefusedOutranked:
      snprintf(out, outSize, "the clock is set from %s, which outranks %s; timeset sets it by hand",
               syncSourceName(syncSource()), syncSourceName(src));
      break;
    case StepResult::RefusedNoSource:
      snprintf(out, outSize, "no time source was named");
      break;
    case StepResult::Failed:
      snprintf(out, outSize, "the system refused the new time");
      break;
  }
  return out;
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
  if (edge) pend |= kPendMarker | kPendAnchor | kPendResolve;

  // The ring's clock: an SNTP step far from a clock already set this boot is
  // not passed on (System_ClockConfirmPolicy.h). A clock adopted from the ring is
  // no reference for a later step, or the ring would vouch for itself. Nor is
  // one taken from the mesh master: a weaker source must not make a later NTP
  // reply look doubtful.
  int64_t wallUs = 0, monoUs = 0;
  if (wallAndMonotonicUs(wallUs, monoUs)) {
    const bool reference = nowValid && src != SYNC_RING && src != SYNC_MESH && src != SYNC_MESH_NTP && src != SYNC_NONE;
    portENTER_CRITICAL(&sClockMux);
    const ClockConfirm::Verdict verdict =
        ClockConfirm::onStep(sConfirm, src == SYNC_NTP, reference, wallUs, monoUs);
    portEXIT_CRITICAL(&sClockMux);
    if (verdict == ClockConfirm::Verdict::HoldStarted) pend |= kPendHoldStart;
    if (verdict == ClockConfirm::Verdict::HoldReleased) pend |= kPendHoldEnd;
  }
  // A big valid→valid correction deserves a diagnostic AND a re-anchor —
  // the old anchor's epoch was wrong by the same amount (upsert-safe).
  if (!edge && beforeValid && (delta > 120 || delta < -120)) {
    pend |= kPendSteplog | kPendAnchor | kPendResolve;
  }
  // Mesh time is not written into the RTC chip (sourceWritesRtc says why).
  if (sourceWritesRtc(src) && nowValid) pend |= kPendRtcWriteback;

  // Labels and pend bits publish TOGETHER, unconditionally: the drain
  // snapshots them under the same mux, so a step that lands mid-drain can
  // never relabel duties that belong to an earlier step. ORing pend==0 is
  // a harmless no-op, and the ledger must update on every step regardless.
  portENTER_CRITICAL(&sClockMux);
  clockcore::ledgerOnStep(sLedger, (clockcore::Source)src, beforeValid);
  sSource    = src;
  sStepDelta = delta;
  sPending  |= pend;
  sStepSerial++;
  sLastStepMs = (uint32_t)millis();
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

  if (pend & kPendSteplog) {
    logSystemEvent("TIME", "clock stepped %+lds by %s while already set — check drift",
                   delta, srcName);
  }
  if (pend & kPendHoldStart) {
    logSystemEvent("TIME", "ntp moved the clock by more than a day (%+lds); the ring keeps the earlier "
                           "clock until timeset, a local time source, an agreeing ntp reply or a restart",
                   delta);
  }
  if (pend & kPendHoldEnd) {
    logSystemEvent("TIME", "the ring follows the device clock again (settled by %s)", srcName);
  }
  if (pend & kPendMarker)  logTimeSyncedMarkerIfReady(srcName);
  if (pend & kPendAnchor)  writeBootAnchor();
  if (pend & kPendResolve) {
    resolvePendingUserCreationTimes();
    quickNoteClockSet();  // notes saved as "boot" this boot get their real time
  }

#if ENABLE_RTC_SENSOR
  if (pend & kPendRtcWriteback) {
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
// tzOffsetMinutes previously lived in kWifiSettingsEntries[], inside
// System_WiFi.cpp's file-wide #if ENABLE_WIFI. Its CLI command
// ("tzoffsetminutes") and every consumer (Clock::tzOffsetMinutes,
// i2csensor_ds3231, G2_Ring, R1_Protocol, WebPage_Sensors) are ungated,
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
static const SettingEntry kClockSettingsEntries[] = {
#if ENABLE_WIFI
  { "ntpServer",          SETTING_STRING, &gSettings.ntpServer,         0, 0, "pool.ntp.org", 0, 0, "NTP Server", nullptr, false, nullptr, "ntpserver" },
#endif
  // The timezone list is built from the one table (clockcore::kTimezones,
  // Clock::timezoneOptionsText): "minutes|label" pairs, which the schema
  // renderer's enum widget shows as a select. The offset is fixed: the label
  // says that nothing changes it in spring and autumn.
  { "tzOffsetMinutes",    SETTING_INT,    &gSettings.tzOffsetMinutes,   0, 0, nullptr,
    clockcore::kTzOffsetMinMinutes, clockcore::kTzOffsetMaxMinutes, "Timezone (no automatic daylight saving)",
    nullptr, false, nullptr, "tzoffsetminutes", false, false, nullptr, Clock::timezoneOptionsText }
};

extern const SettingsModule clockSettingsModule = {
  "clock",
  "system.time",
  kClockSettingsEntries,
  sizeof(kClockSettingsEntries) / sizeof(kClockSettingsEntries[0]),
  nullptr,  // always available — no connection precondition
  "System time: timezone and NTP server"
};
