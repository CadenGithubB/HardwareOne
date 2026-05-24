#include "System_ESPNow_Sensors.h"

#if ENABLE_ESPNOW

#include <ArduinoJson.h>

#include "OLED_Display.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_ESPNow.h"
#include "System_MemUtil.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_Utils.h"

#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor_seesaw.h"
#endif
#if ENABLE_GPS_SENSOR
#include "i2csensor_pa1010d.h"
#endif
#if ENABLE_IMU_SENSOR
#include "i2csensor_bno055.h"
#endif
#if ENABLE_THERMAL_SENSOR
#include "i2csensor_mlx90640.h"
#endif
#if ENABLE_TOF_SENSOR
#include "i2csensor_vl53l4cx.h"
#endif
#if ENABLE_FM_RADIO
  #include "i2csensor_rda5807.h"
#endif
#if ENABLE_RTC_SENSOR
#include "i2csensor_ds3231.h"
#endif
#if ENABLE_PRESENCE_SENSOR
#include "i2csensor_sths34pf80.h"
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

EXT_RAM_BSS_ATTR RemoteSensorData gRemoteSensorCache[MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE];

// Master flag to enable/disable all sensor ESP-NOW communication (status + data)
// Must be explicitly enabled before any sensor broadcasts will be sent
static bool gSensorBroadcastEnabled = false;

// Sensor streaming state (worker devices only)
static bool gSensorStreamingEnabled[REMOTE_SENSOR_MAX] = {false};

// Broadcaster task state
static TaskHandle_t gSensorBroadcasterTask = nullptr;

// Per-sensor "last transmit" timestamps so each sensor paces independently.
// Reset to 0 in startSensorDataStreaming() so the first tick after enable
// always satisfies the interval check (no separate force-send flag needed).
static unsigned long gLastTxMs[REMOTE_SENSOR_MAX] = {0};

// ==========================
// Sensor JSON builder dispatch
// ==========================
//
// Each sensor exports a builder of the shape `int build(char*, size_t)` that
// reads from its own native cache (under the sensor's mutex) and writes JSON
// into the supplied buffer. The broadcaster calls these on demand — there is
// no intermediate "wire cache" to keep in sync, which makes the entire class
// of "first toggle ON shows nothing" bugs structurally impossible.
//
// minIntervalMs controls per-sensor pacing. Gamepad wants ~10 Hz so button
// presses feel responsive; everything else is happy at 1 Hz.
//
// bufBytes is the size required for this sensor's largest possible JSON
// output. The broadcaster allocates ONE shared PSRAM buffer sized to the
// max across all sensors and reuses it forever — no per-tick allocations.

typedef int (*SensorJSONBuilder)(char* buf, size_t bufSize);

struct SensorBroadcastSpec {
  SensorJSONBuilder builder;
  uint16_t minIntervalMs;
  uint16_t bufBytes;
};

static const SensorBroadcastSpec gSensorSpecs[REMOTE_SENSOR_MAX] = {
#if ENABLE_THERMAL_SENSOR
  [REMOTE_SENSOR_THERMAL]    = { buildThermalDataJSONInteger, 1000, 4096 },
#else
  [REMOTE_SENSOR_THERMAL]    = { nullptr, 0, 0 },
#endif
#if ENABLE_TOF_SENSOR
  [REMOTE_SENSOR_TOF]        = { tofBuildDataJSON,            500,  1024 },
#else
  [REMOTE_SENSOR_TOF]        = { nullptr, 0, 0 },
#endif
#if ENABLE_IMU_SENSOR
  [REMOTE_SENSOR_IMU]        = { imuBuildDataJSON,            500,  512  },
#else
  [REMOTE_SENSOR_IMU]        = { nullptr, 0, 0 },
#endif
#if ENABLE_GPS_SENSOR
  [REMOTE_SENSOR_GPS]        = { gpsBuildDataJSON,            1000, 256  },
#else
  [REMOTE_SENSOR_GPS]        = { nullptr, 0, 0 },
#endif
#if ENABLE_GAMEPAD_SENSOR
  [REMOTE_SENSOR_GAMEPAD]    = { gamepadBuildDataJSON,        100,  128  },
#else
  [REMOTE_SENSOR_GAMEPAD]    = { nullptr, 0, 0 },
#endif
#if ENABLE_FM_RADIO
  [REMOTE_SENSOR_FMRADIO]    = { fmRadioBuildDataJSON,        1000, 512  },
#else
  [REMOTE_SENSOR_FMRADIO]    = { nullptr, 0, 0 },
#endif
  [REMOTE_SENSOR_CAMERA]     = { nullptr, 0, 0 },
  [REMOTE_SENSOR_MICROPHONE] = { nullptr, 0, 0 },
#if ENABLE_RTC_SENSOR
  [REMOTE_SENSOR_RTC]        = { rtcBuildDataJSON,            1000, 256  },
#else
  [REMOTE_SENSOR_RTC]        = { nullptr, 0, 0 },
#endif
#if ENABLE_PRESENCE_SENSOR
  [REMOTE_SENSOR_PRESENCE]   = { presenceBuildDataJSON,       500,  256  },
#else
  [REMOTE_SENSOR_PRESENCE]   = { nullptr, 0, 0 },
#endif
  [REMOTE_SENSOR_APDS]       = { nullptr, 0, 0 },
};

// Shared broadcaster buffer (PSRAM). Allocated once on first broadcaster start,
// sized to the largest sensor's bufBytes. Reused across all sensors and all ticks.
static char* gBroadcasterBuf = nullptr;
static size_t gBroadcasterBufSize = 0;

// ==========================
// Initialization
// ==========================

void initRemoteSensorSystem() {
  // Initialize master-side remote-sensor cache (the cache of OTHER devices' data
  // we've received). The worker-side wire cache is gone — sensors are read on
  // demand by the broadcaster, so there's no chalkboard to keep in sync.
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
// Worker → Master Broadcasting
// ==========================

void broadcastSensorStatus(RemoteSensorType sensorType, bool enabled) {
  DEBUG_ESPNOW_METADATAF("[SENSOR_STATUS_TX] broadcastSensorStatus() called: type=%d (%s), enabled=%d",
         sensorType, sensorTypeToString(sensorType), enabled);
  
  // Check master broadcast flag first
  if (!gSensorBroadcastEnabled) {
    DEBUG_ESPNOW_METADATAF("%s", "[SENSOR_STATUS_TX] SKIP: Sensor broadcasting not enabled");
    return;
  }
  
  // Only workers should broadcast to master
  bool meshEn = meshEnabled();
  DEBUG_ESPNOW_METADATAF("[SENSOR_STATUS_TX] Pre-checks: meshEnabled=%d, meshRole=%d", meshEn, gSettings.meshRole);
  
  if (!meshEn) {
    DEBUG_ESPNOW_METADATAF("%s", "[SENSOR_STATUS_TX] SKIP: Mesh not enabled");
    return;
  }
  
  if (gSettings.meshRole == MESH_ROLE_MASTER) {
    DEBUG_ESPNOW_METADATAF("%s", "[SENSOR_STATUS_TX] SKIP: Master devices don't broadcast status");
    return;
  }
  
  // Build and send V3 status message
  DEBUG_ESPNOW_METADATAF("%s", "[SENSOR_STATUS_TX] Broadcasting V3 sensor status");
  
  DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Broadcasting status: %s = %s",
         sensorTypeToString(sensorType), enabled ? "enabled" : "disabled");
  
  // Send via V4 binary protocol
  extern bool v4_broadcast_sensor_status(RemoteSensorType sensorType, bool enabled);
  bool sent = v4_broadcast_sensor_status(sensorType, enabled);
  
  if (sent) {
    DEBUG_ESPNOW_METADATAF("[SENSOR_STATUS_TX] SUCCESS: Broadcast %s status", sensorTypeToString(sensorType));
  } else {
    DEBUG_ESPNOW_METADATAF("[SENSOR_STATUS_TX] ERROR: Failed to broadcast %s status", sensorTypeToString(sensorType));
  }
}

// Forward declarations
static bool startSensorBroadcaster();
static void stopSensorBroadcaster();

void startSensorDataStreaming(RemoteSensorType sensorType) {
  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] startSensorDataStreaming() called with type=%d (%s)", sensorType, sensorTypeToString(sensorType));
  
  if (sensorType >= REMOTE_SENSOR_MAX) {
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] ERROR: Invalid sensor type %d (max=%d)", sensorType, REMOTE_SENSOR_MAX);
    return;
  }
  
#if ENABLE_BONDED_MODE
  // Bond master: send STREAM_CTRL to worker — master doesn't have the sensors locally
  if (gSettings.bondModeEnabled && isBondMaster()) {
    extern bool sendBondStreamCtrl(RemoteSensorType sensorType, bool enable);
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] Bond master: sending STREAM_CTRL %s ON to worker", sensorTypeToString(sensorType));
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
  
  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] Setting streaming flag for %s to TRUE", sensorTypeToString(sensorType));
  
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

  // Reset lastTxMs so the broadcaster's next tick (within ~50ms) fires immediately
  // for this sensor. No separate force-send flag needed.
  gLastTxMs[sensorType] = 0;

  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] Streaming enabled: %s (flag=%d)",
         sensorTypeToString(sensorType), gSensorStreamingEnabled[sensorType]);
  
  DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Started streaming for %s",
         sensorTypeToString(sensorType));
  
  BROADCAST_PRINTF("[ESP-NOW] Started streaming %s sensor data", sensorTypeToString(sensorType));
}

void stopSensorDataStreaming(RemoteSensorType sensorType) {
  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] stopSensorDataStreaming() called with type=%d (%s)", sensorType, sensorTypeToString(sensorType));
  
  if (sensorType >= REMOTE_SENSOR_MAX) {
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] ERROR: Invalid sensor type %d (max=%d)", sensorType, REMOTE_SENSOR_MAX);
    return;
  }
  
#if ENABLE_BONDED_MODE
  // Bond master: send STREAM_CTRL OFF to worker
  if (gSettings.bondModeEnabled && isBondMaster()) {
    extern bool sendBondStreamCtrl(RemoteSensorType sensorType, bool enable);
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] Bond master: sending STREAM_CTRL %s OFF to worker", sensorTypeToString(sensorType));
    sendBondStreamCtrl(sensorType, false);
    // Update local flag so UI reflects the stopped streaming state
    gSensorStreamingEnabled[sensorType] = false;
    BROADCAST_PRINTF("[ESP-NOW] Requested worker to stop streaming %s sensor data", sensorTypeToString(sensorType));
    return;
  }
#endif
  
  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] Setting streaming flag for %s to FALSE", sensorTypeToString(sensorType));
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
  
  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] Streaming disabled: %s (flag=%d)",
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
  DEBUG_ESPNOW_STREAMF("[SENSOR_BROADCAST] Sensor broadcasting %s", enabled ? "ENABLED" : "DISABLED");
}

bool isSensorBroadcastEnabled() {
  return gSensorBroadcastEnabled;
}

// Internal: Actually transmit sensor data via ESP-NOW (called by broadcaster task)
static void transmitSensorData(RemoteSensorType sensorType, const char* jsonData, uint16_t jsonLen) {
  DEBUG_ESPNOW_STREAMF("[SENSOR_TX] type=%s len=%u", sensorTypeToString(sensorType), jsonLen);
  
  
  // Send via V4 binary protocol (both bond and mesh modes)
  extern bool v4_broadcast_sensor_data(RemoteSensorType sensorType, const char* jsonData, uint16_t jsonLen);
  
#if ENABLE_BONDED_MODE
  // Check for bond mode first
  if (gSettings.bondModeEnabled && isBondWorker()) {
    // Bond mode worker - send via v3 binary protocol to master
    if (isBondModeOnline()) {
      DEBUG_ESPNOW_STREAMF("[SENSOR_DATA_TX] Using v3 binary protocol for bond mode");
      
      // Send JSON data directly via v3 (receiver will store in cache)
      bool sent = sendBondedSensorData((uint8_t)sensorType, 
                                       (const uint8_t*)jsonData, 
                                       jsonLen);
      if (sent) {
        DEBUG_ESPNOW_STREAMF("[SENSOR_DATA_TX] SUCCESS: Sent %s data via v3 to bonded master", 
                       sensorTypeToString(sensorType));
      } else {
        DEBUG_ESPNOW_STREAMF("[SENSOR_DATA_TX] FAILED: v3 send failed for %s", 
                       sensorTypeToString(sensorType));
      }
      return;
    } else {
      DEBUG_ESPNOW_STREAMF("[SENSOR_DATA_TX] SKIP: Bond mode but peer not online");
      return;
    }
  }
#endif // ENABLE_BONDED_MODE
  
  // Mesh mode - check prerequisites and use v2 JSON
  if (!gSensorBroadcastEnabled) {
    DEBUG_ESPNOW_STREAMF("%s", "[SENSOR_DATA_TX] SKIP: Sensor broadcasting not enabled");
    return;
  }
  
  bool meshEn = meshEnabled();
  DEBUG_ESPNOW_STREAMF("[SENSOR_DATA_TX] Pre-checks: meshEnabled=%d, meshRole=%d (0=worker,1=master)",
         meshEn, gSettings.meshRole);
  
  if (!meshEn) {
    DEBUG_ESPNOW_STREAMF("%s", "[SENSOR_DATA_TX] SKIP: Mesh not enabled");
    return;
  }
  
  if (gSettings.meshRole == MESH_ROLE_MASTER) {
    DEBUG_ESPNOW_STREAMF("%s", "[SENSOR_DATA_TX] SKIP: Master devices don't send sensor data");
    return;
  }
  
  // Mesh mode - send via V4 binary protocol
  DEBUG_ESPNOW_STREAMF("%s", "[SENSOR_DATA_TX] Using V4 binary protocol for mesh broadcast");
  
  bool sent = v4_broadcast_sensor_data(sensorType, jsonData, jsonLen);
  if (sent) {
    DEBUG_ESPNOW_STREAMF("[SENSOR_TX] SUCCESS: Broadcast %s data (mesh)", sensorTypeToString(sensorType));
  } else {
    DEBUG_ESPNOW_STREAMF("[SENSOR_TX] ERROR: Failed to broadcast %s data", sensorTypeToString(sensorType));
  }
}

// Broadcaster task — wakes every 50ms; for each streaming-enabled sensor whose
// per-sensor interval has elapsed, calls its builder on demand and transmits.
// No intermediate "wire cache": the builder reads the sensor's own native cache
// under that sensor's mutex, so there is no propagation step that could be
// skipped or gated incorrectly.
static void sensorBroadcasterTask(void* param) {
  (void)param;

  DEBUGF(DEBUG_ESPNOW_CORE, "[SENSOR_BCAST_TASK] Started on core %d", xPortGetCoreID());

  for (;;) {
    const unsigned long now = millis();

    for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
      if (!gSensorStreamingEnabled[i]) continue;
      const SensorBroadcastSpec& spec = gSensorSpecs[i];
      if (!spec.builder) continue;  // sensor not compiled in

      // Honor BOTH the per-sensor minimum and the user-tunable global setting,
      // taking whichever is slower so the user can throttle but never speed up
      // past a sensor's native cadence.
      unsigned long interval = spec.minIntervalMs;
      if (gSettings.sensorBroadcastIntervalMs > interval) {
        interval = gSettings.sensorBroadcastIntervalMs;
      }
      if (now - gLastTxMs[i] < interval) continue;

      // Buffer is shared across all sensors and was sized at task start to fit
      // the largest spec.bufBytes. Builder returns bytes written or 0 on failure.
      if (!gBroadcasterBuf || gBroadcasterBufSize < spec.bufBytes) continue;
      int len = spec.builder(gBroadcasterBuf, gBroadcasterBufSize);
      if (len <= 0) continue;

      DEBUG_ESPNOW_STREAMF("[BCAST_TX] %s len=%d", sensorTypeToString((RemoteSensorType)i), len);
      transmitSensorData((RemoteSensorType)i, gBroadcasterBuf, (uint16_t)len);
      gLastTxMs[i] = now;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// Start the broadcaster task
static bool startSensorBroadcaster() {
  if (gSensorBroadcasterTask) return true;

  // Allocate the shared broadcaster buffer in PSRAM, sized to the largest
  // sensor's bufBytes. One allocation, reused forever — no per-tick churn.
  if (!gBroadcasterBuf) {
    size_t maxBuf = 0;
    for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
      if (gSensorSpecs[i].bufBytes > maxBuf) maxBuf = gSensorSpecs[i].bufBytes;
    }
    if (maxBuf == 0) maxBuf = 256;  // defensive; should not happen if any sensor is enabled
    gBroadcasterBuf = (char*)ps_malloc(maxBuf);
    if (!gBroadcasterBuf) {
      DEBUGF(DEBUG_ESPNOW_CORE, "[SENSOR_BROADCASTER] Failed to alloc %u-byte PSRAM buffer", (unsigned)maxBuf);
      return false;
    }
    gBroadcasterBufSize = maxBuf;
  }

  BaseType_t ret = xTaskCreatePinnedToCore(
    sensorBroadcasterTask,
    "sensor_bcast",
    SENSOR_BCAST_STACK_WORDS,
    nullptr,
    TASK_PRIORITY_HIGH,
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
    DEBUG_ESPNOW_METADATAF("[GET_REMOTE_JSON] No valid entry for sensor type %d", sensorType);
    return "{\"error\":\"No data available\"}";
  }
  
  // Check if data is expired
  unsigned long now = millis();
  if (now - entry->lastUpdate > REMOTE_SENSOR_TTL_MS) {
    entry->valid = false;
    DEBUG_ESPNOW_METADATAF("[GET_REMOTE_JSON] Data expired for sensor type %d (age=%lu)", sensorType, now - entry->lastUpdate);
    return "{\"error\":\"Data expired\"}";
  }
  
  DEBUG_ESPNOW_METADATAF("[GET_REMOTE_JSON] Returning cached data: entry=%p, valid=%d, lastUpdate=%lu, age=%lu, len=%u, data=%.80s",
                 entry, entry->valid, entry->lastUpdate, now - entry->lastUpdate, entry->jsonLength, entry->jsonData);
  // Return from fixed buffer (creates String only at API response time, not on every cache update)
  return String(entry->jsonData);
}

int formatRemoteSensorReadable(const char* json, char* out, size_t outSize, int maxLines) {
  if (!out || outSize == 0) return 0;
  out[0] = '\0';
  if (!json || !json[0]) { strncpy(out, "(no data)", outSize - 1); out[outSize - 1] = '\0'; return 1; }

  PSRAM_JSON_DOC(doc);
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    strncpy(out, "(unreadable)", outSize - 1); out[outSize - 1] = '\0'; return 1;
  }
  JsonObjectConst root = doc.as<JsonObjectConst>();
  if (root.isNull()) { strncpy(out, "(no fields)", outSize - 1); out[outSize - 1] = '\0'; return 1; }

  size_t pos = 0;
  int lines = 0;
  for (JsonPairConst kv : root) {
    if (lines >= maxLines) break;
    const char* key = kv.key().c_str();
    if (!key || !key[0]) continue;
    // Skip bookkeeping/noise keys that aren't useful on a small screen.
    if (!strcmp(key, "ts") || !strcmp(key, "seq") || !strcmp(key, "val") || !strcmp(key, "valid"))
      continue;

    char valbuf[20];
    JsonVariantConst v = kv.value();
    if (v.is<bool>()) {
      snprintf(valbuf, sizeof(valbuf), "%s", v.as<bool>() ? "yes" : "no");
    } else if (v.is<const char*>()) {
      const char* s = v.as<const char*>();
      snprintf(valbuf, sizeof(valbuf), "%s", s ? s : "");
    } else if (v.is<float>()) {  // any JSON number (int or real)
      double d = v.as<double>();
      if (d == (double)(long long)d) snprintf(valbuf, sizeof(valbuf), "%lld", (long long)d);
      else                           snprintf(valbuf, sizeof(valbuf), "%.2f", d);
    } else {
      continue;  // arrays / nested objects / null — skip on a small screen
    }

    int n = snprintf(out + pos, outSize - pos, "%s%s: %s", lines ? "\n" : "", key, valbuf);
    if (n < 0) break;
    if ((size_t)n >= outSize - pos) { out[outSize - 1] = '\0'; lines++; break; }
    pos += (size_t)n;
    lines++;
  }
  if (lines == 0) { strncpy(out, "(no fields)", outSize - 1); out[outSize - 1] = '\0'; return 1; }
  return lines;
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
  
  extern bool lockThermalCache(TickType_t timeout);
  extern void unlockThermalCache();
  
  int pos = 0;
  
  if (lockThermalCache(pdMS_TO_TICKS(100))) {
    // Use raw frame only (no interpolation for remote streaming)
    // Swap dimensions if rotation is 90° or 270°
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

const char* cmd_espnow_sensorstream(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM_CMD] Command received: '%s'", argsInput.c_str());
  
  // Parse: <sensor> <on|off>  (dispatcher strips "espnow sensorstream" prefix)
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(2)) {
    DEBUG_ESPNOW_STREAMF("%s", "[SENSOR_STREAM_CMD] ERROR: Missing sensor name");
    return "Usage: espnow sensorstream <sensor> <on|off>";
  }

  String sensorName = a.arg(0);
  normalizeCliArg(sensorName);

  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM_CMD] Parsed: sensor='%s' action='%s'", sensorName.c_str(), a.arg(1).c_str());

  // Convert sensor name to type
  RemoteSensorType sensorType = stringToSensorType(sensorName.c_str());
  if (strcmp(sensorTypeToString(sensorType), sensorName.c_str()) != 0) {
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM_CMD] ERROR: Unknown sensor '%s'", sensorName.c_str());
    return "Usage: espnow sensorstream <sensor> <on|off>";
  }
  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM_CMD] Sensor type resolved: %d (%s)", sensorType, sensorTypeToString(sensorType));

  // Parse action
  int boolResult = parseBoolArg(a.arg(1));
  if (boolResult < 0) {
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM_CMD] ERROR: Invalid action '%s'", a.arg(1).c_str());
    return "Usage: espnow sensorstream <sensor> <on|off>";
  }
  bool enable = (boolResult == 1);
  
  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM_CMD] Action: %s streaming", enable ? "ENABLE" : "DISABLE");
  
  // Only workers can stream sensor data
  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM_CMD] Current mesh role: %d (0=worker, 1=master)", gSettings.meshRole);
  
  if (gSettings.meshRole == MESH_ROLE_MASTER) {
    DEBUG_ESPNOW_STREAMF("%s", "[SENSOR_STREAM_CMD] ERROR: Master devices cannot stream sensor data");
    return "Error: Master devices receive sensor data, they don't stream it";
  }
  
  // Enable/disable streaming
  if (enable) {
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM_CMD] Calling startSensorDataStreaming(%d)", sensorType);
    startSensorDataStreaming(sensorType);
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM_CMD] SUCCESS: Started streaming %s", sensorTypeToString(sensorType));
    BROADCAST_PRINTF("[ESP-NOW] Started streaming %s sensor data to master", sensorTypeToString(sensorType));
    return "OK: Sensor streaming started";
  } else {
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM_CMD] Calling stopSensorDataStreaming(%d)", sensorType);
    stopSensorDataStreaming(sensorType);
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM_CMD] SUCCESS: Stopped streaming %s", sensorTypeToString(sensorType));
    BROADCAST_PRINTF("[ESP-NOW] Stopped streaming %s sensor data", sensorTypeToString(sensorType));
    return "OK: Sensor streaming stopped";
  }
}

const char* cmd_espnow_sensorstatus(const String& argsInput) {
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
const char* cmd_espnow_sensorbroadcast(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  // Parse: <on|off>  (dispatcher strips "espnow sensorbroadcast" prefix)
  String args = argsInput;
  normalizeCliArg(args);
  
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
  PSRAM_JSON_DOC(doc);
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
