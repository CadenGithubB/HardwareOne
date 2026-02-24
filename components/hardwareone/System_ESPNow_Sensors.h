#ifndef ESPNOW_SENSORS_H
#define ESPNOW_SENSORS_H

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
  REMOTE_SENSOR_GAMEPAD = 4,
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
#define REMOTE_SENSOR_BUFFER_SIZE 256  // Enough for gamepad/IMU/GPS/ToF/FM (thermal handled separately)

struct RemoteSensorData {
  uint8_t deviceMac[6];
  char deviceName[32];
  RemoteSensorType sensorType;
  char jsonData[REMOTE_SENSOR_BUFFER_SIZE];  // Fixed buffer, no heap allocation
  uint16_t jsonLength;                        // Actual data length in buffer
  unsigned long lastUpdate;                   // millis() when last updated
  bool valid;                                 // Data is valid and not expired
};

// Maximum remote devices to track
#define MAX_REMOTE_DEVICES 8
#define MAX_SENSORS_PER_DEVICE 8
#define REMOTE_SENSOR_TTL_MS 30000  // 30 seconds TTL

// Total cache size: 8 devices * 8 sensors * ~310 bytes = ~20KB (fixed, no heap growth)

// Remote sensor data cache (master only)
extern RemoteSensorData gRemoteSensorCache[MAX_REMOTE_DEVICES * MAX_SENSORS_PER_DEVICE];

// ==========================
// ESP-NOW Sensor Message Types
// ==========================

#define MSG_TYPE_SENSOR_STATUS "SENSOR_STATUS"
#define MSG_TYPE_SENSOR_DATA "SENSOR_DATA"

// ==========================
// Remote Sensor Functions
// ==========================

// Initialize remote sensor system (master only)
void initRemoteSensorSystem();

// Handle incoming sensor status message
void handleSensorStatusMessage(const uint8_t* senderMac, const String& deviceName, const String& message);

// Handle incoming sensor data message
void handleSensorDataMessage(const uint8_t* senderMac, const String& deviceName, const String& message);

// Broadcast sensor status change (worker → master)
void broadcastSensorStatus(RemoteSensorType sensorType, bool enabled);

// Start streaming sensor data (worker → master, opt-in)
void startSensorDataStreaming(RemoteSensorType sensorType);

// Stop streaming sensor data
void stopSensorDataStreaming(RemoteSensorType sensorType);

bool isSensorDataStreamingEnabled(RemoteSensorType sensorType);

// Send sensor data update (worker → master)
void sendSensorDataUpdate(RemoteSensorType sensorType, const String& jsonData);

// Get remote sensor data for web API
String getRemoteSensorDataJSON(const uint8_t* deviceMac, RemoteSensorType sensorType);

// Get list of all remote devices with sensors
String getRemoteDevicesListJSON();

// Clean up expired remote sensor data
void cleanupExpiredRemoteSensorData();

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
void espnowSensorStatusPeriodicTick();

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

// Get remote GPS data from paired device or mesh workers
// Returns true if valid GPS data is available from a remote source
bool getRemoteGPSData(RemoteGPSData* outData);

// Check if remote GPS data is available (quick check without full parse)
bool hasRemoteGPSData();

#endif // ENABLE_ESPNOW
#endif // ESPNOW_SENSORS_H
