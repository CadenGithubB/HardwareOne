#include "i2csensor_mlx90640.h"
#include "System_Events.h"  // systemEventPost — event register producer
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
#include <memory>
#include <new>                 // placement-new: gMLX90640 lives in PSRAM (ps_alloc)
#include "System_Settings.h"
#include "System_TaskUtils.h"

// External dependencies still needed
// sensorStatusBumpWith, gSensorPollingPaused, drainDebugRing provided by System_I2C.h

// ============================================================================
// Thermal Settings Module (modular settings registry)
// ============================================================================

// Note: gSettings is declared in settings.h (included above)

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry thermalSettingEntries[] = {
  { "thermalEnabled", SETTING_BOOL, &gSettings.thermalEnabled, 1, 0, nullptr, 0, 1, "Enabled", nullptr, false, nullptr, "thermalenabled" },
  { "thermalAutoStart", SETTING_BOOL, &gSettings.thermalAutoStart, 0, 0, nullptr, 0, 1, "Auto-start after boot", nullptr, false, nullptr, "thermalautostart" },
  { "thermalPollingMs", SETTING_INT, &gSettings.thermalPollingMs, 250, 0, nullptr, 50, 5000, "Polling (ms)", nullptr, false, "timing", "thermalpollingms" },
  { "thermalPaletteDefault", SETTING_STRING, &gSettings.thermalPaletteDefault, 0, 0, "grayscale", 0, 0, "Default Palette", "grayscale,iron,rainbow,hot,coolwarm", false, "display", "thermalpalettedefault" },
  { "thermalEWMAFactor", SETTING_FLOAT, &gSettings.thermalEWMAFactor, 0, 0.2f, nullptr, 0, 1, "EWMA Factor", nullptr, false, "filtering", "thermalewmafactor" },
  { "thermalTransitionMs", SETTING_INT, &gSettings.thermalTransitionMs, 80, 0, nullptr, 0, 5000, "Transition (ms)", nullptr, false, "timing", "thermaltransitionms" },
  { "thermalWebMaxFps", SETTING_INT, &gSettings.thermalWebMaxFps, 10, 0, nullptr, 1, 30, "Web Max FPS", nullptr, false, "timing", "thermalwebmaxfps" },
  { "thermalUpscaleFactor", SETTING_INT, &gSettings.thermalUpscaleFactor, 1, 0, nullptr, 1, 4, "Upscale Factor", nullptr, false, "display", "thermalupscalefactor" },
  { "thermalRollingMinMaxEnabled", SETTING_BOOL, &gSettings.thermalRollingMinMaxEnabled, 1, 0, nullptr, 0, 1, "Rolling Min/Max", nullptr, false, "scale", "thermalrollingminmaxenabled" },
  { "thermalRollingMinMaxAlpha", SETTING_FLOAT, &gSettings.thermalRollingMinMaxAlpha, 0, 0.6f, nullptr, 0, 1, "Rolling Alpha", nullptr, false, "scale", "thermalrollingminmaxalpha" },
  { "thermalRollingMinMaxGuardC", SETTING_FLOAT, &gSettings.thermalRollingMinMaxGuardC, 0, 0.3f, nullptr, 0, 10, "Guard Celsius", nullptr, false, "scale", "thermalrollingminmaxguardc" },
  { "thermalInterpolationEnabled", SETTING_BOOL, &gSettings.thermalInterpolationEnabled, 1, 0, nullptr, 0, 1, "Interpolation", nullptr, false, "interpolation", "thermalinterpolationenabled" },
  { "thermalInterpolationSteps", SETTING_INT, &gSettings.thermalInterpolationSteps, 5, 0, nullptr, 1, 8, "Interp. Steps", nullptr, false, "interpolation", "thermalinterpolationsteps" },
  { "thermalInterpolationBufferSize", SETTING_INT, &gSettings.thermalInterpolationBufferSize, 2, 0, nullptr, 1, 10, "Interp. Buffer", nullptr, false, "interpolation", "thermalinterpolationbuffersize" },
  { "thermalTargetFps", SETTING_INT, &gSettings.thermalTargetFps, 8, 0, nullptr, 1, 8, "Target FPS", nullptr, false, "timing", "thermaltargetfps" },
  { "thermalDevicePollMs", SETTING_INT, &gSettings.thermalDevicePollMs, 100, 0, nullptr, 100, 2000, "Poll Interval (ms)", nullptr, false, "timing", "thermaldevicepollms" },
  { "thermalTemporalAlpha", SETTING_FLOAT, &gSettings.thermalTemporalAlpha, 0, 0.5f, nullptr, 0, 1, "Temporal Alpha", nullptr, false, "filtering", "thermaltemporalalpha" },
  { "thermalRotation", SETTING_INT, &gSettings.thermalRotation, 0, 0, nullptr, 0, 3, "Rotation", "0|0°,1|90°,2|180°,3|270°", false, "display", "thermalrotation" },
  { "thermalI2cClockHz", SETTING_INT, &gSettings.i2cClockThermalHz, 400000, 0, nullptr, 100000, 1000000, "I2C Clock (Hz)", nullptr, false, nullptr, "thermali2cclockhz" },
};

static bool isThermalConnected() {
  return gThermalConnected;
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule thermalSettingsModule = {
  "thermal",
  "hardware.sensors.thermal",
  thermalSettingEntries,
  sizeof(thermalSettingEntries) / sizeof(thermalSettingEntries[0]),
  isThermalConnected,
  "MLX90640 32x24 thermal camera"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================

// Readers copy data; only this implementation owns the frame buffers and lock.
struct ThermalCacheState : ThermalCache {
  int16_t* thermalFrame = nullptr;
  float* thermalInterpolated = nullptr;
  int thermalInterpolatedWidth = 0;
  int thermalInterpolatedHeight = 0;
};
static ThermalCacheState sThermalCache;
static SemaphoreHandle_t sThermalCacheMutex = nullptr;

// Existing writer sections use the same private mutex as the copy operations.
static bool lockThermalCache(TickType_t timeout = pdMS_TO_TICKS(100)) {
  return sThermalCacheMutex && xSemaphoreTake(sThermalCacheMutex, timeout) == pdTRUE;
}
static void unlockThermalCache() { xSemaphoreGive(sThermalCacheMutex); }

static void thermalCopyMetadata(ThermalCache& out, bool preferInterpolated) {
  out = sThermalCache;
  out.thermalFrameInterpolated = preferInterpolated && sThermalCache.thermalInterpolated &&
      sThermalCache.thermalInterpolatedWidth > 0 && sThermalCache.thermalInterpolatedHeight > 0;
  out.thermalFrameAvailable = out.thermalFrameInterpolated || sThermalCache.thermalFrame;
  const bool sideways = gSettings.thermalRotation == 1 || gSettings.thermalRotation == 3;
  out.thermalFrameWidth = out.thermalFrameInterpolated ? sThermalCache.thermalInterpolatedWidth : (sideways ? 24 : 32);
  out.thermalFrameHeight = out.thermalFrameInterpolated ? sThermalCache.thermalInterpolatedHeight : (sideways ? 32 : 24);
}

bool thermalCacheSnapshot(ThermalCache& out, uint32_t timeoutMs) {
  out = {};
  SensorCacheGuard guard(sThermalCacheMutex, pdMS_TO_TICKS(timeoutMs), "thermal.snapshot");
  if (!guard.held) return false;
  thermalCopyMetadata(out, false);
  return true;
}

bool thermalFrameSnapshot(ThermalCache& out, int16_t* values, size_t capacity,
                          bool preferInterpolated, uint32_t timeoutMs) {
  out = {};
  if (!values || capacity < THERMAL_RAW_FRAME_PIXELS) return false;
  SensorCacheGuard guard(sThermalCacheMutex, pdMS_TO_TICKS(timeoutMs), "thermal.frameSnapshot");
  if (!guard.held) return false;
  ThermalCache metadata;
  thermalCopyMetadata(metadata, preferInterpolated);
  const size_t count = static_cast<size_t>(metadata.thermalFrameWidth) * metadata.thermalFrameHeight;
  if (count > THERMAL_MAX_FRAME_PIXELS || count > capacity) return false;
  if (metadata.thermalFrameInterpolated) {
    for (size_t i = 0; i < count; ++i) values[i] = static_cast<int16_t>(sThermalCache.thermalInterpolated[i]);
  } else if (metadata.thermalFrameAvailable) {
    memcpy(values, sThermalCache.thermalFrame, count * sizeof(*values));
  }
  out = metadata;
  return true;
}

size_t thermalCacheStorageBytes() { return sizeof(sThermalCache) + sizeof(sThermalCacheMutex); }

using ThermalFrameBuffer = std::unique_ptr<int16_t, decltype(&ps_free)>;
static ThermalFrameBuffer thermalSnapshotBuffer(size_t count) {
  return ThermalFrameBuffer(static_cast<int16_t*>(ps_alloc(count * sizeof(int16_t),
                            AllocPref::PreferPSRAM, "thermal.snapshot")), ps_free);
}

// Runs on the queue worker before the task exists: retires the previous run's
// buffers (a stop leaves them for the next start) and allocates this run's, so
// thermalPoll never allocates or takes the cache lock inside its transaction.
// False (lock or memory) fails the start.
static bool thermalResetCacheForStart() {
  int16_t* oldFrame;
  float* oldInterpolated;
  {
    SensorCacheGuard guard(sThermalCacheMutex, pdMS_TO_TICKS(100), "thermal.reset");
    if (!guard.held) return false;
    oldFrame = sThermalCache.thermalFrame;
    oldInterpolated = sThermalCache.thermalInterpolated;
    sThermalCache.thermalFrame = nullptr;
    sThermalCache.thermalInterpolated = nullptr;
    sThermalCache.thermalInterpolatedWidth = 0;
    sThermalCache.thermalInterpolatedHeight = 0;
    sThermalCache.thermalDataValid = false;
    sThermalCache.thermalSeq = 0;
  }
  free(oldFrame);
  free(oldInterpolated);

  // Zeroed: until the first frame, readers see an invalid all-zero frame (the
  // same shape as after a stop), never uninitialized memory.
  int16_t* frame = static_cast<int16_t*>(
      ps_alloc(768 * sizeof(int16_t), AllocPref::PreferPSRAM, "cache.thermal"));
  if (!frame) return false;
  memset(frame, 0, 768 * sizeof(int16_t));
  float* interpolated = nullptr;
  if (gSettings.thermalUpscaleFactor == 2) {
    interpolated = static_cast<float*>(
        ps_alloc(64 * 48 * sizeof(float), AllocPref::PreferPSRAM, "cache.thermal.interp"));
    if (interpolated) memset(interpolated, 0, 64 * 48 * sizeof(float));
    else DEBUG_THERMAL_LIFECYCLEF("[THERMAL_MEM] Interpolated buffer unavailable; running at 1x");
  }
  SensorCacheGuard guard(sThermalCacheMutex, pdMS_TO_TICKS(100), "thermal.install");
  if (!guard.held) {
    free(frame);
    free(interpolated);
    return false;
  }
  sThermalCache.thermalFrame = frame;
  sThermalCache.thermalInterpolated = interpolated;
  sThermalCache.thermalInterpolatedWidth = interpolated ? 64 : 0;
  sThermalCache.thermalInterpolatedHeight = interpolated ? 48 : 0;
  DEBUG_THERMAL_LIFECYCLEF("[THERMAL_MEM] Frame buffer allocated (3072 B)%s",
                           interpolated ? " + 64x48 interpolation (12288 B)" : "");
  return true;
}

static bool thermalInvalidateCache() {
  SensorCacheGuard guard(sThermalCacheMutex, pdMS_TO_TICKS(50), "thermal.invalidate");
  if (!guard.held) return false;
  sThermalCache.thermalDataValid = false;
  sThermalCache.thermalSeq = 0;
  return true;
}

// Thermal sensor state (definitions)
bool gThermalRunning = false;
bool gThermalConnected = false;
unsigned long gThermalLastStopTime = 0;
TaskHandle_t gThermalTaskHandle = nullptr;

// Thermal initialization handoff flags
volatile bool gThermalInitRequested = false;
volatile bool gThermalInitDone = false;
volatile bool gThermalInitResult = false;
volatile uint32_t gThermalArmAtMs = 0;

// Thermal watermark tracking
volatile UBaseType_t gThermalWatermarkMin = (UBaseType_t)0xFFFFFFFF;
volatile UBaseType_t gThermalWatermarkNow = (UBaseType_t)0;

// Thermal sensor hardware state
bool gMlx90640Initialized = false;
volatile bool gThermalPendingFirstFrame = false;
static Adafruit_MLX90640* gMLX90640 = nullptr;

// Thermal timing constants
const unsigned long MLX90640_READ_INTERVAL = 62;  // 16 Hz

// Frame buffers for thermalPoll (file scope for cleanup)
static float* g_tempFrame = nullptr;
static int16_t* g_localFrame = nullptr;

// Settings



// Forward declarations (implementations in main .ino)
extern bool thermalInit();
extern bool thermalPoll();

// MIN_RESTART_DELAY_MS defined in System_I2C.h

// Queue system functions now in System_I2C.h

// ============================================================================
// Thermal Sensor Command Handlers
// ============================================================================

// Internal function called by queue processor
bool thermalStartInternal() {
  if (gThermalRunning) return true;
  // A stopped task owns its resources until cleanup publishes a null handle.
  if (gThermalTaskHandle != nullptr) return false;

  DEBUG_CLIF("[THERMAL_INTERNAL] Starting thermal sensor initialization");
  DEBUG_CLIF("[THERMAL_INTERNAL] Current state: enabled=%d, connected=%d, heap=%lu", 
             gThermalRunning ? 1 : 0, gThermalConnected ? 1 : 0,
             (unsigned long)ESP.getFreeHeap());
  
  // Check if too soon after stop (prevent rapid restart crashes)
  if (gThermalLastStopTime > 0) {
    unsigned long timeSinceStop = millis() - gThermalLastStopTime;
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

  // Create cache mutex if not already created
  if (!sThermalCacheMutex) {
    sThermalCacheMutex = xSemaphoreCreateMutex();
    if (!sThermalCacheMutex) {
      ERROR_THERMALF("Failed to create cache mutex");
      return false;
    }
    DEBUG_THERMAL_LIFECYCLEF("[THERMAL] Cache mutex created");
  }

  // Clock is now managed automatically by i2cTaskWithStandardTimeout wrapper
  // No manual clock changes needed - device registration specifies thermal's clock speed
  DEBUG_CLIF("[THERMAL_INTERNAL] I2C clock will be managed by transaction wrapper");

  // Clean up any stale memory from previous run BEFORE starting
  // CRITICAL: Memory wasn't freed during stop to avoid dying-task crashes
  if (!thermalResetCacheForStart()) {
    ERROR_THERMALF("Could not prepare thermal cache before start (lock or memory)");
    return false;
  }

  // Initialize thermal sensor independently (no ToF coordination)
  if (!gThermalConnected || gMLX90640 == nullptr) {
    DEBUG_CLIF("[THERMAL_INTERNAL] Sensor not connected - requesting initialization");
    // Defer initialization to thermalTask (larger stack); wait for completion to keep CLI behavior
    gThermalInitDone = false;
    gThermalInitResult = false;
    gThermalInitRequested = true;
  }

  // Enable thermal sensor BEFORE creating task (task checks this flag immediately)
  bool prev = gThermalRunning;
  DEBUG_CLIF("[THERMAL_INTERNAL] Setting gThermalRunning=true (was %d)", prev ? 1 : 0);
  gThermalRunning = true;

  // Create Thermal task lazily; its exit clears the handle after cleanup.
  if (gThermalTaskHandle == nullptr) {
    DEBUG_CLIF("[THERMAL_INTERNAL] Creating thermal task (handle is NULL)");
    if (!createThermalTask()) {
      DEBUG_CLIF("[THERMAL_INTERNAL] FAILED to create Thermal task");
      gThermalRunning = false;  // Restore on failure
      gThermalConnected = false;
      gThermalInitRequested = false;
      gThermalInitDone = false;
      gThermalInitResult = false;
      gThermalPendingFirstFrame = false;
      gThermalArmAtMs = 0;
      ps_delete(gMLX90640);
      gMLX90640 = nullptr;
      return false;
    }
    DEBUG_CLIF("[THERMAL_INTERNAL] Thermal task created successfully");
  } else {
    DEBUG_CLIF("[THERMAL_INTERNAL] Thermal task already exists (handle=%p)", gThermalTaskHandle);
  }
  if (gThermalRunning != prev) {
    sensorStatusBumpWith("openthermal@queue");
  }
  if (gThermalRunning && !prev) {
    gThermalPendingFirstFrame = true;
    gThermalArmAtMs = millis() + 150;  // small arming delay to let system settle
    DEBUG_CLIF("[THERMAL_INTERNAL] Set pendingFirstFrame=true, armAt=%lu", gThermalArmAtMs);
  }

  // If init was requested above, wait for result so caller gets success/fail
  if (gThermalInitRequested || !gThermalConnected || gMLX90640 == nullptr) {
    DEBUG_CLIF("[THERMAL_INTERNAL] Waiting for sensor initialization (timeout=3000ms)");
    unsigned long start = millis();
    while (gThermalRunning && !gThermalInitDone && (millis() - start) < 3000UL) {
      delay(10);
    }
    unsigned long elapsed = millis() - start;
    DEBUG_CLIF("[THERMAL_INTERNAL] Init wait complete: elapsed=%lu ms, done=%d, result=%d", 
               elapsed, gThermalInitDone ? 1 : 0, gThermalInitResult ? 1 : 0);
    
    if (!gThermalRunning || !gThermalInitDone || !gThermalInitResult) {
      // Cleanup flags on failure - no ToF interference, just fail gracefully
      gThermalRunning = false;
      gThermalPendingFirstFrame = false;
      gThermalArmAtMs = 0;
      DEBUG_CLIF("[THERMAL_INTERNAL] FAILED to initialize MLX90640 thermal sensor");
      return false;
    }
  }
  DEBUG_CLIF("[THERMAL_INTERNAL] SUCCESS: MLX90640 thermal sensor started");
  
  
  return true;
}

// Public command - uses centralized queue
const char* cmd_thermalstart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gSettings.thermalEnabled) {
    return "Error: Thermal camera is disabled - run 'thermalenabled 1' first";
  }
  return cmd_sensorstart_queued(I2C_DEVICE_THERMAL, "Thermal", "openthermal@enqueue");
}

// Compact SUMMARY reading on the shared sensor envelope: the raw min/avg/max
// scene temperature (recomputed from an independent copy of the raw frame).
// The full 768-pixel frame stays on its own channel (thermalBuildDataJSON /
// the ESP-NOW frame) — this is the lightweight "what's the thermal scene" read.
// NB: uses the RAW frame min/max, not the rolling/smoothed display-scale values.
static bool thermalReadRawSummary(ThermalCache& reading, float& minT, float& maxT, float& avgT) {
  minT = maxT = avgT = 0.0f;
  auto frame = thermalSnapshotBuffer(THERMAL_RAW_FRAME_PIXELS);
  if (!frame || !thermalFrameSnapshot(reading, frame.get(), THERMAL_RAW_FRAME_PIXELS, false, 50)) {
    reading = {};
    return false;
  }
  if (reading.thermalDataValid && reading.thermalFrameAvailable) {
    minT = 9999.0f; maxT = -9999.0f;
    float sumT = 0.0f;
    for (size_t i = 0; i < THERMAL_RAW_FRAME_PIXELS; ++i) {
      float t = frame.get()[i] / 100.0f;
      if (t < minT) minT = t;
      if (t > maxT) maxT = t;
      sumT += t;
    }
    avgT = sumT / THERMAL_RAW_FRAME_PIXELS;
  }
  return true;
}

int thermalBuildSummaryJSON(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;
  ThermalCache reading;
  float minT, maxT, avgT;
  if (!thermalReadRawSummary(reading, minT, maxT, avgT)) {
    int pos = sensorEnvelopeBegin(buf, bufSize, false, gThermalConnected, 0);
    if (pos == 0) return 0;
    int n = snprintf(buf + pos, bufSize - pos, ",\"min\":0,\"avg\":0,\"max\":0}");
    return n < 0 || static_cast<size_t>(n) >= bufSize - pos ? 0 : pos + n;
  }
  const bool ready = reading.thermalDataValid && reading.thermalFrameAvailable;
  int pos = sensorEnvelopeBegin(buf, bufSize, ready, gThermalConnected, reading.thermalLastUpdate);
  if (pos == 0) return 0;
  int n = snprintf(buf + pos, bufSize - pos,
                   ",\"min\":%.1f,\"avg\":%.1f,\"max\":%.1f}", minT, avgT, maxT);
  return n < 0 || static_cast<size_t>(n) >= bufSize - pos ? 0 : pos + n;
}

const char* cmd_thermalread(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const bool wantJson = argWantsJson(argsInput);

  if (!gThermalRunning || !gThermalConnected) {
    // Envelope-shaped (was `{"valid":false,"enabled":false,"hint":...}` — `enabled`
    // is a discovery-layer key, not an envelope key, and this was the only reading
    // reply in the tree carrying it). The hint is kept: it is the actionable half.
    return wantJson ? "{\"valid\":false,\"connected\":false,\"ts\":0,\"hint\":\"the sensor is not open - run 'openthermal' first\"}"
                    : "Error: [Thermal] Not running. Use 'openthermal' to start.";
  }

  // JSON path: the shared summary envelope (frame stays on its own channel).
  if (wantJson) {
    // ensureDebugBuffer() first — getDebugBuffer() returns gDebugBuffer, which is
    // nullptr until something allocates it. The nine sibling handlers all guard;
    // this one did not, so an alloc failure silently degraded to the generic
    // "unavailable" reply instead of reporting the buffer failure.
    if (!ensureDebugBuffer()) return SENSOR_JSON_NOBUF;
    int n = thermalBuildSummaryJSON(getDebugBuffer(), 1024);
    return (n > 0) ? getDebugBuffer() : SENSOR_JSON_UNAVAILABLE;
  }

  ThermalCache reading;
  float minT, maxT, avgT;
  if (!thermalReadRawSummary(reading, minT, maxT, avgT) ||
      !reading.thermalDataValid || !reading.thermalFrameAvailable) {
    return "[Thermal] No data available yet";
  }

  BROADCAST_PRINTF("Thermal: min=%.1f°C max=%.1f°C avg=%.1f°C (seq=%lu)",
                   minT, maxT, avgT, (unsigned long)reading.thermalSeq);
  return "[Thermal] Reading complete";
}

const char* cmd_thermalstop(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!requestSensorStop(I2C_DEVICE_THERMAL)) return "Error: [Thermal] Could not queue stop";
  return "[Thermal] Stop requested; cleanup will complete asynchronously";
}

const char* cmd_thermalpalettedefault(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: thermalpalettedefault <grayscale|iron|rainbow|hot|coolwarm>";
  const char* p = _arg.c_str();
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
    return "Error: [Thermal] Palette must be grayscale|iron|rainbow|hot|coolwarm";
  }
  BROADCAST_PRINTF("thermalPaletteDefault set to %s", gSettings.thermalPaletteDefault.c_str());
  return "[Thermal] Setting updated";
}

const char* cmd_thermalewmafactor(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: thermalewmafactor <0.0..1.0>";
  float f = strtof(_arg.c_str(), nullptr);
  if (f < 0.0f || f > 1.0f) return "Error: [Thermal] EWMA factor must be 0.0-1.0";
  setSetting(gSettings.thermalEWMAFactor, f);
  BROADCAST_PRINTF("thermalEWMAFactor set to %.3f", f);
  return "[Thermal] Setting updated";
}

const char* cmd_thermaltransitionms(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: thermaltransitionms <0..5000>";
  int v = _arg.toInt();
  if (v < 0 || v > 5000) return "Error: [Thermal] Transition time must be 0-5000ms";
  setSetting(gSettings.thermalTransitionMs, v);
  BROADCAST_PRINTF("thermalTransitionMs set to %d", v);
  return "[Thermal] Setting updated";
}

const char* cmd_thermalwebmaxfps(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: thermalwebmaxfps <1..30>";
  int v = _arg.toInt();
  if (v < 1 || v > 30) return "Error: [Thermal] Web max FPS must be 1-30";
  setSetting(gSettings.thermalWebMaxFps, v);
  BROADCAST_PRINTF("thermalWebMaxFps set to %d", v);
  return "[Thermal] Setting updated";
}

const char* cmd_thermali2cclockhz(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: thermali2cclockhz <100000..1000000>";
  int v = _arg.toInt();
  if (v < 100000 || v > 1000000) return "Error: [Thermal] I2C clock must be 100000-1000000 Hz";
  setSetting(gSettings.i2cClockThermalHz, v);
  BROADCAST_PRINTF("thermalI2cClockHz set to %d", v);
  return "[Thermal] Setting updated";
}

const char* cmd_thermalupscalefactor(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: thermalupscalefactor <1..4>";
  int v = _arg.toInt();
  if (v < 1 || v > 4) return "Error: [Thermal] Upscale factor must be 1-4";
  setSetting(gSettings.thermalUpscaleFactor, v);
  BROADCAST_PRINTF("thermalUpscaleFactor set to %d", v);
  return "[Thermal] Setting updated";
}

const char* cmd_thermalrollingminmaxenabled(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: thermalrollingminmaxenabled <0|1>";
  const char* p = _arg.c_str();
  bool enabled = (*p == '1' || strncasecmp(p, "true", 4) == 0);
  setSetting(gSettings.thermalRollingMinMaxEnabled, enabled);
  BROADCAST_PRINTF("thermalRollingMinMaxEnabled set to %s", enabled ? "1" : "0");
  return "[Thermal] Setting updated";
}

const char* cmd_thermalrollingminmaxalpha(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: thermalrollingminmaxalpha <0.0..1.0>";
  float f = strtof(_arg.c_str(), nullptr);
  if (f < 0.0f || f > 1.0f) return "Error: [Thermal] Rolling min/max alpha must be 0.0-1.0";
  setSetting(gSettings.thermalRollingMinMaxAlpha, f);
  BROADCAST_PRINTF("thermalRollingMinMaxAlpha set to %.3f", f);
  return "[Thermal] Setting updated";
}

const char* cmd_thermalrollingminmaxguardc(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: thermalrollingminmaxguardc <0.0..10.0>";
  float f = strtof(_arg.c_str(), nullptr);
  if (f < 0.0f || f > 10.0f) return "Error: [Thermal] Rolling min/max guard must be 0.0-10.0°C";
  setSetting(gSettings.thermalRollingMinMaxGuardC, f);
  BROADCAST_PRINTF("thermalRollingMinMaxGuardC set to %.3f", f);
  return "[Thermal] Setting updated";
}

const char* cmd_thermaltemporalalpha(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: thermaltemporalalpha <0.0..1.0>";
  float f = strtof(_arg.c_str(), nullptr);
  if (f < 0.0f || f > 1.0f) return "Error: [Thermal] Temporal alpha must be 0.0-1.0";
  setSetting(gSettings.thermalTemporalAlpha, f);
  BROADCAST_PRINTF("thermalTemporalAlpha set to %.3f", f);
  return "[Thermal] Setting updated";
}

const char* cmd_thermalrotation(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: thermalrotation <0|1|2|3> (0=0°, 1=90°, 2=180°, 3=270°)";
  int v = _arg.toInt();
  if (v < 0 || v > 3) return "Error: [Thermal] Rotation must be 0-3 (0=0°, 1=90°, 2=180°, 3=270°)";

  int oldRotation = gSettings.thermalRotation;
  setSetting(gSettings.thermalRotation, v);

  const char* rotations[] = { "0°", "90°", "180°", "270°" };
  DEBUG_THERMAL_LIFECYCLEF("[THERMAL_ROTATION] Changed from %d (%s) to %d (%s)",
                 oldRotation, rotations[oldRotation], v, rotations[v]);
  BROADCAST_PRINTF("thermalRotation set to %d (%s) - will apply to next thermal frame", v, rotations[v]);
  return "[Thermal] Setting updated";
}

// ============================================================================
// Thermal Sensor Initialization and Reading Functions
// ============================================================================

bool thermalInit() {
  extern bool gMlx90640Initialized;
  
  if (gMLX90640 != nullptr) {
    return true;
  }
  
  const uint8_t thermalBus = (uint8_t)gSettings.thermalBus;
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  TwoWire* thermalWire = mgr ? mgr->getWire(thermalBus) : nullptr;
  if (!thermalWire) return false;

  // Use the same configured bus for the manager lock and the library object.
  return i2cDeviceTransaction(thermalBus, I2C_ADDR_THERMAL, 100000, 3000, [&]() -> bool {
    
    // Allocate the sensor object in PSRAM via placement-new. Its ~4.7 KB is an
    // inline paramsMLX90640 calibration struct (alpha/offset/kta/kv[768]), so the
    // whole shell moves off scarce internal DRAM. MUST be torn down with ps_delete
    // (both sites below) — a plain `delete` would free a ps_alloc block via the
    // wrong allocator.
    void* mlxBuf = ps_alloc(sizeof(Adafruit_MLX90640), AllocPref::PreferPSRAM, "thermal.obj");
    if (!mlxBuf) return false;
    gMLX90640 = new (mlxBuf) Adafruit_MLX90640();

    if (!gMLX90640->begin(MLX90640_I2CADDR_DEFAULT, thermalWire)) {
      ps_delete(gMLX90640);
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
    gThermalConnected = true;
    gMlx90640Initialized = true;
    
    return true;
  });
}

// Over-temp hotspot latch. Hysteresis (fire >50C / clear <45C) plus reset on
// task start keeps a lingering hotspot from re-posting every poll.
static bool gThermalHot = false;

bool thermalPoll() {
  extern bool gMlx90640Initialized;
  // interpolateThermalFrame is defined at the bottom of this file
  
  DEBUG_THERMAL_POLLINGF("thermalPoll() entry");

  if (gMLX90640 == nullptr) {
    DEBUG_THERMAL_POLLINGF("thermalPoll() exit: sensor null");
    return false;
  }
  if (!gThermalRunning) {
    DEBUG_THERMAL_POLLINGF("thermalPoll() exit: disabled");
    return false;
  }
  
  // thermalResetCacheForStart() allocated this run's buffers before the task
  // existed and nothing replaces them while it runs, so no lock is taken here.
  if (!sThermalCache.thermalFrame) {
    DEBUG_THERMAL_POLLINGF("thermalPoll() exit: frame buffer missing");
    return false;
  }
  
  if (gThermalArmAtMs) {
    int32_t dt = (int32_t)(millis() - gThermalArmAtMs);
    if (dt < 0) {
      DEBUG_THERMAL_POLLINGF("thermalPoll() exit: arming delay %dms remaining", (int)(-dt));
      return false;
    } else {
      gThermalArmAtMs = 0;
      DEBUG_THERMAL_POLLINGF("thermalPoll() arming delay expired, proceeding");
    }
  }

  // Clock is already managed by i2cDeviceTransaction wrapper - don't set it here

  static uint32_t lastFrameEndMs = 0;
  static float emaFps = 0.0f;
  static uint32_t frameCount = 0;
  uint32_t startTime = millis();

  if (!gMLX90640 || !gMlx90640Initialized) {
    DEBUG_THERMAL_POLLINGF("Thermal sensor not properly initialized - skipping frame capture");
    return false;
  }

  // Allocate frame buffers in PSRAM (not on stack) to reduce task stack usage
  if (!g_tempFrame) {
    g_tempFrame = (float*)ps_alloc(768 * sizeof(float), AllocPref::PreferPSRAM, "thermal.temp");
    if (!g_tempFrame) {
      ERROR_THERMALF("Failed to allocate tempFrame buffer (3KB)");
      return false;
    }
    INFO_THERMAL_LIFECYCLEF("Allocated tempFrame buffer: 3072 bytes in PSRAM");
  }
  
  if (!g_localFrame) {
    g_localFrame = (int16_t*)ps_alloc(768 * sizeof(int16_t), AllocPref::PreferPSRAM, "thermal.local");
    if (!g_localFrame) {
      ERROR_THERMALF("Failed to allocate localFrame buffer (1.5KB)");
      return false;
    }
    INFO_THERMAL_LIFECYCLEF("Allocated localFrame buffer: 1536 bytes in PSRAM");
  }
  
  // Detailed pre-capture diagnostics
  DEBUG_THERMAL_POLLINGF("[THERMAL_FRAME] Pre-capture: sensor=%p enabled=%d connected=%d polling_paused=%d",
                 (void*)gMLX90640, gThermalRunning ? 1 : 0, gThermalConnected ? 1 : 0, 
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
    ERROR_THERMALF("MLX90640 frame capture failed: error=%d (%s), time=%lums, heap=%lu",
                   result, errDesc, (unsigned long)captureTime, (unsigned long)ESP.getFreeHeap());
    
    // Check I2C health for this device
    I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
    I2CDevice* dev = mgr ? mgr->getDevice(I2C_ADDR_THERMAL,
                                          (uint8_t)gSettings.thermalBus) : nullptr;
    if (dev) {
      const I2CDevice::Health& h = dev->getHealth();
      ERROR_THERMALF("  I2C Health: degraded=%d consec=%d total=%d NACK=%d TIMEOUT=%d",
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
        ERROR_THERMALF("Failed to allocate previousFrame buffer");
      } else {
        INFO_THERMAL_LIFECYCLEF("Allocated temporal smoothing buffer: 1536 bytes");
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
      DEBUG_THERMAL_POLLINGF("[Thermal] Rolling min/max initialized: min=%.2f, max=%.2f", rollingMin, rollingMax);
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

  // Over-temp hotspot alert, edge-latched with hysteresis: post once when the
  // scene max crosses 50C, clear silently below 45C. Never posts every poll.
  if (!gThermalHot && maxTemp > 50.0f) {
    gThermalHot = true;
    char maxC[16];
    snprintf(maxC, sizeof(maxC), "%.1fC", maxTemp);
    systemEventPost(SYSEVT_THERMAL_HOT_ALERT, maxC);
  } else if (gThermalHot && maxTemp < 45.0f) {
    gThermalHot = false;
  }

  if (lockThermalCache(pdMS_TO_TICKS(50))) {
    // A start/reset can retire storage while a hardware read is in flight.
    if (!sThermalCache.thermalFrame) {
      unlockThermalCache();
      return false;
    }
    memcpy(sThermalCache.thermalFrame, g_localFrame, 768 * sizeof(int16_t));
    
    if (previousFrame) {
      memcpy(previousFrame, g_localFrame, 768 * sizeof(int16_t));
      previousFrameValid = true;
    }

    sThermalCache.thermalMinTemp = minTemp;
    sThermalCache.thermalMaxTemp = maxTemp;
    sThermalCache.thermalAvgTemp = avgTemp;
    sThermalCache.thermalLastUpdate = millis();
    sThermalCache.thermalDataValid = true;
    sThermalCache.thermalSeq++;

    if (sThermalCache.thermalInterpolated && sThermalCache.thermalInterpolatedWidth > 0) {
      size_t psram_before_op = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      size_t heap_before_op = heap_caps_get_free_size(MALLOC_CAP_8BIT);

      uint32_t interpStart = millis();
      static float* floatFrame = nullptr;
      if (!floatFrame) {
        floatFrame = (float*)ps_alloc(768 * sizeof(float), AllocPref::PreferPSRAM, "thermal.float");
        if (!floatFrame) {
          ERROR_THERMALF("Failed to allocate floatFrame buffer");
          unlockThermalCache();
          return true;
        }
      }
      for (int i = 0; i < 768; i++) {
        floatFrame[i] = g_localFrame[i] / 100.0f;
      }
      thermalInterpolateFrame(floatFrame, sThermalCache.thermalInterpolated,
                              sThermalCache.thermalInterpolatedWidth, sThermalCache.thermalInterpolatedHeight);
      uint32_t interpTime = millis() - interpStart;

      if (isDebugFlagSet(DEBUG_THERMAL | DEBUG_THERMAL_POLLING)) {
        size_t psram_after_op = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        size_t heap_after_op = heap_caps_get_free_size(MALLOC_CAP_8BIT);

        DEBUG_THERMAL_POLLINGF("[THERMAL_MEM] Interpolation runtime: %lums, PSRAM delta=%d, Heap delta=%d",
                       (unsigned long)interpTime,
                       (int)(psram_before_op - psram_after_op),
                       (int)(heap_before_op - heap_after_op));
      }

      DEBUG_THERMAL_POLLINGF("Interpolation completed in %lums (%dx%d -> %dx%d)",
                   (unsigned long)interpTime, 32, 24,
                   sThermalCache.thermalInterpolatedWidth, sThermalCache.thermalInterpolatedHeight);
    }

    if (gSettings.thermalRotation != 0) {
      static int16_t* rotatedFrame = nullptr;
      if (!rotatedFrame) {
        rotatedFrame = (int16_t*)ps_alloc(768 * sizeof(int16_t), AllocPref::PreferPSRAM, "thermal.rotate");
        if (!rotatedFrame) {
          ERROR_THERMALF("Failed to allocate rotation buffer, skipping rotation");
          unlockThermalCache();
          return true;
        }
        DEBUG_THERMAL_LIFECYCLEF("[THERMAL_MEM] Allocated rotation buffer: 1536 bytes");
      }
      
      const int width = 32;
      const int height = 24;
      
      DEBUG_THERMAL_POLLINGF("[ROTATION_DEBUG] Frame capture: applying rotation=%d, seq=%lu",
                     gSettings.thermalRotation,
                     (unsigned long)sThermalCache.thermalSeq);
      DEBUG_THERMAL_POLLINGF("[THERMAL_ROTATION] Applying rotation=%d (after all processing)", gSettings.thermalRotation);
      DEBUG_THERMAL_POLLINGF("[THERMAL_ROTATION] Before: TL=%d TR=%d BL=%d BR=%d",
                     sThermalCache.thermalFrame[0],
                     sThermalCache.thermalFrame[width - 1],
                     sThermalCache.thermalFrame[(height - 1) * width],
                     sThermalCache.thermalFrame[(height - 1) * width + width - 1]);
      
      if (gSettings.thermalRotation == 1) {
        DEBUG_THERMAL_POLLINGF("[THERMAL_ROTATION] Performing 90° clockwise rotation");
        for (int y = 0; y < height; y++) {
          for (int x = 0; x < width; x++) {
            int srcIdx = y * width + x;
            int newX = y;
            int newY = width - 1 - x;
            int dstIdx = newY * height + newX;
            rotatedFrame[dstIdx] = sThermalCache.thermalFrame[srcIdx];
          }
        }
      } else if (gSettings.thermalRotation == 2) {
        DEBUG_THERMAL_POLLINGF("[THERMAL_ROTATION] Performing 180° rotation");
        for (int y = 0; y < height; y++) {
          for (int x = 0; x < width; x++) {
            int srcIdx = y * width + x;
            int dstIdx = (height - 1 - y) * width + (width - 1 - x);
            rotatedFrame[dstIdx] = sThermalCache.thermalFrame[srcIdx];
          }
        }
      } else if (gSettings.thermalRotation == 3) {
        DEBUG_THERMAL_POLLINGF("[THERMAL_ROTATION] Performing 270° clockwise rotation");
        for (int y = 0; y < height; y++) {
          for (int x = 0; x < width; x++) {
            int srcIdx = y * width + x;
            int newX = height - 1 - y;
            int newY = x;
            int dstIdx = newY * height + newX;
            rotatedFrame[dstIdx] = sThermalCache.thermalFrame[srcIdx];
          }
        }
      }

      memcpy(sThermalCache.thermalFrame, rotatedFrame, 768 * sizeof(int16_t));

      DEBUG_THERMAL_POLLINGF("[THERMAL_ROTATION] After:  TL=%d TR=%d BL=%d BR=%d",
                     sThermalCache.thermalFrame[0],
                     sThermalCache.thermalFrame[width - 1],
                     sThermalCache.thermalFrame[(height - 1) * width],
                     sThermalCache.thermalFrame[(height - 1) * width + width - 1]);
    }

    unlockThermalCache();

    if (gThermalPendingFirstFrame) {
      gThermalPendingFirstFrame = false;
      sensorStatusBumpWith("thermal-ready");
    }
  } else {
    DEBUG_THERMAL_POLLINGF("Failed to lock thermal cache for thermal update - skipping");
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

  if (isDebugFlagSet(DEBUG_THERMAL | DEBUG_THERMAL_POLLING) && ((dbgCounter++ % 10) == 0)) {
    DEBUG_THERMAL_POLLINGF("THERM frame: cap=%lums, proc=%lums, total=%lums, fps_i=%.2f, fps_ema=%.2f, i2cHz=%lu, tgtFps=%d(eff=%d), heap=%lu",
                 (unsigned long)captureTime, (unsigned long)processingTime,
                 (unsigned long)totalTime, instFps, emaFps,
                 (unsigned long)gSettings.i2cClockThermalHz,
                 gSettings.thermalTargetFps, effFps,
                 (unsigned long)ESP.getFreeHeap());
  }


  return true;
}

// Helper function to reset frame buffers during thermal task cleanup
void resetThermalFrameBuffers() {
  INFO_THERMAL_LIFECYCLEF("Freeing frame buffers to prevent heap corruption on restart");
  
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

int thermalBuildDataJSON(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;
  auto frame = thermalSnapshotBuffer(THERMAL_MAX_FRAME_PIXELS);
  ThermalCache reading;
  if (!frame || !thermalFrameSnapshot(reading, frame.get(), THERMAL_MAX_FRAME_PIXELS, true, CACHE_MUTEX_TIMEOUT_MS)) {
    int n = snprintf(buf, bufSize, "{\"error\":\"Sensor data temporarily unavailable\"}");
    return n < 0 || static_cast<size_t>(n) >= bufSize ? 0 : n;
  }
  if (!reading.thermalFrameAvailable) {
    int n = snprintf(buf, bufSize, "{\"val\":0,\"error\":\"Sensor stopped\"}");
    return n < 0 || static_cast<size_t>(n) >= bufSize ? 0 : n;
  }
  int pos = snprintf(buf, bufSize,
      "{\"val\":%d,\"seq\":%lu,\"mn\":%.1f,\"mx\":%.1f,\"w\":%d,\"h\":%d,\"data\":[",
      reading.thermalDataValid ? 1 : 0, (unsigned long)reading.thermalSeq,
      reading.thermalMinTemp, reading.thermalMaxTemp, reading.thermalFrameWidth, reading.thermalFrameHeight);
  if (pos < 0 || static_cast<size_t>(pos) >= bufSize) return 0;
  const int count = reading.thermalFrameWidth * reading.thermalFrameHeight;
  for (int i = 0; i < count; ++i) {
    int n = snprintf(buf + pos, bufSize - pos, "%d%s", frame.get()[i], i < count - 1 ? "," : "");
    if (n < 0 || static_cast<size_t>(n) >= bufSize - pos) return 0;
    pos += n;
  }
  int n = snprintf(buf + pos, bufSize - pos, "]}");
  return n < 0 || static_cast<size_t>(n) >= bufSize - pos ? 0 : pos + n;
}

// ============================================================================
// Thermal Interpolation (moved from .ino to fix Arduino preprocessor issues)
// ============================================================================

// Bilinear interpolation for thermal upscaling
// Upscales 32x24 source to targetWidth x targetHeight
void thermalInterpolateFrame(const float* src, float* dst, int targetWidth, int targetHeight) {
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

const char* cmd_thermalpollingms(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: thermalpollingms <50..5000>";
  int v = _arg.toInt();
  if (v < 50 || v > 5000) return "Error: [Thermal] Polling interval must be 50-5000ms";
  setSetting(gSettings.thermalPollingMs, v);
  BROADCAST_PRINTF("thermalPollingMs set to %d", v);
  return "[Thermal] Setting updated";
}

const char* cmd_thermalinterpolationenabled(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: thermalinterpolationenabled <0|1>";
  const char* p = _arg.c_str();
  // Accept 0, 1, true, false (case insensitive)
  bool enabled = (*p == '1' || strncasecmp(p, "true", 4) == 0);
  setSetting(gSettings.thermalInterpolationEnabled, enabled);
  BROADCAST_PRINTF("thermalInterpolationEnabled set to %s", enabled ? "1" : "0");
  return "[Thermal] Setting updated";
}

const char* cmd_thermalinterpolationsteps(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: thermalinterpolationsteps <1..8>";
  int v = _arg.toInt();
  if (v < 1 || v > 8) return "Error: [Thermal] Interpolation steps must be 1-8";
  setSetting(gSettings.thermalInterpolationSteps, v);
  BROADCAST_PRINTF("thermalInterpolationSteps set to %d", v);
  return "[Thermal] Setting updated";
}

const char* cmd_thermalinterpolationbuffersize(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String _arg = argsInput; _arg.trim();
  if (_arg.length() == 0) return "Error: invalid arguments — Usage: thermalinterpolationbuffersize <1..10>";
  int v = _arg.toInt();
  if (v < 1 || v > 10) return "Error: [Thermal] Interpolation buffer size must be 1-10";
  setSetting(gSettings.thermalInterpolationBufferSize, v);
  BROADCAST_PRINTF("thermalInterpolationBufferSize set to %d", v);
  return "[Thermal] Setting updated";
}

const char* cmd_thermaldiag(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  char* buf = getDebugBuffer();
  int remaining = 1024;
  int n = 0;
  
  n = snprintf(buf, remaining, "=== THERMAL SENSOR DIAGNOSTICS ===\n");
  buf += n; remaining -= n;
  
  // Current state
  n = snprintf(buf, remaining, "State: enabled=%d connected=%d sensor=%p\n",
               gThermalRunning ? 1 : 0, gThermalConnected ? 1 : 0, (void*)gMLX90640);
  buf += n; remaining -= n;
  
  n = snprintf(buf, remaining, "Task: handle=%p\n", (void*)gThermalTaskHandle);
  buf += n; remaining -= n;
  
  // Check I2C device health
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  const uint8_t thermalBus = (uint8_t)gSettings.thermalBus;
  I2CDevice* dev = mgr ? mgr->getDevice(I2C_ADDR_THERMAL, thermalBus) : nullptr;
  
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
  if (gThermalRunning) {
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
        DEBUG_THERMAL_LIFECYCLEF("[THERMAL_DIAG] Cleared degraded status before %s test", clockNames[i]);
      }
      
      // Probe the device with mutex protection
      uint8_t result = i2cProbeAddress(I2C_ADDR_THERMAL, testClocks[i], 200,
                                       thermalBus);
      
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
      
      DEBUG_THERMAL_LIFECYCLEF("[THERMAL_DIAG] %s probe result: %s (err=%d)", clockNames[i], resultStr, result);
      
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
  
  // The assembled report exceeds the 256-char broadcast cap (DEBUG_MSG_SIZE),
  // so returning it whole gets truncated ("...100[CUT]"). Re-emit it one line at
  // a time through BROADCAST_PRINTF — every report line is well under the cap —
  // then return a short status as the command result.
  const char* report = getDebugBuffer();
  char line[256];
  size_t li = 0;
  for (const char* p = report; ; ++p) {
    if (*p == '\n' || *p == '\0') {
      line[li] = '\0';
      if (li > 0) BROADCAST_PRINTF("%s", line);
      li = 0;
      if (*p == '\0') break;
    } else if (li < sizeof(line) - 1) {
      line[li++] = *p;
    }
  }
  return "[thermal] diagnostics complete";
}

// ============================================================================
// Thermal Command Registry
// ============================================================================

// External device-level command handlers (defined in i2c_system.cpp)
extern const char* cmd_thermaltargetfps(const String& argsInput);
extern const char* cmd_thermaldevicepollms(const String& argsInput);

const char* cmd_thermalautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String arg = argsInput; arg.trim();
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
  return "Error: invalid arguments — Usage: thermalautostart [on|off]";
}

// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
// Bus routing for this device. Lives here rather than in System_I2C.cpp so it
// compiles out with the driver — see i2cSetDeviceBusAndReport in System_I2C.h.
static const char* cmd_thermalbus(const String& a) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return i2cSetDeviceBusAndReport(gSettings.thermalBus, a, "thermalBus");
}

const CommandEntry thermalCommands[] = {
  // Start/Stop (3-level voice: "sensor" -> "thermal camera" -> "open/close")
  { "openthermal", "Start MLX90640 thermal sensor.", false, cmd_thermalstart },
  { "closethermal", "Stop MLX90640 thermal sensor.", false, cmd_thermalstop },
  { "thermalread", "Read thermal frame; min/max/avg broadcast to output. (add 'json' for JSON output)", false, cmd_thermalread },
  
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
  { "thermali2cclockhz", "Thermal I2C clock: <100000..1000000>", true, cmd_thermali2cclockhz, "Usage: thermalI2cClockHz <100000..1000000>" },
  { "thermalwebmaxfps", "Thermal web max FPS: <1..30>", true, cmd_thermalwebmaxfps, "Usage: thermalWebMaxFps <1..30>" },
  
  // Diagnostics
  { "thermaldiag", "Run thermal sensor diagnostics.", false, cmd_thermaldiag },
  
  // Auto-start
  { "thermalautostart", "Enable/disable thermal auto-start after boot [on|off]", false, cmd_thermalautostart, "Usage: thermalautostart [on|off]" },
  { "thermalbus",  "Route MLX90640 thermal to bus: <0|1> (reboot required)", true, cmd_thermalbus, "Usage: thermalBus <0|1>" },
};

const size_t thermalCommandsCount = sizeof(thermalCommands) / sizeof(thermalCommands[0]);

// ============================================================================
// Command Registration
// ============================================================================
// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// Thermal Task Implementation (moved from i2c_system.cpp for full modularization)
// ============================================================================

// ============================================================================
// Thermal Task - FreeRTOS Task Function
// ============================================================================
// Purpose: Continuously reads 32x24 thermal frame data from MLX90640 sensor
// Stack: 4096 BYTES (4 KB) | Priority: 1 | Core: Any
// Lifecycle: Created by cmd_thermalstart, deleted when gThermalRunning=false
// Polling: Configurable via thermalDevicePollMs (default 100ms) | I2C Clock: 100-1000kHz
//
// Cleanup Strategy:
//   1. Check gThermalRunning flag at loop start
//   2. Acquire bus mutex via I2CDeviceManager to prevent race conditions during cleanup
//   3. Delete sensor object and invalidate cache
//   4. Release mutex and delete task
// ============================================================================

void thermalTask(void* parameter) {
  INFO_THERMAL_LIFECYCLEF("Task started (handle=%p, stack=%u words)", 
                (void*)xTaskGetCurrentTaskHandle(), 
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
  INFO_THERMAL_LIFECYCLEF("[MODULAR] thermalTask() running from i2csensor_mlx90640.cpp");
  gThermalHot = false;
  unsigned long lastThermalRead = 0;
  unsigned long lastStackLog = 0;
  while (true) {
    // CRITICAL: Check enabled flag FIRST before any debug calls or operations
    // This allows graceful shutdown when thermalstop is called
    if (!gThermalRunning) {
      if (!thermalInvalidateCache()) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      gThermalConnected = false;
      if (gMLX90640 != nullptr) {
        ps_delete(gMLX90640);
        gMLX90640 = nullptr;
      }
      // Free static buffers in thermalPoll to prevent heap corruption on restart
      extern void resetThermalFrameBuffers();
      resetThermalFrameBuffers();
      gThermalInitRequested = false;
      gThermalInitDone = false;
      gThermalInitResult = false;
      gThermalPendingFirstFrame = false;
      gThermalArmAtMs = 0;
      
      SENSOR_TASK_EXIT(THERMAL, gThermalTaskHandle);
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
      // 'continue' (not 'break') so the top-of-loop shutdown runs the clean
      // SENSOR_TASK_EXIT (vTaskDelete) path instead of returning from the task
      // function with a near-overflowed stack (IllegalInstruction panic).
      if (checkTaskStackSafety("thermal", THERMAL_STACK_WORDS, &gThermalRunning)) continue;
      // CRITICAL: Check enabled flag again before debug output (prevent crash during shutdown)
      if (gThermalRunning) {
        DEBUG_PERFORMANCEF("[STACK] thermal_task watermark_now=%u min=%u words", (unsigned)gThermalWatermarkNow, (unsigned)gThermalWatermarkMin);
        DEBUG_MEMORY_HEAPF("[HEAP] thermal_task: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }
    // Handle deferred initialization request
    if (gThermalRunning && (!gThermalConnected || gMLX90640 == nullptr)) {
      if (gThermalInitRequested) {
        bool ok = thermalInit();
        gThermalInitResult = ok;
        gThermalInitDone = true;
        gThermalInitRequested = false;
        if (!ok) gThermalRunning = false;
      }
    }

    const uint8_t thermalBus = (uint8_t)gSettings.thermalBus;
    if (gThermalRunning && gThermalConnected && gMLX90640 != nullptr && !pollPaused(thermalBus)) {
      unsigned long nowMs = millis();
      unsigned long pollMs = (gSettings.thermalDevicePollMs > 0) ? (unsigned long)gSettings.thermalDevicePollMs : 100;
      bool ready = true;
      if (gThermalArmAtMs) {
        int32_t dt = (int32_t)(nowMs - gThermalArmAtMs);
        if (dt < 0) ready = false;
      }
      if (ready && (nowMs - lastThermalRead) >= pollMs) {
        uint32_t thermalHz = (gSettings.i2cClockThermalHz > 0) ? (uint32_t)gSettings.i2cClockThermalHz : 400000;
        bool ok = false;
        
        // Thermal frame read takes 400-800ms at 100kHz (768 pixels); needs generous timeout
        ok = i2cTaskWithTimeout(thermalBus, I2C_ADDR_THERMAL, thermalHz, 1500, [&]() -> bool {
          return thermalPoll();
        });
        
        lastThermalRead = millis();
        
        // Auto-disable if too many consecutive failures (like gamepad does)
        if (!ok) {
          if (i2cShouldAutoDisable(I2C_ADDR_THERMAL, (uint8_t)gSettings.thermalBus)) {
            ERROR_THERMALF("Too many consecutive thermal failures - auto-disabling");
            gThermalRunning = false;
            sensorStatusBumpWith("thermal@auto_disabled");
            logSystemEvent("SENSOR", "Thermal auto-disabled after too many consecutive I2C failures");
            systemEventPost(SYSEVT_SENSOR_FAULT, "Thermal", "consecutive I2C failures");
            // Task will clean up and delete itself on next loop iteration
          }
        }
        
        // SAFE: Debug output AFTER i2cTransaction completes, with enabled check
        if (gThermalRunning && gThermalPendingFirstFrame && ok) {
          gThermalPendingFirstFrame = false;
          DEBUG_THERMAL_LIFECYCLEF("Thermal first frame captured");
        }
        
        // Thermal mesh streaming is DISABLED (gSensorSpecs[REMOTE_SENSOR_THERMAL] = {nullptr,0,0});
        // buildThermalDataJSONInteger() currently has no callers. Re-enable over mesh by pointing
        // the spec at thermalBuildSummaryJSON (~90 B clears the 200 B gate); the full frame never will.
      }
      vTaskDelay(pdMS_TO_TICKS(10));
    } else {
      // Enabled-but-not-ready, disconnected, or polling paused: yield instead of
      // busy-spinning the core. Without this else the while(true) loop had no delay
      // on the gated path and pinned the core at 100% during any pollPause window
      // (matches imuTask's 50 ms idle delay).
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
}

// ============================================================================
// Additional Thermal Device Commands
// ============================================================================

const char* cmd_thermaltargetfps(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: thermalTargetFps <1..8>";
  int v = valStr.toInt();
  if (v < 1) v = 1;
  if (v > 8) v = 8;
  setSetting(gSettings.thermalTargetFps, v);
  snprintf(getDebugBuffer(), 1024, "thermalTargetFps set to %d", v);
  return getDebugBuffer();
}

const char* cmd_thermaldevicepollms(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String valStr = argsInput;
  valStr.trim();
  if (valStr.length() == 0) return "Error: invalid arguments — Usage: thermalDevicePollMs <100..2000>";
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
#include "i2csensor_mlx90640_oled.h"
#endif

#endif // ENABLE_THERMAL_SENSOR
