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

// Sensor selection bitmask
#define LOG_THERMAL  (1 << 0)
#define LOG_TOF      (1 << 1)
#define LOG_IMU      (1 << 2)
#define LOG_GAMEPAD  (1 << 3)
#define LOG_APDS     (1 << 4)
#define LOG_GPS      (1 << 5)
#define LOG_PRESENCE (1 << 6)
#define LOG_R1       (1 << 7)   // Even R1 ring vitals (HR/HRV/SpO2/temp/battery/wear)

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
  uint8_t r1Wear;        // 0=unknown 1=notWear 2=wear
};

// Sensor log format options
enum SensorLogFormat {
  SENSOR_LOG_TEXT = 0,
  SENSOR_LOG_CSV  = 1,
  SENSOR_LOG_TRACK = 2   // GPS-only compact track format with signal loss dedup
};

// Sensor logging state (extern for access from other modules)
extern bool gSensorLoggingRunning;
extern String gSensorLogPath;
extern uint32_t gSensorLogIntervalMs;
extern size_t gSensorLogMaxSize;
extern SensorLogFormat gSensorLogFormat;
extern uint8_t gSensorLogMaxRotations;
extern uint8_t gSensorLogMask;
// Sensor logging functions (called from main loop)
void sensorLogTick();

// Auto-start logging with persisted parameters (called from boot)
void sensorLogAutoStart();

// R1 Health logging — HardwareOne's local durable-vitals capture. This is
// independent from the ring's own health-collection privacy setting.
// Turns LOG_R1 on, coerces format to CSV, starts the sensor logger under
// /logging_captures/sensors/ (per-day file when synced, boot-<N>/ when dark),
// and persists healthLoggingEnabled (+ sensorlog autostart) so boot resumes.
// Off removes LOG_R1; stops logging when no other sensors remain.
bool healthLoggingIsActive();
const char* healthLoggingSet(bool on);   // returns SUCCESS:/Error: message (static/debug buf)
const char* cmd_healthlogging(const String& argsInput);
const char* cmd_healthstatus(const String& argsInput);
// Byte-concatenate sensor logs in the caller's order (same pattern as
// gpstrackmerge — and the same defects; see that command's usage text).
// Not TEXT-only despite the name: an extensionless output gets .csv appended.
const char* cmd_healthlogmerge(const String& argsInput);
// At-rest sealing mode/status/export — docs/HEALTH_AT_REST_ENCRYPTION_PLAN.md
const char* cmd_capturecrypt(const String& argsInput);

// Snapshot vitals + Track into buf (JSON object). Returns buf, or "{}" on failure.
// Shared by `healthstatus json` CLI/BLE and GET /api/health/status.
const char* buildHealthStatusJson(char* buf, size_t cap);

// Kick a one-shot 4-vital poll burst (HR/HRV/SpO2/battery). Advances in
// healthLoggingTick; returns false if ring not connected or Health off in build.
bool healthStartPollBurst(void);

// Main-loop tick: when Health logging is on, periodically mines the R1
// (HR/HRV/SpO2/battery poll burst) at healthLoggingPollIntervalSec and forces
// a sensorlog sample after replies settle. Also advances on-demand poll bursts.
void healthLoggingTick();

// Call after a Health-page vitals refresh (entry / Poll Now burst completes).
// Schedules a log sample once notify replies have had time to land — so
// on-demand checks are persisted, not only the timed mine.
void healthLoggingNotePageRefresh();

// Request an immediate sensorlog write on the next sensorLogTick (bypasses
// the normal interval). bypassR1Dedup=true writes even if R1 values are unchanged.
void sensorLogRequestSample(bool bypassR1Dedup);

// Command handler
const char* cmd_sensorlog(const String& originalCmd);

// Command registry
struct CommandEntry;
extern const CommandEntry sensorLoggingCommands[];
extern const size_t sensorLoggingCommandsCount;

// Settings module
struct SettingsModule;
extern const SettingsModule sensorLogSettingsModule;

#endif // SENSOR_LOGGING_H
