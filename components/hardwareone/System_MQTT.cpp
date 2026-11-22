/**
 * System_MQTT - Home Assistant MQTT Integration
 * 
 * Publishes sensor data to MQTT broker for Home Assistant integration.
 * Uses ESP-IDF's esp_mqtt_client for async connection management.
 */

#include "System_MQTT.h"
#include "System_Events.h"  // systemEventPost — event register producer
#include "System_Notifications.h"  // notifyLoginFailed — auth-failure parity
#include "System_BuildConfig.h"
#include <esp_attr.h>       // EXT_RAM_BSS_ATTR (PSRAM .bss staging buffers)

#if ENABLE_WIFI && ENABLE_MQTT

#include <Arduino.h>
#include <WiFi.h>
#include <FS.h>
#include "System_VFS.h"   // VFS::*Guarded + systemAuth (Phase 2 perm refactor)
#include "System_Mutex.h"
#include "System_AuthIdentity.h"
#include "System_CommandTypes.h"  // CMD_RESULT_MAX, executeCommand result cap
#include <mqtt_client.h>
#include <new>
#include "System_Settings.h"
#include "System_SelfDevice.h"
#include "System_Debug.h"
#include "System_Command.h"
#include "System_MemUtil.h"
#include "System_PsramJson.h"  // PsramJsonText: the mesh peer state documents
#include "System_CommandLimits.h"  // the client buffers match the command result ceiling
#include "System_MQTTLifecycleGate.h"
#include "System_MQTTAdmission.h"
#include "System_MQTTControlTopic.h"
#include "System_User.h"
#include <cstdarg>
#include <utility>
#include "System_Utils.h"
#include <ArduinoJson.h>
#include "System_CommandRegistry.h"
#include "System_Clock.h"            // the state document's "time" (UTC)
#include "System_MQTTPayloadCore.h"  // state and discovery documents, HA entity table
#include <esp_heap_caps.h>          // PSRAM total for the state document

#if ENABLE_ESPNOW
#include "System_ESPNow.h"   // espnowPeerState: the peer states every surface shows
#include "System_ESPNow_Sensors.h"
#endif

#include "i2csensor_pa1010d.h"

#include "i2csensor_sths34pf80.h"

#include "i2csensor_mlx90640.h"

#include "i2csensor_vl53l4cx.h"

#include "i2csensor_bno055.h"

#include "i2csensor_apds9960.h"

#include "i2csensor_ds3231.h"

#if ENABLE_OLED_INPUT
#include "HAL_Input.h"   // gInputRunning/gInputConnected and the input builders
#endif

// Forward declarations

// MQTT state
static esp_mqtt_client_handle_t mqttClient = nullptr;
static std::atomic<bool> mqttTofConnected{false};
static std::atomic<bool> mqttClientRunning{false};
static unsigned long lastPublishTime = 0;
static String lastError = "";
static MqttLifecycleGate sMqttLifecycle;
// ESP-MQTT emits BEFORE_CONNECT only after its worker has completed the two
// early setup steps that can make the task exit autonomously. This marker lets
// stop cleanup distinguish that early self-exit from a live worker safely.
static std::atomic<bool> sMqttBeforeConnectSeen{false};

// Holds one admission while non-lifecycle code may use mqttClient. For command
// callbacks this begins before parsing; a queued command moves it into its
// cmd_exec completion, which releases it only after the response has been
// enqueued, and every other response path publishes before it is released.
class MqttClientUse final : public MqttAdmission {
 public:
  MqttClientUse() : MqttAdmission(sMqttLifecycle) {}
};

// Ensures every early start failure re-closes the lifecycle gate.  A stop
// request racing startup is preserved by finishStart(true), which leaves the
// new client closed and pending for main-loop teardown.
class MqttStartAttempt final {
 public:
  MqttStartAttempt() = default;

  ~MqttStartAttempt() {
    if (!finished_) (void)sMqttLifecycle.finishStart(false);
  }

  MqttStartAttempt(const MqttStartAttempt&) = delete;
  MqttStartAttempt& operator=(const MqttStartAttempt&) = delete;

  bool finishStarted() {
    finished_ = true;
    return sMqttLifecycle.finishStart(true);
  }

 private:
  bool finished_ = false;
};

static String redactMqttCommandResult(const String& output) {
  String safe = redactOutputForLog(output);
  static const char kUnknownPrefix[] = "Unknown command: ";
  if (!safe.startsWith(kUnknownPrefix)) return safe;
  const int lineEnd = safe.indexOf('\n');
  const int commandStart = strlen(kUnknownPrefix);
  const String echoed = lineEnd >= 0
                            ? safe.substring(commandStart, lineEnd)
                            : safe.substring(commandStart);
  String rebuilt = String(kUnknownPrefix) + redactCmdForAudit(echoed);
  if (lineEnd >= 0) rebuilt += safe.substring(lineEnd);
  return rebuilt;
}

// ============================================================================
// External Sensor Storage
// ============================================================================

struct ExternalSensor {
  String topic;       // Full topic path
  String name;        // Friendly name (derived from topic)
  String value;       // Last received value (JSON or simple)
  unsigned long lastUpdate;  // millis() of last update
};

static const int MAX_EXTERNAL_SENSORS = 32;
static ExternalSensor* externalSensors = nullptr;
static int externalSensorCount = 0;
static SemaphoreHandle_t externalSensorMutex = nullptr;

// Base topic the command subscription was made with. The broker keeps
// delivering that command topic until the next connect, also after
// `mqttbasetopic` changed the setting. Written (CONNECTED) and read (DATA) on
// the esp-mqtt task only.
static String sMqttSubscribedBaseTopic;

// True for the command and response topics of this client: of the base topic
// in the settings (what handleMqttCommand executes) and of the base topic the
// live subscription was made with.
static bool mqttIsOwnControlTopic(const char* topic, size_t topicLen) {
  return mqttIsControlTopic(topic, topicLen, gSettings.mqttBaseTopic.c_str(),
                            gSettings.mqttBaseTopic.length()) ||
         mqttIsControlTopic(topic, topicLen, sMqttSubscribedBaseTopic.c_str(),
                            sMqttSubscribedBaseTopic.length());
}

static bool initExternalSensorStorage() {
  if (!externalSensorMutex) {
    externalSensorMutex = xSemaphoreCreateMutex();
  }
  if (!externalSensorMutex) return false;

  if (externalSensors) return true;

  void* storage = ps_alloc(sizeof(ExternalSensor) * MAX_EXTERNAL_SENSORS,
                           AllocPref::RequirePsram, "mqtt.sensor.table");
  if (!storage) {
    ERROR_MQTTF("Failed to allocate external sensor table (%u bytes)",
                (unsigned)(sizeof(ExternalSensor) * MAX_EXTERNAL_SENSORS));
    return false;
  }

  ExternalSensor* sensors = (ExternalSensor*)storage;
  for (int i = 0; i < MAX_EXTERNAL_SENSORS; i++) {
    new (&sensors[i]) ExternalSensor();
  }
  externalSensors = sensors;
  externalSensorCount = 0;
  return true;
}

// The state document's 16 KB buffer. publishMqttSensorData makes it at its
// first publish and keeps it, only in PSRAM, until stopMqttNow releases it;
// both run on the main loop (mqttTick), and a publish holds an MqttClientUse,
// which the stop waits out.
static char* sMqttStateJson = nullptr;
static void releaseMqttStateJson() {
  free(sMqttStateJson);
  sMqttStateJson = nullptr;
}

static void releaseExternalSensorStorage() {
  if (!externalSensorMutex) return;

  ExternalSensor* sensors = nullptr;
  if (xSemaphoreTake(externalSensorMutex, pdMS_TO_TICKS(250)) == pdTRUE) {
    sensors = externalSensors;
    externalSensors = nullptr;
    externalSensorCount = 0;
    xSemaphoreGive(externalSensorMutex);
  } else {
    WARN_MQTTF("External sensor storage release skipped: mutex timeout");
    return;
  }

  if (sensors) {
    for (int i = 0; i < MAX_EXTERNAL_SENSORS; i++) {
      sensors[i].~ExternalSensor();
    }
    free(sensors);
  }
}

static void updateExternalSensor(const char* topic, int topicLen, const char* data, int dataLen) {
  if (!externalSensorMutex || !externalSensors) return;
  if (!topic || topicLen <= 0) return;
  // The command topic's payload is the {user,pass,cmd} envelope and the
  // response topic's is command output. `mqttexternalsensors` needs no admin
  // and the MQTT page shows the table, so neither is ever stored.
  if (mqttIsOwnControlTopic(topic, (size_t)topicLen)) return;
  if (!data || dataLen < 0) dataLen = 0;

  // esp-mqtt hands over length-delimited buffers with no terminating NUL.
  String topicStr(topic, (unsigned int)topicLen);
  String dataStr(dataLen > 0 ? data : "", (unsigned int)dataLen);
  
  if (xSemaphoreTake(externalSensorMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (!externalSensors) {
      xSemaphoreGive(externalSensorMutex);
      return;
    }
    // Look for existing sensor
    for (int i = 0; i < externalSensorCount; i++) {
      if (externalSensors[i].topic == topicStr) {
        externalSensors[i].value = dataStr;
        externalSensors[i].lastUpdate = millis();
        xSemaphoreGive(externalSensorMutex);
        DEBUG_MQTT_PUBSUBF("Updated sensor: %s", topicStr.c_str());
        return;
      }
    }
    
    // Add new sensor if space available
    if (externalSensorCount < MAX_EXTERNAL_SENSORS) {
      externalSensors[externalSensorCount].topic = topicStr;
      // Extract friendly name from topic (last segment)
      int lastSlash = topicStr.lastIndexOf('/');
      externalSensors[externalSensorCount].name = (lastSlash >= 0) ? 
        topicStr.substring(lastSlash + 1) : topicStr;
      externalSensors[externalSensorCount].value = dataStr;
      externalSensors[externalSensorCount].lastUpdate = millis();
      externalSensorCount++;
      INFO_MQTT_PUBSUBF("New external sensor: %s", topicStr.c_str());
      systemEventPost(SYSEVT_MQTT_EXT_SENSOR_NEW, topicStr.c_str());
    }
    xSemaphoreGive(externalSensorMutex);
  }
}

static void subscribeToExternalTopics() {
  if (!mqttClient || !gSettings.mqttSubscribeExternal) return;
  if (gSettings.mqttSubscribeTopics.length() == 0) return;
  
  String topics = gSettings.mqttSubscribeTopics;
  int start = 0;
  
  while (start < (int)topics.length()) {
    int comma = topics.indexOf(',', start);
    if (comma < 0) comma = topics.length();
    
    String topic = topics.substring(start, comma);
    topic.trim();
    
    if (topic.length() > 0) {
      int msgId = esp_mqtt_client_subscribe(mqttClient, topic.c_str(), 0);
      if (msgId >= 0) {
        INFO_MQTT_PUBSUBF("Subscribed to: %s", topic.c_str());
      } else {
        WARN_MQTTF("Failed to subscribe: %s", topic.c_str());
      }
    }
    
    start = comma + 1;
  }
}

// Check if MQTT is connected
bool isMqttConnected() {
  return mqttTofConnected.load(std::memory_order_acquire);
}

// Check if the MQTT client is started (not necessarily connected to the broker)
bool isMqttStarted() {
  return mqttClientRunning.load(std::memory_order_acquire);
}

// Get external sensor count
int getExternalSensorCount() {
  if (!externalSensorMutex || !externalSensors) return 0;
  int count = 0;
  if (xSemaphoreTake(externalSensorMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    count = externalSensorCount;
    xSemaphoreGive(externalSensorMutex);
  }
  return count;
}

// Get external sensor data (thread-safe)
bool getExternalSensor(int index, String& topic, String& name, String& value, unsigned long& lastUpdate) {
  if (!externalSensorMutex || !externalSensors || index < 0) return false;
  
  if (xSemaphoreTake(externalSensorMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (!externalSensors || index >= externalSensorCount) {
      xSemaphoreGive(externalSensorMutex);
      return false;
    }
    topic = externalSensors[index].topic;
    name = externalSensors[index].name;
    value = externalSensors[index].value;
    lastUpdate = externalSensors[index].lastUpdate;
    xSemaphoreGive(externalSensorMutex);
    return true;
  }
  return false;
}

// ============================================================================
// MQTT Settings Module
// ============================================================================

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry mqttSettingEntries[] = {
  { "mqttAutoStart", SETTING_BOOL, &gSettings.mqttAutoStart, false, 0, nullptr, 0, 1, "Auto-start at boot", nullptr, false, "broker", "mqttautostart" },
  { "mqttHost", SETTING_STRING, &gSettings.mqttHost, 0, 0, "", 0, 0, "Broker Host", nullptr, false, "broker", "mqtthost" },
  { "mqttPort", SETTING_INT, &gSettings.mqttPort, 1883, 0, nullptr, 1, 65535, "Broker Port", nullptr, false, "broker", "mqttport" },
  { "mqttTLSMode", SETTING_INT, &gSettings.mqttTlsMode, 0, 0, nullptr, 0, 2, "TLS Mode (0=None, 1=TLS, 2=TLS+Verify)", "0:None,1:TLS,2:TLS+Verify", false, "broker", "mqtttlsmode" },
  { "mqttCACertPath", SETTING_STRING, &gSettings.mqttCaCertPath, 0, 0, "/system/certs/mqtt_ca.crt", 0, 0, "CA certificate path", nullptr, false, "broker", "mqttcacertpath" },
  { "mqttSubscribeExternal", SETTING_BOOL, &gSettings.mqttSubscribeExternal, false, 0, nullptr, 0, 1, "Subscribe to external topics", nullptr, false, "topics", "mqttsubscribeexternal" },
  { "mqttSubscribeTopics", SETTING_STRING, &gSettings.mqttSubscribeTopics, 0, 0, "", 0, 0, "Topics (comma-separated)", nullptr, false, "topics", "mqttsubscribetopics" },
  { "mqttUser", SETTING_STRING, &gSettings.mqttUser, 0, 0, "", 0, 0, "Username", nullptr, false, "broker", "mqttuser" },
  { "mqttPassword", SETTING_STRING, &gSettings.mqttPassword, 0, 0, "", 0, 0, "Password", nullptr, true, "broker", "mqttpassword" },
  { "mqttBaseTopic", SETTING_STRING, &gSettings.mqttBaseTopic, 0, 0, "", 0, 0, "Base Topic", nullptr, false, "topics", "mqttbasetopic" },
  { "mqttDiscoveryPrefix", SETTING_STRING, &gSettings.mqttDiscoveryPrefix, 0, 0, "homeassistant", 0, 0, "Discovery Prefix", nullptr, false, "topics", "mqttdiscoveryprefix" },
  { "mqttPublishIntervalMs", SETTING_INT, &gSettings.mqttPublishIntervalMs, 30000, 0, nullptr, 1000, 300000, "Publish Interval (ms)", nullptr, false, "publish", "mqttpublishintervalms" },
  { "mqttPublishWiFi", SETTING_BOOL, &gSettings.mqttPublishWifi, false, 0, nullptr, 0, 1, "Publish WiFi info", nullptr, false, "publish", "mqttpublishwifi" },
  { "mqttPublishSystem", SETTING_BOOL, &gSettings.mqttPublishSystem, false, 0, nullptr, 0, 1, "Publish system info", nullptr, false, "publish", "mqttpublishsystem" },
  { "mqttPublishThermal", SETTING_BOOL, &gSettings.mqttPublishThermal, false, 0, nullptr, 0, 1, "Publish thermal data", nullptr, false, "publish", "mqttpublishthermal" },
  { "mqttPublishToF", SETTING_BOOL, &gSettings.mqttPublishTof, false, 0, nullptr, 0, 1, "Publish ToF data", nullptr, false, "publish", "mqttpublishtof" },
  { "mqttPublishIMU", SETTING_BOOL, &gSettings.mqttPublishImu, false, 0, nullptr, 0, 1, "Publish IMU data", nullptr, false, "publish", "mqttpublishimu" },
  { "mqttPublishPresence", SETTING_BOOL, &gSettings.mqttPublishPresence, false, 0, nullptr, 0, 1, "Publish presence data", nullptr, false, "publish", "mqttpublishpresence" },
  { "mqttPublishGPS", SETTING_BOOL, &gSettings.mqttPublishGps, false, 0, nullptr, 0, 1, "Publish GPS data", nullptr, false, "publish", "mqttpublishgps" },
  { "mqttPublishAPDS", SETTING_BOOL, &gSettings.mqttPublishApds, false, 0, nullptr, 0, 1, "Publish APDS data", nullptr, false, "publish", "mqttpublishapds" },
  { "mqttPublishRTC", SETTING_BOOL, &gSettings.mqttPublishRtc, false, 0, nullptr, 0, 1, "Publish RTC time", nullptr, false, "publish", "mqttpublishrtc" },
  { "mqttPublishInput", SETTING_BOOL, &gSettings.mqttPublishInput, false, 0, nullptr, 0, 1, "Publish input device data", nullptr, false, "publish", "mqttpublishinput" }
};

static bool isMqttAvailable() {
  return WiFi.isConnected();
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule mqttSettingsModule = {
  "mqtt",
  "network.mqtt",
  mqttSettingEntries,
  sizeof(mqttSettingEntries) / sizeof(mqttSettingEntries[0]),
  isMqttAvailable,
  "MQTT broker connection for Home Assistant integration"
};

// ============================================================================
// Home Assistant MQTT Discovery
// ============================================================================

// Get device identifier (MAC-based): "hardwareone_<12 hex>" from the station
// address, kept per boot by SelfDevice (on the P4X a WiFi.macAddress() call is
// a round trip to the C6, and discovery asks once per entity).
static String getDeviceId() {
  uint8_t mac[6] = {0, 0, 0, 0, 0, 0};
  (void)SelfDevice::macBytes(mac);
  char macStr[13];
  macToPathToken(mac, macStr);  // no-separator UPPER (System_Utils.h)
  char idBuf[26];
  snprintf(idBuf, sizeof(idBuf), "hardwareone_%s", macStr);
  return String(idBuf);
}

// PSRAM staging buffer shared by publishDiscoveryConfig() and
// publishPeerDiscoveryConfig(). ONE buffer serves both because they can never be
// live at the same time:
//
//   MQTT_EVENT_CONNECTED (the ONLY caller of publishMqttDiscovery)
//     └─ publishMqttDiscovery()
//          ├─ publishDiscoveryConfig(...)      once per entity, each frame opens and closes
//          └─ publishMeshPeerDiscovery()       runs only AFTER all of the above returned
//               └─ publishPeerDiscoveryConfig(...)  x(peers x sensors)
//
// Neither function calls the other, so at most one is on the stack at any instant.
// Non-reentrant for a stronger reason than that, though: esp-mqtt dispatches EVERY
// event (CONNECTED, DATA, ...) to the single mqtt_event_handler on its own task,
// strictly sequentially — inbound broker data arriving mid-publish queues behind the
// handler rather than preempting it, so two tasks can never be in here at once.
//
// Static rather than per-call heap: publishDiscoveryConfig runs ~40x back-to-back,
// so a malloc/free per call would be pure overhead. Keeps 1 KB off the esp-mqtt
// task's stack (and costs 1 KB of PSRAM, not 2).
static constexpr size_t kMqttDiscoveryJsonCap = mqttpayload::kDiscoveryCap;
EXT_RAM_BSS_ATTR static char sMqttDiscoveryConfigJson[kMqttDiscoveryJsonCap];

// What every discovery config of this device shares, worked out once per
// connect by publishMqttDiscovery.
struct MqttLocalDiscovery {
  String deviceId;
  String stateTopic;
  String availTopic;
  String deviceName;  // the HA device name
};

static MqttLocalDiscovery mqttLocalDiscovery() {
  MqttLocalDiscovery local;
  local.deviceId = getDeviceId();
  local.stateTopic = gSettings.mqttBaseTopic + "/state";
  local.availTopic = gSettings.mqttBaseTopic + "/availability";
  // The friendly name when one is set, else the device name in use: the
  // stored name, or the fallback "HardwareOne-B2C3" (bare: HA makes entity
  // ids from the device name the first time it sees it).
  local.deviceName = gSettings.espnowFriendlyName.length() > 0 ? gSettings.espnowFriendlyName
                                                               : SelfDevice::deviceName();
  return local;
}

// Publish one HA discovery config (retained, so HA picks it up after a
// restart). The document is built with ArduinoJson (System_MQTTPayloadCore.h),
// so names and rooms are escaped; one that does not fit the 1 KB buffer (a
// long mqttBaseTopic appears twice in it) is skipped, never cut.
static void publishDiscoveryConfig(const mqttpayload::HaEntity& entity,
                                   const MqttLocalDiscovery& local) {
  if (!mqttClient ||
      !mqttTofConnected.load(std::memory_order_acquire)) return;

  String uniqueId = local.deviceId + "_" + entity.objectId;

  // Build discovery topic: homeassistant/<component>/<device_id>/<object_id>/config
  String discoveryTopic = gSettings.mqttDiscoveryPrefix + "/" + entity.component + "/" +
                          local.deviceId + "/" + entity.objectId + "/config";

  mqttpayload::DeviceInfo device;
  device.id = local.deviceId.c_str();
  device.name = local.deviceName.c_str();
  device.model = BOARD_NAME;
  device.swVersion = SelfDevice::firmwareVersion();
  device.area = gSettings.espnowRoom.c_str();  // HA auto-assigns the device to this room

  // Built in the shared PSRAM buffer — see sMqttDiscoveryConfigJson for why
  // sharing is safe.
  PSRAM_JSON_DOC(doc);
  const size_t len =
      mqttpayload::buildDiscovery(entity, uniqueId.c_str(), local.stateTopic.c_str(),
                                  local.availTopic.c_str(), device, doc)
          ? mqttpayload::serializeBounded(doc, sMqttDiscoveryConfigJson, kMqttDiscoveryJsonCap)
          : 0;
  if (len == 0) {
    WARN_MQTTF("Discovery config for %s exceeds %u bytes; not published",
               entity.objectId, (unsigned)kMqttDiscoveryJsonCap);
    return;
  }

  esp_mqtt_client_publish(mqttClient, discoveryTopic.c_str(), sMqttDiscoveryConfigJson,
                          (int)len, 1, true);
  DEBUG_MQTT_DISCOVERYF("Discovery: %s", entity.objectId);
}

// Clear the retained config of an entity this firmware no longer declares
// (an empty retained payload), so HA removes it. `deviceId` is this device's
// or a peer's id.
static void publishRetiredDiscovery(const mqttpayload::RetiredEntity& retired,
                                    const String& deviceId) {
  if (!mqttClient ||
      !mqttTofConnected.load(std::memory_order_acquire)) return;
  String discoveryTopic = gSettings.mqttDiscoveryPrefix + "/" + retired.component + "/" +
                          deviceId + "/" + retired.objectId + "/config";
  esp_mqtt_client_publish(mqttClient, discoveryTopic.c_str(), "", 0, 1, true);
  DEBUG_MQTT_DISCOVERYF("Discovery retired: %s", retired.objectId);
}

// The state sections this build has (mqttpayload::Section bits).
static uint16_t mqttCompiledSections() {
  uint16_t sections = mqttpayload::kSystem | mqttpayload::kWifi;
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) sections |= mqttpayload::kPsram;
#if ENABLE_THERMAL_SENSOR
  sections |= mqttpayload::kThermal;
#endif
#if ENABLE_TOF_SENSOR
  sections |= mqttpayload::kTof;
#endif
#if ENABLE_IMU_SENSOR
  sections |= mqttpayload::kImu;
#endif
#if ENABLE_PRESENCE_SENSOR
  sections |= mqttpayload::kPresence;
#endif
#if ENABLE_GPS_SENSOR
  sections |= mqttpayload::kGps;
#endif
#if ENABLE_APDS_SENSOR
  sections |= mqttpayload::kApds;
#endif
#if ENABLE_RTC_SENSOR
  sections |= mqttpayload::kRtc;
#endif
  // The input builder ladder of System_ESPNow_Sensors.cpp (one device per
  // build; GT911 touch has no builder).
#if ENABLE_GAMEPAD_SENSOR
  sections |= mqttpayload::kInputPad;
#elif ENABLE_ANO_ENCODER || ENABLE_GPIO_ENCODER
  sections |= mqttpayload::kInputEncoder;
#endif
#if ENABLE_OLED_INPUT
  sections |= mqttpayload::kInputLegacy;
#endif
  return sections;
}

// The sections the mqttPublish* settings switch on.
static uint16_t mqttSwitchedOnSections() {
  uint16_t sections = 0;
  if (gSettings.mqttPublishSystem) sections |= mqttpayload::kSystem | mqttpayload::kPsram;
  if (gSettings.mqttPublishWifi) sections |= mqttpayload::kWifi;
  if (gSettings.mqttPublishThermal) sections |= mqttpayload::kThermal;
  if (gSettings.mqttPublishTof) sections |= mqttpayload::kTof;
  if (gSettings.mqttPublishImu) sections |= mqttpayload::kImu;
  if (gSettings.mqttPublishPresence) sections |= mqttpayload::kPresence;
  if (gSettings.mqttPublishGps) sections |= mqttpayload::kGps;
  if (gSettings.mqttPublishApds) sections |= mqttpayload::kApds;
  if (gSettings.mqttPublishRtc) sections |= mqttpayload::kRtc;
  if (gSettings.mqttPublishInput) {
    sections |= mqttpayload::kInputPad | mqttpayload::kInputEncoder | mqttpayload::kInputLegacy;
  }
  return sections;
}

// Subscribe to command topic for receiving commands from HA
static void subscribeToCommandTopic() {
  if (!mqttClient ||
      !mqttTofConnected.load(std::memory_order_acquire)) return;
  
  sMqttSubscribedBaseTopic = gSettings.mqttBaseTopic;
  String commandTopic = sMqttSubscribedBaseTopic + kMqttCommandTopicSuffix;
  int msgId = esp_mqtt_client_subscribe(mqttClient, commandTopic.c_str(), 1);
  if (msgId >= 0) {
    INFO_MQTT_PUBSUBF("Subscribed to command topic: %s", commandTopic.c_str());
  } else {
    WARN_MQTTF("Failed to subscribe to command topic");
  }
}

// One queued MQTT command between the esp-mqtt callback and its cmd_exec
// completion. The admission keeps mqttClient alive (mqttTick cannot tear the
// client down) until the response has been enqueued.
struct MqttCommandReply {
  MqttAdmission use;
  String responseTopic;
  String user;
  String safeCommand;
  const char* routedKind = nullptr;  // static text; nullptr for a local command
};

// Builds the response document and hands it to the client's outbox. Enqueue
// takes the client's API lock only briefly and does no socket I/O, so a
// completion on cmd_exec never waits for the esp-mqtt task's network work.
// The esp-mqtt task sends queued items at the top of its next loop pass (up
// to its poll-read timeout later when the broker is idle).
//
// When a stop is already pending (closemqtt sent over MQTT, for example), mqttTick claims teardown as soon as this reply releases
// its admission, and the esp-mqtt task deletes every outbox item when it
// exits, so a queued reply would be lost. Such a reply is published directly
// instead: the event task no longer holds the API lock across a command, and
// teardown waits for the admission this reply still holds.
static void enqueueMqttCommandReply(const MqttCommandReply& reply, bool ok,
                                    const char* result) {
  const String safeResult = redactMqttCommandResult(String(result ? result : ""));
  PSRAM_JSON_DOC(respDoc);
  respDoc["ok"] = ok;
  if (reply.routedKind) {
    respDoc["routed"] = reply.routedKind;
  } else {
    respDoc["user"] = reply.user;
    // Keep command correlation without reflecting a login password (or any
    // other registered secret-bearing argument) back over MQTT.
    respDoc["cmd"] = reply.safeCommand;
  }
  if (ok) {
    respDoc["result"] = safeResult;
  } else {
    respDoc["error"] = safeResult.length() > 0
                           ? safeResult
                           : String("Command execution failed");
  }
  String respStr;
  serializeJson(respDoc, respStr);
  if (sMqttLifecycle.stopPending()) {
    if (esp_mqtt_client_publish(mqttClient, reply.responseTopic.c_str(),
                                respStr.c_str(), (int)respStr.length(), 0,
                                0) < 0) {
      WARN_MQTTF("Command response could not be sent before stop");
    }
    return;
  }
  if (esp_mqtt_client_enqueue(mqttClient, reply.responseTopic.c_str(),
                              respStr.c_str(), (int)respStr.length(), 0, 0,
                              true) < 0) {
    WARN_MQTTF("Command response could not be queued");
  }
}

// cmd_exec completion: answer, then release the admission with the reply.
static void mqttCommandCompleted(bool ok, const char* result, void* userData) {
  MqttCommandReply* reply = static_cast<MqttCommandReply*>(userData);
  if (!reply) return;
  enqueueMqttCommandReply(*reply, ok, result);
  DEBUG_MQTT_COMMANDSF("Command response: ok=%d", ok);
  reply->~MqttCommandReply();
  free(reply);
}

// Queues one authenticated command line on cmd_exec and returns at once: the
// esp-mqtt task holds the client's API lock while it dispatches this event,
// so waiting here would block every other publisher (the main loop's state
// publish among them) for the whole command. The admission moves into the
// completion only once the command is queued; every failure answers here
// while the caller's admission is still held.
static void submitMqttCommand(MqttAdmission&& use, const String& line,
                              const char* username, const String& peer,
                              const String& responseTopic,
                              const String& safeCommand,
                              const char* routedKind) {
  void* storage = ps_alloc(sizeof(MqttCommandReply), AllocPref::PreferPsram,
                           "mqtt.cmd.reply");
  if (!storage) {
    esp_mqtt_client_publish(mqttClient, responseTopic.c_str(),
      "{\"ok\":false,\"error\":\"Out of memory\"}", 0, 0, false);
    return;
  }
  MqttCommandReply* reply = new (storage) MqttCommandReply();
  reply->use = std::move(use);
  reply->responseTopic = responseTopic;
  reply->user = username;
  reply->safeCommand = safeCommand;
  reply->routedKind = routedKind;

  Command queued;
  queued.line = line;
  queued.ctx.origin = ORIGIN_MQTT;
  queued.ctx.auth.transport = SOURCE_MQTT;
  queued.ctx.auth.user = username;
  queued.ctx.auth.ip = peer;
  queued.ctx.auth.path = "/mqtt/command";
  queued.ctx.auth.opaque = nullptr;
  queued.ctx.id = (uint32_t)millis();
  queued.ctx.timestampMs = (uint32_t)millis();
  queued.ctx.outputMask = MSG_ROUTE_FILE;
  queued.ctx.validateOnly = false;
  queued.ctx.captureOutput = false;
  queued.ctx.replyHandle = nullptr;
  queued.ctx.httpReq = nullptr;
  if (!submitCommandAsync(queued, mqttCommandCompleted, reply)) {
    // Not queued: the completion will never run, so answer and release here.
    // submitCommandAsync reports a full queue, a failed request allocation
    // and a missing executor queue alike, so the answer covers all three.
    enqueueMqttCommandReply(*reply, false,
                            commandInputLengthAccepted(line.length())
                                ? "Error: Command could not be queued - try again"
                                : "Error: Command exceeds input limit");
    reply->~MqttCommandReply();
    free(reply);
  }
}

// Handle incoming MQTT command
// Expected JSON format: {"user":"username","pass":"password","cmd":"command"}
static void handleMqttCommand(const char* topic, int topicLen, const char* data, int dataLen) {
  String commandTopic = gSettings.mqttBaseTopic + kMqttCommandTopicSuffix;
  String responseTopic = gSettings.mqttBaseTopic + kMqttResponseTopicSuffix;
  
  // Check if this is our command topic
  if (topicLen != (int)commandTopic.length() || 
      strncmp(topic, commandTopic.c_str(), topicLen) != 0) {
    return;  // Not our command topic
  }

  // Linearize command admission against shutdown before parsing, auth, or
  // queue publication.  If shutdown won the race, answer directly from the
  // MQTT callback without waiting on cmd_exec; the IDF client's recursive API
  // lock keeps the handle alive until this callback returns.
  MqttClientUse commandUse;
  if (!commandUse.admitted()) {
    esp_mqtt_client_publish(
        mqttClient, responseTopic.c_str(),
        "{\"ok\":false,\"error\":\"MQTT client is stopping\"}",
        0, 0, false);
    return;
  }
  
  // Extract payload string
  String payload;
  payload.reserve(dataLen + 1);
  for (int i = 0; i < dataLen; i++) {
    payload += data[i];
  }
  payload.trim();
  
  if (payload.length() == 0) return;
  
  // The JSON envelope carries both the MQTT password and the raw command.
  // Never put it in diagnostics, even when command tracing is enabled.
  DEBUG_MQTT_COMMANDSF("Command payload received: %d bytes",
                       (int)payload.length());
  
  // Parse JSON payload
  PSRAM_JSON_DOC(doc);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    WARN_MQTTF("Command JSON parse error: %s", err.c_str());
    esp_mqtt_client_publish(mqttClient, responseTopic.c_str(), 
      "{\"ok\":false,\"error\":\"Invalid JSON format\"}", 0, 0, false);
    return;
  }
  
  const char* username = doc["user"] | "";
  const char* password = doc["pass"] | "";
  const char* command = doc["cmd"] | "";
  
  // Validate required fields
  if (strlen(username) == 0 || strlen(password) == 0 || strlen(command) == 0) {
    WARN_MQTTF("Command missing user/pass/cmd fields");
    esp_mqtt_client_publish(mqttClient, responseTopic.c_str(),
      "{\"ok\":false,\"error\":\"Missing user, pass, or cmd field\"}", 0, 0, false);
    return;
  }
  
  // Check for target field (mesh bridge routing)
  const char* target = doc["target"] | "";
  const String safeCommand = redactCmdForAudit(String(command));
  
  INFO_MQTT_COMMANDSF("Command from user '%s': %s%s%s", username,
               safeCommand.c_str(),
               strlen(target) > 0 ? " target=" : "", target);
  
  // Authenticate user. Every client of this broker shares one lockout key,
  // so a client that keeps guessing also locks out MQTT users with the right
  // password until the lock ends (the same trade as serial "local").
  const String mqttPeer = "mqtt:" + gSettings.mqttHost;
  unsigned long lockoutRemainingMs = 0;
  if (!credentialAttemptAllowed(SOURCE_MQTT, mqttPeer.c_str(),
                                &lockoutRemainingMs)) {
    WARN_MQTTF("Command login locked out for user '%s'", username);
    recordLoginAttempt(SOURCE_MQTT, String(username), mqttPeer, false,
                       "Locked out");
    char lockReply[96];
    snprintf(lockReply, sizeof(lockReply),
             "{\"ok\":false,\"error\":\"Login locked out. Retry in %lu seconds.\"}",
             (lockoutRemainingMs + 999UL) / 1000UL);
    esp_mqtt_client_publish(mqttClient, responseTopic.c_str(), lockReply, 0,
                            0, false);
    return;
  }
  if (!isValidUser(String(username), String(password))) {
    WARN_MQTTF("Authentication FAILED for user '%s'", username);
    credentialAttemptRecord(SOURCE_MQTT, mqttPeer.c_str(), username, false);
    recordLoginAttempt(SOURCE_MQTT, String(username), mqttPeer, false,
                       "Invalid credentials");
    // Every failure reaches the event register; the notification renderer
    // applies the 30s toast cooldown downstream.
    systemEventPost(SYSEVT_LOGIN_FAIL, username, "mqtt");
    esp_mqtt_client_publish(mqttClient, responseTopic.c_str(),
      "{\"ok\":false,\"error\":\"Authentication failed\"}", 0, 0, false);
    return;
  }
  credentialAttemptRecord(SOURCE_MQTT, mqttPeer.c_str(), username, true);
  
  DEBUG_MQTT_COMMANDSF("Authentication successful for user '%s'", username);

#if ENABLE_ESPNOW
  // If target is specified, route command to mesh devices instead of executing locally
  if (strlen(target) > 0 && gSettings.meshRole == MESH_ROLE_MASTER) {
    String targetStr = target;
    String routedLine;
    const char* routedKind = nullptr;

    // Route based on target prefix: "room:<name>", "tag:<name>", "device:<name_or_mac>"
    if (targetStr.startsWith("room:")) {
      routedKind = "room";
      routedLine = "espnowroomcmd " + targetStr.substring(5);
    } else if (targetStr.startsWith("tag:")) {
      routedKind = "tag";
      routedLine = "espnowtagcmd " + targetStr.substring(4);
    } else if (targetStr.startsWith("device:")) {
      routedKind = "device";
      routedLine = "espnowremote " + targetStr.substring(7);
    } else {
      esp_mqtt_client_publish(mqttClient, responseTopic.c_str(),
        "{\"ok\":false,\"error\":\"Unknown target prefix. Use room:, tag:, or device:\"}", 0, 0, false);
      return;
    }

    // These are admin-only registry commands. Never call their handlers
    // directly from the MQTT callback: that bypasses authorization and cmd_exec
    // serialization. The target credentials remain in the raw line only for
    // encrypted peer delivery; every local sink sees the redacted form.
    routedLine += " "; routedLine += username;
    routedLine += " "; routedLine += password;
    routedLine += " "; routedLine += command;
    submitMqttCommand(std::move(commandUse), routedLine, username, mqttPeer,
                      responseTopic, safeCommand, routedKind);
    return;
  }
#endif

  // Route command execution through cmd_exec instead of calling
  // executeCommand() directly on the esp-mqtt event task. This serializes MQTT
  // commands with every other command source (no concurrent-handler races on
  // shared state / static response buffers) and runs them on cmd_exec's 8 KB
  // stack. The response is published from the completion.
  systemEventPost(SYSEVT_REMOTE_CMD_RX, username, safeCommand.c_str());
  submitMqttCommand(std::move(commandUse), String(command), username, mqttPeer,
                    responseTopic, safeCommand, nullptr);
}

// Forward declarations for mesh peer discovery (defined below)
#if ENABLE_ESPNOW
static void publishMeshPeerDiscovery();
#endif

// Publish all discovery configs for enabled sensors: every row of the entity
// table (System_MQTTPayloadCore.h) whose section this build has and the
// mqttPublish* settings switch on, then clear the configs of retired entities.
static void publishMqttDiscovery() {
  if (!mqttTofConnected.load(std::memory_order_acquire) ||
      gSettings.mqttDiscoveryPrefix.length() == 0) return;
  
  INFO_MQTT_DISCOVERYF("Publishing Home Assistant discovery configs...");

  const MqttLocalDiscovery local = mqttLocalDiscovery();
  const uint16_t compiled = mqttCompiledSections();
  const uint16_t active = compiled & mqttSwitchedOnSections();
  for (size_t i = 0; i < mqttpayload::kEntityCount; ++i) {
    const mqttpayload::HaEntity& entity = mqttpayload::kEntities[i];
    if (mqttpayload::entityActive(entity, mqttpayload::kLocal, active)) {
      publishDiscoveryConfig(entity, local);
    }
  }
  for (size_t i = 0; i < mqttpayload::kRetiredCount; ++i) {
    const mqttpayload::RetiredEntity& retired = mqttpayload::kRetired[i];
    if (mqttpayload::retiredApplies(retired, mqttpayload::kLocal, compiled)) {
      publishRetiredDiscovery(retired, local.deviceId);
    }
  }

  INFO_MQTT_DISCOVERYF("Discovery configs published");
  
  // Publish discovery for mesh peers (master bridge mode)
#if ENABLE_ESPNOW
  publishMeshPeerDiscovery();
#endif
}

// ============================================================================
// Mesh Bridge: Publish HA Discovery for Remote Peers (Master Only)
// ============================================================================
#if ENABLE_ESPNOW

// The HA id of a mesh peer: "hardwareone_<12 hex>".
static String meshPeerDeviceId(const MeshPeerMeta& peer) {
  char macCompact[13];
  macToPathToken(peer.mac, macCompact);  // no-separator UPPER
  char peerIdBuf[26];
  snprintf(peerIdBuf, sizeof(peerIdBuf), "hardwareone_%s", macCompact);
  return String(peerIdBuf);
}

// Publish a single HA discovery config for a remote peer device
// Similar to publishDiscoveryConfig but with peer-specific device identity
static void publishPeerDiscoveryConfig(const MeshPeerMeta& peer,
                                       const mqttpayload::HaEntity& entity,
                                       const String& masterDeviceId) {
  if (!mqttClient ||
      !mqttTofConnected.load(std::memory_order_acquire)) return;

  String peerId = meshPeerDeviceId(peer);

  // Peer state topic: <baseTopic>/devices/<peerId>/state
  String peerStateTopic = gSettings.mqttBaseTopic + "/devices/" + peerId + "/state";
  String peerAvailTopic = gSettings.mqttBaseTopic + "/devices/" + peerId + "/availability";
  String uniqueId = peerId + "_" + entity.objectId;

  String discoveryTopic = gSettings.mqttDiscoveryPrefix + "/" + entity.component + "/" +
                          peerId + "/" + entity.objectId + "/config";

  // Device info — the peer's friendly name, else its device name, else the
  // unknown-peer label; its room. No model or sw_version: the master does not
  // know a peer's board or firmware.
  char unknownPeer[devicename::kFieldLen];
  mqttpayload::DeviceInfo device;
  device.id = peerId.c_str();
  device.name = mqttpayload::peerDisplayName(peer.friendlyName, peer.name, peer.mac, unknownPeer);
  device.area = peer.room;
  device.viaDevice = masterDeviceId.c_str();

  // Shares the same PSRAM buffer as publishDiscoveryConfig — this function only
  // runs after every publishDiscoveryConfig frame has returned. See
  // sMqttDiscoveryConfigJson.
  PSRAM_JSON_DOC(doc);
  const size_t len =
      mqttpayload::buildDiscovery(entity, uniqueId.c_str(), peerStateTopic.c_str(),
                                  peerAvailTopic.c_str(), device, doc)
          ? mqttpayload::serializeBounded(doc, sMqttDiscoveryConfigJson, kMqttDiscoveryJsonCap)
          : 0;
  if (len == 0) {
    WARN_MQTTF("Peer discovery config for %s/%s exceeds %u bytes; not published",
               peerId.c_str(), entity.objectId, (unsigned)kMqttDiscoveryJsonCap);
    return;
  }

  esp_mqtt_client_publish(mqttClient, discoveryTopic.c_str(), sMqttDiscoveryConfigJson,
                          (int)len, 1, true);
}

// A mesh peer's Home Assistant availability (<base>/devices/<peer id>/
// availability, retained): "online" only while the peer is online - a mesh
// heartbeat within 30 s and an encrypted session up (espnowPeerState, the
// rule every surface uses). A peer still connecting (heartbeat, no session
// yet) is "offline" like a silent one: commands to it would not get through,
// so Home Assistant shows its entities unavailable.
static const char* meshPeerAvailability(EspNowPeerState state) {
  return state == EspNowPeerState::Online ? "online" : "offline";
}

// The state sections a peer's readings fill (its sensorMask); system always.
static uint16_t meshPeerSections(const MeshPeerMeta& peer) {
  uint16_t sections = mqttpayload::kSystem;
  if (peer.sensorMask & (1 << REMOTE_SENSOR_TOF)) sections |= mqttpayload::kTof;
  if (peer.sensorMask & (1 << REMOTE_SENSOR_IMU)) sections |= mqttpayload::kImu;
  if (peer.sensorMask & (1 << REMOTE_SENSOR_GPS)) sections |= mqttpayload::kGps;
  return sections;
}

// Publish HA discovery for all known mesh peers based on their sensor capabilities
static void publishMeshPeerDiscovery() {
  if (!mqttClient ||
      !mqttTofConnected.load(std::memory_order_acquire)) return;
  if (gSettings.meshRole != MESH_ROLE_MASTER) return;  // Only master bridges
  if (!gMeshPeerMeta) return;  // Not yet allocated

  const String masterDeviceId = getDeviceId();
  int peerCount = 0;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (!gMeshPeerMeta[i].isActive) continue;
    const MeshPeerMeta& peer = gMeshPeerMeta[i];
    peerCount++;

    // The peer rows of the entity table: system always, the sensors by the
    // peer's sensorMask.
    const uint16_t sections = meshPeerSections(peer);
    for (size_t e = 0; e < mqttpayload::kEntityCount; ++e) {
      const mqttpayload::HaEntity& entity = mqttpayload::kEntities[e];
      if (mqttpayload::entityActive(entity, mqttpayload::kPeer, sections)) {
        publishPeerDiscoveryConfig(peer, entity, masterDeviceId);
      }
    }
    // Clear the peer entities earlier firmware declared (uptime, thermal).
    const String peerId = meshPeerDeviceId(peer);
    for (size_t r = 0; r < mqttpayload::kRetiredCount; ++r) {
      const mqttpayload::RetiredEntity& retired = mqttpayload::kRetired[r];
      if (mqttpayload::retiredApplies(retired, mqttpayload::kPeer,
                                      sections | mqttpayload::kThermal)) {
        publishRetiredDiscovery(retired, peerId);
      }
    }

    // Publish availability for this peer
    String peerAvailTopic = gSettings.mqttBaseTopic + "/devices/" + peerId + "/availability";
    const char* status = meshPeerAvailability(espnowPeerState(peer.mac));
    esp_mqtt_client_publish(mqttClient, peerAvailTopic.c_str(), status, 0, 1, true);
  }

  if (peerCount > 0) {
    INFO_MQTT_DISCOVERYF("Mesh bridge: published discovery for %d peer(s)", peerCount);
  }
}

// Publish sensor data for all mesh peers from the table of received readings
// Called periodically alongside local sensor publishing
static void publishMeshPeerSensorData() {
  if (!mqttClient ||
      !mqttTofConnected.load(std::memory_order_acquire)) return;
  if (gSettings.meshRole != MESH_ROLE_MASTER) return;
  if (!gMeshPeerMeta) return;  // Not yet allocated
  const int remoteSlots = remoteSensorSlotCount();
  if (remoteSlots == 0) return;  // ESP-NOW runtime has not initialized
  // One reading's text at a time, copied under the table's lock.
  char* remoteText = (char*)ps_alloc(espnowsensor::kReadingMaxBytes + 1, AllocPref::PreferPsram, "mqtt.peer.reading");
  if (!remoteText) return;

  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (!gMeshPeerMeta[i].isActive) continue;
    const MeshPeerMeta& peer = gMeshPeerMeta[i];

    // Build peer device ID
    char macCompact[13];
    macToPathToken(peer.mac, macCompact);  // no-separator UPPER

    String peerStateTopic = gSettings.mqttBaseTopic + "/devices/hardwareone_" + macCompact + "/state";

    // Availability every pass, also for a peer without a fresh reading: a
    // peer that went silent (or is only connecting) turns "offline" here
    // instead of keeping the retained "online" until the next discovery.
    const EspNowPeerState state = espnowPeerState(peer.mac);
    String peerAvailTopic = gSettings.mqttBaseTopic + "/devices/hardwareone_" + macCompact + "/availability";
    esp_mqtt_client_publish(mqttClient, peerAvailTopic.c_str(), meshPeerAvailability(state), 0, 1, true);

    // Build JSON state from remote sensor cache
    PSRAM_JSON_DOC(doc);
    bool hasData = false;

    // The slots of this peer that hold a fresh reading
    for (int s = 0; s < remoteSlots; s++) {
      RemoteSensorInfo slot;
      if (!remoteSensorInfoAt(s, &slot) || !slot.fresh) continue;
      if (memcmp(slot.deviceMac, peer.mac, 6) != 0) continue;
      // Only the device's own readings: what a bond master forwards for its
      // partner is that bond master's statement and is not published as the
      // device's state (MQTT is parked; see the forwarding design).
      if (remoteSensorInfoVia(slot) != nullptr) continue;
      const size_t textLen = remoteSensorCopyText(slot.deviceMac, remoteSensorInfoVia(slot), slot.sensorType, remoteText,
                                                  espnowsensor::kReadingMaxBytes + 1);
      if (textLen == 0) continue;

      // Parse the reading and merge into state
      PSRAM_JSON_DOC(sensorDoc);
      if (deserializeJson(sensorDoc, remoteText, textLen) == DeserializationError::Ok) {
        // Determine key from sensor type
        const char* key = nullptr;
        switch (slot.sensorType) {
          case REMOTE_SENSOR_THERMAL: key = "thermal"; break;
          case REMOTE_SENSOR_TOF:     key = "tof"; break;
          case REMOTE_SENSOR_IMU:     key = "imu"; break;
          case REMOTE_SENSOR_GPS:     key = "gps"; break;
          case REMOTE_SENSOR_INPUT: key = "input"; break;
          case REMOTE_SENSOR_FMRADIO: key = "fmradio"; break;
          default: break;
        }
        if (key) {
          doc[key] = sensorDoc.as<JsonObject>();
          hasData = true;
        }
      }
    }

    if (!hasData) continue;

    // System info: the peer's state as every surface shows it (online /
    // connecting / offline, espnowPeerState), "online" true only for online,
    // and how long ago anything was heard (the health slot's stamps).
    MeshPeerHealth* health = getMeshPeerHealth(peer.mac, false);
    JsonObject sys = doc["system"].to<JsonObject>();
    sys["online"] = state == EspNowPeerState::Online;
    sys["state"] = espnowPeerStateName(state);
    if (health) {
      uint32_t lastContact = health->lastMeshHeartbeatMs;
      if (health->lastRxActivityMs > lastContact) lastContact = health->lastRxActivityMs;
      sys["last_seen"] = lastContact ? ((millis() - lastContact) / 1000) : 0;
    }

    // Serialize and publish; a document whose text cannot be had is not sent.
    const PsramJsonText output(doc, "mqtt.peer.json");
    if (output.ok()) {
      esp_mqtt_client_publish(mqttClient, peerStateTopic.c_str(), output.c_str(), 0, 0, false);
    }

  }
  free(remoteText);
}

#endif // ENABLE_ESPNOW

// ============================================================================
// MQTT Event Handler
// ============================================================================

// Timestamp of the last successful broker connect; nonzero means "was connected".
// Gates the [EVENT] disconnect log to once per transition — esp-mqtt retries every
// ~10s and fires DISCONNECTED per failed attempt, and MQTT_EVENT_ERROR clears
// mqttTofConnected before DISCONNECTED on live-connection drops, so the flag alone
// isn't a reliable was-connected signal at DISCONNECTED time.
static std::atomic<uint32_t> sMqttConnectedAtMs{0};

static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
  
  switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_BEFORE_CONNECT:
      sMqttBeforeConnectSeen.store(true, std::memory_order_release);
      break;

    case MQTT_EVENT_CONNECTED:
      mqttTofConnected.store(true, std::memory_order_release);
      lastError = "";
      broadcastOutput("[MQTT] Connected to broker");
      INFO_MQTT_CONNECTIONF("Connected to %s:%d", gSettings.mqttHost.c_str(), gSettings.mqttPort);
      sMqttConnectedAtMs.store(millis(), std::memory_order_release);
      logSystemEvent("MQTT", "connected to broker %s:%d", gSettings.mqttHost.c_str(), gSettings.mqttPort);
      {
        char broker[48];
        snprintf(broker, sizeof(broker), "%s:%d", gSettings.mqttHost.c_str(), gSettings.mqttPort);
        systemEventPost(SYSEVT_MQTT_CONNECTED, broker);
      }
      
      // Publish availability
      if (gSettings.mqttBaseTopic.length() > 0) {
        String availTopic = gSettings.mqttBaseTopic + "/availability";
        esp_mqtt_client_publish(mqttClient, availTopic.c_str(), "online", 0, 1, true);
      }
      
      // Publish Home Assistant discovery configs
      publishMqttDiscovery();
      
      // Subscribe to command topic for HA commands
      subscribeToCommandTopic();
      
      // Subscribe to external topics if enabled
      subscribeToExternalTopics();
      break;
      
    case MQTT_EVENT_DISCONNECTED:
      {
        const uint32_t connectedAt =
            sMqttConnectedAtMs.exchange(0, std::memory_order_acq_rel);
        if (connectedAt != 0) {
          logSystemEvent("MQTT", "broker connection lost (was connected %lus)",
                         (unsigned long)((millis() - connectedAt) / 1000));
          // Inside the once-per-transition gate — esp-mqtt fires DISCONNECTED
          // on every ~10s retry, which would spam the ring outside this guard.
          char secs[16];
          snprintf(secs, sizeof(secs), "%lu",
                   (unsigned long)((millis() - connectedAt) / 1000));
          systemEventPost(SYSEVT_MQTT_DISCONNECTED, secs);
        }
      }
      mqttTofConnected.store(false, std::memory_order_release);
      WARN_MQTTF("Disconnected from broker");
      break;
      
    case MQTT_EVENT_ERROR:
      mqttTofConnected.store(false, std::memory_order_release);
      lastError = "Connection error";
      ERROR_MQTTF("Error event");
      break;
      
    case MQTT_EVENT_DATA:
      // Check for command topic first
      handleMqttCommand(event->topic, event->topic_len, event->data, event->data_len);
      
      // Store incoming data as external sensor if subscriptions enabled.
      // updateExternalSensor drops this client's command and response topics.
      if (gSettings.mqttSubscribeExternal && event->topic_len > 0) {
        updateExternalSensor(event->topic, event->topic_len, event->data, event->data_len);
      }
      // Topic and size only (G-77): the command topic's payload is the
      // {user,pass,cmd} envelope, and a wildcard subscription can deliver
      // it here too, so no inbound payload text reaches a log sink.
      DEBUG_MQTT_PUBSUBF("Received: topic=%.*s data=%d bytes",
                    event->topic_len, event->topic, event->data_len);
      break;
      
    default:
      break;
  }
}

// ============================================================================
// MQTT Lifecycle Functions
// ============================================================================

bool startMqtt() {
  if (sMqttLifecycle.stopPending()) {
    lastError = "MQTT client is stopping";
    return false;
  }

  if (mqttClientRunning.load(std::memory_order_acquire)) {
    return true;
  }

  if (!sMqttLifecycle.beginStart()) {
    lastError = sMqttLifecycle.starting()
                    ? "MQTT client start already in progress"
                    : "MQTT lifecycle is busy";
    return false;
  }
  MqttStartAttempt startAttempt;

  if (!WiFi.isConnected()) {
    lastError = "WiFi not connected";
    systemEventPost(SYSEVT_MQTT_START_FAILED, "wifi not connected");
    return false;
  }
  
  if (gSettings.mqttHost.length() == 0) {
    lastError = "MQTT host not configured";
    systemEventPost(SYSEVT_MQTT_START_FAILED, "host not configured");
    return false;
  }
  
  // Generate base topic if empty
  if (gSettings.mqttBaseTopic.length() == 0) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char macStr[13];
    macToPathToken(mac, macStr);  // no-separator UPPER
    { char topicBuf[32]; snprintf(topicBuf, sizeof(topicBuf), "hardwareone/%s", macStr); setSetting(gSettings.mqttBaseTopic, String(topicBuf)); }
    INFO_MQTT_DISCOVERYF("Auto-generated base topic: %s", gSettings.mqttBaseTopic.c_str());
  }
  
  // Configure MQTT client - strings must persist until esp_mqtt_client_init() completes
  String brokerUri;
  bool useTls = (gSettings.mqttTlsMode > 0);
  {
    char uriBuf[128];
    snprintf(uriBuf, sizeof(uriBuf), "%s://%s:%d", useTls ? "mqtts" : "mqtt", gSettings.mqttHost.c_str(), gSettings.mqttPort);
    brokerUri = uriBuf;
  }
  String availTopic = gSettings.mqttBaseTopic + "/availability";
  
  esp_mqtt_client_config_t mqtt_cfg = {};
  mqtt_cfg.broker.address.uri = brokerUri.c_str();
  // A command envelope is parsed whole only when it fits the client's
  // buffer; the 1 KB default delivered anything larger as unparseable pieces.
  // Sized to the command result ceiling, in and out (8 KB of internal RAM).
  mqtt_cfg.buffer.size = CMD_RESULT_MAX;
  mqtt_cfg.buffer.out_size = CMD_RESULT_MAX;
  
  // TLS configuration based on mqttTlsMode: 0=None, 1=TLS (no verify), 2=TLS+Verify
  static String caCertData;  // Must persist for MQTT client lifetime
  if (gSettings.mqttTlsMode == 2) {
    // TLS + Certificate Verification
    if (gSettings.mqttCaCertPath.length() > 0) {
      FsLockGuard fsGuard("mqtt.cert.load");
      File certFile = VFS::openGuarded(gSettings.mqttCaCertPath, "r", VFS::systemAuth(VFS::Scopes::CERTS, "mqtt.cert.load"));
      if (certFile) {
        caCertData = certFile.readString();
        certFile.close();
        mqtt_cfg.broker.verification.certificate = caCertData.c_str();
        mqtt_cfg.broker.verification.skip_cert_common_name_check = false;
        INFO_MQTT_CONNECTIONF("TLS + Verify: using %s", gSettings.mqttCaCertPath.c_str());
      } else {
        lastError = "CA cert file not found: " + gSettings.mqttCaCertPath;
        ERROR_MQTTF("%s", lastError.c_str());
        systemEventPost(SYSEVT_MQTT_START_FAILED, "CA cert file not found");
        return false;
      }
    } else {
      lastError = "TLS+Verify requires CA cert path";
      ERROR_MQTTF("%s", lastError.c_str());
      systemEventPost(SYSEVT_MQTT_START_FAILED, "TLS+verify needs CA path");
      return false;
    }
  } else if (gSettings.mqttTlsMode == 1) {
    // TLS without certificate verification (encrypted but trusts any server)
    mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
    mqtt_cfg.broker.verification.certificate = nullptr;
    INFO_MQTT_CONNECTIONF("TLS enabled (no cert verification)");
  }
  // Mode 0 = no TLS, no special config needed
  
  if (gSettings.mqttUser.length() > 0) {
    mqtt_cfg.credentials.username = gSettings.mqttUser.c_str();
  }
  if (gSettings.mqttPassword.length() > 0) {
    mqtt_cfg.credentials.authentication.password = gSettings.mqttPassword.c_str();
  }
  
  // LWT (Last Will and Testament) for availability
  mqtt_cfg.session.last_will.topic = availTopic.c_str();
  mqtt_cfg.session.last_will.msg = "offline";
  mqtt_cfg.session.last_will.qos = 1;
  mqtt_cfg.session.last_will.retain = true;

  if (!initExternalSensorStorage()) {
    lastError = "Failed to allocate external sensor storage";
    systemEventPost(SYSEVT_MQTT_START_FAILED, "external sensor storage OOM");
    return false;
  }
  
  mqttClient = esp_mqtt_client_init(&mqtt_cfg);
  if (!mqttClient) {
    lastError = "Failed to initialize MQTT client";
    ERROR_MQTTF("Client init failed");
    systemEventPost(SYSEVT_MQTT_START_FAILED, "client init failed");
    releaseExternalSensorStorage();
    return false;
  }
  
  const esp_err_t registerErr = esp_mqtt_client_register_event(
      mqttClient, MQTT_EVENT_ANY, mqtt_event_handler, nullptr);
  if (registerErr != ESP_OK) {
    lastError = "Failed to register MQTT event handler";
    ERROR_MQTTF("Event registration failed: %s", esp_err_to_name(registerErr));
    systemEventPost(SYSEVT_MQTT_START_FAILED, "event registration failed");
    esp_mqtt_client_destroy(mqttClient);
    mqttClient = nullptr;
    releaseExternalSensorStorage();
    return false;
  }

  sMqttBeforeConnectSeen.store(false, std::memory_order_release);
  const esp_err_t startErr = esp_mqtt_client_start(mqttClient);
  if (startErr != ESP_OK) {
    lastError = "Failed to start MQTT client";
    ERROR_MQTTF("Client start failed: %s", esp_err_to_name(startErr));
    systemEventPost(SYSEVT_MQTT_START_FAILED, "client task start failed");
    esp_mqtt_client_destroy(mqttClient);
    mqttClient = nullptr;
    releaseExternalSensorStorage();
    return false;
  }

  mqttClientRunning.store(true, std::memory_order_release);
  if (!startAttempt.finishStarted()) {
    lastError = "MQTT stop requested during startup";
    WARN_MQTTF("Client started with shutdown already pending");
    return false;
  }
  lastError = "";
  broadcastOutput("[MQTT] Client started");
  INFO_MQTT_CONNECTIONF("Connecting to %s://%s:%d", (gSettings.mqttTlsMode > 0) ? "mqtts" : "mqtt", 
               gSettings.mqttHost.c_str(), gSettings.mqttPort);
  
  return true;
}

// Actual ESP-MQTT teardown is main-loop-affine.  Calling the blocking IDF stop
// from cmd_exec can deadlock against an MQTT event callback that is itself
// waiting for cmd_exec, even when the close command came from WEB or SERIAL.
static bool stopMqttNow() {
  // Main-loop only. The first failed stop before BEFORE_CONNECT is retried to
  // cover the small window after xTaskCreate but before the higher-priority
  // MQTT worker first runs. A second such failure means the pinned IDF worker
  // took one of its pre-event autonomous-exit paths and is safe to destroy.
  static uint8_t failuresBeforeConnect = 0;

  if (!mqttClientRunning.load(std::memory_order_acquire)) {
    failuresBeforeConnect = 0;
    releaseExternalSensorStorage();
    releaseMqttStateJson();
    return true;
  }
  
  if (mqttClient) {
    // Publish offline before disconnecting
    if (mqttTofConnected.load(std::memory_order_acquire) &&
        gSettings.mqttBaseTopic.length() > 0) {
      String availTopic = gSettings.mqttBaseTopic + "/availability";
      esp_mqtt_client_publish(mqttClient, availTopic.c_str(), "offline", 0, 1, true);
    }

    const esp_err_t stopErr = esp_mqtt_client_stop(mqttClient);
    if (stopErr != ESP_OK) {
      const bool workerPassedEarlyExit =
          sMqttBeforeConnectSeen.load(std::memory_order_acquire);
      if (!workerPassedEarlyExit && failuresBeforeConnect > 0) {
        // Local ESP-IDF has only two autonomous run=false paths and both occur
        // before BEFORE_CONNECT. The mandatory first retry rules out a newly
        // created worker that simply had not run yet.
        WARN_MQTTF("Driver task exited before connect; cleaning up client");
      } else {
        if (!workerPassedEarlyExit) ++failuresBeforeConnect;
        // Never destroy a client whose task may still be live. Ask it to
        // disconnect, then let mqttTick retry outside the event callback.
        ERROR_MQTTF("Client stop failed; will retry: %s",
                    esp_err_to_name(stopErr));
        (void)esp_mqtt_client_disconnect(mqttClient);
        return false;
      }
    } else {
      // esp_mqtt_client_stop() does NOT deliver MQTT_EVENT_DISCONNECTED, so
      // post here so mqtt_connected doesn't stay stale after a manual stop.
      systemEventPost(SYSEVT_MQTT_DISCONNECTED, "stopped");
    }
    const esp_err_t destroyErr = esp_mqtt_client_destroy(mqttClient);
    if (destroyErr != ESP_OK) {
      ERROR_MQTTF("Stopped client destroy failed: %s",
                  esp_err_to_name(destroyErr));
    }
    mqttClient = nullptr;
  }
  
  mqttClientRunning.store(false, std::memory_order_release);
  mqttTofConnected.store(false, std::memory_order_release);
  sMqttConnectedAtMs.store(0, std::memory_order_release);
  sMqttBeforeConnectSeen.store(false, std::memory_order_release);
  failuresBeforeConnect = 0;
  releaseExternalSensorStorage();
  releaseMqttStateJson();
  broadcastOutput("[MQTT] Client stopped");
  return true;
}

bool stopMqtt() {
  const bool requested = sMqttLifecycle.requestStop();
  if (requested) {
    DEBUG_MQTT_CONNECTIONF(
        "Client stop scheduled (client_uses_in_flight=%lu)",
        (unsigned long)sMqttLifecycle.usesInFlight());
  }
  return requested;
}

// ============================================================================
// MQTT Publishing
// ============================================================================

// The sensor sections of the state document, for the dropped-section report.
static const char* const kMqttSectionNames[] = {"thermal", "tof", "imu", "presence",
                                                "gps", "apds", "rtc", "input"};
// Sections whose builder text did not parse at the last publish (bit i =
// kMqttSectionNames[i]); a change is reported once, not every publish.
static uint8_t sMqttDroppedSections = 0;

// One builder reading into the state document. The builder writes into
// `scratch` (the state buffer, idle until the document is serialized); the
// document copies the text. Returns whether the section is in the document.
// (Unused in a build with no sensor compiled.)
[[maybe_unused]] static bool mqttAddBuilt(JsonDocument& doc, const char* key, int (*builder)(char*, size_t),
                         char* scratch, size_t cap) {
  const int len = builder(scratch, cap);
  if (len <= 0 || (size_t)len >= cap) return false;
  return strcmp(key, "rtc") == 0 ? mqttpayload::addRtcSection(doc, scratch, (size_t)len)
                                 : mqttpayload::addSection(doc, key, scratch, (size_t)len);
}

void publishMqttSensorData() {
  MqttClientUse publishUse;
  if (!publishUse.admitted()) return;

  if (!mqttTofConnected.load(std::memory_order_acquire) || !mqttClient) {
    return;
  }
  
  // The state document's text, in the kept PSRAM buffer. When PSRAM refuses
  // it, this publish borrows a PreferPsram block and frees it, as every
  // publish did before. Until the document is serialized the same buffer is
  // the sensor builders' scratch.
  if (!sMqttStateJson) {
    sMqttStateJson = (char*)ps_alloc(mqttpayload::kStateCap, AllocPref::RequirePsram, "mqtt.json");
  }
  char* jsonBuf = sMqttStateJson;
  if (!jsonBuf) jsonBuf = (char*)ps_alloc(mqttpayload::kStateCap, AllocPref::PreferPsram, "mqtt.json");
  if (!jsonBuf) {
    WARN_MQTTF("Failed to allocate JSON buffer");
    return;
  }
  const size_t cap = mqttpayload::kStateCap;

  // Plain values: each source read once.
  mqttpayload::StateInput in;
  in.uptimeMs = (uint32_t)millis();
  in.hasTime = Clock::isSynced();
  if (in.hasTime) in.epochUtc = (int64_t)Clock::epochSeconds();
  const String deviceName = SelfDevice::deviceName();
  in.deviceName = deviceName.c_str();
  in.deviceNameSet = SelfDevice::deviceNameSet();
  if (gSettings.mqttPublishSystem) {
    in.hasSystem = true;
    in.uptimeS = SelfDevice::uptimeSeconds();
    in.heapFree = SelfDevice::freeHeapBytes();
    in.heapMin = SelfDevice::minFreeHeapBytes();
    const uint32_t psramTotal = (uint32_t)heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psramTotal > 0) {
      in.hasPsram = true;
      in.psramTotal = psramTotal;
      in.psramFree = SelfDevice::psramFreeBytes();
    }
  }
  String ssid, ip;
  if (gSettings.mqttPublishWifi && WiFi.isConnected()) {
    in.hasWifi = true;
    ssid = WiFi.SSID();
    ip = WiFi.localIP().toString();
    in.ssid = ssid.c_str();
    in.ip = ip.c_str();
    in.rssi = WiFi.RSSI();
  }

  PSRAM_JSON_DOC(doc);
  const bool built = mqttpayload::beginState(in, doc);

  // Sensor sections, each from its builder (the same text `sensors json` and
  // ESP-NOW carry), only while the sensor runs and its publishing is on.
  uint8_t dropped = 0;
#if ENABLE_THERMAL_SENSOR
  // The summary (min/avg/max), not the 768-pixel frame.
  if (gSettings.mqttPublishThermal && gThermalRunning &&
      !mqttAddBuilt(doc, "thermal", thermalBuildSummaryJson, jsonBuf, cap)) dropped |= 1u << 0;
#endif
#if ENABLE_TOF_SENSOR
  if (gSettings.mqttPublishTof && gTofRunning &&
      !mqttAddBuilt(doc, "tof", tofBuildDataJson, jsonBuf, cap)) dropped |= 1u << 1;
#endif
#if ENABLE_IMU_SENSOR
  if (gSettings.mqttPublishImu && gImuRunning &&
      !mqttAddBuilt(doc, "imu", imuBuildDataJson, jsonBuf, cap)) dropped |= 1u << 2;
#endif
#if ENABLE_PRESENCE_SENSOR
  if (gSettings.mqttPublishPresence && gPresenceRunning &&
      !mqttAddBuilt(doc, "presence", presenceBuildDataJson, jsonBuf, cap)) dropped |= 1u << 3;
#endif
#if ENABLE_GPS_SENSOR
  if (gSettings.mqttPublishGps && gGpsRunning && gGpsConnected &&
      !mqttAddBuilt(doc, "gps", gpsBuildDataJson, jsonBuf, cap)) dropped |= 1u << 4;
#endif
#if ENABLE_APDS_SENSOR
  if (gSettings.mqttPublishApds && gApdsColorRunning && gApdsConnected &&
      !mqttAddBuilt(doc, "apds", apdsBuildDataJson, jsonBuf, cap)) dropped |= 1u << 5;
#endif
#if ENABLE_RTC_SENSOR
  // "datetime" (UTC, with Z) is made from the same reading's fields.
  if (gSettings.mqttPublishRtc && gRtcRunning && gRtcConnected &&
      !mqttAddBuilt(doc, "rtc", rtcBuildDataJson, jsonBuf, cap)) dropped |= 1u << 6;
#endif
  // The build's input builder, as System_ESPNow_Sensors.cpp picks it (GT911
  // touch has none).
#if ENABLE_GAMEPAD_SENSOR
  int (*const inputBuilder)(char*, size_t) = gamepadBuildDataJson;
#elif ENABLE_ANO_ENCODER
  int (*const inputBuilder)(char*, size_t) = anoEncoderBuildDataJson;
#elif ENABLE_GPIO_ENCODER
  int (*const inputBuilder)(char*, size_t) = gpioEncoderBuildDataJson;
#endif
#if ENABLE_GAMEPAD_SENSOR || ENABLE_ANO_ENCODER || ENABLE_GPIO_ENCODER
  if (gSettings.mqttPublishInput && gInputRunning && gInputConnected &&
      !mqttAddBuilt(doc, "input", inputBuilder, jsonBuf, cap)) dropped |= 1u << 7;
#endif

  if (dropped != sMqttDroppedSections) {
    const uint8_t newlyDropped = dropped & (uint8_t)~sMqttDroppedSections;
    for (size_t i = 0; i < sizeof(kMqttSectionNames) / sizeof(kMqttSectionNames[0]); ++i) {
      if (newlyDropped & (1u << i)) {
        WARN_MQTTF("State section %s not sent: its reading is not valid JSON", kMqttSectionNames[i]);
      }
    }
    sMqttDroppedSections = dropped;
  }

  // Never send a cut document: one that does not fit is skipped.
  const size_t len = built ? mqttpayload::serializeBounded(doc, jsonBuf, cap) : 0;
  if (len == 0) {
    WARN_MQTTF("State document did not fit in %u bytes; not published", (unsigned)cap);
  } else {
    // Publish to state topic
    String stateTopic = gSettings.mqttBaseTopic + "/state";
    int msg_id = esp_mqtt_client_publish(mqttClient, stateTopic.c_str(), jsonBuf, (int)len, 0, false);

    if (msg_id >= 0) {
      DEBUG_MQTT_PUBSUBF("Published %u bytes to %s", (unsigned)len, stateTopic.c_str());
    } else {
      WARN_MQTTF("Publish failed");
    }
  }

  if (jsonBuf != sMqttStateJson) free(jsonBuf);
  
  // Publish sensor data for mesh peers (master bridge mode)
#if ENABLE_ESPNOW
  publishMeshPeerSensorData();
#endif
}

void mqttTick() {
  const unsigned long now = millis();
  static unsigned long stopRetryAfterMs = 0;
  static bool stopRetryScheduled = false;

  // Drain lifecycle work before touching the handle for normal publication.
  // The packed gate prevents this claim until every admitted MQTT callback has
  // returned from its response publish.  No other task performs stop/destroy.
  if (sMqttLifecycle.stopPending()) {
    if (stopRetryScheduled &&
        (int32_t)(now - stopRetryAfterMs) < 0) return;
    if (sMqttLifecycle.tryBeginStop()) {
      if (stopMqttNow()) {
        sMqttLifecycle.finishStop();
        stopRetryAfterMs = 0;
        stopRetryScheduled = false;
      } else {
        sMqttLifecycle.retryStop();
        stopRetryAfterMs = now + 250;
        stopRetryScheduled = true;
      }
    }
    return;
  }

  if (!sMqttLifecycle.isOpen() ||
      !mqttClientRunning.load(std::memory_order_acquire) || !mqttClient) {
    return;
  }
  
  // Periodic publishing
  if (mqttTofConnected.load(std::memory_order_acquire) &&
      (now - lastPublishTime >= gSettings.mqttPublishIntervalMs)) {
    publishMqttSensorData();
    lastPublishTime = now;
  }
}

// ============================================================================
// MQTT CLI Commands
// ============================================================================

const char* cmd_openmqtt(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (sMqttLifecycle.stopPending()) {
    return "Error: [MQTT] Client is stopping; retry when mqttstatus reports stopped";
  }
  
  if (mqttClientRunning.load(std::memory_order_acquire)) {
    return "Error: MQTT client is already running. Stop it first with 'closemqtt' to restart it.";
  }
  
  if (startMqtt()) {
    // The broker connection completes asynchronously in the event handler, so
    // "started" is not yet "connected" — point the caller at where the live
    // connection state lands.
    cliHint("the broker connection completes asynchronously - confirm it with 'mqttstatus'");
    return "[MQTT] Client started, connecting...";
  } else {
    if (!ensureDebugBuffer()) return "Error: [MQTT] Start failed";
    snprintf(getDebugBuffer(), 1024, "Error: [MQTT] Start failed: %s", lastError.c_str());
    return getDebugBuffer();
  }
}

const char* cmd_closemqtt(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!stopMqtt()) {
    return "Error: MQTT client is not running. Start it with 'openmqtt'.";
  }
  return "[MQTT] Client stop scheduled";
}

// Milliseconds until the next periodic publish, 0 when it is already due.
// `(last + interval) - now` wrapped to about 4294967000 when the command ran
// after the due time and before the main loop had published.
static uint32_t mqttMsUntilNextPublish(uint32_t lastPublishMs, uint32_t intervalMs, uint32_t nowMs) {
  const uint32_t elapsedMs = nowMs - lastPublishMs;
  return elapsedMs < intervalMs ? intervalMs - elapsedMs : 0;
}

// Serializes a json reply into the debug buffer. serializeJson writes no NUL
// when the output fills the buffer, so a document that does not fit becomes
// `tooLarge`, never a cut or unterminated reply. A document that could not
// allocate (overflowed) is out of memory, not too large.
static const char* mqttJsonReply(const JsonDocument& doc, const char* tooLarge) {
  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  if (doc.overflowed()) return kCommandReplyOutOfMemory;
  const size_t n = serializeJson(doc, getDebugBuffer(), GLOBAL_DEBUG_BUFFER_SIZE);
  if (n == 0 || n >= GLOBAL_DEBUG_BUFFER_SIZE) return tooLarge;
  return getDebugBuffer();
}

const char* cmd_mqttstatus(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // JSON: returned to the caller only (no broadcastOutput); text path unchanged.
  if (argWantsJson(argsInput)) {
    const bool running = mqttClientRunning.load(std::memory_order_acquire);
    const bool connected = mqttTofConnected.load(std::memory_order_acquire);
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    doc["enabled"] = running;
    doc["connected"] = connected;
    doc["stopping"] = sMqttLifecycle.stopPending();
    doc["host"] = gSettings.mqttHost;
    doc["port"] = gSettings.mqttPort;
    doc["user"] = gSettings.mqttUser;
    doc["baseTopic"] = gSettings.mqttBaseTopic;
    doc["publishIntervalMs"] = gSettings.mqttPublishIntervalMs;
    if (lastError.length() > 0) doc["lastError"] = lastError;
    return mqttJsonReply(doc, "Error: Status does not fit in one reply. "
                              "Shorten the MQTT host, user or base topic.");
  }

  // Output each line separately to avoid DEBUG_MSG_SIZE (256 byte) truncation
  const bool running = mqttClientRunning.load(std::memory_order_acquire);
  const bool connected = mqttTofConnected.load(std::memory_order_acquire);
  broadcastOutput("=== MQTT STATUS ===");
  BROADCAST_PRINTF("Enabled: %s", running ? "Yes" : "No");
  BROADCAST_PRINTF("Connected: %s", connected ? "Yes" : "No");
  BROADCAST_PRINTF("Stopping: %s", sMqttLifecycle.stopPending() ? "Yes" : "No");
  BROADCAST_PRINTF("Broker: %s:%d", gSettings.mqttHost.c_str(), gSettings.mqttPort);
  BROADCAST_PRINTF("User: %s", gSettings.mqttUser.length() > 0 ? gSettings.mqttUser.c_str() : "(none)");
  BROADCAST_PRINTF("Base Topic: %s", gSettings.mqttBaseTopic.c_str());
  BROADCAST_PRINTF("Publish Interval: %d ms", gSettings.mqttPublishIntervalMs);
  
  if (lastError.length() > 0) {
    BROADCAST_PRINTF("Last Error: %s", lastError.c_str());
  }
  
  if (connected) {
    // The main loop owns lastPublishTime and renews it while this command
    // runs: copy it first, then read the clock, so the copy is never ahead.
    const uint32_t lastPublishMs = (uint32_t)lastPublishTime;
    const unsigned long nextPublish = mqttMsUntilNextPublish(
        lastPublishMs, (uint32_t)gSettings.mqttPublishIntervalMs, (uint32_t)millis());
    BROADCAST_PRINTF("Next Publish: %lu ms", nextPublish);
  }
  
  return "OK";
}

// ============================================================================
// MQTT Settings CLI Commands
// ============================================================================

const char* cmd_mqttautostart(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    return gSettings.mqttAutoStart ? "MQTT auto-start: ON" : "MQTT auto-start: OFF";
  }
  const int parsed = parseBoolArg(arg);
  if (parsed < 0) return kCommandReplyNotOnOff;
  setSetting(gSettings.mqttAutoStart, parsed == 1);
  return gSettings.mqttAutoStart ? "MQTT auto-start enabled" : "MQTT auto-start disabled";
}

const char* cmd_mqtthost(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
    snprintf(getDebugBuffer(), 1024, "MQTT host: %s", 
             gSettings.mqttHost.length() > 0 ? gSettings.mqttHost.c_str() : "(not set)");
    return getDebugBuffer();
  }
  setSetting(gSettings.mqttHost, arg);
  if (!ensureDebugBuffer()) return "MQTT host updated";
  snprintf(getDebugBuffer(), 1024, "MQTT host set to: %s", arg.c_str());
  return getDebugBuffer();
}

const char* cmd_mqttport(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
    snprintf(getDebugBuffer(), 1024, "MQTT port: %d", gSettings.mqttPort);
    return getDebugBuffer();
  }
  int port = 0;
  if (!parseIntArg(arg, port)) return kCommandReplyNotANumber;
  if (port < 1 || port > 65535) {
    return "Error: Port must be 1-65535";
  }
  setSetting(gSettings.mqttPort, port);
  if (!ensureDebugBuffer()) return "MQTT port updated";
  snprintf(getDebugBuffer(), 1024, "MQTT port set to: %d", port);
  return getDebugBuffer();
}

const char* cmd_mqtttlsmode(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  
  // Show current mode if no argument
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
    const char* modeStr = "None (unencrypted)";
    if (gSettings.mqttTlsMode == 1) modeStr = "TLS (encrypted, no verification)";
    else if (gSettings.mqttTlsMode == 2) modeStr = "TLS + Verify (encrypted + cert verification)";
    snprintf(getDebugBuffer(), 1024, "MQTT TLS Mode: %d - %s\nCA cert path: %s",
             gSettings.mqttTlsMode, modeStr,
             gSettings.mqttCaCertPath.length() > 0 ? gSettings.mqttCaCertPath.c_str() : "(not set)");
    return getDebugBuffer();
  }
  
  // Parse mode argument
  int newMode = -1;
  if (arg == "0" || arg.equalsIgnoreCase("none") || arg.equalsIgnoreCase("off")) {
    newMode = 0;
  } else if (arg == "1" || arg.equalsIgnoreCase("tls")) {
    newMode = 1;
  } else if (arg == "2" || arg.equalsIgnoreCase("verify") || arg.equalsIgnoreCase("tls+verify")) {
    newMode = 2;
  }
  
  if (newMode < 0 || newMode > 2) {
    return "Error: invalid arguments — Usage: mqtttlsmode [0|1|2|none|tls|verify]\n  0/none = No TLS\n  1/tls = TLS (no verification)\n  2/verify = TLS + Certificate Verification";
  }
  
  int oldMode = gSettings.mqttTlsMode;
  setSetting(gSettings.mqttTlsMode, newMode);
  
  // Create /system/certs/ folder for TLS modes
  if (newMode > 0 && !VFS::existsGuarded("/system/certs", VFS::systemAuth(VFS::Scopes::CERTS, "mqtt.cert.dir"))) {
    // /system is created at boot; only ensure the certs dir.
    VFS::mkdirGuarded("/system/certs", VFS::systemAuth(VFS::Scopes::CERTS, "mqtt.cert.dir"));
    INFO_MQTT_CONNECTIONF("Created /system/certs/ folder for certificates");
  }
  
  // Auto-switch ports
  if (newMode > 0 && oldMode == 0 && gSettings.mqttPort == 1883) {
    setSetting(gSettings.mqttPort, 8883);
  } else if (newMode == 0 && oldMode > 0 && gSettings.mqttPort == 8883) {
    setSetting(gSettings.mqttPort, 1883);
  }
  
  if (!ensureDebugBuffer()) return "Mode updated";
  const char* modeStr = "None";
  if (newMode == 1) modeStr = "TLS (no verification)";
  else if (newMode == 2) modeStr = "TLS + Verify";
  snprintf(getDebugBuffer(), 1024, "MQTT TLS Mode set to %d: %s (port: %d) - restart MQTT to apply",
           newMode, modeStr, gSettings.mqttPort);
  return getDebugBuffer();
}

const char* cmd_mqttcacertpath(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
    snprintf(getDebugBuffer(), 1024, "MQTT CA cert path: %s",
             gSettings.mqttCaCertPath.length() > 0 ? gSettings.mqttCaCertPath.c_str() : "(not set)");
    return getDebugBuffer();
  }
  if (arg == "clear" || arg == "none") {
    setSetting(gSettings.mqttCaCertPath, String(""));
    if (gSettings.mqttTlsMode == 2) {
      setSetting(gSettings.mqttTlsMode, 1);  // Downgrade to TLS without verify
    }
    return "MQTT CA cert path cleared";
  }
  // The caller (web admin or serial) needs read perm on the path they're
  // pointing MQTT at — currentAuthContext() resolves to this command's
  // installed identity. If they can't read it, telling MQTT to use it is
  // also denied.
  if (!VFS::existsGuarded(arg, currentAuthContext())) {
    if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
    snprintf(getDebugBuffer(), 1024, "Warning: File not found: %s (setting anyway)", arg.c_str());
    setSetting(gSettings.mqttCaCertPath, arg);
    return getDebugBuffer();
  }
  setSetting(gSettings.mqttCaCertPath, arg);
  if (!ensureDebugBuffer()) return "MQTT CA cert path updated";
  snprintf(getDebugBuffer(), 1024, "MQTT CA cert path set to: %s", arg.c_str());
  return getDebugBuffer();
}

const char* cmd_mqttuser(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
    snprintf(getDebugBuffer(), 1024, "MQTT user: %s", 
             gSettings.mqttUser.length() > 0 ? gSettings.mqttUser.c_str() : "(not set)");
    return getDebugBuffer();
  }
  if (arg == "clear" || arg == "none") {
    setSetting(gSettings.mqttUser, String(""));
    return "MQTT user cleared";
  }
  setSetting(gSettings.mqttUser, arg);
  if (!ensureDebugBuffer()) return "MQTT user updated";
  snprintf(getDebugBuffer(), 1024, "MQTT user set to: %s", arg.c_str());
  return getDebugBuffer();
}

const char* cmd_mqttpassword(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    return gSettings.mqttPassword.length() > 0 ? "MQTT password: ********" : "MQTT password: (not set)";
  }
  if (arg == "clear" || arg == "none") {
    setSetting(gSettings.mqttPassword, String(""));
    return "MQTT password cleared";
  }
  setSetting(gSettings.mqttPassword, arg);
  return "MQTT password updated";
}

const char* cmd_mqttbasetopic(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
    snprintf(getDebugBuffer(), 1024, "MQTT base topic: %s", 
             gSettings.mqttBaseTopic.length() > 0 ? gSettings.mqttBaseTopic.c_str() : "(auto-generated)");
    return getDebugBuffer();
  }
  if (arg == "clear" || arg == "auto") {
    setSetting(gSettings.mqttBaseTopic, String(""));
    return "MQTT base topic cleared (will auto-generate on connect)";
  }
  setSetting(gSettings.mqttBaseTopic, arg);
  if (!ensureDebugBuffer()) return "MQTT base topic updated";
  snprintf(getDebugBuffer(), 1024, "MQTT base topic set to: %s", arg.c_str());
  return getDebugBuffer();
}

const char* cmd_mqttdiscoveryprefix(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
    snprintf(getDebugBuffer(), 1024, "MQTT discovery prefix: %s", 
             gSettings.mqttDiscoveryPrefix.length() > 0 ? gSettings.mqttDiscoveryPrefix.c_str() : "homeassistant");
    return getDebugBuffer();
  }
  setSetting(gSettings.mqttDiscoveryPrefix, arg);
  if (!ensureDebugBuffer()) return "MQTT discovery prefix updated";
  snprintf(getDebugBuffer(), 1024, "MQTT discovery prefix set to: %s", arg.c_str());
  return getDebugBuffer();
}

const char* cmd_mqttpublishinterval(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
    snprintf(getDebugBuffer(), 1024, "MQTT publish interval: %d ms", gSettings.mqttPublishIntervalMs);
    return getDebugBuffer();
  }
  int interval = 0;
  if (!parseIntArg(arg, interval)) return kCommandReplyNotANumber;
  if (interval < 1000 || interval > 300000) {
    return "Error: Interval must be 1000-300000 ms";
  }
  setSetting(gSettings.mqttPublishIntervalMs, interval);
  if (!ensureDebugBuffer()) return "MQTT publish interval updated";
  snprintf(getDebugBuffer(), 1024, "MQTT publish interval set to: %d ms", interval);
  return getDebugBuffer();
}

const char* cmd_mqttsubscribe(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    return gSettings.mqttSubscribeExternal ? 
      "MQTT external subscriptions: enabled" : "MQTT external subscriptions: disabled";
  }
  const int parsed = parseBoolArg(arg);
  if (parsed < 0) return kCommandReplyNotOnOff;
  setSetting(gSettings.mqttSubscribeExternal, parsed == 1);
  return parsed == 1 ?
      "MQTT external subscriptions enabled - restart MQTT to apply" :
      "MQTT external subscriptions disabled";
}

const char* cmd_mqtttopics(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String arg = argsInput;
  arg.trim();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
    snprintf(getDebugBuffer(), 1024, "MQTT subscribe topics: %s", 
             gSettings.mqttSubscribeTopics.length() > 0 ? gSettings.mqttSubscribeTopics.c_str() : "(none)");
    return getDebugBuffer();
  }
  if (arg == "clear") {
    setSetting(gSettings.mqttSubscribeTopics, String(""));
    return "MQTT subscribe topics cleared";
  }
  setSetting(gSettings.mqttSubscribeTopics, arg);
  if (!ensureDebugBuffer()) return "MQTT subscribe topics updated";
  snprintf(getDebugBuffer(), 1024, "MQTT subscribe topics set to: %s", arg.c_str());
  return getDebugBuffer();
}

static constexpr char kMqttExternalSensorsBusy[] =
    "Error: External sensor list is busy. Retry mqttexternalsensors.";

const char* cmd_mqttexternalsensors(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    JsonArray arr = doc["sensors"].to<JsonArray>();
    // No mutex yet means no subscription has run: an empty list. A mutex that
    // is held is a failure, not an empty list.
    if (externalSensorMutex) {
      if (xSemaphoreTake(externalSensorMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return kMqttExternalSensorsBusy;
      }
      if (externalSensors && externalSensorCount > 0) {
        unsigned long now = millis();
        for (int i = 0; i < externalSensorCount; i++) {
          JsonObject o = arr.add<JsonObject>();
          o["name"]   = externalSensors[i].name;
          o["value"]  = externalSensors[i].value.substring(0, 64);  // ArduinoJson escapes untrusted MQTT data
          o["ageSec"] = (unsigned long)((now - externalSensors[i].lastUpdate) / 1000);
        }
      }
      xSemaphoreGive(externalSensorMutex);
    }
    doc["count"] = (int)arr.size();
    return mqttJsonReply(doc, "Error: External sensor list does not fit in one reply. "
                              "Reduce the topics in mqttsubscribetopics.");
  }

  if (!ensureDebugBuffer()) return kCommandReplyOutOfMemory;
  char* buf = getDebugBuffer();
  int pos = 0;

  if (!externalSensorMutex || !externalSensors) {
    return "No external sensors received";
  }
  
  if (xSemaphoreTake(externalSensorMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (!externalSensors || externalSensorCount == 0) {
      xSemaphoreGive(externalSensorMutex);
      return "No external sensors received";
    }
    pos += snprintf(buf + pos, 1024 - pos, "External Sensors (%d):\n", externalSensorCount);
    
    unsigned long now = millis();
    for (int i = 0; i < externalSensorCount && pos < 900; i++) {
      unsigned long age = (now - externalSensors[i].lastUpdate) / 1000;
      pos += snprintf(buf + pos, 1024 - pos, "  %s: %s (%lus ago)\n",
                      externalSensors[i].name.c_str(),
                      externalSensors[i].value.substring(0, 50).c_str(),
                      age);
    }
    xSemaphoreGive(externalSensorMutex);
  } else {
    return kMqttExternalSensorsBusy;
  }
  
  return buf;
}

// NOTE: cmd_debugmqtt is defined canonically in System_Debug.cpp as part of the
// debug sub-flag family (sets the runtime DEBUG_MQTT bit and supports
// [temp|runtime]). A duplicate non-static definition used to live here, which
// was a One Definition Rule violation; it has been removed. The `debugmqtt`
// command is registered by the debug module's command table.

// Helper macro for boolean publish settings
#define MQTT_PUBLISH_CMD(name, setting, label) \
const char* cmd_mqttpublish##name(const String& argsInput) { \
  RETURN_VALID_IF_VALIDATE_CSTR(); \
  String arg = argsInput; arg.trim(); \
  if (arg.length() == 0) { \
    return gSettings.setting ? "MQTT publish " label ": ON" : "MQTT publish " label ": OFF"; \
  } \
  const int parsed = parseBoolArg(arg); \
  if (parsed < 0) return kCommandReplyNotOnOff; \
  setSetting(gSettings.setting, parsed == 1); \
  return gSettings.setting ? "MQTT publish " label " enabled" : "MQTT publish " label " disabled"; \
}

MQTT_PUBLISH_CMD(wifi, mqttPublishWifi, "WiFi")
MQTT_PUBLISH_CMD(system, mqttPublishSystem, "System")
MQTT_PUBLISH_CMD(thermal, mqttPublishThermal, "Thermal")
MQTT_PUBLISH_CMD(tof, mqttPublishTof, "ToF")
MQTT_PUBLISH_CMD(imu, mqttPublishImu, "IMU")
MQTT_PUBLISH_CMD(presence, mqttPublishPresence, "Presence")
MQTT_PUBLISH_CMD(gps, mqttPublishGps, "GPS")
MQTT_PUBLISH_CMD(apds, mqttPublishApds, "APDS")
MQTT_PUBLISH_CMD(rtc, mqttPublishRtc, "RTC")
#if ENABLE_ANO_ENCODER
MQTT_PUBLISH_CMD(input, mqttPublishInput, "ANO Encoder")
#else
MQTT_PUBLISH_CMD(input, mqttPublishInput, "Gamepad")
#endif

// Command table - names must match setting keys for web UI compatibility
// Columns: name, help, requiresAdmin, handler, usage[, requiresSuperAdmin]
const CommandEntry mqttCommands[] = {
  // debugmqtt is registered by the debug module (System_Debug.cpp) — see ODR note above.
  { "openmqtt", "Start MQTT client", true, cmd_openmqtt },
  { "closemqtt", "Stop MQTT client", true, cmd_closemqtt },
  { "mqttstatus", "Show MQTT status (add 'json' for JSON output)", false, cmd_mqttstatus },
  { "mqttautostart", "MQTT auto-start [0|1]", true, cmd_mqttautostart, "Usage: mqttautostart [0|1]" },
  { "mqtthost", "MQTT broker host [hostname]", true, cmd_mqtthost, "Usage: mqtthost [hostname]" },
  { "mqttport", "MQTT broker port [port]", true, cmd_mqttport, "Usage: mqttport [port]" },
  { "mqtttlsmode", "TLS mode [0|1|2]", true, cmd_mqtttlsmode, "Usage: mqtttlsmode [0|1|2|none|tls|verify]" },
  { "mqttcacertpath", "CA cert path [path|clear]", true, cmd_mqttcacertpath, "Usage: mqttcacertpath [path|clear]" },
  { "mqttsubscribeexternal", "External subscriptions [0|1]", true, cmd_mqttsubscribe, "Usage: mqttsubscribeexternal [0|1]" },
  { "mqttsubscribetopics", "Subscribe topics [topics]", true, cmd_mqtttopics, "Usage: mqttsubscribetopics [topic1,topic2,...]" },
  { "mqttexternalsensors", "List external sensor data (add 'json' for JSON output)", false, cmd_mqttexternalsensors },
  { "mqttuser", "MQTT username [user|clear]", true, cmd_mqttuser, "Usage: mqttuser [username|clear]" },
  { "mqttpassword", "MQTT password [pass|clear]", true, cmd_mqttpassword, "Usage: mqttpassword [password|clear]" },
  { "mqttbasetopic", "Base topic [topic|auto]", true, cmd_mqttbasetopic, "Usage: mqttbasetopic [topic|auto]" },
  { "mqttdiscoveryprefix", "HA discovery prefix [prefix]", true, cmd_mqttdiscoveryprefix, "Usage: mqttdiscoveryprefix [prefix]" },
  { "mqttpublishintervalms", "Publish interval [ms]", true, cmd_mqttpublishinterval, "Usage: mqttpublishintervalms [1000-300000]" },
  { "mqttpublishwifi", "Publish WiFi [0|1]", true, cmd_mqttpublishwifi, "Usage: mqttpublishwifi [0|1]" },
  { "mqttpublishsystem", "Publish system [0|1]", true, cmd_mqttpublishsystem, "Usage: mqttpublishsystem [0|1]" },
  { "mqttpublishthermal", "Publish thermal [0|1]", true, cmd_mqttpublishthermal, "Usage: mqttpublishthermal [0|1]" },
  { "mqttpublishtof", "Publish ToF [0|1]", true, cmd_mqttpublishtof, "Usage: mqttpublishtof [0|1]" },
  { "mqttpublishimu", "Publish IMU [0|1]", true, cmd_mqttpublishimu, "Usage: mqttpublishimu [0|1]" },
  { "mqttpublishpresence", "Publish presence [0|1]", true, cmd_mqttpublishpresence, "Usage: mqttpublishpresence [0|1]" },
  { "mqttpublishgps", "Publish GPS [0|1]", true, cmd_mqttpublishgps, "Usage: mqttpublishgps [0|1]" },
  { "mqttpublishapds", "Publish APDS [0|1]", true, cmd_mqttpublishapds, "Usage: mqttpublishapds [0|1]" },
  { "mqttpublishrtc", "Publish RTC [0|1]", true, cmd_mqttpublishrtc, "Usage: mqttpublishrtc [0|1]" },
  { "mqttpublishinput", "Publish input device data [0|1]", true, cmd_mqttpublishinput, "Usage: mqttpublishinput [0|1]" }
};

const size_t mqttCommandsCount = sizeof(mqttCommands) / sizeof(mqttCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

#endif // ENABLE_WIFI && ENABLE_MQTT
