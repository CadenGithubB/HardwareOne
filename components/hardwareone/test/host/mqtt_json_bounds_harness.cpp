// Host harness for the MQTT discovery builders and the external-sensor copy.
// The runner inserts the production definitions from System_MQTT.cpp; the
// documents are built by System_MQTTPayloadCore.h (included as it ships, with
// the vendored ArduinoJson). Only String, the settings fields, the board name,
// SelfDevice, the client publish call, the sensor-table mutex and diagnostics
// are host boundaries.
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "System_MQTTPayloadCore.h"

class String : public std::string {
 public:
  using std::string::string;
  String() = default;
  String(const std::string& value) : std::string(value) {}
  String substring(size_t from) const {
    return from >= size() ? String() : String(substr(from));
  }
  String substring(size_t from, size_t to) const {
    if (from >= size() || to <= from) return String();
    return String(substr(from, to - from));
  }
  int lastIndexOf(char c) const {
    const size_t at = rfind(c);
    return at == npos ? -1 : (int)at;
  }
};

#define EXT_RAM_BSS_ATTR
#define PSRAM_JSON_DOC(name) JsonDocument name
#define BOARD_NAME "Seeed XIAO ESP32S3"
static int warnings = 0;
#define WARN_MQTTF(...) (++warnings)
#define DEBUG_MQTT_DISCOVERYF(...) ((void)0)
#define DEBUG_MQTT_PUBSUBF(...) ((void)0)
#define INFO_MQTT_PUBSUBF(...) ((void)0)

struct SettingsBoundary {
  String mqttBaseTopic;
  String mqttDiscoveryPrefix;
  String espnowFriendlyName;
  String espnowDeviceName;
  String espnowRoom;
} gSettings;

typedef void* esp_mqtt_client_handle_t;
static int clientToken;
static esp_mqtt_client_handle_t mqttClient = &clientToken;
static std::atomic<bool> mqttTofConnected{true};

struct Published {
  std::string topic;
  std::string payload;
  int qos;
  bool retain;
};
static std::vector<Published> published;

static int esp_mqtt_client_publish(esp_mqtt_client_handle_t client,
                                   const char* topic, const char* data,
                                   int len, int qos, int retain) {
  if (client != &clientToken) std::abort();
  const size_t length = len > 0 ? (size_t)len : std::strlen(data);
  published.push_back({topic, std::string(data, length), qos, retain != 0});
  return 1;
}

static String getDeviceId() { return String("hardwareone_0123456789AB"); }

namespace SelfDevice {
static String gDeviceName = "HardwareOne-B2C3";  // the name in use (fallback here)
static const char* firmwareVersion() { return "0.99.99.1"; }
static String deviceName() { return gDeviceName; }
}  // namespace SelfDevice

struct MeshPeerMeta {
  uint8_t mac[6];
  char name[32];
  char friendlyName[48];
  char room[32];
};

// External-sensor table boundary (the table itself is production state).
typedef int* SemaphoreHandle_t;
static int sensorMutexToken;
static int mutexDepth = 0;
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
static int xSemaphoreTake(SemaphoreHandle_t, int) { ++mutexDepth; return pdTRUE; }
static void xSemaphoreGive(SemaphoreHandle_t) { --mutexDepth; }
static unsigned long millis() { return 1234; }
enum { SYSEVT_MQTT_EXT_SENSOR_NEW = 1 };
static int newSensorEvents = 0;
static void systemEventPost(int, const char*) { ++newSensorEvents; }

// INSERT_PRODUCTION_DEFINITIONS

static int failures = 0;
static int checks = 0;
static void check(bool condition, const std::string& message) {
  ++checks;
  if (condition) return;
  ++failures;
  if (failures <= 30) std::fprintf(stderr, "FAIL: %s\n", message.c_str());
}

// The entity table row of `objectId` (System_MQTTPayloadCore.h).
static const mqttpayload::HaEntity& row(const char* objectId, uint8_t scope) {
  for (size_t i = 0; i < mqttpayload::kEntityCount; ++i) {
    const mqttpayload::HaEntity& e = mqttpayload::kEntities[i];
    if ((e.scope & scope) && std::strcmp(e.objectId, objectId) == 0) return e;
  }
  std::abort();
}

// JSON string text of `value` (only what these fixtures contain needs escaping).
static std::string escaped(const std::string& value) {
  std::string out;
  for (char c : value) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out;
}

// The exact document for a config that fits: the layout the builders always
// sent, with the model from the board and sw_version (U6, 2026-11-20).
static std::string expectedLocal(const char* objectId, const char* name,
                                 const char* tmpl, const char* unit,
                                 const char* deviceClass, const char* icon) {
  const std::string base = gSettings.mqttBaseTopic;
  std::string out = "{\"name\":\"" + std::string(name) +
                    "\",\"unique_id\":\"hardwareone_0123456789AB_" + objectId +
                    "\",\"state_topic\":\"" + base + "/state\",\"value_template\":\"" +
                    tmpl + "\"";
  if (unit && *unit) out += ",\"unit_of_measurement\":\"" + std::string(unit) + "\"";
  if (deviceClass && *deviceClass) out += ",\"device_class\":\"" + std::string(deviceClass) + "\"";
  if (icon && *icon) out += ",\"icon\":\"" + std::string(icon) + "\"";
  out += ",\"availability_topic\":\"" + base +
         "/availability\",\"payload_available\":\"online\",\"payload_not_available\":\"offline\"";
  std::string deviceName = SelfDevice::deviceName();
  if (!gSettings.espnowFriendlyName.empty()) deviceName = gSettings.espnowFriendlyName;
  out += ",\"device\":{\"identifiers\":[\"hardwareone_0123456789AB\"],\"name\":\"" +
         escaped(deviceName) + "\",\"model\":\"" BOARD_NAME "\",\"sw_version\":\"0.99.99.1\""
         ",\"manufacturer\":\"Custom\"";
  if (!gSettings.espnowRoom.empty()) out += ",\"suggested_area\":\"" + escaped(gSettings.espnowRoom) + "\"";
  out += "}}";
  return out;
}

static std::string expectedPeer(const MeshPeerMeta& peer, const char* objectId,
                                const char* name, const char* tmpl,
                                const char* unit, const char* deviceClass) {
  char mac[13];
  macToPathToken(peer.mac, mac);
  const std::string peerId = std::string("hardwareone_") + mac;
  const std::string base = gSettings.mqttBaseTopic;
  std::string out = "{\"name\":\"" + std::string(name) + "\",\"unique_id\":\"" + peerId +
                    "_" + objectId + "\",\"state_topic\":\"" + base + "/devices/" + peerId +
                    "/state\",\"value_template\":\"" + tmpl + "\"";
  if (unit && *unit) out += ",\"unit_of_measurement\":\"" + std::string(unit) + "\"";
  if (deviceClass && *deviceClass) out += ",\"device_class\":\"" + std::string(deviceClass) + "\"";
  out += ",\"availability_topic\":\"" + base + "/devices/" + peerId +
         "/availability\",\"payload_available\":\"online\",\"payload_not_available\":\"offline\"";
  // No model or sw_version: the master does not know a peer's board or
  // firmware. An unnamed peer gets the unknown-peer label, not its address.
  char label[devicename::kFieldLen];
  devicename::formatWithMacSuffix(devicename::kPeerPrefix, peer.mac, label);
  const std::string peerName = peer.friendlyName[0] ? peer.friendlyName
                               : (peer.name[0] ? peer.name : label);
  out += ",\"device\":{\"identifiers\":[\"" + peerId + "\"],\"name\":\"" + escaped(peerName) +
         "\",\"manufacturer\":\"Custom\",\"via_device\":\"hardwareone_0123456789AB\"";
  if (peer.room[0]) out += ",\"suggested_area\":\"" + std::string(peer.room) + "\"";
  out += "}}";
  return out;
}

static void resetSettings(const std::string& base) {
  gSettings.mqttBaseTopic = base;
  gSettings.mqttDiscoveryPrefix = "homeassistant";
  gSettings.espnowFriendlyName = "Bench Board";
  gSettings.espnowDeviceName = "bench";
  gSettings.espnowRoom = "Lab";
  SelfDevice::gDeviceName = "HardwareOne-B2C3";
  published.clear();
  warnings = 0;
}

static void test_local_discovery_fits_unchanged() {
  resetSettings("hardwareone/bench");
  publishDiscoveryConfig(row("uptime", mqttpayload::kLocal), mqttLocalDiscovery());
  check(published.size() == 1, "short base topic: one discovery config published");
  if (published.size() == 1) {
    check(published[0].topic == "homeassistant/sensor/hardwareone_0123456789AB/uptime/config",
          "discovery topic unchanged");
    check(published[0].payload == expectedLocal("uptime", "Uptime",
          "{{ value_json.system.uptime }}", "s", "duration", "mdi:timer-outline"),
          "discovery document unchanged");
    check(published[0].qos == 1 && published[0].retain, "discovery stays retained QoS 1");
  }
  check(warnings == 0, "no warning for a document that fits");

  // Free text is escaped: a quote or backslash in the friendly name or room
  // cannot break the document (it used to be printed with "%s").
  resetSettings("hardwareone/bench");
  gSettings.espnowFriendlyName = "Kid's \"Lab\" \\ desk";
  gSettings.espnowRoom = "Room \"7\"";
  publishDiscoveryConfig(row("heap_free", mqttpayload::kLocal), mqttLocalDiscovery());
  check(published.size() == 1 &&
            published[0].payload == expectedLocal("heap_free", "Heap Free",
                "{{ value_json.system.heap_free }}", "B", nullptr, "mdi:memory"),
        "a quote in the friendly name and room is escaped");
  JsonDocument parsed;
  check(published.size() == 1 && !deserializeJson(parsed, published[0].payload) &&
            std::string(parsed["device"]["name"] | "") == gSettings.espnowFriendlyName,
        "the escaped document parses back to the friendly name");

  // No friendly name: the device name in use (here the fallback), bare.
  resetSettings("hardwareone/bench");
  gSettings.espnowFriendlyName = "";
  publishDiscoveryConfig(row("uptime", mqttpayload::kLocal), mqttLocalDiscovery());
  check(published.size() == 1 &&
            published[0].payload.find("\"name\":\"HardwareOne-B2C3\",\"model\"") != std::string::npos,
        "unnamed device: HA device name is the fallback name");
}

static void test_local_discovery_long_base_topic_skipped() {
  resetSettings(std::string(400, 't'));
  publishDiscoveryConfig(row("uptime", mqttpayload::kLocal), mqttLocalDiscovery());
  check(published.empty(),
        "400-character base topic: a document that cannot fit is not published");
  for (const auto& message : published) {
    check(message.payload.size() < sizeof(sMqttDiscoveryConfigJson),
          "published document overran the discovery buffer");
  }
  check(warnings == 1, "400-character base topic: the skip is reported once");
}

// Walk the base-topic length across the capacity boundary: every document
// that fits (at most cap - 1 bytes) is published byte-exact; every longer one
// is skipped rather than sent truncated.
static void test_local_discovery_boundary() {
  bool sawFit = false, sawSkip = false;
  for (size_t length = 260; length <= 360; ++length) {
    resetSettings(std::string(length, 'b'));
    const std::string expected = expectedLocal("heap_free", "Heap Free",
        "{{ value_json.system.heap_free }}", "B", nullptr, "mdi:memory");
    publishDiscoveryConfig(row("heap_free", mqttpayload::kLocal), mqttLocalDiscovery());
    const bool fits = expected.size() < sizeof(sMqttDiscoveryConfigJson);
    sawFit |= fits;
    sawSkip |= !fits;
    if (fits) {
      check(published.size() == 1 && published[0].payload == expected,
            "boundary: fitting document published exactly (length " +
                std::to_string(expected.size()) + ")");
    } else {
      check(published.empty(), "boundary: oversized document skipped (length " +
                                   std::to_string(expected.size()) + ")");
    }
  }
  check(sawFit && sawSkip, "boundary sweep crosses the buffer capacity");
}

static MeshPeerMeta makePeer() {
  MeshPeerMeta peer{};
  const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
  std::memcpy(peer.mac, mac, sizeof(mac));
  std::snprintf(peer.name, sizeof(peer.name), "worker");
  std::snprintf(peer.friendlyName, sizeof(peer.friendlyName), "Hall Sensor");
  std::snprintf(peer.room, sizeof(peer.room), "Hall");
  return peer;
}

static void test_peer_discovery() {
  MeshPeerMeta peer = makePeer();
  const mqttpayload::HaEntity& tof = row("tof_distance", mqttpayload::kPeer);
  resetSettings("hardwareone/bench");
  publishPeerDiscoveryConfig(peer, tof, getDeviceId());
  check(published.size() == 1, "peer: short base topic publishes");
  if (published.size() == 1) {
    check(published[0].payload == expectedPeer(peer, "tof_distance", tof.name,
                                                tof.valueTemplate, "mm", "distance"),
          "peer discovery document");
    check(published[0].topic ==
              "homeassistant/sensor/hardwareone_020000000001/tof_distance/config",
          "peer discovery topic unchanged");
  }

  // An unnamed peer: the unknown-peer label, never the whole address.
  peer.friendlyName[0] = '\0';
  peer.name[0] = '\0';
  resetSettings("hardwareone/bench");
  publishPeerDiscoveryConfig(peer, tof, getDeviceId());
  check(published.size() == 1 &&
            published[0].payload == expectedPeer(peer, "tof_distance", tof.name,
                                                 tof.valueTemplate, "mm", "distance") &&
            published[0].payload.find("\"name\":\"peer-") != std::string::npos &&
            published[0].payload.find("\"name\":\"020000000001\"") == std::string::npos,
        "unnamed peer gets the unknown-peer label");
  peer = makePeer();

  resetSettings(std::string(400, 'p'));
  publishPeerDiscoveryConfig(peer, tof, getDeviceId());
  check(published.empty(), "peer: 400-character base topic is not published");
  check(warnings == 1, "peer: the skip is reported once");
}

// esp-mqtt topic/data pointers are length-delimited and not NUL-terminated.
// Copy them into exactly-sized heap buffers so any read past the length is an
// ASan heap-buffer-overflow, and include an embedded NUL in the payload.
static void test_external_sensor_exact_copy() {
  externalSensorMutex = &sensorMutexToken;
  ExternalSensor table[MAX_EXTERNAL_SENSORS];
  externalSensors = table;
  externalSensorCount = 0;
  newSensorEvents = 0;

  const char topicText[] = "home/lab/temperature";
  const int topicLen = (int)std::strlen(topicText);
  char* topic = new char[topicLen];
  std::memcpy(topic, topicText, topicLen);
  const char dataBytes[] = {'2', '1', '.', '5', '\0', 'C'};
  const int dataLen = (int)sizeof(dataBytes);
  char* data = new char[dataLen];
  std::memcpy(data, dataBytes, dataLen);

  updateExternalSensor(topic, topicLen, data, dataLen);
  check(externalSensorCount == 1, "external sensor added");
  check(table[0].topic.size() == (size_t)topicLen && table[0].topic == topicText,
        "topic copied with exactly topic_len bytes");
  check(table[0].name == "temperature", "name is the last topic segment");
  check(table[0].value.size() == (size_t)dataLen &&
            std::memcmp(table[0].value.data(), dataBytes, dataLen) == 0,
        "payload copied with exactly data_len bytes");
  check(newSensorEvents == 1, "new sensor event posted once");

  // A longer buffer whose topic_len covers only a prefix: the stored topic is
  // that prefix, and the same topic updates the existing row.
  const char longer[] = "home/lab/temperatureEXTRA";
  const char update[] = {'2', '2'};
  char* updateData = new char[sizeof(update)];
  std::memcpy(updateData, update, sizeof(update));
  updateExternalSensor(longer, topicLen, updateData, (int)sizeof(update));
  check(externalSensorCount == 1, "prefix of a longer buffer is the same topic");
  check(table[0].value == "22", "value updated with exactly data_len bytes");

  // Empty payloads and invalid lengths.
  updateExternalSensor(topic, topicLen, nullptr, 0);
  check(externalSensorCount == 1 && table[0].value.empty(), "empty payload stores empty value");
  updateExternalSensor(topic, 0, data, dataLen);
  check(externalSensorCount == 1, "zero-length topic ignored");
  check(mutexDepth == 0, "sensor mutex released on every path");

  delete[] topic;
  delete[] data;
  delete[] updateData;
  externalSensors = nullptr;
  externalSensorCount = 0;
}

// The client's own command topic (the {user, pass, cmd} envelope) and response
// topic (command output) are delivered to the same handler as the external
// subscriptions. Neither may be stored: `mqttexternalsensors` needs no admin.
static void test_control_topics_never_stored() {
  externalSensorMutex = &sensorMutexToken;
  ExternalSensor table[MAX_EXTERNAL_SENSORS];
  externalSensors = table;
  externalSensorCount = 0;
  newSensorEvents = 0;
  gSettings.mqttBaseTopic = "hardwareone/bench";
#if HW1_MQTT_CONTROL_TOPIC_PRESENT
  sMqttSubscribedBaseTopic = "hardwareone/bench";
#endif

  const std::string envelope = "{\"user\":\"u\",\"pass\":\"p\",\"cmd\":\"status\"}";
  auto deliver = [&](const std::string& topicText) {
    // Exactly-sized, not NUL-terminated, as esp-mqtt hands them over.
    char* topic = new char[topicText.size()];
    std::memcpy(topic, topicText.data(), topicText.size());
    char* data = new char[envelope.size()];
    std::memcpy(data, envelope.data(), envelope.size());
    updateExternalSensor(topic, (int)topicText.size(), data, (int)envelope.size());
    delete[] topic;
    delete[] data;
  };
  auto storedAnywhere = [&](const std::string& needle) {
    for (int i = 0; i < externalSensorCount; i++) {
      if (table[i].value.find(needle) != std::string::npos) return true;
    }
    return false;
  };

  deliver("hardwareone/bench/command");
  check(externalSensorCount == 0, "the command topic is not stored as a sensor");
  check(!storedAnywhere("\"pass\""), "no command envelope in the sensor table");
  deliver("hardwareone/bench/response");
  check(externalSensorCount == 0, "the response topic is not stored as a sensor");
  check(newSensorEvents == 0, "no new-sensor event for a control topic");
  check(mutexDepth == 0, "sensor mutex untouched or released for a control topic");

  // Only the exact two topics are dropped: neighbours are ordinary sensors.
  for (const char* topic : {"hardwareone/bench/command/extra", "hardwareone/bench/commands",
                            "hardwareone/bench/state", "hardwareone/other/command",
                            "xhardwareone/bench/command", "hardwareone/bench/respons"}) {
    const int before = externalSensorCount;
    deliver(topic);
    check(externalSensorCount == before + 1, std::string("stored as a sensor: ") + topic);
  }

  // `mqttbasetopic` changed the setting while the client stays subscribed to
  // the old command topic until the next connect: both bases are covered.
  gSettings.mqttBaseTopic = "hardwareone/renamed";
  const int before = externalSensorCount;
  deliver("hardwareone/bench/command");
  deliver("hardwareone/bench/response");
  deliver("hardwareone/renamed/command");
  deliver("hardwareone/renamed/response");
  check(externalSensorCount == before,
        "command and response topics of the subscribed and the configured base are dropped");
  // The six neighbour rows hold the envelope text by construction; none of
  // them is a control topic.
  for (int i = 0; i < externalSensorCount; i++) {
    const std::string& topic = table[i].topic;
    check(topic != "hardwareone/bench/command" && topic != "hardwareone/bench/response" &&
              topic != "hardwareone/renamed/command" && topic != "hardwareone/renamed/response",
          "no control topic among the stored rows: " + topic);
  }

  externalSensors = nullptr;
  externalSensorCount = 0;
  gSettings.mqttBaseTopic = "";
#if HW1_MQTT_CONTROL_TOPIC_PRESENT
  sMqttSubscribedBaseTopic = "";
#endif
}

int main(int argc, char** argv) {
  // An optional argument runs one group: discovery, peer or sensor.
  const std::string only = argc > 1 ? argv[1] : "";
  if (only.empty() || only == "discovery") {
    test_local_discovery_fits_unchanged();
    test_local_discovery_long_base_topic_skipped();
    test_local_discovery_boundary();
  }
  if (only.empty() || only == "peer") test_peer_discovery();
  if (only.empty() || only == "sensor") {
    test_external_sensor_exact_copy();
    test_control_topics_never_stored();
  }
  if (failures != 0) {
    std::fprintf(stderr, "%d of %d MQTT JSON bound checks failed\n", failures, checks);
    return 1;
  }
  std::printf("MQTT JSON bounds: %d checks passed\n", checks);
  return 0;
}
