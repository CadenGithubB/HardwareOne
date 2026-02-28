#ifndef ESPNOW_SYSTEM_H
#define ESPNOW_SYSTEM_H

#include <Arduino.h>

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <ArduinoJson.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <vector>
#include <WiFi.h>

#include "System_Debug.h"
#include "System_Settings.h"
#include "System_User.h"
#include "System_Utils.h"

// ESP-NOW command registry
extern const CommandEntry espNowCommands[];
extern const size_t espNowCommandsCount;

// Forward declarations from main system
void broadcastOutput(const String& s);
void broadcastOutput(const char* s);
bool writeSettingsJson();
extern AuthContext gExecAuthContext;
String base64Encode(const uint8_t* data, size_t len);

// Message types
#define MSG_TYPE_HB "HB"
#define MSG_TYPE_ACK "ACK"
#define MSG_TYPE_MESH_SYS "MESH_SYS"
#define MSG_TYPE_RESPONSE "RESPONSE"
#define MSG_TYPE_STREAM "STREAM"
#define MSG_TYPE_BOOT "BOOT"
// Note: MSG_TYPE_MESH removed - mesh is a transport method (TTL-based), not a message type
// JSON-only logical types (names chosen to avoid collision with enum MessageType)
#define MSG_TYPE_FILE_STR "FILE"
#define MSG_TYPE_CMD "CMD"
#define MSG_TYPE_TEXT "TEXT"
#define MSG_TYPE_USER_SYNC "USER_SYNC"
#define MSG_TYPE_FILE_BROWSE "FILE_BROWSE"

// Message priorities
enum MessagePriority {
  PRIORITY_LOW = 0,
  PRIORITY_NORMAL = 1,
  PRIORITY_HIGH = 2
};

// Payload types
#define PAYLOAD_CMD "cmd"
#define PAYLOAD_TOPO_REQ "topoReq"
#define PAYLOAD_TOPO_RESP "topoResp"
#define PAYLOAD_QUERY "query"
#define PAYLOAD_STATUS "status"
#define PAYLOAD_TIME_SYNC "timeSync"

// Mesh roles
enum MeshRole {
  MESH_ROLE_WORKER = 0,
  MESH_ROLE_MASTER = 1,
  MESH_ROLE_BACKUP_MASTER = 2
};

// ==========================
// ESP-NOW Mode (Direct vs Mesh)
// ==========================
enum EspNowMode { 
  ESPNOW_MODE_DIRECT = 0,
  ESPNOW_MODE_MESH = 1 
};

#if ENABLE_BONDED_MODE
// ==========================
// Bond Mode Sync Tracking
// ==========================
// No linear state machine — sync tick idempotently fetches what's missing.
// Master "Synced" = lastRemoteCapValid && bondManifestReceived && bondSettingsReceived
// Worker "Synced" = lastRemoteCapValid && bondCapSent && bondSettingsSent

enum BondSyncRequestType : uint8_t {
  BOND_SYNC_NONE = 0,
  BOND_SYNC_CAP = 1,
  BOND_SYNC_MANIFEST = 2,
  BOND_SYNC_SETTINGS = 3,
};
#endif // ENABLE_BONDED_MODE

// ESP-NOW device name mapping
struct EspNowDevice {
  uint8_t mac[6];
  String name;
  bool encrypted;   // Whether this device uses encryption
  uint8_t key[16];  // Per-device encryption key
};

// ==========================
// Mesh Topology Structures (from espnow_system.cpp)
// ==========================

// Topology streaming support (NEW - matches .cpp implementation)
#define MAX_CONCURRENT_TOPO_STREAMS 4
#define MAX_TOPO_PEERS 16
struct TopologyStream {
  uint32_t reqId;              // Request ID to match responses
  uint8_t senderMac[6];        // MAC of device sending topology
  char senderName[32];         // Name of sender device
  uint16_t totalPeers;         // Total number of peers to expect
  uint16_t receivedPeers;      // Peers received so far
  unsigned long startTime;     // Stream start time
  bool active;                 // Stream in progress
  String accumulatedData;      // Accumulated peer info for display
  String path;                 // Path from master to this device (comma-separated MACs)
};

// Topology device name cache
#define MAX_TOPO_DEVICE_CACHE 16
struct TopoDeviceEntry {
  uint8_t mac[6];
  char name[32];
  bool active;
};

// Buffered peer message (for out-of-order delivery)
#define MAX_BUFFERED_PEERS 10
struct BufferedPeerMessage {
  String message;              // Full JSON message to forward/process later
  uint32_t reqId;              // Request ID to match with stream
  uint8_t masterMac[6];        // Master MAC (destination of PEER)
  unsigned long receivedMs;    // When this was buffered (for timeout)
  bool active;                 // Slot in use
};

// Mesh topology peer structure
struct MeshTopoPeer {
  uint8_t mac[6];
  String name;
  int8_t rssi;
  uint32_t lastSeen;
  uint32_t heartbeatCount;
  bool isDirect;  // true if direct peer, false if mesh-only
};

// Mesh topology node (for graph building)
struct MeshTopoNode {
  uint8_t mac[6];
  String name;
  std::vector<MeshTopoPeer> peers;  // Direct peers with metadata
};

// ==========================
// Mesh Peer Health Tracking (from espnow_system.cpp)
// ==========================
#define MESH_PEER_MAX 16          // Compile-time ceiling (max possible value of meshPeerMax setting)
#define MESH_PEER_TIMEOUT_MS 30000

// Runtime peer slot count (set from gSettings.meshPeerMax at boot, capped to MESH_PEER_MAX)
extern int gMeshPeerSlots;

struct MeshPeerHealth {
  uint8_t mac[6];
  uint8_t _pad[2];           // Padding for alignment
  uint32_t lastHeartbeatMs;  // Last time we received MESHHB from this peer
  uint32_t lastAckMs;        // Last time we received MESHACK from this peer
  uint32_t heartbeatCount;   // Total heartbeats received
  uint32_t ackCount;         // Total ACKs received
  uint32_t lastBootCounter;  // Last known boot counter from this peer
  int8_t rssi;               // Last RSSI from heartbeat
  bool isActive;             // true if this slot is in use
};

// ==========================
// Mesh Peer Metadata (device organization for rooms/zones/tags)
// ==========================
// Aggregated on master from workerStatus payloads; used for
// room-based queries, MQTT HA bridge discovery, and OLED device list.
struct MeshPeerMeta {
  uint8_t mac[6];
  char name[32];           // From espnowDeviceName
  char friendlyName[48];   // From espnowFriendlyName
  char room[32];           // From espnowRoom
  char zone[32];           // From espnowZone
  char tags[64];           // From espnowTags (comma-separated)
  bool stationary;         // From espnowStationary
  uint32_t sensorMask;     // From CapabilitySummary or workerStatus
  uint32_t lastMetaUpdate; // millis() when metadata last received
  bool isActive;           // true if this slot is in use

  void clear() {
    memset(mac, 0, 6);
    memset(name, 0, sizeof(name));
    memset(friendlyName, 0, sizeof(friendlyName));
    memset(room, 0, sizeof(room));
    memset(zone, 0, sizeof(zone));
    memset(tags, 0, sizeof(tags));
    stationary = false;
    sensorMask = 0;
    lastMetaUpdate = 0;
    isActive = false;
  }
};

extern MeshPeerMeta* gMeshPeerMeta;  // Dynamically allocated [gMeshPeerSlots] at init

// Peer metadata helpers
MeshPeerMeta* getMeshPeerMeta(const uint8_t mac[6], bool createIfMissing = false);
int countMeshPeerMetaByRoom(const char* room);

// ==========================
// Mesh Retry Queue (from main .ino)
// ==========================
#define MESH_RETRY_QUEUE_SIZE 8
#define MESH_ACK_TIMEOUT_MS 3000
#define MESH_MAX_RETRIES 2

struct MeshRetryEntry {
  uint32_t msgId;
  uint8_t dstMac[6];
  String envelope;
  uint32_t sentMs;
  uint8_t retryCount;
  bool active;
};

// ==========================
// Mesh Deduplication
// ==========================
#define MESH_DEDUP_SIZE 24
#define MESH_DEDUP_WINDOW 50

struct MeshSeenEntry {
  uint8_t src[6];
  uint32_t msgId;
};

// ==========================
// Broadcast ACK Tracking
// ==========================
#define BROADCAST_TRACKER_SLOTS 8
#define BROADCAST_TRACKER_MAX_PEERS 20
#define BROADCAST_TRACKER_TIMEOUT_MS 3000

struct BroadcastTracker {
  uint32_t msgId;                                     // Message ID being tracked
  uint32_t startMs;                                  // When broadcast started
  uint8_t peerMacs[BROADCAST_TRACKER_MAX_PEERS][6]; // MACs we sent to
  bool ackReceived[BROADCAST_TRACKER_MAX_PEERS];     // Which peers ACK'd
  uint8_t expectedCount;                             // Number of peers we sent to
  uint8_t receivedCount;                             // Number of ACKs received
  bool active;                                       // Tracker slot in use
  bool reported;                                     // Results already reported
  
  void reset() {
    msgId = 0;
    startMs = 0;
    memset(peerMacs, 0, sizeof(peerMacs));
    memset(ackReceived, 0, sizeof(ackReceived));
    expectedCount = 0;
    receivedCount = 0;
    active = false;
    reported = false;
  }
};

// ==========================
// Unpaired Device Tracking
// ==========================
#define MAX_UNPAIRED_DEVICES 16

struct UnpairedDevice {
  uint8_t mac[6];
  String name;
  int rssi;
  uint32_t lastSeenMs;
  uint32_t heartbeatCount;
};

// ==========================
// Bond Mode Structures
// ==========================

// Capability summary - small binary report exchanged on bonding
// Total size: 64 bytes (fits in single ESP-NOW packet)
struct CapabilitySummary {
  uint8_t protoVersion;        // Protocol version (1)
  uint8_t fwHash[16];          // Firmware hash (first 16 bytes of SHA256)
  uint8_t role;                // 0=worker, 1=master
  uint8_t reserved1;
  
  // Feature masks (32 bits each)
  uint32_t featureMask;        // Compile-time features (WiFi, BT, MQTT, etc.)
  uint32_t serviceMask;        // Runtime services (HTTP, ESP-NOW, etc.)
  uint32_t sensorMask;         // I2C sensors (thermal, ToF, IMU, etc.)
  
  // Hardware info
  uint8_t mac[6];              // Device MAC address
  uint8_t chipModel;           // ESP32 chip model
  uint8_t reserved2;
  
  uint32_t flashSizeMB;        // Flash size in MB
  uint32_t psramSizeMB;        // PSRAM size in MB
  uint8_t wifiChannel;         // Current WiFi channel
  uint8_t reserved3[3];
  
  char deviceName[20];         // Device name (null-terminated)
  uint32_t uptimeSeconds;      // Uptime in seconds
};

#if ENABLE_BONDED_MODE
// Bond mode message types (v2 JSON - deprecated, now use v3 binary)
#define MSG_TYPE_BOND_CAP_REQ "PAIR_CAP_REQ"     // Request capability summary (wire value kept for compat)
#define MSG_TYPE_BOND_CAP_RESP "PAIR_CAP_RESP"   // Capability summary response
#define MSG_TYPE_BOND_MANIFEST_REQ "PAIR_MAN_REQ" // Request full manifest
#define MSG_TYPE_BOND_MANIFEST_RESP "PAIR_MAN_RESP" // Manifest response (chunked)

// Periodic bond status snapshot — sent in response to BOND_STATUS_REQ (~30s interval)
// Uses same CAP_SENSOR_* bitmask constants as CapabilitySummary
struct __attribute__((packed)) BondPeerStatus {
  uint32_t uptimeSec;          // Device uptime in seconds
  uint32_t freeHeap;           // Free heap bytes
  uint32_t minFreeHeap;        // Minimum free heap since boot
  uint16_t sensorEnabledMask;  // CAP_SENSOR_* bits for sensors currently running (xxxEnabled)
  uint16_t sensorConnectedMask;// CAP_SENSOR_* bits for sensors currently connected (xxxConnected)
  uint8_t  wifiConnected;      // WiFi STA connected (0/1)
  uint8_t  bluetoothActive;    // Bluetooth active (0/1)
  uint8_t  httpActive;         // HTTP server running (0/1)
  uint8_t  bondHandshakeState; // Current handshake state
  uint8_t  reserved[4];        // Future use
};
#endif // ENABLE_BONDED_MODE

// ==========================
// Capability Bit Definitions
// ==========================

// Feature mask bits (compile-time capabilities)
#define CAP_FEATURE_WIFI        (1 << 0)
#define CAP_FEATURE_BLUETOOTH   (1 << 1)
#define CAP_FEATURE_MQTT        (1 << 2)
#define CAP_FEATURE_CAMERA      (1 << 3)
#define CAP_FEATURE_MICROPHONE  (1 << 4)
#define CAP_FEATURE_ESP_SR      (1 << 5)
#define CAP_FEATURE_AUTOMATION  (1 << 6)
#define CAP_FEATURE_MAPS        (1 << 7)
#define CAP_FEATURE_OLED        (1 << 8)
#define CAP_FEATURE_ESPNOW      (1 << 9)

// Service mask bits (runtime services)
#define CAP_SERVICE_ESPNOW      (1 << 0)
#define CAP_SERVICE_WIFI_CONN   (1 << 1)
#define CAP_SERVICE_HTTP        (1 << 2)
#define CAP_SERVICE_BLUETOOTH   (1 << 3)

// Sensor mask bits (I2C sensors)
#define CAP_SENSOR_THERMAL      (1 << 0)
#define CAP_SENSOR_TOF          (1 << 1)
#define CAP_SENSOR_IMU          (1 << 2)
#define CAP_SENSOR_GAMEPAD      (1 << 3)
#define CAP_SENSOR_APDS         (1 << 4)
#define CAP_SENSOR_GPS          (1 << 5)
#define CAP_SENSOR_RTC          (1 << 6)
#define CAP_SENSOR_PRESENCE     (1 << 7)

// Human-readable capability names (for UI display)
struct CapabilityName {
  uint32_t bit;
  const char* shortName;  // For OLED (compact)
  const char* longName;   // For web/CLI
};

// Feature names
static const CapabilityName FEATURE_NAMES[] = {
  { CAP_FEATURE_WIFI,       "WiFi",   "WiFi" },
  { CAP_FEATURE_BLUETOOTH,  "BT",     "Bluetooth" },
  { CAP_FEATURE_MQTT,       "MQTT",   "MQTT" },
  { CAP_FEATURE_CAMERA,     "Cam",    "Camera" },
  { CAP_FEATURE_MICROPHONE, "Mic",    "Microphone" },
  { CAP_FEATURE_ESP_SR,     "SR",     "Speech Recognition" },
  { CAP_FEATURE_AUTOMATION, "Auto",   "Automation" },
  { CAP_FEATURE_MAPS,       "Maps",   "Maps" },
  { CAP_FEATURE_OLED,       "OLED",   "OLED Display" },
  { CAP_FEATURE_ESPNOW,     "NOW",    "ESP-NOW" },
  { 0, nullptr, nullptr }
};

// Service names
static const CapabilityName SERVICE_NAMES[] = {
  { CAP_SERVICE_ESPNOW,    "NOW",    "ESP-NOW" },
  { CAP_SERVICE_WIFI_CONN, "WiFi",   "WiFi Connected" },
  { CAP_SERVICE_HTTP,      "HTTP",   "HTTP Server" },
  { CAP_SERVICE_BLUETOOTH, "BT",     "Bluetooth Active" },
  { 0, nullptr, nullptr }
};

// Sensor names
static const CapabilityName SENSOR_NAMES[] = {
  { CAP_SENSOR_THERMAL,  "Therm",  "Thermal Camera" },
  { CAP_SENSOR_TOF,      "ToF",    "Time-of-Flight" },
  { CAP_SENSOR_IMU,      "IMU",    "IMU/Accelerometer" },
  { CAP_SENSOR_GAMEPAD,  "Pad",    "Gamepad" },
  { CAP_SENSOR_APDS,     "APDS",   "Gesture/Color" },
  { CAP_SENSOR_GPS,      "GPS",    "GPS" },
  { CAP_SENSOR_RTC,      "RTC",    "Real-Time Clock" },
  { CAP_SENSOR_PRESENCE, "Pres",   "Presence Sensor" },
  { 0, nullptr, nullptr }
};

// Helper: Get comma-separated list of enabled capabilities (short names for OLED)
inline String getCapabilityListShort(uint32_t mask, const CapabilityName* names) {
  String result;
  for (int i = 0; names[i].shortName != nullptr; i++) {
    if (mask & names[i].bit) {
      if (result.length() > 0) result += ",";
      result += names[i].shortName;
    }
  }
  return result.length() > 0 ? result : String("None");
}

// Helper: Get comma-separated list of enabled capabilities (long names for web/CLI)
inline String getCapabilityListLong(uint32_t mask, const CapabilityName* names) {
  String result;
  for (int i = 0; names[i].longName != nullptr; i++) {
    if (mask & names[i].bit) {
      if (result.length() > 0) result += ", ";
      result += names[i].longName;
    }
  }
  return result.length() > 0 ? result : String("None");
}

// ==========================
// Message Structures
// ==========================
// Message type classification (for router internal use)
enum MessageType {
  MSG_TYPE_DATA = 0,     // Generic data message
  MSG_TYPE_COMMAND,      // Remote command execution
  MSG_TYPE_RESPONSE_ENUM,     // Command response (avoid conflict with MSG_TYPE_RESPONSE string)
  MSG_TYPE_FILE,         // File transfer
  MSG_TYPE_STREAM_ENUM,       // Stream output (avoid conflict with MSG_TYPE_STREAM string)
  MSG_TYPE_HEARTBEAT,    // Mesh heartbeat
  MSG_TYPE_TOPOLOGY,     // Topology discovery
  MSG_TYPE_BROADCAST     // Broadcast message
};

// Message structure for router
struct Message {
  uint8_t dstMac[6];           // Destination MAC address
  String payload;              // Message payload (will be chunked if needed)
  MessagePriority priority;    // Message priority
  MessageType type;            // Message type
  bool requiresAck;            // Whether ACK is needed
  uint32_t msgId;              // Unique message ID (auto-generated)
  int ttl;                     // Time-to-live for retries (hops for mesh)
  unsigned long timestamp;     // When message was created
  uint8_t maxRetries;          // Maximum retry attempts (0 = no retry)
  
  // Constructor with defaults
  Message() : priority(PRIORITY_NORMAL), type(MSG_TYPE_DATA), 
              requiresAck(false), msgId(0), ttl(3), timestamp(0), maxRetries(0) {
    memset(dstMac, 0, 6);
  }
};

// Queued message structure (for retry queue)
struct QueuedMessage {
  Message msg;                 // The message to send
  uint8_t retryCount;          // Number of retries attempted
  unsigned long nextRetryTime; // When to retry next (millis)
  bool active;                 // Whether this slot is in use
  
  // Constructor
  QueuedMessage() : retryCount(0), nextRetryTime(0), active(false) {}
};

// Per-device message buffer size based on available memory
// With PSRAM: 100 messages per device (~30KB each), Without: 5 messages (~1.5KB each)
#if CONFIG_SPIRAM_SUPPORT || CONFIG_ESP32S3_SPIRAM_SUPPORT
  #define MESSAGES_PER_DEVICE 100
#else
  #define MESSAGES_PER_DEVICE 5
#endif

// Message types for logging
enum LogMessageType {
  MSG_TEXT = 0,           // Regular text message
  MSG_FILE_SEND_START,    // File transfer started
  MSG_FILE_SEND_SUCCESS,  // File transfer completed successfully
  MSG_FILE_SEND_FAILED,   // File transfer failed
  MSG_FILE_RECV_SUCCESS,  // File received successfully
  MSG_FILE_RECV_FAILED    // File receive failed
};

struct ReceivedTextMessage {
  uint8_t senderMac[6];            // Sender MAC
  char senderName[32];             // Sender device name
  char message[256];               // Message text (trimmed to 256 chars)
  unsigned long timestamp;         // When received (millis)
  bool encrypted;                  // Whether message was encrypted
  uint32_t seqNum;                 // Sequence number for deduplication
  LogMessageType msgType;          // Message type (text, file transfer, etc)
  bool active;                     // Whether this slot is in use
  
  ReceivedTextMessage() : timestamp(0), encrypted(false), seqNum(0), msgType(MSG_TEXT), active(false) {
    memset(senderMac, 0, 6);
    memset(senderName, 0, 32);
    memset(message, 0, 256);
  }
};

// Per-device message history buffer
struct PeerMessageHistory {
  uint8_t peerMac[6];                           // Peer MAC address
  ReceivedTextMessage messages[MESSAGES_PER_DEVICE];  // Ring buffer of messages
  uint8_t head;                                 // Next write position
  uint8_t tail;                                 // Oldest message position
  uint8_t count;                                // Number of messages in buffer
  bool active;                                  // Whether this peer slot is in use
  
  PeerMessageHistory() : head(0), tail(0), count(0), active(false) {
    memset(peerMac, 0, 6);
  }
};

// Chunk reassembly buffer
struct ChunkBuffer {
  uint32_t msgId;                    // Message ID being reassembled
  uint32_t totalChunks;              // Total number of chunks expected
  uint32_t receivedChunks;           // Number of chunks received so far
  String chunks[10];                 // Chunk data (max 10 chunks = 2000 bytes)
  bool chunkReceived[10];            // Track which chunks we have
  unsigned long lastChunkTime;       // Timestamp of last chunk received
  uint8_t senderMac[6];              // Sender MAC address
  bool active;                       // Whether this buffer is in use
  
  // Constructor
  ChunkBuffer() : msgId(0), totalChunks(0), receivedChunks(0), 
                  lastChunkTime(0), active(false) {
    memset(chunkReceived, 0, sizeof(chunkReceived));
    memset(senderMac, 0, 6);
  }
  
  // Check if message is complete
  bool isComplete() const {
    return active && (receivedChunks == totalChunks);
  }
  
  // Reassemble complete message
  String reassemble() const {
    String result = "";
    for (uint32_t i = 0; i < totalChunks; i++) {
      result += chunks[i];
    }
    return result;
  }
  
  // Reset buffer
  void reset() {
    msgId = 0;
    totalChunks = 0;
    receivedChunks = 0;
    lastChunkTime = 0;
    active = false;
    memset(chunkReceived, 0, sizeof(chunkReceived));
    memset(senderMac, 0, 6);
    for (int i = 0; i < 10; i++) {
      chunks[i] = "";
    }
  }
};

// Router metrics
struct RouterMetrics {
  uint32_t messagesSent;
  uint32_t messagesReceived;
  uint32_t messagesFailed;
  uint32_t messagesRetried;
  uint32_t messagesDropped;
  uint32_t directRoutes;
  uint32_t meshRoutes;
  uint32_t chunkedMessages;
  uint32_t chunksSent;
  uint32_t chunksDropped;
  uint32_t chunksReceived;
  uint32_t chunksReassembled;
  uint32_t chunksTimedOut;
  uint32_t avgSendTimeUs;
  uint32_t maxSendTimeUs;
  uint32_t messagesQueued;       // Messages added to retry queue
  uint32_t messagesDequeued;     // Messages removed from retry queue
  uint32_t retriesAttempted;     // Total retry attempts
  uint32_t retriesSucceeded;     // Successful retries
  uint32_t queueOverflows;       // Times queue was full
  // V3 binary fragmentation metrics
  uint32_t v3FragTx;             // Total V3 fragments transmitted
  uint32_t v3FragRx;             // Total V3 fragments received
  uint32_t v3FragRxCompleted;    // V3 messages fully reassembled
  uint32_t v3FragRxGc;           // V3 reassembly contexts GC'ed due to timeout
  // Mesh routing metrics (per-message-type tracking)
  uint32_t meshForwardsByType[8];    // Forwards by type: [HB, ACK, MESH_SYS, FILE, CMD, TEXT, RESPONSE, STREAM]
  uint32_t meshTTLExhausted;         // Messages dropped due to TTL=0
  uint32_t meshLoopDetected;         // Messages dropped due to path loop detection
  uint32_t meshPathLengthSum;        // Sum of all path lengths (for averaging)
  uint32_t meshPathLengthCount;      // Count of messages with path data
  uint8_t meshMaxPathLength;         // Maximum path length observed
  uint32_t meshFallbacks;            // Direct send failures that fell back to mesh routing
  
  // Constructor
  RouterMetrics() : messagesSent(0), messagesReceived(0), messagesFailed(0),
                    messagesRetried(0), messagesDropped(0), directRoutes(0),
                    meshRoutes(0), chunkedMessages(0), chunksSent(0),
                    chunksDropped(0), chunksReceived(0), chunksReassembled(0),
                    chunksTimedOut(0), avgSendTimeUs(0), maxSendTimeUs(0),
                    messagesQueued(0), messagesDequeued(0), retriesAttempted(0),
                    retriesSucceeded(0), queueOverflows(0),
                    v3FragTx(0), v3FragRx(0), v3FragRxCompleted(0), v3FragRxGc(0),
                    meshTTLExhausted(0), meshLoopDetected(0), meshPathLengthSum(0), 
                    meshPathLengthCount(0), meshMaxPathLength(0), meshFallbacks(0) {
    memset(meshForwardsByType, 0, sizeof(meshForwardsByType));
  }
};

// Received message context for dispatch handlers
struct ReceivedMessage {
  const esp_now_recv_info* recvInfo;  // ESP-NOW receive info (contains src MAC, RSSI, etc)
  const uint8_t* rawData;              // Raw incoming data
  int dataLen;                         // Length of raw data
  String message;                      // Parsed message string
  bool isPaired;                       // Whether sender is paired
  bool isEncrypted;                    // Whether message was encrypted
  String deviceName;                   // Device name (if paired)
  String macStr;                       // Formatted MAC address string
  uint32_t cmdMsgId;                   // Command message ID for response tracking
  
  ReceivedMessage() : recvInfo(nullptr), rawData(nullptr), dataLen(0), 
                      isPaired(false), isEncrypted(false), cmdMsgId(0) {}
};

// ==========================
// ESP-NOW State Structure (complete version matching .ino implementation)
// ==========================
struct EspNowState {
  // Core state
  bool initialized;
  uint8_t channel;
  EspNowMode mode;
  
  // Send flow control
  volatile bool txDone;
  volatile esp_now_send_status_t lastStatus;
  volatile bool lastAckReceived;  // Track if last send received ACK (for CLI responses)
  
  // Encryption
  String passphrase;
  uint8_t derivedKey[16];
  bool encryptionEnabled;
  
  // Device registry
  EspNowDevice devices[16];
  int deviceCount;
  
  // Unpaired device tracking
  UnpairedDevice unpairedDevices[MAX_UNPAIRED_DEVICES];
  int unpairedDeviceCount;
  
  // Streaming
  uint8_t* streamTarget;  // MAC address (6 bytes, allocated)
  bool streamActive;
  bool streamingSuspended;
  uint32_t streamDroppedCount;
  uint32_t streamSentCount;
  uint32_t streamReceivedCount;
  unsigned long lastStreamSendTime;
  
  // File transfer ACK
  volatile uint16_t fileAckLast;
  char fileAckHashExpected[16];
  
  // List output buffer (PSRAM-allocated at init)
  char* listBuffer;
  
  // Message Router
  RouterMetrics routerMetrics;
  uint32_t nextMessageId;
  
  // Chunk reassembly (max 4 concurrent chunked messages)
  ChunkBuffer chunkBuffers[4];
  
  // Retry queue (max 8 queued messages)
  QueuedMessage retryQueue[8];
  uint8_t queueSize;  // Current number of messages in queue
  
  // Per-device message history buffers (for web UI and OLED)
  // Dynamically allocated [gMeshPeerSlots] at init
  PeerMessageHistory* peerMessageHistories;
  uint32_t globalMessageSeqNum; // Global sequence number for all messages
  
  // Statistics (non-router specific)
  uint32_t receiveErrors;
  uint32_t heartbeatsSent;
  uint32_t heartbeatsReceived;
  uint32_t meshForwards;
  uint32_t fileTransfersSent;
  uint32_t fileTransfersReceived;
  unsigned long lastResetTime;
  
  // Heartbeat mode
  bool heartbeatPublic;
  
  // Device name
  String deviceName;
  
  // Last received remote capability (for OLED/web display)
  CapabilitySummary lastRemoteCap;
  bool lastRemoteCapValid;
  unsigned long lastRemoteCapTime;
  
#if ENABLE_BONDED_MODE
  // Bond heartbeat tracking
  uint32_t bondHeartbeatsSent;
  uint32_t bondHeartbeatsReceived;
  unsigned long lastBondHeartbeatSentMs;
  unsigned long lastBondHeartbeatReceivedMs;
  bool bondPeerOnline;              // true if heartbeat received within timeout
  uint32_t bondPeerBootCounter;     // Peer's boot counter (detect reboots)
  uint32_t bondPeerSettingsHash;    // Peer's settings hash (detect changes)
  uint32_t bondLocalSettingsHash;   // Our settings hash (sent in heartbeat)
  uint32_t bondPeerUptime;          // Peer's uptime in seconds (from heartbeat)
  uint32_t bondLastOfflineMs;       // millis() when peer last went offline
  
  // Master-driven sync tick state
  BondSyncRequestType bondSyncInFlight; // What request is currently pending
  uint8_t bondSyncRetryCount;           // Retry counter for current request
  uint32_t bondSyncLastAttemptMs;       // When last sync request was sent
  
  // Deferred flags (set in ISR-like callback, consumed in task context)
  bool bondNeedsCapabilityRequest;  // Peer came online — master starts sync
  bool bondNeedsStreamingSetup;     // Apply saved streaming prefs after sync
  bool bondDeferredStreamCtrlPending; // STREAM_CTRL received, needs processing
  uint8_t bondDeferredStreamCtrlSensor;
  uint8_t bondDeferredStreamCtrlEnable;
  bool bondNeedsCapabilityResponse; // Peer requested our capabilities
  bool bondNeedsManifestResponse;   // Peer requested our manifest
  bool bondNeedsSettingsResponse;   // Peer requested our settings
  bool bondReceivedCapability;      // We received peer's CAP_RESP
  uint8_t bondPendingResponseMac[6]; // MAC of peer requesting cap/manifest/settings
  
  // Sync completion tracking (replaces linear handshake state machine)
  bool bondCapSent;                 // We sent our capabilities this session
  bool bondManifestReceived;        // We received/cached peer's manifest (master)
  bool bondSettingsReceived;        // We received peer's settings (master)
  bool bondSettingsSent;            // We sent our settings to peer (worker)
  
  // Session token (RAM only — computed after CAP exchange, cleared on offline)
  uint8_t bondSessionToken[16];
  bool bondSessionTokenValid;
  
  // RSSI tracking (updated from rx_ctrl on every received bond packet)
  int8_t bondRssiLast;
  int8_t bondRssiAvg;
  uint32_t bondLastRssiUpdateMs;
  
  // Deferred diagnostics: bond messages rejected because sender not in device list
  uint32_t bondUnpairedRejectCount;
  uint8_t  bondUnpairedRejectType;
  uint8_t  bondUnpairedRejectMac[6];
  
  // Periodic bond status poll cache (updated every ~30s from BOND_STATUS_RESP)
  BondPeerStatus bondPeerStatus;
  bool bondPeerStatusValid;
  unsigned long bondPeerStatusTimeMs;
  bool bondNeedsStatusResponse;
  bool bondNeedsProactiveStatus;  // Push status to peer when local sensor state changes
  unsigned long bondLastStatusReqMs;
#endif // ENABLE_BONDED_MODE
  
  bool bondNeedsMetadataResponse;   // flag to send metadata response (deferred from callback)
  uint8_t metadataPendingResponseMac[6]; // Dedicated MAC for metadata response (separate from bond exchanges)
  
  // Deferred metadata processing (set in callback, handled in task)
  bool deferredMetadataPending;
  uint8_t deferredMetadataSrcMac[6];
  uint8_t deferredMetadataPayload[216];  // V3PayloadMetadata size (212) + 4 bytes padding
  
  // Deferred message handling (ISR-safe pattern: callback sets flag, task processes)
  // TEXT message ring buffer (4 slots — prevents silent overwrite when messages arrive back-to-back)
  static constexpr int TEXT_QUEUE_SIZE = 4;  // Must be power of 2
  struct TextQueueEntry {
    uint8_t srcMac[6];
    char deviceName[32];
    char content[256];
    bool encrypted;
    bool used;
  };
  TextQueueEntry textQueue[TEXT_QUEUE_SIZE];
  volatile int textQueueHead;  // ISR writes here (producer)
  volatile int textQueueTail;  // Task reads here (consumer)
  
  // CMD_RESP message
  bool deferredCmdRespPending;
  uint8_t deferredCmdRespSrcMac[6];
  char deferredCmdRespDeviceName[32];
  char* deferredCmdRespResult;  // PSRAM-allocated at init (2048 bytes)
  bool deferredCmdRespSuccess;
  
  // STREAM message ring buffer (replaces single-buffer to prevent overwrite loss)
  static constexpr int STREAM_QUEUE_SIZE = 16;  // Must be power of 2
  struct StreamQueueEntry {
    uint8_t srcMac[6];
    char deviceName[32];
    char content[256];
    bool used;  // Slot contains valid data
  };
  StreamQueueEntry streamQueue[STREAM_QUEUE_SIZE];
  volatile int streamQueueHead;  // ISR writes here (producer)
  volatile int streamQueueTail;  // Task reads here (consumer)
  // CMD request (deferred to task for auth + execution)
  bool deferredCmdPending;
  uint8_t deferredCmdSrcMac[6];
  char deferredCmdDeviceName[32];
  char deferredCmdPayload[256];
  uint32_t deferredCmdMsgId;
  
  // Constructor
  EspNowState() : 
    initialized(false),
    channel(0),
    mode(ESPNOW_MODE_DIRECT),
    txDone(false),
    lastStatus(ESP_NOW_SEND_SUCCESS),
    lastAckReceived(false),
    passphrase(""),
    encryptionEnabled(false),
    deviceCount(0),
    unpairedDeviceCount(0),
    streamTarget(nullptr),
    streamActive(false),
    streamingSuspended(false),
    streamDroppedCount(0),
    streamSentCount(0),
    streamReceivedCount(0),
    lastStreamSendTime(0),
    fileAckLast(0),
    listBuffer(nullptr),
    nextMessageId(1),
    queueSize(0),
    globalMessageSeqNum(0),
    receiveErrors(0),
    heartbeatsSent(0),
    heartbeatsReceived(0),
    meshForwards(0),
    fileTransfersSent(0),
    fileTransfersReceived(0),
    lastResetTime(0),
    heartbeatPublic(true),
    deviceName(""),
    lastRemoteCapValid(false),
    lastRemoteCapTime(0),
#if ENABLE_BONDED_MODE
    bondHeartbeatsSent(0),
    bondHeartbeatsReceived(0),
    lastBondHeartbeatSentMs(0),
    lastBondHeartbeatReceivedMs(0),
    bondPeerOnline(false),
    bondPeerBootCounter(0),
    bondPeerSettingsHash(0),
    bondLocalSettingsHash(0),
    bondPeerUptime(0),
    bondLastOfflineMs(0),
    bondSyncInFlight(BOND_SYNC_NONE),
    bondSyncRetryCount(0),
    bondSyncLastAttemptMs(0),
    bondNeedsCapabilityRequest(false),
    bondNeedsStreamingSetup(false),
    bondDeferredStreamCtrlPending(false),
    bondDeferredStreamCtrlSensor(0),
    bondDeferredStreamCtrlEnable(0),
    bondNeedsCapabilityResponse(false),
    bondNeedsManifestResponse(false),
    bondNeedsSettingsResponse(false),
    bondReceivedCapability(false),
    bondCapSent(false),
    bondManifestReceived(false),
    bondSettingsReceived(false),
    bondSettingsSent(false),
    bondSessionTokenValid(false),
    bondRssiLast(-100),
    bondRssiAvg(-100),
    bondLastRssiUpdateMs(0),
    bondPeerStatusValid(false),
    bondPeerStatusTimeMs(0),
    bondNeedsStatusResponse(false),
    bondNeedsProactiveStatus(false),
    bondLastStatusReqMs(0),
#endif
    bondNeedsMetadataResponse(false),
    deferredMetadataPending(false),
    textQueueHead(0),
    textQueueTail(0),
    deferredCmdRespPending(false),
    deferredCmdRespResult(nullptr),
    deferredCmdRespSuccess(false),
    streamQueueHead(0),
    streamQueueTail(0),
    deferredCmdPending(false),
    deferredCmdMsgId(0)
  {
    memset(derivedKey, 0, 16);
#if ENABLE_BONDED_MODE
    memset(bondPendingResponseMac, 0, 6);
    memset(bondSessionToken, 0, 16);
    memset(&bondPeerStatus, 0, sizeof(bondPeerStatus));
#endif
    memset(metadataPendingResponseMac, 0, 6);
    // Initialize deferred message buffers
    memset(textQueue, 0, sizeof(textQueue));
    memset(deferredMetadataSrcMac, 0, 6);
    memset(deferredMetadataPayload, 0, sizeof(deferredMetadataPayload));
    memset(deferredCmdRespSrcMac, 0, 6);
    memset(deferredCmdRespDeviceName, 0, sizeof(deferredCmdRespDeviceName));
    // deferredCmdRespResult is a pointer — zeroed after ps_alloc in initEspNow
    memset(streamQueue, 0, sizeof(streamQueue));
    memset(deferredCmdSrcMac, 0, 6);
    memset(deferredCmdDeviceName, 0, sizeof(deferredCmdDeviceName));
    memset(deferredCmdPayload, 0, sizeof(deferredCmdPayload));
    memset(devices, 0, sizeof(devices));
    memset(unpairedDevices, 0, sizeof(unpairedDevices));
    memset(fileAckHashExpected, 0, 16);
    // listBuffer is a pointer — zeroed after ps_alloc in initEspNow
    memset(&lastRemoteCap, 0, sizeof(lastRemoteCap));
  }
};

// ==========================
// Global Variables (defined in espnow_system.cpp or main .ino)
// ==========================
// Note: These are declared as static in their respective files
// Access through public API functions instead of direct access

// ==========================
// Public API Functions
// ==========================

// Initialization
const char* checkEspNowFirstTimeSetup();

// Maintenance functions
void cleanupTimedOutChunks();
void saveMeshPeers();

// Device management
String getEspNowDeviceName(const uint8_t* mac);
void removeEspNowDevice(const uint8_t* mac);
// Note: addEspNowDevice is now static (internal) in espnow_system.cpp

// Command functions
const char* cmd_espnow_status(const String& cmd);
const char* cmd_espnow_stats(const String& cmd);
const char* cmd_espnow_routerstats(const String& cmd);
const char* cmd_espnow_resetstats(const String& cmd);
const char* cmd_espnow_init(const String& cmd);
const char* cmd_espnow_pair(const String& cmd);
const char* cmd_espnow_unpair(const String& cmd);
const char* cmd_espnow_list(const String& cmd);
const char* cmd_espnow_meshstatus(const String& cmd);
const char* cmd_espnow_send(const String& cmd);
const char* cmd_espnow_broadcast(const String& cmd);
const char* cmd_espnow_sendfile(const String& cmd);
const char* cmd_espnow_remote(const String& cmd);
const char* cmd_espnow_startstream(const String& originalCmd);
const char* cmd_espnow_stopstream(const String& originalCmd);

// Test commands (defined in .ino)
const char* cmd_test_streams(const String& cmd);
const char* cmd_test_concurrent(const String& cmd);
const char* cmd_test_cleanup(const String& cmd);
const char* cmd_test_filelock(const String& cmd);

// Configuration
const char* cmd_espnow_mode(const String& originalCmd);
const char* cmd_espnow_setname(const String& originalCmd);
const char* cmd_espnow_hbmode(const String& originalCmd);

// Streaming
void sendEspNowStreamMessage(const String& message);

// Mesh
const char* cmd_espnow_meshrole(const String& originalCmd);
const char* cmd_espnow_meshmaster(const String& originalCmd);
const char* cmd_espnow_meshbackup(const String& originalCmd);
const char* cmd_espnow_backupenable(const String& originalCmd);
const char* cmd_espnow_meshtopo(const String& originalCmd);
const char* cmd_espnow_toporesults(const String& cmd);

// Encryption
const char* cmd_espnow_setpassphrase(const String& originalCmd);
const char* cmd_espnow_encstatus(const String& cmd);
const char* cmd_espnow_pairsecure(const String& originalCmd);
void deriveKeyFromPassphrase(const String& passphrase, uint8_t* key);

// Time sync
const char* cmd_espnow_timesync(const String& originalCmd);
const char* cmd_espnow_timestatus(const String& cmd);

// Helper functions
String formatMacAddress(const uint8_t* mac);
const char* getMeshRoleString(uint8_t role);
uint32_t generateMessageId();
bool shouldChunk(size_t size);
// Helper to resolve device name or MAC address to MAC bytes
bool resolveDeviceNameOrMac(const String& nameOrMac, uint8_t* outMac);

// Message router (internal - do not call directly)

// Callbacks
void onEspNowDataRecv(const esp_now_recv_info_t* recvInfo, const uint8_t* data, int len);
void onEspNowDataSent(const uint8_t* mac, esp_now_send_status_t status);

// Message queue processing (called from loop)
void processMessageQueue();
void cleanupExpiredChunkedMessage();
void cleanupTimedOutChunks();
void cleanupExpiredBufferedPeers();

// ESP-NOW command functions (cmd_espnow_stopstream implemented in espnow_system.cpp)
const char* cmd_espnow_send(const String& cmd);
const char* cmd_espnowenabled(const String& cmd);

// Global state (defined in espnow_system.cpp)
extern EspNowState* gEspNow;

// Helper functions (exported for other modules)
bool macEqual6(const uint8_t a[6], const uint8_t b[6]);

// Inline helpers
inline const char* getEspNowModeString() {
  return (gEspNow && gEspNow->mode == ESPNOW_MODE_MESH) ? "mesh" : "direct";
}

inline bool isMeshMode() {
  return gEspNow && gEspNow->mode == ESPNOW_MODE_MESH;
}

inline bool meshEnabled() {
  return gEspNow && gEspNow->initialized && gEspNow->mode == ESPNOW_MODE_MESH;
}

// Mesh peer state (used by OLED and status views)
extern MeshPeerHealth* gMeshPeers;   // Dynamically allocated [gMeshPeerSlots] at init

// Mesh helper utilities (moved from .ino)
bool isMeshPeerAlive(const MeshPeerHealth* peer);
MeshPeerHealth* getMeshPeerHealth(const uint8_t mac[6], bool createIfMissing = false);
// Inline: check if MAC is this device
inline bool isSelfMac(const uint8_t* mac) {
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  return memcmp(mac, myMac, 6) == 0;
}
void macFromHexString(const String& s, uint8_t out[6]);
String macToHexString(const uint8_t mac[6]);
// Zero-allocation MAC formatter for debug log call sites.
// Uses a GCC statement expression + stack char[18] — no heap, no String.
// Usage: DEBUGF(..., "%s", MAC_STR(mac))  instead of  macToHexString(mac).c_str()
#define MAC_STR(mac) \
  ({ static char _macbuf[18]; \
     snprintf(_macbuf, sizeof(_macbuf), "%02X:%02X:%02X:%02X:%02X:%02X", \
       (mac)[0],(mac)[1],(mac)[2],(mac)[3],(mac)[4],(mac)[5]); \
     (const char*)_macbuf; })

// Topology helpers (moved from .ino)
void requestTopologyDiscovery();
void sendTopologyResponse(uint32_t reqId, const uint8_t masterMac[6], JsonArray requestPath);
void checkTopologyCollectionWindow();  // Check collection window and finalize when timeout expires

// Mesh envelope sender (for remote sensor broadcasting)
void meshSendEnvelopeToPeers(const String& envelope);

// Topology state (for auto-discovery check in loop)
extern uint32_t gLastTopoRequest;

// Mesh message builders (for heartbeat in loop)
String buildHeartbeat(uint32_t msgId, const char* src);
String buildBootNotification(uint32_t msgId, const char* src, uint32_t bootCounter, uint32_t timestamp);

// V2 envelope builder (for user sync and other features)
void v2_init_envelope(JsonDocument& doc, const char* type, uint32_t msgId, const char* src, const char* dst, int ttl);
uint32_t generateMessageId();

// Mesh heartbeat processing (FreeRTOS task)
extern bool gMeshActivitySuspended;  // Suspend mesh during HTTP requests
void processMeshHeartbeats();  // Internal worker function (called by task)
bool startEspNowTask();        // Start ESP-NOW heartbeat task
void stopEspNowTask();         // Stop ESP-NOW heartbeat task
TaskHandle_t getEspNowTaskHandle();  // Get task handle for stack monitoring

// Message handling (for command execution)
void sendChunkedResponse(const uint8_t* targetMac, bool success, const String& result, const String& senderName);
void cleanupExpiredChunkedMessage();  // Cleanup timed-out chunked messages

// Per-device message buffer management (espnow_message_buffer.cpp)
PeerMessageHistory* findOrCreatePeerHistory(uint8_t* peerMac);
bool storeMessageInPeerHistory(uint8_t* peerMac, const char* peerName, const char* message, bool encrypted, LogMessageType msgType);
void logFileTransferEvent(uint8_t* peerMac, const char* peerName, const char* filename, LogMessageType eventType);
int getPeerMessages(uint8_t* peerMac, ReceivedTextMessage* outMessages, int maxMessages, uint32_t sinceSeq);
int getAllMessages(ReceivedTextMessage* outMessages, int maxMessages, uint32_t sinceSeq);

// File transfer to specific MAC (used by ImageManager)
bool sendFileToMac(const uint8_t* mac, const String& localPath);

#if ENABLE_BONDED_MODE
// V3 binary sensor data streaming for bond mode (worker → master)
bool sendBondedSensorData(uint8_t sensorType, const uint8_t* data, uint16_t dataLen);

// Check if bond mode is active and peer is online
bool isBondModeOnline();
// Check if bond mode is active, peer is online, AND fully synced (cap + manifest + settings)
bool isBondSynced();
#else
// Stubs when bond mode is compiled out
inline bool sendBondedSensorData(uint8_t, const uint8_t*, uint16_t) { return false; }
inline bool isBondModeOnline() { return false; }
inline bool isBondSynced() { return false; }
#endif // ENABLE_BONDED_MODE

// V3 frame sending (for remote command execution from System_Utils)
bool v3_send_frame(const uint8_t* dst, uint8_t type, uint8_t flags, uint32_t msgId,
                   const uint8_t* payload, uint16_t payloadLen, uint8_t ttl);

#else // !ENABLE_ESPNOW

// Stubs for functions called from other modules when ESP-NOW is disabled
inline bool resolveDeviceNameOrMac(const String& nameOrMac, uint8_t* outMac) { 
  (void)nameOrMac; (void)outMac; 
  return false; 
}
inline bool sendFileToMac(const uint8_t* mac, const String& localPath) { 
  (void)mac; (void)localPath; 
  return false; 
}

#endif // ENABLE_ESPNOW

#endif // ESPNOW_SYSTEM_H
