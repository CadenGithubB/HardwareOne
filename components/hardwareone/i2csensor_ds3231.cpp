// i2csensor_ds3231.cpp - DS3231 Precision RTC sensor driver
// Minimal IRAM footprint - uses standard I2C transactions

#include "i2csensor_ds3231.h"
#include "System_BuildConfig.h"

#if ENABLE_RTC_SENSOR

#include <Arduino.h>
#include <freertos/semphr.h>
#include <Wire.h>
#include <time.h>
#include <sys/time.h>

#include "OLED_Display.h"
#include "System_Command.h"
#include "System_Clock.h"
#include "System_Debug.h"
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_I2C.h"
#include "System_MemoryMonitor.h"
#include "System_MemUtil.h"  // ps_alloc: the refusal text of a clock step
#include "System_Mutex.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_Utils.h"
#include "System_CommandRegistry.h"

// RTC Cache
// The mutex is retained across stops; readers never own the live storage.
static RtcCache sRtcCache{};
static SemaphoreHandle_t sRtcCacheMutex = nullptr;

bool rtcCacheSnapshot(RtcCache& out, uint32_t timeoutMs) {
  out = {};
  SensorCacheGuard guard(sRtcCacheMutex, pdMS_TO_TICKS(timeoutMs), "rtc.snapshot");
  if (!guard.held) return false;
  out = sRtcCache;
  return true;
}

static bool rtcInvalidateCache() {
  if (!sRtcCacheMutex) return true;  // No reading can exist before initialization.
  SensorCacheGuard guard(sRtcCacheMutex, pdMS_TO_TICKS(50), "rtc.invalidate");
  if (!guard.held) return false;
  sRtcCache.dataValid = false;
  return true;
}

// Resolve which I2C bus + TwoWire* the DS3231 is on, from settings. Returns
// false (with outputs unchanged) if the requested bus isn't initialized —
// e.g., user set rtcBus=1 but i2c2Enabled=false. Each I2C function below
// calls this and bails on failure rather than silently using Wire1, so a
// misconfigured rtcBus surfaces as "RTC not connected" instead of writing
// to the wrong bus. Reading gSettings.rtcBus on every call is fine — it's a
// single load, no atomicity concerns (reboot-required = no runtime updates).
static bool rtcResolveBus(uint8_t* outBus, TwoWire** outWire) {
  const uint8_t bus = (uint8_t)gSettings.rtcBus;
  TwoWire* w = i2c() ? i2c()->getWire(bus) : nullptr;
  if (!w) return false;
  *outBus = bus;
  *outWire = w;
  return true;
}

// Sensor state
bool gRtcRunning = false;
bool gRtcConnected = false;
unsigned long gRtcLastStopTime = 0;
TaskHandle_t gRtcTaskHandle = nullptr;

// Task stack watermark tracking
volatile UBaseType_t gRtcWatermarkMin = (UBaseType_t)0xFFFFFFFF;
volatile UBaseType_t gRtcWatermarkNow = (UBaseType_t)0;

// ============================================================================
// BCD Conversion Helpers
// ============================================================================

static uint8_t bcdToDec(uint8_t bcd) {
  return ((bcd >> 4) * 10) + (bcd & 0x0F);
}

static uint8_t decToBcd(uint8_t dec) {
  return ((dec / 10) << 4) | (dec % 10);
}

// ============================================================================
// Low-level I2C Functions
// ============================================================================

static bool rtcWriteRegister(uint8_t reg, uint8_t value) {
  uint8_t bus; TwoWire* w;
  if (!rtcResolveBus(&bus, &w)) return false;
  return i2cDeviceTransaction(bus, I2C_ADDR_DS3231, 100000, 100, [&]() -> bool {
    w->beginTransmission(I2C_ADDR_DS3231);
    w->write(reg);
    w->write(value);
    return w->endTransmission() == 0;
  });
}

static bool rtcReadRegisters(uint8_t startReg, uint8_t* buffer, uint8_t count) {
  uint8_t bus; TwoWire* w;
  if (!rtcResolveBus(&bus, &w)) return false;
  return i2cDeviceTransaction(bus, I2C_ADDR_DS3231, 100000, 100, [&]() -> bool {
    w->beginTransmission(I2C_ADDR_DS3231);
    w->write(startReg);
    if (w->endTransmission() != 0) return false;

    w->requestFrom((uint8_t)I2C_ADDR_DS3231, count);
    for (uint8_t i = 0; i < count && w->available(); i++) {
      buffer[i] = w->read();
    }
    return true;
  });
}

// ============================================================================
// RTC Read/Write Functions
// ============================================================================

bool rtcReadDateTime(RtcDateTime* dt) {
  if (!dt) return false;
  
  uint8_t buffer[7];
  if (!rtcReadRegisters(DS3231_REG_SECONDS, buffer, 7)) {
    return false;
  }
  
  dt->second = bcdToDec(buffer[0] & 0x7F);
  dt->minute = bcdToDec(buffer[1]);
  dt->hour = bcdToDec(buffer[2] & 0x3F);  // 24-hour mode
  dt->dayOfWeek = buffer[3];
  dt->day = bcdToDec(buffer[4]);
  dt->month = bcdToDec(buffer[5] & 0x1F);
  dt->year = 2000 + bcdToDec(buffer[6]);
  
  return true;
}

// The chip's "oscillator stopped" flag (status register bit 7): set by the
// chip when it lost power or its battery ran empty, so the time it shows was
// not kept. It stays set until a time is written (rtcWriteDateTime clears it).
static constexpr uint8_t kRtcStatusOscillatorStopped = 0x80;

// True when the flag could be read; *stopped is then its state.
static bool rtcReadOscillatorStopped(bool* stopped) {
  uint8_t status = 0;
  if (!rtcReadRegisters(DS3231_REG_STATUS, &status, 1)) return false;
  *stopped = (status & kRtcStatusOscillatorStopped) != 0;
  return true;
}

bool rtcWriteDateTime(const RtcDateTime* dt) {
  if (!dt) return false;
  uint8_t bus; TwoWire* w;
  if (!rtcResolveBus(&bus, &w)) return false;

  const bool written = i2cDeviceTransaction(bus, I2C_ADDR_DS3231, 100000, 100, [&]() -> bool {
    w->beginTransmission(I2C_ADDR_DS3231);
    w->write(DS3231_REG_SECONDS);
    w->write(decToBcd(dt->second));
    w->write(decToBcd(dt->minute));
    w->write(decToBcd(dt->hour));  // 24-hour mode
    w->write(dt->dayOfWeek);
    w->write(decToBcd(dt->day));
    w->write(decToBcd(dt->month));
    w->write(decToBcd(dt->year - 2000));
    return w->endTransmission() == 0;
  });
  if (!written) return false;
  // The chip holds a time again: the "oscillator stopped" flag has done its
  // work. Left set after a failed read or write; the next write retries.
  uint8_t status = 0;
  if (rtcReadRegisters(DS3231_REG_STATUS, &status, 1) && (status & kRtcStatusOscillatorStopped)) {
    rtcWriteRegister(DS3231_REG_STATUS, status & (uint8_t)~kRtcStatusOscillatorStopped);
  }
  return true;
}

float rtcReadTemperature() {
  uint8_t buffer[2];
  if (!rtcReadRegisters(DS3231_REG_TEMP_MSB, buffer, 2)) {
    return kRtcTemperatureUnavailable;
  }
  
  // Temperature is in 0.25°C resolution
  int16_t temp = ((int16_t)buffer[0] << 2) | (buffer[1] >> 6);
  if (buffer[0] & 0x80) {
    temp |= 0xFC00;  // Sign extend for negative temps
  }
  return temp * 0.25f;
}

// ============================================================================
// Time Sync Functions
// ============================================================================

// UTC RtcDateTime -> epoch without timegm(). Pure civil-calendar arithmetic
// (Howard Hinnant's days_from_civil) — deliberately NOT the old
// "TZ=UTC0 + mktime + restore" dance: mutating the process-global TZ opens
// a window in which every other task's localtime_r() silently returns UTC,
// and the restore-via-saved-getenv-pointer variant was worse (newlib's
// setenv overwrites the environ string in place, so the saved pointer read
// back "UTC0" and pinned the device to UTC for the rest of the boot).
static time_t rtcDateTimeToEpochUtc(const RtcDateTime* dt) {
  // days_from_civil: days since 1970-01-01 for a proleptic-Gregorian date.
  int y = dt->year;
  const int m = dt->month;   // 1..12
  const int d = dt->day;     // 1..31
  y -= (m <= 2) ? 1 : 0;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);                        // [0, 399]
  const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);  // [0, 365]
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;            // [0, 146096]
  const long days = (long)era * 146097L + (long)doe - 719468L;
  return (time_t)days * 86400 + (time_t)dt->hour * 3600 +
         (time_t)dt->minute * 60 + (time_t)dt->second;
}

// A DS3231 with a dead/dying coin cell reports 2000-era time; adopting it
// would step a good clock backwards by decades. One gate, all adopters.
static inline bool rtcDateTimePlausible(const RtcDateTime* dt) {
  return dt->year >= 2020 && dt->year <= 2099;
}

// ---- The chip's time -> the system clock -------------------------------------
//
// Whether the chip's time is taken is two questions:
//
//   Is the chip trusted? Its time is real only when something set it and it
//   has kept time since. "Something set it" is gSettings.rtcTimeHasBeenSet
//   (this firmware wrote the chip from NTP, timeset, rtcset, the ring or the
//   CM5); "kept time since" is the chip's own oscillator-stopped flag. A chip
//   that is plausible but not trusted (a module fresh from a drawer with some
//   earlier owner's time, a chip whose battery ran empty and whose registers
//   held) is NOT adopted automatically: the boot, the RTC task and the NTP
//   fallback all ask rtcChipDistrust(). `rtcsync to` is the operator vouching
//   for a chip whose oscillator has not stopped: it is taken and marked set.
//
//   May the RTC set the clock now? Clock::step asks the rank table: the RTC
//   fills a clock that is not set and corrects the ring, mesh time without
//   NTP and a clock carried over a restart; it yields to NTP, a manual time,
//   an NTP-backed mesh master and the CM5.

enum class RtcAdopt : uint8_t { Automatic, OperatorAsked };

static const char kRtcWhyUnreadable[] = "Error: [RTC] Could not read the RTC. Check wiring and retry.";
static const char kRtcWhyImplausible[] =
    "Error: [RTC] RTC time is implausible (dead battery?). Set it with 'rtcset', or with 'rtcsync from' once the system clock is right.";
static const char kRtcWhyOscillatorStopped[] =
    "Error: [RTC] The RTC lost power since it was set (its oscillator-stopped flag is up), so its time is not used. "
    "Set it with 'rtcset', or with 'rtcsync from' once the system clock is right.";
static const char kRtcWhyNeverSet[] =
    "Error: [RTC] This device has not set the RTC yet, so its time is not adopted on its own. "
    "'rtcsync to' takes it by hand; NTP, 'timeset' or 'rtcset' set it.";

// The reason the chip's time is not trusted, or nullptr when it is.
static const char* rtcChipDistrust(RtcAdopt how) {
  bool stopped = false;
  if (!rtcReadOscillatorStopped(&stopped)) return kRtcWhyUnreadable;
  if (stopped) return kRtcWhyOscillatorStopped;
  if (how == RtcAdopt::Automatic && !gSettings.rtcTimeHasBeenSet) return kRtcWhyNeverSet;
  return nullptr;
}

// The one place the chip's time sets the system clock. False with *whyNot
// (an "Error: ..." line) when it did not.
static bool rtcAdoptToSystem(RtcAdopt how, const char** whyNot, RtcDateTime* adopted = nullptr) {
  const char* fallback = nullptr;
  if (!whyNot) whyNot = &fallback;

  RtcDateTime dt;
  if (!rtcReadDateTime(&dt)) { *whyNot = kRtcWhyUnreadable; return false; }
  if (!rtcDateTimePlausible(&dt)) { *whyNot = kRtcWhyImplausible; return false; }
  if (const char* distrust = rtcChipDistrust(how)) { *whyNot = distrust; return false; }

  const Clock::StepResult result = Clock::step(Clock::SYNC_RTC, rtcDateTimeToEpochUtc(&dt));
  if (result != Clock::StepResult::Stepped) {
    // The refusal names the source that outranks the chip; the text lives
    // for the rest of the boot (replies are returned by pointer).
    static char* refusal = nullptr;
    if (!refusal) refusal = (char*)ps_alloc(160, AllocPref::PreferPsram, "rtc.step.refusal");
    if (!refusal) { *whyNot = "Error: [RTC] The system clock was not changed"; return false; }
    char why[112];
    snprintf(refusal, 160, "Error: [RTC] System time not changed: %s",
             Clock::stepRefusalText(result, Clock::SYNC_RTC, why, sizeof(why)));
    *whyNot = refusal;
    return false;
  }
  // The operator vouched for the chip: later boots adopt it on their own.
  if (how == RtcAdopt::OperatorAsked && !gSettings.rtcTimeHasBeenSet) {
    setSetting(gSettings.rtcTimeHasBeenSet, true);
  }
  if (adopted) *adopted = dt;
  return true;
}

bool rtcEarlyBootSync(const char** whyNot) {
  // Called early at boot to set the system time from the RTC before NTP.
  // Works before the RTC sensor task runs: it needs only the I2C bus.
  const char* fallback = nullptr;
  if (!whyNot) whyNot = &fallback;

  // Quick probe on the RTC's configured bus (mutex-protected). Reads
  // gSettings.rtcBus rather than defaulting to bus 0 so an RTC on the
  // secondary STEMMA QT gets found during early-boot system-time sync.
  const uint8_t rtcBus = (uint8_t)gSettings.rtcBus;
  if (!i2cPingAddress(I2C_ADDR_DS3231, 100000, 200, rtcBus)) {
    DEBUG_RTC_LIFECYCLEF("[RTC] Early boot sync: RTC not detected at 0x%02X on bus %u",
                         I2C_ADDR_DS3231, rtcBus);
    *whyNot = nullptr;  // no chip: nothing to say
    return false;
  }

  RtcDateTime dt;
  if (!rtcAdoptToSystem(RtcAdopt::Automatic, whyNot, &dt)) {
    DEBUG_RTC_LIFECYCLEF("[RTC] Early boot sync: %s", *whyNot ? *whyNot : "not adopted");
    return false;
  }
  INFO_RTC_LIFECYCLEF("Early boot sync: System time set to %s UTC", rtcDateTimeToString(&dt).c_str());
  return true;
}

bool rtcSyncToSystem(const char** whyNot, bool operatorAsked) {
  const char* why = nullptr;
  RtcDateTime dt;
  if (!rtcAdoptToSystem(operatorAsked ? RtcAdopt::OperatorAsked : RtcAdopt::Automatic, &why, &dt)) {
    if (whyNot) *whyNot = why;
    else if (why) broadcastOutput(why);
    return false;
  }
  DEBUG_RTC_VALUESF("[RTC] Synced system time from RTC: %s", rtcDateTimeToString(&dt).c_str());
  return true;
}

bool rtcSyncFromSystem(const char** whyNot) {
  time_t now;
  time(&now);
  struct tm timeinfo;
  gmtime_r(&now, &timeinfo);  // RTC stores UTC, not local time
  
  RtcDateTime dt;
  dt.year = timeinfo.tm_year + 1900;
  dt.month = timeinfo.tm_mon + 1;
  dt.day = timeinfo.tm_mday;
  dt.hour = timeinfo.tm_hour;
  dt.minute = timeinfo.tm_min;
  dt.second = timeinfo.tm_sec;
  dt.dayOfWeek = timeinfo.tm_wday + 1;  // tm_wday is 0-6, RTC uses 1-7

  // A system clock that was never set still counts from 1970. The chip keeps
  // two year digits, so that year would be stored as a 20xx year the boot code
  // adopts. The same gate as for reading the chip, and the range rtcset takes.
  if (!rtcDateTimePlausible(&dt)) {
    if (whyNot) *whyNot = "Error: [RTC] System time is not set, so the RTC was not written. Set the RTC with 'rtcset', or sync the clock over the network and retry.";
    else broadcastOutput("[RTC] System time not set, RTC not written");
    return false;
  }

  if (!rtcWriteDateTime(&dt)) {
    if (whyNot) *whyNot = "Error: [RTC] Could not write the RTC. Check wiring and retry.";
    else broadcastOutput("[RTC] Failed to write system time to RTC");
    return false;
  }
  
  DEBUG_RTC_VALUESF("[RTC] Synced RTC from system time: %s", rtcDateTimeToString(&dt).c_str());
  return true;
}

// ============================================================================
// Utility Functions
// ============================================================================

uint32_t rtcToUnixTime(const RtcDateTime* dt) {
  time_t t = rtcDateTimeToEpochUtc(dt);

  return (uint32_t)t;
}

void unixTimeToRtc(uint32_t unixTime, RtcDateTime* dt) {
  time_t t = (time_t)unixTime;
  struct tm timeinfo;
  gmtime_r(&t, &timeinfo);
  
  dt->year = timeinfo.tm_year + 1900;
  dt->month = timeinfo.tm_mon + 1;
  dt->day = timeinfo.tm_mday;
  dt->hour = timeinfo.tm_hour;
  dt->minute = timeinfo.tm_min;
  dt->second = timeinfo.tm_sec;
  dt->dayOfWeek = timeinfo.tm_wday + 1;
}

String rtcDateTimeToString(const RtcDateTime* dt) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           dt->year, dt->month, dt->day,
           dt->hour, dt->minute, dt->second);
  return String(buf);
}

// Days in a given month, accounting for leap years.
static int rtcDaysInMonth(int month, int year) {
  static const int dim[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))) return 29;
  return dim[month];
}

// Day-of-week for a given date (Sakamoto's algorithm). Returns 0=Sun..6=Sat.
static int rtcDayOfWeek(int y, int m, int d) {
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y--;
  return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

RtcDateTime rtcLocalTime(const RtcDateTime* utc) {
  RtcDateTime local = *utc;

  int offsetMinutes = Clock::tzOffsetMinutes();
  if (offsetMinutes == 0) {
    // UTC: the date stands; derive the weekday from it like the shifted path.
    local.dayOfWeek = rtcDayOfWeek(local.year, local.month, local.day) + 1;
    return local;
  }

  int totalMinutes = local.hour * 60 + local.minute + offsetMinutes;

  if (totalMinutes < 0) {
    // Rolled back past midnight — go to previous day
    totalMinutes += 1440;
    local.day--;
    if (local.day < 1) {
      local.month--;
      if (local.month < 1) {
        local.month = 12;
        local.year--;
      }
      local.day = rtcDaysInMonth(local.month, local.year);
    }
  } else if (totalMinutes >= 1440) {
    // Rolled forward past midnight — go to next day
    totalMinutes -= 1440;
    local.day++;
    if (local.day > rtcDaysInMonth(local.month, local.year)) {
      local.day = 1;
      local.month++;
      if (local.month > 12) {
        local.month = 1;
        local.year++;
      }
    }
  }

  local.hour   = totalMinutes / 60;
  local.minute = totalMinutes % 60;
  // dayOfWeek: Sakamoto returns 0=Sun, RtcDateTime uses 1=Sun
  local.dayOfWeek = rtcDayOfWeek(local.year, local.month, local.day) + 1;

  return local;
}

// ============================================================================
// JSON building for ESP-NOW streaming
// ============================================================================

// The date and time fields are the chip's: UTC. "utc":true says so in the
// reply itself (`time json` and the web sensors page show local time, and
// nothing in this object told a reader that it does not).
int rtcBuildDataJson(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;
  
  RtcCache reading;
  const bool available = rtcCacheSnapshot(reading, 50);
  if (!available) {
    // Cache-lock timeout: not-ready envelope (was: return 0 / no output).
    int pos = sensorEnvelopeBegin(buf, bufSize, false, gRtcConnected, 0);
    if (pos == 0) return 0;
    int n = snprintf(buf + pos, bufSize - pos,
                     ",\"utc\":true,\"year\":0,\"month\":0,\"day\":0,\"hour\":0,\"minute\":0,\"second\":0,\"temp\":0}");
    if (n < 0 || (size_t)n >= bufSize - pos) return 0;
    return pos + n;
  }

  int pos = sensorEnvelopeBegin(buf, bufSize, reading.dataValid, gRtcConnected, reading.lastUpdate);
  if (pos == 0) return 0;
  int n = snprintf(buf + pos, bufSize - pos,
                   ",\"utc\":true,\"year\":%u,\"month\":%u,\"day\":%u,"
                   "\"hour\":%u,\"minute\":%u,\"second\":%u,\"temp\":%.1f}",
                   reading.dateTime.year, reading.dateTime.month, reading.dateTime.day,
                   reading.dateTime.hour, reading.dateTime.minute, reading.dateTime.second,
                   reading.temperature);
  if (n < 0 || (size_t)n >= bufSize - pos) return 0;
  return pos + n;
}

// ============================================================================
// RTC Task
// ============================================================================

// Read DS3231 + write to sRtcCache. Factored out so the task can perform a
// cold-start poll before entering its 30s periodic loop — keeps the cache
// fresh for local readers (OLED, web UI) without waiting up to 30s.
static void rtcReadAndCacheOnce(unsigned long now) {
  RtcDateTime dt;
  float temp = rtcReadTemperature();
  if (!rtcReadDateTime(&dt)) return;

  {
    SensorCacheGuard g(sRtcCacheMutex, pdMS_TO_TICKS(50), "rtc.pollWrite");
    if (g.held) {
      sRtcCache.dateTime = dt;
      sRtcCache.temperature = temp;
      sRtcCache.dataValid = true;
      sRtcCache.lastUpdate = now;
    }
  }
#if ENABLE_OLED_DISPLAY
  if (currentOledMode == OLED_RTC_DATA) {
    oledMarkDirty();
  }
#endif
  // ESP-NOW broadcaster reads rtcBuildDataJson() on demand from the native cache;
  // no propagation step needed here.
}

void rtcTask(void* pvParameters) {
  (void)pvParameters;
  
  DEBUG_RTC_LIFECYCLEF("[RTC] Task started");
  
  // Check if system time is already valid (from RTC early boot sync or NTP)
  bool systemTimeValid = Clock::isSynced();
  
  if (systemTimeValid) {
    // System already has valid time - if RTC is already calibrated, just verify;
    // if not calibrated, sync TO RTC, but only from a source that knows the
    // time: not mesh time and not a clock carried over a soft restart (it may
    // have come from the mesh), or the chip would read back as "rtc" at every
    // later boot. Such a chip waits for the next ntp, timeset, ring or cm5
    // step (Clock::clockStepped writes it then).
    if (!gSettings.rtcTimeHasBeenSet) {
      if (Clock::sourceWritesRtc(Clock::syncSource()) && rtcSyncFromSystem()) {
        broadcastOutput("[RTC] RTC calibrated from system time");
        setSetting(gSettings.rtcTimeHasBeenSet, true);
      }
    } else {
      DEBUG_RTC_LIFECYCLEF("[RTC] System time already valid from RTC early boot - no sync needed");
    }
  } else {
    // No valid system time yet: take the chip's time when the chip is
    // trusted (set by this device, oscillator not stopped since). An
    // untrusted chip waits for NTP, timeset, rtcset or 'rtcsync to'.
    const char* whyNot = nullptr;
    if (rtcSyncToSystem(&whyNot)) {
      broadcastOutput("[RTC] System time synchronized from RTC");
    } else if (whyNot) {
      DEBUG_RTC_LIFECYCLEF("[RTC] System time not taken from the RTC: %s", whyNot);
    }
  }
  
  const unsigned long CACHE_UPDATE_FAST = 1000;   // 1s when OLED is showing RTC
  const unsigned long CACHE_UPDATE_SLOW = 30000;  // 30s otherwise (web UI ticks locally)

  // Cold-start poll so the cache is fresh before the first 30s tick.
  unsigned long lastCacheUpdate = millis();
  rtcReadAndCacheOnce(lastCacheUpdate);

  while (gRtcRunning) {
    unsigned long now = millis();

    // Respect global polling pause (I2C bus recovery, scans, file I/O, etc.)
    if (pollPaused((uint8_t)gSettings.rtcBus)) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Poll fast when OLED is displaying RTC, slow otherwise
#if ENABLE_OLED_DISPLAY
    unsigned long interval = (currentOledMode == OLED_RTC_DATA) ? CACHE_UPDATE_FAST : CACHE_UPDATE_SLOW;
#else
    unsigned long interval = CACHE_UPDATE_SLOW;
#endif
    if (now - lastCacheUpdate >= interval) {
      rtcReadAndCacheOnce(now);
      lastCacheUpdate = now;
    }
    
    // Track stack watermark + safety bailout (check every ~10s = 100 iterations at 100ms)
    UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
    gRtcWatermarkNow = watermark;
    if (watermark < gRtcWatermarkMin) {
      gRtcWatermarkMin = watermark;
    }
    {
      static uint32_t sRtcSafetyCounter = 0;
      if (++sRtcSafetyCounter >= 100) {
        sRtcSafetyCounter = 0;
        if (checkTaskStackSafety("rtc", RTC_STACK_WORDS, &gRtcRunning)) break;
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  
  DEBUG_RTC_LIFECYCLEF("[RTC] Task exiting");
  // The task acknowledges exit only after the reading is invalidated. Each
  // attempt is bounded; readers hold this lock only long enough to copy.
  gRtcConnected = false;
  while (!rtcInvalidateCache()) vTaskDelay(pdMS_TO_TICKS(10));
  gRtcTaskHandle = nullptr;
  vTaskDelete(nullptr);
}

// ============================================================================
// Sensor Lifecycle
// ============================================================================

bool rtcInit() {
  const uint8_t rtcBus = (uint8_t)gSettings.rtcBus;
  DEBUG_RTC_LIFECYCLEF("[RTC] Initializing DS3231 on bus %u...", rtcBus);

  // Check if device responds on the configured bus
  bool found = i2cPingAddress(I2C_ADDR_DS3231, 100000, 50, rtcBus);
  if (!found) {
    DEBUG_RTC_LIFECYCLEF("[RTC] DS3231 not found at 0x%02X on bus %u", I2C_ADDR_DS3231, rtcBus);
    return false;
  }
  
  // Initialize cache mutex
  if (!sRtcCacheMutex) {
    sRtcCacheMutex = xSemaphoreCreateMutex();
    if (!sRtcCacheMutex) {
      DEBUG_RTC_LIFECYCLEF("[RTC] Failed to create cache mutex");
      return false;
    }
  }
  
  // The oscillator-stopped flag says the chip lost power: the time it shows
  // was not kept. The flag is left up (the boot and the RTC task read it and
  // do not adopt the chip) until a time is written, and the chip is no longer
  // "set by this device". Before 2026-11-22 the flag was cleared here, so the
  // next boot took the chip's time as good.
  bool oscillatorStopped = false;
  if (rtcReadOscillatorStopped(&oscillatorStopped) && oscillatorStopped) {
    DEBUG_RTC_LIFECYCLEF("[RTC] Oscillator was stopped - RTC time is not used until it is set again");
    systemEventPost(SYSEVT_RTC_POWER_LOSS, "lost", "RTC battery");
    if (gSettings.rtcTimeHasBeenSet) setSetting(gSettings.rtcTimeHasBeenSet, false);
  }
  
  gRtcConnected = true;
  DEBUG_RTC_LIFECYCLEF("[RTC] DS3231 initialized successfully");
  return true;
}

// createRtcTask() is now defined in System_TaskUtils.cpp (centralized helper).


// Called only by the lifecycle worker. A live task owns its own cleanup;
// rtcInit() can also connect hardware without creating a polling task.
void rtcStop() {
  gRtcRunning = false;
  if (gRtcTaskHandle != nullptr) return;
  gRtcConnected = false;
  if (!rtcInvalidateCache()) {
    DEBUG_RTC_LIFECYCLEF("[RTC] Reading invalidation unavailable");
  }
}

// Internal start function for sensor queue processor
bool rtcStartInternal() {
  if (gRtcRunning && gRtcConnected) {
    DEBUG_RTC_LIFECYCLEF("[RTC] Already running");
    return true;
  }

  // A prior stop may have returned while rtcTask was completing a settings
  // write. Do not flip gRtcRunning back to true underneath that old task or
  // create a second poller; retry is safe once its exit tail clears the handle.
  if (gRtcTaskHandle != nullptr) {
    DEBUGF_QUEUE(DEBUG_ALWAYS | DEBUG_RTC,
                 "[WARN][RTC] Previous task is still exiting; retry start shortly");
    return false;
  }

  // Check memory before creating RTC task
  if (!checkMemoryAvailable("rtc", nullptr)) {
    ERROR_RTCF("Insufficient memory for RTC sensor");
    return false;
  }

  if (!sRtcCacheMutex) {
    sRtcCacheMutex = xSemaphoreCreateMutex();
    if (!sRtcCacheMutex) return false;
  }

  // Clean up any stale cache from previous run BEFORE starting
  {
    SensorCacheGuard g(sRtcCacheMutex, pdMS_TO_TICKS(100), "rtc.cleanStaleCache");
    if (!g.held) return false;
    sRtcCache.dataValid = false;
    sRtcCache.temperature = 0.0f;
    memset(&sRtcCache.dateTime, 0, sizeof(RtcDateTime));
  }

  if (!rtcInit()) {
    DEBUG_RTC_LIFECYCLEF("[RTC] Failed to initialize");
    return false;
  }

  gRtcRunning = true;  // Set this BEFORE task creation — the task's main loop
                       // is `while (gRtcRunning)`, so flipping this after
                       // createRtcTask() races and the task exits immediately
                       // (web UI then shows "RTC Closed" despite a successful
                       // openrtc command). Matches IMU/Thermal pattern.
  if (!createRtcTask()) {
    DEBUG_RTC_LIFECYCLEF("[RTC] Failed to create task");
    gRtcRunning = false;
    gRtcConnected = false;
    return false;
  }

  DEBUG_RTC_LIFECYCLEF("[RTC] Started successfully via queue");
  return true;
}

// ============================================================================
// CLI Command Handlers
// ============================================================================

const char* cmd_rtc(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    if (!ensureDebugBuffer()) return SENSOR_JSON_NOBUF;
    int n = rtcBuildDataJson(getDebugBuffer(), 1024);  // shared builder (also feeds sensors json)
    return (n > 0) ? getDebugBuffer() : SENSOR_JSON_UNAVAILABLE;
  }

  static String response;
  response = "";
  
  String arg = argsInput;
  arg.trim();
  
  if (arg.length() == 0 || arg == "status") {
    // Show RTC status and current time
    if (!gRtcConnected) {
      response = "Error: [RTC] Not connected. Use 'openrtc' to initialize.";
      return response.c_str();
    }
    
    RtcDateTime utc;
    if (rtcReadDateTime(&utc)) {
      float temp = rtcReadTemperature();
      RtcDateTime local = rtcLocalTime(&utc);
      // Shared label helper — the hand-rolled version here lost the sign
      // for negative sub-hour offsets ((offsetMin/60)*sign truncated to +0).
      char tzBuf[12];
      Clock::formatTzOffsetLabel(tzBuf, sizeof(tzBuf));
      response = "[RTC] " + rtcDateTimeToString(&local);
      char tempBuf[64];
      if (temp == kRtcTemperatureUnavailable) {
        snprintf(tempBuf, sizeof(tempBuf), " %s | Temp: n/a | Unix: %lu",
                 tzBuf, (unsigned long)rtcToUnixTime(&utc));
      } else {
        snprintf(tempBuf, sizeof(tempBuf), " %s | Temp: %.1f°C | Unix: %lu",
                 tzBuf, temp, (unsigned long)rtcToUnixTime(&utc));
      }
      response += tempBuf;
    } else {
      response = "Error: [RTC] Failed to read time";
    }
    return response.c_str();
  }

  if (arg == "temp" || arg == "temperature") {
    if (!gRtcConnected) {
      cliHint("the RTC is not open - run 'openrtc' first");
      return "Error: [RTC] Not connected";
    }
    float temp = rtcReadTemperature();
    if (temp == kRtcTemperatureUnavailable) return "Error: [RTC] Temperature read failed. Check wiring and retry.";
    char tempBuf[32];
    snprintf(tempBuf, sizeof(tempBuf), "[RTC] Temperature: %.2f°C", temp);
    response = tempBuf;
    return response.c_str();
  }
  
  response = "Error: [RTC] Unknown command. Use: rtcread [status|temp]";
  return response.c_str();
}

const char* cmd_rtcset(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  static String response;
  response = "";

  if (!gRtcConnected) {
    return "Error: [RTC] Not connected. Use 'openrtc' first.";
  }

  // The same reading as timeset (clockcore::parseTypedTime): a date and time
  // is device-local unless the word utc follows it, a Unix timestamp is UTC,
  // the result lies within 2020-2099 (the years the boot code adopts from the
  // chip). Before 2026-11-22 this command read the same text as UTC while
  // timeset read it as local.
  clockcore::TypedTime typed = { 0, false };
  const clockcore::TypedTimeError problem =
      clockcore::parseTypedTime(argsInput.c_str(), Clock::tzOffsetMinutes(), &typed);
  if (problem != clockcore::TypedTimeError::None) return clockcore::typedTimeErrorText(problem);

  // The chip keeps UTC.
  const time_t t = (time_t)typed.epoch;
  RtcDateTime dt;
  unixTimeToRtc((uint32_t)typed.epoch, &dt);
  if (!rtcWriteDateTime(&dt)) {
    return "Error: [RTC] Could not write the RTC. Check wiring and retry.";
  }

  char local[40], utc[24];
  Clock::formatDateTimeLocal(t, local, sizeof(local), true, true);
  Clock::formatIso8601Utc(t, utc, sizeof(utc));
  response = String("[RTC] Time set to: ") + local + " (" + utc + "), read as " +
             (typed.wasUtc ? "UTC" : "local time");
  // Mark RTC as calibrated so future boots trust RTC first
  if (!gSettings.rtcTimeHasBeenSet) {
    setSetting(gSettings.rtcTimeHasBeenSet, true);
    response += "\n[RTC] Marked as calibrated for future boots";
  }

  // The system clock follows: a time typed by the operator is a manual time
  // for the whole device, not for the chip alone (it used to stay unchanged
  // until the next boot or 'rtcsync to').
  if (Clock::step(Clock::SYNC_MANUAL, t) == Clock::StepResult::Stepped) {
    Clock::clockDutiesFlush();
    response += "\n[RTC] System time set to the same time";
  } else {
    response += "\n[RTC] The system time was not changed (the system refused it); run 'rtcsync to'";
  }

  return response.c_str();
}

const char* cmd_rtcsync(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gRtcConnected) {
    return "Error: [RTC] Not connected. Use 'openrtc' first.";
  }

  String arg = argsInput;
  arg.trim();

  if (arg == "tosystem" || arg == "to" || arg.length() == 0) {
    // RTC -> System (default). The operator asks for the chip's time: a
    // chip this device has not set itself is taken too (and marked set), as
    // long as its oscillator has not stopped. The clock's rank table still
    // decides: the reply says which source outranks the RTC when it does.
    const char* whyNot = "Error: [RTC] Sync failed";
    if (rtcSyncToSystem(&whyNot, true)) {
      Clock::clockDutiesFlush();
      return "[RTC] System time updated from RTC";
    }
    return whyNot ? whyNot : "Error: [RTC] Sync failed";
  }
  
  if (arg == "fromsystem" || arg == "from") {
    // System -> RTC
    const char* whyNot = "Error: [RTC] Sync failed";
    if (rtcSyncFromSystem(&whyNot)) {
      // Mark RTC as calibrated so future boots trust RTC first
      if (!gSettings.rtcTimeHasBeenSet) {
        setSetting(gSettings.rtcTimeHasBeenSet, true);
      }
      return "[RTC] RTC updated from system time";
    }
    return whyNot;
  }

  return "Error: invalid arguments — Usage: rtcsync [to|from]  (to=RTC->system, from=system->RTC)";
}

const char* cmd_rtcstart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (const char* error = i2cOpenCloseArgError(I2C_DEVICE_RTC, argsInput, "openrtc")) return error;
  return cmd_sensorstart_queued(I2C_DEVICE_RTC, "RTC", "openrtc@enqueue");
}

const char* cmd_rtcstop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (const char* error = i2cOpenCloseArgError(I2C_DEVICE_RTC, argsInput, "closertc")) return error;
  return cmd_sensorstop_queued(I2C_DEVICE_RTC, "RTC");
}

// ============================================================================
// RTC Accessor Functions (for MQTT and other modules)
// ============================================================================

int rtcGetYear() {
  RtcCache reading;
  if (!gRtcConnected || !rtcCacheSnapshot(reading) || !reading.dataValid) return 0;
  return reading.dateTime.year;
}

int rtcGetMonth() {
  RtcCache reading;
  if (!gRtcConnected || !rtcCacheSnapshot(reading) || !reading.dataValid) return 0;
  return reading.dateTime.month;
}

int rtcGetDay() {
  RtcCache reading;
  if (!gRtcConnected || !rtcCacheSnapshot(reading) || !reading.dataValid) return 0;
  return reading.dateTime.day;
}

int rtcGetHour() {
  RtcCache reading;
  if (!gRtcConnected || !rtcCacheSnapshot(reading) || !reading.dataValid) return 0;
  return reading.dateTime.hour;
}

int rtcGetMinute() {
  RtcCache reading;
  if (!gRtcConnected || !rtcCacheSnapshot(reading) || !reading.dataValid) return 0;
  return reading.dateTime.minute;
}

int rtcGetSecond() {
  RtcCache reading;
  if (!gRtcConnected || !rtcCacheSnapshot(reading) || !reading.dataValid) return 0;
  return reading.dateTime.second;
}

float rtcGetTemperature() {
  RtcCache reading;
  if (!gRtcConnected || !rtcCacheSnapshot(reading) || !reading.dataValid) return 0.0f;
  return reading.temperature;
}

// ============================================================================
// RTC Modular Settings Registration
// ============================================================================

#include "System_Settings.h"

// RTC settings entries
// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry rtcSettingEntries[] = {
  { "rtcAutoStart", SETTING_BOOL, &gSettings.rtcAutoStart, 1, 0, nullptr, 0, 1, "Auto-start after boot", nullptr, false, nullptr, "rtcautostart" },
  { "rtcTimeHasBeenSet", SETTING_BOOL, &gSettings.rtcTimeHasBeenSet, 0, 0, nullptr, 0, 1, "RTC time has been set (NTP/manual)", nullptr, false, nullptr, nullptr, true },
};

static bool isRtcConnectedSetting() {
  return gRtcConnected;
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule rtcSettingsModule = {
  "rtc",
  "hardware.sensors.rtc",
  rtcSettingEntries,
  sizeof(rtcSettingEntries) / sizeof(rtcSettingEntries[0]),
  isRtcConnectedSetting,
  "DS3231 real-time clock"
};

// ============================================================================
// Command Registry
// ============================================================================

#include "System_Utils.h"

const char* cmd_rtcautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput;
  normalizeCliArg(arg);
  if (arg.length() == 0) {
    return gSettings.rtcAutoStart ? "[RTC] Auto-start: enabled" : "[RTC] Auto-start: disabled";
  }
  if (argIsOn(arg)) {
    setSetting(gSettings.rtcAutoStart, true);
    return "[RTC] Auto-start enabled";
  } else if (argIsOff(arg)) {
    setSetting(gSettings.rtcAutoStart, false);
    return "[RTC] Auto-start disabled";
  }
  return "Error: invalid arguments — Usage: rtcautostart [on|off]";
}

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
// Bus routing for this device. Lives here rather than in System_I2C.cpp so it
// compiles out with the driver — see i2cSetDeviceBusAndReport in System_I2C.h.
static const char* cmd_rtcbus(const String& a) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return i2cSetDeviceBusAndReport(gSettings.rtcBus, a, "rtcBus", i2cInstanceOwner(I2C_DEVICE_RTC, 1));
}

const CommandEntry rtcCommands[] = {
  { "openrtc",  "Start DS3231 RTC sensor.",                    true, cmd_rtcstart },
  { "closertc", "Stop DS3231 RTC sensor.",                     true, cmd_rtcstop },
  { "rtcread",   "Read RTC status [status|temp]",               false, cmd_rtc,      "Usage: rtcread [status|temp] [json]" },
  { "rtcset",   "Set the RTC chip and the system clock: a local date and time (add utc for UTC) or a Unix timestamp.", true,  cmd_rtcset,
    "Usage: rtcset <YYYY-MM-DD HH:MM:SS> [utc|local] | <unix_timestamp>  (local when no word is given; a Unix timestamp is UTC; years 2020-2099)" },
  { "rtcsync",  "Copy time between the RTC chip and the system clock: [to|from]", true,  cmd_rtcsync,
    "Usage: rtcsync [to|from] (to=RTC->system, the default; from=system->RTC). 'to' is refused while the clock is set from a source that outranks the RTC (ntp, manual, mesh-ntp, cm5)." },
  
  // Auto-start
  { "rtcautostart", "Enable/disable RTC auto-start after boot [on|off]", true, cmd_rtcautostart, "Usage: rtcautostart [on|off]" },
  { "rtcbus",      "Route DS3231 RTC to bus: <0|1> (reboot required)",   true, cmd_rtcbus,      "Usage: rtcBus <0|1>" },
};
const size_t rtcCommandsCount = sizeof(rtcCommands) / sizeof(rtcCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// RTC OLED Mode (Display Function + Registration)
// ============================================================================
#if DISPLAY_TYPE > 0
#include "i2csensor_ds3231_oled.h"
#endif

#endif // ENABLE_RTC_SENSOR
