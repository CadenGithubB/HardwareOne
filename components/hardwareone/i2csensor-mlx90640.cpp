#include "i2csensor-mlx90640.h"
#include "System_BuildConfig.h"
#include "System_MemoryMonitor.h"
#include "System_Utils.h"

#if ENABLE_THERMAL_SENSOR

#include <Adafruit_MLX90640.h>
#include <Arduino.h>
#include <Wire.h>

#include "OLED_Display.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_I2C.h"
#include "System_MemUtil.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"

// External dependencies still needed
extern TwoWire Wire1;
// sensorStatusBumpWith, gSensorPollingPaused, i2cMutex, drainDebugRing provided by System_I2C.h

// ============================================================================
// Thermal Settings Module (modular settings registry)
// ============================================================================

// Note: gSettings is declared in settings.h (included above)

static const SettingEntry thermalSettingEntries[] = {
  { "thermalAutoStart",             SETTING_BOOL,   &gSettings.thermalAutoStart,             0,    0, nullptr, 0, 1, "Auto-start after boot", nullptr },
  { "thermalPollingMs",             SETTING_INT,    &gSettings.thermalPollingMs,             250,  0, nullptr, 50, 5000, "Polling (ms)", nullptr },
  { "thermalPaletteDefault",        SETTING_STRING, &gSettings.thermalPaletteDefault,        0,    0, "grayscale", 0, 0, "Default Palette", "grayscale,ironbow,rainbow,hot,cool" },
  { "thermalEWMAFactor",            SETTING_FLOAT,  &gSettings.thermalEWMAFactor,            0,    0.2f, nullptr, 0, 1, "EWMA Factor", nullptr },
  { "thermalTransitionMs",          SETTING_INT,    &gSettings.thermalTransitionMs,          80,   0, nullptr, 0, 5000, "Transition (ms)", nullptr },
  { "thermalWebMaxFps",             SETTING_INT,    &gSettings.thermalWebMaxFps,             10,   0, nullptr, 1, 30, "Web Max FPS", nullptr },
  { "thermalUpscaleFactor",         SETTING_INT,    &gSettings.thermalUpscaleFactor,         1,    0, nullptr, 1, 4, "Upscale Factor", nullptr },
  { "thermalRollingMinMaxEnabled",  SETTING_BOOL,   &gSettings.thermalRollingMinMaxEnabled,  1,    0, nullptr, 0, 1, "Rolling Min/Max", nullptr },
  { "thermalRollingMinMaxAlpha",    SETTING_FLOAT,  &gSettings.thermalRollingMinMaxAlpha,    0,    0.6f, nullptr, 0, 1, "Rolling Alpha", nullptr },
  { "thermalRollingMinMaxGuardC",   SETTING_FLOAT,  &gSettings.thermalRollingMinMaxGuardC,   0,    0.3f, nullptr, 0, 10, "Guard Celsius", nullptr },
  { "thermalInterpolationEnabled",  SETTING_BOOL,   &gSettings.thermalInterpolationEnabled,  1,    0, nullptr, 0, 1, "Interpolation", nullptr },
  { "thermalInterpolationSteps",    SETTING_INT,    &gSettings.thermalInterpolationSteps,    5,    0, nullptr, 1, 8, "Interp. Steps", nullptr },
  { "thermalInterpolationBufferSize", SETTING_INT,  &gSettings.thermalInterpolationBufferSize, 2,  0, nullptr, 1, 10, "Interp. Buffer", nullptr },
  { "thermalTargetFps",             SETTING_INT,    &gSettings.thermalTargetFps,             8,    0, nullptr, 1, 8, "Target FPS", nullptr },
  { "thermalDevicePollMs",          SETTING_INT,    &gSettings.thermalDevicePollMs,          100,  0, nullptr, 50, 1000, "Poll Interval (ms)", nullptr },
  { "thermalTemporalAlpha",         SETTING_FLOAT,  &gSettings.thermalTemporalAlpha,         0,    0.5f, nullptr, 0, 1, "Temporal Alpha", nullptr },
  { "thermalRotation",              SETTING_INT,    &gSettings.thermalRotation,              0,    0, nullptr, 0, 3, "Rotation (0-3)", nullptr },
};

static bool isThermalConnected() {
  return thermalConnected;
}

extern const SettingsModule thermalSettingsModule = {
  "thermal",
  "thermal_mlx90640",
  thermalSettingEntries,
  sizeof(thermalSettingEntries) / sizeof(thermalSettingEntries[0]),
  isThermalConnected,
  "MLX90640 thermal camera settings"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================

// Thermal cache (now defined here, declared in Sensor_Thermal_MLX90640.h)
ThermalCache gThermalCache;

// Thermal sensor state (definitions)
bool thermalEnabled = false;
bool thermalConnected = false;
unsigned long thermalLastStopTime = 0;
TaskHandle_t thermalTaskHandle = nullptr;

// Thermal initialization handoff flags
volatile bool thermalInitRequested = false;
volatile bool thermalInitDone = false;
volatile bool thermalInitResult = false;
volatile uint32_t thermalArmAtMs = 0;

// Thermal watermark tracking
volatile UBaseType_t gThermalWatermarkMin = (UBaseType_t)0xFFFFFFFF;
volatile UBaseType_t gThermalWatermarkNow = (UBaseType_t)0;

// Thermal sensor hardware state
bool mlx90640_initialized = false;
volatile bool thermalPendingFirstFrame = false;
Adafruit_MLX90640* gMLX90640 = nullptr;

// Thermal timing constants
const unsigned long MLX90640_READ_INTERVAL = 62;  // 16 Hz

// Frame buffers for readThermalPixels (file scope for cleanup)
static float* g_tempFrame = nullptr;
static int16_t* g_localFrame = nullptr;

// Settings
extern Settings gSettings;


// Note: lockThermalCache() and unlockThermalCache() are declared in i2c_system.h

// Forward declarations (implementations in main .ino)
extern bool initThermalSensor();
extern bool readThermalPixels();
extern void i2cSetDefaultWire1Clock();

#define MIN_RESTART_DELAY_MS 2000

// Queue system functions now in System_I2C.h

// ============================================================================
// Thermal Sensor Command Handlers
// ============================================================================

// Internal function called by queue processor
bool startThermalSensorInternal() {
  DEBUG_CLIF("[THERMAL_INTERNAL] Starting thermal sensor initialization");
  DEBUG_CLIF("[THERMAL_INTERNAL] Current state: enabled=%d, connected=%d, heap=%lu", 
             thermalEnabled ? 1 : 0, thermalConnected ? 1 : 0,
             (unsigned long)ESP.getFreeHeap());
  
  // Check if too soon after stop (prevent rapid restart crashes)
  if (thermalLastStopTime > 0) {
    unsigned long timeSinceStop = millis() - thermalLastStopTime;
    DEBUG_CLIF("[THERMAL_INTERNAL] Time since last stop: %lu ms (min required: %d ms)", 
               timeSinceStop, MIN_RESTART_DELAY_MS);
    if (timeSinceStop < MIN_RESTART_DELAY_MS) {
      DEBUG_CLIF("[THERMAL_INTERNAL] Too soon after stop - aborting");
      return false;
    }
  }

  // Check memory before creating large thermal task
  if (!checkMemoryAvailable("thermal", nullptr)) {
    DEBUG_CLIF("[THERMAL_INTERNAL] Insufficient memory for thermal sensor");
    return false;
  }
  DEBUG_CLIF("[THERMAL_INTERNAL] Memory check passed: %lu bytes available",
             (unsigned long)ESP.getFreeHeap());

  // Clock is now managed automatically by i2cTaskWithStandardTimeout wrapper
  // No manual clock changes needed - device registration specifies thermal's clock speed
  DEBUG_CLIF("[THERMAL_INTERNAL] I2C clock will be managed by transaction wrapper");

  // Clean up any stale memory from previous run BEFORE starting
  // CRITICAL: Memory wasn't freed during stop to avoid dying-task crashes
  if (lockThermalCache()) {
    if (gThermalCache.thermalFrame) {
      free(gThermalCache.thermalFrame);
      gThermalCache.thermalFrame = nullptr;
    }
    if (gThermalCache.thermalInterpolated) {
      free(gThermalCache.thermalInterpolated);
      gThermalCache.thermalInterpolated = nullptr;
      gThermalCache.thermalInterpolatedWidth = 0;
      gThermalCache.thermalInterpolatedHeight = 0;
    }
    gThermalCache.thermalDataValid = false;
    gThermalCache.thermalSeq = 0;
    unlockThermalCache();
    DEBUG_CLIF("[THERMAL_INTERNAL] Cleaned up stale memory from previous run");
  }

  // Initialize thermal sensor independently (no ToF coordination)
  if (!thermalConnected || gMLX90640 == nullptr) {
    DEBUG_CLIF("[THERMAL_INTERNAL] Sensor not connected - requesting initialization");
    // Defer initialization to thermalTask (larger stack); wait for completion to keep CLI behavior
    thermalInitDone = false;
    thermalInitResult = false;
    thermalInitRequested = true;
  }

  // Enable thermal sensor BEFORE creating task (task checks this flag immediately)
  bool prev = thermalEnabled;
  DEBUG_CLIF("[THERMAL_INTERNAL] Setting thermalEnabled=true (was %d)", prev ? 1 : 0);
  thermalEnabled = true;

  // Create Thermal task lazily (stale handle detection now in createThermalTask)
  if (thermalTaskHandle == nullptr) {
    DEBUG_CLIF("[THERMAL_INTERNAL] Creating thermal task (handle is NULL)");
    if (!createThermalTask()) {
      DEBUG_CLIF("[THERMAL_INTERNAL] FAILED to create Thermal task");
      thermalEnabled = false;  // Restore on failure
      return false;
    }
    DEBUG_CLIF("[THERMAL_INTERNAL] Thermal task created successfully");
  } else {
    DEBUG_CLIF("[THERMAL_INTERNAL] Thermal task already exists (handle=%p)", thermalTaskHandle);
  }
  if (thermalEnabled != prev) {
    sensorStatusBumpWith("openthermal@queue");
  }
  if (thermalEnabled && !prev) {
    thermalPendingFirstFrame = true;
    thermalArmAtMs = millis() + 150;  // small arming delay to let system settle
    DEBUG_CLIF("[THERMAL_INTERNAL] Set pendingFirstFrame=true, armAt=%lu", thermalArmAtMs);
  }

  // If init was requested above, wait for result so caller gets success/fail
  if (thermalInitRequested || !thermalConnected || gMLX90640 == nullptr) {
    DEBUG_CLIF("[THERMAL_INTERNAL] Waiting for sensor initialization (timeout=3000ms)");
    unsigned long start = millis();
    while (!thermalInitDone && (millis() - start) < 3000UL) {
      delay(10);
    }
    unsigned long elapsed = millis() - start;
    DEBUG_CLIF("[THERMAL_INTERNAL] Init wait complete: elapsed=%lu ms, done=%d, result=%d", 
               elapsed, thermalInitDone ? 1 : 0, thermalInitResult ? 1 : 0);
    
    if (!thermalInitDone || !thermalInitResult) {
      // Cleanup flags on failure - no ToF interference, just fail gracefully
      thermalEnabled = false;
      thermalPendingFirstFrame = false;
      thermalArmAtMs = 0;
      DEBUG_CLIF("[THERMAL_INTERNAL] FAILED to initialize MLX90640 thermal sensor");
      return false;
    }
  }
  DEBUG_CLIF("[THERMAL_INTERNAL] SUCCESS: MLX90640 thermal sensor started");
  
  // Broadcast sensor status to ESP-NOW master
#if ENABLE_ESPNOW
  broadcastSensorStatus(REMOTE_SENSOR_THERMAL, true);
#endif
  
  return true;
}

// Public command - uses centralized queue
const char* cmd_thermalstart(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  DEBUG_CLIF("[THERMAL_START] Command called - checking state");
  DEBUG_CLIF("[THERMAL_START] thermalEnabled=%d, heap=%lu",
             thermalEnabled ? 1 : 0, (unsigned long)ESP.getFreeHeap());

  // Check if already enabled or queued
  if (thermalEnabled) {
    DEBUG_CLIF("[THERMAL_START] Already running - returning");
    return "[Thermal] Sensor already running";
  }
  if (isInQueue(I2C_DEVICE_THERMAL)) {
    int pos = getQueuePosition(I2C_DEVICE_THERMAL);
    DEBUG_CLIF("[THERMAL_START] Already in queue at position %d", pos);
    BROADCAST_PRINTF("Thermal sensor already queued (position %d)", pos);
    return "[Thermal] Already queued";
  }

  DEBUG_CLIF("[THERMAL_START] Calling enqueueDeviceStart(I2C_DEVICE_THERMAL=%d)", I2C_DEVICE_THERMAL);
  // Enqueue the request to centralized queue
  if (enqueueDeviceStart(I2C_DEVICE_THERMAL)) {
    DEBUG_CLIF("[THERMAL_START] Successfully enqueued");
    sensorStatusBumpWith("openthermal@enqueue");
    int pos = getQueuePosition(I2C_DEVICE_THERMAL);
    DEBUG_CLIF("[THERMAL_START] Queue position: %d", pos);
    BROADCAST_PRINTF("Thermal sensor queued for open (position %d)", pos);
    return "[Thermal] Sensor queued for open";
  } else {
    DEBUG_CLIF("[THERMAL_START] FAILED to enqueue (queue full)");
    return "[Thermal] Error: Failed to enqueue open (queue full)";
  }
}

const char* cmd_thermalread(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!thermalEnabled || !thermalConnected) {
    return "[Thermal] Not running. Use 'openthermal' to start.";
  }

  if (!gThermalCache.thermalDataValid || !gThermalCache.thermalFrame) {
    return "[Thermal] No data available yet";
  }

  // Compute min/max/avg from cached frame
  float minT = 9999.0f, maxT = -9999.0f, sumT = 0.0f;
  for (int i = 0; i < 768; i++) {
    float t = gThermalCache.thermalFrame[i] / 100.0f;
    if (t < minT) minT = t;
    if (t > maxT) maxT = t;
    sumT += t;
  }
  float avgT = sumT / 768.0f;

  BROADCAST_PRINTF("Thermal: min=%.1f°C max=%.1f°C avg=%.1f°C (seq=%lu)",
                   minT, maxT, avgT, (unsigned long)gThermalCache.thermalSeq);
  return "[Thermal] Reading complete";
}

const char* cmd_thermalstop(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  handleDeviceStopped(I2C_DEVICE_THERMAL);
  return "[Thermal] Stop requested; cleanup will complete asynchronously";
}

const char* cmd_thermalpalettedefault(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: thermalpalettedefault <grayscale|iron|rainbow|hot|coolwarm>";
  while (*p == ' ') p++;  // Skip whitespace
  // Case-insensitive compare for palette names
  if (strncasecmp(p, "grayscale", 9) == 0) {
    setSetting(gSettings.thermalPaletteDefault, "grayscale");
  } else if (strncasecmp(p, "iron", 4) == 0) {
    setSetting(gSettings.thermalPaletteDefault, "iron");
  } else if (strncasecmp(p, "rainbow", 7) == 0) {
    setSetting(gSettings.thermalPaletteDefault, "rainbow");
  } else if (strncasecmp(p, "hot", 3) == 0) {
    setSetting(gSettings.thermalPaletteDefault, "hot");
  } else if (strncasecmp(p, "coolwarm", 8) == 0) {
    setSetting(gSettings.thermalPaletteDefault, "coolwarm");
  } else {
    return "[Thermal] Error: Palette must be grayscale|iron|rainbow|hot|coolwarm";
  }
  BROADCAST_PRINTF("thermalPaletteDefault set to %s", gSettings.thermalPaletteDefault.c_str());
  return "[Thermal] Setting updated";
}

const char* cmd_thermalewmafactor(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: thermalewmafactor <0.0..1.0>";
  while (*p == ' ') p++;  // Skip whitespace
  float f = strtof(p, nullptr);
  if (f < 0.0f || f > 1.0f) return "[Thermal] Error: EWMA factor must be 0.0-1.0";
  setSetting(gSettings.thermalEWMAFactor, f);
  BROADCAST_PRINTF("thermalEWMAFactor set to %.3f", f);
  return "[Thermal] Setting updated";
}

const char* cmd_thermaltransitionms(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: thermaltransitionms <0..5000>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 0 || v > 5000) return "[Thermal] Error: Transition time must be 0-5000ms";
  setSetting(gSettings.thermalTransitionMs, v);
  BROADCAST_PRINTF("thermalTransitionMs set to %d", v);
  return "[Thermal] Setting updated";
}

const char* cmd_thermalupscalefactor(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: thermalupscalefactor <1..4>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 1 || v > 4) return "[Thermal] Error: Upscale factor must be 1-4";
  setSetting(gSettings.thermalUpscaleFactor, v);
  BROADCAST_PRINTF("thermalUpscaleFactor set to %d", v);
  return "[Thermal] Setting updated";
}

const char* cmd_thermalrollingminmaxenabled(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: thermalrollingminmaxenabled <0|1>";
  while (*p == ' ') p++;  // Skip whitespace
  bool enabled = (*p == '1' || strncasecmp(p, "true", 4) == 0);
  setSetting(gSettings.thermalRollingMinMaxEnabled, enabled);
  BROADCAST_PRINTF("thermalRollingMinMaxEnabled set to %s", enabled ? "1" : "0");
  return "[Thermal] Setting updated";
}

const char* cmd_thermalrollingminmaxalpha(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: thermalrollingminmaxalpha <0.0..1.0>";
  while (*p == ' ') p++;  // Skip whitespace
  float f = strtof(p, nullptr);
  if (f < 0.0f || f > 1.0f) return "[Thermal] Error: Rolling min/max alpha must be 0.0-1.0";
  setSetting(gSettings.thermalRollingMinMaxAlpha, f);
  BROADCAST_PRINTF("thermalRollingMinMaxAlpha set to %.3f", f);
  return "[Thermal] Setting updated";
}

const char* cmd_thermalrollingminmaxguardc(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: thermalrollingminmaxguardc <0.0..10.0>";
  while (*p == ' ') p++;  // Skip whitespace
  float f = strtof(p, nullptr);
  if (f < 0.0f || f > 10.0f) return "[Thermal] Error: Rolling min/max guard must be 0.0-10.0°C";
  setSetting(gSettings.thermalRollingMinMaxGuardC, f);
  BROADCAST_PRINTF("thermalRollingMinMaxGuardC set to %.3f", f);
  return "[Thermal] Setting updated";
}

const char* cmd_thermaltemporalalpha(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: thermaltemporalalpha <0.0..1.0>";
  while (*p == ' ') p++;  // Skip whitespace
  float f = strtof(p, nullptr);
  if (f < 0.0f || f > 1.0f) return "[Thermal] Error: Temporal alpha must be 0.0-1.0";
  setSetting(gSettings.thermalTemporalAlpha, f);
  BROADCAST_PRINTF("thermalTemporalAlpha set to %.3f", f);
  return "[Thermal] Setting updated";
}

const char* cmd_thermalrotation(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: thermalrotation <0|1|2|3> (0=0°, 1=90°, 2=180°, 3=270°)";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 0 || v > 3) return "[Thermal] Error: Rotation must be 0-3 (0=0°, 1=90°, 2=180°, 3=270°)";

  int oldRotation = gSettings.thermalRotation;
  setSetting(gSettings.thermalRotation, v);

  const char* rotations[] = { "0°", "90°", "180°", "270°" };
  DEBUG_SENSORSF("[THERMAL_ROTATION] Changed from %d (%s) to %d (%s)",
                 oldRotation, rotations[oldRotation], v, rotations[v]);
  BROADCAST_PRINTF("thermalRotation set to %d (%s) - will apply to next thermal frame", v, rotations[v]);
  return "[Thermal] Setting updated";
}

// ============================================================================
// Thermal Sensor Initialization and Reading Functions
// ============================================================================

bool initThermalSensor() {
  extern void i2cSetDefaultWire1Clock();
  extern bool mlx90640_initialized;
  
  if (gMLX90640 != nullptr) {
    return true;
  }
  
  // Use i2cTransaction wrapper for safe mutex + clock management
  return i2cDeviceTransaction(I2C_ADDR_THERMAL, 100000, 3000, [&]() -> bool {
    // Wire1 is configured centrally with runtime-configurable pins
    i2cSetDefaultWire1Clock();
    
    // Allocate sensor and begin at safe I2C speed
    gMLX90640 = new Adafruit_MLX90640();
    if (!gMLX90640) return false;
    
    if (!gMLX90640->begin(MLX90640_I2CADDR_DEFAULT, &Wire1)) {
      delete gMLX90640;
      gMLX90640 = nullptr;
      return false;
    }
    
    // Configure sensor
    gMLX90640->setMode(MLX90640_CHESS);
    gMLX90640->setResolution(MLX90640_ADC_16BIT);
    int fps = gSettings.thermalTargetFps;
    if (fps < 1) fps = 1;
    if (fps > 8) fps = 8;
    mlx90640_refreshrate_t rate = MLX90640_1_HZ;
    if (fps >= 8) rate = MLX90640_8_HZ;
    else if (fps >= 4) rate = MLX90640_4_HZ;
    else if (fps >= 2) rate = MLX90640_2_HZ;
    else rate = MLX90640_1_HZ;
    gMLX90640->setRefreshRate(rate);
    thermalConnected = true;
    mlx90640_initialized = true;
    
    return true;
  });
}

bool readThermalPixels() {
  extern bool mlx90640_initialized;
  extern bool lockThermalCache(TickType_t timeout);
  extern void unlockThermalCache();
  // interpolateThermalFrame is defined at the bottom of this file
  
  DEBUG_THERMAL_FRAMEF("readThermalPixels() entry");

  if (gMLX90640 == nullptr) {
    DEBUG_THERMAL_FRAMEF("readThermalPixels() exit: sensor null");
    return false;
  }
  if (!thermalEnabled) {
    DEBUG_THERMAL_FRAMEF("readThermalPixels() exit: disabled");
    return false;
  }
  
  // Ensure thermal frame buffer is allocated
  if (!gThermalCache.thermalFrame) {
    if (lockThermalCache(pdMS_TO_TICKS(100))) {
      if (!gThermalCache.thermalFrame) {
        size_t psram_before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        DEBUG_SENSORSF("[THERMAL_MEM] Before frame alloc: PSRAM=%zu, Heap=%zu", psram_before, heap_before);

        gThermalCache.thermalFrame = (int16_t*)ps_alloc(768 * sizeof(int16_t), AllocPref::PreferPSRAM, "cache.thermal");
        if (!gThermalCache.thermalFrame) {
          DEBUG_THERMAL_FRAMEF("readThermalPixels() exit: failed to allocate frame buffer");
          unlockThermalCache();
          return false;
        }

        size_t psram_after = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t heap_after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        DEBUG_SENSORSF("[THERMAL_MEM] After frame alloc (3072 bytes): PSRAM=%zu (-%zu), Heap=%zu (-%zu)",
                       psram_after, psram_before - psram_after,
                       heap_after, heap_before - heap_after);

        DEBUG_THERMAL_FRAMEF("readThermalPixels() allocated thermal frame buffer");

        int upscale = gSettings.thermalUpscaleFactor;
        if (upscale == 2) {
          size_t psram_before_interp = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
          size_t heap_before_interp = heap_caps_get_free_size(MALLOC_CAP_8BIT);
          DEBUG_SENSORSF("[THERMAL_MEM] Before interp alloc (quality=2): PSRAM=%zu, Heap=%zu",
                         psram_before_interp, heap_before_interp);

          gThermalCache.thermalInterpolatedWidth = 64;
          gThermalCache.thermalInterpolatedHeight = 48;
          int interpSize = 64 * 48;
          gThermalCache.thermalInterpolated = (float*)ps_alloc(interpSize * sizeof(float), AllocPref::PreferPSRAM, "cache.thermal.interp");

          if (gThermalCache.thermalInterpolated) {
            size_t psram_after_interp = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
            size_t heap_after_interp = heap_caps_get_free_size(MALLOC_CAP_8BIT);
            DEBUG_SENSORSF("[THERMAL_MEM] After interp alloc (12288 bytes): PSRAM=%zu (-%zu), Heap=%zu (-%zu)",
                           psram_after_interp, psram_before_interp - psram_after_interp,
                           heap_after_interp, heap_before_interp - heap_after_interp);
            DEBUG_THERMAL_FRAMEF("Allocated interpolated buffer: %dx%d (%d pixels, %d bytes)",
                         gThermalCache.thermalInterpolatedWidth, gThermalCache.thermalInterpolatedHeight,
                         interpSize, interpSize * (int)sizeof(float));
          } else {
            DEBUG_THERMAL_FRAMEF("Warning: Failed to allocate interpolated buffer, falling back to 1x");
            gThermalCache.thermalInterpolatedWidth = 0;
            gThermalCache.thermalInterpolatedHeight = 0;
          }
        } else {
          DEBUG_SENSORSF("[THERMAL_MEM] Interpolation disabled (upscale=%d), no additional buffer allocated", upscale);
        }
      }
      unlockThermalCache();
    } else {
      DEBUG_THERMAL_FRAMEF("readThermalPixels() exit: failed to lock cache for allocation");
      return false;
    }
  }
  
  if (thermalArmAtMs) {
    int32_t dt = (int32_t)(millis() - thermalArmAtMs);
    if (dt < 0) {
      DEBUG_THERMAL_FRAMEF("readThermalPixels() exit: arming delay %dms remaining", (int)(-dt));
      return false;
    } else {
      thermalArmAtMs = 0;
      DEBUG_THERMAL_FRAMEF("readThermalPixels() arming delay expired, proceeding");
    }
  }

  // Clock is already managed by i2cDeviceTransaction wrapper - don't set it here

  static uint32_t lastFrameEndMs = 0;
  static float emaFps = 0.0f;
  static uint32_t frameCount = 0;
  uint32_t startTime = millis();

  if (!gMLX90640 || !mlx90640_initialized) {
    DEBUG_THERMAL_FRAMEF("Thermal sensor not properly initialized - skipping frame capture");
    return false;
  }

  // Allocate frame buffers in PSRAM (not on stack) to reduce task stack usage
  if (!g_tempFrame) {
    g_tempFrame = (float*)ps_alloc(768 * sizeof(float), AllocPref::PreferPSRAM, "thermal.temp");
    if (!g_tempFrame) {
      ERROR_SENSORSF("Failed to allocate tempFrame buffer (3KB)");
      return false;
    }
    INFO_SENSORSF("Allocated tempFrame buffer: 3072 bytes in PSRAM");
  }
  
  if (!g_localFrame) {
    g_localFrame = (int16_t*)ps_alloc(768 * sizeof(int16_t), AllocPref::PreferPSRAM, "thermal.local");
    if (!g_localFrame) {
      ERROR_SENSORSF("Failed to allocate localFrame buffer (1.5KB)");
      return false;
    }
    INFO_SENSORSF("Allocated localFrame buffer: 1536 bytes in PSRAM");
  }
  
  // Detailed pre-capture diagnostics
  DEBUG_SENSORSF("[THERMAL_FRAME] Pre-capture: sensor=%p enabled=%d connected=%d polling_paused=%d",
                 (void*)gMLX90640, thermalEnabled ? 1 : 0, thermalConnected ? 1 : 0, 
                 gSensorPollingPaused ? 1 : 0);
  
  int result = gMLX90640->getFrame(g_tempFrame);

  for (int i = 0; i < 768; i++) {
    g_localFrame[i] = (int16_t)(g_tempFrame[i] * 100.0f);
  }

  uint32_t afterCapture = millis();
  uint32_t captureTime = afterCapture - startTime;

  if (result != 0) {
    // Detailed error reporting based on MLX90640 API error codes
    const char* errDesc = "UNKNOWN";
    switch (result) {
      case -1: errDesc = "I2C_READ_FAIL (NACK or timeout)"; break;
      case -2: errDesc = "I2C_WRITE_VERIFY_FAIL"; break;
      case -6: errDesc = "BAD_PIXEL_POSITION"; break;
      case -8: errDesc = "TOO_MANY_RETRIES (dataReady stuck)"; break;
    }
    ERROR_SENSORSF("MLX90640 frame capture failed: error=%d (%s), time=%lums, heap=%lu",
                   result, errDesc, (unsigned long)captureTime, (unsigned long)ESP.getFreeHeap());
    
    // Check I2C health for this device
    I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
    I2CDevice* dev = mgr ? mgr->getDevice(I2C_ADDR_THERMAL) : nullptr;
    if (dev) {
      const I2CDevice::Health& h = dev->getHealth();
      ERROR_SENSORSF("  I2C Health: degraded=%d consec=%d total=%d NACK=%d TIMEOUT=%d",
                     dev->isDegraded() ? 1 : 0, h.consecutiveErrors, h.totalErrors,
                     h.nackCount, h.timeoutCount);
    }
    return false;
  }

  static bool useSpatialDownsampling = true;
  int32_t sumTemp = 0;
  float minTemp = g_localFrame[0] / 100.0f;
  float maxTemp = g_localFrame[0] / 100.0f;
  int hottestX = 0, hottestY = 0;

  if (useSpatialDownsampling) {
    for (int row = 0; row < 24; row += 2) {
      for (int col = 0; col < 32; col += 2) {
        int i = row * 32 + col;
        if (i < 768) {
          int16_t tempCenti = g_localFrame[i];
          float temp = tempCenti / 100.0f;
          sumTemp += (int32_t)tempCenti;

          if (temp < minTemp) minTemp = temp;
          if (temp > maxTemp) {
            maxTemp = temp;
            hottestX = col;
            hottestY = row;
          }
        }
      }
    }
    sumTemp = sumTemp * 4;
  } else {
    for (int i = 0; i < 768; i++) {
      float temp = g_localFrame[i] / 100.0f;
      sumTemp += (int32_t)g_localFrame[i];

      if (temp < minTemp) minTemp = temp;
      if (temp > maxTemp) {
        maxTemp = temp;
        hottestX = i % 32;
        hottestY = i / 32;
      }
    }
  }

  int32_t avgTempInt = sumTemp / 768;
  float avgTemp = avgTempInt / 100.0f;

  static int16_t* previousFrame = nullptr;
  static bool previousFrameValid = false;
  
  if (gSettings.thermalTemporalAlpha > 0.0f) {
    if (!previousFrame) {
      previousFrame = (int16_t*)ps_alloc(768 * sizeof(int16_t), AllocPref::PreferPSRAM, "thermal.prev");
      if (!previousFrame) {
        ERROR_SENSORSF("Failed to allocate previousFrame buffer");
      } else {
        INFO_SENSORSF("Allocated temporal smoothing buffer: 1536 bytes");
      }
    }
    
    if (previousFrame && previousFrameValid) {
      float alpha = gSettings.thermalTemporalAlpha;
      for (int i = 0; i < 768; i++) {
        float currentTemp = g_localFrame[i] / 100.0f;
        float filtered = alpha * (previousFrame[i] / 100.0f) + (1.0f - alpha) * currentTemp;
        g_localFrame[i] = (int16_t)(filtered * 100.0f);
      }
    }
  }

  float variance = 0.0;
  for (int i = 0; i < 768; i++) {
    float temp = g_localFrame[i] / 100.0f;
    float deviation = abs(temp - avgTemp);
    variance += deviation * deviation;
  }
  float stdDev = sqrt(variance / 768.0);

  float outlierThreshold = 1.5 * stdDev;
  float filteredMin = avgTemp + 50.0;
  float filteredMax = avgTemp - 50.0;
  float filteredSum = 0.0;
  int validPixels = 0;

  for (int i = 0; i < 768; i++) {
    float temp = g_localFrame[i] / 100.0f;
    float deviation = abs(temp - avgTemp);

    if (deviation <= outlierThreshold) {
      if (temp < filteredMin) filteredMin = temp;
      if (temp > filteredMax) {
        filteredMax = temp;
        hottestX = i % 32;
        hottestY = i / 32;
      }
      filteredSum += temp;
      validPixels++;
    } else {
      int x = i % 32;
      int y = i / 32;
      float localSum = 0.0;
      int localCount = 0;

      for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
          if (dx == 0 && dy == 0) continue;
          int nx = x + dx;
          int ny = y + dy;
          if (nx >= 0 && nx < 32 && ny >= 0 && ny < 24) {
            int neighborIdx = ny * 32 + nx;
            float neighborTemp = g_localFrame[neighborIdx] / 100.0f;
            if (abs(neighborTemp - avgTemp) <= outlierThreshold) {
              localSum += neighborTemp;
              localCount++;
            }
          }
        }
      }

      if (localCount > 0) {
        g_localFrame[i] = (int16_t)((localSum / localCount) * 100.0f);
      } else {
        g_localFrame[i] = (int16_t)(avgTemp * 100.0f);
      }
    }
  }

  if (validPixels > 600) {
    minTemp = filteredMin;
    maxTemp = filteredMax;
    avgTemp = filteredSum / validPixels;
  }

  static float rollingMin = 0.0f;
  static float rollingMax = 0.0f;
  static bool rollingInitialized = false;

  if (gSettings.thermalRollingMinMaxEnabled) {
    if (!rollingInitialized) {
      rollingMin = minTemp;
      rollingMax = maxTemp;
      rollingInitialized = true;
      DEBUG_THERMAL_FRAMEF("[Thermal] Rolling min/max initialized: min=%.2f, max=%.2f", rollingMin, rollingMax);
    } else {
      float alpha = gSettings.thermalRollingMinMaxAlpha;
      float guard = gSettings.thermalRollingMinMaxGuardC;

      float proposedMin = alpha * rollingMin + (1.0f - alpha) * minTemp;
      float proposedMax = alpha * rollingMax + (1.0f - alpha) * maxTemp;

      if (fabs(proposedMin - rollingMin) >= guard) {
        rollingMin = proposedMin;
      }
      if (fabs(proposedMax - rollingMax) >= guard) {
        rollingMax = proposedMax;
      }

      if (rollingMin < minTemp - 5.0f) {
        rollingMin = minTemp - 5.0f;
      }
      if (rollingMax > maxTemp + 5.0f) {
        rollingMax = maxTemp + 5.0f;
      }
    }

    minTemp = rollingMin;
    maxTemp = rollingMax;
  } else {
    rollingInitialized = false;
  }

  if (lockThermalCache(pdMS_TO_TICKS(50))) {
    memcpy(gThermalCache.thermalFrame, g_localFrame, 768 * sizeof(int16_t));
    
    if (previousFrame) {
      memcpy(previousFrame, g_localFrame, 768 * sizeof(int16_t));
      previousFrameValid = true;
    }

    gThermalCache.thermalMinTemp = minTemp;
    gThermalCache.thermalMaxTemp = maxTemp;
    gThermalCache.thermalAvgTemp = avgTemp;
    gThermalCache.thermalLastUpdate = millis();
    gThermalCache.thermalDataValid = true;
    gThermalCache.thermalSeq++;

    if (gThermalCache.thermalInterpolated && gThermalCache.thermalInterpolatedWidth > 0) {
      size_t psram_before_op = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      size_t heap_before_op = heap_caps_get_free_size(MALLOC_CAP_8BIT);

      uint32_t interpStart = millis();
      static float* floatFrame = nullptr;
      if (!floatFrame) {
        floatFrame = (float*)ps_alloc(768 * sizeof(float), AllocPref::PreferPSRAM, "thermal.float");
        if (!floatFrame) {
          ERROR_SENSORSF("Failed to allocate floatFrame buffer");
          unlockThermalCache();
          return true;
        }
      }
      for (int i = 0; i < 768; i++) {
        floatFrame[i] = g_localFrame[i] / 100.0f;
      }
      interpolateThermalFrame(floatFrame, gThermalCache.thermalInterpolated,
                              gThermalCache.thermalInterpolatedWidth, gThermalCache.thermalInterpolatedHeight);
      uint32_t interpTime = millis() - interpStart;

      if (isDebugFlagSet(DEBUG_THERMAL_DATA)) {
        size_t psram_after_op = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t heap_after_op = heap_caps_get_free_size(MALLOC_CAP_8BIT);

        DEBUG_SENSORSF("[THERMAL_MEM] Interpolation runtime: %lums, PSRAM delta=%d, Heap delta=%d",
                       (unsigned long)interpTime,
                       (int)(psram_before_op - psram_after_op),
                       (int)(heap_before_op - heap_after_op));
      }

      DEBUG_THERMAL_FRAMEF("Interpolation completed in %lums (%dx%d -> %dx%d)",
                   (unsigned long)interpTime, 32, 24,
                   gThermalCache.thermalInterpolatedWidth, gThermalCache.thermalInterpolatedHeight);
    }

    if (gSettings.thermalRotation != 0) {
      static int16_t* rotatedFrame = nullptr;
      if (!rotatedFrame) {
        rotatedFrame = (int16_t*)ps_alloc(768 * sizeof(int16_t), AllocPref::PreferPSRAM, "thermal.rotate");
        if (!rotatedFrame) {
          ERROR_SENSORSF("Failed to allocate rotation buffer, skipping rotation");
          unlockThermalCache();
          return true;
        }
        DEBUG_SENSORSF("[THERMAL_MEM] Allocated rotation buffer: 1536 bytes");
      }
      
      const int width = 32;
      const int height = 24;
      
      DEBUG_SENSORSF("[ROTATION_DEBUG] Frame capture: applying rotation=%d, seq=%lu", 
                     gSettings.thermalRotation,
                     (unsigned long)gThermalCache.thermalSeq);
      DEBUG_SENSORSF("[THERMAL_ROTATION] Applying rotation=%d (after all processing)", gSettings.thermalRotation);
      DEBUG_SENSORSF("[THERMAL_ROTATION] Before: TL=%d TR=%d BL=%d BR=%d",
                     gThermalCache.thermalFrame[0],
                     gThermalCache.thermalFrame[width - 1],
                     gThermalCache.thermalFrame[(height - 1) * width],
                     gThermalCache.thermalFrame[(height - 1) * width + width - 1]);
      
      if (gSettings.thermalRotation == 1) {
        DEBUG_SENSORSF("[THERMAL_ROTATION] Performing 90° clockwise rotation");
        for (int y = 0; y < height; y++) {
          for (int x = 0; x < width; x++) {
            int srcIdx = y * width + x;
            int newX = y;
            int newY = width - 1 - x;
            int dstIdx = newY * height + newX;
            rotatedFrame[dstIdx] = gThermalCache.thermalFrame[srcIdx];
          }
        }
      } else if (gSettings.thermalRotation == 2) {
        DEBUG_SENSORSF("[THERMAL_ROTATION] Performing 180° rotation");
        for (int y = 0; y < height; y++) {
          for (int x = 0; x < width; x++) {
            int srcIdx = y * width + x;
            int dstIdx = (height - 1 - y) * width + (width - 1 - x);
            rotatedFrame[dstIdx] = gThermalCache.thermalFrame[srcIdx];
          }
        }
      } else if (gSettings.thermalRotation == 3) {
        DEBUG_SENSORSF("[THERMAL_ROTATION] Performing 270° clockwise rotation");
        for (int y = 0; y < height; y++) {
          for (int x = 0; x < width; x++) {
            int srcIdx = y * width + x;
            int newX = height - 1 - y;
            int newY = x;
            int dstIdx = newY * height + newX;
            rotatedFrame[dstIdx] = gThermalCache.thermalFrame[srcIdx];
          }
        }
      }

      memcpy(gThermalCache.thermalFrame, rotatedFrame, 768 * sizeof(int16_t));

      DEBUG_SENSORSF("[THERMAL_ROTATION] After:  TL=%d TR=%d BL=%d BR=%d",
                     gThermalCache.thermalFrame[0],
                     gThermalCache.thermalFrame[width - 1],
                     gThermalCache.thermalFrame[(height - 1) * width],
                     gThermalCache.thermalFrame[(height - 1) * width + width - 1]);
    }

    unlockThermalCache();

    if (thermalPendingFirstFrame) {
      thermalPendingFirstFrame = false;
      sensorStatusBumpWith("thermal-ready");
    }
  } else {
    DEBUG_THERMAL_FRAMEF("Failed to lock thermal cache for thermal update - skipping");
    return false;
  }

  uint32_t endTime = millis();
  uint32_t processingTime = endTime - afterCapture;
  uint32_t totalTime = endTime - startTime;
  float instFps = 0.0f;
  if (lastFrameEndMs != 0) {
    uint32_t interFrame = endTime - lastFrameEndMs;
    if (interFrame > 0) instFps = 1000.0f / (float)interFrame;
  }
  if (emaFps == 0.0f && instFps > 0.0f) emaFps = instFps;
  else emaFps = 0.3f * instFps + 0.7f * emaFps;
  lastFrameEndMs = endTime;
  frameCount++;

  int tfps = gSettings.thermalTargetFps;
  if (tfps < 1) tfps = 1;
  if (tfps > 8) tfps = 8;
  int effFps = (tfps >= 8) ? 8 : (tfps >= 4) ? 4 : (tfps >= 2) ? 2 : 1;

  static uint32_t dbgCounter = 0;

  if (isDebugFlagSet(DEBUG_THERMAL_FRAME) && ((dbgCounter++ % 10) == 0)) {
    DEBUG_THERMAL_FRAMEF("THERM frame: cap=%lums, proc=%lums, total=%lums, fps_i=%.2f, fps_ema=%.2f, i2cHz=%lu, tgtFps=%d(eff=%d), heap=%lu",
                 (unsigned long)captureTime, (unsigned long)processingTime,
                 (unsigned long)totalTime, instFps, emaFps,
                 (unsigned long)gSettings.i2cClockThermalHz,
                 gSettings.thermalTargetFps, effFps,
                 (unsigned long)ESP.getFreeHeap());
  }

  if (isDebugFlagSet(DEBUG_THERMAL_FRAME) && ((dbgCounter++ % 10) == 0)) {
    String msg = String("THERM frame: cap=") + captureTime + "ms, proc=" + processingTime + "ms, total=" + totalTime + "ms, fps_i=" + String(instFps, 2) + ", fps_ema=" + String(emaFps, 2) + ", i2cHz=" + String(gSettings.i2cClockThermalHz) + ", tgtFps=" + String(gSettings.thermalTargetFps) + "(eff=" + String(effFps) + ")" + ", heap=" + String(ESP.getFreeHeap());
    broadcastOutput(msg);
  }

  return true;
}

// Helper function to reset frame buffers during thermal task cleanup
void resetThermalFrameBuffers() {
  INFO_SENSORSF("[Thermal] Freeing frame buffers to prevent heap corruption on restart");
  
  if (g_tempFrame) {
    free(g_tempFrame);
    g_tempFrame = nullptr;
  }
  
  if (g_localFrame) {
    free(g_localFrame);
    g_localFrame = nullptr;
  }
}

// ============================================================================
// Thermal Frame Streaming (HTTP Response)
// ============================================================================

int buildThermalDataJSON(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;
  
  unsigned long startMs = millis();
  int pos = 0;

  if (lockThermalCache(pdMS_TO_TICKS(100))) {  // 100ms timeout for HTTP response
    // Determine which frame to send based on interpolation availability
    bool useInterpolated = (gThermalCache.thermalInterpolated != nullptr && gThermalCache.thermalInterpolatedWidth > 0 && gThermalCache.thermalInterpolatedHeight > 0);

    float* frame = useInterpolated ? gThermalCache.thermalInterpolated : nullptr;
    // For raw frame, swap dimensions if rotation is 90° or 270° (output is 24x32 instead of 32x24)
    int width = useInterpolated ? gThermalCache.thermalInterpolatedWidth : ((gSettings.thermalRotation == 1 || gSettings.thermalRotation == 3) ? 24 : 32);
    int height = useInterpolated ? gThermalCache.thermalInterpolatedHeight : ((gSettings.thermalRotation == 1 || gSettings.thermalRotation == 3) ? 32 : 24);
    int frameSize = useInterpolated ? (width * height) : 768;
    
    // Debug: Track rotation value when JSON dimensions are determined
    DEBUG_SENSORSF("[ROTATION_DEBUG] JSON generation: rotation=%d, w=%d, h=%d, seq=%lu",
                   gSettings.thermalRotation, width, height,
                   (unsigned long)gThermalCache.thermalSeq);
    DEBUG_SENSORSF("[RACE_CONDITION_DEBUG] Reading thermalFrame WITH lock held (seq=%lu)",
                   (unsigned long)gThermalCache.thermalSeq);

    // Header
    pos = snprintf(buf, bufSize,
                   "{\"val\":%d,\"seq\":%lu,\"mn\":%.1f,\"mx\":%.1f,\"w\":%d,\"h\":%d,\"data\":[",
                   gThermalCache.thermalDataValid ? 1 : 0,
                   (unsigned long)gThermalCache.thermalSeq,
                   gThermalCache.thermalMinTemp,
                   gThermalCache.thermalMaxTemp,
                   width, height);
    if (pos < 0 || (size_t)pos >= bufSize) {
      unlockThermalCache();
      return 0;
    }

    // Frame data - interpolated uses whole degrees, raw uses centidegrees
    if (useInterpolated && frame) {
      // Interpolated frame is float - send as whole degree integers
      for (int i = 0; i < frameSize; i++) {
        int wholeDegrees = (int)frame[i];
        int written = snprintf(buf + pos, bufSize - pos, "%d%s", wholeDegrees, (i < frameSize - 1) ? "," : "");
        if (written < 0 || (size_t)written >= (bufSize - pos)) {
          unlockThermalCache();
          return 0;
        }
        pos += written;
      }
    } else if (gThermalCache.thermalFrame) {
      // Raw frame is int16_t centidegrees (frontend divides by 100)
      for (int i = 0; i < frameSize; i++) {
        int written = snprintf(buf + pos, bufSize - pos, "%d%s", gThermalCache.thermalFrame[i], (i < frameSize - 1) ? "," : "");
        if (written < 0 || (size_t)written >= (bufSize - pos)) {
          unlockThermalCache();
          return 0;
        }
        pos += written;
      }
    } else {
      // Frame buffer was freed (sensor stopped) - return empty array
      unlockThermalCache();
      pos = snprintf(buf, bufSize, "{\"val\":0,\"error\":\"Sensor stopped\"}");
      return (pos > 0) ? pos : 0;
    }

    // Footer
    int tail = snprintf(buf + pos, bufSize - pos, "]}");
    if (tail < 0 || (size_t)tail >= (bufSize - pos)) {
      unlockThermalCache();
      return 0;
    }
    pos += tail;

    unlockThermalCache();

    unsigned long elapsedMs = millis() - startMs;
    DEBUG_PERFORMANCEF("buildThermalDataJSON: %lu ms, %d bytes, %d pixels",
                       elapsedMs, pos, frameSize);
  } else {
    // Timeout - return error response (non-zero so caller can respond)
    pos = snprintf(buf, bufSize, "{\"error\":\"Sensor data temporarily unavailable\"}");
    if (pos < 0) pos = 0;
  }

  return pos;
}

// ============================================================================
// Thermal Interpolation (moved from .ino to fix Arduino preprocessor issues)
// ============================================================================

// Bilinear interpolation for thermal upscaling
// Upscales 32x24 source to targetWidth x targetHeight
void interpolateThermalFrame(const float* src, float* dst, int targetWidth, int targetHeight) {
  const int srcWidth = 32;
  const int srcHeight = 24;

  for (int y = 0; y < targetHeight; y++) {
    for (int x = 0; x < targetWidth; x++) {
      // Map output coordinates to source coordinates
      float srcX = (float)x * (srcWidth - 1) / (targetWidth - 1);
      float srcY = (float)y * (srcHeight - 1) / (targetHeight - 1);

      // Get the four neighboring source pixels
      int x0 = (int)srcX;
      int y0 = (int)srcY;
      int x1 = min(x0 + 1, srcWidth - 1);
      int y1 = min(y0 + 1, srcHeight - 1);

      // Calculate interpolation weights
      float fx = srcX - x0;
      float fy = srcY - y0;

      // Get source pixel values
      float v00 = src[y0 * srcWidth + x0];
      float v10 = src[y0 * srcWidth + x1];
      float v01 = src[y1 * srcWidth + x0];
      float v11 = src[y1 * srcWidth + x1];

      // Bilinear interpolation
      float v0 = v00 * (1.0f - fx) + v10 * fx;
      float v1 = v01 * (1.0f - fx) + v11 * fx;
      float value = v0 * (1.0f - fy) + v1 * fy;

      dst[y * targetWidth + x] = value;
    }
  }
}

// ============================================================================
// Thermal tuning commands (migrated from .ino)
// ============================================================================

const char* cmd_thermalpollingms(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: thermalpollingms <50..5000>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 50 || v > 5000) return "[Thermal] Error: Polling interval must be 50-5000ms";
  setSetting(gSettings.thermalPollingMs, v);
  BROADCAST_PRINTF("thermalPollingMs set to %d", v);
  return "[Thermal] Setting updated";
}

const char* cmd_thermalinterpolationenabled(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: thermalinterpolationenabled <0|1>";
  while (*p == ' ') p++;  // Skip whitespace
  // Accept 0, 1, true, false (case insensitive)
  bool enabled = (*p == '1' || strncasecmp(p, "true", 4) == 0);
  setSetting(gSettings.thermalInterpolationEnabled, enabled);
  BROADCAST_PRINTF("thermalInterpolationEnabled set to %s", enabled ? "1" : "0");
  return "[Thermal] Setting updated";
}

const char* cmd_thermalinterpolationsteps(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: thermalinterpolationsteps <1..8>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 1 || v > 8) return "[Thermal] Error: Interpolation steps must be 1-8";
  setSetting(gSettings.thermalInterpolationSteps, v);
  BROADCAST_PRINTF("thermalInterpolationSteps set to %d", v);
  return "[Thermal] Setting updated";
}

const char* cmd_thermalinterpolationbuffersize(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const char* p = strchr(cmd.c_str(), ' ');
  if (!p) return "Usage: thermalinterpolationbuffersize <1..10>";
  while (*p == ' ') p++;  // Skip whitespace
  int v = atoi(p);
  if (v < 1 || v > 10) return "[Thermal] Error: Interpolation buffer size must be 1-10";
  setSetting(gSettings.thermalInterpolationBufferSize, v);
  BROADCAST_PRINTF("thermalInterpolationBufferSize set to %d", v);
  return "[Thermal] Setting updated";
}

const char* cmd_thermaldiag(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  char* buf = getDebugBuffer();
  int remaining = 1024;
  int n = 0;
  
  n = snprintf(buf, remaining, "=== THERMAL SENSOR DIAGNOSTICS ===\n");
  buf += n; remaining -= n;
  
  // Current state
  n = snprintf(buf, remaining, "State: enabled=%d connected=%d sensor=%p\n",
               thermalEnabled ? 1 : 0, thermalConnected ? 1 : 0, (void*)gMLX90640);
  buf += n; remaining -= n;
  
  n = snprintf(buf, remaining, "Task: handle=%p\n", (void*)thermalTaskHandle);
  buf += n; remaining -= n;
  
  // Check I2C device health
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  I2CDevice* dev = mgr ? mgr->getDevice(I2C_ADDR_THERMAL) : nullptr;
  
  if (dev) {
    const I2CDevice::Health& h = dev->getHealth();
    n = snprintf(buf, remaining, "I2C Health: addr=0x%02X degraded=%d consec=%d total=%d\n",
                 dev->address, dev->isDegraded() ? 1 : 0, h.consecutiveErrors, h.totalErrors);
    buf += n; remaining -= n;
    
    n = snprintf(buf, remaining, "  NACK=%d TIMEOUT=%d BUS_ERR=%d\n",
                 h.nackCount, h.timeoutCount, h.busErrorCount);
    buf += n; remaining -= n;
  } else {
    n = snprintf(buf, remaining, "I2C Health: Device 0x%02X not registered\n", I2C_ADDR_THERMAL);
    buf += n; remaining -= n;
  }
  
  // Test I2C clock speeds if sensor is stopped
  if (thermalEnabled) {
    n = snprintf(buf, remaining, "\nSensor running - stop first to test I2C speeds\n");
    buf += n; remaining -= n;
  } else {
    n = snprintf(buf, remaining, "\nTesting I2C probe at different clock speeds...\n");
    buf += n; remaining -= n;
    
    // Clock speeds to test (from slow to fast)
    uint32_t testClocks[] = {100000, 400000, 800000, 1000000};
    const char* clockNames[] = {"100kHz", "400kHz", "800kHz", "1MHz"};
    
    for (int i = 0; i < 4 && remaining > 100; i++) {
      // Clear degraded status before each test
      if (dev) {
        dev->attemptRecovery();  // Clears degraded flag and consecutive errors
        DEBUG_SENSORSF("[THERMAL_DIAG] Cleared degraded status before %s test", clockNames[i]);
      }
      
      // Probe the device with mutex protection
      uint8_t result = i2cProbeAddress(I2C_ADDR_THERMAL, testClocks[i], 200);
      
      const char* resultStr = "?";
      switch (result) {
        case 0: resultStr = "OK"; break;
        case 2: resultStr = "NACK"; break;
        case 3: resultStr = "TIMEOUT"; break;
        case 4: resultStr = "BUS_ERR"; break;
        default: resultStr = "UNKNOWN"; break;
      }
      
      n = snprintf(buf, remaining, "  %s: %s (err=%d)\n", clockNames[i], resultStr, result);
      buf += n; remaining -= n;
      
      DEBUG_SENSORSF("[THERMAL_DIAG] %s probe result: %s (err=%d)", clockNames[i], resultStr, result);
      
      delay(100);  // Gap between tests
    }
    
    // Clock automatically restored by transaction wrapper
    
    n = snprintf(buf, remaining, "\nI2C clock restored to 100kHz\n");
    buf += n; remaining -= n;
  }
  
  // Memory status
  n = snprintf(buf, remaining, "\nMemory: heap=%lu min=%lu psram=%lu\n",
               (unsigned long)ESP.getFreeHeap(),
               (unsigned long)ESP.getMinFreeHeap(),
               (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  buf += n; remaining -= n;
  
  return getDebugBuffer();
}

// ============================================================================
// Thermal Command Registry
// ============================================================================

// External device-level command handlers (defined in i2c_system.cpp)
extern const char* cmd_thermaltargetfps(const String& cmd);
extern const char* cmd_thermaldevicepollms(const String& cmd);

const char* cmd_thermalautostart(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = args; arg.trim();
  if (arg.length() == 0) {
    return gSettings.thermalAutoStart ? "[Thermal] Auto-start: enabled" : "[Thermal] Auto-start: disabled";
  }
  arg.toLowerCase();
  if (arg == "on" || arg == "true" || arg == "1") {
    setSetting(gSettings.thermalAutoStart, true);
    return "[Thermal] Auto-start enabled";
  } else if (arg == "off" || arg == "false" || arg == "0") {
    setSetting(gSettings.thermalAutoStart, false);
    return "[Thermal] Auto-start disabled";
  }
  return "Usage: thermalautostart [on|off]";
}

const CommandEntry thermalCommands[] = {
  // Start/Stop (3-level voice: "sensor" -> "thermal camera" -> "open/close")
  { "openthermal", "Start MLX90640 thermal sensor.", false, cmd_thermalstart, nullptr, "sensor", "thermal camera", "open" },
  { "closethermal", "Stop MLX90640 thermal sensor.", false, cmd_thermalstop, nullptr, "sensor", "thermal camera", "close" },
  { "thermalread", "Read thermal sensor data (min/max/avg).", false, cmd_thermalread },
  
  // UI Settings (client-side visualization)
  { "thermalpollingms", "Thermal UI polling: <50..5000>", true, cmd_thermalpollingms, "Usage: thermalpollingms <50..5000>" },
  { "thermalpalettedefault", "Thermal palette: <grayscale|iron|rainbow|hot|coolwarm>", true, cmd_thermalpalettedefault, "Usage: thermalpalettedefault <grayscale|iron|rainbow|hot|coolwarm>" },
  { "thermalewmafactor", "Thermal EWMA factor: <0.0..1.0>", true, cmd_thermalewmafactor, "Usage: thermalewmafactor <0.0..1.0>" },
  { "thermaltransitionms", "Thermal transition time: <0..5000>", true, cmd_thermaltransitionms, "Usage: thermaltransitionms <0..5000>" },
  { "thermalupscalefactor", "Thermal upscale factor: <1..4>", true, cmd_thermalupscalefactor, "Usage: thermalupscalefactor <1..4>" },
  { "thermalrollingminmaxenabled", "Thermal rolling min/max: <0|1>", true, cmd_thermalrollingminmaxenabled, "Usage: thermalrollingminmaxenabled <0|1>" },
  { "thermalrollingminmaxalpha", "Thermal rolling alpha: <0.0..1.0>", true, cmd_thermalrollingminmaxalpha, "Usage: thermalrollingminmaxalpha <0.0..1.0>" },
  { "thermalrollingminmaxguardc", "Thermal rolling guard: <0.0..10.0>", true, cmd_thermalrollingminmaxguardc, "Usage: thermalrollingminmaxguardc <0.0..10.0>" },
  { "thermaltemporalalpha", "Thermal temporal alpha: <0.0..1.0>", true, cmd_thermaltemporalalpha, "Usage: thermaltemporalalpha <0.0..1.0>" },
  { "thermalrotation", "Thermal rotation: <0|1|2|3>", true, cmd_thermalrotation, "Usage: thermalrotation <0|1|2|3> (0=0°, 1=90°, 2=180°, 3=270°)" },
  
  // Interpolation settings
  { "thermalinterpolationenabled", "Thermal interpolation: <0|1>", true, cmd_thermalinterpolationenabled, "Usage: thermalinterpolationenabled <0|1>" },
  { "thermalinterpolationsteps", "Thermal interp steps: <1..8>", true, cmd_thermalinterpolationsteps, "Usage: thermalinterpolationsteps <1..8>" },
  { "thermalinterpolationbuffersize", "Thermal interp buffer: <1..10>", true, cmd_thermalinterpolationbuffersize, "Usage: thermalinterpolationbuffersize <1..10>" },
  
  // Device-level settings (sensor hardware behavior)
  { "thermaltargetfps", "Thermal target FPS: <1..8>", true, cmd_thermaltargetfps, "Usage: thermalTargetFps <1..8>" },
  { "thermaldevicepollms", "Thermal device poll: <100..2000>", true, cmd_thermaldevicepollms, "Usage: thermalDevicePollMs <100..2000>" },
  
  // Diagnostics
  { "thermaldiag", "Run thermal sensor diagnostics.", false, cmd_thermaldiag },
  
  // Auto-start
  { "thermalautostart", "Enable/disable thermal auto-start after boot [on|off]", false, cmd_thermalautostart, "Usage: thermalautostart [on|off]" },
};

const size_t thermalCommandsCount = sizeof(thermalCommands) / sizeof(thermalCommands[0]);

// ============================================================================
// Command Registration
// ============================================================================
static CommandModuleRegistrar _thermal_registrar(thermalCommands, thermalCommandsCount, "thermal");

// ============================================================================
// Thermal Task Implementation (moved from i2c_system.cpp for full modularization)
// ============================================================================

// ============================================================================
// Thermal Task - FreeRTOS Task Function
// ============================================================================
// Purpose: Continuously reads 32x24 thermal frame data from MLX90640 sensor
// Stack: 4096 words (~16KB) | Priority: 1 | Core: Any
// Lifecycle: Created by cmd_thermalstart, deleted when thermalEnabled=false
// Polling: Configurable via thermalDevicePollMs (default 100ms) | I2C Clock: 100-1000kHz
//
// Cleanup Strategy:
//   1. Check thermalEnabled flag at loop start
//   2. Acquire i2cMutex to prevent race conditions during cleanup
//   3. Delete sensor object and invalidate cache
//   4. Release mutex and delete task
// ============================================================================

void thermalTask(void* parameter) {
  INFO_SENSORSF("[Thermal] Task started (handle=%p, stack=%u words)", 
                (void*)xTaskGetCurrentTaskHandle(), 
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  INFO_SENSORSF("[MODULAR] thermalTask() running from Sensor_Thermal_MLX90640.cpp");
  unsigned long lastThermalRead = 0;
  unsigned long lastStackLog = 0;
  while (true) {
    // CRITICAL: Check enabled flag FIRST before any debug calls or operations
    // This allows graceful shutdown when thermalstop is called
    if (!thermalEnabled) {
      thermalConnected = false;
      if (gMLX90640 != nullptr) {
        delete gMLX90640;
        gMLX90640 = nullptr;
      }
      gThermalCache.thermalDataValid = false;
      gThermalCache.thermalSeq = 0;
      
      // Free static buffers in readThermalPixels to prevent heap corruption on restart
      extern void resetThermalFrameBuffers();
      resetThermalFrameBuffers();
      
      INFO_SENSORSF("[THERMAL] Task disabled - cleaning up and deleting");
      // NOTE: Do NOT clear thermalTaskHandle here - let start function use eTaskGetState()
      // to detect stale handles. Clearing here creates a race condition window.
      vTaskDelete(nullptr);
    }
    
    // Update watermark diagnostics (only when enabled)
    if (isDebugFlagSet(DEBUG_PERFORMANCE)) {
      UBaseType_t wm = uxTaskGetStackHighWaterMark(NULL);
      gThermalWatermarkNow = wm;
      if (wm < gThermalWatermarkMin) gThermalWatermarkMin = wm;
    }
    unsigned long nowLog = millis();
    if (nowLog - lastStackLog >= 5000UL) {
      lastStackLog = nowLog;
      if (checkTaskStackSafety("thermal", THERMAL_STACK_WORDS, &thermalEnabled)) break;
      // CRITICAL: Check enabled flag again before debug output (prevent crash during shutdown)
      if (thermalEnabled) {
        DEBUG_PERFORMANCEF("[STACK] thermal_task watermark_now=%u min=%u words", (unsigned)gThermalWatermarkNow, (unsigned)gThermalWatermarkMin);
        DEBUG_MEMORYF("[HEAP] thermal_task: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }
    // Handle deferred initialization request
    if (thermalEnabled && (!thermalConnected || gMLX90640 == nullptr)) {
      if (thermalInitRequested) {
        bool ok = initThermalSensor();
        thermalInitResult = ok;
        thermalInitDone = true;
        thermalInitRequested = false;
      }
    }

    if (thermalEnabled && thermalConnected && gMLX90640 != nullptr && !gSensorPollingPaused) {
      unsigned long nowMs = millis();
      unsigned long pollMs = (gSettings.thermalDevicePollMs > 0) ? (unsigned long)gSettings.thermalDevicePollMs : 100;
      bool ready = true;
      if (thermalArmAtMs) {
        int32_t dt = (int32_t)(nowMs - thermalArmAtMs);
        if (dt < 0) ready = false;
      }
      if (ready && (nowMs - lastThermalRead) >= pollMs) {
        uint32_t thermalHz = (gSettings.i2cClockThermalHz > 0) ? (uint32_t)gSettings.i2cClockThermalHz : 800000;
        bool ok = false;
        
        // Thermal frame read takes 400-800ms at 100kHz (768 pixels); needs generous timeout
        ok = i2cTaskWithTimeout(I2C_ADDR_THERMAL, thermalHz, 1500, [&]() -> bool {
          return readThermalPixels();
        });
        
        lastThermalRead = millis();
        
        // Auto-disable if too many consecutive failures (like gamepad does)
        if (!ok) {
          if (i2cShouldAutoDisable(I2C_ADDR_THERMAL, 5)) {
            ERROR_SENSORSF("Too many consecutive thermal failures - auto-disabling");
            thermalEnabled = false;
            sensorStatusBumpWith("thermal@auto_disabled");
            // Task will clean up and delete itself on next loop iteration
          }
        }
        
        // SAFE: Debug output AFTER i2cTransaction completes, with enabled check
        if (thermalEnabled && thermalPendingFirstFrame && ok) {
          thermalPendingFirstFrame = false;
          DEBUG_SENSORSF("Thermal first frame captured");
        }
        
        // Stream data to ESP-NOW master if enabled (worker devices only)
#if ENABLE_ESPNOW
        // Check mesh mode (worker role) OR bond mode (worker role)
        bool shouldStream = false;
        if (meshEnabled() && gSettings.meshRole != MESH_ROLE_MASTER) {
          shouldStream = true;
        }
        if (gSettings.bondModeEnabled && gSettings.bondRole == 0) {
          shouldStream = true;  // Bond mode worker
        }
        
        if (ok && shouldStream) {
          // Use integer-optimized thermal data for remote streaming
          char thermalJson[4096];  // Large buffer for thermal data
          int jsonLen = buildThermalDataJSONInteger(thermalJson, sizeof(thermalJson));
          if (jsonLen > 0) {
            sendSensorDataUpdate(REMOTE_SENSOR_THERMAL, String(thermalJson));
          }
        }
#endif
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

// ============================================================================
// Additional Thermal Device Commands
// ============================================================================

const char* cmd_thermaltargetfps(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) return "Usage: thermalTargetFps <1..8>";
  int v = valStr.toInt();
  if (v < 1) v = 1;
  if (v > 8) v = 8;
  setSetting(gSettings.thermalTargetFps, v);
  snprintf(getDebugBuffer(), 1024, "thermalTargetFps set to %d", v);
  return getDebugBuffer();
}

const char* cmd_thermaldevicepollms(const String& args) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = args;
  valStr.trim();
  if (valStr.length() == 0) return "Usage: thermalDevicePollMs <100..2000>";
  int v = valStr.toInt();
  if (v < 100) v = 100;
  if (v > 2000) v = 2000;
  setSetting(gSettings.thermalDevicePollMs, v);
  snprintf(getDebugBuffer(), 1024, "thermalDevicePollMs set to %d", v);
  return getDebugBuffer();
}

// ============================================================================
// Thermal Diagnostic Command - Moved above command array
// ============================================================================

// ============================================================================
// Thermal OLED Mode (Display Function + Registration)
// ============================================================================
#if DISPLAY_TYPE > 0
#include "i2csensor-mlx90640-oled.h"
#endif

#endif // ENABLE_THERMAL_SENSOR
