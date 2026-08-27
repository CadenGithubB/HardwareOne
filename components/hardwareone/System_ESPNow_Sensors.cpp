#include "System_ESPNow_Sensors.h"

#if ENABLE_ESPNOW

#include <atomic>
#include <ArduinoJson.h>
#include <freertos/semphr.h>

#include "OLED_Display.h"
#include "System_AuthIdentity.h"      // currentAuthContext() — D1 local-console-only gate
#include "System_Command.h"
#include "System_Debug.h"
#include "System_ESPNow.h"
#include "System_ESPNow_Identity.h"   // peerIdentityFindByMac / espnowIdentityFormatPubHex — fingerprint auth
#include "System_MemUtil.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_Utils.h"

#if ENABLE_GAMEPAD_SENSOR
#include "i2csensor_seesaw.h"
#elif ENABLE_ANO_ENCODER
#include "i2csensor_ano_encoder.h"   // anoEncoderBuildDataJSON — the ANO input builder
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
#if ENABLE_MICROPHONE
#include "System_Microphone.h"
#endif

// External functions
extern void meshSendEnvelopeToPeers(const String& payload);
extern String macToHexString(const uint8_t* mac);

// ==========================
// Remote Sensor Data Cache
// ==========================

RemoteSensorData* gRemoteSensorCache = nullptr;

// Master flag to enable/disable all sensor ESP-NOW communication (status + data)
// Must be explicitly enabled before any sensor broadcasts will be sent
static bool gSensorBroadcastEnabled = false;

// Sensor streaming state (worker devices only)
static bool gSensorStreamingEnabled[REMOTE_SENSOR_MAX] = {false};

// Broadcaster task state
static TaskHandle_t gSensorBroadcasterTask = nullptr;
// Serializes the broadcaster lifecycle. Explicit stop is cooperative: the task
// finishes its current builder/send, publishes a join semaphore, then parks so
// the lifecycle owner can delete it without stranding a mutex/runtime guard.
static portMUX_TYPE gSensorBcastTaskMux = portMUX_INITIALIZER_UNLOCKED;
static std::atomic<bool> gSensorBroadcasterStopRequested{false};
static std::atomic<bool> gSensorStreamingShutdown{true};
static bool gSensorBroadcasterExternalStop = false;  // under gSensorBcastTaskMux
static StaticSemaphore_t gSensorBroadcasterStopDoneStorage;
static SemaphoreHandle_t gSensorBroadcasterStopDone = nullptr;
// Serializes the whole create/publish and stop/join/free transactions. A
// portMUX only protects tiny handle/flag snapshots; it cannot be held across
// allocation, xTaskCreate, a join wait, or free.
static StaticSemaphore_t gSensorBroadcasterLifecycleMutexStorage;
static SemaphoreHandle_t gSensorBroadcasterLifecycleMutex =
    xSemaphoreCreateMutexStatic(&gSensorBroadcasterLifecycleMutexStorage);

// Per-sensor "last transmit" timestamps so each sensor paces independently.
// Reset to 0 in startSensorDataStreaming() so the first tick after enable
// always satisfies the interval check (no separate force-send flag needed).
static unsigned long gLastTxMs[REMOTE_SENSOR_MAX] = {0};

// ---- Secure sensor fetcher lease state (worker) ----
// Per-sensor lease deadline (millis). 0 = no lease: manual/local streaming that
// never auto-expires. >0 = a leased subscription that self-stops in the broadcaster
// task once millis() passes it (docs/ESPNOW_SENSOR_FETCHER_DESIGN.md, H3 fix).
static uint32_t gSensorLeaseExpiresAt[REMOTE_SENSOR_MAX] = {0};
// Per-sensor requested cadence (ms) from the leasing controller. 0 = use the global
// gSettings.sensorBroadcastIntervalMs. The broadcaster floors both at spec.minIntervalMs.
static uint32_t gSensorReqIntervalMs[REMOTE_SENSOR_MAX] = {0};
// The authorized controller's MAC: the encrypted-unicast SENSOR_ENVELOPE reply target.
// All-zero = no controller set → leased/manual streaming emits nothing on the air (D2).
static uint8_t gSensorControllerMac[6] = {0};

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
// minIntervalMs controls per-sensor pacing. The input device (gamepad / ANO)
// wants ~10 Hz so button presses feel responsive. TOF, IMU and presence sit at
// 2 Hz — they read as motion, so 1 Hz looks laggy. GPS, FM radio and RTC change
// slowly enough that 1 Hz is plenty. The table below is the authority.
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
  // DISABLED: the 768-pixel frame always exceeds the 200 B sensor-broadcast gate
  // (v4_broadcast_sensor_data), so it was built every second and silently dropped.
  // Re-enable over the mesh by pointing this at thermalBuildSummaryJSON (~90 B).
  [REMOTE_SENSOR_THERMAL]    = { nullptr, 0, 0 },
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
  [REMOTE_SENSOR_INPUT]    = { gamepadBuildDataJSON,        100,  128  },
#elif ENABLE_ANO_ENCODER
  // ANO rotary encoder is the OTHER input device (mutually exclusive with the
  // gamepad — INPUT_DEVICE_TYPE picks exactly one). Same REMOTE_SENSOR_INPUT
  // slot, same 10 Hz cadence + 128 B budget; anoEncoderBuildDataJSON emits
  // {"valid":..,"connected":..,"ts":..,"pos":N,"axis":0|1,"buttons":B}
  // (~90 B typical, still under the 128 B budget). Without this branch an
  // ANO build left the input builder nullptr, so the broadcaster's
  // `if (!spec.builder) continue;` silently dropped every input frame —
  // streaming "turned on" but nothing reached the peer.
  [REMOTE_SENSOR_INPUT]    = { anoEncoderBuildDataJSON,     100,  128  },
#else
  [REMOTE_SENSOR_INPUT]    = { nullptr, 0, 0 },
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

bool initRemoteSensorSystem() {
  constexpr size_t kCacheEntries = MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE;
  const size_t cacheBytes = kCacheEntries * sizeof(RemoteSensorData);
  if (!gRemoteSensorCache) {
    gRemoteSensorCache = (RemoteSensorData*)ps_alloc(
        cacheBytes, AllocPref::PreferPSRAM, "espnow.remoteSensors");
    if (!gRemoteSensorCache) {
      ERROR_SYSTEMF("[REMOTE_SENSORS] Failed to allocate %u-byte cache",
                    (unsigned)cacheBytes);
      return false;
    }
  }

  // Initialize master-side remote-sensor cache (the cache of OTHER devices' data
  // we've received). The worker-side wire cache is gone — sensors are read on
  // demand by the broadcaster, so there's no chalkboard to keep in sync.
  memset(gRemoteSensorCache, 0, cacheBytes);

  if (!gSensorBroadcasterLifecycleMutex ||
      xSemaphoreTake(gSensorBroadcasterLifecycleMutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  if (!gSensorBroadcasterStopDone) {
    gSensorBroadcasterStopDone =
        xSemaphoreCreateBinaryStatic(&gSensorBroadcasterStopDoneStorage);
  }
  if (!gSensorBroadcasterStopDone) {
    xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
    return false;
  }
  gSensorStreamingShutdown.store(false, std::memory_order_release);
  xSemaphoreGive(gSensorBroadcasterLifecycleMutex);

  DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] System initialized (%u bytes)",
         (unsigned)cacheBytes);
  return true;
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
    case REMOTE_SENSOR_INPUT: return "input";
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
  if (strcmp(str, "input") == 0) return REMOTE_SENSOR_INPUT;
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
  if (!gRemoteSensorCache || !deviceMac) return nullptr;
  for (int i = 0; i < MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE; i++) {
    if (gRemoteSensorCache[i].connected &&
        memcmp(gRemoteSensorCache[i].deviceMac, deviceMac, 6) == 0 &&
        gRemoteSensorCache[i].sensorType == sensorType) {
      return &gRemoteSensorCache[i];
    }
  }
  return nullptr;
}

// Find or create cache entry
RemoteSensorData* findOrCreateCacheEntry(const uint8_t* deviceMac, const char* deviceName, RemoteSensorType sensorType) {
  if (!gRemoteSensorCache || !deviceMac || !deviceName) return nullptr;
  // Try to find existing entry
  RemoteSensorData* entry = findCacheEntry(deviceMac, sensorType);
  if (entry) return entry;
  
  // Find empty slot (a slot is free iff !connected)
  for (int i = 0; i < MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE; i++) {
    if (!gRemoteSensorCache[i].connected) {
      memcpy(gRemoteSensorCache[i].deviceMac, deviceMac, 6);
      strncpy(gRemoteSensorCache[i].deviceName, deviceName, 31);
      gRemoteSensorCache[i].deviceName[31] = '\0';
      gRemoteSensorCache[i].sensorType = sensorType;
      gRemoteSensorCache[i].connected = true;   // slot now in use
      gRemoteSensorCache[i].enabled = false;    // until status/data says otherwise
      gRemoteSensorCache[i].valid = false;      // set true when data arrives
      gRemoteSensorCache[i].lastSeen = millis();
      gRemoteSensorCache[i].lastUpdate = 0;
      gRemoteSensorCache[i].jsonLength = 0;
      gRemoteSensorCache[i].jsonData[0] = '\0';
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
  if (!entry) return;
  // SENSOR_STATUS carries the remote's on/off state — the "enabled" axis, kept
  // distinct from `valid` (fresh data) and `connected` (present). The entry stays
  // connected so a disabled sensor shows as a red card instead of vanishing.
  entry->connected = true;
  entry->enabled = enabled;
  entry->lastSeen = millis();
  if (!enabled) entry->valid = false;  // disabled → no live data
  DEBUGF(DEBUG_ESPNOW_CORE, "[REMOTE_SENSORS] Sensor %s %s on %s",
         sensorTypeToString(type), enabled ? "enabled" : "disabled", name);
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
// Callers hold gSensorBroadcasterLifecycleMutex across these transactions.
static bool startSensorBroadcasterLocked();
static bool stopSensorBroadcasterLocked(uint32_t timeoutMs);

void startSensorDataStreaming(RemoteSensorType sensorType) {
  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] startSensorDataStreaming() called with type=%d (%s)", sensorType, sensorTypeToString(sensorType));
  
  if (sensorType >= REMOTE_SENSOR_MAX) {
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] ERROR: Invalid sensor type %d (max=%d)", sensorType, REMOTE_SENSOR_MAX);
    return;
  }
  // Serialize every flag update and the full task create/publish transaction
  // against closeespnow's stop/join/free transaction.
  if (!gSensorBroadcasterLifecycleMutex ||
      xSemaphoreTake(gSensorBroadcasterLifecycleMutex, portMAX_DELAY) != pdTRUE) {
    return;
  }
  if (gSensorStreamingShutdown.load(std::memory_order_acquire)) {
    xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] start rejected during ESP-NOW shutdown");
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
    xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
    return;
  }
#endif
  
  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] Setting streaming flag for %s to TRUE", sensorTypeToString(sensorType));
  
  // Ensure master broadcast flag is enabled so sensor data reaches the cache.
  // We already own the lifecycle mutex; write directly rather than re-entering
  // the public setter.
  if (!gSensorBroadcastEnabled) gSensorBroadcastEnabled = true;
  
  // Publish the flag before task creation so a newly scheduled broadcaster
  // cannot observe an empty set and self-delete in the start-up window.
  gSensorStreamingEnabled[sensorType] = true;
  // Reset lastTxMs before task creation so the first broadcaster lap observes
  // the intended immediate-send state.
  gLastTxMs[sensorType] = 0;

  // Start broadcaster task if not already running
  bool taskPresent;
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  taskPresent = gSensorBroadcasterTask != nullptr;
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);
  if (!taskPresent) {
    if (!startSensorBroadcasterLocked()) {
      gSensorStreamingEnabled[sensorType] = false;
      xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
      BROADCAST_PRINTF("[ESP-NOW] ERROR: Failed to start sensor broadcaster task");
      return;
    }
  }
  xSemaphoreGive(gSensorBroadcasterLifecycleMutex);

  // Tell masters right away that this sensor is now streaming, so their dot/cards
  // flip without waiting for the next 5s presence announce or first data frame.
  broadcastSensorStatus(sensorType, true);

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
  
  if (!gSensorBroadcasterLifecycleMutex ||
      xSemaphoreTake(gSensorBroadcasterLifecycleMutex, portMAX_DELAY) != pdTRUE) {
    return;
  }

#if ENABLE_BONDED_MODE
  // Bond master: send STREAM_CTRL OFF to worker. Lifecycle serialization keeps
  // the local UI flag from racing closeespnow's all-flags clear.
  if (gSettings.bondModeEnabled && isBondMaster()) {
    extern bool sendBondStreamCtrl(RemoteSensorType sensorType, bool enable);
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] Bond master: sending STREAM_CTRL %s OFF to worker", sensorTypeToString(sensorType));
    sendBondStreamCtrl(sensorType, false);
    gSensorStreamingEnabled[sensorType] = false;
    xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
    BROADCAST_PRINTF("[ESP-NOW] Requested worker to stop streaming %s sensor data", sensorTypeToString(sensorType));
    return;
  }
#endif

  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM] Setting streaming flag for %s to FALSE", sensorTypeToString(sensorType));
  gSensorStreamingEnabled[sensorType] = false;
  bool anyEnabled = false;
  for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
    if (gSensorStreamingEnabled[i]) {
      anyEnabled = true;
      break;
    }
  }
  xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
  // Tell masters immediately that this sensor stopped streaming (don't wait for
  // the 5s presence announce) — this is what makes the remote dot go red and stay.
  broadcastSensorStatus(sensorType, false);

  // If this was the last sensor, the broadcaster observes the cleared flags on
  // its next 50 ms lap and self-deletes. Do not synchronously join it here:
  // subscription updates can run on espnow_task, whose RX drain must not block.
  if (!anyEnabled) {
    DEBUGF(DEBUG_ESPNOW_CORE,
           "[SENSOR_BROADCASTER] All sensors disabled; task retirement requested");
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
// Secure sensor fetcher — authorization + subscription apply (worker side)
// ==========================

// L4: a configured fingerprint matches only if it is EXACTLY 64 hex chars and
// case-insensitively equals the live 64-hex pubkey. Empty/malformed → never match.
static bool sensorFingerprintMatches(const char* configured, const char* liveHex64) {
  if (!configured || strlen(configured) != 64) return false;
  return strcasecmp(configured, liveHex64) == 0;
}

bool espnowSensorControlAuthorized(const uint8_t mac[6]) {
  const PeerIdentity* id = peerIdentityFindByMac(mac);
  if (!id) return false;  // not a securely-paired peer
  char liveHex[65];
  espnowIdentityFormatPubHex(id->longTermPub, liveHex, sizeof(liveHex));
  if (sensorFingerprintMatches(gSettings.espnowMasterFingerprint.c_str(), liveHex))       return true;
  if (sensorFingerprintMatches(gSettings.espnowBackupMasterFingerprint.c_str(), liveHex)) return true;
  return false;
}

void espnowApplySensorSubscription(uint8_t mode, uint32_t sensorMask,
                                   uint32_t intervalMs, uint32_t leaseMs,
                                   const uint8_t* controllerMac) {
  if (mode > SENSOR_REQ_ONESHOT) {  // reject unknown/out-of-range modes (3..255)
    DEBUG_ESPNOW_STREAMF("[SENSOR_SUB] ignoring unknown mode %u", mode);
    return;
  }
  // The deferred apply still runs on espnow_task. Never block that RX drainer
  // behind closeespnow's task join: shutdown publishes its atomic gate before
  // taking this mutex, and an ordinary short-lived creator conflict is retried
  // on the next super-loop lap using the already-staged request fields.
  if (gSensorStreamingShutdown.load(std::memory_order_acquire)) return;
  if (!gSensorBroadcasterLifecycleMutex ||
      xSemaphoreTake(gSensorBroadcasterLifecycleMutex, 0) != pdTRUE) {
    if (!gSensorStreamingShutdown.load(std::memory_order_acquire) && gEspNow) {
      gEspNow->meshSensorReqPending = true;
    }
    return;
  }
  if (gSensorStreamingShutdown.load(std::memory_order_acquire)) {
    xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
    return;
  }

  // Subscribe/oneshot record the controller as the encrypted-reply target.
  if ((mode == SENSOR_REQ_SUBSCRIBE || mode == SENSOR_REQ_ONESHOT) && controllerMac) {
    memcpy(gSensorControllerMac, controllerMac, 6);
  }
  uint32_t statusMask = 0;
  uint32_t enabledMask = 0;
  for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
    if (!(sensorMask & (1u << i))) continue;
    statusMask |= 1u << i;
    if (mode == SENSOR_REQ_UNSUBSCRIBE) {
      gSensorLeaseExpiresAt[i] = 0;
      gSensorReqIntervalMs[i]  = 0;
      gSensorStreamingEnabled[i] = false;
    } else {
      // Subscribe or oneshot: publish every task-observed field before creating
      // the broadcaster, then stamp the lease.
      gSensorBroadcastEnabled = true;
      gSensorStreamingEnabled[i] = true;
      gLastTxMs[i] = 0;
      enabledMask |= 1u << i;
      gSensorReqIntervalMs[i] = intervalMs;
      uint32_t leaseFor = (mode == SENSOR_REQ_ONESHOT)
                            ? (uint32_t)(intervalMs ? intervalMs : 500)  // ~one push then expire
                            : (uint32_t)leaseMs;
      uint32_t exp = (uint32_t)millis() + leaseFor;
      if (exp == 0) exp = 1;  // 0 is reserved for "manual / no lease"
      gSensorLeaseExpiresAt[i] = exp;
    }
  }

  bool taskReady = true;
  if (enabledMask) {
    bool taskPresent;
    taskENTER_CRITICAL(&gSensorBcastTaskMux);
    taskPresent = gSensorBroadcasterTask != nullptr;
    taskEXIT_CRITICAL(&gSensorBcastTaskMux);
    taskReady = taskPresent || startSensorBroadcasterLocked();
    if (!taskReady) {
      for (int i = 0; i < REMOTE_SENSOR_MAX; ++i) {
        if (!(enabledMask & (1u << i))) continue;
        gSensorStreamingEnabled[i] = false;
        gSensorLeaseExpiresAt[i] = 0;
        gSensorReqIntervalMs[i] = 0;
      }
    }
  }
  xSemaphoreGive(gSensorBroadcasterLifecycleMutex);

  // Radio notifications happen after releasing lifecycle state. A concurrent
  // close may reject them through the runtime gate; the local state is already
  // coherent either way.
  for (int i = 0; i < REMOTE_SENSOR_MAX; ++i) {
    if (statusMask & (1u << i)) {
      broadcastSensorStatus((RemoteSensorType)i,
                            taskReady && (mode != SENSOR_REQ_UNSUBSCRIBE));
    }
  }
  DEBUG_ESPNOW_STREAMF("[SENSOR_SUB] applied mode=%u mask=0x%lx interval=%u lease=%u",
                       mode, (unsigned long)sensorMask, intervalMs, leaseMs);
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
  
  
  // Send via V4 binary protocol (both bond and mesh modes).
  // D2: mesh streaming is now an encrypted UNICAST (SENSOR_ENVELOPE) to the leasing
  // controller — the plaintext broadcast path (v4_broadcast_sensor_data) is retired.
  extern bool v4_send_sensor_envelope(const uint8_t* dstMac, RemoteSensorType sensorType,
                                      const char* jsonData, uint16_t jsonLen);

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
  
  // D2: encrypted UNICAST to the leasing controller — no plaintext broadcast. With no
  // controller set (no active/prior lease), a manual local toggle is a no-op on the air.
  bool haveController = false;
  for (int b = 0; b < 6; b++) { if (gSensorControllerMac[b] != 0) { haveController = true; break; } }
  if (!haveController) {
    DEBUG_ESPNOW_STREAMF("%s", "[SENSOR_DATA_TX] SKIP: no leasing controller — nothing on-air (D2)");
    return;
  }
  DEBUG_ESPNOW_STREAMF("%s", "[SENSOR_DATA_TX] Sending encrypted SENSOR_ENVELOPE to controller");
  bool sent = v4_send_sensor_envelope(gSensorControllerMac, sensorType, jsonData, jsonLen);
  if (sent) {
    DEBUG_ESPNOW_STREAMF("[SENSOR_TX] SUCCESS: encrypted %s reading to controller", sensorTypeToString(sensorType));
  } else {
    // No live session to the controller (or send failed). Encrypt-or-fail — never a
    // plaintext fallback (D2). The master's next lease renewal re-warms the session.
    DEBUG_ESPNOW_STREAMF("[SENSOR_TX] %s reading not sent (no session to controller?)", sensorTypeToString(sensorType));
  }
}

// Worker → master: announce every locally-CONNECTED sensor's on/off state — not
// just the streaming ones — so masters can list present-but-disabled remote
// sensors as red cards, the same way the local sensors page shows a connected-
// but-disabled sensor. Reuses the existing SENSOR_STATUS opcode;
// broadcastSensorStatus() self-gates on worker + mesh + broadcast-enabled.
// The connected/enabled globals resolve to real driver state or a `false` stub
// (System_SensorStubs.cpp), so referencing them unconditionally is link-safe.
static void announceConnectedSensors() {
  extern bool gThermalConnected;
  extern bool gTofConnected;
  extern bool gImuConnected;
  extern bool gGpsConnected;
  extern bool gInputConnected;
  extern bool gFmRadioConnected;
  extern bool gRtcConnected;
  extern bool gPresenceConnected;
  struct Item { bool connected; RemoteSensorType type; };
  const Item items[] = {
    { gThermalConnected,  REMOTE_SENSOR_THERMAL },
    { gTofConnected,      REMOTE_SENSOR_TOF },
    { gImuConnected,      REMOTE_SENSOR_IMU },
    { gGpsConnected,      REMOTE_SENSOR_GPS },
    { gInputConnected,    REMOTE_SENSOR_INPUT },
    { gFmRadioConnected,  REMOTE_SENSOR_FMRADIO },
    { gRtcConnected,      REMOTE_SENSOR_RTC },
    { gPresenceConnected, REMOTE_SENSOR_PRESENCE },
  };
  for (const auto& it : items) {
    // The status we announce is the STREAMING state, not the sensor-on state.
    // For a REMOTE sensor, "is it streaming to me" is the only axis the master
    // can observe (it only ever sees streamed data) and the one the Sensor
    // Streaming UI toggles. Announcing gXxxEnabled (sensor-on) was wrong: an
    // always-on sensor like the gamepad reported enabled=true forever, so after
    // the user stopped streaming the master's dot bounced back to green.
    if (it.connected) broadcastSensorStatus(it.type, isSensorDataStreamingEnabled(it.type));
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
    if (gSensorBroadcasterStopRequested.load(std::memory_order_acquire) ||
        gSensorStreamingShutdown.load(std::memory_order_acquire)) {
      break;
    }
    const unsigned long now = millis();

    // Stack HWM diagnostic. uxTaskGetStackHighWaterMark returns the *free* stack
    // ever observed (low-water mark), not used — peak_used = total - free.
    //
    // UNITS: BYTES, not words, despite the "words" in the log line below and in
    // upstream FreeRTOS's own docs. This port sets portSTACK_TYPE = uint8_t, so
    // StackType_t is 1 byte and the count comes back unscaled. SENSOR_BCAST_STACK_WORDS
    // is bytes too (see System_TaskUtils.h), so the subtraction is right — but read
    // any number here as bytes or you'll credit this task with 4x the headroom it
    // has. 4 KB total, not 16 KB.
    //
    // Logged every 5s under DEBUG_ESPNOW_CORE so we can size the stack to
    // measured peak + margin instead of guessing.
    static unsigned long lastHwmLog = 0;
    if (now - lastHwmLog >= 5000) {
      lastHwmLog = now;
      UBaseType_t hwmFreeWords = uxTaskGetStackHighWaterMark(nullptr);
      DEBUGF(DEBUG_ESPNOW_CORE,
             "[SENSOR_BCAST] stack free=%u words peak_used=%u of %u",
             (unsigned)hwmFreeWords,
             (unsigned)(SENSOR_BCAST_STACK_WORDS - hwmFreeWords),
             (unsigned)SENSOR_BCAST_STACK_WORDS);
    }

    // Presence heartbeat: announce connected sensors' on/off state every ~5s so
    // masters keep present-but-disabled remote sensors as red cards (mirrors the
    // local model). Cheap — at most a handful of small SENSOR_STATUS frames.
    static unsigned long lastPresenceAnnounce = 0;
    if (now - lastPresenceAnnounce >= 5000) {
      lastPresenceAnnounce = now;
      announceConnectedSensors();
    }

    for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
      if (!gSensorStreamingEnabled[i]) continue;

      // Lease expiry (secure fetcher, H3): if this sensor's lease has lapsed, clear
      // ONLY the flags here — never call stopSensorDataStreaming/stopSensorBroadcaster
      // from inside this task (that self-vTaskDeletes our own handle and wedges the
      // worker until reboot). The self-teardown after the loop handles task exit.
      if (gSensorLeaseExpiresAt[i] != 0 && (uint32_t)now >= gSensorLeaseExpiresAt[i]) {
        gSensorLeaseExpiresAt[i]   = 0;
        gSensorReqIntervalMs[i]    = 0;
        gSensorStreamingEnabled[i] = false;
        broadcastSensorStatus((RemoteSensorType)i, false);
        continue;
      }

      const SensorBroadcastSpec& spec = gSensorSpecs[i];
      if (!spec.builder) continue;  // sensor not compiled in

      // Honor BOTH the per-sensor minimum and the applicable cadence — the leasing
      // controller's requested interval if set, else the user-tunable global — taking
      // whichever is slower so we throttle but never exceed a sensor's native cadence.
      unsigned long interval = spec.minIntervalMs;
      unsigned long requested = gSensorReqIntervalMs[i] ? gSensorReqIntervalMs[i]
                                                        : gSettings.sensorBroadcastIntervalMs;
      if (requested > interval) interval = requested;
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

    if (gSensorBroadcasterStopRequested.load(std::memory_order_acquire) ||
        gSensorStreamingShutdown.load(std::memory_order_acquire)) {
      break;
    }

    // H3: if every sensor has stopped (e.g. all leases lapsed above), tear the task
    // down cleanly. The lifecycle mux decides whether this is an unowned natural
    // exit (self-delete) or an explicit stop whose owner will join/delete us.
    bool anyStillEnabled = false;
    for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
      if (gSensorStreamingEnabled[i]) { anyStillEnabled = true; break; }
    }
    if (!anyStillEnabled) {
      // Serialize the final recheck/detach with a concurrent start. Use a
      // zero-timeout take: closeespnow holds this mutex while it requests and
      // joins an explicit stop, so blocking here would deadlock that join.
      if (!gSensorBroadcasterLifecycleMutex ||
          xSemaphoreTake(gSensorBroadcasterLifecycleMutex, 0) != pdTRUE) {
        vTaskDelay(1);
        continue;
      }
      anyStillEnabled = false;
      for (int i = 0; i < REMOTE_SENSOR_MAX; ++i) {
        if (gSensorStreamingEnabled[i]) {
          anyStillEnabled = true;
          break;
        }
      }
      if (anyStillEnabled ||
          gSensorBroadcasterStopRequested.load(std::memory_order_acquire) ||
          gSensorStreamingShutdown.load(std::memory_order_acquire)) {
        xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
        continue;
      }
      TaskHandle_t self = xTaskGetCurrentTaskHandle();
      taskENTER_CRITICAL(&gSensorBcastTaskMux);
      const bool externallyOwned = gSensorBroadcasterExternalStop;
      if (!externallyOwned && gSensorBroadcasterTask == self) {
        gSensorBroadcasterTask = nullptr;
      }
      taskEXIT_CRITICAL(&gSensorBcastTaskMux);
      xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
      if (!externallyOwned) {
        DEBUGF(DEBUG_ESPNOW_CORE, "[SENSOR_BCAST_TASK] all sensors stopped — self-teardown");
        vTaskDelete(nullptr);  // natural exit; no external join owner
      }
      break;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }

  // Explicit cooperative stop. All builders/sends and their guards have
  // returned. Publish the join point and park; the lifecycle owner retains the
  // handle and performs the actual delete before any shared buffer is freed.
  xSemaphoreGive(gSensorBroadcasterStopDone);
  vTaskSuspend(nullptr);
}

// Start the broadcaster task
static bool startSensorBroadcasterLocked() {
  if (gSensorStreamingShutdown.load(std::memory_order_acquire)) return false;

  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  if (gSensorBroadcasterTask) {
    const bool running = !gSensorBroadcasterExternalStop;
    taskEXIT_CRITICAL(&gSensorBcastTaskMux);
    return running;
  }
  if (gSensorBroadcasterExternalStop) {
    taskEXIT_CRITICAL(&gSensorBcastTaskMux);
    return false;
  }
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);

  if (!gSensorBroadcasterStopDone) {
    gSensorBroadcasterStopDone =
        xSemaphoreCreateBinaryStatic(&gSensorBroadcasterStopDoneStorage);
  }
  if (!gSensorBroadcasterStopDone) return false;
  (void)xSemaphoreTake(gSensorBroadcasterStopDone, 0);
  gSensorBroadcasterStopRequested.store(false, std::memory_order_release);

  // Allocate the shared broadcaster buffer in PSRAM, sized to the largest
  // sensor's bufBytes. One allocation, reused forever — no per-tick churn.
  if (!gBroadcasterBuf) {
    size_t maxBuf = 0;
    for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
      if (gSensorSpecs[i].bufBytes > maxBuf) maxBuf = gSensorSpecs[i].bufBytes;
    }
    if (maxBuf == 0) maxBuf = 256;  // defensive; should not happen if any sensor is enabled
    gBroadcasterBuf = (char*)ps_alloc(maxBuf, AllocPref::RequirePSRAM,
                                      "espnow.sensor.broadcast");
    if (!gBroadcasterBuf) {
      DEBUGF(DEBUG_ESPNOW_CORE, "[SENSOR_BROADCASTER] Failed to alloc %u-byte PSRAM buffer", (unsigned)maxBuf);
      return false;
    }
    gBroadcasterBufSize = maxBuf;
  }

  TaskHandle_t createdTask = nullptr;
  taskStackRecord("sensor_bcast", SENSOR_BCAST_STACK_WORDS);
  BaseType_t ret = xTaskCreatePinnedToCore(
    sensorBroadcasterTask,
    "sensor_bcast",
    SENSOR_BCAST_STACK_WORDS,
    nullptr,
    TASK_PRIORITY_HIGH,
    &createdTask,
    1      // Core 1 (opposite of ESP-NOW callback which is core 0)
  );

  if (ret == pdPASS) {
    taskENTER_CRITICAL(&gSensorBcastTaskMux);
    gSensorBroadcasterTask = createdTask;
    taskEXIT_CRITICAL(&gSensorBcastTaskMux);
    DEBUGF(DEBUG_ESPNOW_CORE, "[SENSOR_BROADCASTER] Task started");
    return true;
  } else {
    taskENTER_CRITICAL(&gSensorBcastTaskMux);
    gSensorBroadcasterTask = nullptr;
    taskEXIT_CRITICAL(&gSensorBcastTaskMux);
    gSensorBroadcasterStopRequested.store(true, std::memory_order_release);
    DEBUGF(DEBUG_ESPNOW_CORE, "[SENSOR_BROADCASTER] Failed to create task");
    return false;
  }
}

// Stop the broadcaster task
static bool stopSensorBroadcasterLocked(uint32_t timeoutMs) {
  TaskHandle_t task = nullptr;
  SemaphoreHandle_t stopDone = nullptr;
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  task = gSensorBroadcasterTask;
  stopDone = gSensorBroadcasterStopDone;
  if (!task) {
    taskEXIT_CRITICAL(&gSensorBcastTaskMux);
    return true;
  }
  if (task == xTaskGetCurrentTaskHandle()) {
    taskEXIT_CRITICAL(&gSensorBcastTaskMux);
    DEBUGF(DEBUG_ESPNOW_CORE, "[SENSOR_BROADCASTER] stop rejected from broadcaster task");
    return false;
  }
  // A retrying close may adopt the same explicit stop and wait for the task's
  // already-published join token. The lifecycle mutex permits only one owner.
  gSensorBroadcasterExternalStop = true;
  gSensorBroadcasterStopRequested.store(true, std::memory_order_release);
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);

  (void)xTaskAbortDelay(task);
  if (!stopDone || xSemaphoreTake(stopDone, pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
    DEBUGF(DEBUG_ESPNOW_CORE,
           "[SENSOR_BROADCASTER] stop TIMEOUT after %ums; resources retained for retry",
           (unsigned)timeoutMs);
    return false;
  }

  vTaskDelete(task);
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  if (gSensorBroadcasterTask == task) gSensorBroadcasterTask = nullptr;
  gSensorBroadcasterExternalStop = false;
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);
  DEBUGF(DEBUG_ESPNOW_CORE, "[SENSOR_BROADCASTER] Task stopped");
  return true;
}

bool shutdownSensorDataStreamingForEspNowClose() {
  // Close admission first, then clear every local/bond-master UI and lease bit
  // in one short critical section. Do not call stopSensorDataStreaming(): it
  // emits one radio status frame per sensor (and a bond master emits STREAM_CTRL).
  gSensorStreamingShutdown.store(true, std::memory_order_release);
  if (!gSensorBroadcasterLifecycleMutex ||
      xSemaphoreTake(gSensorBroadcasterLifecycleMutex, portMAX_DELAY) != pdTRUE) {
    return false;
  }
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  gSensorBroadcastEnabled = false;
  memset(gSensorStreamingEnabled, 0, sizeof(gSensorStreamingEnabled));
  memset(gSensorLeaseExpiresAt, 0, sizeof(gSensorLeaseExpiresAt));
  memset(gSensorReqIntervalMs, 0, sizeof(gSensorReqIntervalMs));
  memset(gSensorControllerMac, 0, sizeof(gSensorControllerMac));
  memset(gLastTxMs, 0, sizeof(gLastTxMs));
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);

  if (!stopSensorBroadcasterLocked(2000)) {
    xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
    return false;
  }

  // The task has reached its explicit join and has been deleted; no builder can
  // still be using the shared buffer. Detach before free so a future init/start
  // allocates a fresh correctly-sized buffer.
  taskENTER_CRITICAL(&gSensorBcastTaskMux);
  char* buffer = gBroadcasterBuf;
  gBroadcasterBuf = nullptr;
  gBroadcasterBufSize = 0;
  taskEXIT_CRITICAL(&gSensorBcastTaskMux);
  if (buffer) free(buffer);
  xSemaphoreGive(gSensorBroadcasterLifecycleMutex);
  return true;
}

String getRemoteSensorDataJSON(const uint8_t* deviceMac, RemoteSensorType sensorType) {
  RemoteSensorData* entry = findCacheEntry(deviceMac, sensorType);
  if (!entry) {
    // Unknown to us = not connected. Mirrors a local sensor that isn't present.
    return "{\"connected\":false,\"enabled\":false,\"fresh\":false,\"data\":null}";
  }
  // Freshness is independent of enabled/connected: a sensor can be connected +
  // enabled but momentarily stale, or connected + disabled (red dot, no data).
  unsigned long now = millis();
  bool fresh = entry->valid && (now - entry->lastUpdate <= REMOTE_SENSOR_TTL_MS);
  if (!fresh) entry->valid = false;
  String out;
  out.reserve(entry->jsonLength + 96);
  out += "{\"connected\":"; out += entry->connected ? "true" : "false";
  out += ",\"enabled\":";   out += entry->enabled ? "true" : "false";
  out += ",\"fresh\":";     out += fresh ? "true" : "false";
  out += ",\"data\":";
  if (fresh && entry->jsonLength > 0) out += entry->jsonData;  // jsonData is a valid JSON value
  else out += "null";
  out += "}";
  return out;
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
    // Skip bookkeeping/noise keys that aren't useful on a small screen. This
    // includes the shared sensor-reading envelope's metadata (valid/connected/
    // ts/age) — the value keys are what matter on a tiny remote readout.
    if (!strcmp(key, "ts") || !strcmp(key, "seq") || !strcmp(key, "val") ||
        !strcmp(key, "valid") || !strcmp(key, "connected") || !strcmp(key, "age"))
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
  if (!gRemoteSensorCache) {
    String result;
    serializeJson(doc, result);
    return result;
  }
  
  // List every CONNECTED (present) remote sensor — including disabled ones, so the
  // web shows them as red cards instead of dropping them. Each sensor carries its
  // own enabled/fresh state so the dot mirrors the local model.
  unsigned long now = millis();
  for (int i = 0; i < MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE; i++) {
    RemoteSensorData& e = gRemoteSensorCache[i];
    if (!e.connected) continue;

    // Presence aging: a sensor we haven't heard from (status OR data) in a long
    // time means the device went away — free the slot so its card disappears.
    if (now - e.lastSeen > REMOTE_SENSOR_PRESENCE_TTL_MS) {
      e.connected = false;
      continue;
    }

    bool fresh = e.valid && (now - e.lastUpdate <= REMOTE_SENSOR_TTL_MS);

    // Find or create device entry
    String macStr = macToHexString(e.deviceMac);
    JsonObject deviceObj;
    bool found = false;
    for (JsonObject dev : devices) {
      if (strcmp(dev["mac"], macStr.c_str()) == 0) { deviceObj = dev; found = true; break; }
    }
    if (!found) {
      deviceObj = devices.add<JsonObject>();
      deviceObj["mac"] = macStr;
      deviceObj["name"] = e.deviceName;
      deviceObj["sensors"].to<JsonArray>();
    }

    // Add sensor as an object {type, enabled, fresh} (was a bare type string).
    JsonArray sensors = deviceObj["sensors"];
    JsonObject s = sensors.add<JsonObject>();
    s["type"] = sensorTypeToString(e.sensorType);
    s["enabled"] = e.enabled;
    s["fresh"] = fresh;
  }
  
  String result;
  serializeJson(doc, result);
  return result;
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

  // D1: sensor-stream control is LOCAL-CONSOLE-ONLY. The only remote path to control a
  // worker's streaming is the fingerprint-gated SENSOR_REQ opcode — the generic
  // credentialed remote-exec route (espnowremote … espnowsensorstream …) must not be
  // able to drive a worker's sensors. See docs/ESPNOW_SENSOR_FETCHER_DESIGN.md.
  if (currentAuthContext().transport == SOURCE_ESPNOW) {
    return "Error: espnowsensorstream is local-console-only. Remote sensor control is via "
           "the secure fetcher (SENSOR_REQ), not remote command execution.";
  }

  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM_CMD] Command received: '%s'", argsInput.c_str());
  
  // Parse: <sensor> <on|off>  (dispatcher strips "espnow sensorstream" prefix)
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(2)) {
    DEBUG_ESPNOW_STREAMF("%s", "[SENSOR_STREAM_CMD] ERROR: Missing sensor name");
    return "Error: invalid arguments — Usage: espnow sensorstream <sensor> <on|off>";
  }

  String sensorName = a.arg(0);
  normalizeCliArg(sensorName);

  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM_CMD] Parsed: sensor='%s' action='%s'", sensorName.c_str(), a.arg(1).c_str());

  // Convert sensor name to type
  RemoteSensorType sensorType = stringToSensorType(sensorName.c_str());
  if (strcmp(sensorTypeToString(sensorType), sensorName.c_str()) != 0) {
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM_CMD] ERROR: Unknown sensor '%s'", sensorName.c_str());
    return "Error: invalid arguments — Usage: espnow sensorstream <sensor> <on|off>";
  }
  DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM_CMD] Sensor type resolved: %d (%s)", sensorType, sensorTypeToString(sensorType));

  // Parse action
  int boolResult = parseBoolArg(a.arg(1));
  if (boolResult < 0) {
    DEBUG_ESPNOW_STREAMF("[SENSOR_STREAM_CMD] ERROR: Invalid action '%s'", a.arg(1).c_str());
    return "Error: invalid arguments — Usage: espnow sensorstream <sensor> <on|off>";
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

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"]    = 1;
    doc["broadcast"] = isSensorBroadcastEnabled();
    bool isMaster = (gSettings.meshRole == MESH_ROLE_MASTER);
    doc["role"] = isMaster ? "master" : "worker";
    if (isMaster) {
      // Master: nest the remote sensor cache ({"devices":[...]}) verbatim.
      PSRAM_JSON_DOC(tmp);
      if (deserializeJson(tmp, getRemoteDevicesListJSON()) == DeserializationError::Ok &&
          tmp["devices"].is<JsonArray>()) {
        doc["devices"] = tmp["devices"];
      } else {
        doc["devices"].to<JsonArray>();
      }
    } else {
      // Worker: which sensors are currently streaming.
      JsonObject streaming = doc["streaming"].to<JsonObject>();
      const char* sensors[] = {"thermal","tof","imu","gps","input","fmradio","camera","microphone"};
      for (int i = 0; i < 8; i++) {
        streaming[sensors[i]] = isSensorDataStreamingEnabled(stringToSensorType(sensors[i]));
      }
    }
    EXT_RAM_BSS_ATTR static char jbuf[4096];
    serializeJson(doc, jbuf, sizeof(jbuf));
    return jbuf;
  }

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
    
    const char* sensors[] = {"thermal", "tof", "imu", "gps", "input", "fmradio", "camera", "microphone"};
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
    return "Error: invalid arguments — Usage: espnow sensorbroadcast <on|off>";
  }
}

// ==========================
// Remote GPS Data Access
// ==========================

#include <ArduinoJson.h>


bool getRemoteGPSData(RemoteGPSData* outData) {
  if (!outData) return false;
  
  memset(outData, 0, sizeof(RemoteGPSData));
  outData->valid = false;
  if (!gRemoteSensorCache) return false;
  
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
  
  // Parse the JSON data: {"valid":true,"connected":true,"ts":..,"fix":1,"quality":1,"sats":8,"lat":37.123,"lon":-122.456,"alt":100.5,"speed":0.5}
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
