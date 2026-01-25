#ifndef BLUETOOTH_SYSTEM_H
#define BLUETOOTH_SYSTEM_H

#include <Arduino.h>

#include "System_BuildConfig.h"

#if ENABLE_BLUETOOTH

#include <BLE2902.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>

#include "System_Command.h"

// =============================================================================
// BLE SERVICE AND CHARACTERISTIC UUIDs
// =============================================================================
// Using custom UUIDs for HardwareOne services
// Format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx

// Command Service - single service for all communication
// Glasses send commands, receive responses - uses existing command system
#define BLE_COMMAND_SERVICE_UUID      "12345678-1234-5678-1234-56789abcdef0"
#define BLE_CMD_REQUEST_CHAR_UUID     "12345678-1234-5678-1234-56789abcde01"  // Write - send any command
#define BLE_CMD_RESPONSE_CHAR_UUID    "12345678-1234-5678-1234-56789abcde02"  // Notify - receive response
#define BLE_CMD_STATUS_CHAR_UUID      "12345678-1234-5678-1234-56789abcde03"  // Read - connection status

// Data Streaming Service - continuous sensor and system data
#define BLE_DATA_SERVICE_UUID         "12345678-1234-5678-1234-56789abcdef1"
#define BLE_SENSOR_DATA_CHAR_UUID     "12345678-1234-5678-1234-56789abcde11"  // Notify - sensor data stream
#define BLE_SYSTEM_STATUS_CHAR_UUID   "12345678-1234-5678-1234-56789abcde12"  // Notify - system status stream
#define BLE_EVENT_NOTIFY_CHAR_UUID    "12345678-1234-5678-1234-56789abcde13"  // Notify - event notifications
#define BLE_STREAM_CONTROL_CHAR_UUID  "12345678-1234-5678-1234-56789abcde14"  // Write - control streaming

// Device Info (standard BLE service)
#define BLE_DEVICE_INFO_SERVICE_UUID  "180A"
#define BLE_MANUFACTURER_CHAR_UUID    "2A29"
#define BLE_MODEL_CHAR_UUID           "2A24"
#define BLE_FIRMWARE_CHAR_UUID        "2A26"

// =============================================================================
// BLE CONNECTION STATE
// =============================================================================

enum BLEConnectionState {
  BLE_STATE_IDLE = 0,
  BLE_STATE_ADVERTISING,
  BLE_STATE_CONNECTED,
  BLE_STATE_DISCONNECTING
};

// Device types for MAC address mapping
enum BLEDeviceType {
  BLE_DEVICE_UNKNOWN = 0,
  BLE_DEVICE_GLASSES_LEFT,
  BLE_DEVICE_GLASSES_RIGHT,
  BLE_DEVICE_RING,
  BLE_DEVICE_PHONE,
  BLE_DEVICE_CUSTOM
};

// Event types for notification system
enum BLEEventType {
  BLE_EVENT_SENSOR_CONNECTED = 0,
  BLE_EVENT_SENSOR_DISCONNECTED,
  BLE_EVENT_LOW_BATTERY,
  BLE_EVENT_WIFI_CONNECTED,
  BLE_EVENT_WIFI_DISCONNECTED,
  BLE_EVENT_BUTTON_PRESS,
  BLE_EVENT_GESTURE_DETECTED,
  BLE_EVENT_THRESHOLD_EXCEEDED,
  BLE_EVENT_ERROR,
  BLE_EVENT_CUSTOM
};

// Stream control flags
enum BLEStreamFlags {
  BLE_STREAM_NONE = 0,
  BLE_STREAM_SENSORS = (1 << 0),
  BLE_STREAM_SYSTEM = (1 << 1),
  BLE_STREAM_EVENTS = (1 << 2),
  BLE_STREAM_ALL = 0xFF
};

// =============================================================================
// BLE SYSTEM STATE STRUCTURE
// =============================================================================

#define BLE_MAX_CONNECTIONS 4  // Support up to 4 simultaneous connections (glasses x2, ring, phone)

struct BLEConnection {
  bool active;
  uint16_t connId;                  // Connection ID from BLE stack
  uint32_t connectedSince;          // millis() when connected
  String deviceName;                // Name of connected device (if available)
  uint8_t deviceAddr[6];            // MAC address of connected device
  BLEDeviceType deviceType;         // Device type (identified by MAC)
  uint32_t commandsReceived;        // Commands from this device
};

struct BLESystemState {
  bool initialized;
  BLEConnectionState connectionState;
  BLEConnection connections[BLE_MAX_CONNECTIONS];
  uint8_t activeConnectionCount;
  
  // Statistics
  uint32_t totalConnections;
  uint32_t commandsReceived;
  uint32_t responsesSent;
  
  // Streaming state
  uint8_t streamFlags;              // Active streams (BLEStreamFlags)
  uint32_t sensorStreamInterval;    // ms between sensor updates
  uint32_t systemStreamInterval;    // ms between system updates
  uint32_t lastSensorStream;        // millis() of last sensor stream
  uint32_t lastSystemStream;        // millis() of last system stream
  uint32_t sensorStreamCount;       // Total sensor streams sent
  uint32_t systemStreamCount;       // Total system streams sent
  uint32_t eventCount;              // Total events sent
};

extern BLESystemState* gBLEState;

// =============================================================================
// DEBUG FLAGS
// =============================================================================

#define DEBUG_BLE_CORE        0x1000000   // Core BLE operations (init, connect, disconnect)
#define DEBUG_BLE_GATT        0x2000000   // GATT read/write/notify operations
#define DEBUG_BLE_DATA        0x4000000   // Data transfer details

// =============================================================================
// PUBLIC API
// =============================================================================

// Initialization
bool initBluetooth();
void deinitBluetooth();

// Device type management
const char* bleDeviceTypeToString(BLEDeviceType type);
BLEDeviceType bleIdentifyDeviceByMAC(const uint8_t* mac);

// Advertising control
bool startBLEAdvertising();
void stopBLEAdvertising();

// Connection management
bool isBLEConnected();
void disconnectBLE();
uint32_t getBLEConnectionDuration();

// Send response back to connected client
bool sendBLEResponse(const char* data, size_t len);

// Data streaming pipeline API
bool blePushSensorData(const char* jsonData, size_t len);
bool blePushSystemStatus(const char* jsonData, size_t len);
bool blePushEvent(BLEEventType eventType, const char* message, const char* details = nullptr);

// Stream control
void bleEnableStream(uint8_t streamFlags);
void bleDisableStream(uint8_t streamFlags);
void bleSetStreamInterval(uint32_t sensorMs, uint32_t systemMs);
bool bleIsStreamEnabled(uint8_t streamFlag);

// Auto-streaming (call from main loop)
void bleUpdateStreams();

// Status
void getBLEStatus(char* buffer, size_t bufferSize);
const char* getBLEStateString();

// Settings
void bleApplySettings();

// =============================================================================
// COMMAND REGISTRY
// =============================================================================

extern const CommandEntry bluetoothCommands[];
extern const size_t bluetoothCommandsCount;

// =============================================================================
// SETTINGS ENTRIES
// =============================================================================

struct SettingEntry;
struct SettingsModule;
extern const SettingEntry bluetoothSettingsEntries[];
extern const size_t bluetoothSettingsCount;

#else // !ENABLE_BLUETOOTH

// Stub declarations when Bluetooth is disabled
inline bool initBluetooth() { return false; }
inline void deinitBluetooth() {}
inline bool startBLEAdvertising() { return false; }
inline void stopBLEAdvertising() {}
inline bool isBLEConnected() { return false; }
inline void disconnectBLE() {}
inline uint32_t getBLEConnectionDuration() { return 0; }
inline bool sendBLEResponse(const char*, size_t) { return false; }
inline void getBLEStatus(char* buffer, size_t) { if (buffer) buffer[0] = '\0'; }
inline const char* getBLEStateString() { return "disabled"; }
inline void bleApplySettings() {}

// Streaming API stubs
inline bool blePushSensorData(const char*, size_t) { return false; }
inline bool blePushSystemStatus(const char*, size_t) { return false; }
inline bool blePushEvent(int, const char*, const char* = nullptr) { return false; }
inline void bleEnableStream(uint8_t) {}
inline void bleDisableStream(uint8_t) {}
inline void bleSetStreamInterval(uint32_t, uint32_t) {}
inline bool bleIsStreamEnabled(uint8_t) { return false; }
inline void bleUpdateStreams() {}

// OLED menu stubs
inline void bluetoothMenuUp() {}
inline void bluetoothMenuDown() {}
inline void executeBluetoothAction() {}
inline void bluetoothMenuBack() {}
inline void displayBluetoothPage() {}
inline bool handleBluetoothInput(uint16_t, uint16_t) { return false; }

// Global state stub
extern bool bluetoothShowingStatus;

#endif // ENABLE_BLUETOOTH

#endif // BLUETOOTH_SYSTEM_H
