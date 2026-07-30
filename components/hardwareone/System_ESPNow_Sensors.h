#ifndef SYSTEM_ESPNOW_SENSORS_H
#define SYSTEM_ESPNOW_SENSORS_H

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <Arduino.h>
#include <stdint.h>

// ==========================
// Remote Sensor Data Structures
// ==========================

// Sensor types that can be streamed over ESP-NOW
enum RemoteSensorType {
  REMOTE_SENSOR_THERMAL = 0,
  REMOTE_SENSOR_TOF = 1,
  REMOTE_SENSOR_IMU = 2,
  REMOTE_SENSOR_GPS = 3,
  REMOTE_SENSOR_INPUT = 4,
  REMOTE_SENSOR_FMRADIO = 5,
  REMOTE_SENSOR_CAMERA = 6,
  REMOTE_SENSOR_MICROPHONE = 7,
  REMOTE_SENSOR_RTC = 8,
  REMOTE_SENSOR_PRESENCE = 9,
  REMOTE_SENSOR_APDS = 10,
  REMOTE_SENSOR_MAX
};

// Remote sensor status (broadcast when sensor starts/stops)
struct RemoteSensorStatus {
  uint8_t deviceMac[6];
  char deviceName[32];
  RemoteSensorType sensorType;
  bool enabled;
  unsigned long timestamp;
};

// Remote sensor data cache entry
// Uses fixed buffer to avoid heap churn from String allocations
#define REMOTE_SENSOR_BUFFER_SIZE 256  // RX cap — never the binding limit: the 200 B TX gate
                                       // (v4_send_sensor_envelope) rejects first, and the RX
                                       // memcpy truncates safely. (Thermal handled separately.)

struct RemoteSensorData {
  uint8_t deviceMac[6];
  char deviceName[32];
  RemoteSensorType sensorType;
  char jsonData[REMOTE_SENSOR_BUFFER_SIZE];  // Fixed buffer, no heap allocation
  uint16_t jsonLength;                        // Actual data length in buffer
  unsigned long lastUpdate;                   // millis() of last DATA arrival (drives `valid`/fresh)
  unsigned long lastSeen;                     // millis() of last STATUS or DATA (drives presence aging)
  // Three orthogonal states mirroring the LOCAL sensor model
  // (compiled/connected/enabled). `connected` doubles as the slot-occupancy
  // marker — a slot is free iff !connected. This fixes the prior overload of
  // `valid` (which was simultaneously "fresh data", "enabled", AND "slot free",
  // so a disabled sensor's slot could be silently reclaimed).
  bool connected;                             // sensor present on the remote (slot in use)
  bool enabled;                               // sensor currently running on the remote
  bool valid;                                 // fresh data available (within REMOTE_SENSOR_TTL_MS)
};

// Maximum remote devices to track
#define MAX_REMOTE_DEVICES 8
#define MAX_SENSORS_PER_DEVICE 8
#define REMOTE_SENSOR_TTL_MS 30000  // data-freshness TTL (drives `valid`/fresh)
#define REMOTE_SENSOR_PRESENCE_TTL_MS 60000  // drop a present-but-silent sensor (device gone) after 60s

// Total cache size: 8 devices * 8 sensors * ~310 bytes = ~20KB.
// Allocated in initRemoteSensorSystem(), so builds with ESP-NOW compiled but
// never started do not reserve the table in PSRAM .bss.

// Remote sensor data cache (master only)
extern RemoteSensorData* gRemoteSensorCache;

// ==========================
// Remote Sensor Functions
// ==========================

// Initialize remote sensor system (master only). Idempotently allocates the
// cache on first ESP-NOW start and clears it on each subsequent start.
bool initRemoteSensorSystem();

// Broadcast sensor status change (worker → master)
void broadcastSensorStatus(RemoteSensorType sensorType, bool enabled);

// Start streaming sensor data (worker → master, opt-in)
void startSensorDataStreaming(RemoteSensorType sensorType);

// Stop streaming sensor data
void stopSensorDataStreaming(RemoteSensorType sensorType);

bool isSensorDataStreamingEnabled(RemoteSensorType sensorType);

// ---- Secure sensor fetcher (docs/ESPNOW_SENSOR_FETCHER_DESIGN.md) ----
// Sensor-request modes carried in V4PayloadSensorReq.mode.
enum SensorReqMode : uint8_t {
  SENSOR_REQ_SUBSCRIBE   = 0,  // start streaming {mask} under a lease
  SENSOR_REQ_UNSUBSCRIBE = 1,  // stop streaming {mask}
  SENSOR_REQ_ONESHOT     = 2,  // one immediate push per {mask} bit, short self-expiring lease
};

// True iff `mac` is a securely-paired peer whose Ed25519 fingerprint matches this
// worker's configured master OR backup-master fingerprint. This is the SOLE on-wire
// authorization for sensor control. Empty/malformed configured fingerprints never match.
bool espnowSensorControlAuthorized(const uint8_t mac[6]);

// Apply a (previously authorized) SENSOR_REQ off the RX critical path: start/stop
// streaming per mask, stamp the lease, and record the controller MAC as the
// encrypted-unicast reply target. Called from the espnow_task super-loop (block 9f3).
void espnowApplySensorSubscription(uint8_t mode, uint32_t sensorMask,
                                   uint32_t intervalMs, uint32_t leaseMs,
                                   const uint8_t* controllerMac);

// Get remote sensor data for web API
String getRemoteSensorDataJSON(const uint8_t* deviceMac, RemoteSensorType sensorType);

// Format-agnostic readable renderer for a cached sensor's JSON. Walks the
// top-level key:value pairs and writes up to `maxLines` newline-separated
// "key: value" lines into `out` (skipping bookkeeping keys like ts/seq/valid).
// Shared by the OLED Remote-Sensors page (and later the G2 lens) so neither
// hard-codes a per-sensor schema — new sensors / key changes render automatically.
// Returns the number of lines written (>=1; writes a short placeholder on error).
int formatRemoteSensorReadable(const char* json, char* out, size_t outSize, int maxLines);

// Get list of all remote devices with sensors
String getRemoteDevicesListJSON();

// Helper: Convert sensor type to string
const char* sensorTypeToString(RemoteSensorType type);

// Helper: Convert string to sensor type
RemoteSensorType stringToSensorType(const char* str);

// Find or create cache entry for a remote sensor (used by v3 handler)
RemoteSensorData* findOrCreateCacheEntry(const uint8_t* deviceMac, const char* deviceName, RemoteSensorType sensorType);

// ==========================
// Sensor Broadcast Control
// ==========================

// Enable/disable all sensor ESP-NOW communication (status + data)
void setSensorBroadcastEnabled(bool enabled);
bool isSensorBroadcastEnabled();

// True only when the ESP-NOW radio has been initialized at runtime (i.e. the
// user clicked Initialize). ENABLE_ESPNOW merely means the feature is compiled
// in. Lets the web sensors page tell "not initialized" apart from "active".
bool isEspNowInitialized();

// ==========================
// Thermal Data Optimization
// ==========================

// Build thermal data JSON with integer values (no decimals) for remote streaming
// Returns length of JSON string written to buf
int buildThermalDataJSONInteger(char* buf, size_t bufSize);

// ==========================
// Remote GPS Data Access
// ==========================

// Structure for accessing remote GPS data
struct RemoteGPSData {
  bool valid;           // Data is valid and not expired
  bool hasFix;          // GPS has valid fix
  int fixQuality;       // Fix quality (0=invalid, 1=GPS, 2=DGPS)
  int satellites;       // Number of satellites
  float latitude;       // Latitude in degrees (negative = South)
  float longitude;      // Longitude in degrees (negative = West)
  float altitude;       // Altitude in meters
  float speed;          // Speed in knots
  unsigned long lastUpdate; // When data was last received
  char deviceName[32];  // Name of device providing GPS
};

// Get remote GPS data from bonded device or mesh workers
// Returns true if valid GPS data is available from a remote source
bool getRemoteGPSData(RemoteGPSData* outData);

#endif // ENABLE_ESPNOW
#endif // SYSTEM_ESPNOW_SENSORS_H
