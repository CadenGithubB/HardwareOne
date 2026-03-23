#ifndef I2CSENSOR_BNO055_H
#define I2CSENSOR_BNO055_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// IMU sensor cache structure (always available for type-safe references)
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

// IMU Action Detection System (always available for type-safe references)
struct IMUActionState {
  bool isShaking;
  unsigned long lastShakeMs;
  uint32_t shakeCount;
  float shakeIntensity;

  bool isTilted;
  float tiltAngle;
  char tiltDirection;  // 'F'=forward, 'B'=back, 'L'=left, 'R'=right, 'N'=none

  bool tapDetected;
  unsigned long lastTapMs;
  uint32_t tapCount;
  float tapStrength;

  bool isRotating;
  float rotationRate;  // deg/s
  char rotationAxis;   // 'X', 'Y', 'Z', or 'N' for none

  bool isFreefalling;
  unsigned long freefallStartMs;
  uint32_t freefallDurationMs;

  bool isWalking;
  uint32_t stepCount;
  unsigned long lastStepMs;
  float stepFrequency;

  char orientation;  // 'P'=portrait, 'L'=landscape, 'U'=upside-down, 'R'=reverse, 'F'=face-up, 'D'=face-down
  char lastOrientation;
  unsigned long lastOrientationChangeMs;

  float accelHistory[10];
  int accelHistoryIndex;
  unsigned long lastUpdateMs;
  float baselineAccel;

  float lastAccelMag;
  bool stepPeakDetected;
  unsigned long stepWindowStartMs;
  uint32_t stepsInWindow;
};

#if ENABLE_IMU_SENSOR

// Forward declarations
class String;
class Adafruit_BNO055;

extern Adafruit_BNO055* gBNO055;

extern volatile bool imuInitRequested;
extern volatile bool imuInitDone;
extern volatile bool imuInitResult;

extern ImuCache gImuCache;
extern IMUActionState gIMUActions;

// IMU watermark tracking
extern volatile UBaseType_t gIMUWatermarkMin;
extern volatile UBaseType_t gIMUWatermarkNow;

// IMU initialization handoff flags
extern volatile bool imuInitRequested;
extern volatile bool imuInitDone;
extern volatile bool imuInitResult;

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
