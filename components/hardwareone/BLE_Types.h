#ifndef BLE_TYPES_H
#define BLE_TYPES_H

// ============================================================================
// Shared BLE Type Definitions
// ============================================================================
// Single source of truth for BLE enums and constants used by both
// Optional_Bluetooth.h (Arduino BLE stack) and BLE_IDF.h (ESP-IDF native stack).

enum BLEConnectionState {
  BLE_STATE_IDLE = 0,
  BLE_STATE_ADVERTISING,
  BLE_STATE_SCANNING,
  BLE_STATE_CONNECTING,
  BLE_STATE_CONNECTED,
  BLE_STATE_DISCONNECTING
};

enum BLEDeviceType {
  BLE_DEVICE_UNKNOWN = 0,
  BLE_DEVICE_GLASSES_LEFT,
  BLE_DEVICE_GLASSES_RIGHT,
  BLE_DEVICE_RING,
  BLE_DEVICE_PHONE,
  BLE_DEVICE_CUSTOM
};

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

enum BLEStreamFlags {
  BLE_STREAM_NONE = 0,
  BLE_STREAM_SENSORS = (1 << 0),
  BLE_STREAM_SYSTEM = (1 << 1),
  BLE_STREAM_EVENTS = (1 << 2),
  BLE_STREAM_ALL = 0xFF
};

#define BLE_MAX_CONNECTIONS 4

#endif // BLE_TYPES_H
