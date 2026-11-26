/**
 * Sensor Registry - Unified sensor descriptor system
 * System_SensorRegistry.cpp
 */

#include "System_SensorRegistry.h"
#include "System_BuildConfig.h"
#include <string.h>
#include "System_Camera_DVP.h"
// The drivers kSensorSpecs names. Each header declares its builder and flags
// in both branches of its ENABLE_ flag, so none of these has a condition.
#include "System_Settings.h"
#include "HAL_Input.h"             // gInputRunning/gInputConnected and the four input headers
#include "HAL_Motion.h"
#include "i2csensor_apds9960.h"
#include "i2csensor_ds3231.h"
#include "i2csensor_mlx90640.h"
#include "i2csensor_pa1010d.h"
#include "i2csensor_rda5807.h"
#include "i2csensor_sths34pf80.h"
#include "i2csensor_vl53l4cx.h"

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
// Sensor specs (System_SensorRegistry.h)
// ============================================================================

// SENSOR_SPECS_HOST_BEGIN (the host suite sensor_specs compiles from here)
// The build's one input device. Four of them have a reading, each its own
// builder with no disabled branch; GT911 touch has none. Picked here only.
#if ENABLE_GAMEPAD_SENSOR
static constexpr SensorJsonBuilder kInputBuilder = gamepadBuildDataJson;
static constexpr uint16_t kInputBufBytes = 128;
#elif ENABLE_ANO_ENCODER
// {"valid":..,"connected":..,"ts":..,"pos":N,"axis":0,"buttons":B}, about 90 bytes.
static constexpr SensorJsonBuilder kInputBuilder = anoEncoderBuildDataJson;
static constexpr uint16_t kInputBufBytes = 128;
#elif ENABLE_GPIO_ENCODER
static constexpr SensorJsonBuilder kInputBuilder = gpioEncoderBuildDataJson;
static constexpr uint16_t kInputBufBytes = 256;
#elif ENABLE_IMU_INPUT
// The gamepad's shape and button layout ({"x","y","buttons"}): a peer draws
// it with its gamepad widget.
static constexpr SensorJsonBuilder kInputBuilder = imuInputBuildDataJson;
static constexpr uint16_t kInputBufBytes = 128;
#else
static constexpr SensorJsonBuilder kInputBuilder = nullptr;
static constexpr uint16_t kInputBufBytes = 0;
#endif
static constexpr bool kInputHasReading =
    ENABLE_GAMEPAD_SENSOR || ENABLE_ANO_ENCODER || ENABLE_GPIO_ENCODER || ENABLE_IMU_INPUT;

// The mesh masks read `running` and `connected` (System_ESPNow.cpp,
// System_ESPNow_Sensors.cpp); apds is in the capability masks only, by its
// gApdsRunning latch, not the colour mode the MQTT section reads.
static constexpr uint8_t kInEveryMask = kSensorInStatus | kSensorInCapability;

constexpr SensorSpec kSensorSpecs[REMOTE_SENSOR_MAX] = {
  // No mesh reading: the 768-pixel frame is far over the largest reading
  // (536 bytes, System_ESPNow_SensorCore.h). To send thermal over the mesh,
  // give it thermalBuildSummaryJson (about 90 bytes).
  // MQTT: the summary (min/avg/max), never the frame.
  { REMOTE_SENSOR_THERMAL, ENABLE_THERMAL_SENSOR, kInEveryMask, &gThermalRunning, &gThermalConnected,
    kSensorNoMesh,
    sensorMqttSpec(ENABLE_THERMAL_SENSOR, thermalBuildSummaryJson, &gSettings.mqttPublishThermal,
                   &gThermalRunning) },
  { REMOTE_SENSOR_TOF, ENABLE_TOF_SENSOR, kInEveryMask, &gTofRunning, &gTofConnected,
    sensorMeshSpec(ENABLE_TOF_SENSOR, tofBuildDataJson, 500, 1024),
    sensorMqttSpec(ENABLE_TOF_SENSOR, tofBuildDataJson, &gSettings.mqttPublishTof, &gTofRunning) },
  { REMOTE_SENSOR_IMU, ENABLE_IMU_SENSOR, kInEveryMask, &gImuRunning, &gImuConnected,
    sensorMeshSpec(ENABLE_IMU_SENSOR, imuBuildDataJson, 500, 512),
    sensorMqttSpec(ENABLE_IMU_SENSOR, imuBuildDataJson, &gSettings.mqttPublishImu, &gImuRunning) },
  { REMOTE_SENSOR_GPS, ENABLE_GPS_SENSOR, kInEveryMask, &gGpsRunning, &gGpsConnected,
    sensorMeshSpec(ENABLE_GPS_SENSOR, gpsBuildDataJson, 1000, 256),
    sensorMqttSpec(ENABLE_GPS_SENSOR, gpsBuildDataJson, &gSettings.mqttPublishGps,
                   &gGpsRunning, &gGpsConnected) },
  // Built with any input device, GT911 touch too; a reading only with the
  // four of kInputBuilder. 10 Hz for a snappy button press.
  { REMOTE_SENSOR_INPUT, ENABLE_OLED_INPUT, kInEveryMask, &gInputRunning, &gInputConnected,
    sensorMeshSpec(kInputHasReading, kInputBuilder, 100, kInputBufBytes),
    sensorMqttSpec(kInputHasReading, kInputBuilder, &gSettings.mqttPublishInput,
                   &gInputRunning, &gInputConnected) },
  { REMOTE_SENSOR_FMRADIO, ENABLE_FM_RADIO, kInEveryMask, &gFmRadioRunning, &gFmRadioConnected,
    sensorMeshSpec(ENABLE_FM_RADIO, fmRadioBuildDataJson, 1000, 512), kSensorNoMqtt },
  // Camera and microphone are feature bits (CAP_FEATURE_), in no sensor mask.
  { REMOTE_SENSOR_CAMERA, ENABLE_CAMERA_SENSOR, 0, nullptr, nullptr, kSensorNoMesh, kSensorNoMqtt },
  { REMOTE_SENSOR_MICROPHONE, ENABLE_MICROPHONE, 0, nullptr, nullptr, kSensorNoMesh, kSensorNoMqtt },
  // MQTT adds "datetime" (UTC, with Z) from the same reading's fields.
  { REMOTE_SENSOR_RTC, ENABLE_RTC_SENSOR, kInEveryMask, &gRtcRunning, &gRtcConnected,
    sensorMeshSpec(ENABLE_RTC_SENSOR, rtcBuildDataJson, 1000, 256),
    sensorMqttSpec(ENABLE_RTC_SENSOR, rtcBuildDataJson, &gSettings.mqttPublishRtc,
                   &gRtcRunning, &gRtcConnected) },
  { REMOTE_SENSOR_PRESENCE, ENABLE_PRESENCE_SENSOR, kInEveryMask, &gPresenceRunning, &gPresenceConnected,
    sensorMeshSpec(ENABLE_PRESENCE_SENSOR, presenceBuildDataJson, 500, 256),
    sensorMqttSpec(ENABLE_PRESENCE_SENSOR, presenceBuildDataJson, &gSettings.mqttPublishPresence,
                   &gPresenceRunning) },
  // The colour mode is what the section reads (apdsBuildDataJson).
  { REMOTE_SENSOR_APDS, ENABLE_APDS_SENSOR, kSensorInCapability, &gApdsRunning, &gApdsConnected,
    kSensorNoMesh,
    sensorMqttSpec(ENABLE_APDS_SENSOR, apdsBuildDataJson, &gSettings.mqttPublishApds,
                   &gApdsColorRunning, &gApdsConnected) },
};

// A row has an mqtt.publish setting when its type is in kSensorMqttTypes.
static constexpr bool sensorMqttTypesMatch() {
  for (uint8_t i = 0; i < REMOTE_SENSOR_MAX; ++i) {
    const bool listed = (kSensorMqttTypes & espnowsensor::sensorBit(i)) != 0;
    if ((kSensorSpecs[i].mqtt.publish != nullptr) != listed) return false;
  }
  return true;
}
static_assert(sensorMqttTypesMatch(),
              "kSensorSpecs: the rows with an mqtt.publish setting are not kSensorMqttTypes "
              "(System_SensorRegistry.h). Change both, then kMqttStateSensors, the mqttPublish<X> rows of "
              "mqttSettingEntries, MQTT_PUBLISH_CMD and mqttCommands (System_MQTT.cpp) and "
              "kMqttPublishedSensorRows (WebPage_MQTT.cpp)");
// SENSOR_SPECS_HOST_END

constexpr bool sensorSpecsInOrder() {
  for (uint8_t i = 0; i < REMOTE_SENSOR_MAX; ++i) {
    if (kSensorSpecs[i].id != i) return false;
  }
  return true;
}
static_assert(sensorSpecsInOrder(), "kSensorSpecs: one row per RemoteSensorType, in its order");

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
