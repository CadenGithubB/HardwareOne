// HAL_Motion.h - the motion sensor (IMU) layer: one reading contract and one
// API whichever chip the build carries.
//
// The device type stays I2C_DEVICE_IMU (feature "imu", commands "imu*"). One
// backend per build, chosen by IMU_DEVICE_TYPE (System_BuildConfig.h):
//   1 = BNO055        HAL_Motion_BNO055.cpp (fuses on the chip)
//   2 = Adafruit 4517 (LSM6DSOX + LIS3MDL), a later backend
// HAL_Motion.cpp owns the cache, the task, the start/stop lifecycle, the JSON
// builder, the action detector, the commands and the settings; a backend only
// starts its chip and reads one sample (HAL_MotionBackend.h).
#ifndef HAL_MOTION_H
#define HAL_MOTION_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stddef.h>

// One reading, always available for type-safe references. Units: accel m/s^2,
// gyro rad/s, mag uT, Euler degrees (yaw 0..360, pitch +-90, roll +-180),
// temp Celsius.
struct ImuCache {
  float accelX = 0.0, accelY = 0.0, accelZ = 0.0;
  float gyroX = 0.0, gyroY = 0.0, gyroZ = 0.0;
  float imuTemp = 0.0;
  float oriYaw = 0.0, oriPitch = 0.0, oriRoll = 0.0;  // Euler angles (degrees)
  float magX = 0.0, magY = 0.0, magZ = 0.0;           // when hasMag
  unsigned long imuLastUpdate = 0;
  bool imuDataValid = false;
  uint32_t imuSeq = 0;
  bool hasMag = false;          // the backend reads a magnetometer
  bool hasOrientation = false;  // ori is filled (the chip's or the layer's fusion)
  bool hasHeading = false;      // yaw is a heading from magnetic north, not a relative angle
  bool hasCal = false;          // cal carries the backend's calibration state
  uint8_t cal = 0;              // BNO055: CALIB_STAT (sys, gyro, accel, mag: 2 bits each, 3 = done)
  uint8_t backend = 0;          // kMotionBackend* (HAL_MotionCore.h); 0 = none
};

// Values only: callers own their copy; the live cache and lock stay in the layer.
#if ENABLE_IMU_SENSOR

// Forward declarations
class String;
struct MotionActionState;

// IMU watermark tracking
extern volatile UBaseType_t gImuWatermarkMin;
extern volatile UBaseType_t gImuWatermarkNow;

// IMU initialization handoff flags
extern volatile bool gImuInitRequested;
extern volatile bool gImuInitDone;
extern volatile bool gImuInitResult;

// IMU command handlers
const char* cmd_imu(const String& argsInput);
const char* cmd_imustart(const String& argsInput);
const char* cmd_imustop(const String& argsInput);
const char* cmd_imuactions(const String& argsInput);
// IMU web view settings (read by the web Sensors page)
const char* cmd_imupollingms(const String& argsInput);
const char* cmd_imuewmafactor(const String& argsInput);
const char* cmd_imutransitionms(const String& argsInput);
const char* cmd_imuwebmaxfps(const String& argsInput);
// IMU device settings
const char* cmd_imudevicepollms(const String& argsInput);
const char* cmd_imuorientationmode(const String& argsInput);
const char* cmd_imuorientationcorrection(const String& argsInput);
const char* cmd_imupitchoffset(const String& argsInput);
const char* cmd_imurolloffset(const String& argsInput);
const char* cmd_imuyawoffset(const String& argsInput);
const char* cmd_imuaddress(const String& argsInput);

// IMU state and control
extern bool gImuRunning;
extern bool gImuConnected;
extern unsigned long gImuLastStopTime;
extern TaskHandle_t gImuTaskHandle;

// Internal function called by queue processor
bool imuStartInternal();

// The IMU's address: the one it was opened at while it is open (a changed
// imuaddress applies at the next openimu), else its imuAddress setting
// through the backend's rule; and the chip name of this build's backend
// ("BNO055").
uint8_t imuDeviceAddress();
const char* imuBackendName();

// The action detector runs once per sample on the IMU task (postEvents: the
// task posts the imu_* events). Other callers read its state with
// imuActionsSnapshot() and never run it, so renders do not count as samples.
void imuUpdateActions(bool postEvents = false);
bool imuActionsSnapshot(MotionActionState& out, uint32_t timeoutMs = 10);

// JSON building
int imuBuildDataJson(char* buf, size_t bufSize);

// Copy one reading; false clears out when uninitialized or the lock is busy.
// Success only means a copy was available; check imuDataValid where required.
bool imuCacheSnapshot(ImuCache& out, uint32_t timeoutMs = 10);
// Static cache data plus its mutex handle, for memory attribution (not RTOS allocation).
size_t imuCacheStorageBytes();

#else
// The IMU is not in this build. Its state reads as stopped and a start
// request fails, so callers need no build condition.
inline bool gImuRunning = false;
inline bool gImuConnected = false;
inline unsigned long gImuLastStopTime = 0;
inline TaskHandle_t gImuTaskHandle = nullptr;
inline bool imuStartInternal() { return false; }
inline void imuUpdateActions(bool postEvents = false) { (void)postEvents; }
inline bool imuCacheSnapshot(ImuCache& out, uint32_t timeoutMs = 10) {
  (void)timeoutMs;
  out = {};
  return false;
}
inline int imuBuildDataJson(char* buf, size_t bufSize) {
  if (buf && bufSize) buf[0] = '\0';
  return 0;
}
inline size_t imuCacheStorageBytes() { return 0; }
#endif // ENABLE_IMU_SENSOR
#endif // HAL_MOTION_H
