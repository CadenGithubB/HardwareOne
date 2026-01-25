/**
 * Sensor Registry - Unified sensor descriptor system
 * System_SensorRegistry.cpp
 */

#include "System_SensorRegistry.h"
#include "System_BuildConfig.h"
#include <string.h>

// ============================================================================
// Camera Sensor Callbacks
// ============================================================================

#if ENABLE_CAMERA_SENSOR
extern bool cameraEnabled;
extern bool cameraConnected;

static bool getCameraConnected() {
  return cameraConnected;
}

static bool getCameraEnabled() {
  return cameraEnabled;
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

#if ENABLE_MICROPHONE_SENSOR
extern bool micEnabled;
extern bool micConnected;
extern bool micRecording;

static bool getMicConnected() {
  return micConnected;
}

static bool getMicEnabled() {
  return micEnabled;
}

static const char* getMicTask() {
  if (micRecording) return SENSOR_TASK_RECORDING;
  return SENSOR_TASK_NONE;
}

static const char* micValidTasks[] = { SENSOR_TASK_RECORDING, nullptr };
#endif

// ============================================================================
// Non-I2C Sensor Registry
// ============================================================================

const NonI2CSensorEntry nonI2CSensors[] = {
#if ENABLE_CAMERA_SENSOR
  {
    "camera",
    "Camera (OV2640/OV3660)",
    SENSOR_CATEGORY_CAMERA,
    cameraValidTasks,
    getCameraConnected,
    getCameraEnabled,
    getCameraTask,
    "edgeimpulse"  // Associated ML settings module
  },
#endif
#if ENABLE_MICROPHONE_SENSOR
  {
    "microphone",
    "Microphone (PDM)",
    SENSOR_CATEGORY_AUDIO,
    micValidTasks,
    getMicConnected,
    getMicEnabled,
    getMicTask,
    nullptr  // Future: audio ML settings module
  },
#endif
};

const size_t nonI2CSensorsCount = sizeof(nonI2CSensors) / sizeof(nonI2CSensors[0]);

// ============================================================================
// Registry Access Functions
// ============================================================================

const NonI2CSensorEntry* findNonI2CSensor(const char* id) {
  if (!id) return nullptr;
  for (size_t i = 0; i < nonI2CSensorsCount; i++) {
    if (strcmp(nonI2CSensors[i].id, id) == 0) {
      return &nonI2CSensors[i];
    }
  }
  return nullptr;
}

void initSensorRegistry() {
  // Currently just logs initialization
  // Future: could validate registry entries, etc.
}
