/**
 * Sensor Logging System - Data logging for sensor readings
 * 
 * Provides configurable logging of sensor data to files with:
 * - Selectable sensors (thermal, tof, imu, gamepad, apds, gps)
 * - Configurable intervals and file sizes
 * - Text and CSV output formats
 * - Log rotation support
 */

#include "System_SensorLogging.h"
#include "System_Clock.h"   // Clock::isSynced — session subfolder/timestamp shaping
#include "System_Utils.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_Mutex.h"
#include "System_I2C.h"
#include "System_MemUtil.h"
#include "System_TaskUtils.h"
#include "System_Notifications.h"
#include "System_Settings.h"
#include "System_Filesystem.h"
#include "System_VFS.h"
#include "System_Mutex.h"         // FsLockGuard
#include "System_AuthIdentity.h"  // currentAuthContext
#include <LittleFS.h>

// Conditional sensor includes (same approach as main .ino)
#include "System_BuildConfig.h"
#if ENABLE_THERMAL_SENSOR
  #include "i2csensor_mlx90640.h"
#endif
#if ENABLE_IMU_SENSOR
  #include "i2csensor_bno055.h"
#endif
#if ENABLE_TOF_SENSOR
  #include "i2csensor_vl53l4cx.h"
#endif
#if ENABLE_OLED_INPUT
  #include "HAL_Input.h"   // gInputCache, gInputRunning/Connected — populated by either driver
#endif
#if ENABLE_APDS_SENSOR
  #include "i2csensor_apds9960.h"
#endif
#if ENABLE_GPS_SENSOR
  #include "i2csensor_pa1010d.h"
  #include <Adafruit_GPS.h>
#endif
#if ENABLE_PRESENCE_SENSOR
  #include "i2csensor_sths34pf80.h"
#endif
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
  #include "G2_Ring.h"
  #include "BLE_Peers.h"   // blePeerRequestReseek — Health Track mine when ring down
#endif
#if ENABLE_R1_HEALTH
  #include "G2_Health.h"   // g2HealthPageIsActive — don't race page poll bursts
  #include "BLE_Events.h"  // CompactJson for healthstatus json
#endif
#include "System_SensorStubs.h"  // Provides stubs for disabled sensors

// External dependencies
extern void getTimestampPrefixMsCached(char* out, size_t outSize);

// Modular sensor caches (conditionally available based on enabled sensors)

// ============================================================================
// Sensor Logging State Variables
// ============================================================================

bool gSensorLoggingRunning = false;
String gSensorLogPath = "";
unsigned long gSensorLogLastWrite = 0;
uint32_t gSensorLogIntervalMs = 5000;
size_t gSensorLogMaxSize = 250 * 1024;
SensorLogFormat gSensorLogFormat = SENSOR_LOG_TEXT;
uint8_t gSensorLogMaxRotations = 3;
uint8_t gSensorLogMask = 0x00;

// ============================================================================
// Sensor Logging Tick (called from main loop — no dedicated task needed)
// ============================================================================

static volatile bool sForceSample = false;
static volatile bool sBypassR1Dedup = false;

void sensorLogRequestSample(bool bypassR1Dedup) {
  if (!gSensorLoggingRunning) return;
  if ((gSensorLogMask & LOG_R1) == 0 && !bypassR1Dedup) {
    // Still allow forced writes for mixed logs; bypass flag is mainly for R1.
  }
  sForceSample = true;
  sBypassR1Dedup = bypassR1Dedup;
}

// ============================================================================
// Per-day append support — see docs/SENSORLOG_PERDAY_APPEND_PLAN.md
// ============================================================================
// CSV capture sessions target ONE file per calendar day so a day of health
// data graphs as a single continuous series. The pieces below keep that safe:
// path-mode classification (day/boot/manual), a shared CSV-header builder
// used both to create files and to verify an existing day file is column-
// compatible before appending, and size seeding so rotation stays honest
// across restarts.

// Path-shape classification for the ACTIVE session, derived from the path
// cmd_sensorlog start accepted — so every entry point (autostart, healthtrack,
// manual CLI) lands in the right mode without threading flags around:
//   MANUAL      literal user path — never rolled, never re-pointed.
//   BOOT_SHAPED shaped dark-boot session (…/sensors/boot-N/…) — rolls onto
//               the day file the moment the clock syncs (CSV only). Without
//               this, an always-on device that boots before WiFi would write
//               one boot file forever and never produce a day file.
//   DAY         shaped dated session (…/sensors/YYYY-MM-DD/…) — rolls quietly
//               at midnight.
enum class SensorLogPathMode : uint8_t { MANUAL, BOOT_SHAPED, DAY };
static SensorLogPathMode gSensorLogPathMode = SensorLogPathMode::MANUAL;
static char gSensorLogDayStr[11] = {0};  // "YYYY-MM-DD" of a DAY session

// Rotation size accounting. File-scope (not tick-static) so session start and
// rollover can SEED it from the on-disk size of the target — otherwise
// appending to an already-large day file under-counts (rotation fires ~one
// full maxSize late per boot) and a same-boot restart onto a fresh file
// carries a stale count (rotates early). Written from the cmd_exec task at
// start and the main-loop tick; both are single-word writes and the worst
// mis-order costs one slightly-early rotation.
static size_t gApproxSizeBytes = 0;

static String shapeSessionPath(String path);      // shared shaper (defined below)
static String stripSessionShaping(String path);   // its inverse (defined below)

// CSV header for `mask` — the single source for file creation, the append-
// side header-on-create, and the day-file compatibility compare. NO trailing
// newline (callers append it). Must track buildCSVFromSnap's column order.
static String buildCsvHeader(uint8_t mask) {
  String h = "timestamp_ms";
  if (mask & LOG_THERMAL) h += ",thermal_min,thermal_max,thermal_avg";
  if (mask & LOG_TOF) {
    h += ",tof_obj0_dist,tof_obj0_valid,tof_obj0_status";
    h += ",tof_obj1_dist,tof_obj1_valid,tof_obj1_status";
    h += ",tof_obj2_dist,tof_obj2_valid,tof_obj2_status";
    h += ",tof_obj3_dist,tof_obj3_valid,tof_obj3_status";
  }
  if (mask & LOG_IMU) {
    h += ",imu_yaw,imu_pitch,imu_roll";
    h += ",imu_accel_x,imu_accel_y,imu_accel_z";
    h += ",imu_gyro_x,imu_gyro_y,imu_gyro_z";
    h += ",imu_temp";
  }
  if (mask & LOG_GAMEPAD) h += ",gamepad_x,gamepad_y,gamepad_buttons";
  if (mask & LOG_APDS) h += ",apds_red,apds_green,apds_blue,apds_clear,apds_proximity,apds_gesture";
  if (mask & LOG_GPS) h += ",gps_fix,gps_lat,gps_lon,gps_alt,gps_speed,gps_sats,gps_quality";
  if (mask & LOG_PRESENCE) h += ",presence_ambient,presence_value,presence_detected,motion_value,motion_detected";
  // Order must track the row builder in buildCSVFromSnap: connected, hr,
  // hrv, spo2, temp, battery, wear. r1_temp sits BETWEEN spo2 and battery
  // — appending the two missing names at the end would keep the count
  // right and silently mislabel the columns. r1_wear is the raw code
  // (0 unknown / 1 not worn / 2 worn), not the on/off words TEXT logs use.
  if (mask & LOG_R1) h += ",r1_connected,r1_hr,r1_hrv,r1_spo2,r1_temp,r1_battery,r1_wear";
  return h;
}

// TRACK static header (mask-independent; hoisted from the create block).
static const char kTrackHeader[] =
    "# GPS Track Log\n"
    "# time,lat,lon,alt_m,speed_kn,satellites\n"
    "# Signal loss: time,---,SIGNAL_LOST\n"
    "# Signal regained: time,~~~,SIGNAL_REGAINED (lost N intervals)\n";

// Write the format-appropriate header into a freshly-created/empty file.
// TEXT has no header (rows are self-describing). Returns false on a short
// write so callers can treat the file as unusable instead of shipping a
// truncated header that would poison every later compatibility compare.
static bool writeHeaderChecked(File& f) {
  if (gSensorLogFormat == SENSOR_LOG_CSV) {
    String h = buildCsvHeader(gSensorLogMask);
    h += '\n';
    return f.write((const uint8_t*)h.c_str(), h.length()) == h.length();
  }
  if (gSensorLogFormat == SENSOR_LOG_TRACK) {
    return f.write((const uint8_t*)kTrackHeader, strlen(kTrackHeader)) ==
           strlen(kTrackHeader);
  }
  return true;  // TEXT: headerless by design
}

// Classify the accepted session path into gSensorLogPathMode (+ capture the
// day string for DAY sessions). Called at session start and after a rollover.
static void sensorLogClassifyActivePath(const char* path) {
  gSensorLogPathMode = SensorLogPathMode::MANUAL;
  gSensorLogDayStr[0] = '\0';
  static const char kPrefix[] = CAPTURE_DIR_SENSORS "/";
  const size_t prefixLen = sizeof(kPrefix) - 1;
  if (!path || strncmp(path, kPrefix, prefixLen) != 0) return;
  const char* comp = path + prefixLen;
  const char* slash = strchr(comp, '/');
  if (!slash) return;  // flat file directly in sensors/ → manual
  const size_t n = (size_t)(slash - comp);
  if (n == 10 && comp[4] == '-' && comp[7] == '-' &&
      isdigit((unsigned char)comp[0]) && isdigit((unsigned char)comp[1]) &&
      isdigit((unsigned char)comp[2]) && isdigit((unsigned char)comp[3])) {
    gSensorLogPathMode = SensorLogPathMode::DAY;
    memcpy(gSensorLogDayStr, comp, 10);
    gSensorLogDayStr[10] = '\0';
  } else if (n > 5 && strncmp(comp, "boot-", 5) == 0) {
    gSensorLogPathMode = SensorLogPathMode::BOOT_SHAPED;
  }
}

// On-disk size of the overflow-resolved tier for `path` (0 if absent).
// Used to seed gApproxSizeBytes when (re)pointing at a day file.
static size_t sensorLogResolvedSize(const String& path) {
  char resolved[128];
  VFS::resolveOverflowPath(path.c_str(), (size_t)gSensorLogMaxSize,
                           resolved, sizeof(resolved));
  File f = VFS::openGuarded(String(resolved), "r",
                            VFS::systemAuth("senlog.sizeseed"));
  if (!f) return 0;
  size_t sz = f.size();
  f.close();
  return sz;
}

// Pick the actual target for a shaped session path. CSV day files append
// across sessions, so an existing file is only reused when its first line
// matches the header the CURRENT mask would write; a mismatch (mask changed
// since the file was created) probes -2..-9 variants, and if all are taken we
// fall back to a per-session timestamped name — capture must never refuse to
// start. Probes run on the OVERFLOW-RESOLVED tier (appends go there; probing
// the LittleFS path while writes land on /sd would validate the wrong file).
// Missing or EMPTY candidate = fresh and adopted (a failed/interrupted header
// write never burns a variant; the header lands via header-on-create in the
// append path or the start-time create). Returns the UNRESOLVED path — the
// append path re-resolves per write.
static String resolveSessionTarget(const String& shaped) {
  if (gSensorLogFormat != SENSOR_LOG_CSV) return shaped;  // TEXT/TRACK: append-safe

  const String expect = buildCsvHeader(gSensorLogMask);
  int lastDot = shaped.lastIndexOf('.');
  int lastSlash = shaped.lastIndexOf('/');
  String stem = (lastDot > lastSlash) ? shaped.substring(0, lastDot) : shaped;
  String ext  = (lastDot > lastSlash) ? shaped.substring(lastDot) : String("");

  for (int v = 1; v <= 9; v++) {
    String candidate = (v == 1) ? shaped : (stem + "-" + String(v) + ext);
    char resolved[128];
    VFS::resolveOverflowPath(candidate.c_str(), (size_t)gSensorLogMaxSize,
                             resolved, sizeof(resolved));
    File f = VFS::openGuarded(String(resolved), "r",
                              VFS::systemAuth("senlog.compat"));
    if (!f) return candidate;                       // missing → fresh
    if (f.size() == 0) { f.close(); return candidate; }  // empty → adopt
    // Read the first line (header). 1024-byte buffer: the worst-case header
    // with every mask bit set is ~640 B — a smaller buffer would silently
    // truncate and mis-compare. Strip trailing CR/LF/whitespace so a file
    // that round-tripped through a CRLF editor still matches.
    char first[1024];
    size_t fl = 0;
    while (f.available() && fl + 1 < sizeof(first)) {
      int c = f.read();
      if (c < 0 || c == '\n') break;
      first[fl++] = (char)c;
    }
    f.close();
    while (fl > 0 && (first[fl - 1] == '\r' || first[fl - 1] == ' ' ||
                      first[fl - 1] == '\t')) fl--;
    first[fl] = '\0';
    if (expect == first) return candidate;          // header matches → append
    // else: column set differs — try the next variant
  }

  // All variants taken by incompatible masks — per-session timestamped
  // fallback in the same folder (never refuse to capture).
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  char stamp[24];
  strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H-%M-%S", &lt);
  return stem + "-" + stamp + ext;
}

void sensorLogTick() {
  // Health Track mine runs even between sensorlog intervals (and while Track
  // is on). Must be called before the early-return below.
  healthTrackTick();

  if (!gSensorLoggingRunning) return;

  static unsigned long lastTickMs = 0;
  unsigned long nowMs = millis();
  const bool forced = sForceSample;

  // Health Track owns R1-only sessions: samples come only from the mine
  // interval / Poll Now / page refresh (forced). Suppress the normal
  // sensorlog cadence and the 5 s "ring down" timestamp heartbeats that
  // were filling health-*.csv with empty lines.
  const bool healthOwnedR1Only =
      gSettings.healthTrackingEnabled &&
      (gSensorLogMask & LOG_R1) != 0 &&
      (gSensorLogMask & (uint8_t)~LOG_R1) == 0;

  if (healthOwnedR1Only) {
    if (!forced) return;
  } else {
    if (!forced && lastTickMs != 0 && (long)(nowMs - lastTickMs) < (long)gSensorLogIntervalMs) return;
  }

  if (forced) sForceSample = false;
  lastTickMs = nowMs;
  const bool bypassR1Dedup = sBypassR1Dedup;
  sBypassR1Dedup = false;

  // Diagnostics counters
  static uint32_t log_writes = 0;
  static uint32_t log_open_fail = 0;
  static uint32_t log_lock_fail = 0;
  static uint32_t log_idle_skips = 0;
  static uint32_t log_trunc = 0;
  static unsigned long lastSummaryMs = 0;
  // Size accounting lives at file scope (gApproxSizeBytes) so start/rollover
  // can seed it from the existing day file's on-disk size.
  static unsigned long lastTruncateMs = 0;
  static uint32_t writeCount = 0;
  const unsigned long truncateCooldownMs = 5000;

  // Local builder - respects sensor selection mask
  auto buildFromSnap = [](const SensorCacheSnapshot& s) -> const char* {
    static char* buf = nullptr;
    if (!buf) {
      buf = (char*)ps_alloc(512, AllocPref::PreferPSRAM, "sensor.log.buf.snap");
      if (!buf) return "";
    }
    char* pos = buf;
    int remaining = 512;
    int written = 0;
    char tsPrefix[48];
    getTimestampPrefixMsCached(tsPrefix, sizeof(tsPrefix));
    if (tsPrefix[0]) written = snprintf(pos, remaining, "%s", tsPrefix);
    else written = snprintf(pos, remaining, "[BOOT ms=%lu] | ", millis());
    pos += written;
    remaining -= written;

    // Thermal (only if enabled in mask)
    if ((gSensorLogMask & LOG_THERMAL) && s.gThermalRunning && s.gThermalConnected && s.thermalValid && remaining > 0) {
      written = snprintf(pos, remaining, "thermal: min=%dC avg=%dC max=%dC | ",
                         (int)s.thermalMin, (int)s.thermalAvg, (int)s.thermalMax);
      pos += written;
      remaining -= written;
    }

    // ToF (only if enabled in mask)
    if ((gSensorLogMask & LOG_TOF) && s.gTofRunning && s.gTofConnected && s.tofValid && remaining > 0) {
      written = snprintf(pos, remaining, "tof: ");
      pos += written;
      remaining -= written;
      for (int i = 0; i < s.tofTotal && i < 4 && remaining > 0; i++) {
        if (s.tof[i].valid) {
          written = snprintf(pos, remaining, "obj%d=%dmm(st=%d) ", i, s.tof[i].distance_mm, s.tof[i].status);
          pos += written;
          remaining -= written;
        }
      }
      if (remaining > 0) {
        written = snprintf(pos, remaining, "| ");
        pos += written;
        remaining -= written;
      }
    }

    // IMU (only if enabled in mask)
    if ((gSensorLogMask & LOG_IMU) && s.gImuRunning && s.gImuConnected && remaining > 0) {
      written = snprintf(pos, remaining, "imu: yaw=%.1f pitch=%.1f roll=%.1f accel=(%.2f,%.2f,%.2f) temp=%.1fC | ",
                         s.yaw, s.pitch, s.roll, s.ax, s.ay, s.az, s.imuTemp);
      pos += written;
      remaining -= written;
    }

    // Input device (only if enabled in mask). The "input:" prefix is generic
    // because the source could be either a gamepad joystick or the ANO encoder's
    // synthesized state — the format is the same shape either way.
    if ((gSensorLogMask & LOG_GAMEPAD) && s.gInputRunning && s.gInputConnected && s.inputValid && remaining > 0) {
      written = snprintf(pos, remaining, "input: x=%d y=%d btns=0x%lX | ",
                         s.joyX, s.joyY, (unsigned long)s.buttons);
      pos += written;
      remaining -= written;
    }

    // APDS (only if enabled in mask)
    if ((gSensorLogMask & LOG_APDS) && s.gApdsConnected && s.apdsValid && remaining > 0) {
      written = snprintf(pos, remaining, "apds: r=%u g=%u b=%u c=%u prox=%u gest=%u | ",
                         s.apdsRed, s.apdsGreen, s.apdsBlue, s.apdsClear, s.apdsProximity, s.apdsGesture);
      pos += written;
      remaining -= written;
    }

    // GPS (only if enabled in mask)
    if ((gSensorLogMask & LOG_GPS) && s.gGpsRunning && s.gGpsConnected && remaining > 0) {
      if (s.gpsFix) {
        written = snprintf(pos, remaining, "gps: lat=%.6f lon=%.6f alt=%.1fm speed=%.1fkn sats=%d q=%d | ",
                           s.gpsLatitude, s.gpsLongitude, s.gpsAltitude, s.gpsSpeed,
                           (int)s.gpsSatellites, (int)s.gpsFixQuality);
      } else {
        written = snprintf(pos, remaining, "gps: no_fix sats=%d q=%d | ",
                           (int)s.gpsSatellites, (int)s.gpsFixQuality);
      }
      pos += written;
      remaining -= written;
    }

    // Presence (only if enabled in mask)
    if ((gSensorLogMask & LOG_PRESENCE) && s.gPresenceRunning && s.gPresenceConnected && remaining > 0) {
      written = snprintf(pos, remaining, "presence: amb=%.1fC pres=%d%s mot=%d%s | ",
                         s.presenceAmbientTemp, (int)s.presenceValue,
                         s.presenceDetected ? "[DET]" : "",
                         (int)s.motionValue,
                         s.motionDetected ? "[DET]" : "");
      pos += written;
      remaining -= written;
    }

    // R1 ring vitals
    if ((gSensorLogMask & LOG_R1) && s.r1Connected && remaining > 0) {
      char hr[12], hrv[12], spo2[12], temp[12], bat[12], wear[8];
      if (s.r1HrValid)      snprintf(hr, sizeof(hr), "%u", (unsigned)s.r1Hr); else strcpy(hr, "--");
      if (s.r1HrvValid)     snprintf(hrv, sizeof(hrv), "%d", (int)s.r1Hrv); else strcpy(hrv, "--");
      if (s.r1Spo2Valid)    snprintf(spo2, sizeof(spo2), "%u", (unsigned)s.r1Spo2); else strcpy(spo2, "--");
      if (s.r1TempValid)    snprintf(temp, sizeof(temp), "%d.%d",
                                    (int)(s.r1TempTenths / 10),
                                    (int)(s.r1TempTenths < 0 ? -(s.r1TempTenths % 10)
                                                             : (s.r1TempTenths % 10)));
      else strcpy(temp, "--");
      if (s.r1BatteryValid) snprintf(bat, sizeof(bat), "%u", (unsigned)s.r1Battery); else strcpy(bat, "--");
      if (s.r1WearValid) {
        if (s.r1Wear == 2) strcpy(wear, "on");
        else if (s.r1Wear == 1) strcpy(wear, "off");
        else strcpy(wear, "?");
      } else strcpy(wear, "--");
      written = snprintf(pos, remaining,
                         "r1: hr=%s hrv=%s spo2=%s temp=%s bat=%s wear=%s | ",
                         hr, hrv, spo2, temp, bat, wear);
      pos += written;
      remaining -= written;
    }

    int len = strlen(buf);
    bool hasSensorData = (len >= 3 && strcmp(buf + len - 3, " | ") == 0);
    if (hasSensorData) buf[len - 3] = '\0';
    else if (remaining > 0) snprintf(pos, remaining, "(no data from selected sensors)");
    return buf;
  };

  // CSV builder for structured data logging
  auto buildCSVFromSnap = [](const SensorCacheSnapshot& s) -> const char* {
    static char* buf = nullptr;
    if (!buf) {
      buf = (char*)ps_alloc(1024, AllocPref::PreferPSRAM, "sensor.log.csv");
      if (!buf) return "";
    }

    // Dual-range stamp, one column: wall-clock epoch-ms when the clock is
    // synced (13+ digits), millis-since-boot otherwise (≤10 digits — the
    // ranges can't collide). Epoch stamps make rows from different
    // sessions/boots continuous inside a per-day file; dark-boot rows stay
    // boot-relative and are retro-dated at file level by System_TimeAnchors.
    // int64 is mandatory — epoch-ms truncates silently in 32-bit.
    const int64_t timestamp =
        Clock::isSynced() ? Clock::epochMillis() : (int64_t)millis();
    char* pos = buf;
    int remaining = 1024;

    int written = snprintf(pos, remaining, "%lld", (long long)timestamp);
    pos += written;
    remaining -= written;

    if ((gSensorLogMask & LOG_THERMAL) && s.thermalValid && remaining > 0) {
      written = snprintf(pos, remaining, ",%d,%d,%d",
                         (int)s.thermalMin, (int)s.thermalMax, (int)s.thermalAvg);
      pos += written;
      remaining -= written;
    }

    if ((gSensorLogMask & LOG_TOF) && s.tofValid && remaining > 0) {
      written = snprintf(pos, remaining, ",%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
                         s.tof[0].distance_mm, s.tof[0].valid ? 1 : 0, s.tof[0].status,
                         s.tof[1].distance_mm, s.tof[1].valid ? 1 : 0, s.tof[1].status,
                         s.tof[2].distance_mm, s.tof[2].valid ? 1 : 0, s.tof[2].status,
                         s.tof[3].distance_mm, s.tof[3].valid ? 1 : 0, s.tof[3].status);
      pos += written;
      remaining -= written;
    }

    if ((gSensorLogMask & LOG_IMU) && remaining > 0) {
      written = snprintf(pos, remaining, ",%.1f,%.1f,%.1f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.1f",
                         s.yaw, s.pitch, s.roll,
                         s.ax, s.ay, s.az,
                         s.gx, s.gy, s.gz,
                         s.imuTemp);
      pos += written;
      remaining -= written;
    }

    if ((gSensorLogMask & LOG_GAMEPAD) && s.inputValid && remaining > 0) {
      written = snprintf(pos, remaining, ",%d,%d,%lu",
                         s.joyX, s.joyY, (unsigned long)s.buttons);
      pos += written;
      remaining -= written;
    }

    if ((gSensorLogMask & LOG_APDS) && s.apdsValid && remaining > 0) {
      written = snprintf(pos, remaining, ",%u,%u,%u,%u,%u,%u",
                         s.apdsRed, s.apdsGreen, s.apdsBlue, s.apdsClear,
                         s.apdsProximity, s.apdsGesture);
      pos += written;
      remaining -= written;
    }

    if ((gSensorLogMask & LOG_GPS) && remaining > 0) {
      written = snprintf(pos, remaining, ",%d,%.6f,%.6f,%.1f,%.1f,%d,%d",
                         s.gpsFix ? 1 : 0, s.gpsLatitude, s.gpsLongitude,
                         s.gpsAltitude, s.gpsSpeed,
                         (int)s.gpsSatellites, (int)s.gpsFixQuality);
      pos += written;
      remaining -= written;
    }

    if ((gSensorLogMask & LOG_PRESENCE) && remaining > 0) {
      written = snprintf(pos, remaining, ",%.1f,%d,%d,%d,%d",
                         s.presenceAmbientTemp, (int)s.presenceValue,
                         s.presenceDetected ? 1 : 0,
                         (int)s.motionValue, s.motionDetected ? 1 : 0);
      pos += written;
      remaining -= written;
    }

    if ((gSensorLogMask & LOG_R1) && remaining > 0) {
      // Empty fields when invalid so CSV stays column-aligned.
      char hr[8] = "", hrv[8] = "", spo2[8] = "", temp[8] = "", bat[8] = "", wear[4] = "";
      if (s.r1HrValid) snprintf(hr, sizeof(hr), "%u", (unsigned)s.r1Hr);
      if (s.r1HrvValid) snprintf(hrv, sizeof(hrv), "%d", (int)s.r1Hrv);
      if (s.r1Spo2Valid) snprintf(spo2, sizeof(spo2), "%u", (unsigned)s.r1Spo2);
      if (s.r1TempValid) snprintf(temp, sizeof(temp), "%d.%d",
                                  (int)(s.r1TempTenths / 10),
                                  (int)(s.r1TempTenths < 0 ? -(s.r1TempTenths % 10)
                                                           : (s.r1TempTenths % 10)));
      if (s.r1BatteryValid) snprintf(bat, sizeof(bat), "%u", (unsigned)s.r1Battery);
      if (s.r1WearValid) snprintf(wear, sizeof(wear), "%u", (unsigned)s.r1Wear);
      written = snprintf(pos, remaining, ",%d,%s,%s,%s,%s,%s,%s",
                         s.r1Connected ? 1 : 0, hr, hrv, spo2, temp, bat, wear);
      pos += written;
      remaining -= written;
    }

    return buf;
  };

  // Track format state (signal loss dedup) — must be static to persist across ticks
  static uint32_t trackSignalLostCount = 0;
  static bool trackWasConnected = false;

  // Track format builder — GPS-only compact: time,lat,lon,alt,speed,sats
  auto buildTrackFromSnap = [](const SensorCacheSnapshot& s) -> const char* {
    EXT_RAM_BSS_ATTR static char buf[128];
    
    // Format timestamp from GPS time or millis fallback
    char ts[12];
    if (s.gpsHasTime) {
      snprintf(ts, sizeof(ts), "%02d:%02d:%02d", s.gpsHour, s.gpsMinute, s.gpsSecond);
    } else {
      unsigned long secs = millis() / 1000;
      snprintf(ts, sizeof(ts), "%02lu:%02lu:%02lu",
               (secs / 3600) % 24, (secs / 60) % 60, secs % 60);
    }

    if (!s.gpsFix) {
      trackSignalLostCount++;
      if (trackSignalLostCount == 1 && trackWasConnected) {
        snprintf(buf, sizeof(buf), "%s,---,SIGNAL_LOST", ts);
        trackWasConnected = false;
        return buf;
      }
      trackWasConnected = false;
      return nullptr;  // suppress duplicate signal-loss lines
    }

    // Signal regained after loss
    if (trackSignalLostCount > 0) {
      if (trackSignalLostCount > 1) {
        // Emit regained line first — caller will write this, then the data line on next tick
        snprintf(buf, sizeof(buf), "%s,~~~,SIGNAL_REGAINED (lost %lu intervals)",
                 ts, (unsigned long)trackSignalLostCount);
        trackSignalLostCount = 0;
        trackWasConnected = true;
        return buf;
      }
      trackSignalLostCount = 0;
    }
    trackWasConnected = true;

    // NOTE: Live track (GPSTrackManager::appendPoint) is fed directly by
    // gpsTask in i2csensor_pa1010d.cpp, independent of sensor logging.

    snprintf(buf, sizeof(buf), "%s,%.6f,%.6f,%.1f,%.1f,%d",
             ts, s.gpsLatitude, s.gpsLongitude, s.gpsAltitude, s.gpsSpeed, (int)s.gpsSatellites);
    return buf;
  };

    SensorCacheSnapshot snap = {};
    const uint8_t mask = gSensorLogMask;
    
    // Only lock caches for sensors selected in the mask
    if (mask & LOG_THERMAL) {
      if (lockThermalCache(pdMS_TO_TICKS(10))) {
        snap.gThermalRunning = gThermalRunning;
        snap.gThermalConnected = gThermalConnected;
        snap.thermalValid = gThermalCache.thermalDataValid;
        snap.thermalMin = gThermalCache.thermalMinTemp;
        snap.thermalAvg = gThermalCache.thermalAvgTemp;
        snap.thermalMax = gThermalCache.thermalMaxTemp;
        unlockThermalCache();
      }
    }
    
#if ENABLE_TOF_SENSOR
    if (mask & LOG_TOF) {
      SensorCacheGuard g(gTofCache.mutex, pdMS_TO_TICKS(10), "sensorLog.tofSnapshot");
      if (g.held) {
        snap.gTofRunning = gTofRunning;
        snap.gTofConnected = gTofConnected;
        snap.tofValid = gTofCache.tofDataValid;
        snap.tofTotal = gTofCache.tofTotalObjects;
        for (int i = 0; i < 4; i++) {
          snap.tof[i].valid = gTofCache.tofObjects[i].valid;
          snap.tof[i].detected = gTofCache.tofObjects[i].detected;
          snap.tof[i].distance_mm = gTofCache.tofObjects[i].distance_mm;
          snap.tof[i].status = gTofCache.tofObjects[i].status;
        }
      }
    }
#endif

#if ENABLE_IMU_SENSOR
    if (mask & LOG_IMU) {
      SensorCacheGuard g(gImuCache.mutex, pdMS_TO_TICKS(10), "sensorLog.imuSnapshot");
      if (g.held) {
        snap.gImuRunning = gImuRunning;
        snap.gImuConnected = gImuConnected;
        snap.yaw = gImuCache.oriYaw;
        snap.pitch = gImuCache.oriPitch;
        snap.roll = gImuCache.oriRoll;
        snap.ax = gImuCache.accelX;
        snap.ay = gImuCache.accelY;
        snap.az = gImuCache.accelZ;
        snap.gx = gImuCache.gyroX;
        snap.gy = gImuCache.gyroY;
        snap.gz = gImuCache.gyroZ;
        snap.imuTemp = gImuCache.imuTemp;
      }
    }
#endif

#if ENABLE_OLED_INPUT  // gInputCache populated by either gamepad or ANO driver
    if (mask & LOG_GAMEPAD) {
      SensorCacheGuard g(gInputCache.mutex, pdMS_TO_TICKS(10), "sensorLog.inputSnapshot");
      if (g.held) {
        snap.gInputRunning = gInputRunning;
        snap.gInputConnected = gInputConnected;
        snap.inputValid = gInputCache.dataValid;
        // Stored in the cache's native bit layout (per-device, active-low).
        // Replay code that crosses devices should pass this through
        // inputButtonsToLogical() — buttons field is the device-native form.
        snap.buttons = gInputCache.buttons;
        snap.joyX = gInputCache.joyX;
        snap.joyY = gInputCache.joyY;
      }
    }
#endif

#if ENABLE_APDS_SENSOR
    if (mask & LOG_APDS) {
      SensorCacheGuard g(gApdsCache.mutex, pdMS_TO_TICKS(10), "sensorLog.apdsSnapshot");
      if (g.held) {
        snap.gApdsColorRunning = gApdsColorRunning;
        snap.gApdsProximityRunning = gApdsProximityRunning;
        snap.gApdsGestureRunning = gApdsGestureRunning;
        snap.gApdsConnected = gApdsConnected;
        snap.apdsValid = gApdsCache.apdsDataValid;
        snap.apdsRed = gApdsCache.apdsRed;
        snap.apdsGreen = gApdsCache.apdsGreen;
        snap.apdsBlue = gApdsCache.apdsBlue;
        snap.apdsClear = gApdsCache.apdsClear;
        snap.apdsProximity = gApdsCache.apdsProximity;
        snap.apdsGesture = gApdsCache.apdsGesture;
      }
    }
#endif

#if ENABLE_GPS_SENSOR
    if (mask & LOG_GPS) {
      snap.gGpsRunning = gGpsRunning;
      snap.gGpsConnected = gGpsConnected;
      SensorCacheGuard g(gGpsCache.mutex, pdMS_TO_TICKS(10), "sensorLog.gpsSnapshot");
      if (g.held) {
        if (gGpsCache.dataValid && gGpsCache.hasFix) {
          snap.gpsFix = true;
          snap.gpsLatitude = gGpsCache.latitude;
          snap.gpsLongitude = gGpsCache.longitude;
          snap.gpsAltitude = gGpsCache.altitude;
          snap.gpsSpeed = gGpsCache.speed;
          snap.gpsSatellites = gGpsCache.satellites;
          snap.gpsFixQuality = gGpsCache.fixQuality;
          snap.gpsHour = gGpsCache.hour;
          snap.gpsMinute = gGpsCache.minute;
          snap.gpsSecond = gGpsCache.second;
          snap.gpsHasTime = true;
        } else {
          snap.gpsFix = false;
          snap.gpsHasTime = false;
        }
      }
    }
#endif

#if ENABLE_PRESENCE_SENSOR
    if (mask & LOG_PRESENCE) {
      snap.gPresenceRunning = gPresenceRunning;
      snap.gPresenceConnected = gPresenceConnected;
      SensorCacheGuard g(gPresenceCache.mutex, pdMS_TO_TICKS(10), "sensorLog.presenceSnapshot");
      if (g.held) {
        snap.presenceAmbientTemp = gPresenceCache.ambientTemp;
        snap.presenceValue = gPresenceCache.presenceValue;
        snap.motionValue = gPresenceCache.motionValue;
        snap.presenceDetected = gPresenceCache.presenceDetected;
        snap.motionDetected = gPresenceCache.motionDetected;
      }
    }
#endif

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
    if (mask & LOG_R1) {
      // Keep the ring cache warm while logging even if Health page is closed.
      // Skip while Health page owns a poll burst — racing two cursors doubles
      // TX and makes Poll Now look flaky.
#if ENABLE_R1_HEALTH
      if (!g2HealthPageIsActive())
#endif
        g2RingPollVitalForLogging();
      G2RingTelemetry t;
      g2RingGetTelemetry(t);
      snap.r1Connected    = t.connected;
      snap.r1HrValid      = t.hrValid;
      snap.r1HrvValid     = t.hrvValid;
      snap.r1Spo2Valid    = t.spo2Valid;
      snap.r1TempValid    = t.tempValid;
      snap.r1BatteryValid = t.batteryValid;
      snap.r1WearValid    = t.wearValid;
      snap.r1Hr           = t.hr;
      snap.r1Hrv          = t.hrv;
      snap.r1Spo2         = t.spo2;
      snap.r1TempTenths   = t.tempTenths;
      snap.r1Battery      = t.battery;
      snap.r1Wear         = t.wear;
    }
#endif

    // Check if any selected sensor has active data
    bool hasSelectedData = false;
    if ((gSensorLogMask & LOG_THERMAL) && snap.gThermalRunning && snap.gThermalConnected) hasSelectedData = true;
    if ((gSensorLogMask & LOG_TOF) && snap.gTofRunning && snap.gTofConnected) hasSelectedData = true;
    if ((gSensorLogMask & LOG_IMU) && snap.gImuRunning && snap.gImuConnected) hasSelectedData = true;
    if ((gSensorLogMask & LOG_GAMEPAD) && snap.gInputRunning && snap.gInputConnected) hasSelectedData = true;
    if ((gSensorLogMask & LOG_APDS) && snap.gApdsConnected) hasSelectedData = true;
    if ((gSensorLogMask & LOG_GPS) && snap.gGpsRunning && snap.gGpsConnected) hasSelectedData = true;
    if ((gSensorLogMask & LOG_PRESENCE) && snap.gPresenceRunning && snap.gPresenceConnected) hasSelectedData = true;
    if ((gSensorLogMask & LOG_R1) && snap.r1Connected) hasSelectedData = true;

    // Health Track R1-only: never write empty timestamp rows (ring down /
    // no cache yet). Forced mine/Poll samples only land when connected.
    if (healthOwnedR1Only && !hasSelectedData) return;

    // R1-only change-dedup: ring points update on minute scale — skip identical
    // rows when LOG_R1 is the sole selected sensor (or the only one with data).
    // Forced samples from Health Track mines / page refresh bypass this.
    static uint8_t  lastR1Hr = 0, lastR1Spo2 = 0, lastR1Bat = 0, lastR1Wear = 0;
    static int16_t  lastR1Hrv = 0, lastR1Temp = 0;
    static uint8_t  lastR1Flags = 0;
    if ((gSensorLogMask & LOG_R1) && snap.r1Connected) {
      const uint8_t flags = (snap.r1HrValid ? 1 : 0) | (snap.r1HrvValid ? 2 : 0) |
                            (snap.r1Spo2Valid ? 4 : 0) | (snap.r1BatteryValid ? 8 : 0) |
                            (snap.r1TempValid ? 16 : 0) | (snap.r1WearValid ? 32 : 0);
      const bool r1Changed = (flags != lastR1Flags) ||
                             (snap.r1HrValid && snap.r1Hr != lastR1Hr) ||
                             (snap.r1HrvValid && snap.r1Hrv != lastR1Hrv) ||
                             (snap.r1Spo2Valid && snap.r1Spo2 != lastR1Spo2) ||
                             (snap.r1TempValid && snap.r1TempTenths != lastR1Temp) ||
                             (snap.r1BatteryValid && snap.r1Battery != lastR1Bat) ||
                             (snap.r1WearValid && snap.r1Wear != lastR1Wear);
      const uint8_t others = (uint8_t)(gSensorLogMask & (uint8_t)~LOG_R1);
      if (!bypassR1Dedup && !r1Changed && others == 0) {
        return;  // nothing new to write
      }
      if (r1Changed || bypassR1Dedup) {
        lastR1Flags = flags;
        lastR1Hr = snap.r1Hr; lastR1Hrv = snap.r1Hrv;
        lastR1Spo2 = snap.r1Spo2; lastR1Temp = snap.r1TempTenths;
        lastR1Bat = snap.r1Battery; lastR1Wear = snap.r1Wear;
      }
    }

    // CSV never writes no-data heartbeat rows: a bare-timestamp row is junk
    // to any grapher and would accumulate at 5 s cadence across a whole
    // per-day file; epoch-ms stamps make real gaps self-describing. TEXT
    // keeps its heartbeat lines, and TRACK MUST keep them — the one-shot
    // SIGNAL_LOST marker is emitted via this path when GPS disconnects
    // (hasSelectedData goes false, so buildTrackFromSnap is only reached
    // through the heartbeat allowance).
    if (!hasSelectedData && gSensorLogFormat == SENSOR_LOG_CSV) {
      log_idle_skips++;
      return;
    }

    // Suppress idle lines when no selected sensor has data
    static unsigned long lastHeartbeatMs = 0;
    const unsigned long heartbeatMs = 5000;
    if (!hasSelectedData) {
      if (lastHeartbeatMs != 0 && (long)(nowMs - lastHeartbeatMs) < (long)heartbeatMs) {
        log_idle_skips++;
        if (isDebugFlagSet(DEBUG_LOGGER)) {
          DEBUG_LOGGERF("logger: idle skip #%u (dt=%lums)", (unsigned)log_idle_skips, (nowMs - lastHeartbeatMs));
        }
        return;
      }
      lastHeartbeatMs = nowMs;
      if (isDebugFlagSet(DEBUG_LOGGER)) {
        DEBUG_LOGGERF("logger: heartbeat at %lu ms", nowMs);
      }
    } else {
      lastHeartbeatMs = nowMs;
    }

    // Choose format
    const char* line = nullptr;
    if (gSensorLogFormat == SENSOR_LOG_TRACK) {
      line = buildTrackFromSnap(snap);
    } else if (gSensorLogFormat == SENSOR_LOG_CSV) {
      line = buildCSVFromSnap(snap);
    } else {
      line = buildFromSnap(snap);
    }
    if (line && line[0] != '\0') {
      // RAII lock: later branches in this section (rollover, header-on-create)
      // return/skip early — a manual fsLock/fsUnlock pair here would leak the
      // FS mutex on any early exit and deadlock all filesystem access. The
      // nested VFS::*Guarded calls are reentrancy-safe under this guard.
      // Helpers called inside this scope must never call raw fsUnlock().
      FsLockGuard appendGuard("sensorlog.append");

      // Quiet rollover — re-point BEFORE this sample lands so no row is ever
      // written into the wrong day (or into the boot file after the clock
      // synced). MANUAL (literal-path) sessions never roll. DAY sessions roll
      // at midnight; BOOT_SHAPED sessions roll the moment the clock syncs —
      // pre-sync rows stay behind in the boot file (no longer active, so the
      // time-anchor sweep retro-dates it) and day files stay pure epoch-ms.
      // One events.log line for auditability; deliberately NO SYSEVT, no
      // notification, no settings write — a nightly SYSEVT_SENSOR_STARTED
      // would spam the notification/automation pipeline.
      if (gSensorLogFormat == SENSOR_LOG_CSV && Clock::isSynced() &&
          gSensorLogPathMode != SensorLogPathMode::MANUAL) {
        char today[11];
        time_t nowT = time(nullptr);
        struct tm lt;
        localtime_r(&nowT, &lt);
        strftime(today, sizeof(today), "%Y-%m-%d", &lt);
        const bool roll =
            (gSensorLogPathMode == SensorLogPathMode::BOOT_SHAPED) ||
            (strcmp(today, gSensorLogDayStr) != 0);
        if (roll) {
          String target =
              resolveSessionTarget(shapeSessionPath(stripSessionShaping(gSensorLogPath)));
          gSensorLogPath = target;
          sensorLogClassifyActivePath(target.c_str());
          gApproxSizeBytes = sensorLogResolvedSize(target);
          logSystemEvent("SENSOR", "sensor-log rolled to %s", target.c_str());
        }
      }

      // Route to LittleFS primary, or /sd overflow mirror if flash is full.
      // The active path for this write cycle is resolved per-write so a hot
      // card plug-in can start working immediately, but the overflow latch
      // itself is one-way per reboot (see VFS::resolveOverflowPath).
      char activePath[128];
      bool onSd = VFS::resolveOverflowPath(gSensorLogPath.c_str(),
                                           (size_t)gSensorLogMaxSize,
                                           activePath, sizeof(activePath));
      File f = VFS::openGuarded(String(activePath), "a", VFS::systemAuth("senlog.append"), true);
      if (f) {
        // Header-on-create: a fresh/empty file at the ACTIVE tier gets its
        // header before the first row. One check covers three holes: the
        // post-rotation recreated base file, a mid-session SD-overflow flip
        // creating the /sd mirror, and rollover targets created right here.
        // TEXT is headerless by design (writeHeaderChecked no-ops).
        if (f.size() == 0 && !writeHeaderChecked(f)) {
          f.close();
          log_open_fail++;
          if (isDebugFlagSet(DEBUG_LOGGER)) {
            DEBUG_LOGGERF("logger: header write failed on %s", activePath);
          }
          return;
        }
        size_t len = strlen(line);
        f.write((const uint8_t*)line, len);
        f.write('\n');
        f.close();
        gSensorLogLastWrite = millis();
        writeCount++;
        log_writes++;
        gApproxSizeBytes += (len + 1);

        // Handle log rotation. Rotation runs on whichever tier we're writing
        // to. When overflow is active we rotate inside `/sd/...` and the
        // LittleFS copies are preserved (never touched again).
        if (gApproxSizeBytes > gSensorLogMaxSize && (lastTruncateMs == 0 || (long)(millis() - lastTruncateMs) >= (long)truncateCooldownMs)) {
          lastTruncateMs = millis();
          writeCount = 0;

          if (gSensorLogMaxRotations > 0) {
            if (gSensorLogMaxRotations > 1) {
              char oldestFile[128];
              snprintf(oldestFile, sizeof(oldestFile), "%s.%d", activePath, gSensorLogMaxRotations);
              if (VFS::existsGuarded(String(oldestFile), VFS::systemAuth("senlog.rotate"))) {
                VFS::removeGuarded(String(oldestFile), VFS::systemAuth("senlog.rotate"));
              }
            }

            // Shift existing rotations up: .i -> .(i+1) for i = N-1..1. The base
            // file is moved to .1 separately AFTER this loop, so the loop must
            // operate on the numbered files only — never on the base path.
            for (int i = gSensorLogMaxRotations - 1; i >= 1; i--) {
              char fromFile[128], toFile[128];
              snprintf(fromFile, sizeof(fromFile), "%s.%d", activePath, i);
              snprintf(toFile, sizeof(toFile), "%s.%d", activePath, i + 1);
              if (VFS::existsGuarded(String(fromFile), VFS::systemAuth("senlog.rotate"))) {
                VFS::renameGuarded(String(fromFile), String(toFile), VFS::systemAuth("senlog.rotate"));
              }
            }

            if (VFS::existsGuarded(String(activePath), VFS::systemAuth("senlog.rotate"))) {
              String rotatedFile = String(activePath) + ".1";
              VFS::renameGuarded(String(activePath), rotatedFile, VFS::systemAuth("senlog.rotate"));
            }
          } else {
            VFS::removeGuarded(String(activePath), VFS::systemAuth("senlog.rotate"));
          }

          gApproxSizeBytes = 0;
          log_trunc++;
          if (isDebugFlagSet(DEBUG_STORAGE)) {
            DEBUGF_BROADCAST(DEBUG_STORAGE, "Sensor log: rotated file on %s (max size=%u bytes)",
                             onSd ? "SD" : "LittleFS", (unsigned)gSensorLogMaxSize);
          }
          if (isDebugFlagSet(DEBUG_LOGGER)) {
            DEBUG_LOGGERF("logger: rotated at approxSize=%u tier=%s",
                          (unsigned)gSensorLogMaxSize, onSd ? "sd" : "little");
          }
        }
        
        if (isDebugFlagSet(DEBUG_STORAGE)) {
          DEBUGF_BROADCAST(DEBUG_STORAGE, "Sensor log: wrote %d bytes", (int)len);
        }
        if (isDebugFlagSet(DEBUG_LOGGER)) {
          DEBUG_LOGGERF("logger: wrote %dB, approxSize=%uB, writes=%u", (int)len, (unsigned)gApproxSizeBytes, (unsigned)log_writes);
        }
      } else {
        if (isDebugFlagSet(DEBUG_STORAGE)) {
          DEBUGF_BROADCAST(DEBUG_STORAGE, "Sensor log: failed to open file");
        }
        log_open_fail++;
        if (isDebugFlagSet(DEBUG_LOGGER)) {
          DEBUG_LOGGERF("logger: open fail #%u", (unsigned)log_open_fail);
        }
      }
    }

    // Periodic summary
    unsigned long now2 = millis();
    if (isDebugFlagSet(DEBUG_LOGGER) && (lastSummaryMs == 0 || (long)(now2 - lastSummaryMs) >= 5000)) {
      lastSummaryMs = now2;
      DEBUG_LOGGERF("logger: summary | writes=%u open_fail=%u lock_fail=%u idle_skips=%u trunc=%u",
                    (unsigned)log_writes, (unsigned)log_open_fail, (unsigned)log_lock_fail,
                    (unsigned)log_idle_skips, (unsigned)log_trunc);
    }
}

// ============================================================================
// Command Handler
// ============================================================================

const char* cmd_sensorlog(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  CommandArgs a(argsInput);
  if (a.count() == 0) {
    return "Error: invalid arguments — Usage: sensorlog <start|stop|status|format|maxsize|rotations|sensors|autostart> [args...]\n"
           "  start <filepath> [interval_ms]: Begin logging (default 5000ms)\n"
           "  stop: Stop logging\n"
           "  status: Show current logging status\n"
           "  format <text|csv|track>: Set log format (default: text)\n"
           "    track = GPS-only compact format with signal loss dedup\n"
           "  maxsize <bytes>: Set max file size before rotation (default: 256000)\n"
           "  rotations <count>: Set number of old logs to keep (0-9, default: 3)\n"
           "  sensors <thermal|tof|imu|gamepad|apds|gps|presence|r1|all|none>: Select sensors to log\n"
           "  autostart [on|off]: Auto-start logging on boot with last-used parameters";
  }
  String subCmd = a.arg(0);
  subCmd.toLowerCase();

  // Handle 'status' subcommand
  if (subCmd == "status") {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    // Build sensors string
    String sensors = "";
    if (gSensorLogMask & LOG_THERMAL) sensors += "thermal ";
    if (gSensorLogMask & LOG_TOF) sensors += "tof ";
    if (gSensorLogMask & LOG_IMU) sensors += "imu ";
    if (gSensorLogMask & LOG_GAMEPAD) sensors += "gamepad ";
    if (gSensorLogMask & LOG_APDS) sensors += "apds ";
    if (gSensorLogMask & LOG_GPS) sensors += "gps ";
    if (gSensorLogMask & LOG_PRESENCE) sensors += "presence ";
    if (gSensorLogMask & LOG_R1) sensors += "r1 ";
    if (sensors.length() == 0) sensors = "(none)";
    
    const char* fmtName = (gSensorLogFormat == SENSOR_LOG_CSV) ? "CSV" :
                           (gSensorLogFormat == SENSOR_LOG_TRACK) ? "TRACK" : "TEXT";
    char* buf = getDebugBuffer();
    if (gSensorLoggingRunning) {
      snprintf(buf, 1024,
        "Sensor logging ACTIVE\n"
        "  File: %s\n"
        "  Interval: %lums\n"
        "  Format: %s\n"
        "  Max size: %u bytes\n"
        "  Rotations: %u\n"
        "  Sensors: %s\n"
        "  Auto-start: %s\n"
        "  Last write: %lus ago",
        gSensorLogPath.c_str(),
        (unsigned long)gSensorLogIntervalMs,
        fmtName,
        (unsigned)gSensorLogMaxSize,
        (unsigned)gSensorLogMaxRotations,
        sensors.c_str(),
        gSettings.sensorLogAutoStart ? "ON" : "OFF",
        (millis() - gSensorLogLastWrite) / 1000);
    } else {
      snprintf(buf, 1024,
        "Sensor logging is INACTIVE\n"
        "  Format: %s\n"
        "  Max size: %u bytes\n"
        "  Rotations: %u\n"
        "  Sensors: %s\n"
        "  Auto-start: %s",
        fmtName,
        (unsigned)gSensorLogMaxSize,
        (unsigned)gSensorLogMaxRotations,
        sensors.c_str(),
        gSettings.sensorLogAutoStart ? "ON" : "OFF");
    }
    return buf;
  }

  // Handle 'stop' subcommand
  if (subCmd == "stop") {
    if (!gSensorLoggingRunning) {
      return "Error: Sensor logging is not running";
    }
    gSensorLoggingRunning = false;
    systemEventPost(SYSEVT_SENSOR_STOPPED, "Logging");
    broadcastOutput("Sensor logging stop requested; will stop safely");
    return "SUCCESS: Sensor logging stop requested; will stop safely";
  }

  // Handle 'start' subcommand
  if (subCmd == "start") {
    // Master switch. Checked here rather than at each caller because sensor
    // logging is startable from several places that are not obviously "start
    // logging" - notably the OLED Map screen, which begins a GPS track log as a
    // side effect of opening it. Without this, "never write logs on this device"
    // was not expressible.
    if (!gSettings.sensorLogEnabled) {
      return "Error: sensor logging is disabled - run 'sensorlogenabled 1' first";
    }
    if (gSensorLoggingRunning) {
      return "Error: Sensor logging already running. Use 'sensorlog stop' first.";
    }

    if (!a.has(1)) {
      return "Error: invalid arguments — Usage: sensorlog start <filepath> [interval_ms]\n"
             "Example: sensorlog start /logging_captures/sensors/sensors.txt 1000";
    }

    String filepath = a.arg(1);
    uint32_t interval = gSensorLogIntervalMs;

    if (a.has(2)) {
      interval = a.argInt(2, (int)gSensorLogIntervalMs);
      if (interval < 100) interval = 100;
      if (interval > 3600000) interval = 3600000;
    }

    if (filepath.length() == 0 || filepath.charAt(0) != '/') {
      return "Error: Filepath must start with / (e.g., /logging_captures/sensors/sensors.txt)";
    }

    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

    // Ensure directory exists (mkdir is non-recursive, create parents first)
    int lastSlash = filepath.lastIndexOf('/');
    if (lastSlash > 0) {
      String dir = filepath.substring(0, lastSlash);
      if (!VFS::existsGuarded(dir, VFS::systemAuth("senlog.setup_mkdir"))) {
        // Create parent directories iteratively via VFS so the sensor log's
        // write path and directory creation use the same dispatcher.
        for (int i = 1; i <= (int)dir.length(); i++) {
          if (i == (int)dir.length() || dir.charAt(i) == '/') {
            String parent = dir.substring(0, i);
            if (parent.length() > 0 && !VFS::existsGuarded(parent, VFS::systemAuth("senlog.setup_mkdir"))) {
              VFS::mkdirGuarded(parent, VFS::systemAuth("senlog.setup_mkdir"));
            }
          }
        }
        if (!VFS::existsGuarded(dir, VFS::systemAuth("senlog.setup_mkdir"))) {
          snprintf(getDebugBuffer(), 1024, "Error: Failed to create directory: %s", dir.c_str());
          return getDebugBuffer();
        }
        broadcastOutput("Created directory: " + dir);
      }
    }

    // Per-day append: pick the compatible target. Reuses an existing day
    // file only when its header matches the current mask; a mismatch mints a
    // -2..-9 variant (timestamped fallback on exhaustion). No-op for
    // TEXT/TRACK and for non-day paths. See docs/SENSORLOG_PERDAY_APPEND_PLAN.md.
    filepath = resolveSessionTarget(filepath);

    // Create file if needed. Variants/fallbacks stay in the same directory,
    // so the parent-mkdir walk above already covers them. The append path
    // additionally header-on-creates empty files (post-rotation base, /sd
    // overflow mirror), so a file skipped here is still safe.
    if (!VFS::existsGuarded(filepath, VFS::systemAuth("senlog.setup_create"))) {
      File f = VFS::openGuarded(filepath, "w", VFS::systemAuth("senlog.setup_create"), true);
      if (!f) {
        snprintf(getDebugBuffer(), 1024, "Error: Failed to create file: %s", filepath.c_str());
        return getDebugBuffer();
      }
      if (!writeHeaderChecked(f)) {
        // Short write = truncated header that would poison every later
        // day-file compatibility compare — remove the husk and fail loudly.
        f.close();
        VFS::removeGuarded(filepath, VFS::systemAuth("senlog.setup_create"));
        snprintf(getDebugBuffer(), 1024, "Error: header write failed for %s (flash full?)", filepath.c_str());
        return getDebugBuffer();
      }
      f.close();
      broadcastOutput("Created log file: " + filepath);
    }

    if (ESP.getFreeHeap() < 8192) {
      return "Error: Insufficient memory (need 8KB free)";
    }

    // Check filesystem space — need at least gSensorLogMaxSize free on
    // LittleFS OR SD card (if mounted) to allow start. Writes will route
    // transparently to whichever tier has room via the overflow system.
    {
      extern void fsLock(const char* tag);
      extern void fsUnlock();
      fsLock("sensorlog.spacecheck");
      size_t totalBytes = LittleFS.totalBytes();
      size_t usedBytes = LittleFS.usedBytes();
      fsUnlock();
      size_t freeBytes = (totalBytes > usedBytes) ? (totalBytes - usedBytes) : 0;
      bool littleFsOk = (freeBytes >= gSensorLogMaxSize);
      bool sdOk = false;
      if (!littleFsOk && VFS::isSDAvailable()) {
        uint64_t sdTotal = 0, sdUsed = 0, sdFree = 0;
        if (VFS::getStats(VFS::SDCARD, sdTotal, sdUsed, sdFree)) {
          sdOk = (sdFree >= (uint64_t)gSensorLogMaxSize);
        }
      }
      if (!littleFsOk && !sdOk) {
        char errMsg[120];
        snprintf(errMsg, sizeof(errMsg),
                 "Not enough space for log (need %uKB, flash has %uKB, no usable SD overflow)",
                 (unsigned)(gSensorLogMaxSize / 1024), (unsigned)(freeBytes / 1024));
        systemEventPost(SYSEVT_SENSOR_START_FAILED, "Logging");
        logSystemEvent("SENSOR", "Logging start FAILED");
        snprintf(getDebugBuffer(), 1024, "Error: %s", errMsg);
        return getDebugBuffer();
      }
    }

    gSensorLogPath = filepath;
    gSensorLogIntervalMs = interval;
    gSensorLoggingRunning = true;
    gSensorLogLastWrite = millis();
    // Day/boot/manual mode + honest rotation accounting for the (possibly
    // pre-existing, already-large) day file we're appending to.
    sensorLogClassifyActivePath(filepath.c_str());
    gApproxSizeBytes = sensorLogResolvedSize(filepath);

    // Persist last-used parameters for auto-start. The path persists as the
    // UN-shaped base (strip the session subfolder/stamp/variant): autostart
    // and healthtrack re-shape fresh each session, and persisting a shaped
    // path would compound variants / nest day folders. For manual literal
    // paths the strip is a no-op, so they persist exactly as typed.
    setSetting(gSettings.sensorLogPath, stripSessionShaping(filepath));
    setSetting(gSettings.sensorLogIntervalMs, (int)interval);
    setSetting(gSettings.sensorLogMask, (int)gSensorLogMask);
    setSetting(gSettings.sensorLogFormat, (int)gSensorLogFormat);

    systemEventPost(SYSEVT_SENSOR_STARTED, "Logging");
    logSystemEvent("SENSOR", "Logging online");
    snprintf(getDebugBuffer(), 1024, "SUCCESS: Sensor logging started\n  File: %s\n  Interval: %lums",
             filepath.c_str(), (unsigned long)interval);
    broadcastOutput(getDebugBuffer());
    cliHintf("readings are written to the file, not to this output - read them back with 'fileview \"%s\"', and stop logging with 'sensorlog stop'", filepath.c_str());
    return getDebugBuffer();
  }

  // Handle 'format' subcommand
  if (subCmd == "format") {
    if (!a.has(1)) {
      const char* fmtName = (gSensorLogFormat == SENSOR_LOG_CSV) ? "csv" :
                             (gSensorLogFormat == SENSOR_LOG_TRACK) ? "track" : "text";
      snprintf(getDebugBuffer(), 1024, "Current format: %s\nUsage: sensorlog format <text|csv|track>\n"
               "  text: Human-readable sensor data\n"
               "  csv: Structured CSV data\n"
               "  track: GPS-only compact track (time,lat,lon,alt,speed,sats) with signal loss dedup",
               fmtName);
      return getDebugBuffer();
    }

    String formatType = a.arg(1);
    formatType.toLowerCase();

    if (formatType == "csv") {
      gSensorLogFormat = SENSOR_LOG_CSV;
      setSetting(gSettings.sensorLogFormat, (int)SENSOR_LOG_CSV);
      return "Log format set to CSV (applies to next 'sensorlog start')";
    } else if (formatType == "text") {
      gSensorLogFormat = SENSOR_LOG_TEXT;
      setSetting(gSettings.sensorLogFormat, (int)SENSOR_LOG_TEXT);
      return "Log format set to TEXT (applies to next 'sensorlog start')";
    } else if (formatType == "track") {
      gSensorLogFormat = SENSOR_LOG_TRACK;
      gSensorLogMask = LOG_GPS;  // Track format is GPS-only
      setSetting(gSettings.sensorLogFormat, (int)SENSOR_LOG_TRACK);
      setSetting(gSettings.sensorLogMask, (int)gSensorLogMask);
      return "Log format set to TRACK (GPS-only, applies to next 'sensorlog start')";
    } else {
      return "Error: Format must be 'text', 'csv', or 'track'";
    }
  }

  // Handle 'maxsize' subcommand
  if (subCmd == "maxsize") {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    if (!a.has(1)) {
      snprintf(getDebugBuffer(), 1024, "Current max size: %u bytes\nUsage: sensorlog maxsize <bytes>",
               (unsigned)gSensorLogMaxSize);
      return getDebugBuffer();
    }

    size_t newSize = a.argInt(1, 0);

    if (newSize < 10240) return "Error: Max size must be at least 10240 bytes (10KB)";
    if (newSize > 10485760) return "Error: Max size cannot exceed 10485760 bytes (10MB)";

    gSensorLogMaxSize = newSize;
    setSetting(gSettings.sensorLogMaxSize, (int)newSize);
    snprintf(getDebugBuffer(), 1024, "Max log size set to %u bytes (applies to active logging)",
             (unsigned)gSensorLogMaxSize);
    return getDebugBuffer();
  }

  // Handle 'rotations' subcommand
  if (subCmd == "rotations") {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    if (!a.has(1)) {
      snprintf(getDebugBuffer(), 1024, "Current rotations: %u\nUsage: sensorlog rotations <count>\n"
                                   "Set to 0 to disable rotation (delete old logs)",
               (unsigned)gSensorLogMaxRotations);
      return getDebugBuffer();
    }

    int count = a.argInt(1, 0);

    if (count < 0 || count > 9) return "Error: Rotation count must be 0-9";

    gSensorLogMaxRotations = (uint8_t)count;
    setSetting(gSettings.sensorLogMaxRotations, count);
    if (count == 0) {
      return "Rotation disabled - old logs will be deleted";
    } else {
      snprintf(getDebugBuffer(), 1024, "Will keep up to %u old log file%s (.1, .2, etc.)",
               count, count > 1 ? "s" : "");
      return getDebugBuffer();
    }
  }

  // Handle 'sensors' subcommand
  if (subCmd == "sensors") {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    if (!a.has(1)) {
      char* p = getDebugBuffer();
      size_t remaining = 1024;
      int n = snprintf(p, remaining, "Selected sensors:\n");
      p += n; remaining -= n;

      n = snprintf(p, remaining, "  %s Thermal\n", (gSensorLogMask & LOG_THERMAL) ? "☑" : "☐");
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  %s ToF\n", (gSensorLogMask & LOG_TOF) ? "☑" : "☐");
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  %s IMU\n", (gSensorLogMask & LOG_IMU) ? "☑" : "☐");
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  %s Gamepad\n", (gSensorLogMask & LOG_GAMEPAD) ? "☑" : "☐");
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  %s APDS\n", (gSensorLogMask & LOG_APDS) ? "☑" : "☐");
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  %s GPS\n", (gSensorLogMask & LOG_GPS) ? "☑" : "☐");
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  %s Presence\n", (gSensorLogMask & LOG_PRESENCE) ? "☑" : "☐");
      p += n; remaining -= n;
      n = snprintf(p, remaining, "  %s R1 Health\n", (gSensorLogMask & LOG_R1) ? "☑" : "☐");
      p += n; remaining -= n;
      snprintf(p, remaining, "\nUsage: sensorlog sensors <thermal|tof|imu|gamepad|apds|gps|presence|r1|all|none>");
      return getDebugBuffer();
    }

    // Mutating form. Refuse while a CSV session runs: this path changes the
    // column set live with no restart, instantly misaligning every later row
    // against the file's header and bypassing the day-file compat guard.
    // (Refusal over auto-restart: the restart path can fail after flipping
    // the run flag and silently kill capture.) TEXT rows are self-describing
    // key=value, so live changes stay allowed there.
    if (gSensorLoggingRunning && gSensorLogFormat == SENSOR_LOG_CSV) {
      cliHint("run 'sensorlog stop' first, then reselect sensors and restart — or use 'healthtrack' which restarts the session itself");
      return "Error: can't change the sensor selection while a CSV session is running (rows would stop matching the file's header)";
    }

    String sensorList = a.remaining(0);
    sensorList.toLowerCase();

    if (sensorList == "all") {
      gSensorLogMask = LOG_THERMAL | LOG_TOF | LOG_IMU | LOG_GAMEPAD | LOG_APDS | LOG_GPS | LOG_PRESENCE | LOG_R1;
      setSetting(gSettings.sensorLogMask, (int)gSensorLogMask);
      return "All sensors enabled for logging";
    }

    if (sensorList == "none") {
      gSensorLogMask = 0x00;
      setSetting(gSettings.sensorLogMask, (int)gSensorLogMask);
      return "All sensors disabled for logging";
    }

    gSensorLogMask = 0x00;
    int start = 0;
    while (start < (int)sensorList.length()) {
      int comma = sensorList.indexOf(',', start);
      String sensor = (comma >= 0) ? sensorList.substring(start, comma) : sensorList.substring(start);
      sensor.trim();

      if (sensor == "thermal") gSensorLogMask |= LOG_THERMAL;
      else if (sensor == "tof") gSensorLogMask |= LOG_TOF;
      else if (sensor == "imu") gSensorLogMask |= LOG_IMU;
      else if (sensor == "gamepad" || sensor == "input") gSensorLogMask |= LOG_GAMEPAD;
      else if (sensor == "apds") gSensorLogMask |= LOG_APDS;
      else if (sensor == "gps") gSensorLogMask |= LOG_GPS;
      else if (sensor == "presence") gSensorLogMask |= LOG_PRESENCE;
      else if (sensor == "r1" || sensor == "health" || sensor == "ring") gSensorLogMask |= LOG_R1;
      else {
        snprintf(getDebugBuffer(), 1024, "Error: Unknown sensor '%s'", sensor.c_str());
        return getDebugBuffer();
      }

      if (comma < 0) break;
      start = comma + 1;
    }

    setSetting(gSettings.sensorLogMask, (int)gSensorLogMask);
    snprintf(getDebugBuffer(), 1024, "Logging enabled for: %s%s%s%s%s%s%s%s",
             (gSensorLogMask & LOG_THERMAL) ? "thermal " : "",
             (gSensorLogMask & LOG_TOF) ? "tof " : "",
             (gSensorLogMask & LOG_IMU) ? "imu " : "",
             (gSensorLogMask & LOG_GAMEPAD) ? "gamepad " : "",
             (gSensorLogMask & LOG_APDS) ? "apds " : "",
             (gSensorLogMask & LOG_GPS) ? "gps " : "",
             (gSensorLogMask & LOG_PRESENCE) ? "presence " : "",
             (gSensorLogMask & LOG_R1) ? "r1 " : "");
    return getDebugBuffer();
  }

  // Handle 'interval' subcommand
  if (subCmd == "interval") {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    if (!a.has(1)) {
      snprintf(getDebugBuffer(), 1024, "Current interval: %lums\nUsage: sensorlog interval <ms> (100-3600000)",
               (unsigned long)gSensorLogIntervalMs);
      return getDebugBuffer();
    }
    uint32_t ms = (uint32_t)a.argInt(1, (int)gSensorLogIntervalMs);
    if (ms < 100) ms = 100;
    if (ms > 3600000) ms = 3600000;
    gSensorLogIntervalMs = ms;
    setSetting(gSettings.sensorLogIntervalMs, (int)ms);
    snprintf(getDebugBuffer(), 1024, "Sensor log interval set to %lums", (unsigned long)ms);
    return getDebugBuffer();
  }

  // Handle 'autostart' subcommand
  if (subCmd == "autostart") {
    if (!a.has(1)) {
      // Toggle
      bool newVal = !gSettings.sensorLogAutoStart;
      setSetting(gSettings.sensorLogAutoStart, newVal);
      return newVal ? "Sensor logging auto-start ENABLED" : "Sensor logging auto-start DISABLED";
    }
    bool enable = a.argBool(1, false);
    setSetting(gSettings.sensorLogAutoStart, enable);
    return enable ? "Sensor logging auto-start ENABLED" : "Sensor logging auto-start DISABLED";
  }

  return "Error: Unknown subcommand. Use: start, stop, status, format, maxsize, rotations, sensors, or autostart";
}

// ============================================================================
// R1 Health Track (kick off durable vitals capture from the Health feature)
// ============================================================================

#ifndef CAPTURE_HEALTHLOG_DEFAULT
#define CAPTURE_HEALTHLOG_DEFAULT CAPTURE_DIR_SENSORS "/health.csv"
#endif

bool healthTrackIsActive() {
  return gSettings.healthTrackingEnabled &&
         (gSensorLogMask & LOG_R1) != 0 &&
         gSensorLoggingRunning;
}

// Health Track mine state — polls all four vitals, waits for notifies, logs.
enum : uint8_t { HT_MINE_IDLE = 0, HT_MINE_POLLING, HT_MINE_SETTLE };
static uint8_t  sHtMineState = HT_MINE_IDLE;
static uint8_t  sHtMineCursor = 0;
static uint32_t sHtMineLastMs = 0;
static uint32_t sHtMineLastBurstMs = 0;
static bool     sHtPageRefreshPending = false;
static uint32_t sHtPageRefreshDueMs = 0;

static uint32_t healthTrackIntervalMs() {
  int sec = gSettings.healthTrackPollIntervalSec;
  if (sec < 60) sec = 60;
  if (sec > 86400) sec = 86400;
  return (uint32_t)sec * 1000u;
}

void healthTrackNotePageRefresh() {
  // Give notify replies ~1.5 s to land, then force a log line.
  sHtPageRefreshPending = true;
  sHtPageRefreshDueMs = millis() + 1500;
}

#if ENABLE_R1_HEALTH
// On-demand poll burst (healthstatus poll / web Poll Now) — independent of Track.
enum : uint8_t { HT_OD_IDLE = 0, HT_OD_POLLING, HT_OD_SETTLE };
static uint8_t  sHtOdState = HT_OD_IDLE;
static uint8_t  sHtOdCursor = 0;
static uint32_t sHtOdLastMs = 0;
#endif

bool healthStartPollBurst(void) {
#if ENABLE_R1_HEALTH
  if (!g2RingIsConnected()) return false;
  sHtOdState = HT_OD_POLLING;
  sHtOdCursor = 0;
  sHtOdLastMs = 0;
  return true;
#else
  return false;
#endif
}

const char* buildHealthStatusJson(char* buf, size_t cap) {
  if (!buf || cap < 3) return "{}";
#if ENABLE_R1_HEALTH
  G2RingTelemetry t;
  g2RingGetTelemetry(t);
  CompactJson j(buf, cap);
  j.kv("schema", 2)
   .kv("connected", (bool)t.connected)
   .kv("hrValid", (bool)t.hrValid)
   .kv("hr", (unsigned)t.hr)
   .kv("hrAgeSec", (int)t.hrAgeSec)
   .kv("hrvValid", (bool)t.hrvValid)
   .kv("hrv", (int)t.hrv)
   .kv("hrvAgeSec", (int)t.hrvAgeSec)
   .kv("spo2Valid", (bool)t.spo2Valid)
   .kv("spo2", (unsigned)t.spo2)
   .kv("spo2AgeSec", (int)t.spo2AgeSec)
   .kv("tempValid", (bool)t.tempValid)
   .kv("tempTenths", (int)t.tempTenths)
   .kv("tempAgeSec", (int)t.tempAgeSec)
   .kv("batteryValid", (bool)t.batteryValid)
   .kv("battery", (unsigned)t.battery)
   .kv("batteryAgeSec", (int)t.batteryAgeSec)
   .kv("wearValid", (bool)t.wearValid)
   .kv("wear", (unsigned)t.wear)
   .kv("wearAgeSec", (int)t.wearAgeSec)
   .kv("trackActive", healthTrackIsActive())
   .kv("trackEnabled", (bool)gSettings.healthTrackingEnabled)
   .kv("pollIntervalSec", (unsigned)gSettings.healthTrackPollIntervalSec)
   .kv("r1Mask", (bool)((gSensorLogMask & LOG_R1) != 0))
   .kv("logging", (bool)gSensorLoggingRunning);
  return j.c_str();
#else
  snprintf(buf, cap, "{\"schema\":1,\"enabled\":false}");
  return buf;
#endif
}

const char* cmd_healthstatus(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
#if !ENABLE_R1_HEALTH
  (void)argsInput;
  return "Error: R1 Health requires ENABLE_R1_HEALTH in this build";
#else
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  CommandArgs a(argsInput);
  String sub = a.has(0) ? a.arg(0) : String();
  sub.toLowerCase();

  if (sub == "poll") {
    if (!healthStartPollBurst()) return "Error: ring not connected — connect via Bluetooth → R1 Ring";
    return "SUCCESS: R1 Health poll burst started (HR/HRV/SpO2/temp/battery)";
  }

  if (argWantsJson(argsInput) || sub == "json") {
    return buildHealthStatusJson(getDebugBuffer(), 1024);
  }

  G2RingTelemetry t;
  g2RingGetTelemetry(t);
  char hr[8], hrv[8], spo2[8], temp[12], bat[8], wear[8];
  if (t.hrValid) snprintf(hr, sizeof(hr), "%u", (unsigned)t.hr); else snprintf(hr, sizeof(hr), "--");
  if (t.hrvValid) snprintf(hrv, sizeof(hrv), "%d", (int)t.hrv); else snprintf(hrv, sizeof(hrv), "--");
  if (t.spo2Valid) snprintf(spo2, sizeof(spo2), "%u%%", (unsigned)t.spo2); else snprintf(spo2, sizeof(spo2), "--");
  if (t.tempValid) snprintf(temp, sizeof(temp), "%d.%dC",
                            (int)(t.tempTenths / 10),
                            (int)(t.tempTenths < 0 ? -(t.tempTenths % 10) : (t.tempTenths % 10)));
  else snprintf(temp, sizeof(temp), "--");
  if (t.batteryValid) snprintf(bat, sizeof(bat), "%u%%", (unsigned)t.battery); else snprintf(bat, sizeof(bat), "--");
  if (!t.wearValid) snprintf(wear, sizeof(wear), "--");
  else if (t.wear == 2) snprintf(wear, sizeof(wear), "on");
  else if (t.wear == 1) snprintf(wear, sizeof(wear), "off");
  else snprintf(wear, sizeof(wear), "?");
  snprintf(getDebugBuffer(), 1024,
           "R1 Health: ring=%s  HR=%s  HRV=%s  SpO2=%s  T=%s  Bat=%s  Wear=%s\n"
           "  ages(s): hr=%d hrv=%d spo2=%d temp=%d bat=%d wear=%d\n"
           "  Track=%s (setting=%s, r1_mask=%s, logging=%s, poll=%us)\n"
           "Usage: healthstatus [json|poll]",
           t.connected ? "up" : "down", hr, hrv, spo2, temp, bat, wear,
           (int)t.hrAgeSec, (int)t.hrvAgeSec, (int)t.spo2AgeSec,
           (int)t.tempAgeSec, (int)t.batteryAgeSec, (int)t.wearAgeSec,
           healthTrackIsActive() ? "ACTIVE" : "inactive",
           gSettings.healthTrackingEnabled ? "on" : "off",
           (gSensorLogMask & LOG_R1) ? "yes" : "no",
           gSensorLoggingRunning ? "running" : "stopped",
           (unsigned)gSettings.healthTrackPollIntervalSec);
  return getDebugBuffer();
#endif
}

void healthTrackTick() {
#if ENABLE_R1_HEALTH
  const uint32_t now = millis();

  // On-demand Health-page refresh → log whenever R1 sensorlog is running
  // (Track or manual `sensorlog sensors r1`), not only on the timed mine.
  if (sHtPageRefreshPending && (long)(now - sHtPageRefreshDueMs) >= 0) {
    sHtPageRefreshPending = false;
    if (gSensorLoggingRunning && (gSensorLogMask & LOG_R1)) {
      sensorLogRequestSample(/*bypassR1Dedup*/ true);
      if (isDebugFlagSet(DEBUG_LOGGER)) {
        DEBUG_LOGGERF("healthtrack: page refresh → sensorlog sample");
      }
    }
  }

  // On-demand poll burst (CLI/web/OLED can also poll locally).
  // Yield the ring TX while the Health page owns Poll Now / entry burst.
  if (sHtOdState == HT_OD_POLLING) {
    if (!g2RingIsConnected()) {
      sHtOdState = HT_OD_IDLE;
    } else if (g2HealthPageIsActive()) {
      // Pause — resume next tick when page closes.
    } else if (sHtOdLastMs == 0 || (long)(now - sHtOdLastMs) >= 700) {
      (void)g2RingPollVital(sHtOdCursor);
      sHtOdCursor++;
      sHtOdLastMs = now;
      if (sHtOdCursor >= G2_RING_POLL_VITAL_COUNT) {
        sHtOdState = HT_OD_SETTLE;
        sHtOdLastMs = now;
      }
    }
  } else if (sHtOdState == HT_OD_SETTLE) {
    if ((long)(now - sHtOdLastMs) >= 1500) {
      sHtOdState = HT_OD_IDLE;
      healthTrackNotePageRefresh();
    }
  }

  if (!gSettings.healthTrackingEnabled) {
    sHtMineState = HT_MINE_IDLE;
    return;
  }

  // Mine cadence gate — used both when connected (poll vitals) and when
  // down (nudge BLE once per interval instead of spinning every loop).
  const bool mineDue = (sHtMineLastBurstMs == 0 ||
      (long)(now - sHtMineLastBurstMs) >= (long)healthTrackIntervalMs());

  if (!g2RingIsConnected()) {
    sHtMineState = HT_MINE_IDLE;
    if (mineDue) {
      // Advance the mine clock so we only nudge once per interval.
      sHtMineLastBurstMs = now;
      // Non-blocking: schedules connectSaved on the next bleAutoReconnectTick.
      // Respects user ringdisconnect; works even if bleautoreconnect is off
      // (one-shot). With autoReconnect on, drop recovery continues via backoff.
      blePeerRequestReseek(BLE_PEER_R1_RING);
      if (isDebugFlagSet(DEBUG_LOGGER)) {
        DEBUG_LOGGERF("healthtrack: mine due but ring down — requested BLE reseek");
      }
    }
    return;
  }

  if (g2HealthPageIsActive()) {
    // Don't start/continue timed mine over the lens poller.
    return;
  }

  if (sHtMineState == HT_MINE_IDLE) {
    if (mineDue) {
      sHtMineState = HT_MINE_POLLING;
      sHtMineCursor = 0;
      sHtMineLastMs = 0;
      if (isDebugFlagSet(DEBUG_LOGGER)) {
        DEBUG_LOGGERF("healthtrack: timed mine start (interval=%us)",
                      (unsigned)gSettings.healthTrackPollIntervalSec);
      }
    }
    return;
  }

  if (sHtMineState == HT_MINE_POLLING) {
    if (sHtMineLastMs == 0 || (long)(now - sHtMineLastMs) >= 700) {
      (void)g2RingPollVital(sHtMineCursor);
      sHtMineCursor++;
      sHtMineLastMs = now;
      if (sHtMineCursor >= 4) {
        sHtMineState = HT_MINE_SETTLE;
        sHtMineLastMs = now;
      }
    }
    return;
  }

  if (sHtMineState == HT_MINE_SETTLE) {
    if ((long)(now - sHtMineLastMs) >= 1500) {
      sHtMineLastBurstMs = now;
      sHtMineState = HT_MINE_IDLE;
      if (gSensorLoggingRunning && (gSensorLogMask & LOG_R1)) {
        sensorLogRequestSample(/*bypassR1Dedup*/ true);
        if (isDebugFlagSet(DEBUG_LOGGER)) {
          DEBUG_LOGGERF("healthtrack: timed mine → sensorlog sample");
        }
      }
    }
  }
#else
  (void)0;
#endif
}

// Shape a configured capture path into this session's on-disk path:
//   1. split dir / basename / extension
//   2. strip any prior session-timestamp suffix from the basename AND any
//      prior session subfolder (YYYY-MM-DD/ or boot-N/) from the dir, so a
//      previously shaped path fed back through settings can't nest
//   3. pick this session's subfolder under the capture dir: YYYY-MM-DD/
//      when the clock is synced, boot-NNNNNN/ otherwise (zero-padded so
//      lexical order = boot order in every file browser)
//   4. append the per-session stamp: -YYYY-MM-DDTHH-MM-SS, or -bootN-ms —
//      the (bootN, ms) form is exactly what System_TimeAnchors retro-dates
//      into a YYYY-MM-DD/ folder once this boot (or a later one) learns
//      real time.
// Keeps big capture folders G2-browsable: the lens list pages per folder,
// and one day (or one dark boot) is one folder instead of one flat pile.
// Inverse of shapeSessionPath: recover the configured BASE path from a
// shaped one. Strips the session subfolder (YYYY-MM-DD/ or boot-N/) from the
// dir and the session stamp (-YYYY-…, -bootN-…, and any -V variant digit —
// variants always follow the date, so the date strip swallows them) from the
// basename. A no-op for manual literal paths. Used to persist the base into
// settings and by the tick rollover to re-shape from scratch.
static String stripSessionShaping(String path) {
  // Split dir / basename / extension.
  int lastSlash = path.lastIndexOf('/');
  int lastDot = path.lastIndexOf('.');
  String dir = (lastSlash >= 0) ? path.substring(0, lastSlash + 1) : CAPTURE_DIR_SENSORS "/";
  String baseName = (lastSlash >= 0) ? path.substring(lastSlash + 1) : path;
  String ext = "";
  if (lastDot > lastSlash && lastDot > 0) {
    ext = path.substring(lastDot);
    baseName = baseName.substring(0, baseName.lastIndexOf('.'));
  }

  // Strip a prior session subfolder from the dir (idempotency: a shaped
  // path fed back in must not nest date-in-date). dir always ends with '/';
  // look at its last component.
  if (dir.length() > 1) {
    int tailStart = dir.lastIndexOf('/', dir.length() - 2);
    if (tailStart >= 0) {
      String comp = dir.substring(tailStart + 1, dir.length() - 1);
      bool isDate = comp.length() == 10 && comp.charAt(4) == '-' &&
                    comp.charAt(7) == '-' && isdigit(comp.charAt(0)) &&
                    isdigit(comp.charAt(1)) && isdigit(comp.charAt(2)) &&
                    isdigit(comp.charAt(3));
      bool isBoot = comp.startsWith("boot-") && comp.length() > 5;
      if (isDate || isBoot) dir = dir.substring(0, tailStart + 1);
    }
  }

  // Strip any existing timestamp suffixes from baseName to prevent double-timestamping.
  // Timestamps start with "-YYYY-" (e.g. "-2026-04-04T...") or "-boot".
  // Scan forward for the first such pattern rather than looking at the last segment,
  // because the last segment is often just the seconds ("02") which has no T or boot.
  {
    int stripAt = -1;
    for (int i = 0; i < (int)baseName.length() - 5; i++) {
      if (baseName.charAt(i) == '-') {
        // Check for "-YYYY-" (4 digits followed by another dash)
        if (isdigit(baseName.charAt(i+1)) && isdigit(baseName.charAt(i+2)) &&
            isdigit(baseName.charAt(i+3)) && isdigit(baseName.charAt(i+4)) &&
            baseName.charAt(i+5) == '-') {
          stripAt = i;
          break;
        }
        // Check for "-boot"
        if (baseName.length() > (size_t)(i + 4) &&
            baseName.charAt(i+1) == 'b' && baseName.charAt(i+2) == 'o' &&
            baseName.charAt(i+3) == 'o' && baseName.charAt(i+4) == 't') {
          stripAt = i;
          break;
        }
      }
    }
    if (stripAt > 0) baseName = baseName.substring(0, stripAt);
  }

  return dir + baseName + ext;
}

static String shapeSessionPath(String path) {
  // Normalize to the base first so shaping is idempotent for any input.
  path = stripSessionShaping(path);
  int lastSlash = path.lastIndexOf('/');
  int lastDot = path.lastIndexOf('.');
  String dir = (lastSlash >= 0) ? path.substring(0, lastSlash + 1) : CAPTURE_DIR_SENSORS "/";
  String baseName = (lastSlash >= 0) ? path.substring(lastSlash + 1) : path;
  String ext = "";
  if (lastDot > lastSlash && lastDot > 0) {
    ext = path.substring(lastDot);
    baseName = baseName.substring(0, baseName.lastIndexOf('.'));
  }

  // Session subfolder + filename. Clock::isSynced replaces the old raw
  // "now > 1609459200" epoch compare (same year>=2020 heuristic, one home).
  extern uint32_t gBootCounter;
  char sub[20];
  char stamp[32];
  if (Clock::isSynced()) {
    time_t now = time(nullptr);
    struct tm timeinfo;
    localtime_r(&now, &timeinfo);
    strftime(sub, sizeof(sub), "%Y-%m-%d", &timeinfo);
    if (gSensorLogFormat == SENSOR_LOG_CSV) {
      // Per-day file: every CSV session that day appends to the same file so
      // the day graphs as one continuous series (rows carry epoch-ms).
      // Header compatibility across sessions is resolveSessionTarget's job.
      snprintf(stamp, sizeof(stamp), "%s", sub);
    } else {
      // TEXT/TRACK stay one-file-per-session: TEXT rows are prose logs and
      // a TRACK is one GPS journey — day-merging them buys nothing.
      strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H-%M-%S", &timeinfo);
    }
  } else {
    // No valid time — boot counter + ms keep sessions ordered across power
    // cycles; System_TimeAnchors promotes these once time is learned, and a
    // running CSV session rolls onto the day file the moment the clock syncs.
    snprintf(sub, sizeof(sub), "boot-%06lu", (unsigned long)gBootCounter);
    snprintf(stamp, sizeof(stamp), "boot%lu-%lu",
             (unsigned long)gBootCounter, millis());
  }

  // e.g. /logging_captures/sensors/2026-07-27/health-2026-07-27.csv   (CSV day file)
  //      /logging_captures/sensors/2026-07-27/sensors-2026-07-27T14-30-00.txt
  //  or  /logging_captures/sensors/boot-000123/health-boot123-45678.csv
  char pathBuf[256];
  snprintf(pathBuf, sizeof(pathBuf), "%s%s/%s-%s%s", dir.c_str(), sub,
           baseName.c_str(), stamp, ext.c_str());
  return String(pathBuf);
}

static const char* healthTrackRestartWithCurrentMask() {
  // Stop then start so CSV headers match the new mask.
  if (gSensorLoggingRunning) {
    gSensorLoggingRunning = false;
  }
  String path = gSettings.sensorLogPath;
  if (path.length() == 0 || path.charAt(0) != '/') path = CAPTURE_HEALTHLOG_DEFAULT;
  // Same per-session subfolder + timestamp shaping as sensorLogAutoStart —
  // previously this start path wrote the bare configured file (health.csv)
  // with no timestamp at all, diverging from boot-time sessions.
  path = shapeSessionPath(path);
  uint32_t interval = gSensorLogIntervalMs;
  if (interval < 100) interval = 5000;
  char cmd[320];
  snprintf(cmd, sizeof(cmd), "start \"%s\" %lu", path.c_str(), (unsigned long)interval);
  return cmd_sensorlog(String(cmd));
}

const char* healthTrackSet(bool on) {
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (on) {
    if (!gSettings.sensorLogEnabled) {
      return "Error: sensor logging is disabled — run 'sensorlogenabled 1' first";
    }
#if !ENABLE_R1_HEALTH
    return "Error: R1 Health Track requires ENABLE_R1_HEALTH in this build";
#else
    setSetting(gSettings.healthTrackingEnabled, true);
    // Capture BEFORE the OR: the "already logging" shortcut below must only
    // fire when R1 was ALREADY in the mask. Otherwise `healthtrack on` during
    // a running non-R1 CSV session mutates the mask live and every later row
    // gains 7 R1 columns under the old header — permanent misalignment inside
    // a long-lived day file. Newly-added R1 → fall through to the restart.
    const bool hadR1 = (gSensorLogMask & LOG_R1) != 0;
    gSensorLogMask = (uint8_t)(gSensorLogMask | LOG_R1);
    setSetting(gSettings.sensorLogMask, (int)gSensorLogMask);

    // Track format is GPS-only — health needs TEXT/CSV columns.
    if (gSensorLogFormat == SENSOR_LOG_TRACK) {
      gSensorLogFormat = SENSOR_LOG_CSV;
      setSetting(gSettings.sensorLogFormat, (int)SENSOR_LOG_CSV);
    }

    // Prefer the health capture path when starting a health-owned session.
    if (gSettings.sensorLogPath.length() == 0 ||
        gSettings.sensorLogPath.indexOf("health") < 0) {
      setSetting(gSettings.sensorLogPath, String(CAPTURE_HEALTHLOG_DEFAULT));
    }

    // Resume after reboot.
    setSetting(gSettings.sensorLogAutoStart, true);

    // Kick an immediate mine burst (don't wait a full poll interval).
    sHtMineLastBurstMs = 0;
    sHtMineState = HT_MINE_IDLE;

    if (gSensorLoggingRunning && hadR1) {
      // Already logging WITH R1 already in the mask — nothing more to do.
      // (If R1 was just added, fall through to the restart so the CSV header
      // matches the new column set.)
      snprintf(getDebugBuffer(), 1024,
               "SUCCESS: Health Track ON (already logging → %s)",
               gSensorLogPath.c_str());
      return getDebugBuffer();
    }

    const char* result = healthTrackRestartWithCurrentMask();
    if (result && strncmp(result, "SUCCESS", 7) == 0) {
      snprintf(getDebugBuffer(), 1024,
               "SUCCESS: Health Track ON — logging R1 vitals to %s",
               gSensorLogPath.c_str());
      return getDebugBuffer();
    }
    return result ? result : "Error: Health Track failed to start sensorlog";
#endif
  }

  // Off
  setSetting(gSettings.healthTrackingEnabled, false);
  gSensorLogMask = (uint8_t)(gSensorLogMask & (uint8_t)~LOG_R1);
  setSetting(gSettings.sensorLogMask, (int)gSensorLogMask);
  sHtMineState = HT_MINE_IDLE;
  sHtPageRefreshPending = false;

  if (!gSensorLoggingRunning) {
    return "SUCCESS: Health Track OFF";
  }

  if (gSensorLogMask == 0) {
    gSensorLoggingRunning = false;
    setSetting(gSettings.sensorLogAutoStart, false);
    systemEventPost(SYSEVT_SENSOR_STOPPED, "Logging");
    return "SUCCESS: Health Track OFF — sensor logging stopped";
  }

  // Other sensors still selected — restart so CSV headers drop R1 columns.
  const char* result = healthTrackRestartWithCurrentMask();
  if (result && strncmp(result, "SUCCESS", 7) == 0) {
    return "SUCCESS: Health Track OFF — R1 removed; other sensors still logging";
  }
  return result ? result : "SUCCESS: Health Track OFF";
}

const char* cmd_healthtrack(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  CommandArgs a(argsInput);
  if (!a.has(0)) {
    snprintf(getDebugBuffer(), 1024,
             "Health Track: %s\n"
             "  logging=%s  mask_r1=%s  setting=%s  poll=%us\n"
             "Usage: healthtrack <on|off|toggle|status|interval [sec]>",
             healthTrackIsActive() ? "ACTIVE" : "inactive",
             gSensorLoggingRunning ? "running" : "stopped",
             (gSensorLogMask & LOG_R1) ? "yes" : "no",
             gSettings.healthTrackingEnabled ? "on" : "off",
             (unsigned)gSettings.healthTrackPollIntervalSec);
    return getDebugBuffer();
  }
  String sub = a.arg(0); sub.toLowerCase();
  if (sub == "status") {
    snprintf(getDebugBuffer(), 1024,
             "Health Track: %s (setting=%s, r1_mask=%s, logging=%s, poll=%us, path=%s)",
             healthTrackIsActive() ? "ACTIVE" : "inactive",
             gSettings.healthTrackingEnabled ? "on" : "off",
             (gSensorLogMask & LOG_R1) ? "yes" : "no",
             gSensorLoggingRunning ? "running" : "stopped",
             (unsigned)gSettings.healthTrackPollIntervalSec,
             gSensorLogPath.length() ? gSensorLogPath.c_str() : "(none)");
    return getDebugBuffer();
  }
  if (sub == "interval") {
    if (!a.has(1)) {
      snprintf(getDebugBuffer(), 1024,
               "Health Track poll interval: %u sec (min 60, max 86400)\n"
               "Usage: healthtrack interval <seconds>",
               (unsigned)gSettings.healthTrackPollIntervalSec);
      return getDebugBuffer();
    }
    int sec = a.argInt(1, 0);
    if (sec < 60 || sec > 86400) {
      return "Error: healthtrack interval must be 60..86400 seconds";
    }
    setSetting(gSettings.healthTrackPollIntervalSec, sec);
    // Reschedule next mine from now so a shorter interval takes effect promptly.
    sHtMineLastBurstMs = millis();
    snprintf(getDebugBuffer(), 1024,
             "SUCCESS: Health Track poll interval set to %d sec (%d min)",
             sec, sec / 60);
    return getDebugBuffer();
  }
  if (sub == "on" || sub == "1" || sub == "true")  return healthTrackSet(true);
  if (sub == "off" || sub == "0" || sub == "false") return healthTrackSet(false);
  if (sub == "toggle") return healthTrackSet(!gSettings.healthTrackingEnabled);
  return "Error: Usage: healthtrack <on|off|toggle|status|interval [sec]>";
}

// Stitch several health/sensor TEXT captures into one file (gpstrackmerge twin).
// Args: "<output>" "<in1>" "<in2>" ... — bare output names land under
// /logging_captures/sensors/. Newline boundary after each input.
const char* cmd_healthlogmerge(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* buf = getDebugBuffer();

  CommandArgs a(argsInput);
  if (a.count() < 2) {
    return "Error: invalid arguments — Usage: healthlogmerge \"<output>\" \"<in1>\" \"<in2>\" [...] "
           "(output first, then inputs in stitch order; max 9 inputs)";
  }
  if (a.count() > 10) {  // 1 output + 9 inputs
    return "Error: too many inputs — max 9 files";
  }

  String outName;
  const char* qerr = requireQuotedToken(a, 0, outName);
  if (qerr) return qerr;
  String outPath = outName;
  if (!outPath.startsWith("/")) outPath = String(CAPTURE_DIR_SENSORS) + "/" + outPath;
  if (!(outPath.endsWith(".csv") || outPath.endsWith(".log") || outPath.endsWith(".txt")))
    outPath += ".csv";

  const AuthContext& ctx    = currentAuthContext();
  const AuthContext  sysCtx = VFS::systemAuth("health.stitch");
  FsLockGuard fsGuard("healthlogmerge");

  for (int i = 1; i < a.count(); i++) {
    String in;
    const char* e = requireQuotedPath(a, i, in);
    if (e) return e;
    if (in == outPath) return "Error: an input file is the same as the output";
    if (!VFS::existsGuarded(in, ctx)) {
      snprintf(buf, 1024, "Error: input not found: %s", in.c_str());
      return buf;
    }
  }

  VFS::mkdirGuarded("/logging_captures", sysCtx);
  VFS::mkdirGuarded(CAPTURE_DIR_SENSORS, sysCtx);

  File out = VFS::openGuarded(outPath, "w", sysCtx, true);
  if (!out) {
    snprintf(buf, 1024, "Error: cannot create output: %s", outPath.c_str());
    return buf;
  }

  uint8_t chunk[512];
  unsigned long totalBytes = 0;
  int filesDone = 0;
  for (int i = 1; i < a.count(); i++) {
    String in;
    requireQuotedPath(a, i, in);
    File f = VFS::openGuarded(in, "r", ctx);
    if (!f) {
      out.close();
      snprintf(buf, 1024, "Error: cannot open input: %s", in.c_str());
      return buf;
    }
    uint8_t lastByte = (uint8_t)'\n';
    while (f.available()) {
      int n = f.read(chunk, sizeof(chunk));
      if (n <= 0) break;
      if (out.write(chunk, (size_t)n) != (size_t)n) {
        f.close();
        out.close();
        snprintf(buf, 1024, "Error: write failed (filesystem full?) after %lu bytes to %s",
                 totalBytes, outPath.c_str());
        return buf;
      }
      totalBytes += (unsigned long)n;
      lastByte = chunk[n - 1];
    }
    f.close();
    if (lastByte != (uint8_t)'\n') { out.write((uint8_t)'\n'); totalBytes++; }
    filesDone++;
  }
  out.close();

  snprintf(buf, 1024,
           "Stitched %d files into %s (%lu bytes)",
           filesDone, outPath.c_str(), totalBytes);
  return buf;
}

// ============================================================================
// Command Registry
// ============================================================================

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry sensorLoggingCommands[] = {
  { "sensorlog", "Sensor data logging: start, stop, status, format, maxsize, rotations, sensors", false, cmd_sensorlog,
    "Usage: sensorlog <start|stop|status|format|maxsize|rotations|sensors|interval|autostart> [args...]\n"
    "  start <filepath> [interval_ms]: Begin logging (default 5000ms)\n"
    "  stop: Stop logging\n"
    "  status: Show current logging status\n"
    "  format <text|csv|track>: Set log format (default: text)\n"
    "    track = GPS-only compact format with signal loss dedup\n"
    "  maxsize <bytes>: Set max file size before rotation (default: 256000)\n"
    "  rotations <count>: Set number of old logs to keep (0-9, default: 3)\n"
    "  sensors <thermal|tof|imu|gamepad|apds|gps|presence|r1|all|none>: Select sensors to log\n"
    "  interval <ms>: Set poll interval 100-3600000 (default 5000)\n"
    "  autostart [on|off]: Auto-start logging on boot (bare = toggle)" },
  { "healthtrack", "Start/stop R1 Health Track (enables r1 sensorlog + starts capture)", false, cmd_healthtrack,
    "Usage: healthtrack <on|off|toggle|status|interval [sec]>\n"
    "  on: enable LOG_R1, start sensorlog to /logging_captures/sensors/health.csv, persist for boot\n"
    "  off: remove LOG_R1; stop logging if no other sensors remain\n"
    "  interval <sec>: how often to poll/mine the ring while Track is on (default 900 = 15 min)\n"
    "  R1-only Track sessions write ONLY on that mine (and Poll Now) — no 5s empty heartbeats\n"
    "  Also: Apps → Health → Toggle Track on the G2 lens; OLED/Web R1 Health" },
  { "healthstatus", "R1 Health vitals + Track snapshot (text or json); poll starts a 4-vital burst", false, cmd_healthstatus,
    "Usage: healthstatus [json|poll]\n"
    "  bare/json: connected, HR/HRV/SpO2/battery (+valid), Track state\n"
    "  poll: kick HR→HRV→SpO2→battery point queries (replies via notify)\n"
    "  BLE App / Web use healthstatus json; connect via ringconnect / Bluetooth page" },
  { "healthlogmerge", "Stitch health/sensor TEXT logs in order: \"<out>\" \"<in1>\" \"<in2>\" ...", true, cmd_healthlogmerge,
    "Usage: healthlogmerge \"<output>\" \"<in1>\" \"<in2>\" [...]  (output first, then inputs in stitch order; max 9 inputs; bare names → /logging_captures/sensors/)" },
};

const size_t sensorLoggingCommandsCount = sizeof(sensorLoggingCommands) / sizeof(sensorLoggingCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// Settings Module Registration
// ============================================================================

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry sensorLogSettingEntries[] = {
  { "sensorLogEnabled", SETTING_BOOL, &gSettings.sensorLogEnabled, 1, 0, nullptr, 0, 1, "Enabled", nullptr, false, nullptr, "sensorlogenabled" },
  { "sensorLogAutoStart",    SETTING_BOOL,   &gSettings.sensorLogAutoStart,    0, 0, nullptr, 0, 1,       "Auto-start logging after boot", nullptr, false, nullptr, "sensorlog autostart" },
#if ENABLE_R1_HEALTH
  { "healthTrackingEnabled", SETTING_BOOL, &gSettings.healthTrackingEnabled, 0, 0, nullptr, 0, 1, "R1 Health Track", nullptr, false, nullptr, "healthtrack" },
  { "healthTrackPollIntervalSec", SETTING_INT, &gSettings.healthTrackPollIntervalSec, 900, 0, nullptr, 60, 86400, "R1 Health poll interval (sec)", nullptr, false, nullptr, "healthtrack interval" },
#endif
  { "sensorLogPath", SETTING_STRING, &gSettings.sensorLogPath, 0, 0, CAPTURE_SENSORLOG_DEFAULT, 0, 0, "Log file path", nullptr, false, nullptr, "sensorlogpath" },
  { "sensorLogIntervalMs", SETTING_INT, &gSettings.sensorLogIntervalMs, 5000, 0, nullptr, 100, 3600000, "Poll interval (ms)", nullptr, false, nullptr, "sensorlog interval" },
  { "sensorLogMask", SETTING_INT, &gSettings.sensorLogMask, 0, 0, nullptr, 0, 255, "Sensors to log",
    "bitmask:1|Thermal,2|ToF,4|IMU,8|Gamepad,16|APDS,32|GPS,64|Presence,128|R1 Health", false, nullptr, "sensorlogmask" },
  { "sensorLogFormat", SETTING_INT, &gSettings.sensorLogFormat, 0, 0, nullptr, 0, 2, "Format", "0|Text,1|CSV,2|Track", false, nullptr, "sensorlogformat" },
  { "sensorLogMaxSize", SETTING_INT, &gSettings.sensorLogMaxSize, 256000, 0, nullptr, 10240, 10485760, "Max file size (bytes)", nullptr, false, nullptr, "sensorlog maxsize" },
  { "sensorLogMaxRotations", SETTING_INT, &gSettings.sensorLogMaxRotations, 3, 0, nullptr, 0, 9, "Rotations (old logs to keep)", nullptr, false, nullptr, "sensorlog rotations" }
};

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule sensorLogSettingsModule = {
  "sensorlog",
  "logging.sensorlog",
  sensorLogSettingEntries,
  sizeof(sensorLogSettingEntries) / sizeof(sensorLogSettingEntries[0]),
  nullptr,
  "Sensor data logging auto-start and parameters"
};

// ============================================================================
// Auto-Start (called from boot after sensors are initialized)
// ============================================================================

void sensorLogAutoStart() {
  // Health Track alone is enough to resume capture at boot (it also sets
  // sensorLogAutoStart when enabled, but honor the setting either way).
  if (!gSettings.sensorLogAutoStart && !gSettings.healthTrackingEnabled) return;
  if (gSensorLoggingRunning) return;  // Already running

  // Restore persisted parameters
  if (gSettings.sensorLogMask > 0) gSensorLogMask = (uint8_t)gSettings.sensorLogMask;
  if (gSettings.sensorLogFormat >= 0 && gSettings.sensorLogFormat <= 2) gSensorLogFormat = (SensorLogFormat)gSettings.sensorLogFormat;
  if (gSettings.sensorLogIntervalMs >= 100) gSensorLogIntervalMs = gSettings.sensorLogIntervalMs;
  if (gSettings.sensorLogMaxSize >= 10240 && gSettings.sensorLogMaxSize <= 10485760)
    gSensorLogMaxSize = (size_t)gSettings.sensorLogMaxSize;
  if (gSettings.sensorLogMaxRotations >= 0 && gSettings.sensorLogMaxRotations <= 9)
    gSensorLogMaxRotations = (uint8_t)gSettings.sensorLogMaxRotations;

  if (gSettings.healthTrackingEnabled) {
    gSensorLogMask = (uint8_t)(gSensorLogMask | LOG_R1);
    if (gSensorLogFormat == SENSOR_LOG_TRACK) gSensorLogFormat = SENSOR_LOG_CSV;
    if (gSettings.sensorLogPath.length() == 0 || gSettings.sensorLogPath.indexOf("health") < 0) {
      // Keep a stable health path for resumed sessions (timestamp appended below).
      gSettings.sensorLogPath = CAPTURE_HEALTHLOG_DEFAULT;
    }
  }

  if (gSensorLogMask == 0) {
    broadcastOutput("[sensorlog] Auto-start skipped: no sensors selected");
    return;
  }

  String path = gSettings.sensorLogPath;
  if (path.length() == 0 || path.charAt(0) != '/') {
    path = CAPTURE_SENSORLOG_DEFAULT;
  }

  // Session subfolder + timestamp shaping (shared with healthtrack starts).
  path = shapeSessionPath(path);

  // No mkdir here: this used to hand-create /logs + /logs/sensors, which is
  // both the wrong tree now and redundant — cmd_sensorlog below creates every
  // parent recursively, and the boot block already makes the capture tree.

  // Build and execute the CLI command so all validation/space checks run
  char cmd[320];
  snprintf(cmd, sizeof(cmd), "sensorlog start %s %d", path.c_str(), (int)gSensorLogIntervalMs);
  BROADCAST_PRINTF("[sensorlog] Auto-start: %s", cmd);
  const char* result = cmd_sensorlog(String(cmd).substring(10));  // Pass everything after "sensorlog " (10 chars)
  if (result && strncmp(result, "SUCCESS", 7) != 0) {
    // Command failed - broadcast the error
    BROADCAST_PRINTF("[sensorlog] Auto-start failed: %s", result);
    logSystemEvent("LOG", "sensor-log autostart FAILED: %s", result);
  } else {
    logSystemEvent("LOG", "sensor-log autostart OK → %s", path.c_str());
  }
}
