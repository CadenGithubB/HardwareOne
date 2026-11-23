// HAL_Motion.cpp - the shared motion (IMU) layer: the reading cache, the IMU
// task and its start/stop, the JSON builder, the action detector, the imu*
// commands, the settings module and the OLED page. The chip is the build's
// backend (HAL_MotionBackend.h, IMU_DEVICE_TYPE).
#include "HAL_Motion.h"
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_BuildConfig.h"
#include "System_MemoryMonitor.h"
#include "System_Utils.h"

#if ENABLE_IMU_SENSOR

#include <Arduino.h>
#include <esp_attr.h>  // EXT_RAM_BSS_ATTR
#include <freertos/semphr.h>

#include "HAL_MotionBackend.h"
#include "HAL_MotionCore.h"
#include "OLED_Display.h"
#include "System_ArgParse.h"
#include "System_Command.h"
#include "System_CommandReply.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_SensorJsonKeys.h"  // inter-device sensor JSON key names (receivers read the same)
#include "System_MemUtil.h"
#include "System_Mutex.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_CommandRegistry.h"

// IMU initialization handoff variables
volatile bool gImuInitRequested = false;
volatile bool gImuInitDone = false;
volatile bool gImuInitResult = false;

// Why the last start failed ("Error: <reason>"): written by the IMU task
// before it publishes gImuInitDone, reported once by imuStartInternal.
EXT_RAM_BSS_ATTR static char sImuInitWhy[160];

// Settings and debug
// sensorStatusBumpWith, gSensorPollingPaused, drainDebugRing provided by System_I2C.h

// ============================================================================
// IMU Sensor Cache (owned by this module)
// ============================================================================
// The mutex is retained across stops; readers never own the live storage.
// The reading and the detector state live in PSRAM (internal SRAM is scarce).
EXT_RAM_BSS_ATTR static ImuCache sImuCache;
static SemaphoreHandle_t sImuCacheMutex = nullptr;
// The action detector's state: updated by the IMU task under the cache lock,
// copied out by readers (imuActionsSnapshot). NOLOAD PSRAM drops the member
// initialisers: it is all-zero (no pose or direction codes) until
// imuResetActions runs at the start of each task run; readers check
// gImuRunning first.
EXT_RAM_BSS_ATTR static MotionActionState sImuActions;

bool imuCacheSnapshot(ImuCache& out, uint32_t timeoutMs) {
  out = {};
  SensorCacheGuard guard(sImuCacheMutex, pdMS_TO_TICKS(timeoutMs), "imu.snapshot");
  if (!guard.held) return false;
  out = sImuCache;
  return true;
}

size_t imuCacheStorageBytes() {
  return sizeof(sImuCache) + sizeof(sImuCacheMutex);
}

bool imuActionsSnapshot(MotionActionState& out, uint32_t timeoutMs) {
  out = MotionActionState{};
  SensorCacheGuard guard(sImuCacheMutex, pdMS_TO_TICKS(timeoutMs), "imu.actions");
  if (!guard.held) return false;
  out = sImuActions;
  return true;
}

static bool imuInvalidateCache() {
  SensorCacheGuard guard(sImuCacheMutex, pdMS_TO_TICKS(50), "imu.invalidate");
  if (!guard.held) return false;
  sImuCache.imuDataValid = false;
  sImuCache.imuSeq = 0;
  return true;
}

// IMU watermark tracking
volatile UBaseType_t gImuWatermarkMin = (UBaseType_t)0xFFFFFFFF;
volatile UBaseType_t gImuWatermarkNow = (UBaseType_t)0;

// IMU sensor state (definitions)
bool gImuRunning = false;
bool gImuConnected = false;
unsigned long gImuLastStopTime = 0;
TaskHandle_t gImuTaskHandle = nullptr;

// The bus and address the IMU was opened at (HAL_MotionCore.h): written by
// the IMU task when it opens and closes the chip, read by everyone else.
static MotionOpenTarget sImuOpen;

// The imuAddress setting, through the backend's rule: what the next open uses.
static uint8_t imuConfiguredAddress() { return motionBackendAddress(gSettings.imuAddress); }
// A two-chip backend's second chip (the 4517's LIS3MDL, imuMagAddress); 0 for one chip.
static uint8_t imuConfiguredMagAddress() { return motionBackendSecondAddress(gSettings.imuMagAddress); }
// The address in use: the open one while the IMU is open, else the setting.
uint8_t imuDeviceAddress() { return motionActiveAddress(sImuOpen, imuConfiguredAddress()); }
uint8_t imuMagDeviceAddress() { return motionActiveSecondAddress(sImuOpen, imuConfiguredMagAddress()); }
static uint8_t imuDeviceBus() { return motionActiveBus(sImuOpen, (uint8_t)gSettings.imuBus); }
static void imuCloseTarget() { sImuOpen.open = false; }
const char* imuBackendName() { return motionBackendInfo().part; }

// ============================================================================
// IMU Sensor Command Handlers
// ============================================================================

const char* cmd_imu(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    if (!ensureDebugBuffer()) return SENSOR_JSON_NOBUF;
    int n = imuBuildDataJson(getDebugBuffer(), 1024);  // shared builder (also feeds sensors json / MQTT)
    return (n > 0) ? getDebugBuffer() : SENSOR_JSON_UNAVAILABLE;
  }

  if (!gImuConnected || !gImuRunning) {
    return "Error: IMU sensor not connected or not started. Use 'openimu' first.";
  }

  // Read from sensor cache instead of accessing hardware directly
  {
    ImuCache reading;
    if (imuCacheSnapshot(reading, 100)) {
      if (reading.imuDataValid) {
        // Without a heading (the 4517 before its magnetometer calibration),
        // yaw counts from the start.
        BROADCAST_PRINTF("Orientation - Yaw%s: %.1f° Pitch: %.1f° Roll: %.1f°",
                         reading.hasHeading ? "" : " (relative)",
                         reading.oriYaw, reading.oriPitch, reading.oriRoll);
        BROADCAST_PRINTF("Acceleration - X: %.2f Y: %.2f Z: %.2f m/s²",
                         reading.accelX, reading.accelY, reading.accelZ);
        BROADCAST_PRINTF("Gyroscope - X: %.2f Y: %.2f Z: %.2f rad/s",
                         reading.gyroX, reading.gyroY, reading.gyroZ);
        if (reading.hasMag) {
          BROADCAST_PRINTF("Magnetometer - X: %.1f Y: %.1f Z: %.1f uT",
                           reading.magX, reading.magY, reading.magZ);
        }
        BROADCAST_PRINTF("Temperature: %.1f°C", reading.imuTemp);
      } else {
        return "Error: IMU data not available yet. Wait a second after openimu and retry.";
      }
    } else {
      return "Error: Failed to access sensor cache";
    }
  }

  return "OK";
}

// Internal function called by queue processor
bool imuStartInternal() {
  if (gImuRunning) return true;
  // A stopped task owns its resources until cleanup publishes a null handle.
  if (gImuTaskHandle != nullptr) return false;

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
  if (!sImuCacheMutex) {
    sImuCacheMutex = xSemaphoreCreateMutex();
    if (!sImuCacheMutex) {
      ERROR_IMUF("Failed to create cache mutex");
      return false;
    }
    DEBUG_IMU_LIFECYCLEF("[IMU] Cache mutex created");
  }

  // Clean up any stale cache from previous run BEFORE starting
  {
    SensorCacheGuard g(sImuCacheMutex, pdMS_TO_TICKS(100), "imu.cleanStaleCache");
    if (!g.held) return false;
    sImuCache.imuDataValid = false;
    sImuCache.oriYaw = 0;
    sImuCache.oriPitch = 0;
    sImuCache.oriRoll = 0;
    sImuCache.accelX = 0;
    sImuCache.accelY = 0;
    sImuCache.accelZ = 0;
    sImuCache.gyroX = 0;
    sImuCache.gyroY = 0;
    sImuCache.gyroZ = 0;
    sImuCache.magX = 0;
    sImuCache.magY = 0;
    sImuCache.magZ = 0;
    sImuCache.imuTemp = 0;
    sImuCache.hasMag = sImuCache.hasOrientation = sImuCache.hasHeading = sImuCache.hasCal = false;
    sImuCache.cal = 0;
  }

  // CRITICAL: Enable flag BEFORE creating task to prevent race condition
  // Task checks gImuRunning first thing and will delete itself if false
  bool prev = gImuRunning;
  gImuRunning = true;  // Set this BEFORE task creation

  // Defer initialization to imuTask; wait for its result below
  if (!gImuConnected) {
    sImuInitWhy[0] = '\0';
    gImuInitDone = false;
    gImuInitResult = false;
    gImuInitRequested = true;
  }

  // Create IMU task lazily (after setting gImuRunning=true)
  if (!createImuTask()) {
    DEBUG_CLIF("Failed to create IMU task (insufficient memory or resources)");
    gImuRunning = false;  // Reset flag on failure
    gImuConnected = false;
    imuCloseTarget();
    gImuInitRequested = false;
    gImuInitDone = false;
    gImuInitResult = false;
    motionBackendStop();
    return false;
  }
  if (gImuRunning != prev) {
    sensorStatusBumpWith("openimu@queue");
  }

  // The backend's init ends within kMotionInitBudgetMs on its own, so this
  // wait reports the backend's reason; it gives up only on a stalled task.
  if (gImuInitRequested || !gImuConnected) {
    unsigned long start = millis();
    while (gImuRunning && !gImuInitDone && (millis() - start) < kMotionStartWaitMs) {
      delay(10);
    }
    if (!gImuRunning || !gImuInitDone || !gImuInitResult) {
      gImuRunning = false;
      // One reason, from here: the backend's, or the task's silence.
      const char* why = (gImuInitDone && sImuInitWhy[0]) ? sImuInitWhy
          : "Error: The IMU task did not finish starting the chip. Run 'openimu' again.";
      ERROR_IMUF("%s", why);
      broadcastOutput(why);
      return false;
    }
  }
  DEBUG_CLIF("[IMU_INTERNAL] SUCCESS: IMU started");
  return true;
}

// Public command - uses centralized queue
const char* cmd_imustart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (const char* error = i2cOpenCloseArgError(I2C_DEVICE_IMU, argsInput, "openimu")) return error;
  return cmd_sensorstart_queued(I2C_DEVICE_IMU, "IMU", "openimu@enqueue");
}

const char* cmd_imustop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (const char* error = i2cOpenCloseArgError(I2C_DEVICE_IMU, argsInput, "closeimu")) return error;
  return cmd_sensorstop_queued(I2C_DEVICE_IMU, "IMU");
}

const char* cmd_imuactions(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!gImuRunning || !gImuConnected) {
    return "Error: [IMU] Not enabled. Use 'openimu' first.";
  }

  // The detector's state as the IMU task left it (it runs on every sample).
  MotionActionState actions;
  if (!imuActionsSnapshot(actions, 100)) return "Error: The IMU is busy. Run 'imuactions' again.";

  // Stream output line-by-line instead of building in shared buffer
  broadcastOutput("IMU Action Detection Status:");

  // Shake
  if (actions.isShaking) {
    BROADCAST_PRINTF("  Shake: YES (intensity: %.1f, count: %lu)",
                     actions.shakeIntensity, (unsigned long)actions.shakeCount);
  } else {
    broadcastOutput("  Shake: no");
  }

  // Tilt
  if (actions.isTilted) {
    const char* dir = "?";
    switch (actions.tiltDirection) {
      case 'F': dir = "Forward"; break;
      case 'B': dir = "Back"; break;
      case 'L': dir = "Left"; break;
      case 'R': dir = "Right"; break;
    }
    BROADCAST_PRINTF("  Tilt: YES (%s, %.1f deg)", dir, actions.tiltAngle);
  } else {
    broadcastOutput("  Tilt: no");
  }

  // Tap
  if (actions.tapDetected || actions.tapCount > 0) {
    BROADCAST_PRINTF("  Tap: %s (count: %lu, strength: %.1f)",
                     actions.tapDetected ? "YES" : "no",
                     (unsigned long)actions.tapCount, actions.tapStrength);
  } else {
    broadcastOutput("  Tap: no");
  }

  // Rotation (the gyro is stored in rad/s; shown in deg/s)
  if (actions.isRotating) {
    BROADCAST_PRINTF("  Rotation: YES (%c-axis, %.1f deg/s)",
                     actions.rotationAxis, actions.rotationRateDps);
  } else {
    broadcastOutput("  Rotation: no");
  }

  // Freefall
  if (actions.isFreefalling) {
    BROADCAST_PRINTF("  Freefall: YES (%lu ms)", (unsigned long)actions.freefallDurationMs);
  } else {
    broadcastOutput("  Freefall: no");
  }

  // Steps
  if (actions.isWalking) {
    BROADCAST_PRINTF("  Steps: %lu (WALKING, %.1f steps/min)",
                     (unsigned long)actions.stepCount, actions.stepFrequency);
  } else {
    BROADCAST_PRINTF("  Steps: %lu", (unsigned long)actions.stepCount);
  }

  // Orientation
  const char* orient = "?";
  switch (actions.orientation) {
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
// IMU Initialization and Reading (on the IMU task)
// ============================================================================

// Start the backend's chip on the configured bus and address. Runs on the IMU
// task, so the start command never blocks the bus itself.
static bool imuInitBackend() {
  imuCloseTarget();
  const uint8_t bus = (uint8_t)gSettings.imuBus;
  const uint8_t address = imuConfiguredAddress();
  const uint8_t second = imuConfiguredMagAddress();
  // The address settings can also arrive by a settings write, which checks
  // only their range: refuse an address another device of this build holds
  // (each chip's, for a two-chip backend).
  char clash[104];  // with the advice below, fits sImuInitWhy
  if (i2cAddressTakenBy(bus, address, I2C_DEVICE_IMU, 1, clash, sizeof(clash))) {
    snprintf(sImuInitWhy, sizeof(sImuInitWhy), "%s Change 'imuaddress' or 'imubus', then 'openimu'.", clash);
    return false;
  }
  if (second != 0 && i2cAddressTakenBy(bus, second, I2C_DEVICE_IMU, 1, clash, sizeof(clash))) {
    snprintf(sImuInitWhy, sizeof(sImuInitWhy), "%s Change 'imumagaddress' or 'imubus', then 'openimu'.", clash);
    return false;
  }
  // From here the IMU claims this bus and these addresses until it closes.
  sImuOpen.bus = bus;
  sImuOpen.address = address;
  sImuOpen.secondAddress = second;
  sImuOpen.open = true;
  INFO_IMU_LIFECYCLEF("Starting %s at 0x%02X on bus %u", imuBackendName(), (unsigned)address, (unsigned)bus);
  if (!motionBackendInit(bus, address, second, sImuInitWhy, sizeof(sImuInitWhy))) {
    imuCloseTarget();
    if (!sImuInitWhy[0]) {
      snprintf(sImuInitWhy, sizeof(sImuInitWhy), "Error: The %s at 0x%02X on bus %u did not start.",
               imuBackendName(), (unsigned)address, (unsigned)bus);
    }
    return false;
  }
  gImuConnected = true;
  INFO_IMU_LIFECYCLEF("%s started", imuBackendName());
  return true;
}

static MotionOrientationSettings imuOrientationSettings() {
  MotionOrientationSettings settings;
  settings.enabled = gSettings.imuOrientationCorrectionEnabled;
  settings.mode = gSettings.imuOrientationMode;
  settings.pitchOffset = gSettings.imuPitchOffset;
  settings.rollOffset = gSettings.imuRollOffset;
  settings.yawOffset = gSettings.imuYawOffset;
  return settings;
}

// One ready sample: publish it to the cache when `publish` (once per
// imuDevicePollMs), and run the action detector on it either way. Called by
// the IMU task after each successful read. Returns whether it was published.
static bool imuStoreSample(const MotionSample& sample, bool publish, unsigned long sampleMs) {
  float pitch = sample.pitch, roll = sample.roll, yaw = sample.yaw;
  motionApplyOrientationCorrection(imuOrientationSettings(), pitch, roll, yaw);

  bool updated = false;
  if (publish) {
    // Pattern 3: take/write/release, then run imuUpdateActions WITHOUT the lock
    // (it takes its own short lock).
    SensorCacheGuard g(sImuCacheMutex, pdMS_TO_TICKS(50), "imu.pollWrite");
    if (g.held) {
      sImuCache.accelX = sample.accel[0];
      sImuCache.accelY = sample.accel[1];
      sImuCache.accelZ = sample.accel[2];
      sImuCache.gyroX = sample.gyro[0];
      sImuCache.gyroY = sample.gyro[1];
      sImuCache.gyroZ = sample.gyro[2];
      sImuCache.magX = sample.mag[0];
      sImuCache.magY = sample.mag[1];
      sImuCache.magZ = sample.mag[2];
      sImuCache.oriYaw = yaw;
      sImuCache.oriPitch = pitch;
      sImuCache.oriRoll = roll;
      sImuCache.imuTemp = sample.temp;
      sImuCache.hasMag = sample.hasMag;
      sImuCache.hasOrientation = sample.hasOrientation;
      sImuCache.hasHeading = sample.hasHeading;
      sImuCache.hasCal = sample.hasCal;
      sImuCache.cal = sample.cal;
      sImuCache.backend = motionBackendInfo().id;
      sImuCache.imuLastUpdate = millis();
      sImuCache.imuDataValid = true;
      sImuCache.imuSeq++;
      updated = true;
    }
  }
  if (updated) {
    DEBUG_IMU_VALUESF("IMU data updated");
  } else if (publish) {
    DEBUG_IMU_POLLINGF("imu sample: cache lock busy - skipping update");
  }
  // Every sample reaches the detector (a 150 ms fall is 7 samples at 50 Hz).
  MotionActionSample actions;
  actions.ax = sample.accel[0];
  actions.ay = sample.accel[1];
  actions.az = sample.accel[2];
  actions.gx = sample.gyro[0];
  actions.gy = sample.gyro[1];
  actions.gz = sample.gyro[2];
  actions.pitch = pitch;
  actions.roll = roll;
  actions.hasHwTap = sample.hasHwTap;
  actions.hwTap = sample.hwTap;
  actions.hasHwFreefall = sample.hasHwFreefall;
  actions.hwFreefall = sample.hwFreefall;
  actions.periodMs = (uint16_t)(sampleMs < 0xFFFF ? sampleMs : 0xFFFF);
  imuUpdateActions(actions, /*postEvents=*/true);
  return updated;
}

// JSON Building
// ============================================================================

// Build IMU JSON directly into buffer using snprintf (zero String allocations)
int imuBuildDataJson(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;

  ImuCache reading;
  const bool available = imuCacheSnapshot(reading, CACHE_MUTEX_TIMEOUT_MS);
  if (!available) {
    // Cache-lock timeout: not-ready envelope (was: {"error":"IMU cache timeout"}).
    int pos = sensorEnvelopeBegin(buf, bufSize, false, gImuConnected, 0);
    if (pos == 0) return 0;
    int n = snprintf(buf + pos, bufSize - pos, ",\"accel\":null,\"gyro\":null,"
                     HW1_SK_Q(HW1_SK_ORI) "null,\"temp\":null}");
    if (n < 0 || (size_t)n >= bufSize - pos) return 0;
    return pos + n;
  }

  // valid/connected/ts come from the shared envelope. Body carries ONLY the
  // measurement (accel/gyro/ori/temp, then mag and cal when the backend has
  // them). Dropped: seq (bookkeeping, redundant with ts), enabled (duplicate of
  // the discovery-layer enabled), init* flags (device-state, unconsumed) — see
  // docs/SENSOR_ENVELOPE_CLEANUP_PLAN.md.
  int pos = sensorEnvelopeBegin(buf, bufSize, reading.imuDataValid, gImuConnected, reading.imuLastUpdate);
  if (pos == 0) return 0;
  // Precision is matched to the hardware, not habit: BNO055 accel LSB is
  // 0.01 m/s² (a third decimal is always '0'), gyro LSB is 0.0625 dps ≈ 0.0011
  // rad/s (third decimal is real), mag LSB 0.0625 uT, temp is whole degrees.
  // Keeping the reading small matters: up to 190 bytes travel in one frame
  // (more is cut into up to four, System_ESPNow_SensorCore.h), and motion
  // widens every float's integer part.
  int n = snprintf(buf + pos, bufSize - pos,
                   ",\"accel\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f},"
                   "\"gyro\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},"
                   HW1_SK_Q(HW1_SK_ORI) "{" HW1_SK_Q(HW1_SK_YAW) "%.2f,"
                   HW1_SK_Q(HW1_SK_PITCH) "%.2f," HW1_SK_Q(HW1_SK_ROLL) "%.2f},"
                   "\"temp\":%d",
                   reading.accelX, reading.accelY, reading.accelZ,
                   reading.gyroX, reading.gyroY, reading.gyroZ,
                   reading.oriYaw, reading.oriPitch, reading.oriRoll,
                   (int)reading.imuTemp);
  if (n < 0 || (size_t)n >= bufSize - pos) return 0;
  pos += n;
  if (reading.hasMag) {
    n = snprintf(buf + pos, bufSize - pos, "," HW1_SK_Q(HW1_SK_MAG) "{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}",
                 reading.magX, reading.magY, reading.magZ);
    if (n < 0 || (size_t)n >= bufSize - pos) return 0;
    pos += n;
  }
  if (reading.hasCal) {
    n = snprintf(buf + pos, bufSize - pos, "," HW1_SK_Q(HW1_SK_CAL) "%u", (unsigned)reading.cal);
    if (n < 0 || (size_t)n >= bufSize - pos) return 0;
    pos += n;
  }
  if ((size_t)pos + 2 > bufSize) return 0;
  buf[pos++] = '}';
  buf[pos] = '\0';
  return pos;
}

// ============================================================================
// IMU Action Detection
// ============================================================================

// Run the detector on one new sample (after the orientation correction).
// Only the IMU task calls this, once per sample, published or not;
// postEvents posts the imu_* events it finds.
void imuUpdateActions(const MotionActionSample& sample, bool postEvents) {
  if (!gImuRunning || !gImuConnected) return;
  MotionEvents events;
  {
    SensorCacheGuard guard(sImuCacheMutex, pdMS_TO_TICKS(CACHE_MUTEX_TIMEOUT_MS), "imu.actions");
    if (!guard.held) return;
    motionActionsUpdate(sImuActions, sample, millis(), postEvents ? &events : nullptr);
  }
  // Posted after the lock is released.
  for (uint8_t i = 0; i < events.count; ++i) {
    const MotionEvent& event = events.items[i];
    const char* detail = event.detail[0] ? event.detail : nullptr;
    switch (event.kind) {
      case MotionEventKind::Shake: systemEventPost(SYSEVT_IMU_SHAKE, event.subject); break;
      case MotionEventKind::Tap: systemEventPost(SYSEVT_IMU_TAP, event.subject); break;
      case MotionEventKind::Freefall: systemEventPost(SYSEVT_IMU_FREEFALL, event.subject); break;
      case MotionEventKind::Walking: systemEventPost(SYSEVT_IMU_WALKING, event.subject, detail); break;
      case MotionEventKind::Orientation:
        systemEventPost(SYSEVT_IMU_ORIENTATION, event.subject, detail);
        break;
    }
  }
}

// A fresh detector for each run of the task (no history from a previous run).
static void imuResetActions() {
  SensorCacheGuard guard(sImuCacheMutex, pdMS_TO_TICKS(100), "imu.actionsReset");
  if (guard.held) sImuActions = MotionActionState{};
}

// ============================================================================
// IMU web view settings (read by the web Sensors page)
// ============================================================================

const char* cmd_imupollingms(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: imupollingms <50..2000>";
  int v = 0;
  if (!parseIntArg(_arg, v)) return kCommandReplyNotANumber;
  if (v < 50 || v > 2000) return "Error: imuPollingMs must be 50..2000";
  setSetting(gSettings.imuPollingMs, v);
  BROADCAST_PRINTF("imuPollingMs set to %d", v);
  return "OK";
}

const char* cmd_imuewmafactor(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: imuewmafactor <0.0..1.0>";
  float f = 0;
  if (!parseFloatArg(_arg, f)) return kCommandReplyNotADecimal;
  if (f < 0.0f || f > 1.0f) return "Error: imuEWMAFactor must be 0..1";
  setSetting(gSettings.imuEwmaFactor, f);
  BROADCAST_PRINTF("imuEWMAFactor set to %.3f", f);
  return "OK";
}

const char* cmd_imutransitionms(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: imutransitionms <0..1000>";
  int v = 0;
  if (!parseIntArg(_arg, v)) return kCommandReplyNotANumber;
  if (v < 0 || v > 1000) return "Error: imuTransitionMs must be 0..1000";
  setSetting(gSettings.imuTransitionMs, v);
  BROADCAST_PRINTF("imuTransitionMs set to %d", v);
  return "OK";
}

const char* cmd_imuwebmaxfps(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: imuwebmaxfps <1..30>";
  int v = 0;
  if (!parseIntArg(_arg, v)) return kCommandReplyNotANumber;
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
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: imuDevicePollMs <50..1000>";
  int v = 0;
  if (!parseIntArg(valStr, v)) return kCommandReplyNotANumber;
  if (v < 50 || v > 1000) return "Error: imuDevicePollMs must be 50..1000";
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  setSetting(gSettings.imuDevicePollMs, v);
  snprintf(getDebugBuffer(), 1024, "imuDevicePollMs set to %d", v);
  return getDebugBuffer();
}

const char* cmd_imuorientationmode(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  if (valStr.length() == 0) {
    snprintf(getDebugBuffer(), 1024, "Current imuOrientationMode: %d (0=normal, 1=flip_pitch, 2=flip_roll, 3=flip_yaw, 4=flip_pitch_roll, 5=roll_180_fix, 6=rotate_90ccw, 7=alt_extreme_pitch, 8=upside_down)", gSettings.imuOrientationMode);
    return getDebugBuffer();
  }
  int v = 0;
  if (!parseIntArg(valStr, v)) return kCommandReplyNotANumber;
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
  const int v = parseBoolArg(valStr);
  if (v < 0) return kCommandReplyNotOnOff;
  setSetting(gSettings.imuOrientationCorrectionEnabled, v == 1);
  return gSettings.imuOrientationCorrectionEnabled ? "imuOrientationCorrectionEnabled set to 1" : "imuOrientationCorrectionEnabled set to 0";
}

// One of the three angle offsets: no value shows it.
static const char* imuOffsetCommand(float& field, const String& argsInput, const char* name) {
  String valStr = argsInput;
  valStr.trim();
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  if (valStr.length() == 0) {
    snprintf(getDebugBuffer(), 1024, "Current %s: %.2f", name, field);
    return getDebugBuffer();
  }
  float v = 0;
  if (!parseFloatArg(valStr, v)) return kCommandReplyNotADecimal;
  if (v < -180.0f || v > 180.0f) {
    snprintf(getDebugBuffer(), 1024, "Error: %s must be -180..180 degrees.", name);
    return getDebugBuffer();
  }
  setSetting(field, v);
  snprintf(getDebugBuffer(), 1024, "%s set to %.2f", name, v);
  return getDebugBuffer();
}

const char* cmd_imupitchoffset(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return imuOffsetCommand(gSettings.imuPitchOffset, argsInput, "imuPitchOffset");
}

const char* cmd_imurolloffset(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return imuOffsetCommand(gSettings.imuRollOffset, argsInput, "imuRollOffset");
}

const char* cmd_imuyawoffset(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return imuOffsetCommand(gSettings.imuYawOffset, argsInput, "imuYawOffset");
}

// imuaddress [addr]: the backend's addresses, hex or decimal. Refused when
// another device of this build holds it on the IMU bus; it applies at the
// next openimu.
const char* cmd_imuaddress(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String text = argsInput;
  text.trim();
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  // While open, the chip stays at its open address until the next openimu.
  const bool openElsewhere = sImuOpen.open && sImuOpen.address != imuConfiguredAddress();
  if (text.length() == 0) {
    snprintf(getDebugBuffer(), 1024, "[IMU] %s address: 0x%02X on I2C%u%s", imuBackendName(),
             (unsigned)imuConfiguredAddress(), (unsigned)gSettings.imuBus + 1,
             openElsewhere ? " (open at the old address until the next 'openimu')" : "");
    return getDebugBuffer();
  }
  int address = 0;
  if (!parseAddressArg(text, 0x08, 0x77, address) || motionBackendAddress(address) != address) {
    snprintf(getDebugBuffer(), 1024,
             "Error: That is not a %s address (%s). Read the address with 'i2cscan'.",
             imuBackendName(), IMU_ADDRESS_CHOICES_TEXT);
    return getDebugBuffer();
  }
  char why[112];
  if (i2cAddressTakenBy((uint8_t)gSettings.imuBus, (uint8_t)address, I2C_DEVICE_IMU, 1, why, sizeof(why))) {
    snprintf(getDebugBuffer(), 1024, "%s Use the chip's other address or put the IMU on the other bus ('imubus').",
             why);
    return getDebugBuffer();
  }
  setSetting(gSettings.imuAddress, address);
  if (sImuOpen.open && sImuOpen.address != address) {
    snprintf(getDebugBuffer(), 1024, "[IMU] Address 0x%02X saved; it applies at the next 'openimu' (open at "
             "0x%02X until then)", (unsigned)address, (unsigned)sImuOpen.address);
  } else {
    snprintf(getDebugBuffer(), 1024, "[IMU] Address 0x%02X saved; it applies at the next 'openimu'",
             (unsigned)address);
  }
  return getDebugBuffer();
}

#if ENABLE_IMU_LSM6DSOX
// imumagaddress [addr]: the 4517's magnetometer (LIS3MDL), 0x1C or 0x1E, hex
// or decimal. Refused when another device of this build holds it on the IMU
// bus; it applies at the next openimu.
const char* cmd_imumagaddress(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String text = argsInput;
  text.trim();
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  const uint8_t configured = imuConfiguredMagAddress();
  const bool openElsewhere = sImuOpen.open && sImuOpen.secondAddress != configured;
  const char* part = motionBackendInfo().secondPart;
  if (text.length() == 0) {
    snprintf(getDebugBuffer(), 1024, "[IMU] %s address: 0x%02X on I2C%u%s", part, (unsigned)configured,
             (unsigned)gSettings.imuBus + 1,
             openElsewhere ? " (open at the old address until the next 'openimu')" : "");
    return getDebugBuffer();
  }
  int address = 0;
  if (!parseAddressArg(text, 0x08, 0x77, address) || motionBackendSecondAddress(address) != address) {
    snprintf(getDebugBuffer(), 1024, "Error: That is not a %s address (%s). Read the address with 'i2cscan'.",
             part, IMU_MAG_ADDRESS_CHOICES_TEXT);
    return getDebugBuffer();
  }
  char why[112];
  if (i2cAddressTakenBy((uint8_t)gSettings.imuBus, (uint8_t)address, I2C_DEVICE_IMU, 1, why, sizeof(why))) {
    snprintf(getDebugBuffer(), 1024, "%s Use the chip's other address or put the IMU on the other bus ('imubus').",
             why);
    return getDebugBuffer();
  }
  setSetting(gSettings.imuMagAddress, address);
  if (sImuOpen.open && sImuOpen.secondAddress != address) {
    snprintf(getDebugBuffer(), 1024, "[IMU] %s address 0x%02X saved; it applies at the next 'openimu' (open at "
             "0x%02X until then)", part, (unsigned)address, (unsigned)sImuOpen.secondAddress);
  } else {
    snprintf(getDebugBuffer(), 1024, "[IMU] %s address 0x%02X saved; it applies at the next 'openimu'", part,
             (unsigned)address);
  }
  return getDebugBuffer();
}
#endif

const char* cmd_imuautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput;
  normalizeCliArg(arg);
  if (arg.length() == 0) {
    return gSettings.imuAutoStart ? "[IMU] Auto-start: enabled" : "[IMU] Auto-start: disabled";
  }
  const int v = parseBoolArg(arg);
  if (v < 0) return kCommandReplyNotOnOff;
  setSetting(gSettings.imuAutoStart, v == 1);
  return v == 1 ? "[IMU] Auto-start enabled" : "[IMU] Auto-start disabled";
}

// IMU Command Registry
// ============================================================================
// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
// Bus routing for this device. Lives here rather than in System_I2C.cpp so it
// compiles out with the driver — see i2cSetDeviceBusAndReport in System_I2C.h.
static const char* cmd_imubus(const String& a) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return i2cSetDeviceBusAndReport(gSettings.imuBus, a, "imuBus", i2cInstanceOwner(I2C_DEVICE_IMU, 1));
}

const CommandEntry imuCommands[] = {
  // Start/Stop (3-level voice: "sensor" -> "motion sensor" -> "open/close")
  { "openimu", "Start the IMU (motion sensor).", true, cmd_imustart },
  { "closeimu", "Stop the IMU (motion sensor).", true, cmd_imustop },

  // Information
  { "imuread", "Read IMU sensor data. (add 'json' for JSON output)", false, cmd_imu },
  { "imuactions", "Show IMU action detection state.", false, cmd_imuactions },

  // Web view settings (the web Sensors page reads them)
  { "imupollingms", "IMU UI polling interval: <50..2000>", true, cmd_imupollingms, "Usage: imupollingms <50..2000>" },
  { "imuewmafactor", "IMU EWMA smoothing: <0.0..1.0>", true, cmd_imuewmafactor, "Usage: imuewmafactor <0.0..1.0>" },
  { "imutransitionms", "IMU transition time: <0..1000>", true, cmd_imutransitionms, "Usage: imutransitionms <0..1000>" },
  { "imuwebmaxfps", "IMU web max FPS: <1..30>", true, cmd_imuwebmaxfps, "Usage: imuwebmaxfps <1..30>" },

  // Device-level settings (sensor hardware behavior)
  { "imudevicepollms", "IMU device poll interval: <50..1000>", true, cmd_imudevicepollms, "Usage: imuDevicePollMs <50..1000>" },
  { "imuorientationmode", "IMU orientation mode: <0..8>", true, cmd_imuorientationmode, "Usage: imuorientationmode <0..8>" },
  { "imuorientationcorrection", "IMU orientation correction: <0|1>", true, cmd_imuorientationcorrection, "Usage: imuorientationcorrection <0|1>" },
  { "imupitchoffset", "IMU pitch offset in degrees: <-180..180>", true, cmd_imupitchoffset, "Usage: imupitchoffset <-180..180>" },
  { "imurolloffset", "IMU roll offset in degrees: <-180..180>", true, cmd_imurolloffset, "Usage: imurolloffset <-180..180>" },
  { "imuyawoffset", "IMU yaw offset in degrees: <-180..180>", true, cmd_imuyawoffset, "Usage: imuyawoffset <-180..180>" },
  // The help names both chips (one text for every build); a refused value
  // names this build's addresses.
  { "imuaddress", "IMU I2C address (next openimu): [addr] (BNO055 0x28|0x29, Adafruit 4517 0x6A|0x6B)", true, cmd_imuaddress, "Usage: imuaddress [addr]" },
#if ENABLE_IMU_LSM6DSOX
  { "imumagaddress", "IMU magnetometer I2C address (next openimu): [" IMU_MAG_ADDRESS_CHOICES_TEXT "]", true, cmd_imumagaddress, "Usage: imumagaddress [" IMU_MAG_ADDRESS_CHOICES_TEXT "]" },
#endif

  // Auto-start
  { "imuautostart", "Enable/disable IMU auto-start after boot [on|off]", true, cmd_imuautostart, "Usage: imuautostart [on|off]" },
  { "imubus",      "Route the IMU to bus: <0|1> (reboot required)",   true, cmd_imubus,      "Usage: imuBus <0|1>" },
};

const size_t imuCommandsCount = sizeof(imuCommands) / sizeof(imuCommands[0]);

// Registration handled by gCommandModules[] in System_CommandRegistry.cpp

// ============================================================================
// IMU Task - FreeRTOS Task Function
// ============================================================================
// Purpose: starts the backend's chip, then reads a sample every
// imuDevicePollMs (or at the backend's fixed rate, 50 Hz for the 4517), runs
// the action detector on each and refreshes the cache every imuDevicePollMs
// Stack: 4096 BYTES (4 KB) | Priority: 1 | Core: I2C_SENSOR_CORE
// Lifecycle: Created by imuStartInternal, deleted when gImuRunning=false
//
// Cleanup Strategy:
//   1. Check gImuRunning flag at loop start
//   2. Invalidate the cache, release the backend
//   3. Publish a null handle and delete the task (SENSOR_TASK_EXIT)
// ============================================================================

void imuTask(void* parameter) {
  INFO_IMU_LIFECYCLEF("Task started (handle=%p, stack=%u words)",
                (void*)xTaskGetCurrentTaskHandle(),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  imuResetActions();
  unsigned long lastImuRead = 0;
  unsigned long lastPublish = 0;
  bool publishedOnce = false;
  unsigned long lastStackLog = 0;
  while (true) {
    // CRITICAL: Check enabled flag FIRST for graceful shutdown
    if (!gImuRunning) {
      gImuConnected = false;
      imuCloseTarget();
      if (!imuInvalidateCache()) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      motionBackendStop();

      // Reset initialization flags for clean restart
      gImuInitRequested = false;
      gImuInitDone = false;
      gImuInitResult = false;

      SENSOR_TASK_EXIT(IMU, gImuTaskHandle);
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
      // 'continue' (not 'break') so the top-of-loop shutdown runs the clean
      // SENSOR_TASK_EXIT (vTaskDelete) path instead of returning from the task
      // function with a near-overflowed stack (IllegalInstruction panic).
      if (checkTaskStackSafety("imu", IMU_STACK_WORDS, &gImuRunning)) continue;
      // CRITICAL: Check enabled flag again before debug output (prevent crash during shutdown)
      if (gImuRunning) {
        DEBUG_PERFORMANCEF("[STACK] sensor_imu watermark_now=%u min=%u words", (unsigned)gImuWatermarkNow, (unsigned)gImuWatermarkMin);
        DEBUG_MEMORY_HEAPF("[HEAP] sensor_imu: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }
    // Deferred initialization on the task stack (the start command waits for it)
    if (gImuRunning && !gImuConnected && gImuInitRequested) {
      bool ok = imuInitBackend();
      gImuInitResult = ok;
      gImuInitDone = true;
      gImuInitRequested = false;
      if (!ok) {
        gImuRunning = false;
      }
    }

    // The bus and addresses the chip was opened at (a changed imuaddress or
    // imubus applies at the next openimu).
    const uint8_t imuBus = imuDeviceBus();
    if (gImuRunning && gImuConnected && !pollPaused(imuBus)) {
      // The cache is refreshed every imuDevicePollMs; the chip is read then,
      // or at the backend's own fixed rate when the layer fuses its samples
      // (the 4517: 50 Hz, every sample through the detector).
      const unsigned long imuPollMs =
          (gSettings.imuDevicePollMs > 0) ? (unsigned long)gSettings.imuDevicePollMs : 200;
      const unsigned long sampleMs = motionSamplePeriodMs(motionBackendInfo().samplePeriodMs, imuPollMs);
      unsigned long nowMs = millis();
      if (nowMs - lastImuRead >= sampleMs) {
        const uint8_t address = imuDeviceAddress();
        const uint8_t second = imuMagDeviceAddress();
        MotionSample sample;
        const bool result = motionBackendRead(imuBus, address, second, sample);
        // A fixed rate keeps its schedule; a late tick starts a new one.
        lastImuRead = (nowMs - lastImuRead < 2 * sampleMs) ? lastImuRead + sampleMs : nowMs;
        bool published = false;
        if (result && sample.ready) {
          const bool publish = motionPublishDue(publishedOnce, lastPublish, nowMs, imuPollMs, sampleMs);
          published = imuStoreSample(sample, publish, sampleMs);
          if (published) {
            publishedOnce = true;
            lastPublish = nowMs;
          }
        }

        // Mark OLED dirty if IMU page is active (enables real-time display updates)
#if ENABLE_OLED_DISPLAY
        if (published && currentOledMode == OLED_IMU_ACTIONS) {
          oledMarkDirty();
        }
#endif

        // Auto-disable if too many consecutive failures (of either chip)
        if (!result || sample.secondFailed) {
          if (i2cShouldAutoDisable(address, imuBus) || (second != 0 && i2cShouldAutoDisable(second, imuBus))) {
            ERROR_IMUF("Too many consecutive IMU failures - auto-disabling");
            noteSensorAutoDisabled(I2C_DEVICE_IMU);  // before the flag: see System_I2C.h
            gImuRunning = false;
            sensorStatusBumpWith("imu@auto_disabled");
            logSystemEvent("SENSOR", "IMU auto-disabled after too many consecutive I2C failures");
            systemEventPost(SYSEVT_SENSOR_FAULT, "IMU", "consecutive I2C failures");
          }
        }

        // ESP-NOW broadcaster reads imuBuildDataJson() on demand from sImuCache.
      }
      // Sleep until the next sample is due, at most 10 ms (a stop is seen soon).
      const unsigned long since = millis() - lastImuRead;
      const unsigned long wait = since < sampleMs ? sampleMs - since : 1;
      vTaskDelay(pdMS_TO_TICKS(wait < 10 ? wait : 10));
    } else {
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

// ============================================================================
// IMU Settings Module (for modular settings registry)
// ============================================================================

static const char kImuAddressOptions[] = IMU_ADDRESS_OPTIONS;
#if ENABLE_IMU_LSM6DSOX
static const char kImuMagAddressOptions[] = IMU_MAG_ADDRESS_OPTIONS;
#endif

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry imuSettingEntries[] = {
  { "imuAutoStart", SETTING_BOOL, &gSettings.imuAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr, false, nullptr, "imuautostart" },
  { "imuAddress", SETTING_INT, &gSettings.imuAddress, IMU_ADDRESS_DEFAULT, 0, nullptr, IMU_ADDRESS_MIN, IMU_ADDRESS_MAX, "I2C address (next openimu)", kImuAddressOptions, false, nullptr, "imuaddress" },
#if ENABLE_IMU_LSM6DSOX
  { "imuMagAddress", SETTING_INT, &gSettings.imuMagAddress, IMU_MAG_ADDRESS_DEFAULT, 0, nullptr, IMU_MAG_ADDRESS_MIN, IMU_MAG_ADDRESS_MAX, "Magnetometer I2C address (next openimu)", kImuMagAddressOptions, false, nullptr, "imumagaddress" },
#endif
  { "imuPollingMs", SETTING_INT, &gSettings.imuPollingMs, 200, 0, nullptr, 50, 2000, "Polling (ms)", nullptr, false, "timing", "imupollingms" },
  { "imuEWMAFactor", SETTING_FLOAT, &gSettings.imuEwmaFactor, 0, 0.1f, nullptr, 0, 1, "EWMA Factor", nullptr, false, "timing", "imuewmafactor" },
  { "imuTransitionMs", SETTING_INT, &gSettings.imuTransitionMs, 100, 0, nullptr, 0, 1000, "Transition (ms)", nullptr, false, "timing", "imutransitionms" },
  { "imuWebMaxFps", SETTING_INT, &gSettings.imuWebMaxFps, 15, 0, nullptr, 1, 30, "Web Max FPS", nullptr, false, "timing", "imuwebmaxfps" },
  { "imuDevicePollMs", SETTING_INT, &gSettings.imuDevicePollMs, 200, 0, nullptr, 50, 1000, "Poll Interval (ms)", nullptr, false, "timing", "imudevicepollms" },
  { "imuOrientationMode", SETTING_INT, &gSettings.imuOrientationMode, 0, 0, nullptr, 0, 8, "Orientation Mode", "0:Normal,1:Flip Pitch,2:Flip Roll,3:Flip Yaw,4:Flip Pitch+Roll,5:Roll 180 Fix,6:Rotate 90 CCW,7:Alt Extreme Pitch,8:Upside Down", false, "orientation", "imuorientationmode" },
  { "imuOrientationCorrectionEnabled", SETTING_BOOL, &gSettings.imuOrientationCorrectionEnabled, false, 0, nullptr, 0, 1, "Orientation Correction", nullptr, false, "orientation", "imuorientationcorrection" },
  { "imuPitchOffset", SETTING_FLOAT, &gSettings.imuPitchOffset, 0, 0.0f, nullptr, -180, 180, "Pitch Offset", nullptr, false, "orientation", "imupitchoffset" },
  { "imuRollOffset", SETTING_FLOAT, &gSettings.imuRollOffset, 0, 0.0f, nullptr, -180, 180, "Roll Offset", nullptr, false, "orientation", "imurolloffset" },
  { "imuYawOffset", SETTING_FLOAT, &gSettings.imuYawOffset, 0, 0.0f, nullptr, -180, 180, "Yaw Offset", nullptr, false, "orientation", "imuyawoffset" },
};

static bool isImuConnected() {
  return gImuConnected;
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule imuSettingsModule = {
  "imu",
  "hardware.sensors.imu",
  imuSettingEntries,
  sizeof(imuSettingEntries) / sizeof(imuSettingEntries[0]),
  isImuConnected,
  "IMU (motion sensor)"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================
// IMU OLED Mode (Display Function + Registration)
// ============================================================================
#if DISPLAY_TYPE > 0
#include "HAL_Motion_OLED.h"
#endif

#endif // ENABLE_IMU_SENSOR
