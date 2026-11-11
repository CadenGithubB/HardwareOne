/**
 * Sensor Registry - Unified sensor descriptor system
 * System_SensorRegistry.cpp
 */

#include "System_SensorRegistry.h"
#include "System_BuildConfig.h"
#include <string.h>
#if ENABLE_CAMERA_SENSOR
#include "System_Camera_DVP.h"
#endif

// ============================================================================
// Camera Sensor Callbacks
// ============================================================================

#if ENABLE_CAMERA_SENSOR
static bool getCameraConnected() {
  return cameraConnected;
}

static bool getCameraEnabled() {
  return gCameraRunning;
}

static const char* getCameraTask() {
  // Camera could report SENSOR_TASK_STREAMING when actively streaming
  // For now, just return none - can be enhanced later
  return SENSOR_TASK_NONE;
}

static const char* cameraValidTasks[] = { SENSOR_TASK_STREAMING, nullptr };
#endif

// ============================================================================
// Microphone Sensor Callbacks
// ============================================================================

#if ENABLE_MICROPHONE
#include "HAL_Audio.h"   // audioAnySourceAvailable — "connected" = a mic source is reachable
#include "System_Microphone.h"

static bool getMicConnected() {
  // "Connected" = a source is actually available right now (PDM present OR G2
  // glasses connected), independent of whether capture is running.
  return audioAnySourceAvailable();
}

static bool getMicEnabled() {
  return gMicRunning;
}

static const char* getMicTask() {
  if (micRecordingBusy()) return SENSOR_TASK_RECORDING;
  return SENSOR_TASK_NONE;
}

static const char* micValidTasks[] = { SENSOR_TASK_RECORDING, nullptr };
#endif // ENABLE_MICROPHONE

// ============================================================================
// Non-I2C Sensor Registry
// ============================================================================

const NonI2cSensorEntry nonI2cSensors[] = {
#if ENABLE_CAMERA_SENSOR
  {
    "camera",
    "Camera",
    SENSOR_CATEGORY_CAMERA,
    cameraValidTasks,
    getCameraConnected,
    getCameraEnabled,
    getCameraTask,
    "edgeimpulse"  // Associated ML settings module
  },
#endif
#if ENABLE_MICROPHONE
  {
    "microphone",
    "Microphone (PDM/G2)",
    SENSOR_CATEGORY_AUDIO,
    micValidTasks,
    getMicConnected,
    getMicEnabled,
    getMicTask,
    nullptr  // Future: audio ML settings module
  },
#endif
};

const size_t nonI2cSensorsCount = sizeof(nonI2cSensors) / sizeof(nonI2cSensors[0]);

// ============================================================================
// Registry Access Functions
// ============================================================================

const NonI2cSensorEntry* findNonI2cSensor(const char* id) {
  if (!id) return nullptr;
  for (size_t i = 0; i < nonI2cSensorsCount; i++) {
    if (strcmp(nonI2cSensors[i].id, id) == 0) {
      return &nonI2cSensors[i];
    }
  }
  return nullptr;
}

void initSensorRegistry() {
  // Currently just logs initialization
  // Future: could validate registry entries, etc.
}
