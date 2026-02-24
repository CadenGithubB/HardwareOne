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

// Snapshot of sensor cache and flags for logging
struct SensorCacheSnapshot {
  // flags
  bool thermalEnabled;
  bool thermalConnected;
  bool thermalValid;
  bool tofEnabled;
  bool tofConnected;
  bool tofValid;
  bool imuEnabled;
  bool imuConnected;
  bool gamepadEnabled;
  bool gamepadConnected;
  bool gamepadValid;
  bool apdsColorEnabled;
  bool apdsProximityEnabled;
  bool apdsGestureEnabled;
  bool apdsConnected;
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
  uint32_t gamepadButtons;
  int gamepadX, gamepadY;
  // apds
  uint16_t apdsRed, apdsGreen, apdsBlue, apdsClear;
  uint8_t apdsProximity;
  uint8_t apdsGesture;
  // gps
  bool gpsEnabled;
  bool gpsConnected;
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
  bool presenceEnabled;
  bool presenceConnected;
  float presenceAmbientTemp;
  int16_t presenceValue;
  int16_t motionValue;
  bool presenceDetected;
  bool motionDetected;
};

// Sensor log format options
enum SensorLogFormat {
  SENSOR_LOG_TEXT = 0,
  SENSOR_LOG_CSV  = 1,
  SENSOR_LOG_TRACK = 2   // GPS-only compact track format with signal loss dedup
};

// Sensor logging state (extern for access from other modules)
extern bool gSensorLoggingEnabled;
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
