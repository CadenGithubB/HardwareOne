#ifndef I2CSENSOR_MLX90640_H
#define I2CSENSOR_MLX90640_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Copied display statistics and frame metadata. Raw frame values are signed
// centidegrees; interpolated copies use whole degrees, matching the wire format.
constexpr size_t THERMAL_RAW_FRAME_PIXELS = 32 * 24;
constexpr size_t THERMAL_MAX_FRAME_PIXELS = 64 * 48;
struct ThermalCache {
  float thermalMinTemp = 0.0;
  float thermalMaxTemp = 0.0;
  float thermalAvgTemp = 0.0;
  unsigned long thermalLastUpdate = 0;
  bool thermalDataValid = false;
  uint32_t thermalSeq = 0;
  int thermalFrameWidth = 0;
  int thermalFrameHeight = 0;
  bool thermalFrameAvailable = false;
  bool thermalFrameInterpolated = false;
};

#if ENABLE_THERMAL_SENSOR

// Forward declarations
class String;

// Copies clear out on failure. Frame storage remains caller-owned and unchanged
// on failure; capacity is in int16_t values. A successful copy with no frame
// sets thermalFrameAvailable=false and does not write values. Request 768 values
// for raw only, or 3072 when preferInterpolated=true. No lock is retained.
bool thermalCacheSnapshot(ThermalCache& out, uint32_t timeoutMs = 10);
bool thermalFrameSnapshot(ThermalCache& out, int16_t* values, size_t capacity,
                          bool preferInterpolated = false, uint32_t timeoutMs = 10);
size_t thermalCacheStorageBytes();  // Static cache plus mutex handle; excludes heap buffers.

// Thermal sensor state and control
extern bool gThermalRunning;
extern bool gThermalConnected;
extern unsigned long gThermalLastStopTime;
extern TaskHandle_t gThermalTaskHandle;

// Thermal initialization handoff flags
extern volatile bool gThermalInitRequested;
extern volatile bool gThermalInitDone;
extern volatile bool gThermalInitResult;
extern volatile uint32_t gThermalArmAtMs;

// Thermal watermark tracking
extern volatile UBaseType_t gThermalWatermarkMin;
extern volatile UBaseType_t gThermalWatermarkNow;

// Thermal sensor state
extern bool gMlx90640Initialized;
extern volatile bool gThermalPendingFirstFrame;

// Thermal timing constants
extern const unsigned long MLX90640_READ_INTERVAL;

// Thermal sensor command handlers
const char* cmd_thermalstart(const String& argsInput);
const char* cmd_thermalstop(const String& argsInput);
const char* cmd_thermalpalettedefault(const String& argsInput);
const char* cmd_thermalewmafactor(const String& argsInput);
const char* cmd_thermaltransitionms(const String& argsInput);
const char* cmd_thermalupscalefactor(const String& argsInput);
const char* cmd_thermalrollingminmaxenabled(const String& argsInput);
const char* cmd_thermalrollingminmaxalpha(const String& argsInput);
const char* cmd_thermalrollingminmaxguardc(const String& argsInput);
const char* cmd_thermaltemporalalpha(const String& argsInput);
const char* cmd_thermalrotation(const String& argsInput);
const char* cmd_thermalpollingms(const String& argsInput);
const char* cmd_thermalinterpolationenabled(const String& argsInput);
const char* cmd_thermalinterpolationsteps(const String& argsInput);
const char* cmd_thermalinterpolationbuffersize(const String& argsInput);

// Internal function called by queue processor
bool thermalStartInternal();

// Thermal sensor functions
bool thermalInit();
bool thermalPoll();

// JSON building
int thermalBuildDataJSON(char* buf, size_t bufSize);
// Compact SUMMARY reading (min/avg/max scene temp) on the shared sensor envelope.
// The full 768-px frame stays on thermalBuildDataJSON / the ESP-NOW frame channel.
int thermalBuildSummaryJSON(char* buf, size_t bufSize);

// Thermal interpolation (defined in thermal_sensor.cpp)
void thermalInterpolateFrame(const float* src, float* dst, int targetWidth, int targetHeight);

// Thermal command registry (for System_CommandRegistry.cpp module list)
struct CommandEntry;
extern const CommandEntry thermalCommands[];
extern const size_t thermalCommandsCount;

#else
inline bool thermalCacheSnapshot(ThermalCache& out, uint32_t timeoutMs = 10) {
  (void)timeoutMs; out = {}; return false;
}
inline bool thermalFrameSnapshot(ThermalCache& out, int16_t* values, size_t capacity,
                                 bool preferInterpolated = false, uint32_t timeoutMs = 10) {
  (void)values; (void)capacity; (void)preferInterpolated; (void)timeoutMs;
  out = {}; return false;
}
inline size_t thermalCacheStorageBytes() { return 0; }
inline int thermalBuildDataJSON(char* buf, size_t bufSize) {
  if (buf && bufSize) buf[0] = '\0';
  return 0;
}
inline int thermalBuildSummaryJSON(char* buf, size_t bufSize) {
  if (buf && bufSize) buf[0] = '\0';
  return 0;
}
#endif // ENABLE_THERMAL_SENSOR
#endif // THERMAL_SENSOR_H
