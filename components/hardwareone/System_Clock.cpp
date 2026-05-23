#include "System_Clock.h"

#include <cstdio>
#include <cstring>

#include "System_Settings.h"  // gSettings.tzOffsetMinutes

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

int tzOffsetMinutes() {
  return gSettings.tzOffsetMinutes;
}

int tzOffsetQuarterHours() {
  // Integer division — fractional 15-minute offsets (e.g., +5:30 India = 330
  // minutes = 22 quarter-hours exactly, +5:45 Nepal = 345 = 23.0) work out
  // cleanly for every real-world IANA timezone since they're all whole
  // 15-minute increments. If someone configures a weird non-15-multiple,
  // the truncation matches what G2 firmware expects.
  return gSettings.tzOffsetMinutes / 15;
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

} // namespace Clock
