#ifndef I2CSENSOR_MLX90640_H
#define I2CSENSOR_MLX90640_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Thermal sensor cache structure (always available for type-safe references)
struct ThermalCache {
  SemaphoreHandle_t mutex = nullptr;
  int16_t* thermalFrame = nullptr;       // Raw 32x24 frame (768 pixels) stored as centidegrees (celsius x 100)
  float* thermalInterpolated = nullptr;  // Interpolated frame (quality-dependent size)
  int thermalInterpolatedWidth = 0;
  int thermalInterpolatedHeight = 0;
  float thermalMinTemp = 0.0;
  float thermalMaxTemp = 0.0;
  float thermalAvgTemp = 0.0;
  unsigned long thermalLastUpdate = 0;
  bool thermalDataValid = false;
  uint32_t thermalSeq = 0;
};

#if ENABLE_THERMAL_SENSOR

#include <Adafruit_MLX90640.h>
#include "System_Command.h"

// Forward declarations
class String;

extern ThermalCache gThermalCache;

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
extern Adafruit_MLX90640* gMLX90640;

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

// Thermal command registry (for system_utils.cpp module list)
struct CommandEntry;
extern const CommandEntry thermalCommands[];
extern const size_t thermalCommandsCount;

#endif // ENABLE_THERMAL_SENSOR
#endif // THERMAL_SENSOR_H
