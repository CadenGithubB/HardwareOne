#ifndef I2CSENSOR_BNO055_H
#define I2CSENSOR_BNO055_H

#include "System_BuildConfig.h"

#if ENABLE_IMU_SENSOR

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Forward declarations
class String;
class Adafruit_BNO055;

// BNO055 sensor object (defined in imu_sensor.cpp)
extern Adafruit_BNO055* gBNO055;

// IMU initialization handoff variables
extern volatile bool imuInitRequested;
extern volatile bool imuInitDone;
extern volatile bool imuInitResult;

// IMU sensor cache (small data, 5Hz updates)
struct ImuCache {
  SemaphoreHandle_t mutex = nullptr;
  float accelX = 0.0, accelY = 0.0, accelZ = 0.0;
  float gyroX = 0.0, gyroY = 0.0, gyroZ = 0.0;
  float imuTemp = 0.0;
  float oriYaw = 0.0, oriPitch = 0.0, oriRoll = 0.0;  // Euler angles (degrees)
  unsigned long imuLastUpdate = 0;
  bool imuDataValid = false;
  uint32_t imuSeq = 0;
};

// Global IMU cache (defined in imu_sensor.cpp)
extern ImuCache gImuCache;

// IMU Action Detection System
struct IMUActionState {
  // Shake detection
  bool isShaking;
  unsigned long lastShakeMs;
  uint32_t shakeCount;
  float shakeIntensity;  // 0.0 to 1.0

  // Tilt detection
  bool isTilted;
  float tiltAngle;     // Degrees from horizontal
  char tiltDirection;  // 'F'=forward, 'B'=back, 'L'=left, 'R'=right, 'N'=none

  // Tap/knock detection
  bool tapDetected;
  unsigned long lastTapMs;
  uint32_t tapCount;
  float tapStrength;  // 0.0 to 1.0

  // Rotation detection
  bool isRotating;
  float rotationRate;  // deg/s
  char rotationAxis;   // 'X', 'Y', 'Z', or 'N' for none

  // Freefall detection
  bool isFreefalling;
  unsigned long freefallStartMs;
  uint32_t freefallDurationMs;

  // Step counting
  bool isWalking;
  uint32_t stepCount;
  unsigned long lastStepMs;
  float stepFrequency;  // steps per minute

  // Orientation detection
  char orientation;  // 'P'=portrait, 'L'=landscape, 'U'=upside-down portrait, 'R'=reverse landscape, 'F'=face-up, 'D'=face-down
  char lastOrientation;
  unsigned long lastOrientationChangeMs;

  // Internal state for detection algorithms
  float accelHistory[10];  // Rolling buffer for acceleration magnitude
  int accelHistoryIndex;
  unsigned long lastUpdateMs;
  float baselineAccel;  // Baseline for freefall detection (~9.8 m/s²)

  // Step detection state
  float lastAccelMag;
  bool stepPeakDetected;
  unsigned long stepWindowStartMs;
  uint32_t stepsInWindow;
};

// Global IMU action state (defined in imu_sensor.cpp)
extern IMUActionState gIMUActions;

// IMU watermark tracking
extern volatile UBaseType_t gIMUWatermarkMin;
extern volatile UBaseType_t gIMUWatermarkNow;

// IMU initialization handoff flags
extern volatile bool imuInitRequested;
extern volatile bool imuInitDone;
extern volatile bool imuInitResult;

// IMU sensor command handlers
const char* cmd_imu(const String& cmd);
const char* cmd_imustart(const String& cmd);
const char* cmd_imustop(const String& cmd);
const char* cmd_imuactions(const String& cmd);
// IMU UI Settings Commands
const char* cmd_imupollingms(const String& cmd);
const char* cmd_imuewmafactor(const String& cmd);
const char* cmd_imutransitionms(const String& cmd);
const char* cmd_imuwebmaxfps(const String& cmd);
// IMU Device Settings Commands
const char* cmd_imudevicepollms(const String& cmd);
const char* cmd_imuorientationmode(const String& cmd);
const char* cmd_imuorientationcorrection(const String& cmd);
const char* cmd_imupitchoffset(const String& cmd);
const char* cmd_imurolloffset(const String& cmd);
const char* cmd_imuyawoffset(const String& cmd);

// IMU sensor state and control
extern bool imuEnabled;
extern bool imuConnected;
extern unsigned long imuLastStopTime;
extern TaskHandle_t imuTaskHandle;

// Internal function called by queue processor
bool startIMUSensorInternal();

// IMU sensor functions
bool initIMUSensor();
void readIMUSensor();
void applyIMUOrientationCorrection(float& pitch, float& roll, float& yaw);

// IMU action detection
void updateIMUActions();

// JSON building
int buildIMUDataJSON(char* buf, size_t bufSize);

// IMU command registration (sensor-specific naming)
void registerImuBno055Commands();

// Command registry (for system_utils.cpp module list)
struct CommandEntry;
extern const CommandEntry imuCommands[];
extern const size_t imuCommandsCount;

#endif // ENABLE_IMU_SENSOR
#endif // IMU_SENSOR_H
