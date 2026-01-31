/**
 * System_MQTT - Home Assistant MQTT Integration
 * 
 * Publishes sensor data to MQTT broker for Home Assistant integration.
 * Uses ESP-IDF's esp_mqtt_client for async connection management.
 */

#include "System_MQTT.h"
#include "System_BuildConfig.h"

#if ENABLE_WIFI && ENABLE_MQTT

#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <mqtt_client.h>
#include "System_Settings.h"
#include "System_Debug.h"
#include "System_Command.h"
#include "System_MemUtil.h"
#include "System_Auth.h"
#include <ArduinoJson.h>

// Forward declarations
extern Settings gSettings;
extern void broadcastOutput(const char* msg);
extern bool ensureDebugBuffer();
extern char* getDebugBuffer();

// MQTT state
static esp_mqtt_client_handle_t mqttClient = nullptr;
static bool mqttConnected = false;
static bool mqttEnabled = false;
static unsigned long lastPublishTime = 0;
static String lastError = "";

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
static ExternalSensor externalSensors[MAX_EXTERNAL_SENSORS];
static int externalSensorCount = 0;
static SemaphoreHandle_t externalSensorMutex = nullptr;

static void initExternalSensorStorage() {
  if (!externalSensorMutex) {
    externalSensorMutex = xSemaphoreCreateMutex();
  }
}

static void updateExternalSensor(const char* topic, int topicLen, const char* data, int dataLen) {
  if (!externalSensorMutex) return;
  
  String topicStr = String(topic).substring(0, topicLen);
  String dataStr = String(data).substring(0, dataLen);
  
  if (xSemaphoreTake(externalSensorMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    // Look for existing sensor
    for (int i = 0; i < externalSensorCount; i++) {
      if (externalSensors[i].topic == topicStr) {
        externalSensors[i].value = dataStr;
        externalSensors[i].lastUpdate = millis();
        xSemaphoreGive(externalSensorMutex);
        DEBUG_SYSTEMF("[MQTT] Updated sensor: %s", topicStr.c_str());
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
      INFO_SYSTEMF("[MQTT] New external sensor: %s", topicStr.c_str());
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
        INFO_SYSTEMF("[MQTT] Subscribed to: %s", topic.c_str());
      } else {
        WARN_SYSTEMF("[MQTT] Failed to subscribe: %s", topic.c_str());
      }
    }
    
    start = comma + 1;
  }
}

// Check if MQTT is connected
bool isMqttConnected() {
  return mqttConnected;
}

// Get external sensor count
int getExternalSensorCount() {
  return externalSensorCount;
}

// Get external sensor data (thread-safe)
bool getExternalSensor(int index, String& topic, String& name, String& value, unsigned long& lastUpdate) {
  if (!externalSensorMutex || index < 0 || index >= externalSensorCount) return false;
  
  if (xSemaphoreTake(externalSensorMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
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

static const SettingEntry mqttSettingEntries[] = {
  { "mqttAutoStart",          SETTING_BOOL,   &gSettings.mqttAutoStart,          false, 0, nullptr, 0, 1, "Auto-start at boot", nullptr, false },
  { "mqttHost",               SETTING_STRING, &gSettings.mqttHost,               0, 0, "", 0, 0, "Broker Host", nullptr, false },
  { "mqttPort",               SETTING_INT,    &gSettings.mqttPort,               1883, 0, nullptr, 1, 65535, "Broker Port", nullptr, false },
  { "mqttTLSMode",            SETTING_INT,    &gSettings.mqttTLSMode,            0, 0, nullptr, 0, 2, "TLS Mode (0=None, 1=TLS, 2=TLS+Verify)", nullptr, false },
  { "mqttCACertPath",         SETTING_STRING, &gSettings.mqttCACertPath,         0, 0, "/system/certs/mqtt_ca.crt", 0, 0, "CA certificate path", nullptr, false },
  { "mqttSubscribeExternal",  SETTING_BOOL,   &gSettings.mqttSubscribeExternal,  false, 0, nullptr, 0, 1, "Subscribe to external topics", nullptr, false },
  { "mqttSubscribeTopics",    SETTING_STRING, &gSettings.mqttSubscribeTopics,    0, 0, "", 0, 0, "Topics (comma-separated)", nullptr, false },
  { "mqttUser",               SETTING_STRING, &gSettings.mqttUser,               0, 0, "", 0, 0, "Username", nullptr, false },
  { "mqttPassword",           SETTING_STRING, &gSettings.mqttPassword,           0, 0, "", 0, 0, "Password", nullptr, true },
  { "mqttBaseTopic",          SETTING_STRING, &gSettings.mqttBaseTopic,          0, 0, "", 0, 0, "Base Topic", nullptr, false },
  { "mqttDiscoveryPrefix",    SETTING_STRING, &gSettings.mqttDiscoveryPrefix,    0, 0, "homeassistant", 0, 0, "Discovery Prefix", nullptr, false },
  { "mqttPublishIntervalMs",  SETTING_INT,    &gSettings.mqttPublishIntervalMs,  10000, 0, nullptr, 1000, 300000, "Publish Interval (ms)", nullptr, false },
  { "mqttPublishWiFi",        SETTING_BOOL,   &gSettings.mqttPublishWiFi,        true, 0, nullptr, 0, 1, "Publish WiFi info", nullptr, false },
  { "mqttPublishSystem",      SETTING_BOOL,   &gSettings.mqttPublishSystem,      true, 0, nullptr, 0, 1, "Publish system info", nullptr, false },
  { "mqttPublishThermal",     SETTING_BOOL,   &gSettings.mqttPublishThermal,     true, 0, nullptr, 0, 1, "Publish thermal data", nullptr, false },
  { "mqttPublishToF",         SETTING_BOOL,   &gSettings.mqttPublishToF,         true, 0, nullptr, 0, 1, "Publish ToF data", nullptr, false },
  { "mqttPublishIMU",         SETTING_BOOL,   &gSettings.mqttPublishIMU,         true, 0, nullptr, 0, 1, "Publish IMU data", nullptr, false },
  { "mqttPublishPresence",    SETTING_BOOL,   &gSettings.mqttPublishPresence,    true, 0, nullptr, 0, 1, "Publish presence data", nullptr, false },
  { "mqttPublishGPS",         SETTING_BOOL,   &gSettings.mqttPublishGPS,         true, 0, nullptr, 0, 1, "Publish GPS data", nullptr, false },
  { "mqttPublishAPDS",        SETTING_BOOL,   &gSettings.mqttPublishAPDS,        true, 0, nullptr, 0, 1, "Publish APDS data", nullptr, false },
  { "mqttPublishRTC",         SETTING_BOOL,   &gSettings.mqttPublishRTC,         true, 0, nullptr, 0, 1, "Publish RTC time", nullptr, false },
  { "mqttPublishGamepad",     SETTING_BOOL,   &gSettings.mqttPublishGamepad,     true, 0, nullptr, 0, 1, "Publish gamepad data", nullptr, false }
};

static bool isMqttAvailable() {
  return WiFi.isConnected();
}

extern const SettingsModule mqttSettingsModule = {
  "mqtt",
  "mqtt",
  mqttSettingEntries,
  sizeof(mqttSettingEntries) / sizeof(mqttSettingEntries[0]),
  isMqttAvailable,
  "MQTT broker connection for Home Assistant integration"
};

// ============================================================================
// Home Assistant MQTT Discovery
// ============================================================================

// Get device identifier (MAC-based)
static String getDeviceId() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char macStr[13];
  snprintf(macStr, sizeof(macStr), "%02x%02x%02x%02x%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String("hardwareone_") + macStr;
}

// Publish a single HA discovery config message
// component: "sensor", "binary_sensor", etc.
// objectId: unique suffix like "heap_free", "wifi_rssi"
// name: friendly name shown in HA
// valueTemplate: Jinja template to extract value from JSON state
// unit: unit of measurement (nullptr if none)
// deviceClass: HA device class (nullptr if none)
// icon: MDI icon (nullptr for default)
static void publishDiscoveryConfig(const char* component, const char* objectId,
                                    const char* name, const char* valueTemplate,
                                    const char* unit, const char* deviceClass,
                                    const char* icon) {
  if (!mqttClient || !mqttConnected) return;
  
  String deviceId = getDeviceId();
  String stateTopic = gSettings.mqttBaseTopic + "/state";
  String availTopic = gSettings.mqttBaseTopic + "/availability";
  String uniqueId = deviceId + "_" + objectId;
  
  // Build discovery topic: homeassistant/<component>/<device_id>/<object_id>/config
  String discoveryTopic = gSettings.mqttDiscoveryPrefix + "/" + component + "/" + 
                          deviceId + "/" + objectId + "/config";
  
  // Build config JSON
  char configJson[1024];
  int pos = 0;
  
  pos += snprintf(configJson + pos, sizeof(configJson) - pos,
    "{\"name\":\"%s\",\"unique_id\":\"%s\",\"state_topic\":\"%s\",\"value_template\":\"%s\"",
    name, uniqueId.c_str(), stateTopic.c_str(), valueTemplate);
  
  if (unit && strlen(unit) > 0) {
    pos += snprintf(configJson + pos, sizeof(configJson) - pos, ",\"unit_of_measurement\":\"%s\"", unit);
  }
  if (deviceClass && strlen(deviceClass) > 0) {
    pos += snprintf(configJson + pos, sizeof(configJson) - pos, ",\"device_class\":\"%s\"", deviceClass);
  }
  if (icon && strlen(icon) > 0) {
    pos += snprintf(configJson + pos, sizeof(configJson) - pos, ",\"icon\":\"%s\"", icon);
  }
  
  // Add availability
  pos += snprintf(configJson + pos, sizeof(configJson) - pos,
    ",\"availability_topic\":\"%s\",\"payload_available\":\"online\",\"payload_not_available\":\"offline\"",
    availTopic.c_str());
  
  // Add device info (groups all sensors under one device in HA)
  pos += snprintf(configJson + pos, sizeof(configJson) - pos,
    ",\"device\":{\"identifiers\":[\"%s\"],\"name\":\"HardwareOne\",\"model\":\"ESP32-S3\",\"manufacturer\":\"Custom\"}",
    deviceId.c_str());
  
  pos += snprintf(configJson + pos, sizeof(configJson) - pos, "}");
  
  // Publish with retain so HA picks it up even if it restarts
  esp_mqtt_client_publish(mqttClient, discoveryTopic.c_str(), configJson, 0, 1, true);
  DEBUG_SYSTEMF("[MQTT] Discovery: %s", objectId);
}

// Subscribe to command topic for receiving commands from HA
static void subscribeToCommandTopic() {
  if (!mqttClient || !mqttConnected) return;
  
  String commandTopic = gSettings.mqttBaseTopic + "/command";
  int msgId = esp_mqtt_client_subscribe(mqttClient, commandTopic.c_str(), 1);
  if (msgId >= 0) {
    INFO_SYSTEMF("[MQTT] Subscribed to command topic: %s", commandTopic.c_str());
  } else {
    WARN_SYSTEMF("[MQTT] Failed to subscribe to command topic");
  }
}

// Handle incoming MQTT command
// Expected JSON format: {"user":"username","pass":"password","cmd":"command"}
static void handleMQTTCommand(const char* topic, int topicLen, const char* data, int dataLen) {
  String commandTopic = gSettings.mqttBaseTopic + "/command";
  String responseTopic = gSettings.mqttBaseTopic + "/response";
  
  // Check if this is our command topic
  if (topicLen != (int)commandTopic.length() || 
      strncmp(topic, commandTopic.c_str(), topicLen) != 0) {
    return;  // Not our command topic
  }
  
  // Extract payload string
  String payload;
  payload.reserve(dataLen + 1);
  for (int i = 0; i < dataLen; i++) {
    payload += data[i];
  }
  payload.trim();
  
  if (payload.length() == 0) return;
  
  DEBUG_SYSTEMF("[MQTT] Command payload: %s", payload.c_str());
  
  // Parse JSON payload
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    WARN_SYSTEMF("[MQTT] Command JSON parse error: %s", err.c_str());
    esp_mqtt_client_publish(mqttClient, responseTopic.c_str(), 
      "{\"ok\":false,\"error\":\"Invalid JSON format\"}", 0, 0, false);
    return;
  }
  
  const char* username = doc["user"] | "";
  const char* password = doc["pass"] | "";
  const char* command = doc["cmd"] | "";
  
  // Validate required fields
  if (strlen(username) == 0 || strlen(password) == 0 || strlen(command) == 0) {
    WARN_SYSTEMF("[MQTT] Command missing user/pass/cmd fields");
    esp_mqtt_client_publish(mqttClient, responseTopic.c_str(),
      "{\"ok\":false,\"error\":\"Missing user, pass, or cmd field\"}", 0, 0, false);
    return;
  }
  
  INFO_SYSTEMF("[MQTT] Command from user '%s': %s", username, command);
  
  // Authenticate user
  if (!isValidUser(String(username), String(password))) {
    WARN_SYSTEMF("[MQTT] Authentication FAILED for user '%s'", username);
    esp_mqtt_client_publish(mqttClient, responseTopic.c_str(),
      "{\"ok\":false,\"error\":\"Authentication failed\"}", 0, 0, false);
    return;
  }
  
  DEBUG_SYSTEMF("[MQTT] Authentication successful for user '%s'", username);
  
  // Set up auth context for command execution
  AuthContext ctx;
  ctx.transport = SOURCE_MQTT;
  ctx.user = username;
  ctx.ip = "mqtt:" + gSettings.mqttHost;
  ctx.path = "/mqtt/command";
  ctx.opaque = nullptr;
  
  // Execute command with auth context
  static char cmdResult[2048];
  bool success = executeCommand(ctx, command, cmdResult, sizeof(cmdResult));
  
  // Build and publish response
  JsonDocument respDoc;
  respDoc["ok"] = success;
  respDoc["user"] = username;
  respDoc["cmd"] = command;
  if (success) {
    respDoc["result"] = cmdResult;
  } else {
    respDoc["error"] = strlen(cmdResult) > 0 ? cmdResult : "Command execution failed";
  }
  
  String respStr;
  serializeJson(respDoc, respStr);
  esp_mqtt_client_publish(mqttClient, responseTopic.c_str(), respStr.c_str(), 0, 0, false);
  
  DEBUG_SYSTEMF("[MQTT] Command response: ok=%d", success);
}

// Publish all discovery configs for enabled sensors
static void publishMQTTDiscovery() {
  if (!mqttConnected || gSettings.mqttDiscoveryPrefix.length() == 0) return;
  
  INFO_SYSTEMF("[MQTT] Publishing Home Assistant discovery configs...");
  
  // System sensors (always available)
  if (gSettings.mqttPublishSystem) {
    publishDiscoveryConfig("sensor", "uptime", "Uptime", "{{ value_json.system.uptime }}", "s", "duration", "mdi:timer-outline");
    publishDiscoveryConfig("sensor", "heap_free", "Heap Free", "{{ value_json.system.heap_free }}", "B", nullptr, "mdi:memory");
    publishDiscoveryConfig("sensor", "heap_min", "Heap Min", "{{ value_json.system.heap_min }}", "B", nullptr, "mdi:memory");
  }
  
  // WiFi sensors
  if (gSettings.mqttPublishWiFi) {
    publishDiscoveryConfig("sensor", "wifi_rssi", "WiFi RSSI", "{{ value_json.wifi.rssi }}", "dBm", "signal_strength", nullptr);
    publishDiscoveryConfig("sensor", "wifi_ssid", "WiFi SSID", "{{ value_json.wifi.ssid }}", nullptr, nullptr, "mdi:wifi");
    publishDiscoveryConfig("sensor", "wifi_ip", "WiFi IP", "{{ value_json.wifi.ip }}", nullptr, nullptr, "mdi:ip-network");
  }
  
#if ENABLE_THERMAL_SENSOR
  if (gSettings.mqttPublishThermal) {
    publishDiscoveryConfig("sensor", "thermal_min", "Thermal Min", "{{ value_json.thermal.min_temp }}", "°C", "temperature", nullptr);
    publishDiscoveryConfig("sensor", "thermal_max", "Thermal Max", "{{ value_json.thermal.max_temp }}", "°C", "temperature", nullptr);
    publishDiscoveryConfig("sensor", "thermal_avg", "Thermal Avg", "{{ value_json.thermal.avg_temp }}", "°C", "temperature", nullptr);
  }
#endif

#if ENABLE_TOF_SENSOR
  if (gSettings.mqttPublishToF) {
    publishDiscoveryConfig("sensor", "tof_distance", "ToF Distance", "{{ value_json.tof.distance }}", "mm", "distance", nullptr);
  }
#endif

#if ENABLE_IMU_SENSOR
  if (gSettings.mqttPublishIMU) {
    publishDiscoveryConfig("sensor", "imu_accel_x", "IMU Accel X", "{{ value_json.imu.accel_x }}", "m/s²", nullptr, "mdi:axis-x-arrow");
    publishDiscoveryConfig("sensor", "imu_accel_y", "IMU Accel Y", "{{ value_json.imu.accel_y }}", "m/s²", nullptr, "mdi:axis-y-arrow");
    publishDiscoveryConfig("sensor", "imu_accel_z", "IMU Accel Z", "{{ value_json.imu.accel_z }}", "m/s²", nullptr, "mdi:axis-z-arrow");
    publishDiscoveryConfig("sensor", "imu_gyro_x", "IMU Gyro X", "{{ value_json.imu.gyro_x }}", "°/s", nullptr, "mdi:rotate-3d-variant");
    publishDiscoveryConfig("sensor", "imu_gyro_y", "IMU Gyro Y", "{{ value_json.imu.gyro_y }}", "°/s", nullptr, "mdi:rotate-3d-variant");
    publishDiscoveryConfig("sensor", "imu_gyro_z", "IMU Gyro Z", "{{ value_json.imu.gyro_z }}", "°/s", nullptr, "mdi:rotate-3d-variant");
  }
#endif

#if ENABLE_PRESENCE_SENSOR
  if (gSettings.mqttPublishPresence) {
    publishDiscoveryConfig("binary_sensor", "presence_detected", "Presence Detected", "{{ value_json.presence.detected }}", nullptr, "presence", nullptr);
    publishDiscoveryConfig("binary_sensor", "motion_detected", "Motion Detected", "{{ value_json.presence.motion }}", nullptr, "motion", nullptr);
    publishDiscoveryConfig("sensor", "presence_ambient", "Presence Ambient Temp", "{{ value_json.presence.ambient_temp }}", "°C", "temperature", nullptr);
    publishDiscoveryConfig("sensor", "presence_object", "Presence Object Temp", "{{ value_json.presence.object_temp }}", "°C", "temperature", nullptr);
  }
#endif

#if ENABLE_GPS_SENSOR
  if (gSettings.mqttPublishGPS) {
    publishDiscoveryConfig("sensor", "gps_latitude", "GPS Latitude", "{{ value_json.gps.lat }}", "°", nullptr, "mdi:crosshairs-gps");
    publishDiscoveryConfig("sensor", "gps_longitude", "GPS Longitude", "{{ value_json.gps.lon }}", "°", nullptr, "mdi:crosshairs-gps");
    publishDiscoveryConfig("sensor", "gps_altitude", "GPS Altitude", "{{ value_json.gps.alt }}", "m", nullptr, "mdi:altimeter");
    publishDiscoveryConfig("sensor", "gps_speed", "GPS Speed", "{{ value_json.gps.speed }}", "km/h", nullptr, "mdi:speedometer");
    publishDiscoveryConfig("sensor", "gps_satellites", "GPS Satellites", "{{ value_json.gps.satellites }}", nullptr, nullptr, "mdi:satellite-variant");
  }
#endif

#if ENABLE_APDS_SENSOR
  if (gSettings.mqttPublishAPDS) {
    publishDiscoveryConfig("sensor", "apds_proximity", "Proximity", "{{ value_json.apds.proximity }}", nullptr, nullptr, "mdi:hand-wave");
    publishDiscoveryConfig("sensor", "apds_color_r", "Color Red", "{{ value_json.apds.color.r }}", nullptr, nullptr, "mdi:palette");
    publishDiscoveryConfig("sensor", "apds_color_g", "Color Green", "{{ value_json.apds.color.g }}", nullptr, nullptr, "mdi:palette");
    publishDiscoveryConfig("sensor", "apds_color_b", "Color Blue", "{{ value_json.apds.color.b }}", nullptr, nullptr, "mdi:palette");
  }
#endif

#if ENABLE_RTC_SENSOR
  if (gSettings.mqttPublishRTC) {
    publishDiscoveryConfig("sensor", "rtc_datetime", "RTC DateTime", "{{ value_json.rtc.datetime }}", nullptr, "timestamp", "mdi:clock-outline");
    publishDiscoveryConfig("sensor", "rtc_temperature", "RTC Temperature", "{{ value_json.rtc.temperature }}", "°C", "temperature", nullptr);
  }
#endif

#if ENABLE_GAMEPAD_SENSOR
  if (gSettings.mqttPublishGamepad) {
    publishDiscoveryConfig("sensor", "gamepad_x", "Gamepad X", "{{ value_json.gamepad.x }}", nullptr, nullptr, "mdi:gamepad-variant");
    publishDiscoveryConfig("sensor", "gamepad_y", "Gamepad Y", "{{ value_json.gamepad.y }}", nullptr, nullptr, "mdi:gamepad-variant");
    publishDiscoveryConfig("sensor", "gamepad_buttons", "Gamepad Buttons", "{{ value_json.gamepad.buttons }}", nullptr, nullptr, "mdi:gamepad");
  }
#endif

  INFO_SYSTEMF("[MQTT] Discovery configs published");
}

// ============================================================================
// MQTT Event Handler
// ============================================================================

static void mqtt_event_handler(void* handler_args, esp_event_base_t base, int32_t event_id, void* event_data) {
  esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
  
  switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
      mqttConnected = true;
      lastError = "";
      broadcastOutput("[MQTT] Connected to broker");
      INFO_SYSTEMF("[MQTT] Connected to %s:%d", gSettings.mqttHost.c_str(), gSettings.mqttPort);
      
      // Publish availability
      if (gSettings.mqttBaseTopic.length() > 0) {
        String availTopic = gSettings.mqttBaseTopic + "/availability";
        esp_mqtt_client_publish(mqttClient, availTopic.c_str(), "online", 0, 1, true);
      }
      
      // Publish Home Assistant discovery configs
      publishMQTTDiscovery();
      
      // Subscribe to command topic for HA commands
      subscribeToCommandTopic();
      
      // Subscribe to external topics if enabled
      subscribeToExternalTopics();
      break;
      
    case MQTT_EVENT_DISCONNECTED:
      mqttConnected = false;
      WARN_SYSTEMF("[MQTT] Disconnected from broker");
      break;
      
    case MQTT_EVENT_ERROR:
      mqttConnected = false;
      lastError = "Connection error";
      ERROR_SYSTEMF("[MQTT] Error event");
      break;
      
    case MQTT_EVENT_DATA:
      // Check for command topic first
      handleMQTTCommand(event->topic, event->topic_len, event->data, event->data_len);
      
      // Store incoming data as external sensor if subscriptions enabled
      if (gSettings.mqttSubscribeExternal && event->topic_len > 0) {
        updateExternalSensor(event->topic, event->topic_len, event->data, event->data_len);
      }
      DEBUG_SYSTEMF("[MQTT] Received: topic=%.*s data=%.*s",
                    event->topic_len, event->topic,
                    event->data_len, event->data);
      break;
      
    default:
      break;
  }
}

// ============================================================================
// MQTT Lifecycle Functions
// ============================================================================

bool startMQTT() {
  if (mqttEnabled) {
    return true;
  }
  
  // Initialize external sensor storage
  initExternalSensorStorage();
  
  if (!WiFi.isConnected()) {
    lastError = "WiFi not connected";
    return false;
  }
  
  if (gSettings.mqttHost.length() == 0) {
    lastError = "MQTT host not configured";
    return false;
  }
  
  // Generate base topic if empty
  if (gSettings.mqttBaseTopic.length() == 0) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char macStr[13];
    snprintf(macStr, sizeof(macStr), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    gSettings.mqttBaseTopic = String("hardwareone/") + macStr;
    extern bool writeSettingsJson();
    writeSettingsJson();
    INFO_SYSTEMF("[MQTT] Auto-generated base topic: %s", gSettings.mqttBaseTopic.c_str());
  }
  
  // Configure MQTT client - strings must persist until esp_mqtt_client_init() completes
  String brokerUri;
  bool useTLS = (gSettings.mqttTLSMode > 0);
  if (useTLS) {
    brokerUri = String("mqtts://") + gSettings.mqttHost + ":" + String(gSettings.mqttPort);
  } else {
    brokerUri = String("mqtt://") + gSettings.mqttHost + ":" + String(gSettings.mqttPort);
  }
  String availTopic = gSettings.mqttBaseTopic + "/availability";
  
  esp_mqtt_client_config_t mqtt_cfg = {};
  mqtt_cfg.broker.address.uri = brokerUri.c_str();
  
  // TLS configuration based on mqttTLSMode: 0=None, 1=TLS (no verify), 2=TLS+Verify
  static String caCertData;  // Must persist for MQTT client lifetime
  if (gSettings.mqttTLSMode == 2) {
    // TLS + Certificate Verification
    if (gSettings.mqttCACertPath.length() > 0) {
      File certFile = LittleFS.open(gSettings.mqttCACertPath, "r");
      if (certFile) {
        caCertData = certFile.readString();
        certFile.close();
        mqtt_cfg.broker.verification.certificate = caCertData.c_str();
        mqtt_cfg.broker.verification.skip_cert_common_name_check = false;
        INFO_SYSTEMF("[MQTT] TLS + Verify: using %s", gSettings.mqttCACertPath.c_str());
      } else {
        lastError = "CA cert file not found: " + gSettings.mqttCACertPath;
        ERROR_SYSTEMF("[MQTT] %s", lastError.c_str());
        return false;
      }
    } else {
      lastError = "TLS+Verify requires CA cert path";
      ERROR_SYSTEMF("[MQTT] %s", lastError.c_str());
      return false;
    }
  } else if (gSettings.mqttTLSMode == 1) {
    // TLS without certificate verification (encrypted but trusts any server)
    mqtt_cfg.broker.verification.skip_cert_common_name_check = true;
    mqtt_cfg.broker.verification.certificate = nullptr;
    INFO_SYSTEMF("[MQTT] TLS enabled (no cert verification)");
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
  
  mqttClient = esp_mqtt_client_init(&mqtt_cfg);
  if (!mqttClient) {
    lastError = "Failed to initialize MQTT client";
    ERROR_SYSTEMF("[MQTT] Client init failed");
    return false;
  }
  
  esp_mqtt_client_register_event(mqttClient, MQTT_EVENT_ANY, mqtt_event_handler, nullptr);
  esp_mqtt_client_start(mqttClient);
  
  mqttEnabled = true;
  lastError = "";
  broadcastOutput("[MQTT] Client started");
  INFO_SYSTEMF("[MQTT] Connecting to %s://%s:%d", (gSettings.mqttTLSMode > 0) ? "mqtts" : "mqtt", 
               gSettings.mqttHost.c_str(), gSettings.mqttPort);
  
  return true;
}

void stopMQTT() {
  if (!mqttEnabled) {
    return;
  }
  
  if (mqttClient) {
    // Publish offline before disconnecting
    if (mqttConnected && gSettings.mqttBaseTopic.length() > 0) {
      String availTopic = gSettings.mqttBaseTopic + "/availability";
      esp_mqtt_client_publish(mqttClient, availTopic.c_str(), "offline", 0, 1, true);
    }
    
    esp_mqtt_client_stop(mqttClient);
    esp_mqtt_client_destroy(mqttClient);
    mqttClient = nullptr;
  }
  
  mqttEnabled = false;
  mqttConnected = false;
  broadcastOutput("[MQTT] Client stopped");
}

// ============================================================================
// MQTT Publishing
// ============================================================================

void publishMQTTSensorData() {
  if (!mqttConnected || !mqttClient) {
    return;
  }
  
  // Build sensor data JSON blob from caches
  char* jsonBuf = (char*)ps_alloc(16384, AllocPref::PreferPSRAM, "mqtt.json");
  if (!jsonBuf) {
    WARN_SYSTEMF("[MQTT] Failed to allocate JSON buffer");
    return;
  }
  
  // Start JSON object
  int pos = snprintf(jsonBuf, 16384, "{");
  
  // Add timestamp
  pos += snprintf(jsonBuf + pos, 16384 - pos, "\"timestamp\":%lu", millis());
  
  // Add system info (uptime, heap, etc.)
  if (gSettings.mqttPublishSystem) {
    pos += snprintf(jsonBuf + pos, 16384 - pos, 
      ",\"system\":{\"uptime\":%lu,\"heap_free\":%lu,\"heap_min\":%lu}",
      millis() / 1000, (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMinFreeHeap());
  }
  
  // Add WiFi info
  if (gSettings.mqttPublishWiFi && WiFi.isConnected()) {
    pos += snprintf(jsonBuf + pos, 16384 - pos, ",\"wifi\":{\"ssid\":\"%s\",\"rssi\":%d,\"ip\":\"%s\"}",
                    WiFi.SSID().c_str(), WiFi.RSSI(), WiFi.localIP().toString().c_str());
  }
  
  // Add sensor data from caches (only if sensors are enabled and configured to publish)
#if ENABLE_THERMAL_SENSOR
  extern bool thermalEnabled;
  extern int buildThermalDataJSON(char* buf, size_t bufSize);
  if (gSettings.mqttPublishThermal && thermalEnabled) {
    char thermalJson[2048];
    int len = buildThermalDataJSON(thermalJson, sizeof(thermalJson));
    if (len > 0) {
      pos += snprintf(jsonBuf + pos, 16384 - pos, ",\"thermal\":%s", thermalJson);
    }
  }
#endif

#if ENABLE_TOF_SENSOR
  extern bool tofEnabled;
  extern int buildToFDataJSON(char* buf, size_t bufSize);
  if (gSettings.mqttPublishToF && tofEnabled) {
    char tofJson[1024];
    int len = buildToFDataJSON(tofJson, sizeof(tofJson));
    if (len > 0) {
      pos += snprintf(jsonBuf + pos, 16384 - pos, ",\"tof\":%s", tofJson);
    }
  }
#endif

#if ENABLE_IMU_SENSOR
  extern bool imuEnabled;
  extern int buildIMUDataJSON(char* buf, size_t bufSize);
  if (gSettings.mqttPublishIMU && imuEnabled) {
    char imuJson[1024];
    int len = buildIMUDataJSON(imuJson, sizeof(imuJson));
    if (len > 0) {
      pos += snprintf(jsonBuf + pos, 16384 - pos, ",\"imu\":%s", imuJson);
    }
  }
#endif

#if ENABLE_PRESENCE_SENSOR
  extern bool presenceEnabled;
  extern struct PresenceCache { 
    float presence; float motion; float ambientTemp; float objectTemp; 
    bool motionDetected; bool presenceDetected; bool dataValid; 
  } gPresenceCache;
  if (gSettings.mqttPublishPresence && presenceEnabled && gPresenceCache.dataValid) {
    pos += snprintf(jsonBuf + pos, 16384 - pos, 
      ",\"presence\":{\"detected\":%s,\"motion\":%s,\"presence_raw\":%.1f,\"motion_raw\":%.1f,\"ambient_temp\":%.1f,\"object_temp\":%.1f}",
      gPresenceCache.presenceDetected ? "true" : "false",
      gPresenceCache.motionDetected ? "true" : "false",
      gPresenceCache.presence, gPresenceCache.motion,
      gPresenceCache.ambientTemp, gPresenceCache.objectTemp);
  }
#endif

#if ENABLE_GPS_SENSOR
  extern bool gpsEnabled;
  extern bool gpsConnected;
  class Adafruit_GPS;
  extern Adafruit_GPS* gPA1010D;
  if (gSettings.mqttPublishGPS && gpsEnabled && gpsConnected && gPA1010D) {
    extern float getGPSLatitude();
    extern float getGPSLongitude();
    extern float getGPSAltitude();
    extern float getGPSSpeed();
    extern int getGPSSatellites();
    extern bool hasGPSFix();
    if (hasGPSFix()) {
      pos += snprintf(jsonBuf + pos, 16384 - pos,
        ",\"gps\":{\"lat\":%.6f,\"lon\":%.6f,\"alt\":%.1f,\"speed\":%.1f,\"satellites\":%d}",
        getGPSLatitude(), getGPSLongitude(), getGPSAltitude(), getGPSSpeed(), getGPSSatellites());
    } else {
      pos += snprintf(jsonBuf + pos, 16384 - pos, ",\"gps\":{\"fix\":false,\"satellites\":%d}", getGPSSatellites());
    }
  }
#endif

#if ENABLE_APDS_SENSOR
  extern bool apdsEnabled;
  extern bool apdsConnected;
  if (gSettings.mqttPublishAPDS && apdsEnabled && apdsConnected) {
    extern uint8_t getAPDSProximity();
    extern uint16_t getAPDSColorR();
    extern uint16_t getAPDSColorG();
    extern uint16_t getAPDSColorB();
    extern uint16_t getAPDSColorC();
    pos += snprintf(jsonBuf + pos, 16384 - pos,
      ",\"apds\":{\"proximity\":%u,\"color\":{\"r\":%u,\"g\":%u,\"b\":%u,\"c\":%u}}",
      getAPDSProximity(), getAPDSColorR(), getAPDSColorG(), getAPDSColorB(), getAPDSColorC());
  }
#endif

#if ENABLE_RTC_SENSOR
  extern bool rtcEnabled;
  extern bool rtcConnected;
  if (gSettings.mqttPublishRTC && rtcEnabled && rtcConnected) {
    extern int getRTCYear();
    extern int getRTCMonth();
    extern int getRTCDay();
    extern int getRTCHour();
    extern int getRTCMinute();
    extern int getRTCSecond();
    extern float getRTCTemperature();
    char timeBuf[32];
    snprintf(timeBuf, sizeof(timeBuf), "%04d-%02d-%02dT%02d:%02d:%02d",
             getRTCYear(), getRTCMonth(), getRTCDay(),
             getRTCHour(), getRTCMinute(), getRTCSecond());
    pos += snprintf(jsonBuf + pos, 16384 - pos,
      ",\"rtc\":{\"datetime\":\"%s\",\"temperature\":%.1f}",
      timeBuf, getRTCTemperature());
  }
#endif

#if ENABLE_GAMEPAD_SENSOR
  extern bool gamepadEnabled;
  extern bool gamepadConnected;
  if (gSettings.mqttPublishGamepad && gamepadEnabled && gamepadConnected) {
    extern int getGamepadX();
    extern int getGamepadY();
    extern uint32_t getGamepadButtons();
    pos += snprintf(jsonBuf + pos, 16384 - pos,
      ",\"gamepad\":{\"x\":%d,\"y\":%d,\"buttons\":%lu}",
      getGamepadX(), getGamepadY(), (unsigned long)getGamepadButtons());
  }
#endif
  
  // Close JSON object
  pos += snprintf(jsonBuf + pos, 16384 - pos, "}");
  
  // Publish to state topic
  String stateTopic = gSettings.mqttBaseTopic + "/state";
  int msg_id = esp_mqtt_client_publish(mqttClient, stateTopic.c_str(), jsonBuf, 0, 0, false);
  
  if (msg_id >= 0) {
    DEBUG_SYSTEMF("[MQTT] Published %d bytes to %s", pos, stateTopic.c_str());
  } else {
    WARN_SYSTEMF("[MQTT] Publish failed");
  }
  
  free(jsonBuf);
}

void mqttTick() {
  if (!mqttEnabled || !mqttClient) {
    return;
  }
  
  // Periodic publishing
  unsigned long now = millis();
  if (mqttConnected && (now - lastPublishTime >= gSettings.mqttPublishIntervalMs)) {
    publishMQTTSensorData();
    lastPublishTime = now;
  }
}

// ============================================================================
// MQTT CLI Commands
// ============================================================================

const char* cmd_openmqtt(const String& args) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  if (mqttEnabled) {
    return "[MQTT] Already running";
  }
  
  if (startMQTT()) {
    return "[MQTT] Client started, connecting...";
  } else {
    if (!ensureDebugBuffer()) return "[MQTT] Start failed";
    snprintf(getDebugBuffer(), 1024, "[MQTT] Start failed: %s", lastError.c_str());
    return getDebugBuffer();
  }
}

const char* cmd_closemqtt(const String& args) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  if (!mqttEnabled) {
    return "[MQTT] Not running";
  }
  
  stopMQTT();
  return "[MQTT] Client stopped";
}

const char* cmd_mqttstatus(const String& args) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  if (!ensureDebugBuffer()) return "[MQTT] Status unavailable";
  char* buf = getDebugBuffer();
  
  int pos = snprintf(buf, 1024, "=== MQTT STATUS ===\n");
  pos += snprintf(buf + pos, 1024 - pos, "Enabled: %s\n", mqttEnabled ? "Yes" : "No");
  pos += snprintf(buf + pos, 1024 - pos, "Connected: %s\n", mqttConnected ? "Yes" : "No");
  pos += snprintf(buf + pos, 1024 - pos, "Broker: %s:%d\n", 
                  gSettings.mqttHost.c_str(), gSettings.mqttPort);
  pos += snprintf(buf + pos, 1024 - pos, "User: %s\n", 
                  gSettings.mqttUser.length() > 0 ? gSettings.mqttUser.c_str() : "(none)");
  pos += snprintf(buf + pos, 1024 - pos, "Base Topic: %s\n", gSettings.mqttBaseTopic.c_str());
  pos += snprintf(buf + pos, 1024 - pos, "Publish Interval: %d ms\n", gSettings.mqttPublishIntervalMs);
  
  if (lastError.length() > 0) {
    pos += snprintf(buf + pos, 1024 - pos, "Last Error: %s\n", lastError.c_str());
  }
  
  if (mqttConnected) {
    unsigned long nextPublish = (lastPublishTime + gSettings.mqttPublishIntervalMs) - millis();
    pos += snprintf(buf + pos, 1024 - pos, "Next Publish: %lu ms\n", nextPublish);
  }
  
  return buf;
}

// ============================================================================
// MQTT Settings CLI Commands
// ============================================================================

extern bool writeSettingsJson();

const char* cmd_mqttautostart(const String& args) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  String arg = args;
  arg.trim();
  if (arg.length() == 0) {
    return gSettings.mqttAutoStart ? "MQTT auto-start: ON" : "MQTT auto-start: OFF";
  }
  gSettings.mqttAutoStart = (arg == "1" || arg.equalsIgnoreCase("on") || arg.equalsIgnoreCase("true"));
  writeSettingsJson();
  return gSettings.mqttAutoStart ? "MQTT auto-start enabled" : "MQTT auto-start disabled";
}

const char* cmd_mqtthost(const String& args) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  String arg = args;
  arg.trim();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return "Error";
    snprintf(getDebugBuffer(), 1024, "MQTT host: %s", 
             gSettings.mqttHost.length() > 0 ? gSettings.mqttHost.c_str() : "(not set)");
    return getDebugBuffer();
  }
  gSettings.mqttHost = arg;
  writeSettingsJson();
  if (!ensureDebugBuffer()) return "MQTT host updated";
  snprintf(getDebugBuffer(), 1024, "MQTT host set to: %s", arg.c_str());
  return getDebugBuffer();
}

const char* cmd_mqttport(const String& args) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  String arg = args;
  arg.trim();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return "Error";
    snprintf(getDebugBuffer(), 1024, "MQTT port: %d", gSettings.mqttPort);
    return getDebugBuffer();
  }
  int port = arg.toInt();
  if (port < 1 || port > 65535) {
    return "Error: Port must be 1-65535";
  }
  gSettings.mqttPort = port;
  writeSettingsJson();
  if (!ensureDebugBuffer()) return "MQTT port updated";
  snprintf(getDebugBuffer(), 1024, "MQTT port set to: %d", port);
  return getDebugBuffer();
}

const char* cmd_mqtttlsmode(const String& args) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  String arg = args;
  arg.trim();
  
  // Show current mode if no argument
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return "Error";
    const char* modeStr = "None (unencrypted)";
    if (gSettings.mqttTLSMode == 1) modeStr = "TLS (encrypted, no verification)";
    else if (gSettings.mqttTLSMode == 2) modeStr = "TLS + Verify (encrypted + cert verification)";
    snprintf(getDebugBuffer(), 1024, "MQTT TLS Mode: %d - %s\nCA cert path: %s",
             gSettings.mqttTLSMode, modeStr,
             gSettings.mqttCACertPath.length() > 0 ? gSettings.mqttCACertPath.c_str() : "(not set)");
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
    return "Usage: mqttTLSMode [0|1|2|none|tls|verify]\n  0/none = No TLS\n  1/tls = TLS (no verification)\n  2/verify = TLS + Certificate Verification";
  }
  
  int oldMode = gSettings.mqttTLSMode;
  gSettings.mqttTLSMode = newMode;
  
  // Create /system/certs/ folder for TLS modes
  if (newMode > 0 && !LittleFS.exists("/system/certs")) {
    LittleFS.mkdir("/system");
    LittleFS.mkdir("/system/certs");
    INFO_SYSTEMF("[MQTT] Created /system/certs/ folder for certificates");
  }
  
  // Auto-switch ports
  if (newMode > 0 && oldMode == 0 && gSettings.mqttPort == 1883) {
    gSettings.mqttPort = 8883;
  } else if (newMode == 0 && oldMode > 0 && gSettings.mqttPort == 8883) {
    gSettings.mqttPort = 1883;
  }
  
  writeSettingsJson();
  
  if (!ensureDebugBuffer()) return "Mode updated";
  const char* modeStr = "None";
  if (newMode == 1) modeStr = "TLS (no verification)";
  else if (newMode == 2) modeStr = "TLS + Verify";
  snprintf(getDebugBuffer(), 1024, "MQTT TLS Mode set to %d: %s (port: %d) - restart MQTT to apply",
           newMode, modeStr, gSettings.mqttPort);
  return getDebugBuffer();
}

const char* cmd_mqttcacertpath(const String& args) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  String arg = args;
  arg.trim();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return "Error";
    snprintf(getDebugBuffer(), 1024, "MQTT CA cert path: %s",
             gSettings.mqttCACertPath.length() > 0 ? gSettings.mqttCACertPath.c_str() : "(not set)");
    return getDebugBuffer();
  }
  if (arg == "clear" || arg == "none") {
    gSettings.mqttCACertPath = "";
    if (gSettings.mqttTLSMode == 2) {
      gSettings.mqttTLSMode = 1;  // Downgrade to TLS without verify
    }
    writeSettingsJson();
    return "MQTT CA cert path cleared";
  }
  // Verify file exists
  if (!LittleFS.exists(arg)) {
    if (!ensureDebugBuffer()) return "Error";
    snprintf(getDebugBuffer(), 1024, "Warning: File not found: %s (setting anyway)", arg.c_str());
    gSettings.mqttCACertPath = arg;
    writeSettingsJson();
    return getDebugBuffer();
  }
  gSettings.mqttCACertPath = arg;
  writeSettingsJson();
  if (!ensureDebugBuffer()) return "Error";
  snprintf(getDebugBuffer(), 1024, "MQTT CA cert path set to: %s", arg.c_str());
  return getDebugBuffer();
}

const char* cmd_mqttuser(const String& args) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  String arg = args;
  arg.trim();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return "Error";
    snprintf(getDebugBuffer(), 1024, "MQTT user: %s", 
             gSettings.mqttUser.length() > 0 ? gSettings.mqttUser.c_str() : "(not set)");
    return getDebugBuffer();
  }
  if (arg == "clear" || arg == "none") {
    gSettings.mqttUser = "";
    writeSettingsJson();
    return "MQTT user cleared";
  }
  gSettings.mqttUser = arg;
  writeSettingsJson();
  if (!ensureDebugBuffer()) return "MQTT user updated";
  snprintf(getDebugBuffer(), 1024, "MQTT user set to: %s", arg.c_str());
  return getDebugBuffer();
}

const char* cmd_mqttpassword(const String& args) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  String arg = args;
  arg.trim();
  if (arg.length() == 0) {
    return gSettings.mqttPassword.length() > 0 ? "MQTT password: ********" : "MQTT password: (not set)";
  }
  if (arg == "clear" || arg == "none") {
    gSettings.mqttPassword = "";
    writeSettingsJson();
    return "MQTT password cleared";
  }
  gSettings.mqttPassword = arg;
  writeSettingsJson();
  return "MQTT password updated";
}

const char* cmd_mqttbasetopic(const String& args) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  String arg = args;
  arg.trim();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return "Error";
    snprintf(getDebugBuffer(), 1024, "MQTT base topic: %s", 
             gSettings.mqttBaseTopic.length() > 0 ? gSettings.mqttBaseTopic.c_str() : "(auto-generated)");
    return getDebugBuffer();
  }
  if (arg == "clear" || arg == "auto") {
    gSettings.mqttBaseTopic = "";
    writeSettingsJson();
    return "MQTT base topic cleared (will auto-generate on connect)";
  }
  gSettings.mqttBaseTopic = arg;
  writeSettingsJson();
  if (!ensureDebugBuffer()) return "MQTT base topic updated";
  snprintf(getDebugBuffer(), 1024, "MQTT base topic set to: %s", arg.c_str());
  return getDebugBuffer();
}

const char* cmd_mqttdiscoveryprefix(const String& args) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  String arg = args;
  arg.trim();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return "Error";
    snprintf(getDebugBuffer(), 1024, "MQTT discovery prefix: %s", 
             gSettings.mqttDiscoveryPrefix.length() > 0 ? gSettings.mqttDiscoveryPrefix.c_str() : "homeassistant");
    return getDebugBuffer();
  }
  gSettings.mqttDiscoveryPrefix = arg;
  writeSettingsJson();
  if (!ensureDebugBuffer()) return "MQTT discovery prefix updated";
  snprintf(getDebugBuffer(), 1024, "MQTT discovery prefix set to: %s", arg.c_str());
  return getDebugBuffer();
}

const char* cmd_mqttpublishinterval(const String& args) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  String arg = args;
  arg.trim();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return "Error";
    snprintf(getDebugBuffer(), 1024, "MQTT publish interval: %d ms", gSettings.mqttPublishIntervalMs);
    return getDebugBuffer();
  }
  int interval = arg.toInt();
  if (interval < 1000 || interval > 300000) {
    return "Error: Interval must be 1000-300000 ms";
  }
  gSettings.mqttPublishIntervalMs = interval;
  writeSettingsJson();
  if (!ensureDebugBuffer()) return "MQTT publish interval updated";
  snprintf(getDebugBuffer(), 1024, "MQTT publish interval set to: %d ms", interval);
  return getDebugBuffer();
}

const char* cmd_mqttsubscribe(const String& args) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  String arg = args;
  arg.trim();
  if (arg.length() == 0) {
    return gSettings.mqttSubscribeExternal ? 
      "MQTT external subscriptions: enabled" : "MQTT external subscriptions: disabled";
  }
  if (arg == "1" || arg == "true" || arg == "on") {
    gSettings.mqttSubscribeExternal = true;
    writeSettingsJson();
    return "MQTT external subscriptions enabled - restart MQTT to apply";
  } else if (arg == "0" || arg == "false" || arg == "off") {
    gSettings.mqttSubscribeExternal = false;
    writeSettingsJson();
    return "MQTT external subscriptions disabled";
  }
  return "Error: Use 0/1, true/false, or on/off";
}

const char* cmd_mqtttopics(const String& args) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  String arg = args;
  arg.trim();
  if (arg.length() == 0) {
    if (!ensureDebugBuffer()) return "Error";
    snprintf(getDebugBuffer(), 1024, "MQTT subscribe topics: %s", 
             gSettings.mqttSubscribeTopics.length() > 0 ? gSettings.mqttSubscribeTopics.c_str() : "(none)");
    return getDebugBuffer();
  }
  if (arg == "clear") {
    gSettings.mqttSubscribeTopics = "";
    writeSettingsJson();
    return "MQTT subscribe topics cleared";
  }
  gSettings.mqttSubscribeTopics = arg;
  writeSettingsJson();
  if (!ensureDebugBuffer()) return "MQTT subscribe topics updated";
  snprintf(getDebugBuffer(), 1024, "MQTT subscribe topics set to: %s", arg.c_str());
  return getDebugBuffer();
}

const char* cmd_mqttexternalsensors(const String& args) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  if (!ensureDebugBuffer()) return "Error";
  char* buf = getDebugBuffer();
  int pos = 0;
  
  if (!externalSensorMutex || externalSensorCount == 0) {
    return "No external sensors received";
  }
  
  if (xSemaphoreTake(externalSensorMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
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
    return "Error: Could not acquire lock";
  }
  
  return buf;
}

const char* cmd_debugmqtt(const String& args) {
  extern bool gCLIValidateOnly;
  if (gCLIValidateOnly) return "VALID";
  
  String arg = args;
  arg.trim();
  if (arg.length() == 0) {
    return gSettings.debugMqtt ? "MQTT debug: enabled" : "MQTT debug: disabled";
  }
  if (arg == "1" || arg == "true" || arg == "on") {
    gSettings.debugMqtt = true;
    writeSettingsJson();
    return "MQTT debug enabled";
  } else if (arg == "0" || arg == "false" || arg == "off") {
    gSettings.debugMqtt = false;
    writeSettingsJson();
    return "MQTT debug disabled";
  }
  return "Error: Use 0/1, true/false, or on/off";
}

// Helper macro for boolean publish settings
#define MQTT_PUBLISH_CMD(name, setting, label) \
const char* cmd_mqttpublish##name(const String& args) { \
  extern bool gCLIValidateOnly; \
  if (gCLIValidateOnly) return "VALID"; \
  String arg = args; arg.trim(); \
  if (arg.length() == 0) { \
    return gSettings.setting ? "MQTT publish " label ": ON" : "MQTT publish " label ": OFF"; \
  } \
  gSettings.setting = (arg == "1" || arg.equalsIgnoreCase("on") || arg.equalsIgnoreCase("true")); \
  writeSettingsJson(); \
  return gSettings.setting ? "MQTT publish " label " enabled" : "MQTT publish " label " disabled"; \
}

MQTT_PUBLISH_CMD(wifi, mqttPublishWiFi, "WiFi")
MQTT_PUBLISH_CMD(system, mqttPublishSystem, "System")
MQTT_PUBLISH_CMD(thermal, mqttPublishThermal, "Thermal")
MQTT_PUBLISH_CMD(tof, mqttPublishToF, "ToF")
MQTT_PUBLISH_CMD(imu, mqttPublishIMU, "IMU")
MQTT_PUBLISH_CMD(presence, mqttPublishPresence, "Presence")
MQTT_PUBLISH_CMD(gps, mqttPublishGPS, "GPS")
MQTT_PUBLISH_CMD(apds, mqttPublishAPDS, "APDS")
MQTT_PUBLISH_CMD(rtc, mqttPublishRTC, "RTC")
MQTT_PUBLISH_CMD(gamepad, mqttPublishGamepad, "Gamepad")

// Command table - names must match setting keys for web UI compatibility
const CommandEntry mqttCommands[] = {
  { "debugmqtt", "Enable/disable MQTT debug logging", true, cmd_debugmqtt, "Usage: debugmqtt [0|1]" },
  { "openmqtt", "Start MQTT client and connect to broker", false, cmd_openmqtt },
  { "closemqtt", "Stop MQTT client", false, cmd_closemqtt },
  { "mqttstatus", "Show MQTT connection status", false, cmd_mqttstatus },
  { "mqttAutoStart", "Get/set MQTT auto-start (0|1)", true, cmd_mqttautostart, "Usage: mqttAutoStart [0|1]" },
  { "mqttHost", "Get/set MQTT broker host", true, cmd_mqtthost, "Usage: mqttHost [hostname]" },
  { "mqttPort", "Get/set MQTT broker port", true, cmd_mqttport, "Usage: mqttPort [port]" },
  { "mqttTLSMode", "Set TLS mode (0=none, 1=tls, 2=tls+verify)", true, cmd_mqtttlsmode, "Usage: mqttTLSMode [0|1|2|none|tls|verify]" },
  { "mqttCACertPath", "Set CA certificate path for TLS+Verify mode", true, cmd_mqttcacertpath, "Usage: mqttCACertPath [path|clear]" },
  { "mqttSubscribeExternal", "Enable/disable external topic subscriptions", true, cmd_mqttsubscribe, "Usage: mqttSubscribeExternal [0|1]" },
  { "mqttSubscribeTopics", "Get/set topics to subscribe (comma-separated)", true, cmd_mqtttopics, "Usage: mqttSubscribeTopics [topic1,topic2,...]" },
  { "mqttExternalSensors", "List received external sensor data", false, cmd_mqttexternalsensors },
  { "mqttUser", "Get/set MQTT username", true, cmd_mqttuser, "Usage: mqttUser [username|clear]" },
  { "mqttPassword", "Set MQTT password", true, cmd_mqttpassword, "Usage: mqttPassword [password|clear]" },
  { "mqttBaseTopic", "Get/set MQTT base topic", true, cmd_mqttbasetopic, "Usage: mqttBaseTopic [topic|auto]" },
  { "mqttDiscoveryPrefix", "Get/set Home Assistant discovery prefix", true, cmd_mqttdiscoveryprefix, "Usage: mqttDiscoveryPrefix [prefix]" },
  { "mqttPublishIntervalMs", "Get/set publish interval in ms", true, cmd_mqttpublishinterval, "Usage: mqttPublishIntervalMs [1000-300000]" },
  { "mqttPublishWiFi", "Enable/disable WiFi data in MQTT", true, cmd_mqttpublishwifi, "Usage: mqttPublishWiFi [0|1]" },
  { "mqttPublishSystem", "Enable/disable system data in MQTT", true, cmd_mqttpublishsystem, "Usage: mqttPublishSystem [0|1]" },
  { "mqttPublishThermal", "Enable/disable thermal data in MQTT", true, cmd_mqttpublishthermal, "Usage: mqttPublishThermal [0|1]" },
  { "mqttPublishToF", "Enable/disable ToF data in MQTT", true, cmd_mqttpublishtof, "Usage: mqttPublishToF [0|1]" },
  { "mqttPublishIMU", "Enable/disable IMU data in MQTT", true, cmd_mqttpublishimu, "Usage: mqttPublishIMU [0|1]" },
  { "mqttPublishPresence", "Enable/disable presence data in MQTT", true, cmd_mqttpublishpresence, "Usage: mqttPublishPresence [0|1]" },
  { "mqttPublishGPS", "Enable/disable GPS data in MQTT", true, cmd_mqttpublishgps, "Usage: mqttPublishGPS [0|1]" },
  { "mqttPublishAPDS", "Enable/disable APDS data in MQTT", true, cmd_mqttpublishapds, "Usage: mqttPublishAPDS [0|1]" },
  { "mqttPublishRTC", "Enable/disable RTC data in MQTT", true, cmd_mqttpublishrtc, "Usage: mqttPublishRTC [0|1]" },
  { "mqttPublishGamepad", "Enable/disable gamepad data in MQTT", true, cmd_mqttpublishgamepad, "Usage: mqttPublishGamepad [0|1]" }
};

const size_t mqttCommandsCount = sizeof(mqttCommands) / sizeof(mqttCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _mqtt_cmd_registrar(mqttCommands, mqttCommandsCount, "mqtt");

#endif // ENABLE_WIFI && ENABLE_MQTT
