#include "i2csensor-bno055.h"
#include "System_BuildConfig.h"
#include "System_MemoryMonitor.h"
#include "System_Utils.h"

#if ENABLE_IMU_SENSOR

#include <Adafruit_BNO055.h>
#include <Adafruit_Sensor.h>
#include <Arduino.h>
#include <utility/imumaths.h>

#include "OLED_Display.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_ESPNow.h"
#include "System_ESPNow_Sensors.h"
#include "System_I2C.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"

// BNO055 sensor object (owned by this module)
Adafruit_BNO055* gBNO055 = nullptr;

// IMU initialization handoff variables
volatile bool imuInitRequested = false;
volatile bool imuInitDone = false;
volatile bool imuInitResult = false;

// Settings and debug
extern Settings gSettings;
// sensorStatusBumpWith, gSensorPollingPaused, i2cMutex, drainDebugRing provided by System_I2C.h

// ============================================================================
// IMU Sensor Cache (owned by this module)
// ============================================================================
ImuCache gImuCache;

// IMU Action Detection System - definitions
IMUActionState gIMUActions = {
  false, 0, 0, 0.0f,  // shake
  false, 0.0f, 'N',   // tilt
  false, 0, 0, 0.0f,  // tap
  false, 0.0f, 'N',   // rotation
  false, 0, 0,        // freefall
  false, 0, 0, 0.0f,  // steps
  'F', 'F', 0,        // orientation
  { 0 }, 0, 0, 9.8f,  // internal
  0.0f, false, 0, 0   // step detection
};

// IMU watermark tracking
volatile UBaseType_t gIMUWatermarkMin = (UBaseType_t)0xFFFFFFFF;
volatile UBaseType_t gIMUWatermarkNow = (UBaseType_t)0;

// IMU initialization handoff flags - defined above (lines 17-19)

// Macro for validation
#define RETURN_VALID_IF_VALIDATE_CSTR() \
  do { \
    extern bool gCLIValidateOnly; \
    if (gCLIValidateOnly) return "VALID"; \
  } while(0)

// Debug macros (use centralized versions from debug_system.h)
// BROADCAST_PRINTF now defined in debug_system.h with performance optimizations
#define MIN_RESTART_DELAY_MS 2000

// Queue system functions now in System_I2C.h

// IMU sensor state (definitions)
bool imuEnabled = false;
bool imuConnected = false;
unsigned long imuLastStopTime = 0;
TaskHandle_t imuTaskHandle = nullptr;

// Forward declarations
extern bool createIMUTask();

// ============================================================================
// IMU Sensor Command Handlers
// ============================================================================

const char* cmd_imu(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!imuConnected || !imuEnabled) {
    broadcastOutput("IMU sensor not connected or not started. Use 'imustart' first.");
    return "ERROR";
  }

  // Read from sensor cache instead of accessing hardware directly
  if (gImuCache.mutex && xSemaphoreTake(gImuCache.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (gImuCache.imuDataValid) {
      BROADCAST_PRINTF("Orientation - Yaw: %.1f° Pitch: %.1f° Roll: %.1f°",
                       gImuCache.oriYaw, gImuCache.oriPitch, gImuCache.oriRoll);
      BROADCAST_PRINTF("Acceleration - X: %.2f Y: %.2f Z: %.2f m/s²",
                       gImuCache.accelX, gImuCache.accelY, gImuCache.accelZ);
      BROADCAST_PRINTF("Gyroscope - X: %.2f Y: %.2f Z: %.2f rad/s",
                       gImuCache.gyroX, gImuCache.gyroY, gImuCache.gyroZ);
      BROADCAST_PRINTF("Temperature: %.1f°C", gImuCache.imuTemp);
    } else {
      broadcastOutput("IMU data not yet available");
    }
    xSemaphoreGive(gImuCache.mutex);
  } else {
    broadcastOutput("Failed to access sensor cache");
    return "ERROR";
  }

  return "OK";
}

// Internal function called by queue processor
bool startIMUSensorInternal() {
  // Check if too soon after stop (prevent rapid restart crashes)
  if (imuLastStopTime > 0) {
    unsigned long timeSinceStop = millis() - imuLastStopTime;
    if (timeSinceStop < MIN_RESTART_DELAY_MS) {
      DEBUG_CLIF("IMU sensor stopped recently, waiting before restart");
      return false;
    }
  }

  // Check memory before creating IMU task
  if (!checkMemoryAvailable("imu", nullptr)) {
    DEBUG_CLIF("Insufficient memory for IMU sensor");
    return false;
  }

  // Clean up any stale cache from previous run BEFORE starting
  // CRITICAL: Cache wasn't invalidated during stop to avoid dying-task crashes
  if (gImuCache.mutex && xSemaphoreTake(gImuCache.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    gImuCache.imuDataValid = false;
    gImuCache.oriYaw = 0;
    gImuCache.oriPitch = 0;
    gImuCache.oriRoll = 0;
    gImuCache.accelX = 0;
    gImuCache.accelY = 0;
    gImuCache.accelZ = 0;
    gImuCache.gyroX = 0;
    gImuCache.gyroY = 0;
    gImuCache.gyroZ = 0;
    gImuCache.imuTemp = 0;
    xSemaphoreGive(gImuCache.mutex);
    DEBUG_CLIF("[IMU_INTERNAL] Cleaned up stale cache from previous run");
  }

  // CRITICAL: Enable flag BEFORE creating task to prevent race condition
  // Task checks imuEnabled first thing and will delete itself if false
  bool prev = imuEnabled;
  imuEnabled = true;  // Set this BEFORE task creation

  // Defer initialization to imuTask; wait briefly for result
  if (gBNO055 == nullptr || !imuConnected) {
    imuInitDone = false;
    imuInitResult = false;
    imuInitRequested = true;
  }

  // Create IMU task lazily (after setting imuEnabled=true)
  if (!createIMUTask()) {
    DEBUG_CLIF("Failed to create IMU task (insufficient memory or resources)");
    imuEnabled = false;  // Reset flag on failure
    return false;
  }
  if (imuEnabled != prev) {
    sensorStatusBumpWith("openimu@queue");
  }

  // If init was requested, block up to 3s for a result so CLI returns accurate status
  if (imuInitRequested || gBNO055 == nullptr || !imuConnected) {
    unsigned long start = millis();
    while (!imuInitDone && (millis() - start) < 3000UL) {
      delay(10);
    }
    if (!imuInitDone) {
      imuEnabled = false;
      DEBUG_CLIF("Failed to initialize IMU sensor (timeout after 3s)");
      return false;
    }
    if (!imuInitResult) {
      imuEnabled = false;
      DEBUG_CLIF("Failed to initialize IMU sensor (initialization failed)");
      return false;
    }
  }
  DEBUG_CLIF("[IMU_INTERNAL] SUCCESS: BNO055 IMU sensor started");
  
  // Broadcast sensor status to ESP-NOW master
#if ENABLE_ESPNOW
  broadcastSensorStatus(REMOTE_SENSOR_IMU, true);
#endif
  
  return true;
}

// Public command - uses centralized queue
const char* cmd_imustart(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Check if already enabled or queued
  if (imuEnabled) {
    return "[IMU] Error: Already running";
  }
  if (isInQueue(I2C_DEVICE_IMU)) {
    int pos = getQueuePosition(I2C_DEVICE_IMU);
    BROADCAST_PRINTF("IMU sensor already queued (position %d)", pos);
    return "[IMU] Already queued";
  }

  // Enqueue the request to centralized queue
  if (enqueueDeviceStart(I2C_DEVICE_IMU)) {
    sensorStatusBumpWith("openimu@enqueue");
    int pos = getQueuePosition(I2C_DEVICE_IMU);
    BROADCAST_PRINTF("IMU sensor queued for open (position %d)", pos);
    return "[IMU] Sensor queued for open";
  } else {
    return "[IMU] Error: Failed to enqueue open (queue full)";
  }
}

const char* cmd_imustop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  handleDeviceStopped(I2C_DEVICE_IMU);
  return "[IMU] Close requested; cleanup will complete asynchronously";
}

const char* cmd_imuactions(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!imuEnabled || !imuConnected) {
    broadcastOutput("[IMU] Error: Not enabled. Use 'imustart' first.");
    return "ERROR";
  }

  // Update actions once to get current state
  updateIMUActions();

  // Stream output line-by-line instead of building in shared buffer
  broadcastOutput("IMU Action Detection Status:");

  // Shake
  if (gIMUActions.isShaking) {
    BROADCAST_PRINTF("  Shake: YES (intensity: %.1f, count: %lu)",
                     gIMUActions.shakeIntensity, (unsigned long)gIMUActions.shakeCount);
  } else {
    broadcastOutput("  Shake: no");
  }

  // Tilt
  if (gIMUActions.isTilted) {
    const char* dir = "?";
    switch (gIMUActions.tiltDirection) {
      case 'F': dir = "Forward"; break;
      case 'B': dir = "Back"; break;
      case 'L': dir = "Left"; break;
      case 'R': dir = "Right"; break;
    }
    BROADCAST_PRINTF("  Tilt: YES (%s, %.1f deg)", dir, gIMUActions.tiltAngle);
  } else {
    broadcastOutput("  Tilt: no");
  }

  // Tap
  if (gIMUActions.tapDetected || gIMUActions.tapCount > 0) {
    BROADCAST_PRINTF("  Tap: %s (count: %lu, strength: %.1f)",
                     gIMUActions.tapDetected ? "YES" : "no",
                     (unsigned long)gIMUActions.tapCount, gIMUActions.tapStrength);
  } else {
    broadcastOutput("  Tap: no");
  }

  // Rotation
  if (gIMUActions.isRotating) {
    BROADCAST_PRINTF("  Rotation: YES (%c-axis, %.1f deg/s)",
                     gIMUActions.rotationAxis, gIMUActions.rotationRate);
  } else {
    broadcastOutput("  Rotation: no");
  }

  // Freefall
  if (gIMUActions.isFreefalling) {
    BROADCAST_PRINTF("  Freefall: YES (%lu ms)", (unsigned long)gIMUActions.freefallDurationMs);
  } else {
    broadcastOutput("  Freefall: no");
  }

  // Steps
  if (gIMUActions.isWalking) {
    BROADCAST_PRINTF("  Steps: %lu (WALKING, %.1f steps/min)",
                     (unsigned long)gIMUActions.stepCount, gIMUActions.stepFrequency);
  } else {
    BROADCAST_PRINTF("  Steps: %lu", (unsigned long)gIMUActions.stepCount);
  }

  // Orientation
  const char* orient = "?";
  switch (gIMUActions.orientation) {
    case 'P': orient = "Portrait"; break;
    case 'L': orient = "Landscape"; break;
    case 'U': orient = "Upside-down"; break;
    case 'R': orient = "Reverse Landscape"; break;
    case 'F': orient = "Face-up"; break;
    case 'D': orient = "Face-down"; break;
  }
  BROADCAST_PRINTF("  Orientation: %s", orient);

  return "[IMU] Action status displayed";
}

// ============================================================================
// IMU Sensor Initialization and Reading Functions
// ============================================================================

bool initIMUSensor() {
  if (gBNO055 != nullptr) {
    broadcastOutput("[IMU] Error: Already initialized!");
    return true;
  }

  INFO_SENSORSF("Starting BNO055 IMU initialization (STEMMA QT)...");

  // Reset grace period for this initialization attempt (device may have been registered at boot)
  i2cResetGracePeriod(0x28);

  // Use i2cTransaction wrapper with long timeout for IMU init (can take several seconds with retries)
  // Probe for possible I2C addresses (A: 0x28, B: 0x29)
  uint8_t candidateAddrs[2] = { BNO055_ADDRESS_A, BNO055_ADDRESS_B };
  int foundIndex = -1;
  for (int i = 0; i < 2; i++) {
    if (i2cPingAddress(candidateAddrs[i], 100000, 200)) {
      foundIndex = i;
      break;
    }
  }

  return i2cDeviceTransaction(I2C_ADDR_IMU, 100000, 5000, [&]() -> bool {
    // Wire1 already initialized in setup() - no need to call begin() again
    INFO_SENSORSF("Starting IMU initialization at 100kHz I2C clock");

    // BNO055 needs time after power-up/reset before responding reliably
    delay(1000);

    if (foundIndex < 0) {
      WARN_SENSORSF("[IMU] Error: Not detected at 0x28 or 0x29 (initial probe). Will attempt init anyway with retries");
    } else {
      INFO_SENSORSF("Detected BNO055 at address 0x%02X", candidateAddrs[foundIndex]);
    }

    // Retry loop with conservative I2C clocks (BNO055 doesn't like high speeds)
    const int maxAttempts = 5;
    uint32_t clocks[maxAttempts] = { 100000, 100000, 50000, 100000, 100000 };
    
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
      DEBUG_SENSORSF("[IMU] Error: Init attempt %d/%d at I2C %lu Hz", attempt, maxAttempts, clocks[attempt - 1]);

      // Clock management now handled by I2CDeviceManager
      delay(150);

      // If we previously created an object, clean it up before retrying
      if (gBNO055 != nullptr) {
        delete gBNO055;
        gBNO055 = nullptr;
      }

      // If we detected an address, try that first; otherwise try both
      bool begun = false;
      for (int i = 0; i < 2 && !begun; i++) {
        uint8_t addr = (foundIndex >= 0) ? candidateAddrs[foundIndex] : candidateAddrs[i];
        INFO_SENSORSF("Trying BNO055 address 0x%02X", addr);
        gBNO055 = new Adafruit_BNO055(55, addr, &Wire1);
        if (gBNO055 == nullptr) {
          ERROR_SENSORSF("[IMU] Error: Failed to allocate memory for BNO055 object");
          return false;
        }
        delay(20);
        if (gBNO055->begin()) {
          begun = true;
          break;
        }
        // Failed begin on this addr
        delete gBNO055;
        gBNO055 = nullptr;
        delay(100);
      }

      if (begun) {
        // Success! Configure the sensor
        gBNO055->setExtCrystalUse(true);
        delay(100);
        imuConnected = true;
        
        INFO_SENSORSF("[IMU] BNO055 IMU sensor initialized successfully");
        return true;
      }

      // Failed this attempt, wait before next retry
      delay(500);
    }

    // All attempts failed
    if (gBNO055 != nullptr) {
      delete gBNO055;
      gBNO055 = nullptr;
    }
    ERROR_SENSORSF("[IMU] Error: Failed to initialize BNO055 IMU sensor after %d attempts", maxAttempts);
    broadcastOutput("[IMU] Error: Failed to initialize IMU sensor (timeout after 3s)");
    return false;
  });
}

// Apply IMU orientation correction based on physical mounting
void applyIMUOrientationCorrection(float& pitch, float& roll, float& yaw) {
  if (!gSettings.imuOrientationCorrectionEnabled) {
    return;
  }

  // Apply manual offsets first
  pitch += gSettings.imuPitchOffset;
  roll += gSettings.imuRollOffset;
  yaw += gSettings.imuYawOffset;

  // Apply orientation mode corrections for different physical mountings
  switch (gSettings.imuOrientationMode) {
    case 0:  // Normal - no correction
      break;
    case 1:  // Flip pitch (device upside down)
      pitch = -pitch;
      break;
    case 2:  // Flip roll (device rotated 180° around forward axis)
      roll = -roll;
      break;
    case 3:  // Flip yaw (device facing backwards)
      yaw = yaw + 180.0f;
      if (yaw > 360.0f) yaw -= 360.0f;
      break;
    case 4:  // Flip pitch and roll (device upside down and rotated)
      pitch = -pitch;
      roll = -roll;
      break;
    case 5:  // Common case orientation issue: roll values flipped around ±180°
      if (roll > 90.0f) {
        roll = 180.0f - roll;
      } else if (roll < -90.0f) {
        roll = -180.0f - roll;
      }
      break;
    case 6:  // IMU rotated 90° counter-clockwise (axes swapped)
      {
        float origPitch = pitch;
        float origRoll = roll;
        float origYaw = yaw;
        pitch = origRoll;
        roll = origPitch;
        yaw = origYaw;
      }
      break;
    case 7:  // Alternative mapping for extreme pitch values
      {
        float origPitch = pitch;
        float origRoll = roll;
        float origYaw = yaw;
        pitch = origYaw - 270.0f;
        if (pitch < -180.0f) pitch += 360.0f;
        if (pitch > 180.0f) pitch -= 360.0f;
        yaw = origPitch + 180.0f;
        if (yaw < 0.0f) yaw += 360.0f;
        if (yaw >= 360.0f) yaw -= 360.0f;
        roll = origRoll;
      }
      break;
    case 8:  // IMU upside down - roll around ±180°, pitch small
      {
        float origPitch = pitch;
        float origRoll = roll;
        float origYaw = yaw;
        float normalizedRoll = origRoll;
        if (normalizedRoll > 90.0f) {
          normalizedRoll = 180.0f - normalizedRoll;
        } else if (normalizedRoll < -90.0f) {
          normalizedRoll = -180.0f - normalizedRoll;
        }
        pitch = normalizedRoll;
        yaw = origYaw;
        roll = origPitch;
      }
      break;
    default:
      break;
  }

  // Normalize angles to proper ranges
  while (yaw < 0.0f) yaw += 360.0f;
  while (yaw >= 360.0f) yaw -= 360.0f;

  // Clamp pitch to prevent camera flipping
  if (pitch > 75.0f) pitch = 75.0f;
  if (pitch < -75.0f) pitch = -75.0f;

  // Clamp roll to reasonable ranges
  if (roll > 180.0f) roll -= 360.0f;
  if (roll < -180.0f) roll += 360.0f;
}

void readIMUSensor() {
  if (!imuEnabled || !imuConnected || gBNO055 == nullptr) {
    if (!imuConnected) {
      broadcastOutput("[IMU] Error: Not connected. Check wiring.");
    } else if (!imuEnabled) {
      broadcastOutput("[IMU] Error: Not started - use 'imustart' first");
    } else {
      broadcastOutput("[IMU] Error: Failed to initialize BNO055 sensor");
    }
    return;
  }

  // Clock is managed by i2cDeviceTransaction wrapper - no manual changes needed

  sensors_event_t accelEvent;
  sensors_event_t gyroEvent;
  sensors_event_t oriEvent;

  gBNO055->getEvent(&accelEvent, Adafruit_BNO055::VECTOR_ACCELEROMETER);
  gBNO055->getEvent(&gyroEvent, Adafruit_BNO055::VECTOR_GYROSCOPE);
  gBNO055->getEvent(&oriEvent, Adafruit_BNO055::VECTOR_EULER);

  int8_t t = gBNO055->getTemp();

  float rawYaw = oriEvent.orientation.x;
  float rawPitch = oriEvent.orientation.y;
  float rawRoll = oriEvent.orientation.z;

  applyIMUOrientationCorrection(rawPitch, rawRoll, rawYaw);

  if (gImuCache.mutex && xSemaphoreTake(gImuCache.mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    gImuCache.accelX = accelEvent.acceleration.x;
    gImuCache.accelY = accelEvent.acceleration.y;
    gImuCache.accelZ = accelEvent.acceleration.z;
    gImuCache.gyroX = gyroEvent.gyro.x;
    gImuCache.gyroY = gyroEvent.gyro.y;
    gImuCache.gyroZ = gyroEvent.gyro.z;
    gImuCache.oriYaw = rawYaw;
    gImuCache.oriPitch = rawPitch;
    gImuCache.oriRoll = rawRoll;
    gImuCache.imuTemp = (float)t;
    gImuCache.imuLastUpdate = millis();
    gImuCache.imuDataValid = true;
    gImuCache.imuSeq++;
    xSemaphoreGive(gImuCache.mutex);

    updateIMUActions();
    DEBUG_IMU_DATAF("IMU data updated");
  } else {
    DEBUG_IMU_FRAMEF("readIMUSensor() failed to lock cache - skipping update");
  }
}
// JSON Building
// ============================================================================

// Build IMU JSON directly into buffer using snprintf (zero String allocations)
int buildIMUDataJSON(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;

  int pos = 0;

  if (gImuCache.mutex && xSemaphoreTake(gImuCache.mutex, pdMS_TO_TICKS(100)) == pdTRUE) {  // 100ms timeout for HTTP response
    unsigned long nowMs = millis();
    unsigned long lastUpdateMs = gImuCache.imuLastUpdate;
    unsigned long ageMs = (lastUpdateMs > 0 && nowMs >= lastUpdateMs) ? (nowMs - lastUpdateMs) : 0;

    bool enabled = imuEnabled;
    bool connected = imuConnected;
    bool initReq = imuInitRequested;
    bool initDone = imuInitDone;
    bool initOk = imuInitResult;

    // Build complete JSON response in a single snprintf call
    pos = snprintf(buf, bufSize,
                   "{\"valid\":%s,\"seq\":%lu,"
                   "\"enabled\":%s,\"connected\":%s,"
                   "\"initRequested\":%s,\"initDone\":%s,\"initResult\":%s,"
                   "\"ageMs\":%lu,"
                   "\"accel\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},"
                   "\"gyro\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},"
                   "\"ori\":{\"yaw\":%.2f,\"pitch\":%.2f,\"roll\":%.2f},"
                   "\"temp\":%.1f,\"timestamp\":%lu}",
                   gImuCache.imuDataValid ? "true" : "false",
                   (unsigned long)gImuCache.imuSeq,
                   enabled ? "true" : "false",
                   connected ? "true" : "false",
                   initReq ? "true" : "false",
                   initDone ? "true" : "false",
                   initOk ? "true" : "false",
                   ageMs,
                   gImuCache.accelX, gImuCache.accelY, gImuCache.accelZ,
                   gImuCache.gyroX, gImuCache.gyroY, gImuCache.gyroZ,
                   gImuCache.oriYaw, gImuCache.oriPitch, gImuCache.oriRoll,
                   gImuCache.imuTemp,
                   gImuCache.imuLastUpdate);

    if (pos < 0 || (size_t)pos >= bufSize) {
      pos = snprintf(buf, bufSize, "{\"error\":\"IMU JSON overflow\"}");
    }

    xSemaphoreGive(gImuCache.mutex);
  } else {
    // Timeout - return error response
    pos = snprintf(buf, bufSize, "{\"error\":\"IMU cache timeout\"}");
  }

  return pos;
}
// ============================================================================
// IMU Action Detection Functions
// ============================================================================

// Update all IMU action detections
void updateIMUActions() {
  if (!imuEnabled || !imuConnected || !gImuCache.imuDataValid) return;

  unsigned long now = millis();

  if (!gImuCache.mutex || xSemaphoreTake(gImuCache.mutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

  float ax = gImuCache.accelX;
  float ay = gImuCache.accelY;
  float az = gImuCache.accelZ;
  float gx = gImuCache.gyroX;
  float gy = gImuCache.gyroY;
  float gz = gImuCache.gyroZ;
  float roll = gImuCache.oriRoll;
  float pitch = gImuCache.oriPitch;

  xSemaphoreGive(gImuCache.mutex);

  // Calculate acceleration magnitude
  float accelMag = sqrt(ax * ax + ay * ay + az * az);

  // Store in history buffer
  gIMUActions.accelHistory[gIMUActions.accelHistoryIndex] = accelMag;
  gIMUActions.accelHistoryIndex = (gIMUActions.accelHistoryIndex + 1) % 10;

  // 1. SHAKE DETECTION - High frequency acceleration changes
  float accelVariance = 0.0f;
  float accelMean = 0.0f;
  for (int i = 0; i < 10; i++) {
    accelMean += gIMUActions.accelHistory[i];
  }
  accelMean /= 10.0f;
  for (int i = 0; i < 10; i++) {
    float diff = gIMUActions.accelHistory[i] - accelMean;
    accelVariance += diff * diff;
  }
  accelVariance /= 10.0f;

  const float shakeThreshold = 15.0f;  // m/s² variance
  if (accelVariance > shakeThreshold) {
    if (!gIMUActions.isShaking) {
      gIMUActions.shakeCount++;
    }
    gIMUActions.isShaking = true;
    gIMUActions.lastShakeMs = now;
    gIMUActions.shakeIntensity = min(accelVariance / 50.0f, 1.0f);
  } else if (now - gIMUActions.lastShakeMs > 500) {
    gIMUActions.isShaking = false;
    gIMUActions.shakeIntensity = 0.0f;
  }

  // 2. TILT DETECTION - Device tilted past threshold
  const float tiltThreshold = 30.0f;  // degrees
  float maxTilt = max(abs(roll), abs(pitch));

  if (maxTilt > tiltThreshold) {
    gIMUActions.isTilted = true;
    gIMUActions.tiltAngle = maxTilt;

    // Determine direction
    if (abs(pitch) > abs(roll)) {
      gIMUActions.tiltDirection = (pitch > 0) ? 'F' : 'B';  // Forward/Back
    } else {
      gIMUActions.tiltDirection = (roll > 0) ? 'R' : 'L';  // Right/Left
    }
  } else {
    gIMUActions.isTilted = false;
    gIMUActions.tiltAngle = maxTilt;
    gIMUActions.tiltDirection = 'N';
  }

  // 3. TAP/KNOCK DETECTION - Sharp acceleration spike
  const float tapThreshold = 25.0f;  // m/s²
  const float tapDecay = 500;        // ms

  if (accelMag > tapThreshold && (now - gIMUActions.lastTapMs) > 200) {
    gIMUActions.tapDetected = true;
    gIMUActions.lastTapMs = now;
    gIMUActions.tapCount++;
    gIMUActions.tapStrength = min((accelMag - tapThreshold) / 20.0f, 1.0f);
  } else if (now - gIMUActions.lastTapMs > tapDecay) {
    gIMUActions.tapDetected = false;
    gIMUActions.tapStrength = 0.0f;
  }

  // 4. ROTATION DETECTION - High angular velocity
  const float rotationThreshold = 100.0f;  // deg/s
  float maxGyro = max(abs(gx), max(abs(gy), abs(gz)));

  if (maxGyro > rotationThreshold) {
    gIMUActions.isRotating = true;
    gIMUActions.rotationRate = maxGyro;

    // Determine axis
    if (abs(gx) > abs(gy) && abs(gx) > abs(gz)) {
      gIMUActions.rotationAxis = 'X';
    } else if (abs(gy) > abs(gz)) {
      gIMUActions.rotationAxis = 'Y';
    } else {
      gIMUActions.rotationAxis = 'Z';
    }
  } else {
    gIMUActions.isRotating = false;
    gIMUActions.rotationRate = maxGyro;
    gIMUActions.rotationAxis = 'N';
  }

  // 5. FREEFALL DETECTION - Near-zero acceleration
  const float freefallThreshold = 2.0f;  // m/s² (significantly less than 9.8)

  if (accelMag < freefallThreshold) {
    if (!gIMUActions.isFreefalling) {
      gIMUActions.freefallStartMs = now;
    }
    gIMUActions.isFreefalling = true;
    gIMUActions.freefallDurationMs = now - gIMUActions.freefallStartMs;
  } else {
    gIMUActions.isFreefalling = false;
    gIMUActions.freefallDurationMs = 0;
  }

  // 6. STEP COUNTING - Periodic vertical acceleration peaks
  const float stepPeakThreshold = 12.0f;  // m/s²
  const float stepValleyThreshold = 8.0f;
  const unsigned long stepMinInterval = 200;   // ms between steps
  const unsigned long stepMaxInterval = 2000;  // ms - if longer, not walking

  // Detect peak
  if (accelMag > stepPeakThreshold && gIMUActions.lastAccelMag < stepPeakThreshold) {
    if (!gIMUActions.stepPeakDetected && (now - gIMUActions.lastStepMs) > stepMinInterval) {
      gIMUActions.stepPeakDetected = true;
    }
  }

  // Detect valley (step complete)
  if (accelMag < stepValleyThreshold && gIMUActions.stepPeakDetected) {
    gIMUActions.stepCount++;
    gIMUActions.lastStepMs = now;
    gIMUActions.stepPeakDetected = false;
    gIMUActions.stepsInWindow++;
  }

  // Update walking state and frequency
  if (now - gIMUActions.lastStepMs < stepMaxInterval) {
    gIMUActions.isWalking = true;

    // Calculate step frequency over last minute
    if (now - gIMUActions.stepWindowStartMs > 60000) {
      gIMUActions.stepFrequency = gIMUActions.stepsInWindow;
      gIMUActions.stepWindowStartMs = now;
      gIMUActions.stepsInWindow = 0;
    }
  } else {
    gIMUActions.isWalking = false;
    if (now - gIMUActions.stepWindowStartMs > 60000) {
      gIMUActions.stepFrequency = 0.0f;
      gIMUActions.stepWindowStartMs = now;
      gIMUActions.stepsInWindow = 0;
    }
  }

  gIMUActions.lastAccelMag = accelMag;

  // 7. ORIENTATION DETECTION - Device orientation in space
  char newOrientation = 'F';  // Default: face-up

  // Determine primary orientation based on which axis is most vertical
  if (abs(az) > 7.0f) {
    // Z-axis is dominant (face up/down)
    newOrientation = (az > 0) ? 'F' : 'D';  // Face-up / Face-down
  } else if (abs(pitch) > 45.0f) {
    // Pitched significantly
    newOrientation = (pitch > 0) ? 'P' : 'U';  // Portrait / Upside-down portrait
  } else if (abs(roll) > 45.0f) {
    // Rolled significantly
    newOrientation = (roll > 0) ? 'R' : 'L';  // Reverse landscape / Landscape
  } else {
    // Relatively flat
    newOrientation = 'F';  // Face-up
  }

  if (newOrientation != gIMUActions.orientation) {
    gIMUActions.lastOrientation = gIMUActions.orientation;
    gIMUActions.orientation = newOrientation;
    gIMUActions.lastOrientationChangeMs = now;
  }
}

// ============================================================================
// IMU UI Settings Commands
// ============================================================================

const char* cmd_imupollingms(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: imupollingms <50..2000>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 50 || v > 2000) return "Error: imuPollingMs must be 50..2000";
  setSetting(gSettings.imuPollingMs, v);
  BROADCAST_PRINTF("imuPollingMs set to %d", v);
  return "OK";
}

const char* cmd_imuewmafactor(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: imuewmafactor <0.0..1.0>";
  while (*p == ' ') p++;  // Skip whitespace
  float f = strtof(p, nullptr);
  if (f < 0.0f || f > 1.0f) return "Error: imuEWMAFactor must be 0..1";
  setSetting(gSettings.imuEWMAFactor, f);
  BROADCAST_PRINTF("imuEWMAFactor set to %.3f", f);
  return "OK";
}

const char* cmd_imutransitionms(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: imutransitionms <0..1000>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 0 || v > 1000) return "Error: imuTransitionMs must be 0..1000";
  setSetting(gSettings.imuTransitionMs, v);
  BROADCAST_PRINTF("imuTransitionMs set to %d", v);
  return "OK";
}

const char* cmd_imuwebmaxfps(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: imuwebmaxfps <1..30>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 1 || v > 30) return "Error: imuWebMaxFps must be 1..30";
  setSetting(gSettings.imuWebMaxFps, v);
  BROADCAST_PRINTF("imuWebMaxRefreshRate set to %d", v);
  return "OK";
}

// ============================================================================
// IMU Device Settings Commands
// ============================================================================

const char* cmd_imudevicepollms(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) return "Usage: imuDevicePollMs <50..1000>";
  int v = valStr.toInt();
  if (v < 50) v = 50;
  if (v > 1000) v = 1000;
  setSetting(gSettings.imuDevicePollMs, v);
  snprintf(getDebugBuffer(), 1024, "imuDevicePollMs set to %d", v);
  return getDebugBuffer();
}

const char* cmd_imuorientationmode(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) {
    snprintf(getDebugBuffer(), 1024, "Current imuOrientationMode: %d (0=normal, 1=flip_pitch, 2=flip_roll, 3=flip_yaw, 4=flip_pitch_roll, 5=roll_180_fix, 6=rotate_90ccw, 7=alt_extreme_pitch, 8=upside_down)", gSettings.imuOrientationMode);
    return getDebugBuffer();
  }
  int v = valStr.toInt();
  if (v < 0 || v > 8) return "Error: mode must be 0-8";
  setSetting(gSettings.imuOrientationMode, v);
  snprintf(getDebugBuffer(), 1024, "imuOrientationMode set to %d", v);
  return getDebugBuffer();
}

const char* cmd_imuorientationcorrection(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) {
    return gSettings.imuOrientationCorrectionEnabled ? "Current imuOrientationCorrectionEnabled: 1" : "Current imuOrientationCorrectionEnabled: 0";
  }
  int v = valStr.toInt();
  setSetting(gSettings.imuOrientationCorrectionEnabled, (bool)(v != 0));
  return gSettings.imuOrientationCorrectionEnabled ? "imuOrientationCorrectionEnabled set to 1" : "imuOrientationCorrectionEnabled set to 0";
}

const char* cmd_imupitchoffset(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) {
    snprintf(getDebugBuffer(), 1024, "Current imuPitchOffset: %.2f", gSettings.imuPitchOffset);
    return getDebugBuffer();
  }
  float v = valStr.toFloat();
  setSetting(gSettings.imuPitchOffset, v);
  snprintf(getDebugBuffer(), 1024, "imuPitchOffset set to %.2f", v);
  return getDebugBuffer();
}

const char* cmd_imurolloffset(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) {
    snprintf(getDebugBuffer(), 1024, "Current imuRollOffset: %.2f", gSettings.imuRollOffset);
    return getDebugBuffer();
  }
  float v = valStr.toFloat();
  setSetting(gSettings.imuRollOffset, v);
  snprintf(getDebugBuffer(), 1024, "imuRollOffset set to %.2f", v);
  return getDebugBuffer();
}

const char* cmd_imuyawoffset(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) {
    snprintf(getDebugBuffer(), 1024, "Current imuYawOffset: %.2f", gSettings.imuYawOffset);
    return getDebugBuffer();
  }
  float v = valStr.toFloat();
  setSetting(gSettings.imuYawOffset, v);
  getDebugBuffer()[0] = '\0';
  snprintf(getDebugBuffer(), 1024, "imuYawOffset set to %.2f", v);
  return getDebugBuffer();
}

const char* cmd_imuautostart(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = args; arg.trim();
  if (arg.length() == 0) {
    return gSettings.imuAutoStart ? "[IMU] Auto-start: enabled" : "[IMU] Auto-start: disabled";
  }
  arg.toLowerCase();
  if (arg == "on" || arg == "true" || arg == "1") {
    setSetting(gSettings.imuAutoStart, true);
    return "[IMU] Auto-start enabled";
  } else if (arg == "off" || arg == "false" || arg == "0") {
    setSetting(gSettings.imuAutoStart, false);
    return "[IMU] Auto-start disabled";
  }
  return "Usage: imuautostart [on|off]";
}

// IMU Command Registry (Sensor-Specific)
// ============================================================================
const CommandEntry imuCommands[] = {
  // Start/Stop (3-level voice: "sensor" -> "motion sensor" -> "open/close")
  { "openimu", "Start BNO055 IMU sensor.", false, cmd_imustart, nullptr, "sensor", "motion sensor", "open" },
  { "closeimu", "Stop BNO055 IMU sensor.", false, cmd_imustop, nullptr, "sensor", "motion sensor", "close" },
  
  // Information
  { "imuread", "Read IMU sensor data.", false, cmd_imu },
  { "imuactions", "Show IMU action detection state.", false, cmd_imuactions },
  
  // UI Settings (client-side visualization)
  { "imupollingms", "IMU UI polling interval: <50..2000>", true, cmd_imupollingms, "Usage: imupollingms <50..2000>" },
  { "imuewmafactor", "IMU EWMA smoothing: <0.0..1.0>", true, cmd_imuewmafactor, "Usage: imuewmafactor <0.0..1.0>" },
  { "imutransitionms", "IMU transition time: <0..1000>", true, cmd_imutransitionms, "Usage: imutransitionms <0..1000>" },
  { "imuwebmaxfps", "IMU web max FPS: <1..30>", true, cmd_imuwebmaxfps, "Usage: imuwebmaxfps <1..30>" },
  
  // Device-level settings (sensor hardware behavior)
  { "imudevicepollms", "IMU device poll interval: <50..1000>", true, cmd_imudevicepollms, "Usage: imuDevicePollMs <50..1000>" },
  { "imuorientationmode", "IMU orientation mode: <0..8>", true, cmd_imuorientationmode },
  { "imuorientationcorrection", "IMU orientation correction: <0|1>", true, cmd_imuorientationcorrection },
  { "imupitchoffset", "IMU pitch offset: <-180..180>", true, cmd_imupitchoffset },
  { "imurolloffset", "IMU roll offset: <-180..180>", true, cmd_imurolloffset },
  { "imuyawoffset", "IMU yaw offset: <-180..180>", true, cmd_imuyawoffset },
  
  // Auto-start
  { "imuautostart", "Enable/disable IMU auto-start after boot [on|off]", false, cmd_imuautostart, "Usage: imuautostart [on|off]" },
};

const size_t imuCommandsCount = sizeof(imuCommands) / sizeof(imuCommands[0]);

// ============================================================================
// Command Registration (Sensor-Specific)
// ============================================================================
// Direct static registration to avoid macro issues
static CommandModuleRegistrar _imu_cmd_registrar(imuCommands, imuCommandsCount, "imu");

// ============================================================================
// IMU Task Implementation (moved from i2c_system.cpp for full modularization)
// ============================================================================

// ============================================================================
// IMU Task - FreeRTOS Task Function
// ============================================================================
// Purpose: Continuously reads 9-DOF orientation data from BNO055 IMU sensor
// Stack: 4096 words (~16KB) | Priority: 1 | Core: Any
// Lifecycle: Created by cmd_imustart, deleted when imuEnabled=false
// Polling: Configurable via imuDevicePollMs (default 200ms) | I2C Clock: 100kHz
//
// Cleanup Strategy:
//   1. Check imuEnabled flag at loop start
//   2. Acquire i2cMutex to prevent race conditions during cleanup
//   3. Delete sensor object and invalidate cache
//   4. Release mutex and delete task
// ============================================================================

void imuTask(void* parameter) {
  INFO_SENSORSF("[IMU] Task started (handle=%p, stack=%u words)", 
                (void*)xTaskGetCurrentTaskHandle(), 
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  INFO_SENSORSF("[MODULAR] imuTask() running from Sensor_IMU_BNO055.cpp");
  unsigned long lastIMURead = 0;
  unsigned long lastStackLog = 0;
  while (true) {
    // CRITICAL: Check enabled flag FIRST for graceful shutdown
    if (!imuEnabled) {
      imuConnected = false;
      if (gBNO055 != nullptr) {
        delete gBNO055;
        gBNO055 = nullptr;
      }
      gImuCache.imuDataValid = false;
      gImuCache.imuSeq = 0;
      
      // Reset initialization flags for clean restart
      imuInitRequested = false;
      imuInitDone = false;
      imuInitResult = false;
      
      INFO_SENSORSF("[IMU] Task disabled - cleaning up and deleting");
      // NOTE: Do NOT clear imuTaskHandle here - let create function use eTaskGetState()
      // to detect stale handles. Clearing here creates a race condition window.
      vTaskDelete(nullptr);
    }
    
    // Update watermark diagnostics (only when enabled)
    if (isDebugFlagSet(DEBUG_PERFORMANCE)) {
      UBaseType_t wm = uxTaskGetStackHighWaterMark(NULL);
      gIMUWatermarkNow = wm;
      if (wm < gIMUWatermarkMin) gIMUWatermarkMin = wm;
    }
    unsigned long nowLog = millis();
    if (nowLog - lastStackLog >= 5000UL) {
      lastStackLog = nowLog;
      if (checkTaskStackSafety("imu", IMU_STACK_WORDS, &imuEnabled)) break;
      // CRITICAL: Check enabled flag again before debug output (prevent crash during shutdown)
      if (imuEnabled) {
        DEBUG_PERFORMANCEF("[STACK] imu_task watermark_now=%u min=%u words", (unsigned)gIMUWatermarkNow, (unsigned)gIMUWatermarkMin);
        DEBUG_MEMORYF("[HEAP] imu_task: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }
    // Handle deferred IMU initialization on task stack
    if (imuEnabled && (!imuConnected || gBNO055 == nullptr)) {
      if (imuInitRequested) {
        bool ok = initIMUSensor();
        imuInitResult = ok;
        imuInitDone = true;
        imuInitRequested = false;
        if (!ok) {
          imuEnabled = false;
        }
      }
    }

    if (imuEnabled && imuConnected && gBNO055 != nullptr && !gSensorPollingPaused) {
      unsigned long imuPollMs = (gSettings.imuDevicePollMs > 0) ? (unsigned long)gSettings.imuDevicePollMs : 200;
      unsigned long nowMs = millis();
      if (nowMs - lastIMURead >= imuPollMs) {
        // IMU reads ~5ms at 100kHz; fail fast and retry next poll rather than blocking 1000ms
        auto result = i2cTaskWithTimeout(I2C_ADDR_IMU, 100000, 100, [&]() -> bool {
          readIMUSensor();
          return true;  // Assume success for void operation
        });
        lastIMURead = nowMs;
        
        // Mark OLED dirty if IMU page is active (enables real-time display updates)
        if (result && currentOLEDMode == OLED_IMU_ACTIONS) {
          oledMarkDirty();
        }
        
        // Auto-disable if too many consecutive failures
        if (!result) {
          if (i2cShouldAutoDisable(I2C_ADDR_IMU, 5)) {
            ERROR_SENSORSF("Too many consecutive IMU failures - auto-disabling");
            imuEnabled = false;
            sensorStatusBumpWith("imu@auto_disabled");
          }
        }
        
        // Stream data to ESP-NOW master if enabled (worker devices only)
#if ENABLE_ESPNOW
        // Check mesh mode (worker role) OR bond mode (worker role)
        bool shouldStream = false;
        if (meshEnabled() && gSettings.meshRole != MESH_ROLE_MASTER) {
          shouldStream = true;
        }
#if ENABLE_BONDED_MODE
        if (gSettings.bondModeEnabled && gSettings.bondRole == 0) {
          shouldStream = true;  // Bond mode worker
        }
#endif
        
        if (result && shouldStream) {
          // Build IMU JSON from cache
          char imuJson[512];
          int jsonLen = buildIMUDataJSON(imuJson, sizeof(imuJson));
          if (jsonLen > 0) {
            sendSensorDataUpdate(REMOTE_SENSOR_IMU, String(imuJson));
          }
        }
#endif
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    } else {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

// ============================================================================
// IMU Settings Module (for modular settings registry)
// ============================================================================

static const SettingEntry imuSettingEntries[] = {
  { "imuAutoStart",                    SETTING_BOOL,  &gSettings.imuAutoStart,                    0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr },
  { "imuPollingMs",                    SETTING_INT,   &gSettings.imuPollingMs,                    200, 0, nullptr, 50, 2000, "Polling (ms)", nullptr },
  { "imuEWMAFactor",                   SETTING_FLOAT, &gSettings.imuEWMAFactor,                   0, 0.1f, nullptr, 0, 1, "EWMA Factor", nullptr },
  { "imuTransitionMs",                 SETTING_INT,   &gSettings.imuTransitionMs,                 100, 0, nullptr, 0, 1000, "Transition (ms)", nullptr },
  { "imuWebMaxFps",                    SETTING_INT,   &gSettings.imuWebMaxFps,                    15, 0, nullptr, 1, 30, "Web Max FPS", nullptr },
  { "imuDevicePollMs",                 SETTING_INT,   &gSettings.imuDevicePollMs,                 200, 0, nullptr, 50, 1000, "Poll Interval (ms)", nullptr },
  { "imuOrientationMode",              SETTING_INT,   &gSettings.imuOrientationMode,              8, 0, nullptr, 0, 8, "Orientation Mode", nullptr },
  { "imuOrientationCorrectionEnabled", SETTING_BOOL,  &gSettings.imuOrientationCorrectionEnabled, true, 0, nullptr, 0, 1, "Orientation Correction", nullptr },
  { "imuPitchOffset",                  SETTING_FLOAT, &gSettings.imuPitchOffset,                  0, 0.0f, nullptr, -180, 180, "Pitch Offset", nullptr },
  { "imuRollOffset",                   SETTING_FLOAT, &gSettings.imuRollOffset,                   0, 0.0f, nullptr, -180, 180, "Roll Offset", nullptr },
  { "imuYawOffset",                    SETTING_FLOAT, &gSettings.imuYawOffset,                    0, 0.0f, nullptr, -180, 180, "Yaw Offset", nullptr }
};

static bool isIMUConnected() {
  return imuConnected;
}

extern const SettingsModule imuSettingsModule = {
  "imu",
  "imu_bno055",
  imuSettingEntries,
  sizeof(imuSettingEntries) / sizeof(imuSettingEntries[0]),
  isIMUConnected,
  "BNO055 IMU sensor settings"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================
// IMU OLED Mode (Display Function + Registration)
// ============================================================================
#if DISPLAY_TYPE > 0
#include "i2csensor-bno055-oled.h"
#endif

#endif // ENABLE_IMU_SENSOR
