#include "i2csensor_bno055.h"
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
volatile bool gImuInitRequested = false;
volatile bool gImuInitDone = false;
volatile bool gImuInitResult = false;

// Settings and debug
// sensorStatusBumpWith, gSensorPollingPaused, drainDebugRing provided by System_I2C.h

// ============================================================================
// IMU Sensor Cache (owned by this module)
// ============================================================================
ImuCache gImuCache;

// IMU Action Detection System - definitions
IMUActionState gImuActions = {
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
volatile UBaseType_t gImuWatermarkMin = (UBaseType_t)0xFFFFFFFF;
volatile UBaseType_t gImuWatermarkNow = (UBaseType_t)0;

// IMU initialization handoff flags - defined above (lines 17-19)

// Debug macros (use centralized versions from debug_system.h)
// BROADCAST_PRINTF now defined in debug_system.h with performance optimizations
// MIN_RESTART_DELAY_MS defined in System_I2C.h

// Queue system functions now in System_I2C.h

// IMU sensor state (definitions)
bool gImuEnabled = false;
bool gImuConnected = false;
unsigned long gImuLastStopTime = 0;
TaskHandle_t gImuTaskHandle = nullptr;

// Forward declarations
extern bool createIMUTask();

// ============================================================================
// IMU Sensor Command Handlers
// ============================================================================

const char* cmd_imu(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    if (!ensureDebugBuffer()) return "{\"valid\":false,\"error\":\"buffer\"}";
    int n = imuBuildDataJSON(getDebugBuffer(), 1024);  // shared builder (also feeds sensors json / MQTT)
    return (n > 0) ? getDebugBuffer() : "{\"valid\":false}";
  }

  if (!gImuConnected || !gImuEnabled) {
    broadcastOutput("IMU sensor not connected or not started. Use 'imustart' first.");
    return "ERROR";
  }

  // Read from sensor cache instead of accessing hardware directly
  {
    SensorCacheGuard g(gImuCache.mutex, pdMS_TO_TICKS(100), "imu.cmdRead");
    if (g.held) {
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
    } else {
      broadcastOutput("Failed to access sensor cache");
      return "ERROR";
    }
  }

  return "OK";
}

// Internal function called by queue processor
bool imuStartInternal() {
  // Check if too soon after stop (prevent rapid restart crashes)
  if (gImuLastStopTime > 0) {
    unsigned long timeSinceStop = millis() - gImuLastStopTime;
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

  // Create cache mutex if not already created
  if (!gImuCache.mutex) {
    gImuCache.mutex = xSemaphoreCreateMutex();
    if (!gImuCache.mutex) {
      ERROR_IMUF("Failed to create cache mutex");
      return false;
    }
    DEBUG_IMU_LIFECYCLEF("[IMU] Cache mutex created");
  }

  // Clean up any stale cache from previous run BEFORE starting
  // CRITICAL: Cache wasn't invalidated during stop to avoid dying-task crashes
  {
    SensorCacheGuard g(gImuCache.mutex, pdMS_TO_TICKS(100), "imu.cleanStaleCache");
    if (g.held) {
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
      DEBUG_CLIF("[IMU_INTERNAL] Cleaned up stale cache from previous run");
    }
  }

  // CRITICAL: Enable flag BEFORE creating task to prevent race condition
  // Task checks gImuEnabled first thing and will delete itself if false
  bool prev = gImuEnabled;
  gImuEnabled = true;  // Set this BEFORE task creation

  // Defer initialization to imuTask; wait briefly for result
  if (gBNO055 == nullptr || !gImuConnected) {
    gImuInitDone = false;
    gImuInitResult = false;
    gImuInitRequested = true;
  }

  // Create IMU task lazily (after setting gImuEnabled=true)
  if (!createIMUTask()) {
    DEBUG_CLIF("Failed to create IMU task (insufficient memory or resources)");
    gImuEnabled = false;  // Reset flag on failure
    return false;
  }
  if (gImuEnabled != prev) {
    sensorStatusBumpWith("openimu@queue");
  }

  // If init was requested, block up to 3s for a result so CLI returns accurate status
  if (gImuInitRequested || gBNO055 == nullptr || !gImuConnected) {
    unsigned long start = millis();
    while (!gImuInitDone && (millis() - start) < 3000UL) {
      delay(10);
    }
    if (!gImuInitDone) {
      gImuEnabled = false;
      DEBUG_CLIF("Failed to initialize IMU sensor (timeout after 3s)");
      return false;
    }
    if (!gImuInitResult) {
      gImuEnabled = false;
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
const char* cmd_imustart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Check if already enabled or queued
  if (gImuEnabled) {
    return "[IMU] Error: Already running";
  }
  if (isInQueue(I2C_DEVICE_IMU)) {
    int pos = getQueuePosition(I2C_DEVICE_IMU);
    BROADCAST_PRINTF("IMU sensor already queued (position %d)", pos);
    return "[IMU] Already queued";
  }

  if (!i2cPingAddress(I2C_ADDR_IMU, 100000, 50)) {
    return "[IMU] Not detected on I2C bus";
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

const char* cmd_imustop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  handleDeviceStopped(I2C_DEVICE_IMU);
  return "[IMU] Close requested; cleanup will complete asynchronously";
}

const char* cmd_imuactions(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!gImuEnabled || !gImuConnected) {
    broadcastOutput("[IMU] Error: Not enabled. Use 'imustart' first.");
    return "ERROR";
  }

  // Update actions once to get current state
  imuUpdateActions();

  // Stream output line-by-line instead of building in shared buffer
  broadcastOutput("IMU Action Detection Status:");

  // Shake
  if (gImuActions.isShaking) {
    BROADCAST_PRINTF("  Shake: YES (intensity: %.1f, count: %lu)",
                     gImuActions.shakeIntensity, (unsigned long)gImuActions.shakeCount);
  } else {
    broadcastOutput("  Shake: no");
  }

  // Tilt
  if (gImuActions.isTilted) {
    const char* dir = "?";
    switch (gImuActions.tiltDirection) {
      case 'F': dir = "Forward"; break;
      case 'B': dir = "Back"; break;
      case 'L': dir = "Left"; break;
      case 'R': dir = "Right"; break;
    }
    BROADCAST_PRINTF("  Tilt: YES (%s, %.1f deg)", dir, gImuActions.tiltAngle);
  } else {
    broadcastOutput("  Tilt: no");
  }

  // Tap
  if (gImuActions.tapDetected || gImuActions.tapCount > 0) {
    BROADCAST_PRINTF("  Tap: %s (count: %lu, strength: %.1f)",
                     gImuActions.tapDetected ? "YES" : "no",
                     (unsigned long)gImuActions.tapCount, gImuActions.tapStrength);
  } else {
    broadcastOutput("  Tap: no");
  }

  // Rotation
  if (gImuActions.isRotating) {
    BROADCAST_PRINTF("  Rotation: YES (%c-axis, %.1f deg/s)",
                     gImuActions.rotationAxis, gImuActions.rotationRate);
  } else {
    broadcastOutput("  Rotation: no");
  }

  // Freefall
  if (gImuActions.isFreefalling) {
    BROADCAST_PRINTF("  Freefall: YES (%lu ms)", (unsigned long)gImuActions.freefallDurationMs);
  } else {
    broadcastOutput("  Freefall: no");
  }

  // Steps
  if (gImuActions.isWalking) {
    BROADCAST_PRINTF("  Steps: %lu (WALKING, %.1f steps/min)",
                     (unsigned long)gImuActions.stepCount, gImuActions.stepFrequency);
  } else {
    BROADCAST_PRINTF("  Steps: %lu", (unsigned long)gImuActions.stepCount);
  }

  // Orientation
  const char* orient = "?";
  switch (gImuActions.orientation) {
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

bool imuInit() {
  if (gBNO055 != nullptr) {
    broadcastOutput("[IMU] Error: Already initialized!");
    return true;
  }

  INFO_IMU_LIFECYCLEF("Starting BNO055 IMU initialization (STEMMA QT)...");

  // Reset grace period for this initialization attempt (device may have been registered at boot)
  i2cResetGracePeriod(I2C_ADDR_IMU);

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
    INFO_IMU_LIFECYCLEF("Starting IMU initialization at 100kHz I2C clock");

    // BNO055 needs time after power-up/reset before responding reliably
    delay(1000);

    if (foundIndex < 0) {
      WARN_IMUF("Error: Not detected at 0x%02X or 0x%02X (initial probe). Will attempt init anyway with retries", I2C_ADDR_IMU, BNO055_ADDRESS_B);
    } else {
      INFO_IMU_LIFECYCLEF("Detected BNO055 at address 0x%02X", candidateAddrs[foundIndex]);
    }

    // Retry loop with conservative I2C clocks (BNO055 doesn't like high speeds)
    const int maxAttempts = 5;
    uint32_t clocks[maxAttempts] = { 100000, 100000, 50000, 100000, 100000 };
    
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
      DEBUG_IMU_LIFECYCLEF("[IMU] Error: Init attempt %d/%d at I2C %lu Hz", attempt, maxAttempts, clocks[attempt - 1]);

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
        INFO_IMU_LIFECYCLEF("Trying BNO055 address 0x%02X", addr);
        gBNO055 = new Adafruit_BNO055(55, addr, &Wire1);
        if (gBNO055 == nullptr) {
          ERROR_IMUF("Error: Failed to allocate memory for BNO055 object");
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
        gImuConnected = true;
        
        INFO_IMU_LIFECYCLEF("BNO055 IMU sensor initialized successfully");
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
    ERROR_IMUF("Error: Failed to initialize BNO055 IMU sensor after %d attempts", maxAttempts);
    broadcastOutput("[IMU] Error: Failed to initialize IMU sensor (timeout after 3s)");
    return false;
  });
}

// Apply IMU orientation correction based on physical mounting
void imuApplyOrientationCorrection(float& pitch, float& roll, float& yaw) {
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

void imuPoll() {
  if (!gImuEnabled || !gImuConnected || gBNO055 == nullptr) {
    if (!gImuConnected) {
      broadcastOutput("[IMU] Error: Not connected. Check wiring.");
    } else if (!gImuEnabled) {
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

  imuApplyOrientationCorrection(rawPitch, rawRoll, rawYaw);

  bool updated = false;
  {
    // Pattern 3: take/write/release, then run imuUpdateActions WITHOUT the lock
    // (intentional — action detection does its own short take in the function).
    SensorCacheGuard g(gImuCache.mutex, pdMS_TO_TICKS(50), "imu.pollWrite");
    if (g.held) {
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
      updated = true;
    }
  }
  if (updated) {
    imuUpdateActions();
    DEBUG_IMU_VALUESF("IMU data updated");
  } else {
    DEBUG_IMU_POLLINGF("imuPoll() failed to lock cache - skipping update");
  }
}
// JSON Building
// ============================================================================

// Build IMU JSON directly into buffer using snprintf (zero String allocations)
int imuBuildDataJSON(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;

  int pos = 0;

  SensorCacheGuard g(gImuCache.mutex, pdMS_TO_TICKS(CACHE_MUTEX_TIMEOUT_MS), "imu.buildJSON");
  if (g.held) {
    unsigned long nowMs = millis();
    unsigned long lastUpdateMs = gImuCache.imuLastUpdate;
    unsigned long ageMs = (lastUpdateMs > 0 && nowMs >= lastUpdateMs) ? (nowMs - lastUpdateMs) : 0;

    bool enabled = gImuEnabled;
    bool connected = gImuConnected;
    bool initReq = gImuInitRequested;
    bool initDone = gImuInitDone;
    bool initOk = gImuInitResult;

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
void imuUpdateActions() {
  if (!gImuEnabled || !gImuConnected || !gImuCache.imuDataValid) return;

  unsigned long now = millis();

  // Pattern 3: snapshot cache values into locals, release the lock, then do
  // the math (sqrt, history buffer) outside the lock. The explicit { } block
  // controls the guard's lifetime so the lock releases before the math runs.
  float ax, ay, az, gx, gy, gz, roll, pitch;
  {
    SensorCacheGuard g(gImuCache.mutex, pdMS_TO_TICKS(10), "imu.actionDetect");
    if (!g.held) return;
    ax = gImuCache.accelX;
    ay = gImuCache.accelY;
    az = gImuCache.accelZ;
    gx = gImuCache.gyroX;
    gy = gImuCache.gyroY;
    gz = gImuCache.gyroZ;
    roll = gImuCache.oriRoll;
    pitch = gImuCache.oriPitch;
  }

  // Calculate acceleration magnitude
  float accelMag = sqrt(ax * ax + ay * ay + az * az);

  // Store in history buffer
  gImuActions.accelHistory[gImuActions.accelHistoryIndex] = accelMag;
  gImuActions.accelHistoryIndex = (gImuActions.accelHistoryIndex + 1) % 10;

  // 1. SHAKE DETECTION - High frequency acceleration changes
  float accelVariance = 0.0f;
  float accelMean = 0.0f;
  for (int i = 0; i < 10; i++) {
    accelMean += gImuActions.accelHistory[i];
  }
  accelMean /= 10.0f;
  for (int i = 0; i < 10; i++) {
    float diff = gImuActions.accelHistory[i] - accelMean;
    accelVariance += diff * diff;
  }
  accelVariance /= 10.0f;

  const float shakeThreshold = 15.0f;  // m/s² variance
  if (accelVariance > shakeThreshold) {
    if (!gImuActions.isShaking) {
      gImuActions.shakeCount++;
    }
    gImuActions.isShaking = true;
    gImuActions.lastShakeMs = now;
    gImuActions.shakeIntensity = min(accelVariance / 50.0f, 1.0f);
  } else if (now - gImuActions.lastShakeMs > 500) {
    gImuActions.isShaking = false;
    gImuActions.shakeIntensity = 0.0f;
  }

  // 2. TILT DETECTION - Device tilted past threshold
  const float tiltThreshold = 30.0f;  // degrees
  float maxTilt = max(abs(roll), abs(pitch));

  if (maxTilt > tiltThreshold) {
    gImuActions.isTilted = true;
    gImuActions.tiltAngle = maxTilt;

    // Determine direction
    if (abs(pitch) > abs(roll)) {
      gImuActions.tiltDirection = (pitch > 0) ? 'F' : 'B';  // Forward/Back
    } else {
      gImuActions.tiltDirection = (roll > 0) ? 'R' : 'L';  // Right/Left
    }
  } else {
    gImuActions.isTilted = false;
    gImuActions.tiltAngle = maxTilt;
    gImuActions.tiltDirection = 'N';
  }

  // 3. TAP/KNOCK DETECTION - Sharp acceleration spike
  const float tapThreshold = 25.0f;  // m/s²
  const float tapDecay = 500;        // ms

  if (accelMag > tapThreshold && (now - gImuActions.lastTapMs) > 200) {
    gImuActions.tapDetected = true;
    gImuActions.lastTapMs = now;
    gImuActions.tapCount++;
    gImuActions.tapStrength = min((accelMag - tapThreshold) / 20.0f, 1.0f);
  } else if (now - gImuActions.lastTapMs > tapDecay) {
    gImuActions.tapDetected = false;
    gImuActions.tapStrength = 0.0f;
  }

  // 4. ROTATION DETECTION - High angular velocity
  const float rotationThreshold = 100.0f;  // deg/s
  float maxGyro = max(abs(gx), max(abs(gy), abs(gz)));

  if (maxGyro > rotationThreshold) {
    gImuActions.isRotating = true;
    gImuActions.rotationRate = maxGyro;

    // Determine axis
    if (abs(gx) > abs(gy) && abs(gx) > abs(gz)) {
      gImuActions.rotationAxis = 'X';
    } else if (abs(gy) > abs(gz)) {
      gImuActions.rotationAxis = 'Y';
    } else {
      gImuActions.rotationAxis = 'Z';
    }
  } else {
    gImuActions.isRotating = false;
    gImuActions.rotationRate = maxGyro;
    gImuActions.rotationAxis = 'N';
  }

  // 5. FREEFALL DETECTION - Near-zero acceleration
  const float freefallThreshold = 2.0f;  // m/s² (significantly less than 9.8)

  if (accelMag < freefallThreshold) {
    if (!gImuActions.isFreefalling) {
      gImuActions.freefallStartMs = now;
    }
    gImuActions.isFreefalling = true;
    gImuActions.freefallDurationMs = now - gImuActions.freefallStartMs;
  } else {
    gImuActions.isFreefalling = false;
    gImuActions.freefallDurationMs = 0;
  }

  // 6. STEP COUNTING - Periodic vertical acceleration peaks
  const float stepPeakThreshold = 12.0f;  // m/s²
  const float stepValleyThreshold = 8.0f;
  const unsigned long stepMinInterval = 200;   // ms between steps
  const unsigned long stepMaxInterval = 2000;  // ms - if longer, not walking

  // Detect peak
  if (accelMag > stepPeakThreshold && gImuActions.lastAccelMag < stepPeakThreshold) {
    if (!gImuActions.stepPeakDetected && (now - gImuActions.lastStepMs) > stepMinInterval) {
      gImuActions.stepPeakDetected = true;
    }
  }

  // Detect valley (step complete)
  if (accelMag < stepValleyThreshold && gImuActions.stepPeakDetected) {
    gImuActions.stepCount++;
    gImuActions.lastStepMs = now;
    gImuActions.stepPeakDetected = false;
    gImuActions.stepsInWindow++;
  }

  // Update walking state and frequency
  if (now - gImuActions.lastStepMs < stepMaxInterval) {
    gImuActions.isWalking = true;

    // Calculate step frequency over last minute
    if (now - gImuActions.stepWindowStartMs > 60000) {
      gImuActions.stepFrequency = gImuActions.stepsInWindow;
      gImuActions.stepWindowStartMs = now;
      gImuActions.stepsInWindow = 0;
    }
  } else {
    gImuActions.isWalking = false;
    if (now - gImuActions.stepWindowStartMs > 60000) {
      gImuActions.stepFrequency = 0.0f;
      gImuActions.stepWindowStartMs = now;
      gImuActions.stepsInWindow = 0;
    }
  }

  gImuActions.lastAccelMag = accelMag;

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

  if (newOrientation != gImuActions.orientation) {
    gImuActions.lastOrientation = gImuActions.orientation;
    gImuActions.orientation = newOrientation;
    gImuActions.lastOrientationChangeMs = now;
  }
}

// ============================================================================
// IMU UI Settings Commands
// ============================================================================

const char* cmd_imupollingms(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Usage: imupollingms <50..2000>";
  int v = _arg.toInt();
  if (v < 50 || v > 2000) return "Error: imuPollingMs must be 50..2000";
  setSetting(gSettings.imuPollingMs, v);
  BROADCAST_PRINTF("imuPollingMs set to %d", v);
  return "OK";
}

const char* cmd_imuewmafactor(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Usage: imuewmafactor <0.0..1.0>";
  float f = strtof(_arg.c_str(), nullptr);
  if (f < 0.0f || f > 1.0f) return "Error: imuEWMAFactor must be 0..1";
  setSetting(gSettings.imuEWMAFactor, f);
  BROADCAST_PRINTF("imuEWMAFactor set to %.3f", f);
  return "OK";
}

const char* cmd_imutransitionms(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Usage: imutransitionms <0..1000>";
  int v = _arg.toInt();
  if (v < 0 || v > 1000) return "Error: imuTransitionMs must be 0..1000";
  setSetting(gSettings.imuTransitionMs, v);
  BROADCAST_PRINTF("imuTransitionMs set to %d", v);
  return "OK";
}

const char* cmd_imuwebmaxfps(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Usage: imuwebmaxfps <1..30>";
  int v = _arg.toInt();
  if (v < 1 || v > 30) return "Error: imuWebMaxFps must be 1..30";
  setSetting(gSettings.imuWebMaxFps, v);
  BROADCAST_PRINTF("imuWebMaxRefreshRate set to %d", v);
  return "OK";
}

// ============================================================================
// IMU Device Settings Commands
// ============================================================================

const char* cmd_imudevicepollms(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Usage: imuDevicePollMs <50..1000>";
  int v = valStr.toInt();
  if (v < 50) v = 50;
  if (v > 1000) v = 1000;
  setSetting(gSettings.imuDevicePollMs, v);
  snprintf(getDebugBuffer(), 1024, "imuDevicePollMs set to %d", v);
  return getDebugBuffer();
}

const char* cmd_imuorientationmode(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
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

const char* cmd_imuorientationcorrection(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) {
    return gSettings.imuOrientationCorrectionEnabled ? "Current imuOrientationCorrectionEnabled: 1" : "Current imuOrientationCorrectionEnabled: 0";
  }
  int v = valStr.toInt();
  setSetting(gSettings.imuOrientationCorrectionEnabled, (bool)(v != 0));
  return gSettings.imuOrientationCorrectionEnabled ? "imuOrientationCorrectionEnabled set to 1" : "imuOrientationCorrectionEnabled set to 0";
}

const char* cmd_imupitchoffset(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
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

const char* cmd_imurolloffset(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
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

const char* cmd_imuyawoffset(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
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

const char* cmd_imuautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput;
  normalizeCliArg(arg);
  if (arg.length() == 0) {
    return gSettings.imuAutoStart ? "[IMU] Auto-start: enabled" : "[IMU] Auto-start: disabled";
  }
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
// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
const CommandEntry imuCommands[] = {
  // Start/Stop (3-level voice: "sensor" -> "motion sensor" -> "open/close")
  { "openimu", "Start BNO055 IMU sensor.", false, cmd_imustart, nullptr, "sensor", "motion sensor", "open" },
  { "closeimu", "Stop BNO055 IMU sensor.", false, cmd_imustop, nullptr, "sensor", "motion sensor", "close" },
  
  // Information
  { "imuread", "Read IMU sensor data. (add 'json' for JSON output)", false, cmd_imu },
  { "imuactions", "Show IMU action detection state.", false, cmd_imuactions },
  
  // UI Settings (client-side visualization)
  { "imupollingms", "IMU UI polling interval: <50..2000>", true, cmd_imupollingms, "Usage: imupollingms <50..2000>" },
  { "imuewmafactor", "IMU EWMA smoothing: <0.0..1.0>", true, cmd_imuewmafactor, "Usage: imuewmafactor <0.0..1.0>" },
  { "imutransitionms", "IMU transition time: <0..1000>", true, cmd_imutransitionms, "Usage: imutransitionms <0..1000>" },
  { "imuwebmaxfps", "IMU web max FPS: <1..30>", true, cmd_imuwebmaxfps, "Usage: imuwebmaxfps <1..30>" },
  
  // Device-level settings (sensor hardware behavior)
  { "imudevicepollms", "IMU device poll interval: <50..1000>", true, cmd_imudevicepollms, "Usage: imuDevicePollMs <50..1000>" },
  { "imuorientationmode", "IMU orientation mode: <0..8>", true, cmd_imuorientationmode, "Usage: imuorientationmode <0..8>" },
  { "imuorientationcorrection", "IMU orientation correction: <0|1>", true, cmd_imuorientationcorrection, "Usage: imuorientationcorrection <0|1>" },
  { "imupitchoffset", "IMU pitch offset in degrees (recommended -180..180)", true, cmd_imupitchoffset, "Usage: imupitchoffset <degrees>  (recommended -180..180)" },
  { "imurolloffset", "IMU roll offset in degrees (recommended -180..180)", true, cmd_imurolloffset, "Usage: imurolloffset <degrees>  (recommended -180..180)" },
  { "imuyawoffset", "IMU yaw offset in degrees (recommended -180..180)", true, cmd_imuyawoffset, "Usage: imuyawoffset <degrees>  (recommended -180..180)" },
  
  // Auto-start
  { "imuautostart", "Enable/disable IMU auto-start after boot [on|off]", false, cmd_imuautostart, "Usage: imuautostart [on|off]" },
};

const size_t imuCommandsCount = sizeof(imuCommands) / sizeof(imuCommands[0]);

// ============================================================================
// Command Registration (Sensor-Specific)
// ============================================================================
// Direct static registration to avoid macro issues
// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// IMU Task Implementation (moved from i2c_system.cpp for full modularization)
// ============================================================================

// ============================================================================
// IMU Task - FreeRTOS Task Function
// ============================================================================
// Purpose: Continuously reads 9-DOF orientation data from BNO055 IMU sensor
// Stack: 4096 words (~16KB) | Priority: 1 | Core: Any
// Lifecycle: Created by cmd_imustart, deleted when gImuEnabled=false
// Polling: Configurable via imuDevicePollMs (default 200ms) | I2C Clock: 100kHz
//
// Cleanup Strategy:
//   1. Check gImuEnabled flag at loop start
//   2. Acquire bus mutex via I2CDeviceManager to prevent race conditions during cleanup
//   3. Delete sensor object and invalidate cache
//   4. Release mutex and delete task
// ============================================================================

void imuTask(void* parameter) {
  INFO_IMU_LIFECYCLEF("Task started (handle=%p, stack=%u words)", 
                (void*)xTaskGetCurrentTaskHandle(), 
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  INFO_IMU_LIFECYCLEF("[MODULAR] imuTask() running from i2csensor_bno055.cpp");
  unsigned long lastIMURead = 0;
  unsigned long lastStackLog = 0;
  while (true) {
    // CRITICAL: Check enabled flag FIRST for graceful shutdown
    if (!gImuEnabled) {
      gImuConnected = false;
      if (gBNO055 != nullptr) {
        delete gBNO055;
        gBNO055 = nullptr;
      }
      gImuCache.imuDataValid = false;
      gImuCache.imuSeq = 0;
      
      // Reset initialization flags for clean restart
      gImuInitRequested = false;
      gImuInitDone = false;
      gImuInitResult = false;
      
      SENSOR_TASK_EXIT(IMU);
    }
    
    // Update watermark diagnostics (only when enabled)
    if (isDebugFlagSet(DEBUG_PERFORMANCE)) {
      UBaseType_t wm = uxTaskGetStackHighWaterMark(NULL);
      gImuWatermarkNow = wm;
      if (wm < gImuWatermarkMin) gImuWatermarkMin = wm;
    }
    unsigned long nowLog = millis();
    if (nowLog - lastStackLog >= 5000UL) {
      lastStackLog = nowLog;
      if (checkTaskStackSafety("imu", IMU_STACK_WORDS, &gImuEnabled)) break;
      // CRITICAL: Check enabled flag again before debug output (prevent crash during shutdown)
      if (gImuEnabled) {
        DEBUG_PERFORMANCEF("[STACK] imu_task watermark_now=%u min=%u words", (unsigned)gImuWatermarkNow, (unsigned)gImuWatermarkMin);
        DEBUG_MEMORY_HEAPF("[HEAP] imu_task: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }
    // Handle deferred IMU initialization on task stack
    if (gImuEnabled && (!gImuConnected || gBNO055 == nullptr)) {
      if (gImuInitRequested) {
        bool ok = imuInit();
        gImuInitResult = ok;
        gImuInitDone = true;
        gImuInitRequested = false;
        if (!ok) {
          gImuEnabled = false;
        }
      }
    }

    if (gImuEnabled && gImuConnected && gBNO055 != nullptr && !pollPaused(0 /* legacy Wire1 = bus 0 */)) {
      unsigned long imuPollMs = (gSettings.imuDevicePollMs > 0) ? (unsigned long)gSettings.imuDevicePollMs : 200;
      unsigned long nowMs = millis();
      if (nowMs - lastIMURead >= imuPollMs) {
        // IMU reads ~5ms at 100kHz; fail fast and retry next poll rather than blocking 1000ms
        auto result = i2cTaskWithTimeout(I2C_ADDR_IMU, 100000, 100, [&]() -> bool {
          // Probe device presence so health system can track failures
          Wire1.beginTransmission(I2C_ADDR_IMU);
          if (Wire1.endTransmission() != 0) return false;
          imuPoll();
          return true;
        });
        lastIMURead = nowMs;
        
        // Mark OLED dirty if IMU page is active (enables real-time display updates)
#if ENABLE_OLED_DISPLAY
        if (result && currentOLEDMode == OLED_IMU_ACTIONS) {
          oledMarkDirty();
        }
#endif
        
        // Auto-disable if too many consecutive failures
        if (!result) {
          if (i2cShouldAutoDisable(I2C_ADDR_IMU)) {
            ERROR_IMUF("Too many consecutive IMU failures - auto-disabling");
            gImuEnabled = false;
            sensorStatusBumpWith("imu@auto_disabled");
          }
        }
        
        // ESP-NOW broadcaster reads imuBuildDataJSON() on demand from gImuCache.
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

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry imuSettingEntries[] = {
  { "imuAutoStart", SETTING_BOOL, &gSettings.imuAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr, false, nullptr, nullptr },
  { "imuPollingMs", SETTING_INT, &gSettings.imuPollingMs, 200, 0, nullptr, 50, 2000, "Polling (ms)", nullptr, false, "timing", nullptr },
  { "imuEWMAFactor", SETTING_FLOAT, &gSettings.imuEWMAFactor, 0, 0.1f, nullptr, 0, 1, "EWMA Factor", nullptr, false, "timing", nullptr },
  { "imuTransitionMs", SETTING_INT, &gSettings.imuTransitionMs, 100, 0, nullptr, 0, 1000, "Transition (ms)", nullptr, false, "timing", nullptr },
  { "imuWebMaxFps", SETTING_INT, &gSettings.imuWebMaxFps, 15, 0, nullptr, 1, 30, "Web Max FPS", nullptr, false, "timing", nullptr },
  { "imuDevicePollMs", SETTING_INT, &gSettings.imuDevicePollMs, 200, 0, nullptr, 50, 1000, "Poll Interval (ms)", nullptr, false, "timing", nullptr },
  { "imuOrientationMode", SETTING_INT, &gSettings.imuOrientationMode, 8, 0, nullptr, 0, 8, "Orientation Mode", "0:Normal,1:Flip Pitch,2:Flip Roll,3:Flip Yaw,4:Flip Pitch+Roll,5:Roll 180 Fix,6:Rotate 90 CCW,7:Alt Extreme Pitch,8:Upside Down", false, "orientation", nullptr },
  { "imuOrientationCorrectionEnabled", SETTING_BOOL, &gSettings.imuOrientationCorrectionEnabled, true, 0, nullptr, 0, 1, "Orientation Correction", nullptr, false, "orientation", nullptr },
  { "imuPitchOffset", SETTING_FLOAT, &gSettings.imuPitchOffset, 0, 0.0f, nullptr, -180, 180, "Pitch Offset", nullptr, false, "orientation", nullptr },
  { "imuRollOffset", SETTING_FLOAT, &gSettings.imuRollOffset, 0, 0.0f, nullptr, -180, 180, "Roll Offset", nullptr, false, "orientation", nullptr },
  { "imuYawOffset", SETTING_FLOAT, &gSettings.imuYawOffset, 0, 0.0f, nullptr, -180, 180, "Yaw Offset", nullptr, false, "orientation", nullptr }
};

static bool isIMUConnected() {
  return gImuConnected;
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule imuSettingsModule = {
  "imu",
  "hardware.sensors.imu",
  imuSettingEntries,
  sizeof(imuSettingEntries) / sizeof(imuSettingEntries[0]),
  isIMUConnected,
  "BNO055 9-axis IMU"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================
// IMU OLED Mode (Display Function + Registration)
// ============================================================================
#if DISPLAY_TYPE > 0
#include "i2csensor_bno055_oled.h"
#endif

#endif // ENABLE_IMU_SENSOR
