#include "System_ESPNow_Sensors.h"

#if ENABLE_ESPNOW

#include <ArduinoJson.h>

#include "OLED_Display.h"
#include "System_Debug.h"
#include "System_ESPNow.h"
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

// External functions
extern void broadcastOutput(const String& msg);
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
static unsigned long gLastSensorBroadcast[REMOTE_SENSOR_MAX] = {0};
static const unsigned long SENSOR_BROADCAST_INTERVAL_MS = 1000;  // 1 second for responsive UI

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
static RemoteSensorData* findOrCreateCacheEntry(const uint8_t* deviceMac, const char* deviceName, RemoteSensorType sensorType) {
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

// ==========================
// Message Handlers
// ==========================

void handleSensorStatusMessage(const uint8_t* senderMac, const String& deviceName, const String& message) {
  DEBUG_SENSORSF("[SENSOR_STATUS_RX] Received status message from %s (MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
         deviceName.c_str(), senderMac[0], senderMac[1], senderMac[2], senderMac[3], senderMac[4], senderMac[5]);
  DEBUG_SENSORSF("[SENSOR_STATUS_RX] Message length: %d bytes", message.length());
  DEBUG_SENSORSF("[SENSOR_STATUS_RX] Raw message: %.200s", message.c_str());
  
  JsonDocument doc;
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
  broadcastOutput("[ESP-NOW] Remote sensor " + String(sensorTypeStr) + " on " + deviceName + 
                  " is now " + (enabled ? "enabled" : "disabled"));
}

void handleSensorDataMessage(const uint8_t* senderMac, const String& deviceName, const String& message) {
  DEBUG_SENSORSF("[SENSOR_DATA_RX] Received data message from %s (MAC: %02X:%02X:%02X:%02X:%02X:%02X)",
         deviceName.c_str(), senderMac[0], senderMac[1], senderMac[2], senderMac[3], senderMac[4], senderMac[5]);
  DEBUG_SENSORSF("[SENSOR_DATA_RX] Message length: %d bytes", message.length());
  DEBUG_SENSORSF("[SENSOR_DATA_RX] Raw message (first 200 chars): %.200s", message.c_str());
  
  JsonDocument doc;
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
  
  // Build status message
  DEBUG_SENSORSF("%s", "[SENSOR_STATUS_TX] Building status message JSON");
  JsonDocument doc;
  doc["type"] = MSG_TYPE_SENSOR_STATUS;
  doc["sensor"] = sensorTypeToString(sensorType);
  doc["enabled"] = enabled;
  
  String message;
  serializeJson(doc, message);
  DEBUG_SENSORSF("[SENSOR_STATUS_TX] Message built: %d bytes", message.length());
  DEBUG_SENSORSF("[SENSOR_STATUS_TX] Message: %s", message.c_str());
  
  DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Broadcasting status: %s = %s",
         sensorTypeToString(sensorType), enabled ? "enabled" : "disabled");
  
  // Send to master (will be handled by mesh routing)
  DEBUG_SENSORSF("%s", "[SENSOR_STATUS_TX] Calling meshSendEnvelopeToPeers()...");
  extern void meshSendEnvelopeToPeers(const String& payload);
  meshSendEnvelopeToPeers(message);
  DEBUG_SENSORSF("[SENSOR_STATUS_TX] SUCCESS: Broadcast %s status", sensorTypeToString(sensorType));
}

void startSensorDataStreaming(RemoteSensorType sensorType) {
  DEBUG_SENSORSF("[SENSOR_STREAM] startSensorDataStreaming() called with type=%d (%s)", sensorType, sensorTypeToString(sensorType));
  
  if (sensorType >= REMOTE_SENSOR_MAX) {
    DEBUG_SENSORSF("[SENSOR_STREAM] ERROR: Invalid sensor type %d (max=%d)", sensorType, REMOTE_SENSOR_MAX);
    return;
  }
  
  DEBUG_SENSORSF("[SENSOR_STREAM] Setting streaming flag for %s to TRUE", sensorTypeToString(sensorType));
  gSensorStreamingEnabled[sensorType] = true;
  gLastSensorBroadcast[sensorType] = 0;  // Force immediate broadcast
  
  DEBUG_SENSORSF("[SENSOR_STREAM] Streaming enabled: %s (flag=%d, lastBroadcast=%lu)",
         sensorTypeToString(sensorType), gSensorStreamingEnabled[sensorType], gLastSensorBroadcast[sensorType]);
  
  DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Started streaming for %s",
         sensorTypeToString(sensorType));
  
  broadcastOutput("[ESP-NOW] Started streaming " + String(sensorTypeToString(sensorType)) + " sensor data");
}

void stopSensorDataStreaming(RemoteSensorType sensorType) {
  DEBUG_SENSORSF("[SENSOR_STREAM] stopSensorDataStreaming() called with type=%d (%s)", sensorType, sensorTypeToString(sensorType));
  
  if (sensorType >= REMOTE_SENSOR_MAX) {
    DEBUG_SENSORSF("[SENSOR_STREAM] ERROR: Invalid sensor type %d (max=%d)", sensorType, REMOTE_SENSOR_MAX);
    return;
  }
  
  DEBUG_SENSORSF("[SENSOR_STREAM] Setting streaming flag for %s to FALSE", sensorTypeToString(sensorType));
  gSensorStreamingEnabled[sensorType] = false;
  
  DEBUG_SENSORSF("[SENSOR_STREAM] Streaming disabled: %s (flag=%d)",
         sensorTypeToString(sensorType), gSensorStreamingEnabled[sensorType]);
  
  DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Stopped streaming for %s",
         sensorTypeToString(sensorType));
  
  broadcastOutput("[ESP-NOW] Stopped streaming " + String(sensorTypeToString(sensorType)) + " sensor data");
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

void sendSensorDataUpdate(RemoteSensorType sensorType, const String& jsonData) {
  DEBUG_SENSORSF("[SENSOR_DATA_TX] sendSensorDataUpdate() called: type=%d (%s), dataLen=%d",
         sensorType, sensorTypeToString(sensorType), jsonData.length());
  
  // Check master broadcast flag first
  if (!gSensorBroadcastEnabled) {
    DEBUG_SENSORSF("%s", "[SENSOR_DATA_TX] SKIP: Sensor broadcasting not enabled");
    return;
  }
  
  // Only workers should send data to master
  extern Settings gSettings;
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
  
  // Check if streaming is enabled for this sensor
  if (sensorType >= REMOTE_SENSOR_MAX) {
    DEBUG_SENSORSF("[SENSOR_DATA_TX] SKIP: Invalid sensor type %d", sensorType);
    return;
  }
  
  if (!gSensorStreamingEnabled[sensorType]) {
    DEBUG_SENSORSF("[SENSOR_DATA_TX] SKIP: Streaming not enabled for %s (flag=%d)",
           sensorTypeToString(sensorType), gSensorStreamingEnabled[sensorType]);
    return;
  }
  
  DEBUG_SENSORSF("[SENSOR_DATA_TX] Streaming IS enabled for %s", sensorTypeToString(sensorType));
  
  // Rate limit broadcasts
  unsigned long now = millis();
  unsigned long timeSinceLastBroadcast = now - gLastSensorBroadcast[sensorType];
  DEBUG_SENSORSF("[SENSOR_DATA_TX] Rate limit check: now=%lu, last=%lu, delta=%lu, interval=%lu",
         now, gLastSensorBroadcast[sensorType], timeSinceLastBroadcast, SENSOR_BROADCAST_INTERVAL_MS);
  
  if (timeSinceLastBroadcast < SENSOR_BROADCAST_INTERVAL_MS) {
    DEBUG_SENSORSF("[SENSOR_DATA_TX] SKIP: Rate limited (need %lu more ms)",
           SENSOR_BROADCAST_INTERVAL_MS - timeSinceLastBroadcast);
    return;
  }
  
  DEBUG_SENSORSF("%s", "[SENSOR_DATA_TX] Rate limit passed, proceeding with broadcast");
  gLastSensorBroadcast[sensorType] = now;
  
  // Build sensor data message
  DEBUG_SENSORSF("%s", "[SENSOR_DATA_TX] Building JSON message envelope");
  JsonDocument doc;
  doc["type"] = MSG_TYPE_SENSOR_DATA;
  doc["sensor"] = sensorTypeToString(sensorType);
  DEBUG_SENSORSF("[SENSOR_DATA_TX] Envelope: type=%s, sensor=%s", MSG_TYPE_SENSOR_DATA, sensorTypeToString(sensorType));
  
  // Parse the incoming JSON data and attach it
  DEBUG_SENSORSF("[SENSOR_DATA_TX] Parsing sensor data JSON (len=%d)", jsonData.length());
  JsonDocument dataDoc;
  DeserializationError error = deserializeJson(dataDoc, jsonData);
  if (error) {
    DEBUG_SENSORSF("[SENSOR_DATA_TX] ERROR: Failed to parse sensor data JSON: %s", error.c_str());
    DEBUG_SENSORSF("[SENSOR_DATA_TX] Raw data (first 200 chars): %.200s", jsonData.c_str());
    DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Failed to parse sensor data for broadcast: %s", error.c_str());
    return;
  }
  DEBUG_SENSORSF("%s", "[SENSOR_DATA_TX] JSON parsed successfully");
  
  doc["data"] = dataDoc.as<JsonObject>();
  
  String message;
  serializeJson(doc, message);
  DEBUG_SENSORSF("[SENSOR_DATA_TX] Final message built: %d bytes", message.length());
  DEBUG_SENSORSF("[SENSOR_DATA_TX] Message preview (first 200 chars): %.200s", message.c_str());
  
  DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Sending %s data (%d bytes)",
         sensorTypeToString(sensorType), message.length());
  
  // Send to master (will use fragmentation if needed)
  DEBUG_SENSORSF("%s", "[SENSOR_DATA_TX] Calling meshSendEnvelopeToPeers()...");
  extern void meshSendEnvelopeToPeers(const String& payload);
  meshSendEnvelopeToPeers(message);
  DEBUG_SENSORSF("[SENSOR_DATA_TX] SUCCESS: Sent %s data to master", sensorTypeToString(sensorType));
}

// ==========================
// Web API Functions
// ==========================

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
  JsonDocument doc;
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
    broadcastOutput("[ESP-NOW] Started streaming " + String(sensorTypeToString(sensorType)) + " sensor data to master");
    return "OK: Sensor streaming started";
  } else {
    DEBUG_SENSORSF("[SENSOR_STREAM_CMD] Calling stopSensorDataStreaming(%d)", sensorType);
    stopSensorDataStreaming(sensorType);
    DEBUG_SENSORSF("[SENSOR_STREAM_CMD] SUCCESS: Stopped streaming %s", sensorTypeToString(sensorType));
    broadcastOutput("[ESP-NOW] Stopped streaming " + String(sensorTypeToString(sensorType)) + " sensor data");
    return "OK: Sensor streaming stopped";
  }
}

const char* cmd_espnow_sensorstatus(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Show current streaming status
  // Show master broadcast flag status
  broadcastOutput("[ESP-NOW] Sensor broadcast: " + String(isSensorBroadcastEnabled() ? "ENABLED" : "DISABLED"));
  
  if (gSettings.meshRole == MESH_ROLE_MASTER) {
    // Master: show remote sensor cache status
    String devicesList = getRemoteDevicesListJSON();
    broadcastOutput("[ESP-NOW] Remote sensor cache:");
    broadcastOutput(devicesList);
    return "OK: Remote sensor status displayed";
  } else {
    // Worker: show streaming status
    broadcastOutput("[ESP-NOW] Sensor streaming status:");
    
    const char* sensors[] = {"thermal", "tof", "imu", "gps", "gamepad", "fmradio"};
    for (int i = 0; i < 6; i++) {
      RemoteSensorType type = stringToSensorType(sensors[i]);
      bool enabled = isSensorDataStreamingEnabled(type);
      broadcastOutput("  " + String(sensors[i]) + ": " + (enabled ? "on" : "off"));
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
    broadcastOutput("[ESP-NOW] Sensor broadcast is " + String(enabled ? "ENABLED" : "DISABLED"));
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

#endif // ENABLE_ESPNOW
