/**
 * Sensor Registry - Unified sensor descriptor system
 * System_SensorRegistry.h
 * 
 * Provides a unified way to register and query sensors regardless of
 * their underlying interface (I2C, DVP, PDM, etc.)
 */

#ifndef SYSTEM_SENSOR_REGISTRY_H
#define SYSTEM_SENSOR_REGISTRY_H

#include <stdint.h>
#include <stddef.h>

// ============================================================================
// Standard Task Constants
// ============================================================================
// Use these constants for the getTask() return value to ensure consistency
// across all sensors. Empty string means no special task active.

#define SENSOR_TASK_NONE        ""
#define SENSOR_TASK_RECORDING   "recording"
#define SENSOR_TASK_STREAMING   "streaming"
#define SENSOR_TASK_INFERENCING "inferencing"
#define SENSOR_TASK_CALIBRATING "calibrating"

// ============================================================================
// Sensor Categories
// ============================================================================

#define SENSOR_CATEGORY_I2C     "i2c"
#define SENSOR_CATEGORY_CAMERA  "camera"
#define SENSOR_CATEGORY_AUDIO   "audio"
#define SENSOR_CATEGORY_ML      "ml"

// ============================================================================
// Non-I2C Sensor Entry
// ============================================================================
// For sensors that don't use I2C (camera, microphone, etc.)
// I2C sensors use I2CSensorEntry in System_I2C.h

typedef bool (*SensorConnectedFunc)();
typedef bool (*SensorEnabledFunc)();
typedef const char* (*SensorTaskFunc)();

struct NonI2CSensorEntry {
  const char* id;               // Unique identifier: "camera", "microphone"
  const char* displayName;      // Human-readable: "Camera (OV2640/OV3660)"
  const char* category;         // SENSOR_CATEGORY_* constant
  const char** validTasks;      // Null-terminated array of valid task strings, or nullptr
  SensorConnectedFunc getConnected;  // Returns true if hardware is available
  SensorEnabledFunc getEnabled;      // Returns true if sensor is active
  SensorTaskFunc getTask;            // Returns current task or SENSOR_TASK_NONE
  const char* mlSettingsModule;      // Name of associated ML settings module, or nullptr
};

// ============================================================================
// Non-I2C Sensor Registry
// ============================================================================

extern const NonI2CSensorEntry nonI2CSensors[];
extern const size_t nonI2CSensorsCount;

// ============================================================================
// Registry Access Functions
// ============================================================================

// Find a non-I2C sensor by ID, returns nullptr if not found
const NonI2CSensorEntry* findNonI2CSensor(const char* id);

// Initialize the sensor registry (call during setup)
void initSensorRegistry();

#endif // SYSTEM_SENSOR_REGISTRY_H
