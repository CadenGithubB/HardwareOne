#ifndef I2CSENSOR_BNO055_H
#define I2CSENSOR_BNO055_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stddef.h>

// IMU sensor cache structure (always available for type-safe references)
struct ImuCache {
  float accelX = 0.0, accelY = 0.0, accelZ = 0.0;
  float gyroX = 0.0, gyroY = 0.0, gyroZ = 0.0;
  float imuTemp = 0.0;
  float oriYaw = 0.0, oriPitch = 0.0, oriRoll = 0.0;  // Euler angles (degrees)
  unsigned long imuLastUpdate = 0;
  bool imuDataValid = false;
  uint32_t imuSeq = 0;
};

// Values only: callers own their copy; the live cache and lock stay in the driver.
#if ENABLE_IMU_SENSOR

// Forward declarations
class String;

// IMU watermark tracking
extern volatile UBaseType_t gImuWatermarkMin;
extern volatile UBaseType_t gImuWatermarkNow;

// IMU initialization handoff flags
extern volatile bool gImuInitRequested;
extern volatile bool gImuInitDone;
extern volatile bool gImuInitResult;

// IMU sensor command handlers
const char* cmd_imu(const String& argsInput);
const char* cmd_imustart(const String& argsInput);
const char* cmd_imustop(const String& argsInput);
const char* cmd_imuactions(const String& argsInput);
// IMU UI Settings Commands
const char* cmd_imupollingms(const String& argsInput);
const char* cmd_imuewmafactor(const String& argsInput);
const char* cmd_imutransitionms(const String& argsInput);
const char* cmd_imuwebmaxfps(const String& argsInput);
// IMU Device Settings Commands
const char* cmd_imudevicepollms(const String& argsInput);
const char* cmd_imuorientationmode(const String& argsInput);
const char* cmd_imuorientationcorrection(const String& argsInput);
const char* cmd_imupitchoffset(const String& argsInput);
const char* cmd_imurolloffset(const String& argsInput);
const char* cmd_imuyawoffset(const String& argsInput);

// IMU sensor state and control
extern bool gImuRunning;
extern bool gImuConnected;
extern unsigned long gImuLastStopTime;
extern TaskHandle_t gImuTaskHandle;

// Internal function called by queue processor
bool imuStartInternal();

// IMU sensor functions
bool imuInit();
void imuPoll();
void imuApplyOrientationCorrection(float& pitch, float& roll, float& yaw);

// IMU action detection
// postEvents: only the IMU poll task passes true — the OLED IMU page and
// cmd_imuactions also call this for fresh display state, and event posts /
// debounce counters from those callers would count renders instead of
// samples and double-post across cores.
void imuUpdateActions(bool postEvents = false);

// JSON building
int imuBuildDataJSON(char* buf, size_t bufSize);

// IMU command registration (sensor-specific naming)
void registerImuBno055Commands();

// Command registry (for System_CommandRegistry.cpp module list)
struct CommandEntry;
extern const CommandEntry imuCommands[];
extern const size_t imuCommandsCount;

// Copy one reading; false clears out when uninitialized or the lock is busy.
// Success only means a copy was available; check imuDataValid where required.
bool imuCacheSnapshot(ImuCache& out, uint32_t timeoutMs = 10);
// Static cache data plus its mutex handle, for memory attribution (not RTOS allocation).
size_t imuCacheStorageBytes();

#else
inline bool imuCacheSnapshot(ImuCache& out, uint32_t timeoutMs = 10) {
  (void)timeoutMs;
  out = {};
  return false;
}
inline int imuBuildDataJSON(char* buf, size_t bufSize) {
  if (buf && bufSize) buf[0] = '\0';
  return 0;
}
inline size_t imuCacheStorageBytes() { return 0; }
#endif // ENABLE_IMU_SENSOR
#endif // IMU_SENSOR_H
