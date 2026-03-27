#ifndef BLE_IDF_H
#define BLE_IDF_H

#include "System_BuildConfig.h"

#ifndef ENABLE_BLE_IDF_EXPERIMENTAL
#define ENABLE_BLE_IDF_EXPERIMENTAL 0
#endif

#if ENABLE_BLUETOOTH && ENABLE_BLE_IDF_EXPERIMENTAL

#include <stdint.h>
#include <stdbool.h>
#include "esp_bt.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_gattc_api.h"
#include "esp_bt_main.h"
#include "esp_bt_defs.h"

#include "BLE_Types.h"

// =============================================================================
// BLE SERVICE AND CHARACTERISTIC UUIDs (IDF uses integer constants for standard services)
// =============================================================================

// Command Service
#define BLE_IDF_COMMAND_SERVICE_UUID      "12345678-1234-5678-1234-56789abcdef0"
#define BLE_IDF_CMD_REQUEST_CHAR_UUID     "12345678-1234-5678-1234-56789abcde01"
#define BLE_IDF_CMD_RESPONSE_CHAR_UUID    "12345678-1234-5678-1234-56789abcde02"
#define BLE_IDF_CMD_STATUS_CHAR_UUID      "12345678-1234-5678-1234-56789abcde03"

// Data Streaming Service
#define BLE_IDF_DATA_SERVICE_UUID         "12345678-1234-5678-1234-56789abcdef1"
#define BLE_IDF_SENSOR_DATA_CHAR_UUID     "12345678-1234-5678-1234-56789abcde11"
#define BLE_IDF_SYSTEM_STATUS_CHAR_UUID   "12345678-1234-5678-1234-56789abcde12"
#define BLE_IDF_EVENT_NOTIFY_CHAR_UUID    "12345678-1234-5678-1234-56789abcde13"
#define BLE_IDF_STREAM_CONTROL_CHAR_UUID  "12345678-1234-5678-1234-56789abcde14"

// Device Info (standard BLE service - integer UUIDs for IDF GATTS)
#define BLE_IDF_DEVICE_INFO_SERVICE_UUID  0x180A
#define BLE_IDF_MANUFACTURER_CHAR_UUID    0x2A29
#define BLE_IDF_MODEL_CHAR_UUID           0x2A24
#define BLE_IDF_FIRMWARE_CHAR_UUID        0x2A26

// =============================================================================
// BLE MODE (IDF-specific, not shared)
// =============================================================================

enum BLEMode {
  BLE_MODE_OFF = 0,
  BLE_MODE_SERVER,      // GATT Server (phone peripheral mode)
  BLE_MODE_CLIENT       // GATT Client (G2 glasses central mode)
};

// =============================================================================
// CONNECTION TRACKING
// =============================================================================

struct BLEConnection {
  bool active;
  uint16_t connId;
  uint16_t gatts_if;            // GATTS interface for this connection
  esp_bd_addr_t remote_bda;     // Remote device address
  uint32_t connectedSince;
  char deviceName[32];
  BLEDeviceType deviceType;
  uint32_t commandsReceived;
  uint32_t responsesSent;
  uint32_t lastActivityMs;
  uint8_t streamFlags;
};

// =============================================================================
// PUBLIC API
// =============================================================================

// Lifecycle
bool bleIdfInit();
void bleIdfDeinit();
bool bleIdfIsRunning();

// Mode control
bool bleIdfStartServer();      // Start GATT Server (phone mode)
bool bleIdfStopServer();
bool bleIdfStartClient();      // Start GATT Client (G2 mode)
bool bleIdfStopClient();
BLEMode bleIdfGetMode();
BLEConnectionState bleIdfGetState();

// Server (GATTS) API
bool bleIdfServerSendResponse(uint16_t connId, const uint8_t* data, size_t len);
bool bleIdfServerBroadcastResponse(const uint8_t* data, size_t len);
bool bleIdfServerSendSensorData(const uint8_t* data, size_t len);
bool bleIdfServerSendSystemStatus(const uint8_t* data, size_t len);
bool bleIdfServerSendEvent(BLEEventType eventType, const char* message);

// Stream control
void bleIdfEnableStream(uint8_t streamFlags);
void bleIdfDisableStream(uint8_t streamFlags);
void bleIdfSetStreamInterval(uint32_t sensorMs, uint32_t systemMs);
bool bleIdfIsStreamEnabled(uint8_t streamFlag);
void bleIdfUpdateStreams();

// Connection status
bool bleIdfIsConnected();

// Client (GATTC) API - for G2 glasses
bool bleIdfClientScan(uint32_t durationMs);
bool bleIdfClientConnect(const esp_bd_addr_t remote_bda);
bool bleIdfClientDisconnect();
bool bleIdfClientWrite(const uint8_t* data, size_t len);
bool bleIdfClientIsConnected();

// Status
void bleIdfGetStatus(char* buffer, size_t bufferSize);
const char* bleIdfGetStateString();
int bleIdfGetConnectionCount();
bool bleIdfGetConnectionInfo(int index, BLEConnection* outInfo);

// Session management
void bleIdfSessionTick();

#else // !ENABLE_BLUETOOTH || !ENABLE_BLE_IDF_EXPERIMENTAL

#include <cstdint>
#include <cstddef>

// Stubs when Bluetooth is disabled
inline bool bleIdfInit() { return false; }
inline void bleIdfDeinit() {}
inline bool bleIdfIsRunning() { return false; }
inline bool bleIdfStartServer() { return false; }
inline bool bleIdfStopServer() { return false; }
inline bool bleIdfStartClient() { return false; }
inline bool bleIdfStopClient() { return false; }
inline int bleIdfGetMode() { return 0; }
inline int bleIdfGetState() { return 0; }
inline bool bleIdfServerSendResponse(uint16_t, const uint8_t*, size_t) { return false; }
inline bool bleIdfServerBroadcastResponse(const uint8_t*, size_t) { return false; }
inline bool bleIdfServerSendSensorData(const uint8_t*, size_t) { return false; }
inline bool bleIdfServerSendSystemStatus(const uint8_t*, size_t) { return false; }
inline bool bleIdfServerSendEvent(int, const char*) { return false; }
inline void bleIdfEnableStream(uint8_t) {}
inline void bleIdfDisableStream(uint8_t) {}
inline void bleIdfSetStreamInterval(uint32_t, uint32_t) {}
inline bool bleIdfIsStreamEnabled(uint8_t) { return false; }
inline void bleIdfUpdateStreams() {}
inline bool bleIdfIsConnected() { return false; }
inline bool bleIdfClientScan(uint32_t) { return false; }
inline bool bleIdfClientConnect(const uint8_t*) { return false; }
inline bool bleIdfClientDisconnect() { return false; }
inline bool bleIdfClientWrite(const uint8_t*, size_t) { return false; }
inline bool bleIdfClientIsConnected() { return false; }
inline void bleIdfGetStatus(char* buffer, size_t) { if (buffer) buffer[0] = '\0'; }
inline const char* bleIdfGetStateString() { return "disabled"; }
inline int bleIdfGetConnectionCount() { return 0; }
inline bool bleIdfGetConnectionInfo(int, void*) { return false; }
inline void bleIdfSessionTick() {}

#endif // ENABLE_BLUETOOTH

#endif // BLE_IDF_H
