#ifndef OPTIONAL_EVEN_G2_H
#define OPTIONAL_EVEN_G2_H

#include "System_BuildConfig.h"
#include <Arduino.h>

// =============================================================================
// EVEN REALITIES G2 GLASSES - BLE CLIENT
// =============================================================================
// This module implements ESP32 as a BLE Central/GATT Client to connect to
// Even Realities G2 smart glasses. This mode is mutually exclusive with the
// phone BLE server mode (Optional_Bluetooth).
//
// Requires: ENABLE_BLUETOOTH=1 AND ENABLE_G2_GLASSES=1
// Protocol reference: https://github.com/i-soxi/even-g2-protocol
// =============================================================================

// G2 requires Bluetooth to be enabled first
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// -----------------------------------------------------------------------------
// G2 BLE UUIDs (from protocol docs)
// -----------------------------------------------------------------------------
// Base UUID: 00002760-08c2-11e1-9073-0e8ac72e{xxxx}
#define G2_UUID_BASE          "00002760-08c2-11e1-9073-0e8ac72e"
#define G2_SERVICE_UUID       "00002760-08c2-11e1-9073-0e8ac72e0000"
#define G2_CHAR_WRITE_UUID    "00002760-08c2-11e1-9073-0e8ac72e5401"  // Write Without Response (commands)
#define G2_CHAR_NOTIFY_UUID   "00002760-08c2-11e1-9073-0e8ac72e5402"  // Notify (responses/events)
#define G2_CHAR_DISPLAY_UUID  "00002760-08c2-11e1-9073-0e8ac72e6402"  // Display/rendering

// -----------------------------------------------------------------------------
// G2 Protocol Constants
// -----------------------------------------------------------------------------
#define G2_PACKET_MAGIC       0xAA
#define G2_PACKET_TYPE_CMD    0x21  // Phone -> Glasses
#define G2_PACKET_TYPE_RSP    0x12  // Glasses -> Phone
#define G2_MTU_TARGET         512
#define G2_AUTH_PACKET_COUNT  7

// Service IDs (high byte, low byte)
#define G2_SVC_AUTH_CTRL_HI   0x80
#define G2_SVC_AUTH_CTRL_LO   0x00
#define G2_SVC_AUTH_DATA_HI   0x80
#define G2_SVC_AUTH_DATA_LO   0x20
#define G2_SVC_TELEPROMPTER_HI 0x06
#define G2_SVC_TELEPROMPTER_LO 0x20
#define G2_SVC_DISPLAY_CFG_HI 0x0E
#define G2_SVC_DISPLAY_CFG_LO 0x20
#define G2_SVC_SYNC_HI        0x80
#define G2_SVC_SYNC_LO        0x00

// -----------------------------------------------------------------------------
// G2 Connection State
// -----------------------------------------------------------------------------
enum G2State {
  G2_STATE_IDLE = 0,       // Not connected, not scanning
  G2_STATE_SCANNING,       // Scanning for G2 devices
  G2_STATE_CONNECTING,     // Connection in progress
  G2_STATE_AUTHENTICATING, // Running auth handshake
  G2_STATE_CONNECTED,      // Fully connected and authenticated
  G2_STATE_DISCONNECTING,  // Disconnecting
  G2_STATE_ERROR           // Error state
};

// Which eye to connect to
enum G2Eye {
  G2_EYE_LEFT = 0,
  G2_EYE_RIGHT = 1,
  G2_EYE_AUTO = 2  // Connect to first found
};

// G2 Event types for notifications
enum G2EventType {
  G2_EVENT_UNKNOWN = 0,
  G2_EVENT_SWIPE_UP,
  G2_EVENT_SWIPE_DOWN,
  G2_EVENT_SWIPE_LEFT,
  G2_EVENT_SWIPE_RIGHT,
  G2_EVENT_TAP,
  G2_EVENT_LONG_PRESS,
  G2_EVENT_DOUBLE_TAP
};

// Callback type for G2 input events
typedef void (*G2EventCallback)(G2EventType event);

// -----------------------------------------------------------------------------
// G2 Client State Structure
// -----------------------------------------------------------------------------
struct G2ClientState {
  G2State state;
  G2Eye targetEye;
  bool initialized;
  
  // Connection info
  String deviceName;
  String deviceAddress;
  uint16_t mtu;
  uint32_t connectedSince;
  
  // Protocol state
  uint8_t seqNumber;      // Incrementing sequence for packets
  uint16_t msgId;         // Message ID for payloads
  
  // Statistics
  uint32_t packetsSent;
  uint32_t packetsReceived;
  uint32_t authAttempts;
  
  // Event callback
  G2EventCallback eventCallback;
  
  // Deferred event handling (ISR-safe pattern: callback sets flag, task processes)
  bool deferredGesturePending;
  G2EventType deferredGestureEvent;
};

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

// Initialization (sets up BLE client mode, tears down server if running)
bool initG2Client();
void deinitG2Client();
bool isG2ClientInitialized();

// Connection management
bool g2Connect(G2Eye eye = G2_EYE_LEFT);
void g2Disconnect();
bool isG2Connected();
G2State getG2State();
const char* getG2StateString();

// Scanning
bool g2StartScan(uint32_t durationMs = 10000);
void g2StopScan();

// Display output
bool g2ShowText(const char* text);
bool g2ShowMultiLine(const char* lines[], size_t lineCount);
bool g2ClearDisplay();

// Event handling
void g2SetEventCallback(G2EventCallback callback);
void g2Tick();  // Call from main loop to process events

// Status
void getG2Status(char* buffer, size_t bufferSize);

// Low-level packet functions (for advanced use)
uint16_t g2CalcCRC16(const uint8_t* data, size_t len);
bool g2SendPacket(uint8_t serviceHi, uint8_t serviceLo, const uint8_t* payload, size_t payloadLen);
size_t g2EncodeVarint(uint32_t value, uint8_t* buffer);

#else // !(ENABLE_BLUETOOTH && ENABLE_G2_GLASSES)

// -----------------------------------------------------------------------------
// Stub declarations when G2 glasses support is disabled
// -----------------------------------------------------------------------------
enum G2State { G2_STATE_IDLE = 0 };
enum G2Eye { G2_EYE_LEFT = 0, G2_EYE_RIGHT = 1, G2_EYE_AUTO = 2 };
enum G2EventType { G2_EVENT_UNKNOWN = 0 };
typedef void (*G2EventCallback)(G2EventType event);

inline bool initG2Client() { return false; }
inline void deinitG2Client() {}
inline bool isG2ClientInitialized() { return false; }
inline bool g2Connect(G2Eye eye = G2_EYE_LEFT) { return false; }
inline void g2Disconnect() {}
inline bool isG2Connected() { return false; }
inline G2State getG2State() { return G2_STATE_IDLE; }
inline const char* getG2StateString() { return "disabled"; }
inline bool g2StartScan(uint32_t durationMs = 10000) { return false; }
inline void g2StopScan() {}
inline bool g2ShowText(const char* text) { return false; }
inline bool g2ShowMultiLine(const char* lines[], size_t lineCount) { return false; }
inline bool g2ClearDisplay() { return false; }
inline void g2SetEventCallback(G2EventCallback callback) {}
inline void g2Tick() {}
inline void getG2Status(char* buffer, size_t bufferSize) { if (buffer) buffer[0] = '\0'; }

#endif // ENABLE_BLUETOOTH

#endif // OPTIONAL_EVEN_G2_H
