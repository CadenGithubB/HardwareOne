#ifndef I2CSENSOR_MLX90640_H
#define I2CSENSOR_MLX90640_H

#include "System_BuildConfig.h"

#if ENABLE_THERMAL_SENSOR

#include <Adafruit_MLX90640.h>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "System_Command.h"

// Forward declarations
class String;

// Thermal sensor cache structure
struct ThermalCache {
  SemaphoreHandle_t mutex = nullptr;
  // Using int16_t to store temperatures as centidegrees (celsius × 100) for memory efficiency
  int16_t* thermalFrame = nullptr;       // Raw 32x24 frame (768 pixels) stored as centidegrees
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

extern ThermalCache gThermalCache;

// Thermal sensor state and control
extern bool thermalEnabled;
extern bool thermalConnected;
extern unsigned long thermalLastStopTime;

// Thermal initialization handoff flags
extern volatile bool thermalInitRequested;
extern volatile bool thermalInitDone;
extern volatile bool thermalInitResult;
extern volatile uint32_t thermalArmAtMs;

// Thermal watermark tracking
extern volatile UBaseType_t gThermalWatermarkMin;
extern volatile UBaseType_t gThermalWatermarkNow;

// Thermal sensor state
extern bool mlx90640_initialized;
extern volatile bool thermalPendingFirstFrame;
extern Adafruit_MLX90640* gMLX90640;

// Thermal timing constants
extern const unsigned long MLX90640_READ_INTERVAL;

// Thermal sensor command handlers
const char* cmd_thermalstart(const String& cmd);
const char* cmd_thermalstop(const String& cmd);
const char* cmd_thermalpalettedefault(const String& cmd);
const char* cmd_thermalewmafactor(const String& cmd);
const char* cmd_thermaltransitionms(const String& cmd);
const char* cmd_thermalupscalefactor(const String& cmd);
const char* cmd_thermalrollingminmaxenabled(const String& cmd);
const char* cmd_thermalrollingminmaxalpha(const String& cmd);
const char* cmd_thermalrollingminmaxguardc(const String& cmd);
const char* cmd_thermaltemporalalpha(const String& cmd);
const char* cmd_thermalrotation(const String& cmd);
const char* cmd_thermalpollingms(const String& cmd);
const char* cmd_thermalinterpolationenabled(const String& cmd);
const char* cmd_thermalinterpolationsteps(const String& cmd);
const char* cmd_thermalinterpolationbuffersize(const String& cmd);

// Thermal sensor state and control
extern bool thermalEnabled;
extern bool thermalConnected;
extern uint32_t thermalLastStopTime;
extern TaskHandle_t thermalTaskHandle;

// Internal function called by queue processor
bool startThermalSensorInternal();

// Thermal sensor functions
bool initThermalSensor();
bool readThermalPixels();

// JSON building
int buildThermalDataJSON(char* buf, size_t bufSize);

// Thermal interpolation (defined in thermal_sensor.cpp)
void interpolateThermalFrame(const float* src, float* dst, int targetWidth, int targetHeight);

// Thermal command registry (for system_utils.cpp module list)
struct CommandEntry;
extern const CommandEntry thermalCommands[];
extern const size_t thermalCommandsCount;

#endif // ENABLE_THERMAL_SENSOR
#endif // THERMAL_SENSOR_H
