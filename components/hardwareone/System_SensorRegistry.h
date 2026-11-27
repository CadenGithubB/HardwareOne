/**
 * Sensor Registry - Unified sensor descriptor system
 * System_SensorRegistry.h
 * 
 * Provides a unified way to register and query sensors regardless of
 * their underlying interface (I2C, DVP, PDM, etc.)
 */

#ifndef SYSTEM_SENSORREGISTRY_H
#define SYSTEM_SENSORREGISTRY_H

#include <stdint.h>
#include <stddef.h>

#include "System_ESPNow_SensorCore.h"  // RemoteSensorType: the index of kSensorSpecs (a pure header)

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
// I2C sensors use I2cSensorEntry in System_I2C.h

typedef bool (*SensorConnectedFunc)();
typedef bool (*SensorEnabledFunc)();
typedef const char* (*SensorTaskFunc)();

struct NonI2cSensorEntry {
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

extern const NonI2cSensorEntry nonI2cSensors[];
extern const size_t nonI2cSensorsCount;

// ============================================================================
// Sensor specs: what this build can do with each sensor type
// ============================================================================
// One row per RemoteSensorType. The type's number and names stay in
// System_ESPNow_SensorCore.h (espnowsensor::kSensorTable, a header with no
// firmware names in it); a row here adds the facts only a firmware build has:
// the driver's builder, its flags, its settings. A consumer reads the column
// of its surface and keeps no list of its own behind ENABLE_ conditions.
//
// A column is added with its first reader. A text that must exist for a
// sensor that is not built (a "Not compiled" row, a banner) stays with the
// surface that shows it.

// One reading as JSON text into buf. Bytes written, 0 when there is no reading.
typedef int (*SensorJsonBuilder)(char* buf, size_t bufSize);

// The reading as it travels over the mesh (System_ESPNow_Sensors.cpp).
struct SensorMeshSpec {
  SensorJsonBuilder builder;   // nullptr: this firmware sends no reading of the type
  uint16_t minIntervalMs;      // the sensor's own fastest cadence
  uint16_t bufBytes;           // room for its largest reading
};

// The type's section of the MQTT state document (System_MQTT.cpp).
// Read-only pointers: nothing writes a setting or a flag through this table.
struct SensorMqttSpec {
  SensorJsonBuilder builder;   // nullptr: no section in this firmware
  const bool* publish;         // gSettings.mqttPublish<X>; nullptr: the type has no section at all
  const bool* running;         // the section is sent only while this is set
  const bool* connected;       // nullptr: not asked; else needed as well
};

// The mesh masks a type has a bit in (SensorSpec::masks).
// kSensorInStatus: "present" and "streaming" of SENSOR_STATUS (sensorPresentMask).
// kSensorInCapability: the types with a CAP_SENSOR_ bit (System_ESPNow.h): the
// capability summary's sensorMask and the bond status enabled/connected masks.
inline constexpr uint8_t kSensorInStatus = 1u << 0;
inline constexpr uint8_t kSensorInCapability = 1u << 1;

struct SensorSpec {
  uint8_t id;                  // RemoteSensorType, the row's index
  bool built;                  // the sensor is in this build (input: any input device, ENABLE_OLED_INPUT)
  uint8_t masks;               // kSensorInStatus | kSensorInCapability; 0: in no sensor mask
  const bool* running;         // the driver is started; nullptr when masks is 0
  const bool* connected;       // the driver has its device; nullptr when masks is 0
  SensorMeshSpec mesh;
  SensorMqttSpec mqtt;
};

// A row is written once for both builds: it names the driver's builder and
// flags (the disabled branch of the driver's header has them too), and
// `built` false leaves the builder out.
constexpr SensorMeshSpec sensorMeshSpec(bool built, SensorJsonBuilder builder,
                                        uint16_t minIntervalMs, uint16_t bufBytes) {
  return built ? SensorMeshSpec{ builder, minIntervalMs, bufBytes } : SensorMeshSpec{ nullptr, 0, 0 };
}
constexpr SensorMqttSpec sensorMqttSpec(bool built, SensorJsonBuilder builder, const bool* publish,
                                        const bool* running, const bool* connected = nullptr) {
  return SensorMqttSpec{ built ? builder : nullptr, publish, running, connected };
}
inline constexpr SensorMeshSpec kSensorNoMesh = { nullptr, 0, 0 };
inline constexpr SensorMqttSpec kSensorNoMqtt = { nullptr, nullptr, nullptr, nullptr };

extern const SensorSpec kSensorSpecs[REMOTE_SENSOR_MAX];   // in RemoteSensorType order

// The types with an MQTT state section: the kSensorSpecs rows with an
// mqtt.publish setting (checked in System_SensorRegistry.cpp). The lists that
// name them by hand check themselves against it: kMqttStateSensors and
// kMqttSettingEntries (System_MQTT.cpp), kMqttPublishedSensorRows
// (WebPage_MQTT.cpp).
inline constexpr espnowsensor::SensorMask kSensorMqttTypes =
    espnowsensor::sensorBit(REMOTE_SENSOR_THERMAL) | espnowsensor::sensorBit(REMOTE_SENSOR_TOF) |
    espnowsensor::sensorBit(REMOTE_SENSOR_IMU) | espnowsensor::sensorBit(REMOTE_SENSOR_GPS) |
    espnowsensor::sensorBit(REMOTE_SENSOR_INPUT) | espnowsensor::sensorBit(REMOTE_SENSOR_RTC) |
    espnowsensor::sensorBit(REMOTE_SENSOR_PRESENCE) | espnowsensor::sensorBit(REMOTE_SENSOR_APDS);

// The number of types in `mask`.
constexpr size_t sensorMaskCount(espnowsensor::SensorMask mask) {
  size_t n = 0;
  for (; mask; mask &= mask - 1) ++n;
  return n;
}

// True when `types` holds each type of `mask` once and no other.
template <size_t N>
constexpr bool sensorTypesAre(const uint8_t (&types)[N], espnowsensor::SensorMask mask) {
  espnowsensor::SensorMask seen = 0;
  for (size_t i = 0; i < N; ++i) {
    if (types[i] >= REMOTE_SENSOR_MAX || (seen & espnowsensor::sensorBit(types[i]))) return false;
    seen |= espnowsensor::sensorBit(types[i]);
  }
  return seen == mask;
}

// ============================================================================
// Registry Access Functions
// ============================================================================

// Find a non-I2C sensor by ID, returns nullptr if not found
const NonI2cSensorEntry* findNonI2cSensor(const char* id);

// Initialize the sensor registry (call during setup)
void initSensorRegistry();

#endif // SYSTEM_SENSORREGISTRY_H
