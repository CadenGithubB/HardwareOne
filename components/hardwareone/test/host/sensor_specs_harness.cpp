// Host harness for kSensorSpecs (System_SensorRegistry.cpp): one row per
// sensor type with its build and mesh mask columns, its mesh reading and its
// MQTT state section.
// test_sensor_specs.py builds this file twice and links the two units, once
// per build profile (ENABLE_ flags on the command line):
//   SPECS_UNIT: the production text between the SENSOR_SPECS_HOST_BEGIN and
//     _END marks and the row order static_assert, with the production
//     System_SensorRegistry.h (and through it System_ESPNow_SensorCore.h);
//   otherwise: the stand-ins and main, which prints every row by name.
// Linking the two shows the table keeps the header's external linkage.
// Builders, flags and gSettings are stand-ins with their production names;
// each has its own address, so a row says which one it holds.
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "System_SensorRegistry.h"

#define HOST_BUILDERS(X)                                                                   \
  X(thermalBuildDataJson) X(thermalBuildSummaryJson) X(tofBuildDataJson) X(imuBuildDataJson) \
  X(gpsBuildDataJson) X(gamepadBuildDataJson) X(anoEncoderBuildDataJson)                    \
  X(gpioEncoderBuildDataJson) X(imuInputBuildDataJson) X(fmRadioBuildDataJson)              \
  X(rtcBuildDataJson) X(presenceBuildDataJson) X(apdsBuildDataJson)
#define HOST_FLAGS(X)                                                                      \
  X(gThermalRunning) X(gThermalConnected) X(gTofRunning) X(gTofConnected) X(gImuRunning)    \
  X(gImuConnected) X(gGpsRunning) X(gGpsConnected) X(gInputRunning) X(gInputConnected)      \
  X(gAnoEncoderConnected) X(gFmRadioRunning) X(gFmRadioConnected) X(gRtcRunning)            \
  X(gRtcConnected) X(gPresenceRunning) X(gPresenceConnected) X(gApdsRunning)                \
  X(gApdsColorRunning) X(gApdsProximityRunning) X(gApdsGestureRunning) X(gApdsConnected)
#define HOST_SETTINGS(X)                                                                   \
  X(mqttPublishWifi) X(mqttPublishSystem) X(mqttPublishThermal) X(mqttPublishTof)           \
  X(mqttPublishImu) X(mqttPublishPresence) X(mqttPublishGps) X(mqttPublishApds)             \
  X(mqttPublishRtc) X(mqttPublishInput)

#define DECLARE_BUILDER(name) int name(char* buf, size_t bufSize);
#define DECLARE_FLAG(name) extern bool name;
#define SETTING_FIELD(name) bool name = false;
HOST_BUILDERS(DECLARE_BUILDER)
HOST_FLAGS(DECLARE_FLAG)
struct HostSettings { HOST_SETTINGS(SETTING_FIELD) };
extern HostSettings gSettings;

#if defined(SPECS_UNIT)
// INSERT_SPECS
#else
#define DEFINE_BUILDER(name) int name(char*, size_t) { return 0; }
#define DEFINE_FLAG(name) bool name = false;
HOST_BUILDERS(DEFINE_BUILDER)
HOST_FLAGS(DEFINE_FLAG)
HostSettings gSettings;

static const char* builderName(SensorJsonBuilder builder) {
  if (!builder) return "null";
#define NAME_BUILDER(name) if (builder == &name) return #name;
  HOST_BUILDERS(NAME_BUILDER)
  return "unknown";
}
static const char* masksName(uint8_t masks) {
  switch (masks) {
    case 0: return "none";
    case kSensorInStatus: return "status";
    case kSensorInCapability: return "capability";
    case kSensorInStatus | kSensorInCapability: return "status+capability";
    default: return "unknown";
  }
}
static const char* flagName(const bool* flag) {
  if (!flag) return "null";
#define NAME_FLAG(name) if (flag == &name) return #name;
  HOST_FLAGS(NAME_FLAG)
#define NAME_SETTING(name) if (flag == &gSettings.name) return #name;
  HOST_SETTINGS(NAME_SETTING)
  return "unknown";
}

int main() {
  for (int i = 0; i < REMOTE_SENSOR_MAX; ++i) {
    const SensorSpec& row = kSensorSpecs[i];
    std::printf("%s id=%u built=%d masks=%s running=%s connected=%s mesh=%s,%u,%u mqtt=%s,%s,%s,%s\n",
                espnowsensor::sensorName(i), (unsigned)row.id, (int)row.built, masksName(row.masks),
                flagName(row.running), flagName(row.connected),
                builderName(row.mesh.builder), (unsigned)row.mesh.minIntervalMs, (unsigned)row.mesh.bufBytes,
                builderName(row.mqtt.builder), flagName(row.mqtt.publish), flagName(row.mqtt.running),
                flagName(row.mqtt.connected));
  }
  return 0;
}
#endif
