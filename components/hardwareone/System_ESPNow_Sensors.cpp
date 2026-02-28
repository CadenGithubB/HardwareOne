#include "System_ESPNow_Sensors.h"

#if ENABLE_ESPNOW

#include <ArduinoJson.h>

#include "OLED_Display.h"
#include "System_Debug.h"
#include "System_ESPNow.h"
#include "System_MemUtil.h"
#include "System_Settings.h"

#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor-seesaw.h"
#endif
#if ENABLE_GPS_SENSOR
#include "i2csensor-pa1010d.h"
#endif
#if ENABLE_IMU_SENSOR
#include "i2csensor-bno055.h"
#endif
#if ENABLE_THERMAL_SENSOR
#include "i2csensor-mlx90640.h"
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor-vl53l4cx.h"
#endif
#if ENABLE_FMRADIO_SENSOR
  #include "i2csensor-rda5807.h"
#endif
#if ENABLE_CAMERA_SENSOR
#include "System_Camera_DVP.h"
#endif
#if ENABLE_MICROPHONE_SENSOR
#include "System_Microphone.h"
#endif

// External functions
extern void meshSendEnvelopeToPeers(const String& payload);
extern String macToHexString(const uint8_t* mac);

// ==========================
// Remote Sensor Data Cache
// ==========================

RemoteSensorData gRemoteSensorCache[MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE];

// Master flag to enable/disable all sensor ESP-NOW communication (status + data)
// Must be explicitly enabled before any sensor broadcasts will be sent
static bool gSensorBroadcastEnabled = false;

// Sensor streaming state (worker devices only)
static bool gSensorStreamingEnabled[REMOTE_SENSOR_MAX] = {false};

// Local sensor data cache (sensors write here, broadcaster reads)
struct LocalSensorCache {
  char jsonData[256];  // Cached JSON string
  uint16_t jsonLength;
  bool dirty;          // True if data changed since last broadcast
  bool forceSend;      // True to force immediate send (event-driven)
  unsigned long lastUpdate;  // When cache was last written
};
static LocalSensorCache gLocalSensorCache[REMOTE_SENSOR_MAX];

// Broadcaster task state
static TaskHandle_t gSensorBroadcasterTask = nullptr;
static SemaphoreHandle_t gSensorCacheMutex = nullptr;
static unsigned long gLastBroadcastTime = 0;

// ==========================
// Initialization
// ==========================

void initRemoteSensorSystem() {
  // Initialize cache
  for (int i = 0; i < MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE; i++) {
    memset(gRemoteSensorCache[i].deviceMac, 0, 6);
    gRemoteSensorCache[i].deviceName[0] = '\0';
    gRemoteSensorCache[i].sensorType = REMOTE_SENSOR_THERMAL;
    gRemoteSensorCache[i].jsonData[0] = '\0';
    gRemoteSensorCache[i].jsonLength = 0;
    gRemoteSensorCache[i].lastUpdate = 0;
    gRemoteSensorCache[i].valid = false;
  }
  
  DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] System initialized");
}

// ==========================
// Helper Functions
// ==========================

const char* sensorTypeToString(RemoteSensorType type) {
  switch (type) {
    case REMOTE_SENSOR_THERMAL: return "thermal";
    case REMOTE_SENSOR_TOF: return "tof";
    case REMOTE_SENSOR_IMU: return "imu";
    case REMOTE_SENSOR_GPS: return "gps";
    case REMOTE_SENSOR_GAMEPAD: return "gamepad";
    case REMOTE_SENSOR_FMRADIO: return "fmradio";
    case REMOTE_SENSOR_CAMERA: return "camera";
    case REMOTE_SENSOR_MICROPHONE: return "microphone";
    case REMOTE_SENSOR_RTC: return "rtc";
    case REMOTE_SENSOR_PRESENCE: return "presence";
    case REMOTE_SENSOR_APDS: return "apds";
    default: return "unknown";
  }
}

RemoteSensorType stringToSensorType(const char* str) {
  if (strcmp(str, "thermal") == 0) return REMOTE_SENSOR_THERMAL;
  if (strcmp(str, "tof") == 0) return REMOTE_SENSOR_TOF;
  if (strcmp(str, "imu") == 0) return REMOTE_SENSOR_IMU;
  if (strcmp(str, "gps") == 0) return REMOTE_SENSOR_GPS;
  if (strcmp(str, "gamepad") == 0) return REMOTE_SENSOR_GAMEPAD;
  if (strcmp(str, "fmradio") == 0) return REMOTE_SENSOR_FMRADIO;
  if (strcmp(str, "camera") == 0) return REMOTE_SENSOR_CAMERA;
  if (strcmp(str, "microphone") == 0) return REMOTE_SENSOR_MICROPHONE;
  if (strcmp(str, "rtc") == 0) return REMOTE_SENSOR_RTC;
  if (strcmp(str, "presence") == 0) return REMOTE_SENSOR_PRESENCE;
  if (strcmp(str, "apds") == 0) return REMOTE_SENSOR_APDS;
  return REMOTE_SENSOR_THERMAL;  // Default
}

// Find cache entry for device+sensor
static RemoteSensorData* findCacheEntry(const uint8_t* deviceMac, RemoteSensorType sensorType) {
  for (int i = 0; i < MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE; i++) {
    if (memcmp(gRemoteSensorCache[i].deviceMac, deviceMac, 6) == 0 &&
        gRemoteSensorCache[i].sensorType == sensorType) {
      return &gRemoteSensorCache[i];
    }
  }
  return nullptr;
}

// Find or create cache entry
RemoteSensorData* findOrCreateCacheEntry(const uint8_t* deviceMac, const char* deviceName, RemoteSensorType sensorType) {
  // Try to find existing entry
  RemoteSensorData* entry = findCacheEntry(deviceMac, sensorType);
  if (entry) return entry;
  
  // Find empty slot
  for (int i = 0; i < MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE; i++) {
    if (!gRemoteSensorCache[i].valid) {
      memcpy(gRemoteSensorCache[i].deviceMac, deviceMac, 6);
      strncpy(gRemoteSensorCache[i].deviceName, deviceName, 31);
      gRemoteSensorCache[i].deviceName[31] = '\0';
      gRemoteSensorCache[i].sensorType = sensorType;
      gRemoteSensorCache[i].valid = false;  // Will be set to true when data arrives
      return &gRemoteSensorCache[i];
    }
  }
  
  DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Cache full, cannot add device %s sensor %s",
         deviceName, sensorTypeToString(sensorType));
  return nullptr;
}

// Update remote sensor status (called from V3 message handler)
void updateRemoteSensorStatus(const uint8_t* mac, const char* name, RemoteSensorType type, bool enabled) {
  RemoteSensorData* entry = findOrCreateCacheEntry(mac, name, type);
  if (entry) {
    if (!enabled) {
      // Mark as invalid when disabled
      entry->valid = false;
      DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Sensor %s disabled on %s",
             sensorTypeToString(type), name);
    } else {
      // Mark as valid when enabled (data will arrive separately)
      entry->lastUpdate = millis();
      DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Sensor %s enabled on %s",
             sensorTypeToString(type), name);
    }
  }
}

// ==========================
// Message Handlers
// ==========================

void handleSensorStatusMessage(const uint8_t* senderMac, const String& deviceName, const String& message) {
  DEBUG_SENSORSF("[SENSOR_STATUS_RX] Received status message from %s (MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
         deviceName.c_str(), senderMac[0], senderMac[1], senderMac[2], senderMac[3], senderMac[4], senderMac[5]);
  DEBUG_SENSORSF("[SENSOR_STATUS_RX] Message length: %d bytes", message.length());
  DEBUG_SENSORSF("[SENSOR_STATUS_RX] Raw message: %.200s", message.c_str());
  
  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, message);
  if (error) {
    DEBUG_SENSORSF("[SENSOR_STATUS_RX] ERROR: Failed to parse status JSON: %s", error.c_str());
    DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Failed to parse status JSON: %s", error.c_str());
    return;
  }
  DEBUG_SENSORSF("%s", "[SENSOR_STATUS_RX] JSON parsed successfully");
  
  const char* sensorTypeStr = doc["sensor"] | "";
  bool enabled = doc["enabled"] | false;
  DEBUG_SENSORSF("[SENSOR_STATUS_RX] Extracted: sensor='%s', enabled=%d", sensorTypeStr, enabled);
  
  RemoteSensorType sensorType = stringToSensorType(sensorTypeStr);
  DEBUG_SENSORSF("[SENSOR_STATUS_RX] Sensor type: %d (%s)", sensorType, sensorTypeToString(sensorType));
  
  DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Status from %s: %s = %s",
         deviceName.c_str(), sensorTypeStr, enabled ? "enabled" : "disabled");
  
  // Update cache entry
  DEBUG_SENSORSF("[SENSOR_STATUS_RX] Looking up/creating cache entry for %s", deviceName.c_str());
  RemoteSensorData* entry = findOrCreateCacheEntry(senderMac, deviceName.c_str(), sensorType);
  if (entry) {
    DEBUG_SENSORSF("[SENSOR_STATUS_RX] Cache entry found/created at %p", entry);
    if (!enabled) {
      // Sensor disabled - invalidate cache entry
      DEBUG_SENSORSF("[SENSOR_STATUS_RX] Sensor disabled, invalidating cache entry");
      entry->valid = false;
      entry->jsonData[0] = '\0';
      entry->jsonLength = 0;
      DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Invalidated cache for %s %s",
             deviceName.c_str(), sensorTypeStr);
    } else {
      DEBUG_SENSORSF("[SENSOR_STATUS_RX] Sensor enabled, cache entry ready for data");
    }
  } else {
    DEBUG_SENSORSF("%s", "[SENSOR_STATUS_RX] ERROR: Failed to find/create cache entry");
  }
  
  // Broadcast to web clients via SSE
  BROADCAST_PRINTF("[ESP-NOW] Remote sensor %s on %s is now %s", sensorTypeStr, deviceName.c_str(), enabled ? "enabled" : "disabled");
}

void handleSensorDataMessage(const uint8_t* senderMac, const String& deviceName, const String& message) {
  DEBUG_SENSORSF("[SENSOR_DATA_RX] Received data message from %s (MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
         deviceName.c_str(), senderMac[0], senderMac[1], senderMac[2], senderMac[3], senderMac[4], senderMac[5]);
  DEBUG_SENSORSF("[SENSOR_DATA_RX] Message length: %d bytes", message.length());
  DEBUG_SENSORSF("[SENSOR_DATA_RX] Raw message (first 200 chars): %.200s", message.c_str());
  
  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, message);
  if (error) {
    DEBUG_SENSORSF("[SENSOR_DATA_RX] ERROR: Failed to parse sensor data JSON: %s", error.c_str());
    DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Failed to parse sensor data JSON: %s", error.c_str());
    return;
  }
  DEBUG_SENSORSF("%s", "[SENSOR_DATA_RX] JSON parsed successfully");
  
  const char* sensorTypeStr = doc["sensor"] | "";
  JsonObject data = doc["data"];
  DEBUG_SENSORSF("[SENSOR_DATA_RX] Extracted: sensor='%s', has_data=%d", sensorTypeStr, data ? 1 : 0);
  
  if (!data) {
    DEBUG_SENSORSF("%s", "[SENSOR_DATA_RX] ERROR: No data field in sensor message");
    DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] No data field in sensor message");
    return;
  }
  
  RemoteSensorType sensorType = stringToSensorType(sensorTypeStr);
  DEBUG_SENSORSF("[SENSOR_DATA_RX] Sensor type: %d (%s)", sensorType, sensorTypeToString(sensorType));
  
  // Update cache entry
  DEBUG_SENSORSF("[SENSOR_DATA_RX] Looking up/creating cache entry for %s", deviceName.c_str());
  RemoteSensorData* entry = findOrCreateCacheEntry(senderMac, deviceName.c_str(), sensorType);
  if (entry) {
    DEBUG_SENSORSF("[SENSOR_DATA_RX] Cache entry found/created at %p", entry);
    
    // Serialize directly into fixed buffer (no heap allocation)
    size_t written = serializeJson(data, entry->jsonData, REMOTE_SENSOR_BUFFER_SIZE);
    if (written >= REMOTE_SENSOR_BUFFER_SIZE) {
      // Truncated - data too large for buffer
      DEBUG_SENSORSF("[SENSOR_DATA_RX] WARNING: Data truncated (%zu >= %d)", written, REMOTE_SENSOR_BUFFER_SIZE);
      entry->jsonData[REMOTE_SENSOR_BUFFER_SIZE - 1] = '\0';
      entry->jsonLength = REMOTE_SENSOR_BUFFER_SIZE - 1;
    } else {
      entry->jsonLength = (uint16_t)written;
    }
    entry->lastUpdate = millis();
    entry->valid = true;
    
    DEBUG_SENSORSF("[SENSOR_DATA_RX] Cache updated: valid=%d, lastUpdate=%lu, len=%u", 
                   entry->valid, entry->lastUpdate, entry->jsonLength);
    DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Updated cache for %s %s (%u bytes)",
           deviceName.c_str(), sensorTypeStr, entry->jsonLength);
  } else {
    DEBUG_SENSORSF("%s", "[SENSOR_DATA_RX] ERROR: Failed to find/create cache entry");
  }
}

// ==========================
// Worker → Master Broadcasting
// ==========================

void broadcastSensorStatus(RemoteSensorType sensorType, bool enabled) {
  DEBUG_SENSORSF("[SENSOR_STATUS_TX] broadcastSensorStatus() called: type=%d (%s), enabled=%d",
         sensorType, sensorTypeToString(sensorType), enabled);
  
  // Check master broadcast flag first
  if (!gSensorBroadcastEnabled) {
    DEBUG_SENSORSF("%s", "[SENSOR_STATUS_TX] SKIP: Sensor broadcasting not enabled");
    return;
  }
  
  // Only workers should broadcast to master
  extern Settings gSettings;
  bool meshEn = meshEnabled();
  DEBUG_SENSORSF("[SENSOR_STATUS_TX] Pre-checks: meshEnabled=%d, meshRole=%d", meshEn, gSettings.meshRole);
  
  if (!meshEn) {
    DEBUG_SENSORSF("%s", "[SENSOR_STATUS_TX] SKIP: Mesh not enabled");
    return;
  }
  
  if (gSettings.meshRole == MESH_ROLE_MASTER) {
    DEBUG_SENSORSF("%s", "[SENSOR_STATUS_TX] SKIP: Master devices don't broadcast status");
    return;
  }
  
  // Build and send V3 status message
  DEBUG_SENSORSF("%s", "[SENSOR_STATUS_TX] Broadcasting V3 sensor status");
  
  DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Broadcasting status: %s = %s",
         sensorTypeToString(sensorType), enabled ? "enabled" : "disabled");
  
  // Send via V3 binary protocol
  extern bool v3_broadcast_sensor_status(RemoteSensorType sensorType, bool enabled);
  bool sent = v3_broadcast_sensor_status(sensorType, enabled);
  
  if (sent) {
    DEBUG_SENSORSF("[SENSOR_STATUS_TX] SUCCESS: Broadcast %s status", sensorTypeToString(sensorType));
  } else {
    DEBUG_SENSORSF("[SENSOR_STATUS_TX] ERROR: Failed to broadcast %s status", sensorTypeToString(sensorType));
  }
}

// Forward declarations
static bool startSensorBroadcaster();
static void stopSensorBroadcaster();

void startSensorDataStreaming(RemoteSensorType sensorType) {
  DEBUG_SENSORSF("[SENSOR_STREAM] startSensorDataStreaming() called with type=%d (%s)", sensorType, sensorTypeToString(sensorType));
  
  if (sensorType >= REMOTE_SENSOR_MAX) {
    DEBUG_SENSORSF("[SENSOR_STREAM] ERROR: Invalid sensor type %d (max=%d)", sensorType, REMOTE_SENSOR_MAX);
    return;
  }
  
#if ENABLE_BONDED_MODE
  // Bond master: send STREAM_CTRL to worker — master doesn't have the sensors locally
  extern Settings gSettings;
  if (gSettings.bondModeEnabled && gSettings.bondRole == 1) {
    extern bool sendBondStreamCtrl(RemoteSensorType sensorType, bool enable);
    DEBUG_SENSORSF("[SENSOR_STREAM] Bond master: sending STREAM_CTRL %s ON to worker", sensorTypeToString(sensorType));
    bool sent = sendBondStreamCtrl(sensorType, true);
    if (sent) {
      // Update local flag so UI reflects the requested streaming state
      gSensorStreamingEnabled[sensorType] = true;
      BROADCAST_PRINTF("[ESP-NOW] Requested worker to stream %s sensor data", sensorTypeToString(sensorType));
    } else {
      BROADCAST_PRINTF("[ESP-NOW] Failed to send stream request to worker (peer offline?)");
    }
    return;
  }
#endif
  
  DEBUG_SENSORSF("[SENSOR_STREAM] Setting streaming flag for %s to TRUE", sensorTypeToString(sensorType));
  
  // Ensure master broadcast flag is enabled so sensor data reaches the cache
  if (!gSensorBroadcastEnabled) {
    setSensorBroadcastEnabled(true);
  }
  
  // Start broadcaster task if not already running
  if (!gSensorBroadcasterTask) {
    if (!startSensorBroadcaster()) {
      BROADCAST_PRINTF("[ESP-NOW] ERROR: Failed to start sensor broadcaster task");
      return;
    }
  }
  
  gSensorStreamingEnabled[sensorType] = true;
  
  // Force immediate send of this sensor
  if (gSensorCacheMutex && xSemaphoreTake(gSensorCacheMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    gLocalSensorCache[sensorType].forceSend = true;
    xSemaphoreGive(gSensorCacheMutex);
  }
  
  DEBUG_SENSORSF("[SENSOR_STREAM] Streaming enabled: %s (flag=%d)",
         sensorTypeToString(sensorType), gSensorStreamingEnabled[sensorType]);
  
  DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Started streaming for %s",
         sensorTypeToString(sensorType));
  
  BROADCAST_PRINTF("[ESP-NOW] Started streaming %s sensor data", sensorTypeToString(sensorType));
}

void stopSensorDataStreaming(RemoteSensorType sensorType) {
  DEBUG_SENSORSF("[SENSOR_STREAM] stopSensorDataStreaming() called with type=%d (%s)", sensorType, sensorTypeToString(sensorType));
  
  if (sensorType >= REMOTE_SENSOR_MAX) {
    DEBUG_SENSORSF("[SENSOR_STREAM] ERROR: Invalid sensor type %d (max=%d)", sensorType, REMOTE_SENSOR_MAX);
    return;
  }
  
#if ENABLE_BONDED_MODE
  // Bond master: send STREAM_CTRL OFF to worker
  extern Settings gSettings;
  if (gSettings.bondModeEnabled && gSettings.bondRole == 1) {
    extern bool sendBondStreamCtrl(RemoteSensorType sensorType, bool enable);
    DEBUG_SENSORSF("[SENSOR_STREAM] Bond master: sending STREAM_CTRL %s OFF to worker", sensorTypeToString(sensorType));
    sendBondStreamCtrl(sensorType, false);
    // Update local flag so UI reflects the stopped streaming state
    gSensorStreamingEnabled[sensorType] = false;
    BROADCAST_PRINTF("[ESP-NOW] Requested worker to stop streaming %s sensor data", sensorTypeToString(sensorType));
    return;
  }
#endif
  
  DEBUG_SENSORSF("[SENSOR_STREAM] Setting streaming flag for %s to FALSE", sensorTypeToString(sensorType));
  gSensorStreamingEnabled[sensorType] = false;
  
  // Check if all sensors are now disabled - if so, stop broadcaster task
  bool anyEnabled = false;
  for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
    if (gSensorStreamingEnabled[i]) {
      anyEnabled = true;
      break;
    }
  }
  if (!anyEnabled) {
    stopSensorBroadcaster();
    DEBUGF(DEBUG_ESPNOW_CORE, "[SENSOR_BROADCASTER] All sensors disabled, task stopped");
  }
  
  DEBUG_SENSORSF("[SENSOR_STREAM] Streaming disabled: %s (flag=%d)",
         sensorTypeToString(sensorType), gSensorStreamingEnabled[sensorType]);
  
  DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Stopped streaming for %s",
         sensorTypeToString(sensorType));
  
  BROADCAST_PRINTF("[ESP-NOW] Stopped streaming %s sensor data", sensorTypeToString(sensorType));
}

 bool isSensorDataStreamingEnabled(RemoteSensorType sensorType) {
   if (sensorType >= REMOTE_SENSOR_MAX) return false;
   return gSensorStreamingEnabled[sensorType];
 }

// ==========================
// Sensor Broadcast Control
// ==========================

void setSensorBroadcastEnabled(bool enabled) {
  gSensorBroadcastEnabled = enabled;
  DEBUG_SENSORSF("[SENSOR_BROADCAST] Sensor broadcasting %s", enabled ? "ENABLED" : "DISABLED");
}

bool isSensorBroadcastEnabled() {
  return gSensorBroadcastEnabled;
}

void espnowSensorStatusPeriodicTick() {
  if (!gSensorBroadcastEnabled) return;

  extern Settings gSettings;
  if (!meshEnabled()) return;
  if (gSettings.meshRole == MESH_ROLE_MASTER) return;

  unsigned long now = millis();

#if ENABLE_CAMERA_SENSOR
  static unsigned long lastCameraMs = 0;
  if ((now - lastCameraMs) >= 1000UL) {
    lastCameraMs = now;

    PSRAM_JSON_DOC(doc);
    doc["enabled"] = cameraEnabled;
    doc["connected"] = cameraConnected;
    doc["streaming"] = cameraStreaming;
    doc["model"] = cameraModel;
    doc["width"] = cameraWidth;
    doc["height"] = cameraHeight;
    doc["psram"] = psramFound();

    String message;
    serializeJson(doc, message);
    
    sendSensorDataUpdate(REMOTE_SENSOR_CAMERA, message);
  }
#endif

#if ENABLE_MICROPHONE_SENSOR
  static unsigned long lastMicMs = 0;
  if ((now - lastMicMs) >= 1000UL) {
    lastMicMs = now;

    PSRAM_JSON_DOC(doc);
    doc["enabled"] = micEnabled;
    doc["connected"] = micConnected;
    doc["recording"] = micRecording;
    doc["sampleRate"] = micSampleRate;
    doc["bitDepth"] = micBitDepth;
    doc["channels"] = micChannels;
    doc["level"] = (micEnabled && !micRecording) ? getAudioLevel() : 0;

    String message;
    serializeJson(doc, message);
    
    sendSensorDataUpdate(REMOTE_SENSOR_MICROPHONE, message);
  }
#endif
}

// Update local sensor cache (called by sensor polling loops)
// This is a fast, non-blocking write - no ESP-NOW transmission here
void sendSensorDataUpdate(RemoteSensorType sensorType, const String& jsonData) {
  if (sensorType >= REMOTE_SENSOR_MAX) {
    DEBUG_SENSORSF("[CACHE_UPDATE] REJECT: Invalid sensor type %d", sensorType);
    return;
  }
  if (!gSensorStreamingEnabled[sensorType]) {
    // Don't log - too spammy when streaming is disabled
    return;
  }
  
  // Quick cache update with mutex protection
  if (gSensorCacheMutex && xSemaphoreTake(gSensorCacheMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    LocalSensorCache* cache = &gLocalSensorCache[sensorType];
    bool wasDirty = cache->dirty;
    unsigned long timeSinceLastUpdate = millis() - cache->lastUpdate;
    
    size_t len = jsonData.length();
    if (len >= sizeof(cache->jsonData)) len = sizeof(cache->jsonData) - 1;
    memcpy(cache->jsonData, jsonData.c_str(), len);
    cache->jsonData[len] = '\0';
    cache->jsonLength = len;
    cache->dirty = true;
    cache->lastUpdate = millis();
    
    DEBUG_SENSORSF("[CACHE_UPDATE] %s len=%u wasDirty=%d age=%lums json=%.60s",
                   sensorTypeToString(sensorType), (unsigned)len, wasDirty, 
                   timeSinceLastUpdate, cache->jsonData);
    
    xSemaphoreGive(gSensorCacheMutex);
  } else {
    DEBUG_SENSORSF("[CACHE_UPDATE] %s MUTEX_TIMEOUT", sensorTypeToString(sensorType));
  }
}

// Force immediate broadcast of a sensor (event-driven API)
void forceSensorBroadcast(RemoteSensorType sensorType) {
  if (sensorType >= REMOTE_SENSOR_MAX) {
    DEBUG_SENSORSF("[FORCE_SEND] REJECT: Invalid sensor type %d", sensorType);
    return;
  }
  if (!gSensorStreamingEnabled[sensorType]) {
    DEBUG_SENSORSF("[FORCE_SEND] REJECT: %s streaming not enabled", sensorTypeToString(sensorType));
    return;
  }
  
  if (gSensorCacheMutex && xSemaphoreTake(gSensorCacheMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    bool wasDirty = gLocalSensorCache[sensorType].dirty;
    unsigned long cacheAge = millis() - gLocalSensorCache[sensorType].lastUpdate;
    gLocalSensorCache[sensorType].forceSend = true;
    
    DEBUG_SENSORSF("[FORCE_SEND] %s SET (wasDirty=%d age=%lums)", 
                   sensorTypeToString(sensorType), wasDirty, cacheAge);
    
    xSemaphoreGive(gSensorCacheMutex);
  } else {
    DEBUG_SENSORSF("[FORCE_SEND] %s MUTEX_TIMEOUT", sensorTypeToString(sensorType));
  }
}

// Internal: Actually transmit sensor data via ESP-NOW (called by broadcaster task)
static void transmitSensorData(RemoteSensorType sensorType, const char* jsonData, uint16_t jsonLen) {
  DEBUG_SENSORSF("[SENSOR_TX] type=%s len=%u", sensorTypeToString(sensorType), jsonLen);
  
  extern Settings gSettings;
  
  // Send via V3 binary protocol (both bond and mesh modes)
  extern bool v3_broadcast_sensor_data(RemoteSensorType sensorType, const char* jsonData, uint16_t jsonLen);
  
#if ENABLE_BONDED_MODE
  // Check for bond mode first
  if (gSettings.bondModeEnabled && gSettings.bondRole == 0) {
    // Bond mode worker - send via v3 binary protocol to master
    if (isBondModeOnline()) {
      DEBUG_SENSORSF("[SENSOR_DATA_TX] Using v3 binary protocol for bond mode");
      
      // Send JSON data directly via v3 (receiver will store in cache)
      bool sent = sendBondedSensorData((uint8_t)sensorType, 
                                       (const uint8_t*)jsonData, 
                                       jsonLen);
      if (sent) {
        DEBUG_SENSORSF("[SENSOR_DATA_TX] SUCCESS: Sent %s data via v3 to bonded master", 
                       sensorTypeToString(sensorType));
      } else {
        DEBUG_SENSORSF("[SENSOR_DATA_TX] FAILED: v3 send failed for %s", 
                       sensorTypeToString(sensorType));
      }
      return;
    } else {
      DEBUG_SENSORSF("[SENSOR_DATA_TX] SKIP: Bond mode but peer not online");
      return;
    }
  }
#endif // ENABLE_BONDED_MODE
  
  // Mesh mode - check prerequisites and use v2 JSON
  if (!gSensorBroadcastEnabled) {
    DEBUG_SENSORSF("%s", "[SENSOR_DATA_TX] SKIP: Sensor broadcasting not enabled");
    return;
  }
  
  bool meshEn = meshEnabled();
  DEBUG_SENSORSF("[SENSOR_DATA_TX] Pre-checks: meshEnabled=%d, meshRole=%d (0=worker,1=master)",
         meshEn, gSettings.meshRole);
  
  if (!meshEn) {
    DEBUG_SENSORSF("%s", "[SENSOR_DATA_TX] SKIP: Mesh not enabled");
    return;
  }
  
  if (gSettings.meshRole == MESH_ROLE_MASTER) {
    DEBUG_SENSORSF("%s", "[SENSOR_DATA_TX] SKIP: Master devices don't send sensor data");
    return;
  }
  
  // Mesh mode - send via V3 binary protocol
  DEBUG_SENSORSF("%s", "[SENSOR_DATA_TX] Using V3 binary protocol for mesh broadcast");
  
  bool sent = v3_broadcast_sensor_data(sensorType, jsonData, jsonLen);
  if (sent) {
    DEBUG_SENSORSF("[SENSOR_TX] SUCCESS: Broadcast %s data (mesh)", sensorTypeToString(sensorType));
  } else {
    DEBUG_SENSORSF("[SENSOR_TX] ERROR: Failed to broadcast %s data", sensorTypeToString(sensorType));
  }
}

// Broadcaster task - runs periodically and sends dirty/forced sensor data
static void sensorBroadcasterTask(void* param) {
  (void)param;
  
  extern Settings gSettings;
  unsigned long loopCount = 0;
  
  DEBUGF(DEBUG_ESPNOW_CORE, "[SENSOR_BCAST_TASK] Started on core %d", xPortGetCoreID());
  
  for (;;) {
    unsigned long now = millis();
    unsigned long interval = gSettings.sensorBroadcastIntervalMs;
    if (interval < 100) interval = 100;
    if (interval > 10000) interval = 10000;
    
    unsigned long timeSinceLastBroadcast = now - gLastBroadcastTime;
    bool shouldBroadcast = timeSinceLastBroadcast >= interval;
    
    // Log interval check every 20 loops (~1 second)
    if ((loopCount % 20) == 0) {
      DEBUG_SENSORSF("[BCAST_TICK] loop=%lu interval=%lums elapsed=%lums shouldBcast=%d",
                     loopCount, interval, timeSinceLastBroadcast, shouldBroadcast);
    }
    
    // Check each sensor type
    for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
      if (!gSensorStreamingEnabled[i]) continue;
      
      bool needsSend = false;
      char jsonCopy[256];
      uint16_t jsonLen = 0;
      bool wasDirty = false;
      bool wasForced = false;
      unsigned long cacheAge = 0;
      
      // Check if this sensor needs to be sent
      if (gSensorCacheMutex && xSemaphoreTake(gSensorCacheMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        LocalSensorCache* cache = &gLocalSensorCache[i];
        
        wasDirty = cache->dirty;
        wasForced = cache->forceSend;
        cacheAge = now - cache->lastUpdate;
        
        // Decision logic with detailed path tracking
        if (cache->forceSend) {
          // PATH A: Force-send (event-driven, immediate)
          DEBUG_SENSORSF("[BCAST_PATH_A] %s FORCE_SEND (age=%lums len=%u)",
                         sensorTypeToString((RemoteSensorType)i), cacheAge, cache->jsonLength);
          memcpy(jsonCopy, cache->jsonData, cache->jsonLength);
          jsonCopy[cache->jsonLength] = '\0';
          jsonLen = cache->jsonLength;
          cache->dirty = false;
          cache->forceSend = false;
          needsSend = true;
        } else if (cache->dirty && shouldBroadcast) {
          // PATH B: Dirty cache + interval elapsed (periodic)
          DEBUG_SENSORSF("[BCAST_PATH_B] %s DIRTY+INTERVAL (age=%lums len=%u elapsed=%lums)",
                         sensorTypeToString((RemoteSensorType)i), cacheAge, cache->jsonLength, timeSinceLastBroadcast);
          memcpy(jsonCopy, cache->jsonData, cache->jsonLength);
          jsonCopy[cache->jsonLength] = '\0';
          jsonLen = cache->jsonLength;
          cache->dirty = false;
          cache->forceSend = false;
          needsSend = true;
        } else if (cache->dirty && !shouldBroadcast) {
          // PATH C: Dirty but waiting for interval (rate-limited)
          if ((loopCount % 20) == 0) {  // Log every ~1 second
            DEBUG_SENSORSF("[BCAST_PATH_C] %s DIRTY_WAITING (age=%lums wait=%lums)",
                           sensorTypeToString((RemoteSensorType)i), cacheAge, interval - timeSinceLastBroadcast);
          }
        } else if (!cache->dirty && shouldBroadcast) {
          // PATH D: Interval elapsed but cache is clean (no new data)
          if ((loopCount % 20) == 0) {  // Log every ~1 second
            DEBUG_SENSORSF("[BCAST_PATH_D] %s CLEAN_SKIP (age=%lums)",
                           sensorTypeToString((RemoteSensorType)i), cacheAge);
          }
        } else {
          // PATH E: Clean cache, waiting for interval (idle)
          // Don't log this - too spammy
        }
        
        xSemaphoreGive(gSensorCacheMutex);
      }
      
      // Transmit outside of mutex to avoid blocking sensor updates
      if (needsSend && jsonLen > 0) {
        DEBUG_SENSORSF("[BCAST_TX] %s len=%u forced=%d dirty=%d",
                       sensorTypeToString((RemoteSensorType)i), jsonLen, wasForced, wasDirty);
        transmitSensorData((RemoteSensorType)i, jsonCopy, jsonLen);
      }
    }
    
    if (shouldBroadcast) {
      DEBUG_SENSORSF("[BCAST_INTERVAL_RESET] Next broadcast in %lums", interval);
      gLastBroadcastTime = now;
    }
    
    loopCount++;
    // Sleep for 50ms (responsive to force-send events)
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Start the broadcaster task
static bool startSensorBroadcaster() {
  if (gSensorBroadcasterTask) return true;
  
  // Create mutex if needed
  if (!gSensorCacheMutex) {
    gSensorCacheMutex = xSemaphoreCreateMutex();
    if (!gSensorCacheMutex) {
      DEBUGF(DEBUG_ESPNOW_CORE, "[SENSOR_BROADCASTER] Failed to create mutex");
      return false;
    }
  }
  
  // Initialize cache
  memset(gLocalSensorCache, 0, sizeof(gLocalSensorCache));
  
  BaseType_t ret = xTaskCreatePinnedToCore(
    sensorBroadcasterTask,
    "sensor_bcast",
    3072,  // 3KB stack
    nullptr,
    5,     // Priority 5 (same as ESP-NOW task)
    &gSensorBroadcasterTask,
    1      // Core 1 (opposite of ESP-NOW callback which is core 0)
  );
  
  if (ret == pdPASS) {
    DEBUGF(DEBUG_ESPNOW_CORE, "[SENSOR_BROADCASTER] Task started");
    return true;
  } else {
    DEBUGF(DEBUG_ESPNOW_CORE, "[SENSOR_BROADCASTER] Failed to create task");
    return false;
  }
}

// Stop the broadcaster task
static void stopSensorBroadcaster() {
  if (gSensorBroadcasterTask) {
    vTaskDelete(gSensorBroadcasterTask);
    gSensorBroadcasterTask = nullptr;
    DEBUGF(DEBUG_ESPNOW_CORE, "[SENSOR_BROADCASTER] Task stopped");
  }
}

String getRemoteSensorDataJSON(const uint8_t* deviceMac, RemoteSensorType sensorType) {
  RemoteSensorData* entry = findCacheEntry(deviceMac, sensorType);
  if (!entry || !entry->valid) {
    DEBUG_SENSORSF("[GET_REMOTE_JSON] No valid entry for sensor type %d", sensorType);
    return "{\"error\":\"No data available\"}";
  }
  
  // Check if data is expired
  unsigned long now = millis();
  if (now - entry->lastUpdate > REMOTE_SENSOR_TTL_MS) {
    entry->valid = false;
    DEBUG_SENSORSF("[GET_REMOTE_JSON] Data expired for sensor type %d (age=%lu)", sensorType, now - entry->lastUpdate);
    return "{\"error\":\"Data expired\"}";
  }
  
  DEBUG_SENSORSF("[GET_REMOTE_JSON] Returning cached data: entry=%p, valid=%d, lastUpdate=%lu, age=%lu, len=%u, data=%.80s",
                 entry, entry->valid, entry->lastUpdate, now - entry->lastUpdate, entry->jsonLength, entry->jsonData);
  // Return from fixed buffer (creates String only at API response time, not on every cache update)
  return String(entry->jsonData);
}

String getRemoteDevicesListJSON() {
  PSRAM_JSON_DOC(doc);
  JsonArray devices = doc["devices"].to<JsonArray>();
  
  // Build list of unique devices with their sensors
  for (int i = 0; i < MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE; i++) {
    if (!gRemoteSensorCache[i].valid) continue;
    
    // Check if data is expired
    unsigned long now = millis();
    if (now - gRemoteSensorCache[i].lastUpdate > REMOTE_SENSOR_TTL_MS) {
      gRemoteSensorCache[i].valid = false;
      continue;
    }
    
    // Find or create device entry
    String macStr = macToHexString(gRemoteSensorCache[i].deviceMac);
    JsonObject deviceObj;
    bool found = false;
    
    for (JsonObject dev : devices) {
      if (strcmp(dev["mac"], macStr.c_str()) == 0) {
        deviceObj = dev;
        found = true;
        break;
      }
    }
    
    if (!found) {
      deviceObj = devices.add<JsonObject>();
      deviceObj["mac"] = macStr;
      deviceObj["name"] = gRemoteSensorCache[i].deviceName;
      deviceObj["sensors"].to<JsonArray>();
    }
    
    // Add sensor to device
    JsonArray sensors = deviceObj["sensors"];
    sensors.add(sensorTypeToString(gRemoteSensorCache[i].sensorType));
  }
  
  String result;
  serializeJson(doc, result);
  return result;
}

void cleanupExpiredRemoteSensorData() {
  unsigned long now = millis();
  
  for (int i = 0; i < MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE; i++) {
    if (gRemoteSensorCache[i].valid) {
      if (now - gRemoteSensorCache[i].lastUpdate > REMOTE_SENSOR_TTL_MS) {
        DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Expired data for %s %s",
               gRemoteSensorCache[i].deviceName,
               sensorTypeToString(gRemoteSensorCache[i].sensorType));
        gRemoteSensorCache[i].valid = false;
        gRemoteSensorCache[i].jsonData[0] = '\0';
        gRemoteSensorCache[i].jsonLength = 0;
      }
    }
  }
}

// ==========================
// Thermal Data Optimization
// ==========================

#if ENABLE_THERMAL_SENSOR
int buildThermalDataJSONInteger(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;
  
  extern ThermalCache gThermalCache;
  extern bool lockThermalCache(TickType_t timeout);
  extern void unlockThermalCache();
  
  int pos = 0;
  
  if (lockThermalCache(pdMS_TO_TICKS(100))) {
    // Use raw frame only (no interpolation for remote streaming)
    // Swap dimensions if rotation is 90° or 270°
    extern Settings gSettings;
    int width = (gSettings.thermalRotation == 1 || gSettings.thermalRotation == 3) ? 24 : 32;
    int height = (gSettings.thermalRotation == 1 || gSettings.thermalRotation == 3) ? 32 : 24;
    int frameSize = 768;
    
    // Header
    pos = snprintf(buf, bufSize,
                   "{\"val\":%d,\"seq\":%lu,\"mn\":%d,\"mx\":%d,\"w\":%d,\"h\":%d,\"data\":[",
                   gThermalCache.thermalDataValid ? 1 : 0,
                   (unsigned long)gThermalCache.thermalSeq,
                   (int)gThermalCache.thermalMinTemp,  // Integer min
                   (int)gThermalCache.thermalMaxTemp,  // Integer max
                   width, height);
    if (pos < 0 || (size_t)pos >= bufSize) {
      unlockThermalCache();
      return 0;
    }
    
    // Frame data - convert centidegrees to whole degrees
    if (gThermalCache.thermalFrame) {
      for (int i = 0; i < frameSize; i++) {
        // Convert centidegrees (int16_t) to whole degrees (int)
        int wholeDegrees = gThermalCache.thermalFrame[i] / 100;
        int written = snprintf(buf + pos, bufSize - pos, "%d%s", wholeDegrees, (i < frameSize - 1) ? "," : "");
        if (written < 0 || (size_t)written >= (bufSize - pos)) {
          unlockThermalCache();
          return 0;
        }
        pos += written;
      }
    } else {
      unlockThermalCache();
      pos = snprintf(buf, bufSize, "{\"val\":0,\"error\":\"Sensor stopped\"}");
      return (pos > 0) ? pos : 0;
    }
    
    // Footer
    int tail = snprintf(buf + pos, bufSize - pos, "]}");
    if (tail < 0 || (size_t)tail >= (bufSize - pos)) {
      unlockThermalCache();
      return 0;
    }
    pos += tail;
    
    unlockThermalCache();
  } else {
    pos = snprintf(buf, bufSize, "{\"error\":\"Sensor data temporarily unavailable\"}");
    if (pos < 0) pos = 0;
  }
  
  return pos;
}
#else
int buildThermalDataJSONInteger(char* buf, size_t bufSize) {
  if (!buf || bufSize == 0) return 0;
  int pos = snprintf(buf, bufSize, "{\"error\":\"Thermal sensor not compiled\"}");
  return (pos > 0) ? pos : 0;
}
#endif

// ============================================================================
// CLI Commands for Sensor Streaming (merged from espnow_sensor_commands.cpp)
// ============================================================================

const char* cmd_espnow_sensorstream(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  DEBUG_SENSORSF("[SENSOR_STREAM_CMD] Command received: '%s'", cmd.c_str());
  
  // Parse: espnow sensorstream <sensor> <on|off>
  String line = cmd;
  line.trim();
  
  const char* prefix = "espnow sensorstream";
  if (!line.startsWith(prefix)) {
    DEBUG_SENSORSF("[SENSOR_STREAM_CMD] ERROR: Unexpected prefix (line='%s')", line.c_str());
    return "Usage: espnow sensorstream <sensor> <on|off>";
  }
  
  String args = line.substring(strlen(prefix));
  args.trim();
  
  int firstSpace = args.indexOf(' ');
  if (firstSpace < 0) {
    DEBUG_SENSORSF("%s", "[SENSOR_STREAM_CMD] ERROR: Missing sensor name");
    return "Usage: espnow sensorstream <sensor> <on|off>";
  }
  
  String sensorName = args.substring(0, firstSpace);
  sensorName.trim();
  sensorName.toLowerCase();
  
  String action = args.substring(firstSpace + 1);
  action.trim();
  action.toLowerCase();
  
  if (action.indexOf(' ') >= 0) {
    DEBUG_SENSORSF("[SENSOR_STREAM_CMD] ERROR: Too many arguments (action='%s')", action.c_str());
    return "Usage: espnow sensorstream <sensor> <on|off>";
  }
  
  DEBUG_SENSORSF("[SENSOR_STREAM_CMD] Parsed: sensor='%s' action='%s'", sensorName.c_str(), action.c_str());
  
  // Convert sensor name to type
  RemoteSensorType sensorType = stringToSensorType(sensorName.c_str());
  if (strcmp(sensorTypeToString(sensorType), sensorName.c_str()) != 0) {
    DEBUG_SENSORSF("[SENSOR_STREAM_CMD] ERROR: Unknown sensor '%s'", sensorName.c_str());
    return "Usage: espnow sensorstream <sensor> <on|off>";
  }
  DEBUG_SENSORSF("[SENSOR_STREAM_CMD] Sensor type resolved: %d (%s)", sensorType, sensorTypeToString(sensorType));
  
  // Parse action
  bool enable = false;
  if (action == "on" || action == "1" || action == "true") {
    enable = true;
  } else if (action == "off" || action == "0" || action == "false") {
    enable = false;
  } else {
    DEBUG_SENSORSF("[SENSOR_STREAM_CMD] ERROR: Invalid action '%s'", action.c_str());
    return "Usage: espnow sensorstream <sensor> <on|off>";
  }
  
  DEBUG_SENSORSF("[SENSOR_STREAM_CMD] Action: %s streaming", enable ? "ENABLE" : "DISABLE");
  
  // Only workers can stream sensor data
  DEBUG_SENSORSF("[SENSOR_STREAM_CMD] Current mesh role: %d (0=worker, 1=master)", gSettings.meshRole);
  
  if (gSettings.meshRole == MESH_ROLE_MASTER) {
    DEBUG_SENSORSF("%s", "[SENSOR_STREAM_CMD] ERROR: Master devices cannot stream sensor data");
    return "Error: Master devices receive sensor data, they don't stream it";
  }
  
  // Enable/disable streaming
  if (enable) {
    DEBUG_SENSORSF("[SENSOR_STREAM_CMD] Calling startSensorDataStreaming(%d)", sensorType);
    startSensorDataStreaming(sensorType);
    DEBUG_SENSORSF("[SENSOR_STREAM_CMD] SUCCESS: Started streaming %s", sensorTypeToString(sensorType));
    BROADCAST_PRINTF("[ESP-NOW] Started streaming %s sensor data to master", sensorTypeToString(sensorType));
    return "OK: Sensor streaming started";
  } else {
    DEBUG_SENSORSF("[SENSOR_STREAM_CMD] Calling stopSensorDataStreaming(%d)", sensorType);
    stopSensorDataStreaming(sensorType);
    DEBUG_SENSORSF("[SENSOR_STREAM_CMD] SUCCESS: Stopped streaming %s", sensorTypeToString(sensorType));
    BROADCAST_PRINTF("[ESP-NOW] Stopped streaming %s sensor data", sensorTypeToString(sensorType));
    return "OK: Sensor streaming stopped";
  }
}

const char* cmd_espnow_sensorstatus(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Show current streaming status
  // Show master broadcast flag status
  BROADCAST_PRINTF("[ESP-NOW] Sensor broadcast: %s", isSensorBroadcastEnabled() ? "ENABLED" : "DISABLED");
  
  if (gSettings.meshRole == MESH_ROLE_MASTER) {
    // Master: show remote sensor cache status
    String devicesList = getRemoteDevicesListJSON();
    broadcastOutput("[ESP-NOW] Remote sensor cache:");
    broadcastOutput(devicesList);
    return "OK: Remote sensor status displayed";
  } else {
    // Worker: show streaming status
    broadcastOutput("[ESP-NOW] Sensor streaming status:");
    
    const char* sensors[] = {"thermal", "tof", "imu", "gps", "gamepad", "fmradio", "camera", "microphone"};
    for (int i = 0; i < 8; i++) {
      RemoteSensorType type = stringToSensorType(sensors[i]);
      bool enabled = isSensorDataStreamingEnabled(type);
      BROADCAST_PRINTF("  %s: %s", sensors[i], enabled ? "on" : "off");
    }
  }
  
  return "OK: Streaming status displayed";
}

// Enable/disable all sensor ESP-NOW communication (status + data broadcasts)
const char* cmd_espnow_sensorbroadcast(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Parse: espnow sensorbroadcast <on|off>
  String line = cmd;
  line.trim();
  
  const char* prefix = "espnow sensorbroadcast";
  if (!line.startsWith(prefix)) {
    return "Usage: espnow sensorbroadcast <on|off>";
  }
  
  String args = line.substring(strlen(prefix));
  args.trim();
  args.toLowerCase();
  
  if (args.length() == 0) {
    // No argument - show current status
    bool enabled = isSensorBroadcastEnabled();
    BROADCAST_PRINTF("[ESP-NOW] Sensor broadcast is %s", enabled ? "ENABLED" : "DISABLED");
    return enabled ? "Sensor broadcast: on" : "Sensor broadcast: off";
  }
  
  if (args == "on" || args == "1" || args == "true" || args == "enable") {
    setSensorBroadcastEnabled(true);
    broadcastOutput("[ESP-NOW] Sensor broadcast ENABLED - status and data will be sent to master");
    return "OK: Sensor broadcast enabled";
  } else if (args == "off" || args == "0" || args == "false" || args == "disable") {
    setSensorBroadcastEnabled(false);
    broadcastOutput("[ESP-NOW] Sensor broadcast DISABLED - no sensor data will be sent");
    return "OK: Sensor broadcast disabled";
  } else {
    return "Usage: espnow sensorbroadcast <on|off>";
  }
}

// ==========================
// Remote GPS Data Access
// ==========================

#include <ArduinoJson.h>

bool hasRemoteGPSData() {
  unsigned long now = millis();
  
  for (int i = 0; i < MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE; i++) {
    if (gRemoteSensorCache[i].valid && 
        gRemoteSensorCache[i].sensorType == REMOTE_SENSOR_GPS &&
        (now - gRemoteSensorCache[i].lastUpdate) < REMOTE_SENSOR_TTL_MS) {
      return true;
    }
  }
  return false;
}

bool getRemoteGPSData(RemoteGPSData* outData) {
  if (!outData) return false;
  
  memset(outData, 0, sizeof(RemoteGPSData));
  outData->valid = false;
  
  unsigned long now = millis();
  RemoteSensorData* bestEntry = nullptr;
  unsigned long bestTime = 0;
  
  // Find the most recent valid GPS data from any remote device
  for (int i = 0; i < MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE; i++) {
    if (gRemoteSensorCache[i].valid && 
        gRemoteSensorCache[i].sensorType == REMOTE_SENSOR_GPS &&
        (now - gRemoteSensorCache[i].lastUpdate) < REMOTE_SENSOR_TTL_MS) {
      
      if (gRemoteSensorCache[i].lastUpdate > bestTime) {
        bestEntry = &gRemoteSensorCache[i];
        bestTime = gRemoteSensorCache[i].lastUpdate;
      }
    }
  }
  
  if (!bestEntry || bestEntry->jsonLength == 0) {
    return false;
  }
  
  // Parse the JSON data: {"val":1,"fix":1,"quality":1,"sats":8,"lat":37.123,"lon":-122.456,"alt":100.5,"speed":0.5}
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, bestEntry->jsonData, bestEntry->jsonLength);
  if (err) {
    return false;
  }
  
  // Extract GPS values
  outData->hasFix = doc["fix"] | 0;
  outData->fixQuality = doc["quality"] | 0;
  outData->satellites = doc["sats"] | 0;
  outData->latitude = doc["lat"] | 0.0f;
  outData->longitude = doc["lon"] | 0.0f;
  outData->altitude = doc["alt"] | 0.0f;
  outData->speed = doc["speed"] | 0.0f;
  outData->lastUpdate = bestEntry->lastUpdate;
  strncpy(outData->deviceName, bestEntry->deviceName, sizeof(outData->deviceName) - 1);
  outData->deviceName[sizeof(outData->deviceName) - 1] = '\0';
  
  // Only valid if GPS has a fix
  outData->valid = outData->hasFix;
  
  return outData->valid;
}

#endif // ENABLE_ESPNOW
