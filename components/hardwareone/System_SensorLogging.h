/**
 * Sensor Logging System - Data logging for sensor readings
 * 
 * Provides configurable logging of sensor data to files with:
 * - Selectable sensors (thermal, tof, imu, gamepad, apds, gps)
 * - Configurable intervals and file sizes
 * - Text and CSV output formats
 * - Log rotation support
 */

#ifndef SENSOR_LOGGING_H
#define SENSOR_LOGGING_H

#include <Arduino.h>
// Required: this header gates the R1 health declarations on ENABLE_R1_HEALTH.
// Without it the macro is undefined here, every `#if ENABLE_R1_HEALTH` below
// silently evaluates false, and the skipped `extern` leaves the matching
// `const CommandEntry healthCommands[]` definition with INTERNAL linkage
// (const implies static at namespace scope in C++) — which links fine until
// another TU that does see the macro references it. Same trap as any
// gated-include/ungated-use pair.
#include "System_BuildConfig.h"

// Sensor selection bitmask
#define LOG_THERMAL  (1 << 0)
#define LOG_TOF      (1 << 1)
#define LOG_IMU      (1 << 2)
#define LOG_GAMEPAD  (1 << 3)
#define LOG_APDS     (1 << 4)
#define LOG_GPS      (1 << 5)
#define LOG_PRESENCE (1 << 6)
#define LOG_R1       (1 << 7)   // Even R1 ring vitals (HR/HRV/SpO2/temp/battery/wear)

// Selection names and display labels belong to the logger, independent of
// which sensor drivers a build includes. Keep the persisted mask bits stable.
struct SensorLogSensor {
  uint8_t bit;
  const char* name;
  const char* label;
};
inline constexpr SensorLogSensor kSensorLogSensors[] = {
  { LOG_THERMAL,  "thermal",  "Thermal"   },
  { LOG_TOF,      "tof",      "ToF"       },
  { LOG_IMU,      "imu",      "IMU"       },
  { LOG_GAMEPAD,  "gamepad",  "Gamepad"   },
  { LOG_APDS,     "apds",     "APDS"      },
  { LOG_GPS,      "gps",      "GPS"       },
  { LOG_PRESENCE, "presence", "Presence"  },
  { LOG_R1,       "r1",       "R1 Health" },
};
inline constexpr size_t kSensorLogSensorCount =
    sizeof(kSensorLogSensors) / sizeof(kSensorLogSensors[0]);

// A scalar presentation read, not a synchronized snapshot of logger state.
// UI changes go through the authorized command executor: `sensorlog sensors
// toggle <name>` applies to the selection when executed, not when rendered.
uint8_t sensorLogSelectedMask();

// Snapshot of sensor cache and flags for logging
struct SensorCacheSnapshot {
  // flags
  bool gThermalRunning;
  bool gThermalConnected;
  bool thermalValid;
  bool gTofRunning;
  bool gTofConnected;
  bool tofValid;
  bool gImuRunning;
  bool gImuConnected;
  bool gInputRunning;
  bool gInputConnected;
  bool inputValid;
  bool gApdsColorRunning;
  bool gApdsProximityRunning;
  bool gApdsGestureRunning;
  bool gApdsConnected;
  bool apdsValid;
  // thermal summary
  float thermalMin;
  float thermalAvg;
  float thermalMax;
  // tof objects (max 4)
  int tofTotal;
  struct {
    bool valid;
    int distance_mm;
    bool detected;
    int status;  // VL53L5CX status code
  } tof[4];
  // imu
  float yaw, pitch, roll;
  float ax, ay, az;
  float gx, gy, gz;  // gyro
  float imuTemp;     // IMU internal temperature
  // gamepad
  uint32_t buttons;
  int joyX, joyY;
  // apds
  uint16_t apdsRed, apdsGreen, apdsBlue, apdsClear;
  uint8_t apdsProximity;
  uint8_t apdsGesture;
  // gps
  bool gGpsRunning;
  bool gGpsConnected;
  bool gpsFix;
  float gpsLatitude;
  float gpsLongitude;
  float gpsAltitude;
  float gpsSpeed;
  uint8_t gpsSatellites;
  uint8_t gpsFixQuality;
  uint8_t gpsHour;
  uint8_t gpsMinute;
  uint8_t gpsSecond;
  bool gpsHasTime;
  // presence
  bool gPresenceRunning;
  bool gPresenceConnected;
  float presenceAmbientTemp;
  int16_t presenceValue;
  int16_t motionValue;
  bool presenceDetected;
  bool motionDetected;
  // R1 ring vitals (BLE cache snapshot)
  bool r1Connected;
  bool r1HrValid;
  bool r1HrvValid;
  bool r1Spo2Valid;
  bool r1TempValid;
  bool r1BatteryValid;
  bool r1WearValid;
  uint8_t r1Hr;
  int16_t r1Hrv;
  uint8_t r1Spo2;
  int16_t r1TempTenths;  // °C × 10
  uint8_t r1Battery;
  uint8_t r1Wear;        // 0=unknown 1=notWear 2=wear 3=possiblyWear
};

// Sensor log format options
enum SensorLogFormat {
  SENSOR_LOG_TEXT = 0,
  SENSOR_LOG_CSV  = 1,
  SENSOR_LOG_TRACK = 2   // GPS-only compact track format with signal loss dedup
};

// Scalar presentation read; format changes use the authorized logger commands.
SensorLogFormat sensorLogCurrentFormat();
uint32_t sensorLogIntervalMs();

// Boot-local identity of the active capture (0 when stopped). A new successful
// start gets a new ID, even at the same path; rotation retains it. The guarded
// stop checks identity and stops together, so a consumer cannot stop a
// replacement capture. These do not synchronize the logger's sample buffers.
uint32_t sensorLogCurrentSessionId();
bool sensorLogStopSession(uint32_t sessionId);

// Sensor logging state (extern for access from other modules)
extern bool gSensorLoggingRunning;
extern String gSensorLogPath;
// Sensor logging functions (called from main loop)
void sensorLogTick();

// Start a timestamped session with saved parameters, without the boot-only
// health-policy overrides. For already-authorized manual setup callers; this
// performs normal start validation and returns the same borrowed command result
// (consume it before another command). This is not a synchronization boundary.
const char* sensorLogStartConfiguredSession();

// Live map recording: start a GPS-only TRACK capture at a literal path without
// saving it as the last-used session. The live format/mask switch for this
// capture only; the logger restores the saved format, mask and interval when
// the session ends by any stop path, so boot auto-start and the next typed
// start resume the user's own selection. Same admission checks, borrowed
// result and session identity as a typed start.
const char* sensorLogStartLiveTrack(const char* path, uint32_t intervalMs);

// Auto-start logging with persisted parameters and boot health policy.
void sensorLogAutoStart();

// R1 Health logging — HardwareOne's local durable-vitals capture. This is
// independent from the ring's own health-collection privacy setting.
// The authorized `healthlogging` command turns LOG_R1 on, coerces format to CSV,
// starts the sensor logger under
// /logging_captures/sensors/ (per-day file when synced, boot-<N>/ when dark),
// and persists healthLoggingEnabled (+ sensorlog autostart) so boot resumes.
// Off removes LOG_R1; stops logging when no other sensors remain.
bool healthLoggingIsActive();
// The R1 health logging commands exist only when the feature is compiled — they
// are registered through healthCommands[] below, not sensorLoggingCommands[].
#if ENABLE_R1_HEALTH
const char* cmd_healthlogging(const String& argsInput);
// Byte-concatenate sensor logs in the caller's order (same pattern as
// gpstrackmerge — and the same defects; see that command's usage text).
// Not TEXT-only despite the name: an extensionless output gets .csv appended.
const char* cmd_healthlogmerge(const String& argsInput);
#endif
// At-rest sealing mode/status/export — docs/HEALTH_AT_REST_ENCRYPTION_PLAN.md
const char* cmd_capturecrypt(const String& argsInput);

// Snapshot vitals + Track into buf (JSON object). Returns buf, or "{}" on failure.
// Shared by `ring health json` CLI/BLE and GET /api/health/status.
const char* buildHealthStatusJson(char* buf, size_t cap);

// Kick the exact-2.2.9 one-shot refresh (DAILY HR/HRV/SpO2/sleep/activity,
// followed by deviceStatus). Advances in healthLoggingTick; returns false if
// the ring is disconnected, setup/profile is not ready, the exact profile is
// unsupported, or Health is off in this build. This public action never falls
// back to the legacy POINT lane.
bool healthStartPollBurst(void);
bool healthPollBurstActive(void);

// Main-loop tick: advances the exact-2.2.9 on-demand refresh above. Legacy
// timed logging retains its profile-gated POINT miner and therefore sends no
// timed query when POINT is unsupported on the active profile.
void healthLoggingTick();

// Call after a Health-page refresh completes. Schedules a log sample once
// notify replies have had time to land, so successful/partial on-demand checks
// are persisted rather than relying on the legacy timed miner.
void healthLoggingNotePageRefresh();

// Request an immediate sensorlog write on the next sensorLogTick (bypasses
// the normal interval). bypassR1Dedup=true writes even if R1 values are unchanged.
void sensorLogRequestSample(bool bypassR1Dedup);

// Command handler
const char* cmd_sensorlog(const String& originalCmd);
// Admin-only numeric editors retain their settings registry entries.
const char* cmd_sensorlogmask(const String& args);
const char* cmd_sensorlogformat(const String& args);

// Command registry
struct CommandEntry;
extern const CommandEntry sensorLoggingCommands[];
extern const size_t sensorLoggingCommandsCount;
#if ENABLE_R1_HEALTH
extern const CommandEntry healthCommands[];
extern const size_t healthCommandsCount;
#endif

// Settings module
struct SettingsModule;
extern const SettingsModule sensorLogSettingsModule;

#endif // SENSOR_LOGGING_H
