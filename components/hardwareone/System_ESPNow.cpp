/**
 * ESP-NOW System - COMPLETE Implementation
 * Extracted from HardwareOne.ino
 */

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <time.h>
#include <ArduinoJson.h>
#include <esp_wifi.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

#include "OLED_Display.h"
#include "OLED_UI.h"
#include "System_Command.h"
#include "System_Notifications.h"
#include "System_Debug.h"
#include "System_ESPNow.h"
#include "System_ESPNow_Sensors.h"
#include "System_MemUtil.h"
#include "System_MemoryMonitor.h"
#include "System_Mutex.h"
#include "System_SensorStubs.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "System_UserSettings.h"
#include "System_Utils.h"
#include "System_I2C.h"  // For ConnectedDevice struct
#include "System_Filesystem.h"  // For canRead() security check
#if ENABLE_HTTP_SERVER
#include "WebServer_Server.h"
#endif

#if ENABLE_IMU_SENSOR
#include "i2csensor-bno055.h"
#endif
#if ENABLE_THERMAL_SENSOR
#include "i2csensor-mlx90640.h"
#endif

// External dependencies from main .ino - now in espnow_system.h
extern bool isValidUser(const String& username, const String& password);
extern bool isAdminUser(const String& username);
extern void printToWeb(const String& s);
extern void printToSerial(const String& s);
extern volatile uint32_t gOutputFlags;
extern bool gCLIValidateOnly;
// gDebugBuffer, gDebugFlags, ensureDebugBuffer now from debug_system.h

// Debug flags (defined in .ino)
// Removed local DEBUG_HTTP define; use debug_system.h as single source of truth

// Deduplication is MANDATORY - no runtime toggle (required for mesh flood forwarding)
struct MsgDedupEntry { uint8_t src[6]; uint32_t id; uint32_t ts; bool active; };
#define MSG_DEDUP_SIZE 32
static MsgDedupEntry gMsgDedup[MSG_DEDUP_SIZE];
static int gMsgDedupIdx = 0;

// Ack wait table (small, lock-free)
struct MsgAckWait { uint32_t id; volatile bool got; uint32_t ts; bool active; };
#define MSG_ACK_WAIT_MAX 8
static MsgAckWait gMsgAckWait[MSG_ACK_WAIT_MAX];
static int msgAckWaitRegister(uint32_t id) {
  // reuse slot with same id if present
  for (int i = 0; i < MSG_ACK_WAIT_MAX; i++) {
    if (gMsgAckWait[i].active && gMsgAckWait[i].id == id) {
      gMsgAckWait[i].got = false; gMsgAckWait[i].ts = millis();
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[ACK_WAIT] Reusing slot %d for msgId=%lu", i, (unsigned long)id);
      return i;
    }
  }
  for (int i = 0; i < MSG_ACK_WAIT_MAX; i++) {
    if (!gMsgAckWait[i].active) {
      gMsgAckWait[i].active = true;
      gMsgAckWait[i].id = id;
      gMsgAckWait[i].got = false;
      gMsgAckWait[i].ts = millis();
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[ACK_WAIT] Registered slot %d for msgId=%lu", i, (unsigned long)id);
      return i;
    }
  }
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[ACK_WAIT] ERROR: All slots full, cannot register msgId=%lu", (unsigned long)id);
  return -1;
}
static bool msgAckWaitBlock(uint32_t id, uint32_t timeoutMs) {
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[ACK_WAIT] Blocking for msgId=%lu timeout=%lums", (unsigned long)id, (unsigned long)timeoutMs);
  unsigned long start = millis();
  while ((millis() - start) < timeoutMs) {
    for (int i = 0; i < MSG_ACK_WAIT_MAX; i++) {
      if (gMsgAckWait[i].active && gMsgAckWait[i].id == id && gMsgAckWait[i].got) {
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[ACK_WAIT] ACK received for msgId=%lu after %lums", (unsigned long)id, millis() - start);
        return true;
      }
    }
    // Use vTaskDelay to properly yield to RTOS scheduler and feed watchdog
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[ACK_WAIT] TIMEOUT waiting for msgId=%lu after %lums", (unsigned long)id, (unsigned long)timeoutMs);
  return false;
}

// General system functions from .ino (non-ESP-NOW specific)
extern AuthContext gExecAuthContext;
// Note: handleFileTransferMessage and sendTopologyResponse are static in this file, not extern

// ============================================================================
// ESP-NOW Global State (owned by this module)
// ============================================================================
EspNowState* gEspNow = nullptr;  // Allocated on-demand when ESP-NOW is initialized

// Forward declarations for ESP-NOW helper functions (implemented below)
// RX processing functions
static void onEspNowDataReceived(const esp_now_recv_info* recv_info, const uint8_t* incomingData, int len);
static void onEspNowRawRecv(const esp_now_recv_info* recv_info, const uint8_t* data, int len);

// ============================================================================
// ESP-NOW V3 Binary Protocol - Forward Declarations
// ============================================================================
#define ESPNOW_V3_MAGIC 0x3148u
#define ESPNOW_V3_MAX_PAYLOAD (250 - 24)  // 226 bytes max payload

enum EspNowV3Type : uint8_t {
  ESPNOW_V3_TYPE_ACK          = 1,
  ESPNOW_V3_TYPE_BOND_CAP_REQ = 2,
  ESPNOW_V3_TYPE_BOND_CAP_RESP= 3,
  ESPNOW_V3_TYPE_TEXT         = 4,
  ESPNOW_V3_TYPE_CMD          = 5,
  ESPNOW_V3_TYPE_CMD_RESP     = 6,
  ESPNOW_V3_TYPE_HEARTBEAT    = 7,
  ESPNOW_V3_TYPE_FILE_START   = 8,
  ESPNOW_V3_TYPE_FILE_DATA    = 9,
  ESPNOW_V3_TYPE_FILE_END     = 10,
  ESPNOW_V3_TYPE_MANIFEST_REQ = 11,
  ESPNOW_V3_TYPE_MANIFEST_RESP= 12,
  ESPNOW_V3_TYPE_STREAM       = 13,
  ESPNOW_V3_TYPE_BOND_HEARTBEAT = 14,
  ESPNOW_V3_TYPE_SENSOR_DATA  = 15,  // Binary sensor data (bond mode)
  ESPNOW_V3_TYPE_SETTINGS_REQ = 16,  // Request settings from bonded device
  ESPNOW_V3_TYPE_SETTINGS_RESP= 17,  // Settings response (JSON payload)
  ESPNOW_V3_TYPE_SETTINGS_PUSH= 18,  // RESERVED (push removed — settings changes use remote commands)
  ESPNOW_V3_TYPE_METADATA_REQ = 19,  // Request peer's metadata
  ESPNOW_V3_TYPE_METADATA_RESP= 20,  // Metadata response
  ESPNOW_V3_TYPE_METADATA_PUSH= 21,  // Push metadata update (when changed)
  ESPNOW_V3_TYPE_TIME_SYNC    = 22,  // Time synchronization (epoch + millis)
  ESPNOW_V3_TYPE_TOPO_REQ     = 23,  // Topology discovery request
  ESPNOW_V3_TYPE_TOPO_START   = 24,  // Topology response start (peer count)
  ESPNOW_V3_TYPE_TOPO_PEER    = 25,  // Topology response peer entry
  ESPNOW_V3_TYPE_USER_SYNC    = 26,  // User data synchronization
  ESPNOW_V3_TYPE_WORKER_STATUS= 27,  // Worker status report to master (detailed)
  ESPNOW_V3_TYPE_SENSOR_STATUS= 28,  // Sensor status broadcast (enabled/disabled)
  ESPNOW_V3_TYPE_SENSOR_BROADCAST= 29, // Sensor data broadcast to mesh
  ESPNOW_V3_TYPE_BOND_STATUS_REQ = 30, // Request live status from bonded peer
  ESPNOW_V3_TYPE_BOND_STATUS_RESP= 31, // Live status response (BondPeerStatus payload)
  ESPNOW_V3_TYPE_STREAM_CTRL     = 32, // Stream control (master->worker: start/stop sensor streaming)
};

struct __attribute__((packed)) V3PayloadHeartbeat {
  uint8_t role;
  uint8_t peerCount;
  int8_t  rssi;
  uint8_t reserved;
  uint32_t uptimeSec;
  uint32_t freeHeap;
  char deviceName[20];
};

// Time sync payload
struct __attribute__((packed)) V3PayloadTimeSync {
  uint32_t epochTime;     // Unix epoch time
  int64_t timeOffset;     // Time offset in milliseconds
  uint32_t senderUptime;  // Sender uptime in seconds
};

// Topology request payload
struct __attribute__((packed)) V3PayloadTopoReq {
  uint32_t reqId;       // Request ID for correlation
  uint8_t reserved[4];  // Padding for alignment
};

// Topology start payload (first message in topology response)
struct __attribute__((packed)) V3PayloadTopoStart {
  uint32_t reqId;       // Matches request ID
  uint8_t peerCount;    // Number of TOPO_PEER messages to follow
  uint8_t reserved[3];
};

// Topology peer entry (one per peer)
struct __attribute__((packed)) V3PayloadTopoPeer {
  uint32_t reqId;       // Matches request ID
  uint8_t peerIndex;    // Which peer (0-based)
  uint8_t isLast;       // 1 if this is the last peer
  uint8_t mac[6];       // Peer MAC
  int8_t rssi;          // Last RSSI
  uint8_t encrypted;    // 1 if encrypted
  char name[32];        // Peer name
};

// Worker status payload (detailed, for master consumption)
struct __attribute__((packed)) V3PayloadWorkerStatus {
  uint32_t freeHeap;
  uint32_t totalHeap;
  int8_t rssi;
  uint8_t thermalEnabled;
  uint8_t imuEnabled;
  uint8_t reserved;
  char name[20];
  // Metadata fields follow as variable JSON payload if needed
};

#if ENABLE_BONDED_MODE
// Bond mode heartbeat payload (lightweight)
struct __attribute__((packed)) V3PayloadBondHeartbeat {
  uint8_t role;           // Bond role (master/worker)
  int8_t  rssi;           // WiFi RSSI
  uint8_t reserved[2];    // Padding for alignment
  uint32_t uptimeSec;     // Uptime in seconds
  uint32_t freeHeap;      // Free heap bytes
  uint32_t seqNum;        // Sequence number for tracking
  uint32_t bootCounter;   // Persistent boot counter
  uint32_t settingsHash;  // Hash of local settings (exclude passwords)
};
#endif // ENABLE_BONDED_MODE

#if ENABLE_BONDED_MODE
// Binary sensor data payload for bond mode streaming (compact, no JSON overhead)
// Max payload is 226 bytes, header is 8 bytes, leaving 218 bytes for sensor data
struct __attribute__((packed)) V3PayloadSensorData {
  uint8_t sensorType;     // RemoteSensorType enum value
  uint8_t flags;          // Bit 0: valid, Bit 1: streaming enabled
  uint16_t dataLen;       // Length of data[] that follows
  uint32_t seqNum;        // Sequence number for ordering
  uint8_t data[];         // Variable-length sensor data (flexible array member)
};
// Stream control payload for bond mode (master -> worker)
struct __attribute__((packed)) V3PayloadStreamCtrl {
  uint8_t sensorType;   // RemoteSensorType enum value
  uint8_t enable;       // 1 = start streaming, 0 = stop streaming
  uint8_t reserved[2];  // Padding
};
#endif // ENABLE_BONDED_MODE

// Sensor status payload for mesh broadcast
struct __attribute__((packed)) V3PayloadSensorStatus {
  uint8_t sensorType;   // RemoteSensorType enum value
  uint8_t enabled;      // 1 if enabled, 0 if disabled
  uint8_t reserved[2];  // Padding for alignment
};

// Sensor broadcast payload (sensor data to mesh)
struct __attribute__((packed)) V3PayloadSensorBroadcast {
  uint8_t sensorType;   // RemoteSensorType enum value
  uint16_t dataLen;     // Length of JSON data that follows
  uint8_t reserved;     // Padding for alignment
  uint8_t data[];       // Variable-length JSON data (flexible array member)
};

// Metadata payload for metadata exchange (REQ/RESP/PUSH)
// Total: 180 bytes (fits comfortably in 226 byte payload limit)
struct __attribute__((packed)) V3PayloadMetadata {
  char deviceName[32];
  char friendlyName[48];
  char room[32];
  char zone[32];
  char tags[64];
  uint8_t stationary;
  uint8_t reserved[3];    // Padding for future fields
};

// ============================================================================
// V3 FRAGMENTATION CONSTANTS AND REASSEMBLY
// ============================================================================
#define V3_MAX_FRAGMENT_PAYLOAD 200   // Max payload bytes per fragment
#define V3_FRAG_MAX             16    // Max fragments per message (max msg = 3200 bytes)
#define V3_REASM_MAX            2     // Max concurrent reassembly contexts
#define V3_REASM_TIMEOUT_MS     5000  // Reassembly context GC timeout (ms)
#define V3_FRAG_ACK_WAIT_MAX    8     // Max concurrent fragment ACK waiters

struct V3ReasmEntry {
  bool     active;
  uint8_t  src[6];
  uint32_t msgId;
  uint8_t  type;
  uint8_t  fragCount;
  uint8_t  received;
  bool     have[V3_FRAG_MAX];
  uint8_t  buffer[V3_FRAG_MAX * V3_MAX_FRAGMENT_PAYLOAD];
  uint16_t bufferSize;
  uint32_t lastUpdateMs;
};

static V3ReasmEntry* gV3Reasm = nullptr;  // Allocated in PSRAM at init (~6.4KB)

static void v3_reasm_reset(V3ReasmEntry& e) {
  e.active   = false;
  e.msgId    = 0;
  e.received = 0;
  e.fragCount = 0;
  memset(e.src,  0, 6);
  memset(e.have, 0, sizeof(e.have));
}

static void v3_reasm_gc(uint32_t nowMs) {
  if (!gV3Reasm) return;
  for (int i = 0; i < V3_REASM_MAX; i++) {
    if (gV3Reasm[i].active && (nowMs - gV3Reasm[i].lastUpdateMs) > V3_REASM_TIMEOUT_MS) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_REASM_GC] Evicting stale msgId=%lu from %02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned long)gV3Reasm[i].msgId,
             gV3Reasm[i].src[0], gV3Reasm[i].src[1], gV3Reasm[i].src[2],
             gV3Reasm[i].src[3], gV3Reasm[i].src[4], gV3Reasm[i].src[5]);
      if (gEspNow) { gEspNow->routerMetrics.v3FragRxGc++; }
      v3_reasm_reset(gV3Reasm[i]);
    }
  }
}

static V3ReasmEntry* v3_reasm_find_or_alloc(const uint8_t* src, uint32_t msgId, uint8_t type, uint8_t fragCount) {
  if (!gV3Reasm) return nullptr;
  for (int i = 0; i < V3_REASM_MAX; i++) {
    if (gV3Reasm[i].active && gV3Reasm[i].msgId == msgId &&
        memcmp(gV3Reasm[i].src, src, 6) == 0) {
      return &gV3Reasm[i];
    }
  }
  for (int i = 0; i < V3_REASM_MAX; i++) {
    if (!gV3Reasm[i].active) {
      v3_reasm_reset(gV3Reasm[i]);
      memcpy(gV3Reasm[i].src, src, 6);
      gV3Reasm[i].msgId       = msgId;
      gV3Reasm[i].type        = type;
      gV3Reasm[i].fragCount   = (fragCount <= V3_FRAG_MAX) ? fragCount : V3_FRAG_MAX;
      gV3Reasm[i].bufferSize  = (uint16_t)(gV3Reasm[i].fragCount * V3_MAX_FRAGMENT_PAYLOAD);
      gV3Reasm[i].lastUpdateMs = millis();
      gV3Reasm[i].active      = true;
      return &gV3Reasm[i];
    }
  }
  return nullptr;
}

struct V3FragAckWait {
  bool     active;
  uint32_t msgId;
  uint8_t  fragIndex;
  uint8_t  dstMac[6];
  volatile bool acked;   // Set true by RX ACK handler; polled by TX task
  uint32_t sentMs;
};

static V3FragAckWait gV3FragAckWait[V3_FRAG_ACK_WAIT_MAX];

static V3FragAckWait* v3_frag_ack_alloc(const uint8_t* dst, uint32_t msgId, uint8_t fragIdx) {
  for (int i = 0; i < V3_FRAG_ACK_WAIT_MAX; i++) {
    if (!gV3FragAckWait[i].active) {
      gV3FragAckWait[i].msgId     = msgId;
      gV3FragAckWait[i].fragIndex = fragIdx;
      gV3FragAckWait[i].acked     = false;
      gV3FragAckWait[i].sentMs    = 0;
      memcpy(gV3FragAckWait[i].dstMac, dst, 6);
      gV3FragAckWait[i].active    = true;
      return &gV3FragAckWait[i];
    }
  }
  return nullptr;
}

#if ENABLE_BONDED_MODE
// Paired heartbeat constants
static const uint32_t BOND_HEARTBEAT_INTERVAL_MS = 5000;   // Send every 5 seconds
static const uint32_t BOND_HEARTBEAT_TIMEOUT_MS = 15000;   // Peer offline after 15s no heartbeat
static uint32_t gLastBondHeartbeatSentMs = 0;
static uint32_t gBondHeartbeatSeqNum = 0;
static const uint32_t BOND_SYNC_RETRY_MS = 3000;   // Retry sync request every 3s if stuck

static void resetBondSync();
#endif

// Forward declaration for v3_send_frame (implemented later)
bool v3_send_frame(const uint8_t* dst, uint8_t type, uint8_t flags, uint32_t msgId,
                   const uint8_t* payload, uint16_t payloadLen, uint8_t ttl);

// Forward declarations for V3 helper functions (non-static for external linkage)
bool v3_broadcast(uint8_t type, uint8_t flags, uint32_t msgId, const uint8_t* payload, uint16_t payloadLen, uint8_t ttl);
bool v3_send_chunked(const uint8_t* dst, uint8_t type, uint8_t flags, uint32_t msgId, const uint8_t* payload, uint16_t payloadLen, uint8_t ttl);
bool v3_broadcast_topo_request(uint32_t reqId);
bool v3_send_worker_status(const uint8_t* dst, const V3PayloadWorkerStatus& status, const char* jsonMetadata, uint16_t jsonLen);
bool v3_send_command_response(const uint8_t* dst, uint32_t cmdMsgId, bool success, const char* resultText, uint16_t textLen);
bool v3_send_text(const uint8_t* dst, const char* text, uint16_t textLen);
bool v3_broadcast_text(const char* text, uint16_t textLen);
bool v3_broadcast_sensor_status(RemoteSensorType sensorType, bool enabled);
bool v3_broadcast_sensor_data(RemoteSensorType sensorType, const char* jsonData, uint16_t jsonLen);
bool v3_send_user_sync(const uint8_t* dst, const char* jsonPayload, uint16_t jsonLen);

// MAC address formatting (stack buffer version to reduce heap churn)
void formatMacAddressBuf(const uint8_t* mac, char* buf, size_t bufSize);

// Forward declaration for internal addEspNowPeerWithEncryption (different from extern version in .ino)
static bool addEspNowPeerWithEncryption(const uint8_t* mac, bool useEncryption, const uint8_t* encryptionKey);

// Forward declarations for structures from main .ino
// AuthContext and CommandSource now in user_system.h
struct CommandContext;
// ConnectedDevice is now defined in System_I2C.h (included above)

// External globals for device tracking (defined in System_I2C.cpp)
extern ConnectedDevice connectedDevices[];
extern int connectedDeviceCount;

// Memory allocation helpers

// Note: FsLockGuard is now in mutex_system.h (included above)

// Settings struct is now in settings.h (included via espnow_system.h)

// File paths
static const char* ESPNOW_DEVICES_FILE = "/system/espnow_devices.json";
static const char* MESH_PEERS_FILE = "/system/mesh_peers.json";

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

// Note: gEspNow is defined in HardwareOne.ino as non-static (extern in header)

// Legacy compatibility accessors (to minimize code changes)
static uint32_t gMeshMsgCounter = 1;  // Message ID counter for mesh envelopes

std::vector<MeshTopoNode> gMeshTopology;        // Exported for .ino access
uint32_t gTopoRequestId = 0;                    // Exported for .ino access
uint32_t gTopoRequestTimeout = 0;               // Exported for .ino access
uint32_t gTopoLastResponseTime = 0;             // Exported for .ino access
int gTopoResponsesReceived = 0;                 // Exported for .ino access
int gExpectedWorkerCount = 0;                   // Exported for .ino access
const uint32_t TOPO_COLLECTION_WINDOW_MS = 3000;  // Exported for .ino access
uint32_t gLastTopoRequest = 0;                  // Exported for .ino access
String gTopoResultsBuffer = "";                 // Exported for .ino access

// Time synchronization state (exported for .ino access)
int64_t gTimeOffset = 0;           // Offset to add to millis() to get epoch time (milliseconds)
bool gTimeIsSynced = false;        // True if we have received time sync from master
unsigned long gLastTimeSyncMs = 0; // When we last synced time (millis())
extern const unsigned long TIME_SYNC_INTERVAL = 600000; // Broadcast time sync every 10 minutes (master only)

// Worker status configuration
struct WorkerStatusConfig {
  bool enabled = true;              // Master switch for worker status reporting
  uint16_t intervalMs = 30000;      // Send interval in milliseconds (default 30s)
  bool includeHeap = true;          // Include heap memory stats
  bool includeRssi = false;         // Include WiFi RSSI
  bool includeThermal = false;      // Include thermal sensor status
  bool includeImu = false;          // Include IMU sensor status
};
static WorkerStatusConfig gWorkerStatusConfig;

// Metadata transmission tracking
static bool gMetadataSentThisSession = false;  // Whether metadata has been sent since boot
static bool gMetadataChanged = false;          // Whether metadata changed since last send
static String gLastSentFriendlyName = "";
static String gLastSentRoom = "";
static String gLastSentZone = "";
static String gLastSentTags = "";
static bool gLastSentStationary = false;

// Master/Backup heartbeat tracking
static uint32_t gLastMasterHeartbeat = 0;
static uint32_t gLastBackupHeartbeat = 0;
static uint32_t gLastWorkerStatusReport = 0;
static bool gBackupPromoted = false;

// --------------------------
// Minimal mesh envelope support (fallback, unicast-only)
// --------------------------
// Note: MeshSeenEntry, MESH_DEDUP_SIZE, MeshPeerHealth, MESH_PEER_MAX, MESH_MAX_RETRIES are now in espnow_system.h

// Lightweight RX ring to defer heavy processing to espnowHeartbeatTask (no new task)
struct InboundRxItem {
  uint8_t src[6];
  int len;
  int8_t rssi;
  uint8_t data[250];
};
static volatile uint8_t gEspNowRxHead = 0;
static volatile uint8_t gEspNowRxTail = 0;
static InboundRxItem gEspNowRxRing[8];
static volatile uint32_t gEspNowRxDrops = 0;
MeshSeenEntry gMeshSeen[MESH_DEDUP_SIZE];  // Exported for .ino access
int gMeshSeenIndex = 0;                     // Exported for .ino access
// Forward declaration: accessor defined later in file
static bool isEspNowInitializedFlag();

int gMeshPeerSlots = 8;  // Runtime slot count, set from gSettings.meshPeerMax at init
MeshPeerHealth* gMeshPeers = nullptr;
MeshPeerMeta* gMeshPeerMeta = nullptr;
uint32_t gLastHeartbeatSentMs = 0;

static MeshRetryEntry gMeshRetryQueue[MESH_RETRY_QUEUE_SIZE];
// Note: gMeshRetryMutex is now defined in mutex_system.cpp
// Send flow control
[[maybe_unused]] static bool isEspNowInitializedFlag() {
  return gEspNow && gEspNow->initialized;
}
// NOTE: FILE_ACK_INTERVAL removed - v3 protocol doesn't use v2 ACK mechanism

// ESP-NOW chunked message support (for command responses only - file transfers use v3 binary protocol)
#define MAX_CHUNKS 20              // For remote command responses (buffered in RAM)
#define CHUNK_SIZE 200             // Generic message chunk size (fits ESP-NOW payload)
// NOTE: MAX_FILE_CHUNKS and FILE_CHUNK_DATA_BYTES removed - v3 uses binary frames
// Fixed-size receive buffer sized for MAX_CHUNKS * CHUNK_SIZE (only for RESULT messages)
#define MAX_RESULT_BYTES (MAX_CHUNKS * CHUNK_SIZE)  // 4000 bytes for command responses

struct ChunkedMessage {
  char hash[16];        // Fixed size (was String)
  char status[16];      // Fixed size (was String)
  char deviceName[32];  // Fixed size (was String)
  int totalChunks;
  int totalLength;  // expected total bytes for the result
  int receivedChunks;
  char buffer[MAX_RESULT_BYTES];  // static buffer for reassembly
  unsigned long startTime;
  bool active;
};

// Allocated only when ESP-NOW is initialized (saves ~2KB when not in use)
static ChunkedMessage* gActiveMessage = nullptr;

// ESP-NOW file transfer support
struct FileTransfer {
  char filename[64];        // Destination filename
  uint32_t totalSize;       // Total file size in bytes
  uint32_t receivedBytes;   // Bytes received so far
  uint16_t totalChunks;     // Expected number of chunks
  uint16_t receivedChunks;  // Chunks received so far
  uint16_t chunkSize;
  char hash[16];            // Transfer hash for validation
  unsigned long startTime;  // Transfer start time
  bool active;              // Transfer in progress
  uint8_t senderMac[6];     // MAC address of sender
  uint8_t* dataBuffer;      // PSRAM buffer for incoming file data (avoids callback filesystem I/O)
  uint32_t bufferSize;      // Size of allocated buffer
  uint8_t* chunkMap;
  uint16_t chunkMapBytes;
};
static FileTransfer* gActiveFileTransfer = nullptr;
// NOTE: gActiveFileTransferFile removed - v3 protocol buffers in PSRAM and writes at end

// ============================================================================
// ESP-NOW Mesh Chunking Pattern (New Architecture)
// ============================================================================
// This section implements the new chunking pattern with:
// - Multiple concurrent streams (topology: 4, files: 1 with lock)
// - Explicit completion via isLast field on every message
// - Sender identification via (senderMac, reqId) tuple
// - Automatic cleanup of stale streams
// ============================================================================

// ESP-NOW topology streaming support (NEW PATTERN - Multiple Concurrent Streams)
// Note: TopologyStream, TopoDeviceEntry, BufferedPeerMessage structs and constants are now in espnow_system.h
static TopologyStream gTopoStreams[MAX_CONCURRENT_TOPO_STREAMS];  // Array of concurrent streams
static TopoDeviceEntry gTopoDeviceCache[MAX_TOPO_DEVICE_CACHE];
static BufferedPeerMessage gPeerBuffer[MAX_BUFFERED_PEERS];

// ESP-NOW file transfer support (NEW PATTERN - Single Stream with Lock)
// Only one file transfer at a time, requires handshake to acquire lock
static bool gFileTransferLocked = false;           // File transfer lock state
static uint8_t gFileTransferOwnerMac[6] = {0};     // MAC of device holding the lock
static unsigned long gFileTransferLockTime = 0;    // When lock was acquired (for timeout)

// ESP-NOW output streaming support - MIGRATED TO gEspNow struct
static const unsigned long STREAM_MIN_INTERVAL_MS = 100;  // Rate limit: max 10 messages/second
bool gMeshActivitySuspended = false;               // Suspend mesh heartbeats during HTTP requests (exported to web_server.cpp)

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// MAC address comparison (exported for oled_display.cpp)
bool macEqual6(const uint8_t a[6], const uint8_t b[6]) {
  return memcmp(a, b, 6) == 0;
}

// V2 reliability toggles (ack/dedup)
const char* cmd_espnow_rel(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String args = argsIn;
  args.trim();
  if (args.length() == 0) {
    return "Reliability status: ACK=on, dedup=on (both MANDATORY - v2 protocol)";
  }
  // Reliability (ACK+dedup) is MANDATORY and cannot be disabled in v2 protocol
  return "Reliability (ACK+dedup) is MANDATORY and always enabled for robust operation.";
}

// Save mesh peer MAC addresses to filesystem (topology only, not health metrics)
// This is called only when topology changes (new peer discovered, peer removed, mode change)
// Health metrics (timestamps, counters) rebuild naturally from heartbeats after reboot
void saveMeshPeers() {
  extern bool filesystemReady;
  if (!filesystemReady) return;

  // Pause sensor polling during file I/O
  extern volatile bool gSensorPollingPaused;
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;

  FsLockGuard fsGuard("mesh.peers.save");
  File file = LittleFS.open(MESH_PEERS_FILE, "w");
  if (!file) {
    gSensorPollingPaused = wasPaused;
    return;
  }

  file.println("{");
 
  file.println("  \"peers\": [");

  int count = 0;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (!gMeshPeers[i].isActive || isSelfMac(gMeshPeers[i].mac)) continue;  // Don't save self-entry
    
    if (count > 0) file.println(",");
    
    String peerName = "";
    if (gEspNow) {
      for (int j = 0; j < gEspNow->deviceCount; j++) {
        if (memcmp(gEspNow->devices[j].mac, gMeshPeers[i].mac, 6) == 0) {
          peerName = gEspNow->devices[j].name;
          break;
        }
      }
    }
    file.print("    {\"mac\": \"");
    file.print(macToHexString(gMeshPeers[i].mac));
    file.print("\"");
    if (peerName.length() > 0) {
      file.print(", \"name\": \"");
      file.print(peerName);
      file.print("\"");
    }
    file.print("}");
    
    count++;
  }

  file.println();
  file.println("  ]");
  file.println("}");
  file.close();
  
  gSensorPollingPaused = wasPaused;
  
  DEBUGF(DEBUG_ESPNOW_MESH, "[MESH] Saved role=%s, %d peer MAC addresses to filesystem", 
                getMeshRoleString(gSettings.meshRole), count);
}

// Save named ESP-NOW devices (paired devices with names/keys) to filesystem
static void saveEspNowDevices() {
  if (!gEspNow) return;
  extern bool filesystemReady;
  if (!filesystemReady) return;

  FsLockGuard fsGuard("espnow.devices.save");
  File f = LittleFS.open(ESPNOW_DEVICES_FILE, "w");
  if (!f) return;

  f.println("{");
  f.println("  \"devices\": [");
  int count = 0;
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (isSelfMac(gEspNow->devices[i].mac)) continue;
    if (count > 0) f.println(",");
    f.print("    {\"mac\":\"");
    f.print(formatMacAddress(gEspNow->devices[i].mac));
    f.print("\",\"name\":\"");
    f.print(gEspNow->devices[i].name);
    f.print("\",\"encrypted\":");
    f.print(gEspNow->devices[i].encrypted ? "true" : "false");
    if (gEspNow->devices[i].encrypted) {
      f.print(",\"key\":\"");
      for (int k = 0; k < 16; k++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", gEspNow->devices[i].key[k]);
        f.print(hex);
      }
      f.print("\"");
    }
    f.print("}");
    count++;
  }
  f.println();
  f.println("  ]");
  f.println("}");
  f.close();
  DEBUGF(DEBUG_ESPNOW_MESH, "[ESPNOW] Saved %d device(s) to %s", count, ESPNOW_DEVICES_FILE);
}

// Load mesh peer MAC addresses from filesystem (topology only)
// Forward declaration for parseMacAddress (defined later in file)
bool parseMacAddress(const String& macStr, uint8_t mac[6]);

// Health metrics (timestamps, counters) will be initialized to zero and rebuild from heartbeats
static void loadMeshPeers() {
  extern bool filesystemReady;
  if (!filesystemReady) return;

  // Pause sensor polling during file I/O
  extern volatile bool gSensorPollingPaused;
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;

  FsLockGuard fsGuard("mesh.peers.load");
  File file = LittleFS.open(MESH_PEERS_FILE, "r");
  if (!file) {
    gSensorPollingPaused = wasPaused;
    DEBUGF(DEBUG_ESPNOW_MESH, "[MESH] No saved peer list found");
    return;
  }

  String content = file.readString();
  file.close();
  
  // Resume sensor polling before parsing
  gSensorPollingPaused = wasPaused;

  // NOTE: mesh_peers.json is topology-only. Role/master/backup are persisted via settings.json.

  // Clear existing peers (except self-entry which will be recreated)
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac)) {
      gMeshPeers[i].isActive = false;
    }
  }

  // Simple JSON parsing for peer list
  int count = 0;
  int pos = 0;
  while ((pos = content.indexOf("\"mac\":", pos)) >= 0) {
    // Extract MAC address
    int macStart = content.indexOf("\"", pos + 6) + 1;
    int macEnd = content.indexOf("\"", macStart);
    if (macStart <= 0 || macEnd <= macStart) break;

    String macStr = content.substring(macStart, macEnd);
    uint8_t mac[6];
    if (!parseMacAddress(macStr, mac)) {
      pos = macEnd;
      continue;
    }

    // Don't load self-entry (will be created automatically)
    if (isSelfMac(mac)) {
      pos = macEnd;
      continue;
    }

    // Create peer entry with zero health metrics (will rebuild from heartbeats)
    MeshPeerHealth* peer = getMeshPeerHealth(mac, true);
    if (peer) {
      peer->lastHeartbeatMs = 0;  // Will update on first heartbeat
      peer->lastAckMs = 0;
      peer->heartbeatCount = 0;
      peer->ackCount = 0;
      count++;
    }

    pos = macEnd;
  }
  
  DEBUGF(DEBUG_ESPNOW_MESH, "[MESH] Loaded %d peer MAC addresses from filesystem", count);
}

// Restore ESP-NOW peers from saved devices
static void restoreEspNowPeers() {
  if (!gEspNow) return;
  if (!gEspNow->initialized) return;

  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (isSelfMac(gEspNow->devices[i].mac)) {
      continue;
    }
    bool success = addEspNowPeerWithEncryption(
      gEspNow->devices[i].mac,
      gEspNow->devices[i].encrypted,
      gEspNow->devices[i].encrypted ? gEspNow->devices[i].key : nullptr);

    if (success) {
      String encStatus = gEspNow->devices[i].encrypted ? " (encrypted)" : " (unencrypted)";
      DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] Restored device: %s (%s)%s", 
             gEspNow->devices[i].name.c_str(), formatMacAddress(gEspNow->devices[i].mac).c_str(), encStatus.c_str());
    }
  }
}

// ============================================================================
// MESSAGE ROUTER AND DISPATCH
// ============================================================================
// Derive encryption key from passphrase
void deriveKeyFromPassphrase(const String& passphrase, uint8_t* key) {
  if (!gEspNow) return;
  if (passphrase.length() == 0) {
    // No passphrase = no encryption
    memset(key, 0, 16);
    gEspNow->encryptionEnabled = false;
    return;
  }

  // Create consistent input for all devices (no device-specific salt)
  // All devices with the same passphrase will derive the same key
  String saltedInput = passphrase + ":ESP-NOW-SHARED-KEY";

  // Use SHA-256 to derive key (first 16 bytes)
  uint8_t hash[32];
  mbedtls_sha256((uint8_t*)saltedInput.c_str(), saltedInput.length(), hash, 0);
  memcpy(key, hash, 16);

  gEspNow->encryptionEnabled = true;

  // DEBUG: Show detailed key derivation info
  uint8_t mac[6];
  WiFi.macAddress(mac);
  String macStr = formatMacAddress(mac);

  DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] Encryption key derived successfully");
  // NOTE: Do not call broadcastOutput here - can cause watchdog timeout during init
}

// Set ESP-NOW passphrase and derive encryption key
static void setEspNowPassphrase(const String& passphrase) {
  if (!gEspNow) return;
  gEspNow->passphrase = passphrase;
  setSetting(gSettings.espnowPassphrase, passphrase);
  deriveKeyFromPassphrase(passphrase, gEspNow->derivedKey);
}

// ============================================================================
// BOND MODE SESSION TOKEN
// ============================================================================
// Session token = HMAC-SHA256(passphrase, peerMAC || ourMAC)
// This proves knowledge of shared passphrase AND specific bond relationship
// Token is stored in RAM only, never persisted - cleared on disconnect

static void computeBondSessionToken(const uint8_t* peerMac) {
  if (!gEspNow) {
    ERROR_ESPNOWF("[BOND_AUTH] computeToken: gEspNow null");
    return;
  }
  if (gSettings.espnowPassphrase.length() == 0) {
    gEspNow->bondSessionTokenValid = false;
    memset(gEspNow->bondSessionToken, 0, 16);
    DEBUG_ESPNOWF("[BOND_AUTH] No passphrase set - session token disabled");
    return;
  }
  
  uint8_t ourMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, ourMac);
  
  bool peerFirst = memcmp(peerMac, ourMac, 6) < 0;
  
  DEBUG_ESPNOWF("[BOND_AUTH] Computing token: passLen=%d, peerFirst=%d",
                gSettings.espnowPassphrase.length(), peerFirst);
  DEBUG_ESPNOWF("[BOND_AUTH]   ourMac=%02X:%02X:%02X:%02X:%02X:%02X",
                ourMac[0], ourMac[1], ourMac[2], ourMac[3], ourMac[4], ourMac[5]);
  DEBUG_ESPNOWF("[BOND_AUTH]   peerMac=%02X:%02X:%02X:%02X:%02X:%02X",
                peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5]);
  
  uint8_t input[128];
  size_t inputLen = 0;
  
  size_t passLen = gSettings.espnowPassphrase.length();
  if (passLen > 64) passLen = 64;
  memcpy(input + inputLen, gSettings.espnowPassphrase.c_str(), passLen);
  inputLen += passLen;
  
  if (peerFirst) {
    memcpy(input + inputLen, peerMac, 6); inputLen += 6;
    memcpy(input + inputLen, ourMac, 6); inputLen += 6;
  } else {
    memcpy(input + inputLen, ourMac, 6); inputLen += 6;
    memcpy(input + inputLen, peerMac, 6); inputLen += 6;
  }
  
  uint8_t hash[32];
  mbedtls_sha256(input, inputLen, hash, 0);
  memcpy(gEspNow->bondSessionToken, hash, 16);
  gEspNow->bondSessionTokenValid = true;
  
  DEBUG_ESPNOWF("[BOND_AUTH] Token computed: %02X%02X%02X%02X%02X%02X%02X%02X...",
         gEspNow->bondSessionToken[0], gEspNow->bondSessionToken[1],
         gEspNow->bondSessionToken[2], gEspNow->bondSessionToken[3],
         gEspNow->bondSessionToken[4], gEspNow->bondSessionToken[5],
         gEspNow->bondSessionToken[6], gEspNow->bondSessionToken[7]);
}

static bool validateBondSessionToken(const uint8_t* token, size_t tokenLen) {
  if (!gEspNow) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[BOND_AUTH] Validate failed: gEspNow null");
    return false;
  }
  if (!gEspNow->bondSessionTokenValid) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[BOND_AUTH] Validate failed: no valid local token (passphrase set?)");
    return false;
  }
  if (tokenLen != 16) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[BOND_AUTH] Validate failed: wrong token length %zu", tokenLen);
    return false;
  }
  bool match = memcmp(token, gEspNow->bondSessionToken, 16) == 0;
  if (!match) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[BOND_AUTH] Token mismatch: recv=%02X%02X%02X%02X... local=%02X%02X%02X%02X...",
           token[0], token[1], token[2], token[3],
           gEspNow->bondSessionToken[0], gEspNow->bondSessionToken[1],
           gEspNow->bondSessionToken[2], gEspNow->bondSessionToken[3]);
  } else {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[BOND_AUTH] Token validated OK");
  }
  return match;
}

// Clear session token (call when peer goes offline or bonding ends)
static void clearBondSessionToken() {
  if (!gEspNow) return;
  memset(gEspNow->bondSessionToken, 0, 16);
  gEspNow->bondSessionTokenValid = false;
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[BOND] Session token cleared");
}

// Format session token as hex string for sending in commands
// Returns pointer to static buffer (not thread-safe, use immediately)
static const char* formatSessionToken() {
  static char tokenStr[33];
  if (!gEspNow || !gEspNow->bondSessionTokenValid) {
    return "";
  }
  for (int i = 0; i < 16; i++) {
    sprintf(tokenStr + i*2, "%02X", gEspNow->bondSessionToken[i]);
  }
  tokenStr[32] = '\0';
  return tokenStr;
}

// Parse hex session token from string back to bytes
// Returns true if parsing succeeded
static bool parseSessionToken(const char* tokenStr, uint8_t* tokenOut) {
  if (strlen(tokenStr) != 32) return false;
  for (int i = 0; i < 16; i++) {
    char hex[3] = { tokenStr[i*2], tokenStr[i*2+1], '\0' };
    char* endPtr;
    tokenOut[i] = (uint8_t)strtol(hex, &endPtr, 16);
    if (*endPtr != '\0') return false;
  }
  return true;
}

// Check if session token is valid (for external callers like OLED menu)
bool isBondSessionTokenValid() {
  return gEspNow && gEspNow->bondSessionTokenValid;
}

String buildBondedCommandPayload(const String& command) {
  if (!gEspNow) {
    ERROR_ESPNOWF("[BOND_AUTH] buildPayload: gEspNow null");
    return "";
  }
  if (!gEspNow->bondSessionTokenValid) {
    WARN_ESPNOWF("[BOND_AUTH] buildPayload: no valid token - passphrase set?");
    return "";
  }
  String payload = "@BOND:";
  const char* tokenStr = formatSessionToken();
  payload += tokenStr;
  payload += ":";
  payload += command;
  DEBUG_ESPNOWF("[BOND_AUTH] buildPayload: token=%.8s... cmd=%s", tokenStr, command.c_str());
  return payload;
}

// Add ESP-NOW peer with optional encryption
static bool addEspNowPeerWithEncryption(const uint8_t* mac, bool useEncryption, const uint8_t* encryptionKey) {
  // Check if peer already exists
  if (esp_now_is_peer_exist(mac)) {
    // Remove existing peer first
    esp_now_del_peer(mac);
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = gEspNow->channel;
  peerInfo.ifidx = WIFI_IF_STA;

  if (useEncryption && encryptionKey) {
    peerInfo.encrypt = true;
    memcpy(peerInfo.lmk, encryptionKey, 16);  // Local Master Key

    DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] Adding encrypted peer: %s", formatMacAddress(mac).c_str());
  } else {
    peerInfo.encrypt = false;
    DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] Adding unencrypted peer: %s", formatMacAddress(mac).c_str());
  }

  esp_err_t result = esp_now_add_peer(&peerInfo);
  if (result != ESP_OK) {
    DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] Failed to add peer: %d", result);
    return false;
  }

  return true;
}

// Send ESP-NOW response via router (v2 JSON RESPONSE messages)
void sendChunkedResponse(const uint8_t* targetMac, bool success, const String& result, const String& senderName, uint32_t msgId = 0) {
  if (!gEspNow) return;
  
  // Temporarily suspend streaming to prevent feedback during response transmission
  bool wasStreaming = gEspNow->streamingSuspended;
  gEspNow->streamingSuspended = true;

  // Build v2 JSON RESPONSE message (use compact MAC format)
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String statusPrefix = success ? "[SUCCESS] " : "[FAILED] ";
  String fullResult = statusPrefix + result;
  BROADCAST_PRINTF("[ESP-NOW] Sending response to %s (%u bytes)", senderName.c_str(), (unsigned)result.length());

  // Send via V3 command response
  bool sent = v3_send_command_response(targetMac, msgId, success, fullResult.c_str(), fullResult.length());

  if (sent) {
    broadcastOutput("[ESP-NOW] V3 response sent successfully");
    DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] V3 response sent successfully to %s", senderName.c_str());
  } else {
    broadcastOutput("[ESP-NOW] WARNING: V3 response transmission failed");
  }

  // Restore streaming state
  gEspNow->streamingSuspended = wasStreaming;
}

// Send plain text message via V3
void sendTextMessage(const uint8_t* targetMac, const String& text) {
  if (!gEspNow) return;
  
  v3_send_text(targetMac, text.c_str(), text.length());
}

// Cleanup expired chunked messages (5 second timeout)
void cleanupExpiredChunkedMessage() {
  if (!gActiveMessage) return;  // Not allocated
  
  ChunkedMsgGuard guard("cleanupExpiredChunkedMessage");

  if (gActiveMessage->active && (millis() - gActiveMessage->startTime > 5000)) {
    // Only show timeout if we actually started receiving a real message (totalChunks > 0)
    if (gActiveMessage->totalChunks > 0) {
      BROADCAST_PRINTF("[ESP-NOW] Chunked message timeout from %s - showing partial result:", gActiveMessage->deviceName);

      // Show partial result from static buffer
      int partialLen = gActiveMessage->receivedChunks * CHUNK_SIZE;
      if (gActiveMessage->totalLength > 0) {
        partialLen = min(partialLen, gActiveMessage->totalLength);
      }
      partialLen = min(partialLen, MAX_RESULT_BYTES);
      // Create String directly from buffer to avoid character-by-character concatenation
      String partialResult;
      if (partialLen > 0) {
        char* tempBuf = (char*)malloc(partialLen + 1);
        if (tempBuf) {
          memcpy(tempBuf, gActiveMessage->buffer, partialLen);
          tempBuf[partialLen] = '\0';
          partialResult = String(tempBuf);
          free(tempBuf);
          broadcastOutput(partialResult);
        }
      }
      BROADCAST_PRINTF("[ESP-NOW] Error: Incomplete message (%d/%d chunks received)", gActiveMessage->receivedChunks, gActiveMessage->totalChunks);
    }

    // Reset state (even if we didn't show timeout - cleanup stale data)
    gActiveMessage->active = false;
    memset(gActiveMessage->buffer, 0, MAX_RESULT_BYTES);
  }
}

// Forward declaration for session-based streaming helper (defined in V3 protocol section)
static bool trySendToStreamSession(uint32_t cmdMsgId, const char* data, size_t len);
uint32_t gCurrentStreamCmdId = 0;  // Set during command execution on cmd_exec task (non-static for HardwareOne.cpp)

// Send stream message to ESP-NOW target (called by broadcastOutput) - V3 binary STREAM
// Supports two modes:
// 1. Session-based: If gCurrentStreamCmdId is set, stream to that session's target
// 2. Legacy global: If streamActive, stream to streamTarget (startstream/stopstream commands)
void sendEspNowStreamMessage(const String& message) {
  if (!gEspNow || !gEspNow->initialized) return;
  if (gEspNow->streamingSuspended) return;

  // Check for session-based streaming first (V3 CMD output)
  if (gCurrentStreamCmdId != 0) {
    size_t msgLen = message.length();
    if (msgLen > ESPNOW_V3_MAX_PAYLOAD - 1) msgLen = ESPNOW_V3_MAX_PAYLOAD - 1;
    if (trySendToStreamSession(gCurrentStreamCmdId, message.c_str(), msgLen)) {
      return;  // Sent via session
    }
  }

  // Legacy global streaming (startstream/stopstream commands)
  if (!gEspNow->streamActive || !gEspNow->streamTarget) return;

  // Rate limiting: max 10 messages/second
  unsigned long now = millis();
  if (now - gEspNow->lastStreamSendTime < STREAM_MIN_INTERVAL_MS) {
    gEspNow->streamDroppedCount++;
    return;
  }
  gEspNow->lastStreamSendTime = now;

  size_t msgLen = message.length();
  if (msgLen > ESPNOW_V3_MAX_PAYLOAD - 1) msgLen = ESPNOW_V3_MAX_PAYLOAD - 1;
  
  uint32_t msgId = generateMessageId();
  bool sent = v3_send_chunked(gEspNow->streamTarget, ESPNOW_V3_TYPE_STREAM, 0, msgId,
                              (const uint8_t*)message.c_str(), (uint16_t)msgLen, 1);

  if (sent) {
    gEspNow->streamSentCount++;
    DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] Legacy sent | sent=%lu | %.50s",
           (unsigned long)gEspNow->streamSentCount, message.c_str());
  }
}

// Generic handler for chunked message assembly (TYPE_START/CHUNK/END)
// Returns true if message was handled, false if not recognized
static bool handleGenericChunkedMessage(const String& message, const String& msgType,
                                        const String& deviceName, bool hasStatusField) {
  if (!gActiveMessage) return false;
  
  ChunkedMsgGuard guard("handleGenericChunkedMessage");

  String startPrefix = msgType + "_START:";
  String chunkPrefix = msgType + "_CHUNK:";
  String endPrefix = msgType + "_END:";
  int startPrefixLen = startPrefix.length();
  int chunkPrefixLen = chunkPrefix.length();
  int endPrefixLen = endPrefix.length();

  if (message.startsWith(startPrefix)) {
    // Parse: TYPE_START:[status:]chunks:length:hash
    cleanupExpiredChunkedMessage();

    // Find colons in the data portion (after prefix)
    // For STREAM: "STREAM_START:5:965:1867" → extract "5:965:1867"
    // For RESULT: "RESULT_START:SUCCESS:5:965:1867" → extract "SUCCESS:5:965:1867"
    int dataStart = startPrefixLen;
    int colon1 = message.indexOf(':', dataStart);
    int colon2 = message.indexOf(':', colon1 + 1);
    int colon3 = message.indexOf(':', colon2 + 1);

    // Parse based on whether status field exists
    int chunksColonPos, lengthColonPos, hashColonPos;
    if (hasStatusField) {
      // Extract status field
      int statusLen = min(colon1 - dataStart, (int)sizeof(gActiveMessage->status) - 1);
      memcpy(gActiveMessage->status, message.c_str() + dataStart, statusLen);
      gActiveMessage->status[statusLen] = '\0';
      // Chunks start after status
      chunksColonPos = colon1;
      lengthColonPos = colon2;
      hashColonPos = colon3;
    } else {
      gActiveMessage->status[0] = '\0';
      // Chunks start immediately
      chunksColonPos = dataStart - 1;  // So chunksColonPos+1 = dataStart
      lengthColonPos = colon1;
      hashColonPos = colon2;
    }

    if (lengthColonPos > 0 && hashColonPos > 0) {
      gActiveMessage->totalChunks = message.substring(chunksColonPos + 1, lengthColonPos).toInt();
      gActiveMessage->totalLength = message.substring(lengthColonPos + 1, hashColonPos).toInt();

      int hashLen = min((int)message.length() - (hashColonPos + 1), (int)sizeof(gActiveMessage->hash) - 1);
      memcpy(gActiveMessage->hash, message.c_str() + hashColonPos + 1, hashLen);
      gActiveMessage->hash[hashLen] = '\0';

      int nameLen = min((int)deviceName.length(), (int)sizeof(gActiveMessage->deviceName) - 1);
      memcpy(gActiveMessage->deviceName, deviceName.c_str(), nameLen);
      gActiveMessage->deviceName[nameLen] = '\0';

      gActiveMessage->receivedChunks = 0;
      gActiveMessage->startTime = millis();
      gActiveMessage->active = true;
      memset(gActiveMessage->buffer, 0, MAX_RESULT_BYTES);

      DEBUGF(DEBUG_ESPNOW_STREAM, "[%s] Receiving chunked from %s (%d chunks, %d bytes) hash=%s",
             msgType.c_str(), deviceName.c_str(), gActiveMessage->totalChunks, gActiveMessage->totalLength, gActiveMessage->hash);
      DEBUGF(DEBUG_ESPNOW_STREAM, "[%s] DEBUG: Parsed START message='%s'", msgType.c_str(), message.c_str());
      DEBUGF(DEBUG_ESPNOW_STREAM, "[%s] DEBUG: dataStart=%d colon1=%d colon2=%d colon3=%d",
             msgType.c_str(), dataStart, colon1, colon2, colon3);
      DEBUGF(DEBUG_ESPNOW_STREAM, "[%s] DEBUG: chunksColonPos=%d lengthColonPos=%d hashColonPos=%d",
             msgType.c_str(), chunksColonPos, lengthColonPos, hashColonPos);
    }
    return true;

  } else if (message.startsWith(chunkPrefix) && gActiveMessage->active) {
    // Parse: TYPE_CHUNK:N:data
    // Format is "TYPE_CHUNK:1:actual_chunk_data" - only 2 colons, data starts after second
    int dataColonPos = message.indexOf(':', chunkPrefixLen);

    if (dataColonPos > 0) {
      int chunkNum = message.substring(chunkPrefixLen, dataColonPos).toInt();
      String chunkData = message.substring(dataColonPos + 1);

      if (chunkNum >= 1 && chunkNum <= MAX_CHUNKS) {
        int offset = (chunkNum - 1) * CHUNK_SIZE;
        int space = MAX_RESULT_BYTES - offset;
        if (gActiveMessage->totalLength > 0) {
          space = min(space, gActiveMessage->totalLength - offset);
        }
        int toCopy = min(space, (int)chunkData.length());
        if (toCopy > 0 && offset >= 0 && offset < MAX_RESULT_BYTES) {
          memcpy(&gActiveMessage->buffer[offset], chunkData.c_str(), toCopy);
          if (gActiveMessage->receivedChunks < chunkNum) {
            gActiveMessage->receivedChunks = chunkNum;
          }
          DEBUGF(DEBUG_ESPNOW_STREAM, "[%s] Chunk %d/%d received (%d bytes, offset=%d)",
                 msgType.c_str(), chunkNum, gActiveMessage->totalChunks, toCopy, offset);
        }
      }
    }
    return true;

  } else if (message.startsWith(endPrefix) && gActiveMessage->active) {
    // Parse: TYPE_END:hash
    String endHash = message.substring(endPrefixLen);

    DEBUGF(DEBUG_ESPNOW_STREAM, "[%s] END received, hash: %s (expected: %s)",
           msgType.c_str(), endHash.c_str(), gActiveMessage->hash);

    if (endHash == String(gActiveMessage->hash)) {
      int finalLen = gActiveMessage->totalLength > 0 ? min(gActiveMessage->totalLength, MAX_RESULT_BYTES)
                                                     : min(gActiveMessage->receivedChunks * CHUNK_SIZE, MAX_RESULT_BYTES);
      // Create String directly from buffer to avoid character-by-character concatenation
      String fullMessage;
      if (finalLen > 0) {
        char* tempBuf = (char*)malloc(finalLen + 1);
        if (tempBuf) {
          memcpy(tempBuf, gActiveMessage->buffer, finalLen);
          tempBuf[finalLen] = '\0';
          fullMessage = String(tempBuf);
          free(tempBuf);
        }
      }

      DEBUGF(DEBUG_ESPNOW_STREAM, "[%s] Complete: %d bytes from %s",
             msgType.c_str(), finalLen, gActiveMessage->deviceName);

      // Type-specific output
      if (msgType == "STREAM") {
        gEspNow->streamReceivedCount++;
        broadcastOutput("[STREAM:" + String(gActiveMessage->deviceName) + "] " + fullMessage);
      } else if (msgType == "RESULT") {
        broadcastOutput("[ESP-NOW] Remote result from " + String(gActiveMessage->deviceName) + " (" + String(gActiveMessage->status) + "):\n" + fullMessage);
      }

      if (gActiveMessage->receivedChunks < gActiveMessage->totalChunks) {
        BROADCAST_PRINTF("[%s] Warning: Missing %d chunks", msgType.c_str(), gActiveMessage->totalChunks - gActiveMessage->receivedChunks);
      }

      gActiveMessage->active = false;
      memset(gActiveMessage->buffer, 0, MAX_RESULT_BYTES);
    } else {
      BROADCAST_PRINTF("[%s] Error: Hash mismatch", msgType.c_str());
      gActiveMessage->active = false;
    }
    return true;
  }

  return false;
}

// Minimal RX callback: enqueue raw frame into tiny ring and return immediately
static void onEspNowDataReceived(const esp_now_recv_info* recv_info, const uint8_t* incomingData, int len) {
  if (!recv_info || !incomingData || len <= 0) return;
  uint8_t next = (uint8_t)((gEspNowRxHead + 1) % (uint8_t)(sizeof(gEspNowRxRing)/sizeof(gEspNowRxRing[0])));
  if (next == gEspNowRxTail) { gEspNowRxDrops++; return; }
  InboundRxItem& it = gEspNowRxRing[gEspNowRxHead];
  memcpy(it.src, recv_info->src_addr, 6);
  it.len = len; if (it.len > 250) it.len = 250; if (it.len < 0) it.len = 0;
  it.rssi = recv_info->rx_ctrl ? recv_info->rx_ctrl->rssi : (int8_t)-127;
  if (it.len > 0) memcpy(it.data, incomingData, it.len);
  gEspNowRxHead = next;
}

// ============================================================================
// ESP-NOW V3 Binary Protocol - Additional Structures
// (enum EspNowV3Type and V3PayloadHeartbeat forward-declared near top of file)
// ============================================================================

enum EspNowV3Flags : uint8_t {
  ESPNOW_V3_FLAG_ACK_REQ      = 0x01,  // Request ACK from receiver
  ESPNOW_V3_FLAG_ENCRYPTED    = 0x02,  // Payload is encrypted
  ESPNOW_V3_FLAG_COMPRESS     = 0x04,  // Payload is compressed (future)
  ESPNOW_V3_FLAG_STREAM_BEGIN = 0x10,  // First chunk of stream
  ESPNOW_V3_FLAG_STREAM_END   = 0x20,  // Last chunk of stream
};

// Stream session for real-time command output streaming
// Links a CMD msgId to a target MAC so output can be routed correctly
struct StreamSession {
  uint8_t targetMac[6];
  uint32_t cmdMsgId;      // Correlates output to originating CMD
  uint32_t createdAt;     // millis() when created
  bool active;
};
#define MAX_STREAM_SESSIONS 4
#define STREAM_SESSION_TIMEOUT_MS 30000  // Auto-expire after 30s
static StreamSession gStreamSessions[MAX_STREAM_SESSIONS];
// Note: gCurrentStreamCmdId is forward-declared near sendEspNowStreamMessage

// Stream session management
static StreamSession* createStreamSession(const uint8_t* mac, uint32_t cmdMsgId) {
  // Find free slot or reuse expired
  uint32_t now = millis();
  for (int i = 0; i < MAX_STREAM_SESSIONS; i++) {
    if (!gStreamSessions[i].active || 
        (now - gStreamSessions[i].createdAt > STREAM_SESSION_TIMEOUT_MS)) {
      gStreamSessions[i].active = true;
      gStreamSessions[i].cmdMsgId = cmdMsgId;
      gStreamSessions[i].createdAt = now;
      memcpy(gStreamSessions[i].targetMac, mac, 6);
      DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] Session created: msgId=%lu slot=%d", 
             (unsigned long)cmdMsgId, i);
      return &gStreamSessions[i];
    }
  }
  DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] No free session slots for msgId=%lu", (unsigned long)cmdMsgId);
  return nullptr;
}

static StreamSession* findStreamSession(uint32_t cmdMsgId) {
  for (int i = 0; i < MAX_STREAM_SESSIONS; i++) {
    if (gStreamSessions[i].active && gStreamSessions[i].cmdMsgId == cmdMsgId) {
      return &gStreamSessions[i];
    }
  }
  return nullptr;
}

static void destroyStreamSession(uint32_t cmdMsgId) {
  for (int i = 0; i < MAX_STREAM_SESSIONS; i++) {
    if (gStreamSessions[i].active && gStreamSessions[i].cmdMsgId == cmdMsgId) {
      gStreamSessions[i].active = false;
      DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] Session destroyed: msgId=%lu slot=%d", 
             (unsigned long)cmdMsgId, i);
      return;
    }
  }
}

// Send stream frame to session target with optional flags
static void sendSessionStreamFrame(uint32_t cmdMsgId, const char* data, size_t len, uint8_t flags) {
  StreamSession* sess = findStreamSession(cmdMsgId);
  if (!sess || !sess->active) return;
  
  v3_send_chunked(sess->targetMac, ESPNOW_V3_TYPE_STREAM, flags, cmdMsgId,
                  data ? (const uint8_t*)data : nullptr, (uint16_t)len, 1);
}

// Helper for sendEspNowStreamMessage - tries to send via session, returns true if sent
static bool trySendToStreamSession(uint32_t cmdMsgId, const char* data, size_t len) {
  StreamSession* sess = findStreamSession(cmdMsgId);
  if (!sess || !sess->active) return false;
  
  v3_send_chunked(sess->targetMac, ESPNOW_V3_TYPE_STREAM, 0, cmdMsgId,
                  (const uint8_t*)data, (uint16_t)len, 1);
  DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] Session %lu sent: %.50s", (unsigned long)cmdMsgId, data);
  return true;
}

struct __attribute__((packed)) EspNowV3Header {
  uint16_t magic;        // 0x3148 ('H1' little-endian)
  uint8_t  ver;          // Protocol version (3)
  uint8_t  type;         // Message type (EspNowV3Type)
  uint8_t  flags;        // Flags (EspNowV3Flags)
  uint8_t  headerLen;    // Header length (24)
  uint16_t payloadLen;   // Payload length in bytes
  uint32_t msgId;        // Unique message ID
  uint8_t  origin[6];    // Original sender MAC (for mesh forwarding)
  uint8_t  ttl;          // Time-to-live (hops remaining)
  uint8_t  fragIndex;    // Fragment index (0-based)
  uint8_t  fragCount;    // Total fragment count (1 = not fragmented)
  uint16_t crc16;        // CRC16-CCITT of payload
  uint8_t  reserved;     // Reserved for future use
};
static_assert(sizeof(EspNowV3Header) == 24, "EspNowV3Header must be 24 bytes");
static_assert(sizeof(V3PayloadHeartbeat) == 32, "V3PayloadHeartbeat must be 32 bytes");

// V3 Payload structures (all packed, no heap allocation)
struct __attribute__((packed)) V3PayloadCmdResp {
  uint8_t success;                    // 1=success, 0=failure
  char result[ESPNOW_V3_MAX_PAYLOAD - 1];  // Null-terminated result (truncated if needed)
};

struct __attribute__((packed)) V3PayloadFileStart {
  uint32_t fileSize;      // Total file size in bytes
  uint16_t chunkCount;    // Total number of chunks
  uint16_t chunkSize;     // Size of each chunk (except last)
  char filename[64];      // Destination filename
};

struct __attribute__((packed)) V3PayloadFileData {
  uint16_t chunkIndex;    // Chunk index (0-based)
  uint8_t  data[ESPNOW_V3_MAX_PAYLOAD - 2];  // Chunk data (224 bytes max)
};

struct __attribute__((packed)) V3PayloadFileEnd {
  uint32_t crc32;         // CRC32 of entire file
  uint8_t  success;       // 1=transfer complete, 0=aborted
};

struct V3DedupEntry { uint8_t origin[6]; uint32_t id; uint32_t ts; bool active; };
#define V3_DEDUP_SIZE 64
static V3DedupEntry gV3Dedup[V3_DEDUP_SIZE];
static int gV3DedupIdx = 0;

// Broadcast ACK tracking
static BroadcastTracker gBroadcastTrackers[BROADCAST_TRACKER_SLOTS];
static uint32_t gBroadcastsTracked = 0;
static uint32_t gBroadcastsCompleted = 0;
static uint32_t gBroadcastsTimedOut = 0;

// Initialize all broadcast trackers
static void broadcast_tracker_init() {
  for (int i = 0; i < BROADCAST_TRACKER_SLOTS; i++) {
    gBroadcastTrackers[i].reset();
  }
}

// Find tracker by msgId, return nullptr if not found
static BroadcastTracker* broadcast_tracker_find(uint32_t msgId) {
  for (int i = 0; i < BROADCAST_TRACKER_SLOTS; i++) {
    if (gBroadcastTrackers[i].active && gBroadcastTrackers[i].msgId == msgId) {
      return &gBroadcastTrackers[i];
    }
  }
  return nullptr;
}

// Find free tracker slot, return nullptr if all busy
static BroadcastTracker* broadcast_tracker_alloc() {
  for (int i = 0; i < BROADCAST_TRACKER_SLOTS; i++) {
    if (!gBroadcastTrackers[i].active) {
      gBroadcastTrackers[i].reset();
      return &gBroadcastTrackers[i];
    }
  }
  return nullptr;
}

// Record ACK from a peer for a broadcast msgId
static void broadcast_tracker_record_ack(uint32_t msgId, const uint8_t* peerMac) {
  BroadcastTracker* tracker = broadcast_tracker_find(msgId);
  if (!tracker) return;
  
  // Find peer in expected list and mark as received
  for (int i = 0; i < tracker->expectedCount; i++) {
    if (memcmp(tracker->peerMacs[i], peerMac, 6) == 0 && !tracker->ackReceived[i]) {
      tracker->ackReceived[i] = true;
      tracker->receivedCount++;
      DEBUGF(DEBUG_ESPNOW_CORE, "[BROADCAST_TRACK] ACK %u/%u for msgId=%lu",
             tracker->receivedCount, tracker->expectedCount, (unsigned long)msgId);
      return;
    }
  }
}

static uint16_t v3_crc16_ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++) { if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021); else crc = (uint16_t)(crc << 1); }
  }
  return crc;
}

static bool v3_dedup_seen_and_insert(const uint8_t* origin, uint32_t id) {
  for (int i = 0; i < V3_DEDUP_SIZE; i++) {
    if (gV3Dedup[i].active && gV3Dedup[i].id == id && memcmp(gV3Dedup[i].origin, origin, 6) == 0) return true;
  }
  V3DedupEntry& e = gV3Dedup[gV3DedupIdx];
  memcpy(e.origin, origin, 6); e.id = id; e.ts = (uint32_t)millis(); e.active = true;
  gV3DedupIdx = (gV3DedupIdx + 1) % V3_DEDUP_SIZE;
  return false;
}

// Check broadcast trackers for timeouts and report results
static void broadcast_tracker_check_timeouts() {
  uint32_t now = millis();
  for (int i = 0; i < BROADCAST_TRACKER_SLOTS; i++) {
    BroadcastTracker* t = &gBroadcastTrackers[i];
    if (!t->active || t->reported) continue;
    
    bool timedOut = (now - t->startMs) > BROADCAST_TRACKER_TIMEOUT_MS;
    bool allReceived = (t->receivedCount == t->expectedCount);
    
    if (timedOut || allReceived) {
      // Report results (skip if no peers — 0/0 is noise)
      if (t->expectedCount == 0) {
        // Silent completion — no peers to report on
        gBroadcastsCompleted++;
      } else if (allReceived) {
        BROADCAST_PRINTF("[Broadcast] msgId=%lu: %u/%u peers ACK'd (100%%) in %lums",
                        (unsigned long)t->msgId, t->receivedCount, t->expectedCount,
                        (unsigned long)(now - t->startMs));
        gBroadcastsCompleted++;
      } else {
        BROADCAST_PRINTF("[Broadcast] msgId=%lu: %u/%u peers ACK'd (%.1f%%) - timed out",
                        (unsigned long)t->msgId, t->receivedCount, t->expectedCount,
                        (t->expectedCount > 0) ? (100.0f * t->receivedCount / t->expectedCount) : 0.0f);
        gBroadcastsTimedOut++;
        
        // List non-responsive peers
        for (int j = 0; j < t->expectedCount; j++) {
          if (!t->ackReceived[j]) {
            char macStr[18];
            formatMacAddressBuf(t->peerMacs[j], macStr, sizeof(macStr));
            BROADCAST_PRINTF("  No ACK: %s", macStr);
          }
        }
      }
      
      t->reported = true;
      t->active = false;  // Free slot
    }
  }
}

bool v3_send_frame(const uint8_t* dst, uint8_t type, uint8_t flags, uint32_t msgId,
                   const uint8_t* payload, uint16_t payloadLen, uint8_t ttl) {
  if (!dst || (payloadLen > 0 && !payload)) return false;
  size_t totalLen = sizeof(EspNowV3Header) + payloadLen;
  if (totalLen > 250) return false;
  uint8_t frame[250];
  EspNowV3Header h = {};
  h.magic = (uint16_t)ESPNOW_V3_MAGIC; h.ver = 3; h.type = type; h.flags = flags;
  h.headerLen = (uint8_t)sizeof(EspNowV3Header); h.payloadLen = payloadLen; h.msgId = msgId;
  uint8_t myMac[6]; esp_wifi_get_mac(WIFI_IF_STA, myMac); memcpy(h.origin, myMac, 6);
  h.ttl = ttl; h.fragIndex = 0; h.fragCount = 1;
  h.crc16 = payloadLen > 0 ? v3_crc16_ccitt(payload, payloadLen) : 0;
  memcpy(frame, &h, sizeof(h));
  if (payloadLen > 0) memcpy(frame + sizeof(h), payload, payloadLen);
  return esp_now_send(dst, frame, totalLen) == ESP_OK;
}

/**
 * Broadcast message to all mesh peers
 * Sends to each active peer individually (ESP-NOW doesn't support true broadcast)
 * If ESPNOW_V3_FLAG_ACK_REQ is set, tracks ACKs for delivery confirmation
 */
bool v3_broadcast(uint8_t type, uint8_t flags, uint32_t msgId, const uint8_t* payload, uint16_t payloadLen, uint8_t ttl) {
  if (!gEspNow || !gEspNow->initialized) return false;
  
  bool anySuccess = false;
  int sentCount = 0;
  BroadcastTracker* tracker = nullptr;
  
  // If ACK requested, allocate tracker
  if (flags & ESPNOW_V3_FLAG_ACK_REQ) {
    tracker = broadcast_tracker_alloc();
    if (tracker) {
      tracker->msgId = msgId;
      tracker->startMs = millis();
      tracker->expectedCount = 0;
      tracker->receivedCount = 0;
      tracker->active = true;
      tracker->reported = false;
      gBroadcastsTracked++;
      DEBUGF(DEBUG_ESPNOW_CORE, "[BROADCAST_TRACK] Allocated tracker for msgId=%lu", (unsigned long)msgId);
    } else {
      WARN_ESPNOWF("[BROADCAST_TRACK] No free tracker slots for msgId=%lu", (unsigned long)msgId);
    }
  }
  
  // Send to all active mesh peers
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac)) {
      bool sent = v3_send_frame(gMeshPeers[i].mac, type, flags, msgId, payload, payloadLen, ttl);
      if (sent) {
        anySuccess = true;
        sentCount++;
        
        // Record peer in tracker if allocated
        if (tracker && tracker->expectedCount < BROADCAST_TRACKER_MAX_PEERS) {
          memcpy(tracker->peerMacs[tracker->expectedCount], gMeshPeers[i].mac, 6);
          tracker->ackReceived[tracker->expectedCount] = false;
          tracker->expectedCount++;
        }
      }
    }
  }
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_BROADCAST] Sent to %d peers (msgId=%lu type=%u tracked=%s)",
         sentCount, (unsigned long)msgId, type, tracker ? "YES" : "NO");
  
  return anySuccess;
}

/**
 * Send large payload with V3 fragmentation
 * Splits payload into multiple fragments if needed
 * Returns true if all fragments sent successfully
 */
bool v3_send_chunked(const uint8_t* dst, uint8_t type, uint8_t flags, uint32_t msgId,
                            const uint8_t* payload, uint16_t payloadLen, uint8_t ttl) {
  if (!dst || (payloadLen > 0 && !payload)) return false;
  
  // If payload fits in single frame, use regular send
  if (payloadLen <= ESPNOW_V3_MAX_PAYLOAD) {
    return v3_send_frame(dst, type, flags, msgId, payload, payloadLen, ttl);
  }
  
  // Calculate fragments needed
  uint16_t fragPayloadSize = V3_MAX_FRAGMENT_PAYLOAD;
  uint8_t fragCount = (payloadLen + fragPayloadSize - 1) / fragPayloadSize;
  if (fragCount > V3_FRAG_MAX) {
    WARN_ESPNOWF("[V3_FRAG_TX] Payload too large: %u bytes requires %u frags (max %u)", 
                 payloadLen, fragCount, V3_FRAG_MAX);
    return false;
  }
  
  char dstMac[18];
  formatMacAddressBuf(dst, dstMac, sizeof(dstMac));
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] ==============================");
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] Starting fragmented send to %s", dstMac);
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] msgId=%lu type=%u payloadLen=%u",
         (unsigned long)msgId, type, payloadLen);
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] fragCount=%u fragSize=%u", fragCount, fragPayloadSize);
  
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  
  // Send fragments with ACK waiting and retry logic (V2-style reliability)
  const uint8_t MAX_RETRIES = 3;
  const uint32_t ACK_TIMEOUT_MS = 200;  // 200ms per fragment
  
  uint16_t offset = 0;
  for (uint8_t fragIdx = 0; fragIdx < fragCount; fragIdx++) {
    uint16_t fragLen = (offset + fragPayloadSize <= payloadLen) ? fragPayloadSize : (payloadLen - offset);
    
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] --- Fragment %u/%u ---", fragIdx + 1, fragCount);
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] offset=%u len=%u", offset, fragLen);
    
    // Build fragment frame
    uint8_t frame[250];
    EspNowV3Header h = {};
    h.magic = (uint16_t)ESPNOW_V3_MAGIC;
    h.ver = 3;
    h.type = type;
    h.flags = flags | ESPNOW_V3_FLAG_ACK_REQ;  // Always request ACK for fragments
    h.headerLen = (uint8_t)sizeof(EspNowV3Header);
    h.payloadLen = fragLen;
    h.msgId = msgId;
    memcpy(h.origin, myMac, 6);
    h.ttl = ttl;
    h.fragIndex = fragIdx;
    h.fragCount = fragCount;
    h.crc16 = v3_crc16_ccitt(payload + offset, fragLen);
    
    memcpy(frame, &h, sizeof(h));
    memcpy(frame + sizeof(h), payload + offset, fragLen);
    
    // Retry loop for this fragment
    bool fragSent = false;
    for (uint8_t retry = 0; retry < MAX_RETRIES; retry++) {
      if (retry > 0) {
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] Retry attempt %u/%u for fragment %u",
               retry + 1, MAX_RETRIES, fragIdx + 1);
      }
      // Allocate ACK wait slot
      V3FragAckWait* ackWait = v3_frag_ack_alloc(dst, msgId, fragIdx);
      if (!ackWait) {
        WARN_ESPNOWF("[V3_FRAG_TX] No ACK slot available for frag %u/%u", fragIdx + 1, fragCount);
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] Aborting: ACK tracking full");
        return false;
      }
      
      ackWait->acked = false;
      ackWait->sentMs = millis();
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] ACK waiter allocated for msgId=%lu fragIdx=%u",
             (unsigned long)msgId, fragIdx);
      
      // Send fragment
      uint16_t totalLen = sizeof(h) + fragLen;
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] Calling esp_now_send: totalLen=%u", totalLen);
      esp_err_t result = esp_now_send(dst, frame, totalLen);
      if (result != ESP_OK) {
        WARN_ESPNOWF("[V3_FRAG_TX] Fragment %u/%u send failed (retry %u): esp_err=%d", 
                     fragIdx + 1, fragCount, retry, result);
        ackWait->active = false;
        uint32_t backoffMs = 50 * (retry + 1);
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] Backing off %lums before retry", (unsigned long)backoffMs);
        vTaskDelay(pdMS_TO_TICKS(backoffMs));  // Exponential backoff
        continue;
      }
      
      if (gEspNow) { gEspNow->routerMetrics.v3FragTx++; }
      
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] ✓ Fragment %u/%u sent: %u bytes (offset=%u, retry=%u)",
             fragIdx + 1, fragCount, fragLen, offset, retry);
      
      // Wait for ACK
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] Waiting for ACK (timeout=%lums)...",
             (unsigned long)ACK_TIMEOUT_MS);
      uint32_t waitStart = millis();
      while ((millis() - waitStart) < ACK_TIMEOUT_MS) {
        if (ackWait->acked) {
          uint32_t elapsed = millis() - waitStart;
          DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] ✓ Fragment %u/%u ACK received after %lums",
                 fragIdx + 1, fragCount, (unsigned long)elapsed);
          fragSent = true;
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      
      ackWait->active = false;  // Release slot
      
      if (fragSent) {
        break;  // Success, move to next fragment
      }
      
      uint32_t elapsed = millis() - waitStart;
      WARN_ESPNOWF("[V3_FRAG_TX] ✗ Fragment %u/%u ACK timeout after %lums (retry %u)",
                   fragIdx + 1, fragCount, (unsigned long)elapsed, retry);
    }
    
    if (!fragSent) {
      WARN_ESPNOWF("[V3_FRAG_TX] FAILED: Fragment %u/%u did not ACK after %u retries",
                   fragIdx + 1, fragCount, MAX_RETRIES);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] Aborting: Only %u/%u fragments succeeded",
             fragIdx, fragCount);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] ==============================");
      return false;
    }
    
    offset += fragLen;
  }
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] ✓ SUCCESS: All %u fragments sent with ACKs!", fragCount);
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_TX] ==============================");
  return true;
}

static bool v3_send_ack(const uint8_t* dst, uint32_t ackFor) {
  char dstMac[18];
  formatMacAddressBuf(dst, dstMac, sizeof(dstMac));
  DEBUGF(DEBUG_ESPNOW_CORE, "[V3_ACK_TX] Sending ACK to %s for msgId=%lu", dstMac, (unsigned long)ackFor);
  bool result = v3_send_frame(dst, ESPNOW_V3_TYPE_ACK, 0, ackFor, nullptr, 0, 1);
  DEBUGF(DEBUG_ESPNOW_CORE, "[V3_ACK_TX] Result: %s", result ? "SUCCESS" : "FAILED");
  return result;
}

// Send ACK for a specific fragment
static bool v3_send_frag_ack(const uint8_t* dst, uint32_t msgId, uint8_t fragIndex, uint8_t fragCount) {
  char dstMac[18];
  formatMacAddressBuf(dst, dstMac, sizeof(dstMac));
  DEBUGF(DEBUG_ESPNOW_CORE, "[V3_FRAG_ACK_TX] Sending fragment ACK to %s", dstMac);
  DEBUGF(DEBUG_ESPNOW_CORE, "[V3_FRAG_ACK_TX] msgId=%lu fragIdx=%u fragCnt=%u",
         (unsigned long)msgId, fragIndex, fragCount);
  
  // Build ACK frame with fragment info in header
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  
  EspNowV3Header h = {};
  h.magic = (uint16_t)ESPNOW_V3_MAGIC;
  h.ver = 3;
  h.type = ESPNOW_V3_TYPE_ACK;
  h.flags = 0;
  h.headerLen = (uint8_t)sizeof(EspNowV3Header);
  h.payloadLen = 0;
  h.msgId = msgId;
  memcpy(h.origin, myMac, 6);
  h.ttl = 1;
  h.fragIndex = fragIndex;
  h.fragCount = fragCount;
  h.crc16 = 0;
  
  DEBUGF(DEBUG_ESPNOW_CORE, "[V3_FRAG_ACK_TX] Calling esp_now_send: headerLen=%u", (unsigned)sizeof(h));
  esp_err_t result = esp_now_send(dst, (uint8_t*)&h, sizeof(h));
  
  if (result == ESP_OK) {
    DEBUGF(DEBUG_ESPNOW_CORE, "[V3_FRAG_ACK_TX] ✓ Fragment ACK sent successfully");
  } else {
    DEBUGF(DEBUG_ESPNOW_CORE, "[V3_FRAG_ACK_TX] ✗ Fragment ACK send failed: esp_err=%d", result);
  }
  
  return (result == ESP_OK);
}

// V3 sender functions for mesh system messages
static bool v3_send_time_sync(const uint8_t* dst, uint32_t epochTime, int32_t timeOffset) {
  V3PayloadTimeSync payload;
  payload.epochTime = epochTime;
  payload.timeOffset = timeOffset;
  
  uint32_t msgId = generateMessageId();
  char dstMac[18];
  formatMacAddressBuf(dst, dstMac, sizeof(dstMac));
  DEBUGF(DEBUG_ESPNOW_MESH, "[V3_TX_TIME_SYNC] Sending to %s msgId=%lu", dstMac, (unsigned long)msgId);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V3_TX_TIME_SYNC] epochTime=%lu timeOffset=%ld",
         (unsigned long)epochTime, (long)timeOffset);
  bool result = v3_send_frame(dst, ESPNOW_V3_TYPE_TIME_SYNC, 0, msgId, (const uint8_t*)&payload, sizeof(payload), 2);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V3_TX_TIME_SYNC] Result: %s", result ? "SUCCESS" : "FAILED");
  return result;
}

static bool v3_broadcast_time_sync(uint32_t epochTime, int32_t timeOffset) {
  uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  return v3_send_time_sync(broadcastMac, epochTime, timeOffset);
}

static bool v3_send_topo_request(const uint8_t* dst, uint32_t reqId) {
  V3PayloadTopoReq payload;
  payload.reqId = reqId;
  memset(payload.reserved, 0, sizeof(payload.reserved));
  
  uint32_t msgId = generateMessageId();
  char dstMac[18];
  formatMacAddressBuf(dst, dstMac, sizeof(dstMac));
  DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_TX_TOPO_REQ] Sending to %s msgId=%lu reqId=%lu",
         dstMac, (unsigned long)msgId, (unsigned long)reqId);
  bool result = v3_send_frame(dst, ESPNOW_V3_TYPE_TOPO_REQ, 0, msgId, (const uint8_t*)&payload, sizeof(payload), 2);
  DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_TX_TOPO_REQ] Result: %s", result ? "SUCCESS" : "FAILED");
  return result;
}

bool v3_broadcast_topo_request(uint32_t reqId) {
  DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_BROADCAST_TOPO_REQ] Broadcasting reqId=%lu", (unsigned long)reqId);
  uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  return v3_send_topo_request(broadcastMac, reqId);
}

bool v3_send_worker_status(const uint8_t* dst, const V3PayloadWorkerStatus& status, 
                                   const char* jsonMetadata, uint16_t jsonLen) {
  uint32_t msgId = generateMessageId();
  
  char dstMac[18];
  formatMacAddressBuf(dst, dstMac, sizeof(dstMac));
  DEBUGF(DEBUG_ESPNOW_MESH, "[V3_TX_WORKER_STATUS] Sending to %s msgId=%lu", dstMac, (unsigned long)msgId);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V3_TX_WORKER_STATUS] Payload: freeHeap=%lu totalHeap=%lu rssi=%d thermal=%d imu=%d",
         (unsigned long)status.freeHeap, (unsigned long)status.totalHeap, status.rssi, status.thermalEnabled, status.imuEnabled);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V3_TX_WORKER_STATUS] name='%s' metadata=%s (%u bytes)",
         status.name, jsonMetadata ? "YES" : "NO", jsonLen);
  
  if (jsonMetadata && jsonLen > 0) {
    // Has metadata - combine struct + JSON
    uint16_t totalLen = sizeof(status) + jsonLen;
    DEBUGF(DEBUG_ESPNOW_MESH, "[V3_TX_WORKER_STATUS] Total payload: %u bytes (struct=%u + json=%u)",
           totalLen, (unsigned)sizeof(status), jsonLen);
    if (totalLen > ESPNOW_V3_MAX_PAYLOAD) {
      DEBUGF(DEBUG_ESPNOW_MESH, "[V3_TX_WORKER_STATUS] Payload exceeds max, using fragmented send");
      // Use chunked send for large payloads
      uint8_t* buffer = (uint8_t*)malloc(totalLen);
      if (!buffer) return false;
      memcpy(buffer, &status, sizeof(status));
      memcpy(buffer + sizeof(status), jsonMetadata, jsonLen);
      bool result = v3_send_chunked(dst, ESPNOW_V3_TYPE_WORKER_STATUS, ESPNOW_V3_FLAG_ACK_REQ, 
                                     msgId, buffer, totalLen, 2);
      free(buffer);
      DEBUGF(DEBUG_ESPNOW_MESH, "[V3_TX_WORKER_STATUS] Fragmented send result: %s", result ? "SUCCESS" : "FAILED");
      return result;
    } else {
      // Fits in one frame
      DEBUGF(DEBUG_ESPNOW_MESH, "[V3_TX_WORKER_STATUS] Sending single frame");
      uint8_t buffer[250];
      memcpy(buffer, &status, sizeof(status));
      memcpy(buffer + sizeof(status), jsonMetadata, jsonLen);
      bool result = v3_send_frame(dst, ESPNOW_V3_TYPE_WORKER_STATUS, ESPNOW_V3_FLAG_ACK_REQ, 
                          msgId, buffer, totalLen, 2);
      DEBUGF(DEBUG_ESPNOW_MESH, "[V3_TX_WORKER_STATUS] Single frame result: %s", result ? "SUCCESS" : "FAILED");
      return result;
    }
  } else {
    // No metadata - just struct
    DEBUGF(DEBUG_ESPNOW_MESH, "[V3_TX_WORKER_STATUS] No metadata, sending struct only (%u bytes)", (unsigned)sizeof(status));
    bool result = v3_send_frame(dst, ESPNOW_V3_TYPE_WORKER_STATUS, ESPNOW_V3_FLAG_ACK_REQ, 
                        msgId, (const uint8_t*)&status, sizeof(status), 2);
    DEBUGF(DEBUG_ESPNOW_MESH, "[V3_TX_WORKER_STATUS] Result: %s", result ? "SUCCESS" : "FAILED");
    return result;
  }
}

static bool v3_send_topo_start(const uint8_t* dst, uint32_t reqId, uint8_t peerCount) {
  V3PayloadTopoStart payload;
  payload.reqId = reqId;
  payload.peerCount = peerCount;
  memset(payload.reserved, 0, sizeof(payload.reserved));
  
  uint32_t msgId = generateMessageId();
  return v3_send_frame(dst, ESPNOW_V3_TYPE_TOPO_START, 0, msgId, (const uint8_t*)&payload, sizeof(payload), 2);
}

static bool v3_send_topo_peer(const uint8_t* dst, uint32_t reqId, uint8_t peerIndex, 
                               bool isLast, const uint8_t* peerMac, int8_t rssi, 
                               bool encrypted, const char* peerName) {
  V3PayloadTopoPeer payload;
  payload.reqId = reqId;
  payload.peerIndex = peerIndex;
  payload.isLast = isLast ? 1 : 0;
  memcpy(payload.mac, peerMac, 6);
  payload.rssi = rssi;
  payload.encrypted = encrypted ? 1 : 0;
  strncpy(payload.name, peerName, sizeof(payload.name) - 1);
  payload.name[sizeof(payload.name) - 1] = '\0';
  
  uint32_t msgId = generateMessageId();
  return v3_send_frame(dst, ESPNOW_V3_TYPE_TOPO_PEER, 0, msgId, (const uint8_t*)&payload, sizeof(payload), 2);
}

// Broadcast sensor status to all mesh peers
bool v3_broadcast_sensor_status(RemoteSensorType sensorType, bool enabled) {
  V3PayloadSensorStatus payload;
  payload.sensorType = (uint8_t)sensorType;
  payload.enabled = enabled ? 1 : 0;
  memset(payload.reserved, 0, sizeof(payload.reserved));
  
  uint32_t msgId = generateMessageId();
  extern const char* sensorTypeToString(RemoteSensorType type);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V3_BROADCAST_SENSOR_STATUS] Broadcasting msgId=%lu", (unsigned long)msgId);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V3_BROADCAST_SENSOR_STATUS] sensor=%s enabled=%s",
         sensorTypeToString(sensorType), enabled ? "YES" : "NO");
  bool result = v3_broadcast(ESPNOW_V3_TYPE_SENSOR_STATUS, 0, msgId, (const uint8_t*)&payload, sizeof(payload), 2);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V3_BROADCAST_SENSOR_STATUS] Result: %s", result ? "SUCCESS" : "FAILED");
  return result;
}

// Broadcast sensor data to all mesh peers
bool v3_broadcast_sensor_data(RemoteSensorType sensorType, const char* jsonData, uint16_t jsonLen) {
  if (!jsonData || jsonLen == 0 || jsonLen > 200) {
    DEBUGF(DEBUG_ESPNOW_MESH, "[V3_BROADCAST_SENSOR_DATA] ERROR: Invalid params (jsonData=%p len=%u)",
           jsonData, jsonLen);
    return false;
  }
  
  // Build payload: struct + JSON data
  uint8_t buffer[256];
  V3PayloadSensorBroadcast* payload = (V3PayloadSensorBroadcast*)buffer;
  payload->sensorType = (uint8_t)sensorType;
  payload->dataLen = jsonLen;
  memcpy(payload->data, jsonData, jsonLen);
  
  uint16_t totalLen = sizeof(V3PayloadSensorBroadcast) + jsonLen;
  uint32_t msgId = generateMessageId();
  extern const char* sensorTypeToString(RemoteSensorType type);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V3_BROADCAST_SENSOR_DATA] Broadcasting msgId=%lu", (unsigned long)msgId);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V3_BROADCAST_SENSOR_DATA] sensor=%s jsonLen=%u totalLen=%u",
         sensorTypeToString(sensorType), jsonLen, totalLen);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V3_BROADCAST_SENSOR_DATA] JSON (first 80 chars): %.80s", jsonData);
  bool result = v3_broadcast(ESPNOW_V3_TYPE_SENSOR_BROADCAST, 0, msgId, buffer, totalLen, 2);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V3_BROADCAST_SENSOR_DATA] Result: %s", result ? "SUCCESS" : "FAILED");
  return result;
}

// Send user sync to specific device (JSON payload)
bool v3_send_user_sync(const uint8_t* dst, const char* jsonPayload, uint16_t jsonLen) {
  if (!jsonPayload || jsonLen == 0) {
    DEBUGF(DEBUG_ESPNOW_MESH, "[V3_TX_USER_SYNC] ERROR: Invalid params (jsonPayload=%p len=%u)",
           jsonPayload, jsonLen);
    return false;
  }
  
  uint32_t msgId = generateMessageId();
  uint8_t flags = ESPNOW_V3_FLAG_ACK_REQ;  // User sync requires ACK
  char dstMac[18];
  formatMacAddressBuf(dst, dstMac, sizeof(dstMac));
  DEBUGF(DEBUG_ESPNOW_MESH, "[V3_TX_USER_SYNC] Sending to %s msgId=%lu", dstMac, (unsigned long)msgId);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V3_TX_USER_SYNC] jsonLen=%u JSON (first 100 chars): %.100s",
         jsonLen, jsonPayload);
  bool result = v3_send_frame(dst, ESPNOW_V3_TYPE_USER_SYNC, flags, msgId, (const uint8_t*)jsonPayload, jsonLen, 3);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V3_TX_USER_SYNC] Result: %s", result ? "SUCCESS" : "FAILED");
  return result;
}

// V3 helper functions for command responses and text messages
bool v3_send_text(const uint8_t* dst, const char* text, uint16_t textLen) {
  if (!text || textLen == 0 || textLen > ESPNOW_V3_MAX_PAYLOAD) return false;
  
  uint32_t msgId = generateMessageId();
  char dstMac[18];
  formatMacAddressBuf(dst, dstMac, sizeof(dstMac));
  DEBUGF(DEBUG_ESPNOW_CORE, "[V3_TX_TEXT] Sending to %s msgId=%lu len=%u", dstMac, (unsigned long)msgId, textLen);
  DEBUGF(DEBUG_ESPNOW_CORE, "[V3_TX_TEXT] Text (first 100 chars): %.100s", text);
  
  bool result = v3_send_frame(dst, ESPNOW_V3_TYPE_TEXT, ESPNOW_V3_FLAG_ACK_REQ, msgId, 
                              (const uint8_t*)text, textLen, 1);
  DEBUGF(DEBUG_ESPNOW_CORE, "[V3_TX_TEXT] Result: %s", result ? "SUCCESS" : "FAILED");
  return result;
}

bool v3_send_command_response(const uint8_t* dst, uint32_t cmdMsgId, bool success, const char* resultText, uint16_t textLen) {
  if (!resultText || textLen == 0) return false;
  
  // Build response payload: success byte + result text
  uint16_t totalLen = 1 + textLen;
  if (totalLen > ESPNOW_V3_MAX_PAYLOAD) {
    // Use chunked send for large responses
    uint8_t* buffer = (uint8_t*)malloc(totalLen);
    if (!buffer) return false;
    buffer[0] = success ? 1 : 0;
    memcpy(buffer + 1, resultText, textLen);
    
    char dstMac[18];
    formatMacAddressBuf(dst, dstMac, sizeof(dstMac));
    DEBUGF(DEBUG_ESPNOW_CORE, "[V3_TX_CMD_RESP] Sending to %s cmdMsgId=%lu success=%d len=%u (chunked)",
           dstMac, (unsigned long)cmdMsgId, success, totalLen);
    
    bool result = v3_send_chunked(dst, ESPNOW_V3_TYPE_CMD_RESP, ESPNOW_V3_FLAG_ACK_REQ, 
                                   cmdMsgId, buffer, totalLen, 1);
    free(buffer);
    DEBUGF(DEBUG_ESPNOW_CORE, "[V3_TX_CMD_RESP] Result: %s", result ? "SUCCESS" : "FAILED");
    return result;
  } else {
    // Single frame
    uint8_t buffer[256];
    buffer[0] = success ? 1 : 0;
    memcpy(buffer + 1, resultText, textLen);
    
    char dstMac[18];
    formatMacAddressBuf(dst, dstMac, sizeof(dstMac));
    DEBUGF(DEBUG_ESPNOW_CORE, "[V3_TX_CMD_RESP] Sending to %s cmdMsgId=%lu success=%d len=%u",
           dstMac, (unsigned long)cmdMsgId, success, totalLen);
    
    bool result = v3_send_frame(dst, ESPNOW_V3_TYPE_CMD_RESP, ESPNOW_V3_FLAG_ACK_REQ, 
                                cmdMsgId, buffer, totalLen, 1);
    DEBUGF(DEBUG_ESPNOW_CORE, "[V3_TX_CMD_RESP] Result: %s", result ? "SUCCESS" : "FAILED");
    return result;
  }
}

static bool v3_send_file_response(const uint8_t* dst, uint32_t reqMsgId, bool success, const char* message) {
  if (!message) return false;
  
  uint16_t msgLen = strlen(message);
  uint16_t totalLen = 1 + msgLen;  // success byte + message
  
  uint8_t buffer[256];
  buffer[0] = success ? 1 : 0;
  memcpy(buffer + 1, message, msgLen);
  
  char dstMac[18];
  formatMacAddressBuf(dst, dstMac, sizeof(dstMac));
  DEBUGF(DEBUG_ESPNOW_CORE, "[V3_TX_FILE_RESP] Sending to %s reqMsgId=%lu success=%d",
         dstMac, (unsigned long)reqMsgId, success);
  
  bool result = v3_send_frame(dst, ESPNOW_V3_TYPE_CMD_RESP, ESPNOW_V3_FLAG_ACK_REQ, 
                              reqMsgId, buffer, totalLen, 1);
  DEBUGF(DEBUG_ESPNOW_CORE, "[V3_TX_FILE_RESP] Result: %s", result ? "SUCCESS" : "FAILED");
  return result;
}

bool v3_broadcast_text(const char* text, uint16_t textLen) {
  if (!text || textLen == 0 || textLen > ESPNOW_V3_MAX_PAYLOAD) return false;
  
  uint32_t msgId = generateMessageId();
  DEBUGF(DEBUG_ESPNOW_CORE, "[V3_BROADCAST_TEXT] Broadcasting msgId=%lu len=%u", (unsigned long)msgId, textLen);
  
  bool result = v3_broadcast(ESPNOW_V3_TYPE_TEXT, 0, msgId, (const uint8_t*)text, textLen, 2);
  DEBUGF(DEBUG_ESPNOW_CORE, "[V3_BROADCAST_TEXT] Result: %s", result ? "SUCCESS" : "FAILED");
  return result;
}

// Forward declarations for static functions defined later in this file
static TopologyStream* findTopoStream(const uint8_t* senderMac, uint32_t reqId);
static TopologyStream* findOrCreateTopoStream(const uint8_t* senderMac, uint32_t reqId);
static void            addTopoDeviceName(const uint8_t* mac, const char* name);
static String          getTopoDeviceName(const uint8_t* mac);
static void            finalizeTopologyStream(TopologyStream* stream);
static bool            handleJsonMessage(const ReceivedMessage& ctx);
static bool            parseJsonMessage(const String& message, JsonDocument& doc);
static void            updateUnpairedDevice(const uint8_t* mac, const String& name, int rssi);
#if ENABLE_BONDED_MODE
static bool            cacheManifestToLittleFS(const uint8_t fwHash[16], const String& manifest);
#endif
#if ENABLE_BONDED_MODE
static void            processBondSettings(const uint8_t* srcMac, const String& deviceName, const String& settingsStr);
static void            requestBondSettings(const uint8_t* peerMac);
#endif
static void            meshRetryDequeue(uint32_t msgId);

#if ENABLE_BONDED_MODE
// Process a received manifest from a bonded peer (bond mode handshake step)
static void processBondModeManifestResp(const uint8_t* srcMac, const String& deviceName, const String& manifestStr) {
  if (!gEspNow) return;
  BROADCAST_PRINTF("[BOND_SYNC] Manifest received from %s len=%d role=%d",
                   deviceName.c_str(), manifestStr.length(), (int)gSettings.bondRole);
  
  // Reject if peer is offline (stale transfer from before disconnect)
  if (!gEspNow->bondPeerOnline) {
    BROADCAST_PRINTF("[BOND_SYNC] REJECTED stale manifest (peer offline)");
    return;
  }
  
  // Validate JSON integrity — reject truncated/corrupted file transfers
  if (manifestStr.length() < 2 || manifestStr[0] != '{' || manifestStr[manifestStr.length() - 1] != '}') {
    BROADCAST_PRINTF("[BOND_SYNC] REJECTED corrupt manifest (len=%d, not valid JSON object)", manifestStr.length());
    gEspNow->bondSyncInFlight = BOND_SYNC_NONE;
    gEspNow->bondSyncRetryCount = 0;
    return;  // Sync tick will re-request
  }
  
  gEspNow->bondManifestReceived = true;
  gEspNow->bondSyncInFlight = BOND_SYNC_NONE;
  gEspNow->bondSyncRetryCount = 0;

  if (gEspNow->lastRemoteCapValid) {
    cacheManifestToLittleFS(gEspNow->lastRemoteCap.fwHash, manifestStr);
  }
  // Sync tick will pick up the next missing item (settings) on next iteration
}
#endif // ENABLE_BONDED_MODE

// Forward declaration for command execution
static void v3_handle_cmd(const uint8_t* srcMac, const char* deviceName, uint32_t msgId, const char* cmd);

static bool v3_try_handle_incoming(const esp_now_recv_info* recv_info, const uint8_t* data, int len) {
  if (!recv_info || !data || len < (int)sizeof(EspNowV3Header)) return false;
  const EspNowV3Header* h = (const EspNowV3Header*)data;
  if (h->magic != (uint16_t)ESPNOW_V3_MAGIC) return false;
  if (h->ver != 3 || h->headerLen != sizeof(EspNowV3Header)) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_RX] REJECTED: ver=%u headerLen=%u (expected ver=3 headerLen=%u)",
           h->ver, h->headerLen, (unsigned)sizeof(EspNowV3Header));
    return true;
  }
  if ((int)h->headerLen + h->payloadLen > len) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_RX] REJECTED: headerLen(%u)+payloadLen(%u)=%u > len(%d)",
           h->headerLen, h->payloadLen, (unsigned)(h->headerLen + h->payloadLen), len);
    return true;
  }
  const uint8_t* payload = data + sizeof(EspNowV3Header);
  uint16_t payloadLen = h->payloadLen;
  if (payloadLen > 0 && v3_crc16_ccitt(payload, payloadLen) != h->crc16) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_RX] REJECTED: CRC mismatch (got=0x%04X expected=0x%04X) payloadLen=%u",
           v3_crc16_ccitt(payload, payloadLen), h->crc16, payloadLen);
    return true;
  }
  
  // === V3 FRAGMENTATION REASSEMBLY ===
  if (h->fragCount > 1) {
    // Multi-fragment message - reassemble
    if (gEspNow) { gEspNow->routerMetrics.v3FragRx++; }
    
    char srcMac[18];
    formatMacAddressBuf(recv_info->src_addr, srcMac, sizeof(srcMac));
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] ==============================");
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] Fragment received from %s", srcMac);
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] Fragment %u/%u msgId=%lu type=%u len=%u",
           h->fragIndex + 1, h->fragCount, (unsigned long)h->msgId, h->type, payloadLen);
    
    // Run GC
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] Running reassembly GC...");
    v3_reasm_gc(millis());
    
    // Find or allocate reassembly buffer
    V3ReasmEntry* e = v3_reasm_find_or_alloc(recv_info->src_addr, h->msgId, h->type, h->fragCount);
    if (!e) {
      WARN_ESPNOWF("[V3_FRAG_RX] No reassembly slot available - all %u slots in use", V3_REASM_MAX);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] ==============================");
      return true;
    }
    
    bool isNewEntry = (e->received == 0);
    if (isNewEntry) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] Allocated new reassembly buffer: bufSize=%u", e->bufferSize);
    } else {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] Using existing reassembly buffer: %u/%u fragments already received",
             e->received, e->fragCount);
    }
    
    // Store fragment if not duplicate
    if (h->fragIndex >= h->fragCount) {
      WARN_ESPNOWF("[V3_FRAG_RX] Invalid fragment index %u (max %u)", h->fragIndex, h->fragCount - 1);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] ==============================");
      return true;
    }
    
    if (e->have[h->fragIndex]) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] Duplicate fragment %u - ignoring", h->fragIndex + 1);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] ==============================");
      return true;
    }
    
    // Copy fragment data to buffer
    uint16_t offset = h->fragIndex * V3_MAX_FRAGMENT_PAYLOAD;
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] Storing at offset=%u len=%u (bufSize=%u)",
           offset, payloadLen, e->bufferSize);
    if (offset + payloadLen > e->bufferSize) {
      WARN_ESPNOWF("[V3_FRAG_RX] Fragment overflow: offset=%u len=%u bufSize=%u", offset, payloadLen, e->bufferSize);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] Resetting corrupted reassembly buffer");
      v3_reasm_reset(*e);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] ==============================");
      return true;
    }
    
    memcpy(e->buffer + offset, payload, payloadLen);
    e->have[h->fragIndex] = true;
    e->received++;
    e->lastUpdateMs = millis();
    
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] Fragment %u stored successfully", h->fragIndex + 1);
    
    // Check if complete
    if (e->received < e->fragCount) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] Progress: %u/%u fragments received",
             e->received, e->fragCount);
      
      // Log which fragments are still missing
      if (e->fragCount <= 10) {  // Only log for reasonable fragment counts
        char missing[64] = {0};  // Max: "1,2,3,4,5,6,7,8,9,10" = 20 chars + margin
        int offset = 0;
        for (uint8_t i = 0; i < e->fragCount; i++) {
          if (!e->have[i]) {
            if (offset > 0 && offset < (int)sizeof(missing) - 5) {
              offset += snprintf(missing + offset, sizeof(missing) - offset, ",");
            }
            if (offset < (int)sizeof(missing) - 5) {
              offset += snprintf(missing + offset, sizeof(missing) - offset, "%u", i + 1);
            }
          }
        }
        if (offset > 0) {
          DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] Still waiting for fragments: %s", missing);
        }
      }
      
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] ==============================");
      return true;  // Not complete yet
    }
    
    // Calculate actual reassembled size (last fragment may be partial)
    uint16_t reassembledSize = 0;
    for (uint8_t i = 0; i < e->fragCount; i++) {
      if (i == e->fragCount - 1) {
        // Last fragment - use actual payload length from that fragment
        // We need to track this - for now estimate conservatively
        reassembledSize += payloadLen;  // Last fragment's size
      } else {
        reassembledSize += V3_MAX_FRAGMENT_PAYLOAD;
      }
    }
    
    unsigned long totalTime = millis() - (e->lastUpdateMs - (millis() - e->lastUpdateMs));
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] ✓✓✓ REASSEMBLY COMPLETE ✓✓✓");
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] All %u fragments received! Total size: %u bytes",
           e->fragCount, reassembledSize);
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] Proceeding to handle reassembled message type=%u", h->type);
    if (gEspNow) { gEspNow->routerMetrics.v3FragRxCompleted++; }
    
    // Update pointers to point to reassembled buffer
    payload = e->buffer;
    payloadLen = reassembledSize;
    
    // CRITICAL: Mark this reassembly entry for cleanup after message processing
    // We can't free it immediately because payload points to e->buffer
    // The cleanup happens at the end of this function before returning
    // Note: Store the entry pointer for cleanup (h->fragCount check at end will handle it)
  }
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_RX] ========================================");
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_RX] V3 BINARY MESSAGE RECEIVED");
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_RX] Type=%u MsgId=%lu PayloadLen=%u Flags=0x%02X",
         h->type, (unsigned long)h->msgId, payloadLen, h->flags);
  
  // Handle ACK (no dedup needed)
  if (h->type == ESPNOW_V3_TYPE_ACK) {
    char srcMac[18];
    formatMacAddressBuf(recv_info->src_addr, srcMac, sizeof(srcMac));
    DEBUGF(DEBUG_ESPNOW_CORE, "[V3_ACK_RX] ACK received from %s for msgId=%lu fragIdx=%u fragCnt=%u",
           srcMac, (unsigned long)h->msgId, h->fragIndex, h->fragCount);
    
    // Update peer health ACK tracking
    MeshPeerHealth* peer = getMeshPeerHealth(recv_info->src_addr, true);
    if (peer) {
      peer->lastAckMs = millis();
      peer->ackCount++;
    }
    
    // Check broadcast tracker
    broadcast_tracker_record_ack(h->msgId, recv_info->src_addr);
    
    // Check V3 fragment ACK waiters
    bool foundV3 = false;
    for (int i = 0; i < V3_FRAG_ACK_WAIT_MAX; i++) {
      if (gV3FragAckWait[i].active && gV3FragAckWait[i].msgId == h->msgId && 
          gV3FragAckWait[i].fragIndex == h->fragIndex) {
        gV3FragAckWait[i].acked = true;
        foundV3 = true;
        DEBUGF(DEBUG_ESPNOW_CORE, "[V3_ACK_RX] Matched V3 fragment ACK waiter slot %d (msgId=%lu fragIdx=%u)",
               i, (unsigned long)h->msgId, h->fragIndex);
        break;
      }
    }
    
    if (!foundV3) {
      DEBUGF(DEBUG_ESPNOW_CORE, "[V3_ACK_RX] No matching ACK waiter found (msgId=%lu fragIdx=%u)",
             (unsigned long)h->msgId, h->fragIndex);
    }
    
    // Cleanup reassembly buffer if this was a fragmented message
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }
  
  // Send ACK if requested
  if (h->flags & ESPNOW_V3_FLAG_ACK_REQ) {
    // For fragmented messages, send fragment-specific ACK
    if (h->fragCount > 1) {
      v3_send_frag_ack(recv_info->src_addr, h->msgId, h->fragIndex, h->fragCount);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_ACK] Sent ACK for fragment %u/%u msgId=%lu",
             h->fragIndex + 1, h->fragCount, (unsigned long)h->msgId);
    } else {
      v3_send_ack(recv_info->src_addr, h->msgId);
    }
  }
  
  // Dedup check
  // STREAM frames intentionally reuse msgId=cmdMsgId for correlation and are multi-frame.
  // If we dedup STREAM by msgId, STREAM_BEGIN (payloadLen=0) will cause the sender to drop
  // all subsequent stream data frames and even CMD_RESP with the same msgId.
  if (h->type != ESPNOW_V3_TYPE_STREAM &&
      h->type != ESPNOW_V3_TYPE_FILE_START &&
      h->type != ESPNOW_V3_TYPE_FILE_DATA &&
      h->type != ESPNOW_V3_TYPE_FILE_END) {
    if (h->msgId != 0 && v3_dedup_seen_and_insert(h->origin, h->msgId)) {
      DEBUG_ESPNOWF("[V3_DEDUP] Dropped duplicate: type=%u msgId=%lu", h->type, (unsigned long)h->msgId);
      return true;
    }
  } else {
    DEBUG_ESPNOWF("[V3_DEDUP] Bypassed for type=%u msgId=%lu", h->type, (unsigned long)h->msgId);
  }
  
  if (!gEspNow || !gEspNow->initialized) return true;
  
  // Resolve device name
  bool isPaired = false; 
  const char* deviceName = nullptr;
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (memcmp(gEspNow->devices[i].mac, recv_info->src_addr, 6) == 0) { 
      isPaired = true; 
      deviceName = gEspNow->devices[i].name.c_str(); 
      break; 
    }
  }
  char macStrBuf[18]; 
  if (!deviceName || !deviceName[0]) { 
    formatMacAddressBuf(recv_info->src_addr, macStrBuf, sizeof(macStrBuf)); 
    deviceName = macStrBuf; 
  }
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_RX] Source: %s (paired=%s encrypted=%s)",
         deviceName, isPaired ? "YES" : "NO", 
         (h->flags & ESPNOW_V3_FLAG_ENCRYPTED) ? "YES" : "NO");

#if ENABLE_BONDED_MODE
  // Track when bond-type messages arrive but would be rejected due to isPaired=false
  // Can't use BROADCAST_PRINTF here (ISR-like context) — use deferred counter instead
  if (!isPaired && (h->type == ESPNOW_V3_TYPE_BOND_HEARTBEAT ||
                    h->type == ESPNOW_V3_TYPE_BOND_CAP_REQ ||
                    h->type == ESPNOW_V3_TYPE_BOND_CAP_RESP ||
                    h->type == ESPNOW_V3_TYPE_MANIFEST_REQ ||
                    h->type == ESPNOW_V3_TYPE_SETTINGS_REQ)) {
    static volatile uint32_t sBondUnpairedRejectCount = 0;
    static volatile uint8_t  sBondUnpairedRejectType = 0;
    static volatile uint8_t  sBondUnpairedRejectMac[6] = {};
    sBondUnpairedRejectCount++;
    sBondUnpairedRejectType = h->type;
    memcpy((void*)sBondUnpairedRejectMac, recv_info->src_addr, 6);
    // Also set a flag on gEspNow for the task loop to pick up
    if (gEspNow) {
      gEspNow->bondUnpairedRejectCount = sBondUnpairedRejectCount;
      gEspNow->bondUnpairedRejectType = sBondUnpairedRejectType;
      memcpy(gEspNow->bondUnpairedRejectMac, (const void*)sBondUnpairedRejectMac, 6);
    }
  }
#endif

  // === TEXT MESSAGE ===
  // NOTE: Deferred to task context - callback is ISR-like with limited stack
  if (h->type == ESPNOW_V3_TYPE_TEXT) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_RX] TEXT message detected, payload: %.80s", 
           payloadLen > 0 ? (const char*)payload : "(empty)");
    if (payloadLen > 0 && payloadLen <= ESPNOW_V3_MAX_PAYLOAD && gEspNow) {
      int head = gEspNow->textQueueHead;
      int nextHead = (head + 1) & (EspNowState::TEXT_QUEUE_SIZE - 1);
      if (nextHead != gEspNow->textQueueTail) {
        auto& slot = gEspNow->textQueue[head];
        size_t copyLen = (payloadLen < sizeof(slot.content) - 1) ? payloadLen : sizeof(slot.content) - 1;
        memcpy(slot.content, payload, copyLen);
        slot.content[copyLen] = '\0';
        memcpy(slot.srcMac, recv_info->src_addr, 6);
        strncpy(slot.deviceName, deviceName, sizeof(slot.deviceName) - 1);
        slot.deviceName[sizeof(slot.deviceName) - 1] = '\0';
        slot.encrypted = (h->flags & ESPNOW_V3_FLAG_ENCRYPTED) != 0;
        slot.used = true;
        gEspNow->textQueueHead = nextHead;
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_RX] TEXT message enqueued slot=%d (encrypted=%s)",
               head, slot.encrypted ? "YES" : "NO");
      } else {
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_RX] TEXT queue full, message dropped");
      }
    } else {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_RX] TEXT message REJECTED: payloadLen=%u gEspNow=%p",
             payloadLen, gEspNow);
    }
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_RX] ========================================");
    // Cleanup reassembly buffer if this was a fragmented message
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }

  // === COMMAND REQUEST ===
  // NOTE: Deferred to task context - callback is ISR-like with limited stack
  // v3_handle_cmd has malloc, String ops, auth checks - all ISR-unsafe
  if (h->type == ESPNOW_V3_TYPE_CMD && isPaired) {
    if (payloadLen > 0 && payloadLen <= ESPNOW_V3_MAX_PAYLOAD && gEspNow && !gEspNow->deferredCmdPending) {
      size_t copyLen = (payloadLen < sizeof(gEspNow->deferredCmdPayload) - 1) ? payloadLen : sizeof(gEspNow->deferredCmdPayload) - 1;
      memcpy(gEspNow->deferredCmdPayload, payload, copyLen);
      gEspNow->deferredCmdPayload[copyLen] = '\0';
      memcpy(gEspNow->deferredCmdSrcMac, recv_info->src_addr, 6);
      strncpy(gEspNow->deferredCmdDeviceName, deviceName, sizeof(gEspNow->deferredCmdDeviceName) - 1);
      gEspNow->deferredCmdDeviceName[sizeof(gEspNow->deferredCmdDeviceName) - 1] = '\0';
      gEspNow->deferredCmdMsgId = h->msgId;
      gEspNow->deferredCmdPending = true;
    }
    // Cleanup reassembly buffer if this was a fragmented message
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }

  // === COMMAND RESPONSE ===
  // NOTE: Deferred to task context - callback is ISR-like with limited stack
  if (h->type == ESPNOW_V3_TYPE_CMD_RESP) {
    if (payloadLen >= 1 && gEspNow) {
      const V3PayloadCmdResp* resp = (const V3PayloadCmdResp*)payload;
      size_t resultLen = payloadLen - 1;
      if (resultLen > 2047) resultLen = 2047;
      if (!gEspNow->deferredCmdRespResult) { gEspNow->deferredCmdRespPending = false; return true; }
      memcpy(gEspNow->deferredCmdRespResult, resp->result, resultLen);
      gEspNow->deferredCmdRespResult[resultLen] = '\0';
      memcpy(gEspNow->deferredCmdRespSrcMac, recv_info->src_addr, 6);
      strncpy(gEspNow->deferredCmdRespDeviceName, deviceName, sizeof(gEspNow->deferredCmdRespDeviceName) - 1);
      gEspNow->deferredCmdRespDeviceName[sizeof(gEspNow->deferredCmdRespDeviceName) - 1] = '\0';
      gEspNow->deferredCmdRespSuccess = resp->success;
      gEspNow->deferredCmdRespPending = true;
    }
    // Cleanup reassembly buffer if this was a fragmented message
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }

  // === TIME SYNC ===
  if (h->type == ESPNOW_V3_TYPE_TIME_SYNC) {
    DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_TIME_SYNC] Received from %s msgId=%lu payloadLen=%u",
           deviceName, (unsigned long)h->msgId, payloadLen);
    if (payloadLen >= sizeof(V3PayloadTimeSync)) {
      const V3PayloadTimeSync* ts = (const V3PayloadTimeSync*)payload;
      DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_TIME_SYNC] epochTime=%lu timeOffset=%ld",
             (unsigned long)ts->epochTime, (long)ts->timeOffset);
      DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_TIME_SYNC] Previous time state: synced=%s offset=%lld",
             gTimeIsSynced ? "YES" : "NO", (long long)gTimeOffset);
      gTimeOffset = (int64_t)ts->timeOffset;
      gTimeIsSynced = true;
      DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_TIME_SYNC] Time sync applied successfully");
    } else {
      DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_TIME_SYNC] ERROR: Payload too small (%u < %u)",
             payloadLen, (unsigned)sizeof(V3PayloadTimeSync));
    }
    // Cleanup reassembly buffer if this was a fragmented message
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }
  
  // === TOPOLOGY REQUEST ===
  if (h->type == ESPNOW_V3_TYPE_TOPO_REQ) {
    DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_REQ] Received from %s msgId=%lu payloadLen=%u",
           deviceName, (unsigned long)h->msgId, payloadLen);
    if (payloadLen >= sizeof(V3PayloadTopoReq)) {
      const V3PayloadTopoReq* tr = (const V3PayloadTopoReq*)payload;
      DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_REQ] reqId=%lu", (unsigned long)tr->reqId);
      
      // Count active peers (excluding self)
      int peerCount = 0;
      for (int i = 0; i < gMeshPeerSlots; i++) {
        if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac)) {
          peerCount++;
        }
      }
      
      DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_REQ] Responding with %d peer(s)", peerCount);
      
      // Send TOPO_START to requester
      v3_send_topo_start(recv_info->src_addr, tr->reqId, (uint8_t)peerCount);
      
      // Send one TOPO_PEER per active peer
      int peerIndex = 0;
      for (int i = 0; i < gMeshPeerSlots; i++) {
        if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac)) {
          bool isLast = (peerIndex == peerCount - 1);
          String peerName = getEspNowDeviceName(gMeshPeers[i].mac);
          MeshPeerHealth* ph = getMeshPeerHealth(gMeshPeers[i].mac, false);
          int8_t rssi = ph ? ph->rssi : 0;
          v3_send_topo_peer(recv_info->src_addr, tr->reqId, (uint8_t)peerIndex,
                            isLast, gMeshPeers[i].mac, rssi,
                            false, peerName.length() > 0 ? peerName.c_str() : "Unknown");
          peerIndex++;
        }
      }
      
      DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_REQ] Sent %d TOPO_PEER frame(s)", peerIndex);
    } else {
      DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_REQ] ERROR: Payload too small (%u < %u)",
             payloadLen, (unsigned)sizeof(V3PayloadTopoReq));
    }
    // Cleanup reassembly buffer if this was a fragmented message
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }
  
  // === TOPOLOGY START (response from peer) ===
  if (h->type == ESPNOW_V3_TYPE_TOPO_START) {
    DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_START] Received from %s msgId=%lu payloadLen=%u",
           deviceName, (unsigned long)h->msgId, payloadLen);
    if (payloadLen >= sizeof(V3PayloadTopoStart)) {
      const V3PayloadTopoStart* ts = (const V3PayloadTopoStart*)payload;
      DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_START] reqId=%lu peerCount=%u",
             (unsigned long)ts->reqId, ts->peerCount);
      
      // Check if this matches our active request
      if (ts->reqId != gTopoRequestId || millis() >= gTopoRequestTimeout) {
        DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_START] Rejected: reqId mismatch or timeout");
      } else {
        // Find or create stream for this sender
        TopologyStream* stream = findOrCreateTopoStream(recv_info->src_addr, ts->reqId);
        if (stream && stream->active) {
          // Initialize stream if newly created
          if (stream->receivedPeers == 0 && stream->totalPeers == 0) {
            String senderName = String(deviceName);
            if (senderName.length() == 0) {
              senderName = getEspNowDeviceName(recv_info->src_addr);
            }
            if (senderName.length() == 0) {
              char macBuf[18];
              formatMacAddressBuf(recv_info->src_addr, macBuf, sizeof(macBuf));
              senderName = macBuf;
            }
            strncpy(stream->senderName, senderName.c_str(), 31);
            stream->senderName[31] = '\0';
            stream->totalPeers = ts->peerCount;
            stream->accumulatedData = "";
            
            // Cache device name for topology display
            if (senderName.length() > 0) {
              addTopoDeviceName(recv_info->src_addr, senderName.c_str());
            }
            
            DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_START] Stream initialized for %s: expecting %d peers",
                   stream->senderName, ts->peerCount);
          }
          
          // If 0 peers, finalize immediately (edge device)
          if (ts->peerCount == 0) {
            DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_START] 0 peers - edge device, finalizing");
            finalizeTopologyStream(stream);
          }
        } else {
          DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_START] ERROR: Could not allocate stream");
        }
      }
    } else {
      DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_START] ERROR: Payload too small (%u < %u)",
             payloadLen, (unsigned)sizeof(V3PayloadTopoStart));
    }
    // Cleanup reassembly buffer if this was a fragmented message
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }
  
  // === TOPOLOGY PEER (response from peer) ===
  if (h->type == ESPNOW_V3_TYPE_TOPO_PEER) {
    DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_PEER] Received from %s msgId=%lu payloadLen=%u",
           deviceName, (unsigned long)h->msgId, payloadLen);
    if (payloadLen >= sizeof(V3PayloadTopoPeer)) {
      const V3PayloadTopoPeer* tp = (const V3PayloadTopoPeer*)payload;
      char peerMacStr[18];
      formatMacAddressBuf(tp->mac, peerMacStr, sizeof(peerMacStr));
      DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_PEER] reqId=%lu idx=%u isLast=%u mac=%s name=%s",
             (unsigned long)tp->reqId, tp->peerIndex, tp->isLast, peerMacStr, tp->name);
      
      // Check if this matches our active request
      if (tp->reqId != gTopoRequestId || millis() >= gTopoRequestTimeout) {
        DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_PEER] Rejected: reqId mismatch or timeout");
      } else {
        // Find stream for this sender
        TopologyStream* stream = findTopoStream(recv_info->src_addr, tp->reqId);
        if (stream && stream->active) {
          // Check for duplicate
          if (stream->accumulatedData.indexOf(peerMacStr) != -1) {
            DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_PEER] Duplicate peer %s, skipping", peerMacStr);
          } else {
            // Get peer name
            String peerName = String(tp->name);
            if (peerName.length() == 0 || peerName == "Unknown") {
              peerName = getTopoDeviceName(tp->mac);
              if (peerName.length() == 0) peerName = getEspNowDeviceName(tp->mac);
              if (peerName.length() == 0) peerName = "Unknown";
            }
            
            // Cache peer name
            if (peerName != "Unknown") {
              addTopoDeviceName(tp->mac, peerName.c_str());
            }
            
            // Accumulate peer info (same format as JSON handler)
            char peerInfoBuf[128];
            snprintf(peerInfoBuf, sizeof(peerInfoBuf), "  \xe2\x86\x92 %s (%s)\n    RSSI: %d dBm\n",
                     peerName.c_str(), peerMacStr, (int)tp->rssi);
            stream->accumulatedData += peerInfoBuf;
            stream->receivedPeers++;
            
            // Update collection window timer
            gTopoLastResponseTime = millis();
            
            DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_PEER] Accumulated peer %d/%d: %s",
                   stream->receivedPeers, stream->totalPeers, peerName.c_str());
          }
        } else {
          DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_PEER] No active stream for this sender");
        }
      }
    } else {
      DEBUGF(DEBUG_ESPNOW_TOPO, "[V3_RX_TOPO_PEER] ERROR: Payload too small (%u < %u)",
             payloadLen, (unsigned)sizeof(V3PayloadTopoPeer));
    }
    // Cleanup reassembly buffer if this was a fragmented message
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }
  
  // === SENSOR STATUS (mesh broadcast) ===
  if (h->type == ESPNOW_V3_TYPE_SENSOR_STATUS) {
    DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_SENSOR_STATUS] Received from %s msgId=%lu payloadLen=%u",
           deviceName, (unsigned long)h->msgId, payloadLen);
    if (payloadLen >= sizeof(V3PayloadSensorStatus)) {
      const V3PayloadSensorStatus* ss = (const V3PayloadSensorStatus*)payload;
      RemoteSensorType sensorType = (RemoteSensorType)ss->sensorType;
      bool enabled = (ss->enabled != 0);
      extern const char* sensorTypeToString(RemoteSensorType type);
      DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_SENSOR_STATUS] sensor=%s enabled=%s",
             sensorTypeToString(sensorType), enabled ? "YES" : "NO");
      
      DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_SENSOR_STATUS] Updating remote sensor status");
      extern void updateRemoteSensorStatus(const uint8_t* mac, const char* name, RemoteSensorType type, bool enabled);
      updateRemoteSensorStatus(recv_info->src_addr, deviceName, sensorType, enabled);
      DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_SENSOR_STATUS] Status update complete");
    } else {
      DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_SENSOR_STATUS] ERROR: Payload too small (%u < %u)",
             payloadLen, (unsigned)sizeof(V3PayloadSensorStatus));
    }
    // Cleanup reassembly buffer if this was a fragmented message
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }
  
  // === SENSOR BROADCAST (mesh data) ===
  if (h->type == ESPNOW_V3_TYPE_SENSOR_BROADCAST) {
    DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_SENSOR_BROADCAST] Received from %s msgId=%lu payloadLen=%u",
           deviceName, (unsigned long)h->msgId, payloadLen);
    if (payloadLen >= sizeof(V3PayloadSensorBroadcast)) {
      const V3PayloadSensorBroadcast* sb = (const V3PayloadSensorBroadcast*)payload;
      RemoteSensorType sensorType = (RemoteSensorType)sb->sensorType;
      uint16_t dataLen = sb->dataLen;
      extern const char* sensorTypeToString(RemoteSensorType type);
      DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_SENSOR_BROADCAST] sensor=%s dataLen=%u",
             sensorTypeToString(sensorType), dataLen);
      
      if (dataLen > 0 && payloadLen >= (sizeof(V3PayloadSensorBroadcast) + dataLen)) {
        const char* jsonData = (const char*)sb->data;
        DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_SENSOR_BROADCAST] JSON (first 100 chars): %.100s", jsonData);
        
        DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_SENSOR_BROADCAST] Caching sensor data");
        extern RemoteSensorData* findOrCreateCacheEntry(const uint8_t* mac, const char* name, RemoteSensorType type);
        RemoteSensorData* entry = findOrCreateCacheEntry(recv_info->src_addr, deviceName, sensorType);
        if (entry) {
          size_t copyLen = (dataLen < REMOTE_SENSOR_BUFFER_SIZE - 1) ? dataLen : REMOTE_SENSOR_BUFFER_SIZE - 1;
          memcpy(entry->jsonData, jsonData, copyLen);
          entry->jsonData[copyLen] = '\0';
          entry->jsonLength = (uint16_t)copyLen;
          entry->lastUpdate = millis();
          entry->valid = true;
          DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_SENSOR_BROADCAST] Data cached successfully (%u bytes)", (unsigned)copyLen);
        } else {
          DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_SENSOR_BROADCAST] ERROR: Failed to allocate cache entry");
        }
      } else {
        DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_SENSOR_BROADCAST] ERROR: Invalid data length (%u) or truncated payload",
               dataLen);
      }
    } else {
      DEBUGF(DEBUG_ESPNOW_MESH, "[V3_RX_SENSOR_BROADCAST] ERROR: Payload too small (%u < %u)",
             payloadLen, (unsigned)sizeof(V3PayloadSensorBroadcast));
    }
    // Cleanup reassembly buffer if this was a fragmented message
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }

  // === HEARTBEAT ===
  if (h->type == ESPNOW_V3_TYPE_HEARTBEAT) {
    if (payloadLen >= sizeof(V3PayloadHeartbeat)) {
      const V3PayloadHeartbeat* hb = (const V3PayloadHeartbeat*)payload;
      MeshPeerHealth* peer = getMeshPeerHealth(recv_info->src_addr, true);
      if (peer) {
        peer->lastHeartbeatMs = millis();
        peer->heartbeatCount++;
        peer->rssi = hb->rssi;
        peer->isActive = true;
      }
      if (gEspNow) gEspNow->heartbeatsReceived++;

      // Backup master failover: track heartbeats from the configured master MAC
      if (meshEnabled() && gSettings.meshBackupEnabled &&
          gSettings.meshMasterMAC.length() > 0) {
        bool isBackupOrPromoted = (gSettings.meshRole == MESH_ROLE_BACKUP_MASTER) ||
                                   (gSettings.meshRole == MESH_ROLE_MASTER && gBackupPromoted);
        if (isBackupOrPromoted) {
          uint8_t masterMac[6] = {};
          if (parseMacAddress(gSettings.meshMasterMAC, masterMac) &&
              memcmp(recv_info->src_addr, masterMac, 6) == 0) {
            gLastMasterHeartbeat = millis();
            if (gBackupPromoted) {
              // Original master is back — demote to backup role (runtime only, not saved)
              gBackupPromoted = false;
              gSettings.meshRole = MESH_ROLE_BACKUP_MASTER;
              BROADCAST_PRINTF("[BACKUP] Master returned — demoted back to backup role");
            }
          }
        }
      }
    }
    // Send ACK if requested
    if (h->flags & ESPNOW_V3_FLAG_ACK_REQ) {
      v3_send_ack(recv_info->src_addr, h->msgId);
    }
    // Cleanup reassembly buffer if this was a fragmented message
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }

#if ENABLE_BONDED_MODE
  // === BOND HEARTBEAT ===
  if (h->type == ESPNOW_V3_TYPE_BOND_HEARTBEAT && isPaired && gSettings.bondModeEnabled) {
    DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_HB_RX] from %s payloadLen=%u (need %u) isPaired=%d",
           deviceName, payloadLen, (unsigned)sizeof(V3PayloadBondHeartbeat), (int)isPaired);
    if (payloadLen >= sizeof(V3PayloadBondHeartbeat)) {
      const V3PayloadBondHeartbeat* hb = (const V3PayloadBondHeartbeat*)payload;
      if (gEspNow) {
        bool bootChanged = (hb->bootCounter != 0 && gEspNow->bondPeerBootCounter != 0 &&
                            hb->bootCounter != gEspNow->bondPeerBootCounter);
        bool wasOffline = !gEspNow->bondPeerOnline || bootChanged;
        uint32_t oldSettingsHash = gEspNow->bondPeerSettingsHash;

        gEspNow->bondPeerBootCounter = hb->bootCounter;
        gEspNow->bondPeerSettingsHash = hb->settingsHash;
        gEspNow->bondPeerUptime = hb->uptimeSec;

        if (bootChanged) {
          resetBondSync();
        }

        // Detect live settings change: peer's hash changed while we had their settings cached
        bool settingsChanged = (gEspNow->bondSettingsReceived && oldSettingsHash != 0 &&
                                hb->settingsHash != 0 && hb->settingsHash != oldSettingsHash);
        if (settingsChanged && !bootChanged) {
          gEspNow->bondSettingsReceived = false;
          gEspNow->bondSyncInFlight = BOND_SYNC_NONE;
          gEspNow->bondSyncRetryCount = 0;
          DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_HB_RX] Peer settings hash changed 0x%08lX->0x%08lX, will re-fetch",
                 (unsigned long)oldSettingsHash, (unsigned long)hb->settingsHash);
        }

        gEspNow->lastBondHeartbeatReceivedMs = millis();
        gEspNow->bondHeartbeatsReceived++;
        gEspNow->bondPeerOnline = true;
        
        // Update RSSI from rx_ctrl
        if (recv_info->rx_ctrl) {
          gEspNow->bondRssiLast = recv_info->rx_ctrl->rssi;
          if (gEspNow->bondLastRssiUpdateMs == 0) {
            gEspNow->bondRssiAvg = gEspNow->bondRssiLast;
          } else {
            gEspNow->bondRssiAvg = (int8_t)((9 * (int)gEspNow->bondRssiAvg + (int)gEspNow->bondRssiLast) / 10);
          }
          gEspNow->bondLastRssiUpdateMs = millis();
        }
        
        // Worker recovery: if caps are exchanged but bondSettingsSent was never
        // set (e.g. firmware update without wipe on an already-synced session),
        // infer that settings were already sent and complete the worker sync.
        // Time gate: only after 30s of caps exchanged, to avoid false-triggering
        // during normal handshake when the master just hasn't asked for settings yet.
        bool capExchangedLongEnough = (gEspNow->lastRemoteCapTime > 0 &&
                                       (millis() - gEspNow->lastRemoteCapTime) > 30000);
        if (gSettings.bondRole == 0 && !gEspNow->bondSessionTokenValid &&
            gEspNow->lastRemoteCapValid && gEspNow->bondCapSent &&
            !gEspNow->bondSettingsSent && capExchangedLongEnough) {
          gEspNow->bondSettingsSent = true;
          if (isBondSynced()) {
            uint8_t pMac[6];
            if (parseMacAddress(gSettings.bondPeerMac, pMac)) {
              computeBondSessionToken(pMac);
            }
            BROADCAST_PRINTF("[BOND_SYNC] *** SYNC COMPLETE *** role=0 (worker, recovered)");
          }
        }
        
        if (wasOffline) {
          // Deferred: master starts sync tick
          if (gSettings.bondRole == 1) {
            gEspNow->bondNeedsCapabilityRequest = true;
            // bondNeedsStreamingSetup is set after sync completes in processBondSettings()
          }
        }
      }
    }
    // Cleanup reassembly buffer if this was a fragmented message
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }
#endif // ENABLE_BONDED_MODE

#if ENABLE_BONDED_MODE  
  // === BOND CAP REQ ===
  // NOTE: Defer heavy work to main loop - callback context has limited stack and is ISR-like
  if (h->type == ESPNOW_V3_TYPE_BOND_CAP_REQ && isPaired && gSettings.bondModeEnabled) {
    DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_CAP_REQ_RX] from %s role=%d capSent=%d",
           deviceName, (int)gSettings.bondRole,
           gEspNow ? (int)gEspNow->bondCapSent : -1);
    if (gEspNow) {
      memcpy(gEspNow->bondPendingResponseMac, recv_info->src_addr, 6);
      gEspNow->bondNeedsCapabilityResponse = true;
    }
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }

  // === BOND CAP RESP ===
  // NOTE: Defer heavy work to main loop - callback context is ISR-like
  if (h->type == ESPNOW_V3_TYPE_BOND_CAP_RESP && isPaired && gSettings.bondModeEnabled && payloadLen == sizeof(CapabilitySummary)) {
    const CapabilitySummary* cap = (const CapabilitySummary*)payload;
    DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_CAP_RESP_RX] from %s role=%d capSent=%d sensorMask=0x%04lX devName='%.16s'",
           deviceName, (int)gSettings.bondRole,
           gEspNow ? (int)gEspNow->bondCapSent : -1,
           (unsigned long)cap->sensorMask, cap->deviceName);
    if (gEspNow) {
      memcpy(&gEspNow->lastRemoteCap, cap, sizeof(CapabilitySummary));
      gEspNow->lastRemoteCapValid = true;
      gEspNow->lastRemoteCapTime = millis();
      gEspNow->bondReceivedCapability = true;  // Defer logging
      gEspNow->bondSyncInFlight = BOND_SYNC_NONE;
      gEspNow->bondSyncRetryCount = 0;
      gEspNow->bondSyncLastAttemptMs = 0;
      // Send our own CAP_RESP back so the peer also gets our capabilities.
      // Without this, the master never learns the worker's caps (one-directional exchange).
      // bondCapSent prevents infinite ping-pong: each side sends at most once per handshake.
      if (!gEspNow->bondCapSent) {
        memcpy(gEspNow->bondPendingResponseMac, recv_info->src_addr, 6);
        gEspNow->bondNeedsCapabilityResponse = true;
        DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_CAP_RESP_RX] queued reciprocal CAP_RESP (bondCapSent was false)");
      } else {
        DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_CAP_RESP_RX] NOT sending reciprocal (bondCapSent already true)");
      }
    }
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }

  // === SENSOR DATA (bond mode binary streaming) ===
  // NOTE: This runs in ESP-NOW callback context - minimize debug prints!
  if (h->type == ESPNOW_V3_TYPE_SENSOR_DATA && isPaired && gSettings.bondModeEnabled) {
    if (payloadLen >= sizeof(V3PayloadSensorData)) {
      const V3PayloadSensorData* sd = (const V3PayloadSensorData*)payload;
      
      // Validate data length
      size_t totalExpected = sizeof(V3PayloadSensorData) + sd->dataLen;
      if (sd->dataLen > 0 && totalExpected <= payloadLen) {
        // Store in remote sensor cache (reuse existing mesh infrastructure)
        RemoteSensorType sensorType = (RemoteSensorType)sd->sensorType;
        
        if (sensorType < REMOTE_SENSOR_MAX) {
          RemoteSensorData* entry = findOrCreateCacheEntry(recv_info->src_addr, deviceName, sensorType);
          if (entry) {
            // Copy data directly into cache (it's already JSON-encoded by sender)
            size_t copyLen = (sd->dataLen < REMOTE_SENSOR_BUFFER_SIZE - 1) ? sd->dataLen : REMOTE_SENSOR_BUFFER_SIZE - 1;
            memcpy(entry->jsonData, sd->data, copyLen);
            entry->jsonData[copyLen] = '\0';
            entry->jsonLength = (uint16_t)copyLen;
            entry->lastUpdate = millis();
            entry->valid = true;
            
            // Single concise debug line - safe in callback context
            DEBUGF(DEBUG_ESPNOW_MESH, "[BOND] Sensor %s from %s len=%u seq=%lu",
                   sensorTypeToString(sensorType), deviceName, (unsigned)copyLen, (unsigned long)sd->seqNum);
          }
        }
      }
    }
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }

  // === STREAM CONTROL (bond mode: master -> worker) ===
  // NOTE: Deferred to task context - startSensorDataStreaming creates tasks/mutexes
  if (h->type == ESPNOW_V3_TYPE_STREAM_CTRL && isPaired && gSettings.bondModeEnabled) {
    if (payloadLen >= 2 && gEspNow) {
      gEspNow->bondDeferredStreamCtrlSensor = payload[0];  // sensorType
      gEspNow->bondDeferredStreamCtrlEnable = payload[1];   // enable
      gEspNow->bondDeferredStreamCtrlPending = true;
      DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_STREAM_CTRL_RX] sensor=%u enable=%u (deferred)", payload[0], payload[1]);
    }
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }

  // === SETTINGS REQUEST ===
  // NOTE: Defer to task context - callback is ISR-like with limited stack
  if (h->type == ESPNOW_V3_TYPE_SETTINGS_REQ && isPaired && gSettings.bondModeEnabled) {
    DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_SETTINGS_REQ_RX] from %s isPaired=%d", deviceName, (int)isPaired);
    if (gEspNow) {
      memcpy(gEspNow->bondPendingResponseMac, recv_info->src_addr, 6);
      gEspNow->bondNeedsSettingsResponse = true;
      DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_SETTINGS_REQ_RX] set bondNeedsSettingsResponse=true");
    }
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }

  // === SETTINGS RESPONSE/PUSH ===
  // NOTE: Settings are sent via file transfer, so they arrive as FILE_END
  // The file path will indicate it's a settings file (/system/_settings_out.json)
  // Processing happens in the FILE_END handler below

  // === BOND STATUS REQ ===
  if (h->type == ESPNOW_V3_TYPE_BOND_STATUS_REQ && isPaired && gSettings.bondModeEnabled) {
    DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_STATUS_REQ_RX] from %s", deviceName);
    if (gEspNow) {
      memcpy(gEspNow->bondPendingResponseMac, recv_info->src_addr, 6);
      gEspNow->bondNeedsStatusResponse = true;
    }
    return true;
  }

  // === BOND STATUS RESP ===
  if (h->type == ESPNOW_V3_TYPE_BOND_STATUS_RESP && isPaired && gSettings.bondModeEnabled) {
    if (payloadLen >= sizeof(BondPeerStatus) && gEspNow) {
      memcpy(&gEspNow->bondPeerStatus, payload, sizeof(BondPeerStatus));
      gEspNow->bondPeerStatusValid = true;
      gEspNow->bondPeerStatusTimeMs = millis();
      BROADCAST_PRINTF("[BOND_STATUS_RESP_RX] from %s enabled=0x%04X connected=0x%04X heap=%lu",
             deviceName, gEspNow->bondPeerStatus.sensorEnabledMask,
             gEspNow->bondPeerStatus.sensorConnectedMask,
             (unsigned long)gEspNow->bondPeerStatus.freeHeap);
    }
    return true;
  }
#endif // ENABLE_BONDED_MODE

  // === METADATA REQUEST ===
  // Check if peer is encrypted by looking in device list
  bool isEncrypted = false;
  if (gEspNow) {
    for (int i = 0; i < gEspNow->deviceCount; i++) {
      if (memcmp(gEspNow->devices[i].mac, recv_info->src_addr, 6) == 0) {
        isEncrypted = gEspNow->devices[i].encrypted;
        break;
      }
    }
  }
  
  if (h->type == ESPNOW_V3_TYPE_METADATA_REQ) {
    DEBUG_ESPNOW_METADATAF("[METADATA] REQ received from %s (%s) msgId=%lu isPaired=%d isEncrypted=%d",
      deviceName, MAC_STR(recv_info->src_addr), (unsigned long)h->msgId,
      (int)isPaired, (int)isEncrypted);
    if (gEspNow) {
      memcpy(gEspNow->metadataPendingResponseMac, recv_info->src_addr, 6);
      gEspNow->bondNeedsMetadataResponse = true;
      DEBUG_ESPNOW_METADATAF("[METADATA] bondNeedsMetadataResponse=true, will RESP to %s",
        MAC_STR(recv_info->src_addr));
    } else {
      WARN_ESPNOWF("[METADATA] REQ from %s ignored: gEspNow is null", deviceName);
    }
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }

  // === METADATA RESPONSE/PUSH ===
  if ((h->type == ESPNOW_V3_TYPE_METADATA_RESP || h->type == ESPNOW_V3_TYPE_METADATA_PUSH)) {
    const char* metaType = (h->type == ESPNOW_V3_TYPE_METADATA_PUSH) ? "PUSH" : "RESP";
    DEBUG_ESPNOW_METADATAF("[METADATA] %s received from %s (%s) msgId=%lu payloadLen=%u (need %u) isPaired=%d",
      metaType, deviceName, MAC_STR(recv_info->src_addr),
      (unsigned long)h->msgId, payloadLen, (unsigned)sizeof(V3PayloadMetadata), (int)isPaired);
    if (payloadLen >= sizeof(V3PayloadMetadata)) {
      const V3PayloadMetadata* meta = (const V3PayloadMetadata*)payload;
      DEBUG_ESPNOW_METADATAF("[METADATA] %s payload: name='%s' friendlyName='%s' room='%s' zone='%s' tags='%s' stationary=%d",
        metaType, meta->deviceName, meta->friendlyName, meta->room, meta->zone, meta->tags, (int)meta->stationary);
      if (gEspNow) {
        bool wasPending = gEspNow->deferredMetadataPending;
        memcpy(gEspNow->deferredMetadataSrcMac, recv_info->src_addr, 6);
        memcpy(&gEspNow->deferredMetadataPayload, meta, sizeof(V3PayloadMetadata));
        gEspNow->deferredMetadataPending = true;
        DEBUG_ESPNOW_METADATAF("[METADATA] %s deferred for task processing (overwrote=%d)",
          metaType, (int)wasPending);
      } else {
        WARN_ESPNOWF("[METADATA] %s from %s dropped: gEspNow is null", metaType, deviceName);
      }
    } else {
      WARN_ESPNOWF("[METADATA] %s from %s REJECTED: payload too small (%u < %u)",
        metaType, deviceName, payloadLen, (unsigned)sizeof(V3PayloadMetadata));
    }
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }

  // === STREAM OUTPUT ===
  // NOTE: Deferred to task context - callback is ISR-like with limited stack
  // Uses ring buffer queue to prevent message loss when frames arrive faster than task processes
  if (h->type == ESPNOW_V3_TYPE_STREAM) {
    if (payloadLen > 0 && payloadLen <= ESPNOW_V3_MAX_PAYLOAD && gEspNow) {
      int head = gEspNow->streamQueueHead;
      int nextHead = (head + 1) & (EspNowState::STREAM_QUEUE_SIZE - 1);
      if (nextHead != gEspNow->streamQueueTail) {
        // Slot available - enqueue
        auto& entry = gEspNow->streamQueue[head];
        size_t copyLen = (payloadLen < sizeof(entry.content) - 1) ? payloadLen : sizeof(entry.content) - 1;
        memcpy(entry.content, payload, copyLen);
        entry.content[copyLen] = '\0';
        memcpy(entry.srcMac, recv_info->src_addr, 6);
        strncpy(entry.deviceName, deviceName, sizeof(entry.deviceName) - 1);
        entry.deviceName[sizeof(entry.deviceName) - 1] = '\0';
        entry.used = true;
        gEspNow->streamQueueHead = nextHead;  // Publish to consumer
      }
      // else: queue full, drop this frame (better than overwriting)
      gEspNow->streamReceivedCount++;
    }
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }

  // === V3 FILE_START ===
  // Buffer file data in PSRAM to avoid filesystem I/O in callback (causes watchdog timeout)
  if (h->type == ESPNOW_V3_TYPE_FILE_START && isPaired) {
    if (payloadLen >= sizeof(V3PayloadFileStart)) {
      const V3PayloadFileStart* fs = (const V3PayloadFileStart*)payload;
      
      // Check for stale transfer (timeout after 30 seconds) or same sender restarting
      if (gActiveFileTransfer) {
        bool isStale = (millis() - gActiveFileTransfer->startTime) > 30000;
        bool sameSender = (memcmp(gActiveFileTransfer->senderMac, recv_info->src_addr, 6) == 0);
        
        if (!isStale && !sameSender) {
          // Different sender trying to start while transfer in progress - reject
          ERROR_ESPNOWF("[V3_FILE] Rejected: transfer already in progress from different sender");
          return true;
        }
        
        // Cleanup stale or same-sender transfer
        if (gActiveFileTransfer->chunkMap) {
          heap_caps_free(gActiveFileTransfer->chunkMap);
          gActiveFileTransfer->chunkMap = nullptr;
        }
        if (gActiveFileTransfer->dataBuffer) {
          heap_caps_free(gActiveFileTransfer->dataBuffer);
        }
        delete gActiveFileTransfer;
        gActiveFileTransfer = nullptr;
      }
      
      // Reject files larger than 64KB (PSRAM limit for single transfer)
      if (fs->fileSize > 65536) {
        ERROR_ESPNOWF("[V3_FILE] File too large for buffer: %lu bytes (max 64KB)", (unsigned long)fs->fileSize);
        return true;
      }
      
      // Allow zero-size files but log warning (unusual but valid)
      if (fs->fileSize == 0) {
        DEBUG_ESPNOWF("[V3_FILE] Warning: zero-size file transfer: %s", fs->filename);
      }

      if (fs->chunkSize == 0 || (fs->chunkCount == 0 && fs->fileSize > 0)) {
        ERROR_ESPNOWF("[V3_FILE] Rejected invalid chunk params: chunkSize=%u chunkCount=%u", (unsigned)fs->chunkSize, (unsigned)fs->chunkCount);
        return true;
      }
      
      // Allocate new transfer
      gActiveFileTransfer = new FileTransfer();
      if (!gActiveFileTransfer) {
        ERROR_ESPNOWF("[V3_FILE] Failed to allocate FileTransfer");
        return true;
      }
      
      memset(gActiveFileTransfer, 0, sizeof(FileTransfer));
      
      // Allocate PSRAM buffer for file data
      uint32_t allocSize = fs->fileSize > 0 ? fs->fileSize : 1;  // Allocate at least 1 byte (malloc(0) is undefined)
      gActiveFileTransfer->dataBuffer = (uint8_t*)heap_caps_malloc(allocSize, MALLOC_CAP_SPIRAM);
      if (!gActiveFileTransfer->dataBuffer) {
        ERROR_ESPNOWF("[V3_FILE] Failed to allocate %lu byte PSRAM buffer", (unsigned long)allocSize);
        delete gActiveFileTransfer;
        gActiveFileTransfer = nullptr;
        return true;
      }
      gActiveFileTransfer->bufferSize = fs->fileSize;
      
      strncpy(gActiveFileTransfer->filename, fs->filename, 63);
      gActiveFileTransfer->filename[63] = '\0';
      gActiveFileTransfer->totalSize = fs->fileSize;
      gActiveFileTransfer->totalChunks = fs->chunkCount;
      gActiveFileTransfer->chunkSize = fs->chunkSize;
      gActiveFileTransfer->receivedBytes = 0;
      gActiveFileTransfer->receivedChunks = 0;

      gActiveFileTransfer->chunkMap = nullptr;
      gActiveFileTransfer->chunkMapBytes = (uint16_t)((fs->chunkCount + 7) / 8);
      if (gActiveFileTransfer->chunkMapBytes == 0) gActiveFileTransfer->chunkMapBytes = 1;
      gActiveFileTransfer->chunkMap = (uint8_t*)heap_caps_malloc(gActiveFileTransfer->chunkMapBytes, MALLOC_CAP_8BIT);
      if (!gActiveFileTransfer->chunkMap) {
        ERROR_ESPNOWF("[V3_FILE] Failed to allocate chunk bitmap (%u bytes)", (unsigned)gActiveFileTransfer->chunkMapBytes);
        heap_caps_free(gActiveFileTransfer->dataBuffer);
        delete gActiveFileTransfer;
        gActiveFileTransfer = nullptr;
        return true;
      }
      memset(gActiveFileTransfer->chunkMap, 0, gActiveFileTransfer->chunkMapBytes);
      snprintf(gActiveFileTransfer->hash, sizeof(gActiveFileTransfer->hash), "%lu", (unsigned long)h->msgId);
      gActiveFileTransfer->active = true;
      gActiveFileTransfer->startTime = millis();  // Track start time for timeout
      memcpy(gActiveFileTransfer->senderMac, recv_info->src_addr, 6);
      
      DEBUG_ESPNOWF("[V3_FILE_RX] FILE_START: %s (%lu bytes, %u chunks, chunkSize=%u) from %s",
                   fs->filename, (unsigned long)fs->fileSize, fs->chunkCount, fs->chunkSize, deviceName);
      DEBUG_ESPNOWF("[V3_FILE_RX] Allocated: dataBuffer=%lu bytes, chunkMap=%u bytes",
                   (unsigned long)gActiveFileTransfer->bufferSize, (unsigned)gActiveFileTransfer->chunkMapBytes);
    }
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }

  // === V3 FILE_DATA ===
  // Copy chunks into PSRAM buffer - NO filesystem I/O here
  if (h->type == ESPNOW_V3_TYPE_FILE_DATA) {
    if (!isPaired) {
      DEBUG_ESPNOWF("[V3_FILE_RX] FILE_DATA rejected: sender not paired");
      return true;
    }
    if (!gActiveFileTransfer || !gActiveFileTransfer->active || !gActiveFileTransfer->dataBuffer) {
      DEBUG_ESPNOWF("[V3_FILE_RX] FILE_DATA ignored: no active transfer (active=%d)",
                   gActiveFileTransfer ? gActiveFileTransfer->active : 0);
      return true;
    }
    
    // Validate sender MAC matches the transfer initiator (prevents cross-talk)
    if (memcmp(gActiveFileTransfer->senderMac, recv_info->src_addr, 6) != 0) {
      return true;  // Silently ignore data from different sender
    }
    
    if (payloadLen < 3) return true;  // Need at least chunkIndex + 1 byte data
    
    const V3PayloadFileData* fd = (const V3PayloadFileData*)payload;
    uint16_t dataLen = payloadLen - 2;  // Subtract chunkIndex size
    
    if (!gActiveFileTransfer->chunkMap || gActiveFileTransfer->chunkSize == 0 || gActiveFileTransfer->totalChunks == 0) {
      return true;
    }

    uint16_t idx = fd->chunkIndex;
    if (idx >= gActiveFileTransfer->totalChunks) {
      return true;
    }

    uint32_t offset = (uint32_t)idx * (uint32_t)gActiveFileTransfer->chunkSize;
    
    // Bounds check
    if (offset + dataLen > gActiveFileTransfer->bufferSize) {
      ERROR_ESPNOWF("[V3_FILE] Buffer overflow: offset=%lu + len=%u > size=%lu",
                   (unsigned long)offset, dataLen, (unsigned long)gActiveFileTransfer->bufferSize);
      return true;
    }
    
    uint16_t byteIndex = (uint16_t)(idx / 8);
    uint8_t bitMask = (uint8_t)(1u << (idx % 8));
    bool alreadyHave = (byteIndex < gActiveFileTransfer->chunkMapBytes) && ((gActiveFileTransfer->chunkMap[byteIndex] & bitMask) != 0);

    if (alreadyHave) {
      DEBUG_ESPNOWF("[V3_FILE_RX] Chunk %u: DUPLICATE", idx);
    } else {
      DEBUG_ESPNOWF("[V3_FILE_RX] Chunk %u: offset=%lu len=%u", idx, (unsigned long)offset, dataLen);
    }

    memcpy(gActiveFileTransfer->dataBuffer + offset, fd->data, dataLen);
    if (!alreadyHave) {
      if (byteIndex < gActiveFileTransfer->chunkMapBytes) {
        gActiveFileTransfer->chunkMap[byteIndex] |= bitMask;
      }
      gActiveFileTransfer->receivedBytes += dataLen;
      gActiveFileTransfer->receivedChunks++;
      if ((gActiveFileTransfer->receivedChunks % 10) == 0) {
        DEBUG_ESPNOWF("[V3_FILE_RX] Progress: %u/%u chunks, %lu/%lu bytes",
                     gActiveFileTransfer->receivedChunks,
                     gActiveFileTransfer->totalChunks,
                     (unsigned long)gActiveFileTransfer->receivedBytes,
                     (unsigned long)gActiveFileTransfer->totalSize);
      }
    }
    
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }

  // === V3 FILE_END ===
  // Write buffered data to filesystem and process
  if (h->type == ESPNOW_V3_TYPE_FILE_END && isPaired) {
    if (!gActiveFileTransfer || !gActiveFileTransfer->active) {
      ERROR_ESPNOWF("[V3_FILE] Received FILE_END without active transfer");
      return true;
    }
    
    // Validate sender MAC matches the transfer initiator
    if (memcmp(gActiveFileTransfer->senderMac, recv_info->src_addr, 6) != 0) {
      ERROR_ESPNOWF("[V3_FILE] FILE_END from different sender than FILE_START");
      return true;
    }
    
    const V3PayloadFileEnd* fe = (const V3PayloadFileEnd*)payload;
    String senderMacStr = formatMacAddress(gActiveFileTransfer->senderMac);
    
    DEBUG_ESPNOWF("[V3_FILE_RX] FILE_END: %s (%lu bytes, %u/%u chunks, success=%d)",
                 gActiveFileTransfer->filename,
                 (unsigned long)gActiveFileTransfer->receivedBytes,
                 gActiveFileTransfer->receivedChunks,
                 gActiveFileTransfer->totalChunks,
                 fe->success);

    bool isComplete = (gActiveFileTransfer->totalSize == 0) ||
                      ((gActiveFileTransfer->receivedChunks == gActiveFileTransfer->totalChunks) &&
                       (gActiveFileTransfer->receivedBytes == gActiveFileTransfer->totalSize));

    if (!isComplete) {
      BROADCAST_PRINTF("[V3_FILE] REJECTED incomplete transfer '%s': %u/%u chunks, %lu/%lu bytes",
                   gActiveFileTransfer->filename,
                   (unsigned)gActiveFileTransfer->receivedChunks,
                   (unsigned)gActiveFileTransfer->totalChunks,
                   (unsigned long)gActiveFileTransfer->receivedBytes,
                   (unsigned long)gActiveFileTransfer->totalSize);
      // Clean up — sync tick will re-request if this was manifest/settings
      if (gActiveFileTransfer->chunkMap) heap_caps_free(gActiveFileTransfer->chunkMap);
      if (gActiveFileTransfer->dataBuffer) heap_caps_free(gActiveFileTransfer->dataBuffer);
      delete gActiveFileTransfer;
      gActiveFileTransfer = nullptr;
      return true;
    }

    if (fe->success && gActiveFileTransfer->dataBuffer) {
#if ENABLE_BONDED_MODE
      // Check if this is a manifest file - process directly from buffer without filesystem
      if (strcmp(gActiveFileTransfer->filename, "_manifest_out.json") == 0) {
        // Process manifest directly from PSRAM buffer
        String manifestStr((char*)gActiveFileTransfer->dataBuffer, gActiveFileTransfer->receivedBytes);
        processBondModeManifestResp(gActiveFileTransfer->senderMac, senderMacStr, manifestStr);
        BROADCAST_PRINTF("[V3_FILE] Manifest processed: %lu bytes", (unsigned long)gActiveFileTransfer->receivedBytes);
      }
      // Check if this is a settings file - process directly from buffer without filesystem
      else if (strcmp(gActiveFileTransfer->filename, "_settings_out.json") == 0) {
        DEBUG_ESPNOWF("[FILE_END] Detected settings file: %s (%lu bytes)", 
                      gActiveFileTransfer->filename, (unsigned long)gActiveFileTransfer->receivedBytes);
        // Process settings directly from PSRAM buffer
        String settingsStr((char*)gActiveFileTransfer->dataBuffer, gActiveFileTransfer->receivedBytes);
        DEBUG_ESPNOWF("[FILE_END] Calling processBondSettings (settingsStr len=%d)", settingsStr.length());
        processBondSettings(gActiveFileTransfer->senderMac, senderMacStr, settingsStr);
        BROADCAST_PRINTF("[V3_FILE] Settings processed: %lu bytes", (unsigned long)gActiveFileTransfer->receivedBytes);
      } 
      else
#endif // ENABLE_BONDED_MODE
      {
        // For non-manifest files, write to filesystem (single write operation)
        String senderMacHex = macToHexString(gActiveFileTransfer->senderMac);
        senderMacHex.replace(":", "");
        String deviceDir = String("/espnow/received/") + senderMacHex;
        
        FsLockGuard guard("v3file.write");
        LittleFS.mkdir("/espnow");
        LittleFS.mkdir("/espnow/received");
        LittleFS.mkdir(deviceDir.c_str());
        String filepath = deviceDir + "/" + gActiveFileTransfer->filename;
        File f = LittleFS.open(filepath, "w");
        if (f) {
          f.write(gActiveFileTransfer->dataBuffer, gActiveFileTransfer->receivedBytes);
          f.close();
          BROADCAST_PRINTF("[V3_FILE] Complete: %s (%lu bytes)", gActiveFileTransfer->filename, (unsigned long)gActiveFileTransfer->receivedBytes);
          
          // If this is an automations file, broadcast a formatted summary so CLI users
          // can read it directly without needing the web UI (works radio-only)
          if (strcmp(gActiveFileTransfer->filename, "automations.json") == 0) {
            String senderName = getEspNowDeviceName(gActiveFileTransfer->senderMac);
            if (senderName.length() == 0) senderName = formatMacAddress(gActiveFileTransfer->senderMac);
            String jsonStr((char*)gActiveFileTransfer->dataBuffer, gActiveFileTransfer->receivedBytes);
            PSRAM_JSON_DOC(adoc);
            DeserializationError aerr = deserializeJson(adoc, jsonStr);
            if (!aerr && adoc["automations"].is<JsonArray>()) {
              JsonArray arr = adoc["automations"].as<JsonArray>();
              int total = (int)arr.size();
              BROADCAST_PRINTF("[AUTOMATIONS] %s: %d automation%s", senderName.c_str(), total, total == 1 ? "" : "s");
              int idx = 0;
              for (JsonObject a : arr) {
                if (idx >= 10) {
                  BROADCAST_PRINTF("  ... (%d more)", total - idx);
                  break;
                }
                const char* aname = a["name"] | "(unnamed)";
                bool enabled = a["enabled"] | true;
                JsonObject sched = a["schedule"];
                String schedStr = sched ? String(sched["type"] | "?") : String("?");
                if (schedStr == "time") {
                  const char* t = sched["time"] | "";
                  if (t && t[0]) schedStr = String(t);
                }
                int cmdCount = a["commands"].is<JsonArray>()
                               ? (int)a["commands"].as<JsonArray>().size() : 0;
                BROADCAST_PRINTF("  [%s] %s @ %s (%d cmd%s)", enabled ? "ON " : "OFF", aname, schedStr.c_str(), cmdCount, cmdCount == 1 ? "" : "s");
                idx++;
              }
            } else {
              BROADCAST_PRINTF("[AUTOMATIONS] Parse failed from %s%s%s", senderName.c_str(), aerr ? ": " : "", aerr ? aerr.c_str() : "");
            }
          }
        }
      }
      
      logFileTransferEvent(gActiveFileTransfer->senderMac, senderMacStr.c_str(),
                          gActiveFileTransfer->filename, MSG_FILE_RECV_SUCCESS);
      if (gEspNow) gEspNow->fileTransfersReceived++;
    } else {
      logFileTransferEvent(gActiveFileTransfer->senderMac, senderMacStr.c_str(),
                          gActiveFileTransfer->filename, MSG_FILE_RECV_FAILED);
    }
    
    // Send final ACK
    v3_send_ack(recv_info->src_addr, h->msgId);
    
    // Cleanup
    if (gActiveFileTransfer->chunkMap) {
      heap_caps_free(gActiveFileTransfer->chunkMap);
      gActiveFileTransfer->chunkMap = nullptr;
    }
    if (gActiveFileTransfer->dataBuffer) {
      heap_caps_free(gActiveFileTransfer->dataBuffer);
    }
    delete gActiveFileTransfer;
    gActiveFileTransfer = nullptr;
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }

#if ENABLE_BONDED_MODE
  // === MANIFEST REQ (v3) ===
  // NOTE: Defer heavy work to main loop - callback context has limited stack and is ISR-like
  if (h->type == ESPNOW_V3_TYPE_MANIFEST_REQ && isPaired && gSettings.bondModeEnabled) {
    DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_MANIFEST_REQ_RX] from %s isPaired=%d", deviceName, (int)isPaired);
    if (gEspNow) {
      memcpy(gEspNow->bondPendingResponseMac, recv_info->src_addr, 6);
      gEspNow->bondNeedsManifestResponse = true;
      DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_MANIFEST_REQ_RX] set bondNeedsManifestResponse=true");
    }
    if (h->fragCount > 1) {
      for (int i = 0; i < V3_REASM_MAX; i++) {
        if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
            memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v3_reasm_reset(gV3Reasm[i]);
          break;
        }
      }
    }
    return true;
  }

  // === MANIFEST RESP is handled via FILE_END for _manifest_out.json ===
  // (Large manifests are sent as file transfers, not as single frames)
#endif // ENABLE_BONDED_MODE

  // Unknown V3 type - log and ignore
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3] Unknown type %d from %s", h->type, deviceName);
  
  // CRITICAL: Cleanup reassembly buffer for ALL fragmented messages (not just unknown types)
  // This was previously only cleaning up at the end of the function, causing memory leaks
  // for message types that returned early (TEXT, CMD, HEARTBEAT, etc.)
  // Now we cleanup immediately after processing any fragmented message
  if (h->fragCount > 1) {
    for (int i = 0; i < V3_REASM_MAX; i++) {
      if (gV3Reasm[i].active && gV3Reasm[i].msgId == h->msgId && 
          memcmp(gV3Reasm[i].src, recv_info->src_addr, 6) == 0) {
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FRAG_RX] Cleaning up reassembly buffer for msgId=%lu",
               (unsigned long)h->msgId);
        v3_reasm_reset(gV3Reasm[i]);
        break;
      }
    }
  }
  
  return true;
}

// V3 CMD async context - passed to callback for sending response
struct V3CmdAsyncCtx {
  uint8_t srcMac[6];
  char deviceName[32];
  char cmdName[32];   // Command name (first word) for inclusion in CMD_RESP
  uint32_t cmdMsgId;  // For session-based streaming correlation
};

// Async callback type (matches HardwareOne.cpp)
typedef void (*ExecAsyncCallback)(bool ok, const char* result, void* userData);

// Command origin enum (matches HardwareOne.cpp)
enum CommandOrigin { ORIGIN_SERIAL, ORIGIN_WEB, ORIGIN_AUTOMATION, ORIGIN_SYSTEM };

// Command context structure (matches HardwareOne.cpp)
struct CommandContext {
  CommandOrigin origin;
  AuthContext auth;
  uint32_t id;
  uint32_t timestampMs;
  uint32_t outputMask;
  bool validateOnly;
  void* replyHandle;
  httpd_req_t* httpReq;
};

// Command structure (matches HardwareOne.cpp)
struct Command {
  String line;
  CommandContext ctx;
};

// External async command submission
extern bool submitCommandAsync(const Command& cmd, ExecAsyncCallback callback, void* userData);

// Async callback for V3 CMD results - called on cmd_exec task (large stack)
static void v3CmdResultCallback(bool ok, const char* result, void* userData) {
  V3CmdAsyncCtx* ctx = (V3CmdAsyncCtx*)userData;
  if (!ctx) return;
  
  size_t resultLen = result ? strlen(result) : 0;
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3] CMD result callback: ok=%d len=%zu to %s (msgId=%lu)", 
         ok, resultLen, ctx->deviceName, (unsigned long)ctx->cmdMsgId);
  
  // Clear the current stream session ID (stops any further streaming)
  gCurrentStreamCmdId = 0;
  
  // Send STREAM_END frame to signal output is complete
  sendSessionStreamFrame(ctx->cmdMsgId, nullptr, 0, ESPNOW_V3_FLAG_STREAM_END);
  
  // Send CMD_RESP with success/fail byte + actual command result text.
  // v3_send_chunked handles fragmentation for large results automatically.
  // The receiver reassembles fragments before processing.
  const char* resultText = (result && resultLen > 0) ? result : 
                           (ctx->cmdName[0] ? ctx->cmdName : (ok ? "OK" : "FAIL"));
  size_t textLen = strlen(resultText);
  // Cap at 2KB to avoid excessive fragmentation (matches ExecReq.out buffer size)
  if (textLen > 2047) textLen = 2047;
  
  // Allocate payload: 1 byte success + result text + null terminator
  size_t payloadLen = 1 + textLen + 1;
  uint8_t* respPayload = (uint8_t*)malloc(payloadLen);
  if (respPayload) {
    respPayload[0] = ok ? 1 : 0;
    memcpy(respPayload + 1, resultText, textLen);
    respPayload[1 + textLen] = '\0';
    
    v3_send_chunked(ctx->srcMac, ESPNOW_V3_TYPE_CMD_RESP, ESPNOW_V3_FLAG_ACK_REQ,
                    ctx->cmdMsgId, respPayload, payloadLen, 1);
    free(respPayload);
  } else {
    // Fallback: send just command name if malloc fails
    uint8_t fallback[64];
    fallback[0] = ok ? 1 : 0;
    const char* name = ctx->cmdName[0] ? ctx->cmdName : (ok ? "OK" : "FAIL");
    size_t nameLen = strlen(name) + 1;
    if (nameLen > sizeof(fallback) - 1) nameLen = sizeof(fallback) - 1;
    memcpy(fallback + 1, name, nameLen);
    v3_send_chunked(ctx->srcMac, ESPNOW_V3_TYPE_CMD_RESP, ESPNOW_V3_FLAG_ACK_REQ,
                    ctx->cmdMsgId, fallback, 1 + nameLen, 1);
  }
  
  // Clean up session
  destroyStreamSession(ctx->cmdMsgId);
  free(ctx);
}

// V3 command execution handler
// Payload formats:
//   1. "username:password:command" - Traditional auth with credentials
//   2. "@BOND:<32-hex-token>:command" - Bond mode session token auth
// Authenticates quickly, then queues to cmd_exec task for execution
static void v3_handle_cmd(const uint8_t* srcMac, const char* deviceName, uint32_t msgId, const char* cmdPayload) {
  const char* firstColon = strchr(cmdPayload, ':');
  if (!firstColon) {
    BROADCAST_PRINTF("[ESP-NOW] Invalid CMD format from %s (missing delimiter)", deviceName);
    return;
  }
  const char* secondColon = strchr(firstColon + 1, ':');
  if (!secondColon) {
    BROADCAST_PRINTF("[ESP-NOW] Invalid CMD format from %s (missing second delimiter)", deviceName);
    return;
  }
  
  const char* actualCmd = secondColon + 1;
  bool authOk = false;
  
  // Declare username outside the if/else blocks so it's accessible later
  char username[32] = "espnow";  // Default for token auth
  
  if (strncmp(cmdPayload, "@BOND:", 6) == 0) {
    DEBUG_ESPNOWF("[BOND_AUTH] Received bonded command from %s", deviceName);
    size_t tokenLen = secondColon - firstColon - 1;
    DEBUG_ESPNOWF("[BOND_AUTH]   tokenLen=%zu (expect 32)", tokenLen);
    
    if (tokenLen == 32) {
      char tokenStr[33];
      memcpy(tokenStr, firstColon + 1, 32);
      tokenStr[32] = '\0';
      DEBUG_ESPNOWF("[BOND_AUTH]   recvToken=%.8s...", tokenStr);
      
      uint8_t token[16];
      if (parseSessionToken(tokenStr, token)) {
        DEBUG_ESPNOWF("[BOND_AUTH]   parsed: %02X%02X%02X%02X...",
                      token[0], token[1], token[2], token[3]);
        if (validateBondSessionToken(token, 16)) {
          authOk = true;
          BROADCAST_PRINTF("[ESP-NOW] Bonded command from %s (session token): %s", deviceName, actualCmd);
        } else {
          BROADCAST_PRINTF("[ESP-NOW] Invalid session token from %s", deviceName);
          WARN_ESPNOWF("[BOND_AUTH]   MISMATCH - check passphrase on both devices");
        }
      } else {
        BROADCAST_PRINTF("[ESP-NOW] Malformed session token from %s", deviceName);
        WARN_ESPNOWF("[BOND_AUTH]   parse failed for: %s", tokenStr);
      }
    } else {
      BROADCAST_PRINTF("[ESP-NOW] Wrong token length from %s: %zu (expected 32)", deviceName, tokenLen);
    }
    
    if (!authOk) {
      WARN_ESPNOWF("[BOND_AUTH]   AUTH FAILED - sending error response");
      uint8_t respPayload[48];
      respPayload[0] = 0;
      const char* errMsg = "Session token auth failed";
      memcpy(respPayload + 1, errMsg, strlen(errMsg) + 1);
      v3_send_chunked(srcMac, ESPNOW_V3_TYPE_CMD_RESP, ESPNOW_V3_FLAG_ACK_REQ, 
                      generateMessageId(), respPayload, 1 + strlen(errMsg) + 1, 1);
      return;
    }
  } else {
    // Traditional username:password auth
    char password[32];
    size_t userLen = firstColon - cmdPayload;
    size_t passLen = secondColon - firstColon - 1;
    if (userLen >= sizeof(username)) userLen = sizeof(username) - 1;
    if (passLen >= sizeof(password)) passLen = sizeof(password) - 1;
    memcpy(username, cmdPayload, userLen); username[userLen] = '\0';
    memcpy(password, firstColon + 1, passLen); password[passLen] = '\0';
    
    BROADCAST_PRINTF("[ESP-NOW] Remote command from %s (user=%s): %s", deviceName, username, actualCmd);
    
    // Quick auth check (runs in espnow_task - small stack usage)
    if (isValidUser(String(username), String(password))) {
      authOk = true;
    } else {
      BROADCAST_PRINTF("[ESP-NOW] Auth failed for user '%s' from %s", username, deviceName);
      uint8_t respPayload[32];
      respPayload[0] = 0;
      const char* errMsg = "Auth failed";
      memcpy(respPayload + 1, errMsg, strlen(errMsg) + 1);
      v3_send_chunked(srcMac, ESPNOW_V3_TYPE_CMD_RESP, ESPNOW_V3_FLAG_ACK_REQ, 
                      generateMessageId(), respPayload, 1 + strlen(errMsg) + 1, 1);
      return;
    }
  }
  
  if (!authOk) return;  // Safety check
  
  // Create stream session for real-time output streaming
  StreamSession* sess = createStreamSession(srcMac, msgId);
  if (!sess) {
    BROADCAST_PRINTF("[ESP-NOW] CMD handler: no stream session slots");
    uint8_t respPayload[32];
    respPayload[0] = 0;
    const char* errMsg = "No session slots";
    memcpy(respPayload + 1, errMsg, strlen(errMsg) + 1);
    v3_send_chunked(srcMac, ESPNOW_V3_TYPE_CMD_RESP, ESPNOW_V3_FLAG_ACK_REQ,
                    msgId, respPayload, 1 + strlen(errMsg) + 1, 1);
    return;
  }
  
  // Allocate async context (freed in callback)
  V3CmdAsyncCtx* asyncCtx = (V3CmdAsyncCtx*)malloc(sizeof(V3CmdAsyncCtx));
  if (!asyncCtx) {
    BROADCAST_PRINTF("[ESP-NOW] CMD handler: alloc failed");
    destroyStreamSession(msgId);
    return;
  }
  memcpy(asyncCtx->srcMac, srcMac, 6);
  strncpy(asyncCtx->deviceName, deviceName, sizeof(asyncCtx->deviceName) - 1);
  asyncCtx->deviceName[sizeof(asyncCtx->deviceName) - 1] = '\0';
  asyncCtx->cmdMsgId = msgId;
  // Extract command name (first word of actualCmd) for inclusion in CMD_RESP
  const char* cmdSpace = strchr(actualCmd, ' ');
  if (cmdSpace) {
    size_t nameLen = cmdSpace - actualCmd;
    if (nameLen > sizeof(asyncCtx->cmdName) - 1) nameLen = sizeof(asyncCtx->cmdName) - 1;
    memcpy(asyncCtx->cmdName, actualCmd, nameLen);
    asyncCtx->cmdName[nameLen] = '\0';
  } else {
    strncpy(asyncCtx->cmdName, actualCmd, sizeof(asyncCtx->cmdName) - 1);
    asyncCtx->cmdName[sizeof(asyncCtx->cmdName) - 1] = '\0';
  }
  
  // Send STREAM_BEGIN frame to signal output is starting
  sendSessionStreamFrame(msgId, nullptr, 0, ESPNOW_V3_FLAG_STREAM_BEGIN);
  
  // Set the current stream session ID (cmd_exec task will use this)
  gCurrentStreamCmdId = msgId;
  
  // Build command for cmd_exec task
  Command cmd;
  cmd.line = actualCmd;
  cmd.ctx.origin = ORIGIN_SYSTEM;
  cmd.ctx.auth.transport = SOURCE_ESPNOW;
  cmd.ctx.auth.path = actualCmd;
  cmd.ctx.auth.user = username;
  cmd.ctx.auth.sid = "";
  cmd.ctx.auth.opaque = (void*)asyncCtx->srcMac;
  cmd.ctx.id = msgId;
  cmd.ctx.timestampMs = millis();
  cmd.ctx.outputMask = 0;  // Output streams via V3 STREAM frames
  cmd.ctx.validateOnly = false;
  cmd.ctx.replyHandle = nullptr;
  cmd.ctx.httpReq = nullptr;
  
  // Queue for execution on cmd_exec task (has large stack)
  if (!submitCommandAsync(cmd, v3CmdResultCallback, asyncCtx)) {
    BROADCAST_PRINTF("[ESP-NOW] Failed to queue CMD from %s", deviceName);
    gCurrentStreamCmdId = 0;
    destroyStreamSession(msgId);
    free(asyncCtx);
    uint8_t respPayload[32];
    respPayload[0] = 0;
    const char* errMsg = "Queue failed";
    memcpy(respPayload + 1, errMsg, strlen(errMsg) + 1);
    v3_send_chunked(srcMac, ESPNOW_V3_TYPE_CMD_RESP, ESPNOW_V3_FLAG_ACK_REQ,
                    msgId, respPayload, 1 + strlen(errMsg) + 1, 1);
  }
}

static void onEspNowRawRecv(const esp_now_recv_info* recv_info, const uint8_t* incomingData, int len) {
  // Increment receive counter
  if (gEspNow) gEspNow->routerMetrics.messagesReceived++;

  // === V3-ONLY MODE ===
  // Try v3 binary protocol (only handler enabled)
  if (recv_info && incomingData && len > 0 && v3_try_handle_incoming(recv_info, incomingData, len)) {
    return;
  }
  
  // V3 didn't handle it - log and drop
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX] Message not handled by V3 - dropped (len=%d)", len);
  return;
  
}

/**
 * @brief ESP-NOW send status callback (registered with ESP-IDF)
 * @param tx_info Transmission info structure (ESP-IDF v5.x)
 * @param status Send result (ESP_NOW_SEND_SUCCESS or ESP_NOW_SEND_FAIL)
 * @note Called in interrupt context - keep processing minimal
 * @note Updates gEspNow->lastStatus and gEspNow->txDone flags
 * @warning Do not call blocking functions or allocate memory
 */
void onEspNowDataSent(const uint8_t* mac_addr, esp_now_send_status_t status) {
  // Statistics are now tracked in routerSend() for better accuracy
  // This callback is kept for flow control only
 
  (void)mac_addr;
  if (!gEspNow) return;

  gEspNow->lastStatus = status;
  gEspNow->txDone = true;
}

/**
 * @brief Check if ESP-NOW first-time setup is needed
 * @return Error message if setup needed, empty string if ready to proceed
 * @note Displays setup instructions and returns error to block initialization
 */

/**
 * @brief Main message dispatcher - routes messages to appropriate handlers
 * @param ctx Received message context
 * @return true if message was handled
 */
static bool dispatchMessage(const ReceivedMessage& ctx) {
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[DISPATCH] ========================================");
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[DISPATCH] === MESSAGE DISPATCH ENTRY ===");
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[DISPATCH] ========================================");
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[DISPATCH] Message length: %d bytes", ctx.message.length());
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[DISPATCH] First char: '%c' (0x%02X)", 
         ctx.message.length() > 0 ? ctx.message.charAt(0) : '?',
         ctx.message.length() > 0 ? (unsigned char)ctx.message.charAt(0) : 0);
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[DISPATCH] Content (first 80 chars): %.80s", ctx.message.c_str());
  
  // Priority 1: JSON messages (heartbeats, ACKs, mesh envelopes, topology)
  if (ctx.message.startsWith("{")) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[DISPATCH] Routing to JSON handler");
    return handleJsonMessage(ctx);
  }
  
  // Priority 2: Legacy CHUNK removed - v2 fragmentation handles all chunking
  
  // All messages should now be v2 JSON - if we get here, it's an unknown/legacy format
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[DISPATCH] WARNING: Non-JSON message received (legacy format?)");
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[DISPATCH] Content: %.80s", ctx.message.c_str());
  
  // For debugging: show the message but don't process it
  String deviceName = ctx.deviceName.length() > 0 ? ctx.deviceName : ctx.macStr;
  broadcastOutput("[ESP-NOW] Unknown format from " + deviceName + ": " + ctx.message);
  
  return false;
}

/**
 * @brief Check if mesh routing should be used
 * @param mac Destination MAC address
 * @return true if mesh routing should be used
 */
static bool shouldUseMesh(const uint8_t* mac) {
  if (!gEspNow) return false;
  
  // If mesh mode disabled, never use mesh
  if (gEspNow->mode != ESPNOW_MODE_MESH) {
    return false;
  }
  
  // If peer is directly paired, prefer direct
  if (esp_now_is_peer_exist(mac)) {
    return false;
  }
  
  // Peer not directly paired, use mesh routing
  return true;
}

// ==========================
// Message Queue Management
// ==========================

/**
 * @brief Check if message retry queue is full
 * @return true if queue is full
 */
static bool isMessageQueueFull() {
  if (!gEspNow) return true;
  return gEspNow->queueSize >= 8;
}

/**
 * @brief Get current queue size
 * @return Number of messages in queue
 */
static uint8_t getQueueSize() {
  if (!gEspNow) return 0;
  return gEspNow->queueSize;
}

/**
 * @brief Enqueue message for retry
 * @param msg Message to enqueue
 * @return true if enqueued successfully
 */
static bool enqueueMessage(const Message& msg) {
  if (!gEspNow) return false;
  if (isMessageQueueFull()) {
    gEspNow->routerMetrics.queueOverflows++;
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[Queue] Queue full, cannot enqueue message ID %lu",
           (unsigned long)msg.msgId);
    return false;
  }
  
  // Find empty slot
  for (int i = 0; i < 8; i++) {
    if (!gEspNow->retryQueue[i].active) {
      gEspNow->retryQueue[i].msg = msg;
      gEspNow->retryQueue[i].retryCount = 0;
      gEspNow->retryQueue[i].nextRetryTime = millis() + 100;  // First retry in 100ms
      gEspNow->retryQueue[i].active = true;
      gEspNow->queueSize++;
      gEspNow->routerMetrics.messagesQueued++;
      
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[Queue] Enqueued message ID %lu (queue size: %d)", 
             (unsigned long)msg.msgId, gEspNow->queueSize);
      return true;
    }
  }
  
  return false;  // Should never reach here if isQueueFull() works correctly
}

/**
 * @brief Dequeue message from retry queue
 * @param index Queue slot index to dequeue
 */
static void dequeueMessage(int index) {
  if (!gEspNow) return;
  if (index < 0 || index >= 8) return;
  if (!gEspNow->retryQueue[index].active) return;
  
  gEspNow->retryQueue[index].active = false;
  gEspNow->retryQueue[index].msg.payload = "";  // Free String memory
  gEspNow->queueSize--;
  gEspNow->routerMetrics.messagesDequeued++;
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[Queue] Dequeued message from slot %d (queue size: %d)", 
         index, gEspNow->queueSize);
}

/**
 * @brief Process retry queue - attempt to send queued messages
 * @note Should be called periodically from main loop
 */
void processMessageQueue() {
  if (!gEspNow) return;
  if (gEspNow->queueSize == 0) return;
  
  unsigned long now = millis();
  
  for (int i = 0; i < 8; i++) {
    QueuedMessage* qm = &gEspNow->retryQueue[i];
    if (!qm->active) continue;
    
    // Check if it's time to retry
    if (now < qm->nextRetryTime) continue;
    
    // Check if max retries exceeded
    if (qm->retryCount >= qm->msg.maxRetries) {
      // Before dropping, try mesh fallback if in mesh mode and direct send was attempted
      if (gEspNow->mode == ESPNOW_MODE_MESH && esp_now_is_peer_exist(qm->msg.dstMac)) {
        DEBUGF(DEBUG_ESPNOW_ROUTER,
               "[Queue] Direct retries exhausted for ID %lu, attempting mesh fallback",
               (unsigned long)qm->msg.msgId);
        
        // Inject TTL for mesh routing
        String meshPayload = qm->msg.payload;
        PSRAM_JSON_DOC(doc);
        if (!deserializeJson(doc, qm->msg.payload)) {
          if (doc["ttl"].isNull() || doc["ttl"] == 0) {
            doc["ttl"] = gSettings.meshTTL;
            meshPayload = "";
            serializeJson(doc, meshPayload);
          }
        }
        
        // Try mesh routing
        meshSendEnvelopeToPeers(meshPayload);
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[Queue] Mesh fallback attempted for ID %lu",
               (unsigned long)qm->msg.msgId);
        gEspNow->routerMetrics.meshFallbacks++;
      } else {
        DEBUGF(DEBUG_ESPNOW_ROUTER,
               "[Queue] Message ID %lu exceeded max retries (%d), dropping",
               (unsigned long)qm->msg.msgId, qm->msg.maxRetries);
        gEspNow->routerMetrics.messagesDropped++;
      }
      dequeueMessage(i);
      continue;
    }
    
    // Attempt retry
    qm->retryCount++;
    gEspNow->routerMetrics.retriesAttempted++;
    
    DEBUGF(DEBUG_ESPNOW_ROUTER,
           "[Queue] Retrying message ID %lu (attempt %d/%d)",
           (unsigned long)qm->msg.msgId, qm->retryCount, qm->msg.maxRetries);
    
    // Try to send (use internal send, not routerSend to avoid recursion)
    bool success = false;
    bool useMesh = shouldUseMesh(qm->msg.dstMac);
    
    if (useMesh) {
      // Unified v2: qm->msg.payload is already a full logical v2 JSON envelope
      // (built via v2_init_envelope / buildMeshEnvelope / buildTimeSyncMessage).
      // We simply re-broadcast this envelope via meshSendEnvelopeToPeers, without
      // introducing any additional wrapper or base64 encoding.
      meshSendEnvelopeToPeers(qm->msg.payload);
      success = true;
    } else {
      // Look up device info
      bool isEncrypted = false;
      String deviceName = "";
      for (int j = 0; j < gEspNow->deviceCount; j++) {
        if (memcmp(gEspNow->devices[j].mac, qm->msg.dstMac, 6) == 0) {
          isEncrypted = gEspNow->devices[j].encrypted;
          deviceName = gEspNow->devices[j].name;
          break;
        }
      }

      // V2 retry disabled - V3 handles retries at fragment level
      success = false;
    }
    
    if (success) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[Queue] Retry successful for message ID %lu",
             (unsigned long)qm->msg.msgId);
      gEspNow->routerMetrics.retriesSucceeded++;
      dequeueMessage(i);
    } else {
      // Calculate exponential backoff: 100ms, 200ms, 400ms, 800ms
      unsigned long backoff = 100 << qm->retryCount;  // 100 * 2^retryCount
      if (backoff > 800) backoff = 800;  // Cap at 800ms
      qm->nextRetryTime = now + backoff;
      
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[Queue] Retry failed, next attempt in %lu ms", backoff);
    }
  }
}

#if ENABLE_BONDED_MODE
// ==========================
// Bond Mode Helper Functions
// ==========================

/**
 * Build BondPeerStatus snapshot from local device state
 * Called in task context when responding to BOND_STATUS_REQ
 */
static void buildLocalBondStatus(BondPeerStatus& status) {
  memset(&status, 0, sizeof(BondPeerStatus));
  
  status.uptimeSec = millis() / 1000;
  status.freeHeap = (uint32_t)ESP.getFreeHeap();
  status.minFreeHeap = (uint32_t)ESP.getMinFreeHeap();
  
  // Build sensor enabled mask from runtime booleans
  extern bool thermalEnabled, tofEnabled, imuEnabled, gamepadEnabled;
  extern bool gpsEnabled, presenceEnabled;
  uint16_t enabled = 0;
#if ENABLE_THERMAL_SENSOR
  if (thermalEnabled)  enabled |= CAP_SENSOR_THERMAL;
#endif
#if ENABLE_TOF_SENSOR
  if (tofEnabled)      enabled |= CAP_SENSOR_TOF;
#endif
#if ENABLE_IMU_SENSOR
  if (imuEnabled)      enabled |= CAP_SENSOR_IMU;
#endif
#if ENABLE_GAMEPAD_SENSOR
  if (gamepadEnabled)  enabled |= CAP_SENSOR_GAMEPAD;
#endif
#if ENABLE_GPS_SENSOR
  if (gpsEnabled)      enabled |= CAP_SENSOR_GPS;
#endif
#if ENABLE_PRESENCE_SENSOR
  if (presenceEnabled) enabled |= CAP_SENSOR_PRESENCE;
#endif
  status.sensorEnabledMask = enabled;
  
  // Build sensor connected mask
  extern bool thermalConnected, tofConnected, imuConnected, gamepadConnected;
  extern bool gpsConnected, presenceConnected;
  uint16_t connected = 0;
#if ENABLE_THERMAL_SENSOR
  if (thermalConnected)  connected |= CAP_SENSOR_THERMAL;
#endif
#if ENABLE_TOF_SENSOR
  if (tofConnected)      connected |= CAP_SENSOR_TOF;
#endif
#if ENABLE_IMU_SENSOR
  if (imuConnected)      connected |= CAP_SENSOR_IMU;
#endif
#if ENABLE_GAMEPAD_SENSOR
  if (gamepadConnected)  connected |= CAP_SENSOR_GAMEPAD;
#endif
#if ENABLE_GPS_SENSOR
  if (gpsConnected)      connected |= CAP_SENSOR_GPS;
#endif
#if ENABLE_PRESENCE_SENSOR
  if (presenceConnected) connected |= CAP_SENSOR_PRESENCE;
#endif
  status.sensorConnectedMask = connected;
  
#if ENABLE_WIFI
  status.wifiConnected = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
#endif
#if ENABLE_BLUETOOTH
  status.bluetoothActive = gSettings.bluetoothAutoStart ? 1 : 0;
#endif
#if ENABLE_HTTP_SERVER
  status.httpActive = gSettings.httpAutoStart ? 1 : 0;
#endif
  // Report sync progress as a simple 0-3 value for the wire format
  uint8_t syncProgress = 0;
  if (gEspNow) {
    if (gEspNow->lastRemoteCapValid) syncProgress = 1;
    if (gEspNow->bondManifestReceived) syncProgress = 2;
    if (gEspNow->bondSettingsReceived) syncProgress = 3;
  }
  status.bondHandshakeState = syncProgress;
}

/**
 * Build CapabilitySummary for this device
 */
static void buildCapabilitySummary(CapabilitySummary& cap) {
  memset(&cap, 0, sizeof(CapabilitySummary));
  
  cap.protoVersion = 1;
  
  // Get firmware hash (use first 16 bytes of build timestamp as placeholder)
  // Use heap-allocated context to avoid stack overflow in task context
  const char* buildId = __DATE__ " " __TIME__;
  mbedtls_sha256_context* sha_ctx = (mbedtls_sha256_context*)malloc(sizeof(mbedtls_sha256_context));
  if (sha_ctx) {
    mbedtls_sha256_init(sha_ctx);
    mbedtls_sha256_starts(sha_ctx, 0);
    mbedtls_sha256_update(sha_ctx, (const unsigned char*)buildId, strlen(buildId));
    uint8_t hash[32];
    mbedtls_sha256_finish(sha_ctx, hash);
    mbedtls_sha256_free(sha_ctx);
    free(sha_ctx);
    memcpy(cap.fwHash, hash, 16);
  } else {
    // Fallback: use simple hash if allocation fails
    for (int i = 0; i < 16 && buildId[i]; i++) {
      cap.fwHash[i] = (uint8_t)buildId[i];
    }
  }
  
  cap.role = gSettings.bondRole;
  
  // Build feature mask (using CAP_FEATURE_* constants from header)
  cap.featureMask = 0;
#if ENABLE_WIFI
  cap.featureMask |= CAP_FEATURE_WIFI;
#endif
#if ENABLE_BLUETOOTH
  cap.featureMask |= CAP_FEATURE_BLUETOOTH;
#endif
#if ENABLE_MQTT
  cap.featureMask |= CAP_FEATURE_MQTT;
#endif
#if ENABLE_CAMERA_SENSOR
  cap.featureMask |= CAP_FEATURE_CAMERA;
#endif
#if ENABLE_MICROPHONE_SENSOR
  cap.featureMask |= CAP_FEATURE_MICROPHONE;
#endif
#if ENABLE_ESP_SR
  cap.featureMask |= CAP_FEATURE_ESP_SR;
#endif
#if ENABLE_AUTOMATION
  cap.featureMask |= CAP_FEATURE_AUTOMATION;
#endif
#if ENABLE_MAPS
  cap.featureMask |= CAP_FEATURE_MAPS;
#endif
#if ENABLE_OLED_DISPLAY
  cap.featureMask |= CAP_FEATURE_OLED;
#endif
#if ENABLE_ESPNOW
  cap.featureMask |= CAP_FEATURE_ESPNOW;
#endif
  
  // Build service mask (runtime, using CAP_SERVICE_* constants)
  cap.serviceMask = 0;
  if (gSettings.espnowenabled) cap.serviceMask |= CAP_SERVICE_ESPNOW;
#if ENABLE_WIFI
  if (WiFi.status() == WL_CONNECTED) cap.serviceMask |= CAP_SERVICE_WIFI_CONN;
#endif
#if ENABLE_HTTP_SERVER
  if (gSettings.httpAutoStart) cap.serviceMask |= CAP_SERVICE_HTTP;
#endif
#if ENABLE_BLUETOOTH
  if (gSettings.bluetoothAutoStart) cap.serviceMask |= CAP_SERVICE_BLUETOOTH;
#endif
  
  // Build sensor mask (using CAP_SENSOR_* constants)
  cap.sensorMask = 0;
#if ENABLE_THERMAL_SENSOR
  cap.sensorMask |= CAP_SENSOR_THERMAL;
#endif
#if ENABLE_TOF_SENSOR
  cap.sensorMask |= CAP_SENSOR_TOF;
#endif
#if ENABLE_IMU_SENSOR
  cap.sensorMask |= CAP_SENSOR_IMU;
#endif
#if ENABLE_GAMEPAD_SENSOR
  cap.sensorMask |= CAP_SENSOR_GAMEPAD;
#endif
#if ENABLE_APDS_SENSOR
  cap.sensorMask |= CAP_SENSOR_APDS;
#endif
#if ENABLE_GPS_SENSOR
  cap.sensorMask |= CAP_SENSOR_GPS;
#endif
#if ENABLE_RTC_SENSOR
  cap.sensorMask |= CAP_SENSOR_RTC;
#endif
#if ENABLE_PRESENCE_SENSOR
  cap.sensorMask |= CAP_SENSOR_PRESENCE;
#endif
  
  // Hardware info
  esp_wifi_get_mac(WIFI_IF_STA, cap.mac);
  
  esp_chip_info_t chip_info;
  esp_chip_info(&chip_info);
  cap.chipModel = chip_info.model;
  
  uint32_t flashSize = 0;
  esp_flash_get_size(NULL, &flashSize);
  cap.flashSizeMB = flashSize / (1024 * 1024);
  // ESP.getPsramSize() returns bytes, convert to MB
  uint32_t psramBytes = ESP.getPsramSize();
  cap.psramSizeMB = (psramBytes + 512 * 1024) / (1024 * 1024);  // Round to nearest MB
  
  if (gEspNow && gEspNow->initialized) {
    cap.wifiChannel = gEspNow->channel;
  } else {
    cap.wifiChannel = WiFi.channel();
  }
  
  strncpy(cap.deviceName, gSettings.espnowDeviceName.c_str(), 19);
  cap.deviceName[19] = '\0';
  
  cap.uptimeSeconds = millis() / 1000;
}

/**
 * Generate full device manifest (UI apps + CLI command dump)
 */
static String generateDeviceManifest() {
  PSRAM_JSON_DOC(doc);
  
  // Device info section
  JsonObject device = doc["device"].to<JsonObject>();
  device["name"] = gSettings.espnowDeviceName;
  
  uint8_t mac[6];
  esp_wifi_get_mac(WIFI_IF_STA, mac);
  device["mac"] = formatMacAddress(mac);
  device["role"] = gSettings.bondRole == 1 ? "master" : "worker";
  device["uptime"] = millis() / 1000;
  
  // Capability summary (embedded)
  CapabilitySummary cap;
  buildCapabilitySummary(cap);
  JsonObject capObj = device["capabilities"].to<JsonObject>();
  capObj["featureMask"] = cap.featureMask;
  capObj["serviceMask"] = cap.serviceMask;
  capObj["sensorMask"] = cap.sensorMask;
  capObj["flashMB"] = cap.flashSizeMB;
  capObj["psramMB"] = cap.psramSizeMB;
  capObj["wifiChannel"] = cap.wifiChannel;
  
  // Add sensor connectivity status (which sensors are actually connected vs compiled)
  JsonObject sensorStatus = device["sensorStatus"].to<JsonObject>();
#if ENABLE_I2C_SYSTEM
  // connectedDevices[] and connectedDeviceCount are extern'd at file scope
  // Check which sensors are actually connected
  bool thermalConnected = false, tofConnected = false, imuConnected = false;
  bool gamepadConnected = false, apdsConnected = false, gpsConnected = false;
  bool rtcConnected = false, presenceConnected = false;
  
  for (int i = 0; i < connectedDeviceCount; i++) {
    if (!connectedDevices[i].isConnected) continue;
    switch (connectedDevices[i].address) {
      case 0x33: thermalConnected = true; break;  // MLX90640
      case 0x29: tofConnected = true; break;      // VL53L0X
      case 0x28: imuConnected = true; break;      // BNO055
      case 0x50: gamepadConnected = true; break;  // Seesaw
      case 0x39: apdsConnected = true; break;     // APDS9960
      case 0x10: gpsConnected = true; break;      // PA1010D
      case 0x68: rtcConnected = true; break;      // DS3231
      case 0x61: presenceConnected = true; break; // LD2410
    }
  }
  
  sensorStatus["thermal"] = thermalConnected;
  sensorStatus["tof"] = tofConnected;
  sensorStatus["imu"] = imuConnected;
  sensorStatus["gamepad"] = gamepadConnected;
  sensorStatus["apds"] = apdsConnected;
  sensorStatus["gps"] = gpsConnected;
  sensorStatus["rtc"] = rtcConnected;
  sensorStatus["presence"] = presenceConnected;
#endif
  
  // Add fwHash as hex string for cache keying
  char hashHex[33];
  for (int i = 0; i < 16; i++) {
    snprintf(hashHex + (i * 2), 3, "%02x", cap.fwHash[i]);
  }
  hashHex[32] = '\0';
  capObj["fwHash"] = hashHex;
  
  // UI apps section (curated list of OLED modes)
  JsonArray apps = doc["uiApps"].to<JsonArray>();
  
#if ENABLE_OLED_DISPLAY
  // Add OLED menu items as UI apps
  extern const OLEDMenuItem oledMenuItems[];
  extern const int oledMenuItemCount;
  
  for (int i = 0; i < oledMenuItemCount; i++) {
    JsonObject app = apps.add<JsonObject>();
    app["name"] = oledMenuItems[i].name;
    app["icon"] = oledMenuItems[i].iconName;
    app["mode"] = (int)oledMenuItems[i].targetMode;
  }
#endif
  
  // CLI commands section (full command dump)
  JsonArray cliModules = doc["cliModules"].to<JsonArray>();
  
  size_t moduleCount = 0;
  const CommandModule* modules = getCommandModules(moduleCount);
  
  for (size_t m = 0; m < moduleCount; m++) {
    JsonObject module = cliModules.add<JsonObject>();
    module["name"] = modules[m].name;
    module["description"] = modules[m].description ? modules[m].description : "";
    
    JsonArray commands = module["commands"].to<JsonArray>();
    for (size_t c = 0; c < modules[m].count; c++) {
      JsonObject cmd = commands.add<JsonObject>();
      cmd["name"] = modules[m].commands[c].name;
      cmd["help"] = modules[m].commands[c].help;
      cmd["admin"] = modules[m].commands[c].requiresAdmin;
    }
  }
  
  String manifest;
  serializeJson(doc, manifest);
  return manifest;
}

/**
 * Cache manifest to LittleFS keyed by firmware hash
 */
static bool cacheManifestToLittleFS(const uint8_t fwHash[16], const String& manifest) {
  extern bool filesystemReady;
  if (!filesystemReady) return false;
  
  // Build filename from hash
  char hashHex[33];
  for (int i = 0; i < 16; i++) {
    snprintf(hashHex + (i * 2), 3, "%02x", fwHash[i]);
  }
  hashHex[32] = '\0';
  
  String path = String("/system/manifests/") + hashHex + ".json";
  
  // Ensure directory exists
  if (!LittleFS.exists("/system/manifests")) {
    LittleFS.mkdir("/system/manifests");
  }
  
  // Write manifest
  FsLockGuard fsGuard("pair.manifest.cache");
  File f = LittleFS.open(path.c_str(), "w");
  if (!f) {
    broadcastOutput("[BOND] ERROR: Failed to open manifest cache file");
    return false;
  }
  
  size_t written = f.print(manifest);
  f.close();
  
  if (written != manifest.length()) {
    broadcastOutput("[BOND] ERROR: Incomplete manifest write");
    return false;
  }
  
  BROADCAST_PRINTF("[BOND] Cached manifest to %s (%u bytes)", path.c_str(), (unsigned)written);
  return true;
}

// ============================================================================
// Settings Sync for Bond Mode
// ============================================================================

/**
 * Compute a lightweight FNV-1a hash of device settings (excluding passwords).
 * Stored in gEspNow->bondLocalSettingsHash and sent in bond heartbeats so the
 * peer can detect when our settings have changed without a full exchange.
 */
void computeBondLocalSettingsHash() {
  if (!gEspNow) return;
  // FNV-1a 32-bit
  uint32_t hash = 2166136261u;
  // Hash a few key settings fields that the peer cares about
  auto fnv = [&hash](const char* s) {
    while (*s) { hash ^= (uint8_t)*s++; hash *= 16777619u; }
  };
  fnv(gSettings.espnowDeviceName.c_str());
  fnv(gSettings.espnowFriendlyName.c_str());
  fnv(gSettings.espnowRoom.c_str());
  fnv(gSettings.espnowZone.c_str());
  fnv(gSettings.espnowTags.c_str());
  // Include bond role and some numeric settings
  uint8_t role = gSettings.bondRole;
  hash ^= role; hash *= 16777619u;
  uint8_t stationary = gSettings.espnowStationary ? 1 : 0;
  hash ^= stationary; hash *= 16777619u;
  gEspNow->bondLocalSettingsHash = hash;
}

/**
 * Generate current device settings as JSON
 * Reuses the existing buildSettingsJsonDoc() to get ALL settings
 */
static String generateDeviceSettings() {
  PSRAM_JSON_DOC(doc);
  
  // Use the existing settings serialization (excludes passwords for security)
  extern void buildSettingsJsonDoc(JsonDocument& doc, bool excludePasswords);
  buildSettingsJsonDoc(doc, true);  // true = exclude passwords
  
  // Add device identification metadata
  doc["_deviceName"] = gSettings.espnowDeviceName;
  doc["_bondRole"] = gSettings.bondRole == 1 ? "master" : "worker";
  
  String settings;
  serializeJson(doc, settings);
  return settings;
}

/**
 * Cache settings to LittleFS for a specific peer MAC
 */
static bool cacheSettingsToLittleFS(const uint8_t* peerMac, const String& settings) {
  extern bool filesystemReady;
  DEBUG_ESPNOWF("[SETTINGS_CACHE] cacheSettingsToLittleFS: fsReady=%d len=%d",
                filesystemReady ? 1 : 0, settings.length());
  if (!filesystemReady || !peerMac) {
    DEBUG_ESPNOWF("[SETTINGS_CACHE] EXIT: filesystemReady or peerMac invalid");
    return false;
  }
  
  // Build path from MAC address
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
           peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5]);
  
  String dirPath = String("/system/espnow/peers/") + macStr;
  String filePath = dirPath + "/settings.json";
  DEBUG_ESPNOWF("[SETTINGS_CACHE] Target path: %s", filePath.c_str());
  
  // Ensure directory exists
  FsLockGuard fsGuard("pair.settings.cache");
  if (!LittleFS.exists("/system")) { LittleFS.mkdir("/system"); }
  if (!LittleFS.exists("/system/espnow")) { LittleFS.mkdir("/system/espnow"); }
  if (!LittleFS.exists("/system/espnow/peers")) { LittleFS.mkdir("/system/espnow/peers"); }
  if (!LittleFS.exists(dirPath.c_str())) { LittleFS.mkdir(dirPath.c_str()); }
  
  // Write settings
  File f = LittleFS.open(filePath.c_str(), "w");
  if (!f) {
    ERROR_ESPNOWF("[SETTINGS_CACHE] Failed to open %s for writing", filePath.c_str());
    return false;
  }
  
  size_t written = f.print(settings);
  f.close();
  
  if (written != settings.length()) {
    ERROR_ESPNOWF("[SETTINGS_CACHE] Incomplete write (wrote %d of %d)", written, settings.length());
    return false;
  }
  
  DEBUG_ESPNOWF("[SETTINGS_CACHE] SUCCESS: Cached %d bytes", written);
  return true;
}

/**
 * Load cached settings from LittleFS for a specific peer MAC
 * Non-static so OLED_RemoteSettings can use it
 */
String loadSettingsFromCache(const uint8_t* peerMac) {
  extern bool filesystemReady;
  DEBUG_ESPNOWF("[SETTINGS_LOAD] loadSettingsFromCache called: fsReady=%d peerMac=%p",
                filesystemReady ? 1 : 0, peerMac);
  if (!filesystemReady || !peerMac) {
    DEBUG_ESPNOWF("[SETTINGS_LOAD] EXIT: filesystemReady or peerMac invalid");
    return "";
  }
  
  // Build path from MAC address
  char macStr[18];
  snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X",
           peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5]);
  
  String filePath = String("/system/espnow/peers/") + macStr + "/settings.json";
  DEBUG_ESPNOWF("[SETTINGS_LOAD] Checking path: %s", filePath.c_str());
  
  if (!LittleFS.exists(filePath.c_str())) {
    DEBUG_ESPNOWF("[SETTINGS_LOAD] File does not exist");
    return "";  // Not cached
  }
  
  FsLockGuard fsGuard("pair.settings.load");
  File f = LittleFS.open(filePath.c_str(), "r");
  if (!f) {
    ERROR_ESPNOWF("[SETTINGS_LOAD] Failed to open file");
    return "";
  }
  
  String settings = f.readString();
  f.close();
  
  DEBUG_ESPNOWF("[SETTINGS_LOAD] SUCCESS: Loaded %d bytes from cache", settings.length());
  return settings;
}

// Debouncing state for settings transfer
static uint32_t sLastSettingsSendMs = 0;
static volatile bool sSettingsTransferInProgress = false;
static const uint32_t SETTINGS_DEBOUNCE_MS = 3000;  // 3 second cooldown between requests
static const uint32_t SETTINGS_MIN_HEAP = 20000;    // Minimum 20KB heap required

// Debouncing state for metadata transfer
static uint32_t sLastMetadataRequestMs = 0;
static uint32_t sLastMetadataSendMs = 0;
static const uint32_t METADATA_DEBOUNCE_MS = 3000;  // 3 second cooldown between requests

/**
 * Request settings from bonded peer (master only).
 * Called from the sync tick — timing/retry is owned by the sync tick.
 */
static void requestBondSettings(const uint8_t* peerMac) {
  if (!peerMac || !gEspNow || !gSettings.bondModeEnabled) return;
  if (gSettings.bondRole != 1) return;

  uint32_t now = millis();
  gEspNow->bondSyncInFlight = BOND_SYNC_SETTINGS;
  gEspNow->bondSyncLastAttemptMs = now;
  // NOTE: retry count is managed by the sync tick retry block — don't increment here
  
  uint32_t msgId = generateMessageId();
  bool sent = v3_send_frame(peerMac, ESPNOW_V3_TYPE_SETTINGS_REQ, 0, msgId, nullptr, 0, 1);
  
  if (sent) {
    DEBUG_ESPNOWF("[SETTINGS_REQ] Sent (msgId=%lu retry=%d)", (unsigned long)msgId, (int)gEspNow->bondSyncRetryCount);
  } else {
    ERROR_ESPNOWF("[SETTINGS_REQ] Failed to send");
  }
}

/**
 * Send settings to bonded peer (response to SETTINGS_REQ during initial sync).
 * Settings are cached by the peer for display only — never applied/mounted.
 * Live settings changes use remote commands, not file push.
 */
static void sendBondSettings(const uint8_t* peerMac) {
  DEBUG_ESPNOWF("[SETTINGS_SEND] sendBondSettings called");
  if (!peerMac || !gEspNow) {
    ERROR_ESPNOWF("[SETTINGS_SEND] EXIT: peerMac or gEspNow is NULL");
    return;
  }
  
  if (!gEspNow->bondPeerOnline) {
    DEBUG_ESPNOWF("[SETTINGS_SEND] SKIP: peer offline");
    return;
  }
  
  // Guard: prevent overlapping transfers
  if (sSettingsTransferInProgress) {
    DEBUG_ESPNOWF("[SETTINGS_SEND] SKIP: transfer already in progress");
    return;
  }
  
  // Debounce: skip if we sent recently
  uint32_t now = millis();
  if (now - sLastSettingsSendMs < SETTINGS_DEBOUNCE_MS) {
    DEBUG_ESPNOWF("[SETTINGS_SEND] SKIP: debounced (last=%lums ago)", 
                  (unsigned long)(now - sLastSettingsSendMs));
    return;
  }
  
  // Heap check: ensure we have enough memory for JSON generation
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < SETTINGS_MIN_HEAP) {
    WARN_ESPNOWF("[SETTINGS_SEND] SKIP: low heap (%u < %lu required)", 
                  freeHeap, (unsigned long)SETTINGS_MIN_HEAP);
    return;
  }
  
  sSettingsTransferInProgress = true;
  sLastSettingsSendMs = now;
  
  // Yield to other tasks before heavy operation
  vTaskDelay(pdMS_TO_TICKS(10));
  
  // Generate settings JSON and write to temp file, then immediately release the
  // String from internal heap before the file transfer begins.
  String tempPath = "/system/_settings_out.json";
  size_t settingsLen = 0;
  {
    String settings = generateDeviceSettings();
    settingsLen = settings.length();
    DEBUG_ESPNOWF("[SETTINGS_SEND] Generated settings JSON: %d bytes", settingsLen);

    FsLockGuard guard("pair.settings.send");
    if (!LittleFS.exists("/system")) {
      LittleFS.mkdir("/system");
      DEBUG_ESPNOWF("[SETTINGS_SEND] Created /system directory");
    }
    File f = LittleFS.open(tempPath.c_str(), "w");
    if (!f) {
      ERROR_ESPNOWF("[SETTINGS_SEND] Cannot create %s", tempPath.c_str());
      sSettingsTransferInProgress = false;
      return;
    }
    f.print(settings);
    f.close();
    DEBUG_ESPNOWF("[SETTINGS_SEND] Wrote %d bytes to %s", settingsLen, tempPath.c_str());
    // settings String destructor runs here — internal heap freed before transfer
  }

  // Send via file transfer
  DEBUG_ESPNOWF("[SETTINGS_SEND] Calling sendFileToMac for %s", tempPath.c_str());
  bool sent = sendFileToMac(peerMac, tempPath);

  if (sent) {
    DEBUG_ESPNOWF("[SETTINGS_SEND] SUCCESS: File transfer initiated (%d bytes)",
                  settingsLen);
    // Session token computed in processBondSettings() when sync is confirmed
  } else {
    ERROR_ESPNOWF("[SETTINGS_SEND] sendFileToMac failed");
  }
  
  // Cleanup temp file
  {
    FsLockGuard guard("pair.settings.cleanup");
    LittleFS.remove(tempPath);
    DEBUG_ESPNOWF("[SETTINGS_SEND] Cleaned up temp file");
  }
  
  sSettingsTransferInProgress = false;

#if ENABLE_AUTOMATION
  // Also send our automation list so the peer can view what automations
  // this device has configured.
  // Receiver side is already handled: FILE_END saves automations.json to
  // /espnow/received/<mac>/automations.json and broadcasts a formatted summary.
  {
    extern const char* AUTOMATIONS_JSON_FILE;
    extern bool filesystemReady;
    if (filesystemReady && LittleFS.exists(AUTOMATIONS_JSON_FILE)) {
      vTaskDelay(pdMS_TO_TICKS(200));  // Brief gap between transfers
      bool autoSent = sendFileToMac(peerMac, String(AUTOMATIONS_JSON_FILE));
      if (autoSent) {
        BROADCAST_PRINTF("[BOND] Automation list sent to peer");
      } else {
        DEBUG_ESPNOWF("[BOND] Automation list send failed (non-critical)");
      }
    } else {
      DEBUG_ESPNOWF("[BOND] No automation list to send (file absent or fs not ready)");
    }
  }
#endif
}

/**
 * Process received settings from bonded peer
 */
static void processBondSettings(const uint8_t* srcMac, const String& deviceName, const String& settingsStr) {
  DEBUG_ESPNOWF("[SETTINGS_PROC] processBondSettings: srcMac=%p len=%d",
                srcMac, settingsStr.length());
  if (!srcMac) {
    DEBUG_ESPNOWF("[SETTINGS_PROC] EXIT: srcMac is NULL");
    return;
  }
  
  // Validate JSON integrity — reject truncated/corrupted file transfers
  if (settingsStr.length() < 2 || settingsStr[0] != '{' || settingsStr[settingsStr.length() - 1] != '}') {
    BROADCAST_PRINTF("[BOND_SYNC] REJECTED corrupt settings (len=%d, not valid JSON object)", settingsStr.length());
    if (gEspNow) {
      gEspNow->bondSyncInFlight = BOND_SYNC_NONE;
      gEspNow->bondSyncRetryCount = 0;
    }
    return;  // Sync tick will re-request
  }
  
  // Cache the settings
  DEBUG_ESPNOWF("[SETTINGS_PROC] Calling cacheSettingsToLittleFS...");
  if (cacheSettingsToLittleFS(srcMac, settingsStr)) {
    DEBUG_ESPNOWF("[SETTINGS_PROC] SUCCESS: Settings cached");
    broadcastOutput("[SETTINGS] Cached settings from " + deviceName);
  } else {
    ERROR_ESPNOWF("[SETTINGS_PROC] Failed to cache settings");
    broadcastOutput("[SETTINGS] WARNING: Failed to cache settings from " + deviceName);
  }
  
  // Mark settings received and check if fully synced
  if (gEspNow) {
    if (!gEspNow->bondPeerOnline) {
      BROADCAST_PRINTF("[BOND_SYNC] REJECTED stale settings (peer offline)");
      return;
    }
    
    gEspNow->bondSettingsReceived = true;
    gEspNow->bondSyncInFlight = BOND_SYNC_NONE;
    gEspNow->bondSyncRetryCount = 0;
    gEspNow->bondSyncLastAttemptMs = 0;
    
    bool synced = isBondSynced();
    BROADCAST_PRINTF("[BOND_SYNC] Settings received, synced=%d role=%d", (int)synced, (int)gSettings.bondRole);
    
    if (synced) {
      // Compute session token once on first sync
      if (!gEspNow->bondSessionTokenValid) {
        uint8_t pMac[6];
        if (parseMacAddress(gSettings.bondPeerMac, pMac)) {
          computeBondSessionToken(pMac);
          uint32_t statusReqId = generateMessageId();
          v3_send_frame(pMac, ESPNOW_V3_TYPE_BOND_STATUS_REQ, 0, statusReqId, nullptr, 0, 1);
          gEspNow->bondLastStatusReqMs = millis();
        }
      }
      // Master: push saved streaming prefs to worker now that sync is done
      if (gSettings.bondRole == 1) {
        gEspNow->bondNeedsStreamingSetup = true;
      }
      BROADCAST_PRINTF("[BOND_SYNC] *** SYNC COMPLETE *** role=%d", (int)gSettings.bondRole);
    }
  }
  
  // Invalidate dynamic menu to trigger rebuild with new settings data
#if ENABLE_OLED_DISPLAY
  extern void invalidateDynamicMenu();
  invalidateDynamicMenu();
  DEBUG_ESPNOWF("[SETTINGS_PROC] Invalidated dynamic menu");
#endif
}

/**
 * Load cached manifest from LittleFS by firmware hash
 */
static String loadManifestFromCache(const uint8_t fwHash[16]) {
  extern bool filesystemReady;
  if (!filesystemReady) return "";
  
  // Build filename from hash
  char hashHex[33];
  for (int i = 0; i < 16; i++) {
    snprintf(hashHex + (i * 2), 3, "%02x", fwHash[i]);
  }
  hashHex[32] = '\0';
  
  String path = String("/system/manifests/") + hashHex + ".json";
  
  if (!LittleFS.exists(path.c_str())) {
    return "";  // Not cached
  }
  
  FsLockGuard fsGuard("pair.manifest.load");
  File f = LittleFS.open(path.c_str(), "r");
  if (!f) {
    return "";
  }
  
  String manifest = f.readString();
  f.close();
  
  return manifest;
}

#endif // ENABLE_BONDED_MODE

// ==========================
// Metadata Exchange Functions
// ==========================

/**
 * Build metadata payload from current settings
 */
static void buildMetadataPayload(V3PayloadMetadata* payload) {
  if (!payload) return;
  
  memset(payload, 0, sizeof(V3PayloadMetadata));
  
  strncpy(payload->deviceName, gSettings.espnowDeviceName.c_str(), sizeof(payload->deviceName) - 1);
  strncpy(payload->friendlyName, gSettings.espnowFriendlyName.c_str(), sizeof(payload->friendlyName) - 1);
  strncpy(payload->room, gSettings.espnowRoom.c_str(), sizeof(payload->room) - 1);
  strncpy(payload->zone, gSettings.espnowZone.c_str(), sizeof(payload->zone) - 1);
  strncpy(payload->tags, gSettings.espnowTags.c_str(), sizeof(payload->tags) - 1);
  payload->stationary = gSettings.espnowStationary ? 1 : 0;
}

/**
 * Request metadata from peer - called after heartbeat confirmation or on-demand
 * @param force If true, bypass debounce (for explicit user requests)
 */
void requestMetadata(const uint8_t* peerMac, bool force) {
  if (!peerMac || !gEspNow) {
    WARN_ESPNOWF("[METADATA] requestMetadata: null peerMac=%p gEspNow=%p", peerMac, gEspNow);
    return;
  }
  
  uint32_t now = millis();
  if (!force && (now - sLastMetadataRequestMs < METADATA_DEBOUNCE_MS)) {
    DEBUG_ESPNOW_METADATAF("[METADATA] REQ debounced (%lums < %lums) for %s",
      (unsigned long)(now - sLastMetadataRequestMs), (unsigned long)METADATA_DEBOUNCE_MS,
      MAC_STR(peerMac));
    return;
  }
  sLastMetadataRequestMs = now;
  
  uint32_t msgId = generateMessageId();
  DEBUG_ESPNOW_METADATAF("[METADATA] Sending REQ to %s msgId=%lu force=%d",
    MAC_STR(peerMac), (unsigned long)msgId, (int)force);
  bool sent = v3_send_frame(peerMac, ESPNOW_V3_TYPE_METADATA_REQ, 0, msgId, nullptr, 0, 1);
  
  if (sent) {
    DEBUG_ESPNOW_METADATAF("[METADATA] REQ sent OK to %s msgId=%lu",
      MAC_STR(peerMac), (unsigned long)msgId);
  } else {
    WARN_ESPNOWF("[METADATA] REQ FAILED to send to %s (peer not in hw table?)",
      MAC_STR(peerMac));
  }
}

/**
 * Send metadata to peer (response or push)
 */
static void sendMetadata(const uint8_t* peerMac, bool isPush, bool force = false) {
  if (!peerMac || !gEspNow) {
    WARN_ESPNOWF("[METADATA] sendMetadata: null peerMac=%p gEspNow=%p", peerMac, gEspNow);
    return;
  }
  
  uint32_t now = millis();
  if (!force && !isPush && (now - sLastMetadataSendMs < METADATA_DEBOUNCE_MS)) {
    DEBUG_ESPNOW_METADATAF("[METADATA] RESP debounced for %s", MAC_STR(peerMac));
    return;
  }
  sLastMetadataSendMs = now;
  
  V3PayloadMetadata payload;
  buildMetadataPayload(&payload);
  
  uint32_t msgId = generateMessageId();
  uint8_t type = isPush ? ESPNOW_V3_TYPE_METADATA_PUSH : ESPNOW_V3_TYPE_METADATA_RESP;
  DEBUG_ESPNOW_METADATAF("[METADATA] Sending %s to %s msgId=%lu payloadLen=%u name='%s' room='%s' zone='%s' tags='%s' force=%d",
    isPush ? "PUSH" : "RESP", MAC_STR(peerMac), (unsigned long)msgId,
    (unsigned)sizeof(payload), payload.deviceName, payload.room, payload.zone, payload.tags, (int)force);
  
  bool sent = v3_send_frame(peerMac, type, 0, msgId, (const uint8_t*)&payload, sizeof(payload), 1);
  
  if (sent) {
    DEBUG_ESPNOW_METADATAF("[METADATA] %s sent OK to %s msgId=%lu",
      isPush ? "PUSH" : "RESP", MAC_STR(peerMac), (unsigned long)msgId);
  } else {
    WARN_ESPNOWF("[METADATA] %s FAILED to send to %s (peer not in hw table?)",
      isPush ? "PUSH" : "RESP", MAC_STR(peerMac));
  }
}

/**
 * Process received metadata - store in gMeshPeerMeta
 */
static void processMetadata(const uint8_t* srcMac, const V3PayloadMetadata* metadata) {
  if (!srcMac || !metadata || !gMeshPeerMeta) {
    WARN_ESPNOWF("[METADATA] processMetadata: null guard failed srcMac=%p metadata=%p gMeshPeerMeta=%p slots=%d",
      srcMac, metadata, gMeshPeerMeta, gMeshPeerSlots);
    return;
  }
  
  DEBUG_ESPNOW_METADATAF("[METADATA] processMetadata: srcMac=%s slots=%d name='%s' room='%s' zone='%s' friendlyName='%s'",
    MAC_STR(srcMac), gMeshPeerSlots,
    metadata->deviceName, metadata->room, metadata->zone, metadata->friendlyName);
  
  // Find or create entry in gMeshPeerMeta
  int idx = -1;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeerMeta[i].isActive && memcmp(gMeshPeerMeta[i].mac, srcMac, 6) == 0) {
      idx = i;
      DEBUG_ESPNOW_METADATAF("[METADATA] processMetadata: found existing slot=%d for %s",
        i, MAC_STR(srcMac));
      break;
    }
  }
  
  if (idx == -1) {
    for (int i = 0; i < gMeshPeerSlots; i++) {
      if (!gMeshPeerMeta[i].isActive) {
        idx = i;
        gMeshPeerMeta[i].clear();
        memcpy(gMeshPeerMeta[i].mac, srcMac, 6);
        gMeshPeerMeta[i].isActive = true;
        DEBUG_ESPNOW_METADATAF("[METADATA] processMetadata: allocated new slot=%d for %s",
          i, MAC_STR(srcMac));
        break;
      }
    }
  }
  
  if (idx == -1) {
    WARN_ESPNOWF("[METADATA] processMetadata: ALL %d slots full, cannot store metadata from %s",
      gMeshPeerSlots, MAC_STR(srcMac));
    return;
  }
  
  MeshPeerMeta* meta = &gMeshPeerMeta[idx];
  strncpy(meta->name, metadata->deviceName, sizeof(meta->name) - 1);
  meta->name[sizeof(meta->name) - 1] = '\0';
  strncpy(meta->friendlyName, metadata->friendlyName, sizeof(meta->friendlyName) - 1);
  meta->friendlyName[sizeof(meta->friendlyName) - 1] = '\0';
  strncpy(meta->room, metadata->room, sizeof(meta->room) - 1);
  meta->room[sizeof(meta->room) - 1] = '\0';
  strncpy(meta->zone, metadata->zone, sizeof(meta->zone) - 1);
  meta->zone[sizeof(meta->zone) - 1] = '\0';
  strncpy(meta->tags, metadata->tags, sizeof(meta->tags) - 1);
  meta->tags[sizeof(meta->tags) - 1] = '\0';
  meta->stationary = (metadata->stationary != 0);
  meta->lastMetaUpdate = millis();
  
  DEBUG_ESPNOW_METADATAF("[METADATA] processMetadata: stored slot=%d mac=%s name='%s' friendlyName='%s' room='%s' zone='%s' tags='%s' stationary=%d isActive=%d",
    idx, MAC_STR(srcMac),
    meta->name, meta->friendlyName, meta->room, meta->zone, meta->tags,
    (int)meta->stationary, (int)meta->isActive);
}

/**
 * Master Metadata Push: Broadcast metadata to all encrypted mesh peers
 * Security: Only sends to verified encrypted peers
 */
static void broadcastMetadataToMesh() {
  if (!gEspNow || !gEspNow->initialized) return;
  
  esp_now_peer_info_t peer;
  esp_now_peer_num_t peerNum;
  
  esp_now_get_peer_num(&peerNum);
  if (peerNum.total_num == 0) return;
  
  // First send our own metadata to all encrypted peers
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  
  V3PayloadMetadata myMetadata;
  buildMetadataPayload(&myMetadata);
  
  int sentCount = 0;
  
  // Iterate through all peers
  for (int i = 0; i < peerNum.total_num; i++) {
    if (esp_now_fetch_peer(true, &peer) == ESP_OK) {
      // Security: Only send to encrypted peers (verified mesh members)
      if (peer.encrypt) {
        // Send our metadata
        uint32_t msgId = generateMessageId();
        if (v3_send_frame(peer.peer_addr, ESPNOW_V3_TYPE_METADATA_PUSH, 0, msgId, 
                         (const uint8_t*)&myMetadata, sizeof(myMetadata), 1)) {
          sentCount++;
          delay(5);  // Small delay between sends to avoid congestion
        }
        
        // Also forward stored metadata from other workers (master push)
        for (int j = 0; j < gMeshPeerSlots; j++) {
          if (gMeshPeerMeta && gMeshPeerMeta[j].isActive && gMeshPeerMeta[j].lastMetaUpdate > 0 && 
              !macEqual6(gMeshPeerMeta[j].mac, peer.peer_addr) &&  // Don't echo back
              !macEqual6(gMeshPeerMeta[j].mac, myMac)) {  // Don't forward our own
            
            // Build metadata payload from stored data
            V3PayloadMetadata fwdMetadata;
            memset(&fwdMetadata, 0, sizeof(fwdMetadata));
            strncpy(fwdMetadata.deviceName, gMeshPeerMeta[j].name, sizeof(fwdMetadata.deviceName) - 1);
            strncpy(fwdMetadata.friendlyName, gMeshPeerMeta[j].friendlyName, sizeof(fwdMetadata.friendlyName) - 1);
            strncpy(fwdMetadata.room, gMeshPeerMeta[j].room, sizeof(fwdMetadata.room) - 1);
            strncpy(fwdMetadata.zone, gMeshPeerMeta[j].zone, sizeof(fwdMetadata.zone) - 1);
            strncpy(fwdMetadata.tags, gMeshPeerMeta[j].tags, sizeof(fwdMetadata.tags) - 1);
            fwdMetadata.stationary = gMeshPeerMeta[j].stationary ? 1 : 0;
            
            uint32_t fwdMsgId = generateMessageId();
            v3_send_frame(peer.peer_addr, ESPNOW_V3_TYPE_METADATA_PUSH, 0, fwdMsgId,
                         (const uint8_t*)&fwdMetadata, sizeof(fwdMetadata), 1);
            delay(5);
          }
        }
      }
    }
  }
  
  if (sentCount > 0) {
    DEBUG_ESPNOWF("[METADATA_PUSH] Broadcast to %d encrypted peers", sentCount);
  }
}

// ==========================
// Message Handler Implementations
// ==========================

// Update peer metadata from a worker status JSON payload
static void updatePeerMetaFromWorkerStatus(const uint8_t* srcMac, JsonVariant payload) {
  if (!srcMac) return;
  MeshPeerMeta* meta = getMeshPeerMeta(srcMac, true);
  if (!meta) return;
  const char* name         = payload["name"]         | "";
  const char* friendlyName = payload["friendlyName"] | "";
  const char* room         = payload["room"]         | "";
  const char* zone         = payload["zone"]         | "";
  const char* tags         = payload["tags"]         | "";
  if (name[0])         strncpy(meta->name,         name,         sizeof(meta->name) - 1);
  if (friendlyName[0]) strncpy(meta->friendlyName, friendlyName, sizeof(meta->friendlyName) - 1);
  if (room[0])         strncpy(meta->room,         room,         sizeof(meta->room) - 1);
  if (zone[0])         strncpy(meta->zone,         zone,         sizeof(meta->zone) - 1);
  if (tags[0])         strncpy(meta->tags,         tags,         sizeof(meta->tags) - 1);
  meta->lastMetaUpdate = millis();
  meta->isActive = true;
  memcpy(meta->mac, srcMac, 6);
}

/**
 * @brief Handle JSON messages (heartbeats, ACKs, mesh envelopes, topology)
 * @param ctx Received message context
 * @return true if handled
 */
static bool handleJsonMessage(const ReceivedMessage& ctx) {
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[HANDLER] === handleJsonMessage ENTRY ===");
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[HANDLER] Message length: %d bytes", ctx.message.length());
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[HANDLER] Content (first 80 chars): %.80s", ctx.message.c_str());
  
  // CRITICAL: Validate ctx.recvInfo is not NULL before dereferencing
  if (!ctx.recvInfo) {
    DEBUGF(DEBUG_ESPNOW_STREAM, "[HANDLER] CRITICAL ERROR: handleJsonMessage called with NULL recvInfo");
    return false;
  }
  
  PSRAM_JSON_DOC(doc);
  if (!parseJsonMessage(ctx.message, doc)) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[Dispatch] Failed to parse JSON");
    return false;
  }
  
  
  const char* type = doc["type"];
  if (!type) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[Dispatch] JSON missing 'type' field");
    return false;
  }
  
  // Handle boot notification messages
  if (strcmp(type, MSG_TYPE_BOOT) == 0) {
    JsonObject payload = doc["pld"].as<JsonObject>();
    uint32_t bootCounter = payload["bootCounter"] | 0;
    uint32_t timestamp = payload["timestamp"] | 0;
    
    // Update peer boot counter
    MeshPeerHealth* peer = getMeshPeerHealth(ctx.recvInfo->src_addr, true);
    if (peer) {
      uint32_t prevCounter = peer->lastBootCounter;
      peer->lastBootCounter = bootCounter;
      
      if (prevCounter > 0 && bootCounter > prevCounter + 1) {
        // Device rebooted multiple times while we were offline or out of range
        DEBUGF_BROADCAST(DEBUG_ESPNOW_STREAM, "[BOOT] %s rebooted %lu times (missed %lu events)",
                         ctx.deviceName.c_str(), bootCounter, bootCounter - prevCounter - 1);
      } else if (prevCounter > 0 && bootCounter < prevCounter) {
        // Boot counter reset - device was replaced or NVS cleared
        DEBUGF_BROADCAST(DEBUG_ESPNOW_STREAM, "[BOOT] %s boot counter reset (%lu -> %lu)",
                         ctx.deviceName.c_str(), prevCounter, bootCounter);
      } else {
        // Normal reboot
        DEBUGF_BROADCAST(DEBUG_ESPNOW_STREAM, "[BOOT] %s rebooted (counter=%lu)",
                         ctx.deviceName.c_str(), bootCounter);
      }
    }
    return true;
  }
  
  // Handle heartbeat messages
  if (strcmp(type, MSG_TYPE_HB) == 0) {
    if (meshEnabled()) {
      if (ctx.isPaired) {
        // Update or create peer health entry for paired device
        MeshPeerHealth* peer = getMeshPeerHealth(ctx.recvInfo->src_addr, true);
        if (peer) {
          peer->lastHeartbeatMs = millis();
          peer->heartbeatCount++;
          
          uint32_t msgId = doc["msgId"];
          DEBUGF(DEBUG_ESPNOW_STREAM, "[MESH] JSON heartbeat from %s (count=%lu, msgId=%lu)",
                 ctx.macStr.c_str(), (unsigned long)peer->heartbeatCount, (unsigned long)msgId);
        }
      } else {
        // Track unpaired device sending heartbeats (for discovery/pairing)
        String srcName = doc["src"] | "";
        if (srcName.length() == 0) {
          srcName = ctx.deviceName;
        }
        int rssi = ctx.recvInfo->rx_ctrl ? ctx.recvInfo->rx_ctrl->rssi : -100;
        updateUnpairedDevice(ctx.recvInfo->src_addr, srcName, rssi);
        DEBUGF(DEBUG_ESPNOW_STREAM, "[MESH] Unpaired device heartbeat: %s (%s) RSSI=%d",
               ctx.macStr.c_str(), srcName.c_str(), rssi);
      }
    }
    return true;
  }
  
  // Handle ACK messages
  if (strcmp(type, MSG_TYPE_ACK) == 0) {
    if (meshEnabled()) {
      uint32_t ackFor = doc["ackFor"];
      
      // Update peer health entry
      MeshPeerHealth* peer = getMeshPeerHealth(ctx.recvInfo->src_addr, true);
      if (peer) {
        peer->lastAckMs = millis();
        peer->ackCount++;
      }
      
      // Remove from retry queue
      meshRetryDequeue(ackFor);
      
      DEBUGF_BROADCAST(DEBUG_ESPNOW_STREAM, "[MESH] ACK received for msgid=%lu", (unsigned long)ackFor);
    }
    return true;
  }
  
  // Handle mesh system control messages (master/worker control plane)
  if (strcmp(type, MSG_TYPE_MESH_SYS) == 0) {
    JsonObject payload = doc["pld"].as<JsonObject>();
    const char* kind = payload["kind"] | "";

    // Master heartbeat for backup nodes
    if (strcmp(kind, "masterHb") == 0) {
      if (meshEnabled() && gSettings.meshRole == MESH_ROLE_BACKUP_MASTER) {
        gEspNow->heartbeatsReceived++;
        gLastMasterHeartbeat = millis();
        gBackupPromoted = false;  // Reset promotion flag if master is alive
        DEBUGF(DEBUG_ESPNOW_STREAM, "[BACKUP] JSON master heartbeat received from %s", ctx.macStr.c_str());
      }
      return true;
    }

    // Worker status reports for master node
    if (strcmp(kind, "workerStatus") == 0) {
      if (meshEnabled() && gSettings.meshRole == MESH_ROLE_MASTER) {
        // Decode workerStatus telemetry fields (see buildMeshSysWorkerStatus for schema)
        const char* workerMAC = payload["mac"] | ctx.macStr.c_str();
        const char* workerName = payload["name"] | "";
        uint32_t freeHeap = (uint32_t)(payload["free"] | 0);
        uint32_t totalHeap = (uint32_t)(payload["total"] | 0);
        int rssi = (int)(payload["rssi"] | 0);
        bool thermal = (bool)(payload["thermal"] | false);
        bool imu = (bool)(payload["imu"] | false);

        int heapPercent = (totalHeap > 0) ? ((freeHeap * 100) / totalHeap) : 0;

        DEBUGF(DEBUG_ESPNOW_STREAM, "[MASTER] Worker status from %s (%s)",
               workerName[0] ? workerName : workerMAC,
               workerMAC);

        BROADCAST_PRINTF("[MESH] Worker %s: heap=%lu/%lu (%d%% free) rssi=%ddBm thermal=%s imu=%s",
                         workerName[0] ? workerName : workerMAC,
                         (unsigned long)freeHeap,
                         (unsigned long)totalHeap,
                         heapPercent,
                         rssi,
                         thermal ? "ON" : "OFF",
                         imu ? "ON" : "OFF");

        // Update peer metadata for room/zone/tags aggregation
        if (ctx.recvInfo && ctx.recvInfo->src_addr) {
          updatePeerMetaFromWorkerStatus(ctx.recvInfo->src_addr, payload);
        }
      }
      return true;
    }

    return false;
  }

  // Handle MESH_SYS routed messages - delegate to existing complex handler
  if (strcmp(type, MSG_TYPE_MESH_SYS) == 0) {
    // This is complex (topology, time sync, commands, forwarding)
    // Keep existing logic in onEspNowDataReceived for now
    return false;  // Fall through to legacy handler
  }

  // Handle FILE transfer messages - v2 JSON format is DEPRECATED, use v3 binary frames
  // Block v2 FILE processing to prevent crashes from malformed JSON fragments
  if (strcmp(type, MSG_TYPE_FILE_STR) == 0) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[FILE] v2 JSON file message ignored (deprecated) - use v3 binary protocol");
    return true;  // Consume but don't process
  }

  // Handle CMD (remote command) messages
  if (strcmp(type, MSG_TYPE_CMD) == 0) {
    // Security: Only allow remote commands over encrypted connections
    if (!ctx.isEncrypted) {
      broadcastOutput("[ESP-NOW] SECURITY: Remote command rejected - encryption required");
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[CMD] Remote command rejected from %s - not encrypted", ctx.macStr.c_str());
      return true;
    }

    JsonObject payload = doc["pld"].as<JsonObject>();
    const char* username = payload["user"] | "";
    const char* password = payload["pass"] | "";
    const char* command = payload["cmd"] | "";

    if (strlen(username) == 0 || strlen(password) == 0 || strlen(command) == 0) {
      broadcastOutput("[ESP-NOW] Remote command: Invalid format - missing user/pass/cmd");
      return true;
    }

    DEBUGF(DEBUG_ESPNOW_ROUTER, "[CMD] Remote command from %s: user='%s' cmd='%s'",
           ctx.deviceName.c_str(), username, command);

    // Authenticate user
    if (!isValidUser(String(username), String(password))) {
      BROADCAST_PRINTF("[ESP-NOW] Remote command: Authentication FAILED for user '%s'", username);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[CMD] Auth failed for user '%s'", username);
      return true;
    }

    DEBUGF(DEBUG_ESPNOW_ROUTER, "[CMD] Authentication successful for user '%s'", username);

    // Set execution context for remote command
    gExecAuthContext.user = username;
    gExecAuthContext.ip = "espnow:" + ctx.macStr;
    gExecAuthContext.opaque = (void*)ctx.recvInfo->src_addr;

    // Execute command
    BROADCAST_PRINTF("[ESP-NOW] Executing remote command from %s: %s", ctx.deviceName.c_str(), command);
    static char* cmdResult = nullptr;
    if (!cmdResult) cmdResult = (char*)ps_alloc(2048, AllocPref::PreferPSRAM, "espnow.cmdResult");
    if (!cmdResult) {
      broadcastOutput("[ESP-NOW] ERROR: Failed to allocate command result buffer");
      return false;
    }
    bool success = executeCommand(gExecAuthContext, command, cmdResult, 2048);
    const char* result = success ? cmdResult : "Command execution failed";

    // Send response
    bool cmdSuccess = (strstr(result, "[SUCCESS]") != nullptr || strstr(result, "FAILED") == nullptr);
    sendChunkedResponse(ctx.recvInfo->src_addr, cmdSuccess, String(result), ctx.deviceName, ctx.cmdMsgId);


    // Clear execution context
    gExecAuthContext.user = "";
    gExecAuthContext.ip = "";
    gExecAuthContext.opaque = nullptr;

    return true;
  }

  // Handle RESPONSE (remote command result) messages
  if (strcmp(type, MSG_TYPE_RESPONSE) == 0) {
    JsonObject payload = doc["pld"].as<JsonObject>();
    const char* kind = payload["kind"] | "";
    
    if (strcmp(kind, "remoteCmdResult") == 0) {
      bool ok = payload["ok"] | false;
      const char* msg = payload["msg"] | "";
      
      String deviceName = ctx.deviceName.length() > 0 ? ctx.deviceName : ctx.macStr;
      BROADCAST_PRINTF("[ESP-NOW] Response from %s:", deviceName.c_str());
      broadcastOutput(String(msg));
      storeMessageInPeerHistory((uint8_t*)ctx.recvInfo->src_addr,
                                deviceName.c_str(),
                                msg,
                                ctx.isEncrypted,
                                MSG_TEXT);
      
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[RESPONSE] Remote command result from %s: ok=%s", 
             deviceName.c_str(), ok ? "true" : "false");
      
      // Send ACK back to sender
      uint32_t msgId = doc["id"] | doc["msgId"] | 0;
      if (msgId != 0) {
        v3_send_ack(ctx.recvInfo->src_addr, msgId);
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[RESPONSE] Sent ACK for msgId=%lu to sender", (unsigned long)msgId);
      }
      
      return true;
    }
    
    if (strcmp(kind, "userSyncResult") == 0) {
      bool ok = payload["ok"] | false;
      const char* msg = payload["msg"] | "";
      const char* username = payload["username"] | "";
      uint32_t userId = payload["userId"] | 0;
      const char* role = payload["role"] | "";
      
      String deviceName = ctx.deviceName.length() > 0 ? ctx.deviceName : ctx.macStr;
      
      if (ok) {
        if (userId > 0) {
          INFO_USERF("[USER_SYNC] ✓ %s: %s (user='%s', id=%lu, role=%s)", 
                    deviceName.c_str(), msg, username, (unsigned long)userId, role);
          BROADCAST_PRINTF("[ESP-NOW] User sync SUCCESS from %s: %s (user='%s', id=%lu, role=%s)", deviceName.c_str(), msg, username, (unsigned long)userId, role);
        } else {
          INFO_USERF("[USER_SYNC] ✓ %s: %s (user='%s')", deviceName.c_str(), msg, username);
          BROADCAST_PRINTF("[ESP-NOW] User sync from %s: %s (user='%s')", deviceName.c_str(), msg, username);
        }
      } else {
        ERROR_USERF("[USER_SYNC] ✗ %s: %s (user='%s')", deviceName.c_str(), msg, username);
        BROADCAST_PRINTF("[ESP-NOW] User sync FAILED from %s: %s (user='%s')", deviceName.c_str(), msg, username);
      }
      
      
      return true;
    }
    
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RESPONSE] Unknown kind: %s", kind);
    return false;
  }
  
#if ENABLE_BONDED_MODE
  // === BOND messages now use v3 binary protocol - block v2 JSON versions ===
  if (strcmp(type, MSG_TYPE_BOND_CAP_REQ) == 0 ||
      strcmp(type, MSG_TYPE_BOND_CAP_RESP) == 0 ||
      strcmp(type, MSG_TYPE_BOND_MANIFEST_REQ) == 0 ||
      strcmp(type, MSG_TYPE_BOND_MANIFEST_RESP) == 0) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[BOND] Ignoring v2 JSON %s from %s - use v3 binary protocol", type, ctx.macStr.c_str());
    return true;  // Consume but don't process
  }
#endif // ENABLE_BONDED_MODE

  // Handle TEXT (plain text) messages
  if (strcmp(type, MSG_TYPE_TEXT) == 0) {
    JsonObject payload = doc["pld"].as<JsonObject>();
    const char* msg = payload["msg"] | "";
    
    String deviceName = ctx.deviceName.length() > 0 ? ctx.deviceName : ctx.macStr;
    String encStatus = ctx.isEncrypted ? " [ENCRYPTED]" : " [UNENCRYPTED]";
    BROADCAST_PRINTF("[ESP-NOW] %s: %s%s", deviceName.c_str(), msg, encStatus.c_str());
    
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[TEXT] Plain text from %s: %.80s", deviceName.c_str(), msg);
    
    // Store in per-device message buffer for web UI and OLED
    storeMessageInPeerHistory(
      (uint8_t*)ctx.recvInfo->src_addr,
      deviceName.c_str(),
      msg,
      ctx.isEncrypted,
      MSG_TEXT
    );
    
    
    return true;
  }

  // Handle FILE_BROWSE (remote file browsing) messages
  if (strcmp(type, MSG_TYPE_FILE_BROWSE) == 0) {
    // Security: Only allow file browsing over encrypted connections
    if (!ctx.isEncrypted) {
      broadcastOutput("[ESP-NOW] SECURITY: File browse rejected - encryption required");
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[FILE_BROWSE] Rejected from %s - not encrypted", ctx.macStr.c_str());
      return true;
    }

    JsonObject payload = doc["pld"].as<JsonObject>();
    const char* kind = payload["kind"] | "";
    const char* path = payload["path"] | "/";

    DEBUGF(DEBUG_ESPNOW_ROUTER, "[FILE_BROWSE] Message from %s: kind='%s' path='%s'",
           ctx.deviceName.c_str(), kind, path);

    // Handle list_result response (we sent a browse request, remote sent back the listing)
    // Responses don't require auth - they're replies to our requests
    if (strcmp(kind, "list_result") == 0) {
      bool ok = payload["ok"] | false;
      const char* resultPath = payload["path"] | "/";
      
      String deviceName = ctx.deviceName.length() > 0 ? ctx.deviceName : ctx.macStr;
      
      if (ok) {
        JsonArray files = payload["files"].as<JsonArray>();
        String summary = String("File listing for ") + resultPath;
        if (files.size() == 0) summary += " (empty directory)";
        storeMessageInPeerHistory((uint8_t*)ctx.recvInfo->src_addr,
                                  deviceName.c_str(),
                                  summary.c_str(),
                                  ctx.isEncrypted,
                                  MSG_TEXT);
        
        BROADCAST_PRINTF("[ESP-NOW] File listing from %s for path: %s", deviceName.c_str(), resultPath);
        broadcastOutput("--------------------------------------------");
        
        if (files.size() == 0) {
          broadcastOutput("  (empty directory)");
        } else {
          for (JsonVariant file : files) {
            String name = file["name"] | "";
            String type = file["type"] | "file";
            String size = file["size"] | "";
            
            if (type == "folder") {
              String line = String("[DIR] ") + name + "/";
              storeMessageInPeerHistory((uint8_t*)ctx.recvInfo->src_addr,
                                        deviceName.c_str(),
                                        line.c_str(),
                                        ctx.isEncrypted,
                                        MSG_TEXT);
              broadcastOutput("  [DIR]  " + name + "/");
            } else {
              String line = String("[FILE] ") + name + " (" + size + ")";
              storeMessageInPeerHistory((uint8_t*)ctx.recvInfo->src_addr,
                                        deviceName.c_str(),
                                        line.c_str(),
                                        ctx.isEncrypted,
                                        MSG_TEXT);
              broadcastOutput("  [FILE] " + name + " (" + size + ")");
            }
          }
        }
        broadcastOutput("--------------------------------------------");
        
        // Store the result for OLED file browser if applicable
        extern void storeRemoteFileBrowseResult(const uint8_t* mac, const char* path, JsonArray& files);
        storeRemoteFileBrowseResult(ctx.recvInfo->src_addr, resultPath, files);
        
      } else {
        const char* error = payload["error"] | "Unknown error";
        String failMsg = String("File browse FAILED: ") + error;
        storeMessageInPeerHistory((uint8_t*)ctx.recvInfo->src_addr,
                                  deviceName.c_str(),
                                  failMsg.c_str(),
                                  ctx.isEncrypted,
                                  MSG_TEXT);
        BROADCAST_PRINTF("[ESP-NOW] File browse FAILED from %s: %s", deviceName.c_str(), error);
      }
      
      
      return true;
    }

    // For list and fetch requests, require authentication
    const char* username = payload["user"] | "";
    const char* password = payload["pass"] | "";
    
    if (strlen(username) == 0 || strlen(password) == 0) {
      broadcastOutput("[ESP-NOW] File browse: Missing credentials");
      return true;
    }

    if (!isValidUser(String(username), String(password))) {
      BROADCAST_PRINTF("[ESP-NOW] File browse: Authentication FAILED for user '%s'", username);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[FILE_BROWSE] Auth failed for user '%s'", username);
      return true;
    }

    if (!isAdminUser(String(username))) {
      broadcastOutput("[ESP-NOW] File browse: Admin privileges required");
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[FILE_BROWSE] User '%s' is not admin", username);
      return true;
    }

    DEBUGF(DEBUG_ESPNOW_ROUTER, "[FILE_BROWSE] Authenticated request: user='%s' kind='%s' path='%s'",
           username, kind, path);

    // Handle list request
    if (strcmp(kind, "list") == 0) {
      extern bool filesystemReady;
      extern bool buildFilesListing(const String& inPath, String& out, bool asJson);
      
      String filesJson;
      bool ok = false;
      
      if (filesystemReady) {
        ok = buildFilesListing(String(path), filesJson, true);
      }
      
      // Build response
      JsonDocument respDoc;
      v2_init_envelope(respDoc, MSG_TYPE_FILE_BROWSE, generateMessageId(), 
                      gSettings.espnowDeviceName.c_str(), ctx.deviceName.c_str(), -1);
      JsonObject pld = respDoc["pld"].to<JsonObject>();
      pld["kind"] = "list_result";
      pld["path"] = path;
      pld["ok"] = ok;
      
      if (ok) {
        // Parse the files JSON string into a proper JSON array
        JsonDocument filesDoc;
        String wrappedJson = String("[") + filesJson + "]";
        DeserializationError err = deserializeJson(filesDoc, wrappedJson);
        if (err == DeserializationError::Ok) {
          pld["files"] = filesDoc.as<JsonArray>();
        } else {
          // Fallback: send as string
          pld["filesRaw"] = filesJson;
        }
      } else {
        pld["error"] = filesystemReady ? "Directory not found" : "Filesystem not ready";
      }
      
      String respStr;
      serializeJson(respDoc, respStr);
      v3_send_file_response(ctx.recvInfo->src_addr, generateMessageId(), ok, respStr.c_str());
      
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[FILE_BROWSE] Sent V3 list response for path '%s' ok=%d", path, ok);
      BROADCAST_PRINTF("[ESP-NOW] File browse: Sent directory listing for %s", path);
      return true;
    }
    
    // Handle fetch request (request to send a file back)
    if (strcmp(kind, "fetch") == 0) {
      extern bool filesystemReady;
      
      if (!filesystemReady) {
        broadcastOutput("[ESP-NOW] File fetch: Filesystem not ready");
        return true;
      }
      
      // Use existing file send mechanism
      String filePath = String(path);
      {
        FsLockGuard guard("espnow.file_fetch.exists");
        if (!LittleFS.exists(filePath)) {
        broadcastOutput("[ESP-NOW] File fetch: File not found: " + filePath);
        return true;
        }
      }
      
      // Queue file for sending back to requester
      // Reuse existing sendFile function with target MAC
      extern bool sendFileToMac(const uint8_t* mac, const String& localPath);
      bool sent = sendFileToMac(ctx.recvInfo->src_addr, filePath);
      
      if (sent) {
        broadcastOutput("[ESP-NOW] File fetch: Sending " + filePath + " to " + ctx.deviceName);
      } else {
        broadcastOutput("[ESP-NOW] File fetch: Failed to send " + filePath);
      }
      return true;
    }

    DEBUGF(DEBUG_ESPNOW_ROUTER, "[FILE_BROWSE] Unknown kind: %s", kind);
    return false;
  }

  // Handle USER_SYNC (user credential propagation) messages
  if (strcmp(type, MSG_TYPE_USER_SYNC) == 0) {
    // Check if user sync is enabled
    extern Settings gSettings;
    if (!gSettings.espnowUserSyncEnabled) {
      WARN_ESPNOWF("[USER_SYNC] User sync disabled - rejecting sync request from %s", ctx.deviceName.c_str());
      broadcastOutput("[ESP-NOW] User sync DISABLED - enable with 'espnow usersync on'");
      return true;
    }

    // Security: Only allow user sync over encrypted connections
    if (!ctx.isEncrypted) {
      ERROR_ESPNOWF("[USER_SYNC] SECURITY: User sync rejected from %s - encryption required", ctx.macStr.c_str());
      broadcastOutput("[ESP-NOW] SECURITY: User sync rejected - encryption required");
      return true;
    }

    JsonObject payload = doc["pld"].as<JsonObject>();
    const char* adminUser = payload["admin_user"] | "";
    const char* adminPass = payload["admin_pass"] | "";
    const char* targetUser = payload["target_user"] | "";
    const char* targetPass = payload["target_pass"] | "";
    const char* role = payload["role"] | "user";

    if (strlen(adminUser) == 0 || strlen(adminPass) == 0 || strlen(targetUser) == 0 || strlen(targetPass) == 0) {
      WARN_ESPNOWF("[USER_SYNC] Invalid format - missing required fields");
      broadcastOutput("[ESP-NOW] User sync: Invalid format - missing fields");
      
      // Send error response
      uint32_t msgId = doc["id"] | doc["msgId"] | 0;
      if (msgId != 0) {
        JsonDocument respDoc;
        v2_init_envelope(respDoc, MSG_TYPE_RESPONSE, generateMessageId(), 
                        gSettings.espnowDeviceName.c_str(), ctx.deviceName.c_str(), -1);
        JsonObject pld = respDoc["pld"].to<JsonObject>();
        pld["kind"] = "userSyncResult";
        pld["ok"] = false;
        pld["msg"] = "Invalid format - missing required fields";
        pld["username"] = targetUser;
        v3_send_file_response(ctx.recvInfo->src_addr, msgId, false, "Invalid format - missing required fields");
      }
      return true;
    }

    INFO_ESPNOWF("[USER_SYNC] Request from %s: admin='%s' target='%s' role='%s'",
           ctx.deviceName.c_str(), adminUser, targetUser, role);

    // Authenticate admin user on THIS device
    if (!isValidUser(String(adminUser), String(adminPass))) {
      ERROR_ESPNOWF("[USER_SYNC] Authentication FAILED for admin '%s'", adminUser);
      broadcastOutput("[ESP-NOW] User sync: Admin authentication FAILED");
      
      // Send error response
      uint32_t msgId = doc["id"] | doc["msgId"] | 0;
      if (msgId != 0) {
        JsonDocument respDoc;
        v2_init_envelope(respDoc, MSG_TYPE_RESPONSE, generateMessageId(), 
                        gSettings.espnowDeviceName.c_str(), ctx.deviceName.c_str(), -1);
        JsonObject pld = respDoc["pld"].to<JsonObject>();
        pld["kind"] = "userSyncResult";
        pld["ok"] = false;
        pld["msg"] = "Admin authentication failed";
        pld["username"] = targetUser;
        v3_send_file_response(ctx.recvInfo->src_addr, msgId, false, "Admin authentication failed");
      }
      return true;
    }

    // Verify admin privileges on THIS device
    if (!isAdminUser(String(adminUser))) {
      ERROR_ESPNOWF("[USER_SYNC] User '%s' is not an admin - sync rejected", adminUser);
      broadcastOutput("[ESP-NOW] User sync: Admin privileges required");
      
      // Send error response
      uint32_t msgId = doc["id"] | doc["msgId"] | 0;
      if (msgId != 0) {
        JsonDocument respDoc;
        v2_init_envelope(respDoc, MSG_TYPE_RESPONSE, generateMessageId(), 
                        gSettings.espnowDeviceName.c_str(), ctx.deviceName.c_str(), -1);
        JsonObject pld = respDoc["pld"].to<JsonObject>();
        pld["kind"] = "userSyncResult";
        pld["ok"] = false;
        pld["msg"] = "Admin privileges required";
        pld["username"] = targetUser;
        v3_send_file_response(ctx.recvInfo->src_addr, msgId, false, "Admin privileges required");
      }
      return true;
    }

    INFO_ESPNOWF("[USER_SYNC] Admin authentication successful for '%s'", adminUser);

    // Check if target user already exists
    uint32_t existingUserId = 0;
    if (getUserIdByUsername(String(targetUser), existingUserId)) {
      WARN_ESPNOWF("[USER_SYNC] User '%s' already exists (id=%u) - skipping", targetUser, (unsigned)existingUserId);
      BROADCAST_PRINTF("[ESP-NOW] User sync: User '%s' already exists", targetUser);
      
      // Send response (not an error, just info)
      uint32_t msgId = doc["id"] | doc["msgId"] | 0;
      if (msgId != 0) {
        JsonDocument respDoc;
        v2_init_envelope(respDoc, MSG_TYPE_RESPONSE, generateMessageId(), 
                        gSettings.espnowDeviceName.c_str(), ctx.deviceName.c_str(), -1);
        JsonObject pld = respDoc["pld"].to<JsonObject>();
        pld["kind"] = "userSyncResult";
        pld["ok"] = true;
        pld["msg"] = "User already exists (skipped)";
        pld["username"] = targetUser;
        v3_send_file_response(ctx.recvInfo->src_addr, msgId, true, "User already exists (skipped)");
      }
      return true;
    }

    // Hash the plaintext password with THIS device's key
    String hashedPassword = hashUserPassword(String(targetPass));
    
    // Create user directly (similar to approvePendingUserInternal but simpler)
    extern uint32_t gBootSeq;
    extern uint32_t gBootCounter;
    extern bool filesystemReady;
    
    if (!filesystemReady) {
      ERROR_ESPNOWF("[USER_SYNC] Filesystem not ready");
      broadcastOutput("[ESP-NOW] User sync: Filesystem not ready");
      
      // Send error response
      uint32_t msgId = doc["id"] | doc["msgId"] | 0;
      if (msgId != 0) {
        JsonDocument respDoc;
        v2_init_envelope(respDoc, MSG_TYPE_RESPONSE, generateMessageId(), 
                        gSettings.espnowDeviceName.c_str(), ctx.deviceName.c_str(), -1);
        JsonObject pld = respDoc["pld"].to<JsonObject>();
        pld["kind"] = "userSyncResult";
        pld["ok"] = false;
        pld["msg"] = "Filesystem not ready";
        pld["username"] = targetUser;
        v3_send_file_response(ctx.recvInfo->src_addr, msgId, false, "Filesystem not ready");
      }
      return true;
    }

    FsLockGuard guard("user_sync.create");
    
    // Load users.json
    if (!LittleFS.exists(USERS_JSON_FILE)) {
      ERROR_ESPNOWF("[USER_SYNC] users.json does not exist");
      broadcastOutput("[ESP-NOW] User sync: users.json not found");
      return true;
    }

    File file = LittleFS.open(USERS_JSON_FILE, "r");
    if (!file) {
      ERROR_ESPNOWF("[USER_SYNC] Could not open users.json");
      broadcastOutput("[ESP-NOW] User sync: Could not open users.json");
      return true;
    }

    JsonDocument userDoc;
    DeserializationError error = deserializeJson(userDoc, file);
    file.close();

    if (error) {
      ERROR_ESPNOWF("[USER_SYNC] Malformed users.json");
      broadcastOutput("[ESP-NOW] User sync: Malformed users.json");
      return true;
    }

    // Get nextId
    int nextId = userDoc["nextId"] | 2;
    
    // Add new user
    JsonArray users = userDoc["users"];
    if (!users) {
      ERROR_ESPNOWF("[USER_SYNC] Missing users array");
      broadcastOutput("[ESP-NOW] User sync: Missing users array");
      return true;
    }

    JsonObject newUser = users.add<JsonObject>();
    newUser["id"] = nextId;
    newUser["username"] = targetUser;
    // Password now stored in per-user settings file, not here
    newUser["role"] = role;
    newUser["createdAt"] = (const char*)nullptr;  // null
    newUser["createdBy"] = String("espnow:") + ctx.deviceName;
    newUser["createdMs"] = millis();
    newUser["bootSeq"] = gBootSeq;
    newUser["bootCount"] = gBootCounter;

    // Update nextId
    userDoc["nextId"] = nextId + 1;

    // Write back to file
    file = LittleFS.open(USERS_JSON_FILE, "w");
    if (!file) {
      ERROR_ESPNOWF("[USER_SYNC] Could not write users.json");
      broadcastOutput("[ESP-NOW] User sync: Could not write users.json");
      return true;
    }
    
    size_t written = serializeJson(userDoc, file);
    file.close();

    if (written == 0) {
      ERROR_ESPNOWF("[USER_SYNC] Failed to write users.json");
      broadcastOutput("[ESP-NOW] User sync: Failed to write users.json");
      return true;
    }

    // Create user settings with password
    uint32_t createdUserId = (uint32_t)nextId;
    if (createdUserId > 0) {
      JsonDocument defaults;
      defaults["theme"] = "light";
      defaults["password"] = hashedPassword;  // Store password in user settings
      if (!saveUserSettings(createdUserId, defaults)) {
        WARN_ESPNOWF("[USER_SYNC] Failed to create settings for userId=%u", (unsigned)createdUserId);
      }
    }

    INFO_ESPNOWF("[USER_SYNC] ✓ Created user '%s' (id=%d, role=%s) from %s", 
           targetUser, nextId, role, ctx.deviceName.c_str());
    BROADCAST_PRINTF("[ESP-NOW] User sync: Created user '%s' (role=%s) from %s", targetUser, role, ctx.deviceName.c_str());

    // Send success response back to sender
    uint32_t msgId = doc["id"] | doc["msgId"] | 0;
    if (msgId != 0) {
      JsonDocument respDoc;
      v2_init_envelope(respDoc, MSG_TYPE_RESPONSE, generateMessageId(), 
                      gSettings.espnowDeviceName.c_str(), ctx.deviceName.c_str(), -1);
      JsonObject pld = respDoc["pld"].to<JsonObject>();
      pld["kind"] = "userSyncResult";
      pld["ok"] = true;
      pld["msg"] = "User created successfully";
      pld["username"] = targetUser;
      pld["userId"] = nextId;
      pld["role"] = role;
      char respBuf[256];
      snprintf(respBuf, sizeof(respBuf), "User created successfully (reqId=%lu)", (unsigned long)msgId);
      v3_send_file_response(ctx.recvInfo->src_addr, msgId, true, respBuf);
      
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[USER_SYNC] Sent success response for msgId=%lu to sender", (unsigned long)msgId);
    }

    return true;
  }

  // Handle SENSOR_STATUS messages (worker → master)
  if (strcmp(type, MSG_TYPE_SENSOR_STATUS) == 0) {
    String deviceName = ctx.deviceName.length() > 0 ? ctx.deviceName : ctx.macStr;
    handleSensorStatusMessage(ctx.recvInfo->src_addr, deviceName, ctx.message);
    return true;
  }

  // Handle SENSOR_DATA messages (worker → master)
  if (strcmp(type, MSG_TYPE_SENSOR_DATA) == 0) {
    String deviceName = ctx.deviceName.length() > 0 ? ctx.deviceName : ctx.macStr;
    handleSensorDataMessage(ctx.recvInfo->src_addr, deviceName, ctx.message);
    return true;
  }
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[Dispatch] Unknown JSON type: %s", type);
  return false;
}

// NOTE: v2 handleFileTransferMessage wrapper has been REMOVED - v3 binary protocol handles file transfers

/**
 * @brief Handle remote command messages (CMD:username:password:command)
 * @param ctx Received message context
 * @return true if handled
 * @note Only accepts commands over encrypted connections for security
 */
static bool handleCommandMessage(const ReceivedMessage& ctx) {
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[HANDLER] === handleCommandMessage ENTRY ===");
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[HANDLER] Command: %.80s", ctx.message.c_str());
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[HANDLER] From: %s, Encrypted: %s",
         ctx.deviceName.c_str(), ctx.isEncrypted ? "YES" : "NO");
  
  // CRITICAL: Validate ctx.recvInfo is not NULL before dereferencing
  if (!ctx.recvInfo) {
    DEBUGF(DEBUG_ESPNOW_STREAM, "[HANDLER] CRITICAL ERROR: handleCommandMessage called with NULL recvInfo");
    return false;
  }
  
  // Security: Only allow remote commands over encrypted connections
  if (!ctx.isEncrypted) {
    broadcastOutput("[ESP-NOW] SECURITY: Remote command rejected - encryption required");
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[Router] Remote command rejected from %s - not encrypted", 
           ctx.macStr.c_str());
    return true;  // Handled (rejected)
  }
  
  // Parse format: CMD:username:password:command
  String payload = ctx.message.substring(4);  // Remove "CMD:" prefix
  
  // Find colons: username:password:command
  int firstColon = payload.indexOf(':');
  int secondColon = payload.indexOf(':', firstColon + 1);
  
  if (firstColon < 0 || secondColon < 0) {
    broadcastOutput("[ESP-NOW] Remote command: Invalid format - need CMD:user:pass:command");
    return true;  // Handled (invalid)
  }
  
  String username = payload.substring(0, firstColon);
  String password = payload.substring(firstColon + 1, secondColon);
  String command = payload.substring(secondColon + 1);
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[Router] Remote command from %s: user='%s' cmd='%s'",
         ctx.deviceName.c_str(), username.c_str(), command.c_str());
  
  // Authenticate user
  if (!isValidUser(username, password)) {
    broadcastOutput("[ESP-NOW] Remote command: Authentication FAILED for user '" + username + "'");
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[Router] Auth failed for user '%s'", username.c_str());
    return true;  // Handled (auth failed)
  }
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[Router] Authentication successful for user '%s'", username.c_str());
  
  // Suspend streaming during remote command execution
  bool wasStreaming = gEspNow->streamingSuspended;
  gEspNow->streamingSuspended = true;
  
  // Execute command with authenticated context
  AuthContext authCtx;
  authCtx.transport = SOURCE_ESPNOW;  // Command source: ESP-NOW mesh
  authCtx.user = username;
  authCtx.ip = String("espnow:") + ctx.deviceName;
  authCtx.path = "/espnow-remote";
  authCtx.opaque = (void*)ctx.recvInfo->src_addr;
  
  char result[1024];
  bool success = executeCommand(authCtx, command.c_str(), result, sizeof(result));
  
  // Restore streaming state
  gEspNow->streamingSuspended = wasStreaming;
  
  // Log result
  String resultStr = result;
  String resultPreview = resultStr.length() > 100 ? resultStr.substring(0, 100) + "..." : resultStr;
  if (success) {
    broadcastOutput("[ESP-NOW] Remote command executed successfully");
  } else {
    broadcastOutput("[ESP-NOW] Remote command FAILED: " + resultPreview);
  }
  
  // Send result back to sender
  sendChunkedResponse(ctx.recvInfo->src_addr, success, resultStr, ctx.deviceName, ctx.cmdMsgId);
  
  return true;  // Handled
}



// ==========================
// Message Router (Send Path)
// ==========================

/**
 * @brief Core message router - sends message with automatic routing, chunking, and metrics
 * @param msg Message to send (will be modified with msgId and timestamp)
 * @return true if message sent successfully
 * @note This is the main entry point for all message sending
 */
/*
bool routerSend(Message& msg) {
  if (!gEspNow) return false;
  if (!gEspNow->initialized) return false;
  
  // Generate message ID and timestamp
  msg.msgId = generateMessageId();
  msg.timestamp = millis();
  
  // Start timing
  unsigned long startUs = micros();
  
  // Check if chunking needed
  bool needsChunking = shouldChunk(msg.payload.length());
  if (needsChunking) {
    gEspNow->routerMetrics.chunkedMessages++;
  }
  
  // Determine routing method
  bool useMesh = shouldUseMesh(msg.dstMac);
  if (useMesh) {
    gEspNow->routerMetrics.meshRoutes++;
  } else {
    gEspNow->routerMetrics.directRoutes++;
  }
  
  // Look up device info (needed for both direct and chunked sends)
  bool isEncrypted = false;
  String deviceName = "";
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (memcmp(gEspNow->devices[i].mac, msg.dstMac, 6) == 0) {
      isEncrypted = gEspNow->devices[i].encrypted;
      deviceName = gEspNow->devices[i].name;
      break;
    }
  }
  
  // Parse payload once for both TTL injection and msgId extraction
  String finalPayload = msg.payload;
  uint32_t actualMsgId = msg.msgId;
  
  if (msg.payload.startsWith("{")) {
    PSRAM_JSON_DOC(doc);
    DeserializationError err = deserializeJson(doc, msg.payload);
    if (!err) {
      // Extract msgId from JSON if present (do this first, before any modifications)
      uint32_t jsonMsgId = doc["id"] | doc["msgId"] | 0;
      if (jsonMsgId != 0) {
        actualMsgId = jsonMsgId;
        if (actualMsgId != msg.msgId) {
          DEBUGF(DEBUG_ESPNOW_ROUTER, "[Router] Using msgId=%lu from JSON payload", (unsigned long)actualMsgId);
        }
      }
      
      // If using mesh routing, inject TTL
      if (useMesh && (doc["ttl"].isNull() || doc["ttl"] == 0)) {
        // Update meshTTL if adaptive mode is enabled
        
        doc["ttl"] = gSettings.meshTTL;
        finalPayload = "";
        serializeJson(doc, finalPayload);
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[Router] Added TTL=%d for mesh routing (%s)", 
               gSettings.meshTTL, gSettings.meshAdaptiveTTL ? "adaptive" : "fixed");
      }
    } else {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[Router] WARNING: Failed to parse payload as JSON");
    }
  }
  
  // Send the message using unified transport layer
  // Size check determines fragmentation, mesh flag determines routing
  bool success = false;
  
  if (needsChunking) {
    success = sendV2Fragmented(msg.dstMac, finalPayload, actualMsgId, isEncrypted, deviceName, useMesh);
  } else {
    success = sendV2Unfragmented(msg.dstMac, finalPayload, actualMsgId, isEncrypted, deviceName, useMesh);
  }
  
  // Update metrics
  unsigned long elapsedUs = micros() - startUs;
  
  if (success) {
    gEspNow->routerMetrics.messagesSent++;
    
    // Update timing metrics (rolling average)
    if (gEspNow->routerMetrics.avgSendTimeUs == 0) {
      gEspNow->routerMetrics.avgSendTimeUs = elapsedUs;
    } else {
      gEspNow->routerMetrics.avgSendTimeUs = 
        (gEspNow->routerMetrics.avgSendTimeUs * 9 + elapsedUs) / 10;
    }
    
    if (elapsedUs > gEspNow->routerMetrics.maxSendTimeUs) {
      gEspNow->routerMetrics.maxSendTimeUs = elapsedUs;
    }
  } else {
    gEspNow->routerMetrics.messagesFailed++;
    
    // If message has retry enabled, enqueue it
    if (msg.maxRetries > 0) {
      if (enqueueMessage(msg)) {
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[Router] Message ID %lu queued for retry",
               (unsigned long)msg.msgId);
        return true;  // Return true since message is queued (will retry later)
      } else {
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[Router] Failed to queue message ID %lu",
               (unsigned long)msg.msgId);
      }
    }
  }
  
  return success;
}
*/ // End routerSend - V2 TRANSPORT DEPRECATED

// Unused helper functions removed (flagged by compiler warnings)



// ============================================================================
// HELPER FUNCTIONS (moved from .ino)
// ============================================================================

// Helper: Parse incoming JSON message
static bool parseJsonMessage(const String& message, JsonDocument& doc) {
  DeserializationError error = deserializeJson(doc, message);
  if (error) {
    WARN_ESPNOWF("Failed to parse JSON message: %s", error.c_str());
    return false;
  }
  return true;
}

// Helper: Generate unique message ID
uint32_t generateMessageId() {
  if (!gEspNow) return 0;
  return gEspNow->nextMessageId++;
}

// Helper: Check if chunking is needed
bool shouldChunk(size_t size) {
  return size > 250;
}

// Helper: Update unpaired device tracking
static void updateUnpairedDevice(const uint8_t* mac, const String& name, int rssi) {
  if (!gEspNow) return;
  
  // Filter out own MAC (STA and AP) to prevent self-pairing
  uint8_t selfSta[6], selfAp[6];
  esp_wifi_get_mac(WIFI_IF_STA, selfSta);
  esp_wifi_get_mac(WIFI_IF_AP, selfAp);
  if (memcmp(mac, selfSta, 6) == 0 || memcmp(mac, selfAp, 6) == 0) return;
  
  unsigned long now = millis();
  
  // Find existing or empty slot
  for (int i = 0; i < MAX_UNPAIRED_DEVICES; i++) {
    if (memcmp(gEspNow->unpairedDevices[i].mac, mac, 6) == 0) {
      // Update existing
      gEspNow->unpairedDevices[i].name = name;
      gEspNow->unpairedDevices[i].rssi = rssi;
      gEspNow->unpairedDevices[i].lastSeenMs = now;
      gEspNow->unpairedDevices[i].heartbeatCount++;
      return;
    }
  }
  
  // Add new (find empty slot)
  for (int i = 0; i < MAX_UNPAIRED_DEVICES; i++) {
    if (gEspNow->unpairedDevices[i].lastSeenMs == 0) {
      memcpy(gEspNow->unpairedDevices[i].mac, mac, 6);
      gEspNow->unpairedDevices[i].name = name;
      gEspNow->unpairedDevices[i].rssi = rssi;
      gEspNow->unpairedDevices[i].lastSeenMs = now;
      gEspNow->unpairedDevices[i].heartbeatCount = 1;
      if (gEspNow->unpairedDeviceCount < MAX_UNPAIRED_DEVICES) {
        gEspNow->unpairedDeviceCount++;
      }
      return;
    }
  }
}

// findOrAllocateChunkBuffer removed - unused (legacy CHUNK code was removed)

// ============================================================================
// TOPOLOGY STREAM MANAGEMENT (moved from .ino)
// ============================================================================


// Finalize a topology stream: flush accumulated data into global results buffer
static void finalizeTopologyStream(TopologyStream* stream) {
  if (!stream) return;
  char macBuf[18];
  formatMacAddressBuf(stream->senderMac, macBuf, sizeof(macBuf));
  String entry = String(stream->senderName) + " (" + macBuf + "):\n";
  if (stream->accumulatedData.length() > 0) {
    entry += stream->accumulatedData;
  } else {
    entry += "  (no peers)\n";
  }
  entry += "\n";
  gTopoResultsBuffer += entry;
  gTopoResponsesReceived++;
  stream->active = false;
  DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO] Finalized stream for %s (%d peers)", stream->senderName, stream->receivedPeers);
}

// Helper: Find existing topology stream by sender MAC + reqId
static TopologyStream* findTopoStream(const uint8_t* senderMac, uint32_t reqId) {
  TopoStreamsGuard guard("findTopoStream");
  for (int i = 0; i < MAX_CONCURRENT_TOPO_STREAMS; i++) {
    if (gTopoStreams[i].reqId == reqId &&
        macEqual6(gTopoStreams[i].senderMac, senderMac)) {
      return &gTopoStreams[i];
    }
  }
  return nullptr;
}

// findTopoStreamByReqId removed - unused

// Helper: Create new topology stream slot
static TopologyStream* createTopoStream(const uint8_t* senderMac, uint32_t reqId) {
  TopoStreamsGuard guard("createTopoStream");
  // First, try to find an inactive slot
  for (int i = 0; i < MAX_CONCURRENT_TOPO_STREAMS; i++) {
    if (!gTopoStreams[i].active) {
      memset(&gTopoStreams[i], 0, sizeof(TopologyStream));
      memcpy(gTopoStreams[i].senderMac, senderMac, 6);
      gTopoStreams[i].reqId = reqId;
      gTopoStreams[i].active = true;
      gTopoStreams[i].startTime = millis();
      gTopoStreams[i].accumulatedData = "";
      return &gTopoStreams[i];
    }
  }
  
  // All slots full - evict oldest
  int oldestIdx = 0;
  unsigned long oldestTime = gTopoStreams[0].startTime;
  for (int i = 1; i < MAX_CONCURRENT_TOPO_STREAMS; i++) {
    if (gTopoStreams[i].startTime < oldestTime) {
      oldestTime = gTopoStreams[i].startTime;
      oldestIdx = i;
    }
  }
  
  DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO] WARNING: All %d stream slots full, evicting oldest", MAX_CONCURRENT_TOPO_STREAMS);
  memset(&gTopoStreams[oldestIdx], 0, sizeof(TopologyStream));
  memcpy(gTopoStreams[oldestIdx].senderMac, senderMac, 6);
  gTopoStreams[oldestIdx].reqId = reqId;
  gTopoStreams[oldestIdx].active = true;
  gTopoStreams[oldestIdx].startTime = millis();
  gTopoStreams[oldestIdx].accumulatedData = "";
  return &gTopoStreams[oldestIdx];
}

// Helper: Find or create topology stream
static TopologyStream* findOrCreateTopoStream(const uint8_t* senderMac, uint32_t reqId) {
  TopologyStream* stream = findTopoStream(senderMac, reqId);
  if (stream) {
    return stream;
  }
  return createTopoStream(senderMac, reqId);
}

// Helper: Add or update device name in topology cache
static void addTopoDeviceName(const uint8_t* mac, const char* name) {
  if (!mac || !name || strlen(name) == 0) return;
  
  // Check if already exists
  for (int i = 0; i < MAX_TOPO_DEVICE_CACHE; i++) {
    if (gTopoDeviceCache[i].active && memcmp(gTopoDeviceCache[i].mac, mac, 6) == 0) {
      // Update existing entry
      strncpy(gTopoDeviceCache[i].name, name, 31);
      gTopoDeviceCache[i].name[31] = '\0';
      DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_CACHE] Updated device: %s = %s", MAC_STR(mac), name);
      return;
    }
  }
  
  // Find empty slot
  for (int i = 0; i < MAX_TOPO_DEVICE_CACHE; i++) {
    if (!gTopoDeviceCache[i].active) {
      memcpy(gTopoDeviceCache[i].mac, mac, 6);
      strncpy(gTopoDeviceCache[i].name, name, 31);
      gTopoDeviceCache[i].name[31] = '\0';
      gTopoDeviceCache[i].active = true;
      DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_CACHE] Added device: %s = %s", MAC_STR(mac), name);
      return;
    }
  }
  
  DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_CACHE] Cache full, cannot add %s", name);
}

// Helper: Get device name from topology cache
static String getTopoDeviceName(const uint8_t* mac) {
  if (!mac) return "";
  
  for (int i = 0; i < MAX_TOPO_DEVICE_CACHE; i++) {
    if (gTopoDeviceCache[i].active && memcmp(gTopoDeviceCache[i].mac, mac, 6) == 0) {
      return String(gTopoDeviceCache[i].name);
    }
  }
  return "";
}

// Helper: Buffer a PEER message for later processing
static bool bufferPeerMessage(const String& message, uint32_t reqId, const uint8_t* masterMac) {
  // Find empty slot
  for (int i = 0; i < MAX_BUFFERED_PEERS; i++) {
    if (!gPeerBuffer[i].active) {
      gPeerBuffer[i].message = message;
      gPeerBuffer[i].reqId = reqId;
      memcpy(gPeerBuffer[i].masterMac, masterMac, 6);
      gPeerBuffer[i].receivedMs = millis();
      gPeerBuffer[i].active = true;
      DEBUG_ESPNOWF("[PEER_BUFFER] Buffered PEER for reqId=%lu, master=%s (slot %d)",
                    (unsigned long)reqId, MAC_STR(masterMac), i);
      return true;
    }
  }
  WARN_ESPNOWF("Peer buffer full, dropping PEER for reqId=%lu", (unsigned long)reqId);
  return false;
}

// processBufferedPeersForStream removed - unused

// Helper: Forward a topology PEER message using the stream's stored path
// Returns true if forwarded successfully, false otherwise
static bool forwardTopologyPeer(const String& message, TopologyStream* stream) {
  if (!stream || stream->path.length() == 0) {
    ERROR_ESPNOWF("No stream or no path, cannot forward");
    return false;
  }
  
  // Parse incoming message
  PSRAM_JSON_DOC(doc);
  DeserializationError error = deserializeJson(doc, message);
  if (error) {
    WARN_ESPNOWF("JSON parse error in forward");
    return false;
  }
  
  // Check TTL
  int ttl = doc["ttl"] | 3;
  if (ttl <= 0) {
    DEBUG_ESPNOWF("[PEER_FWD] TTL exhausted, dropping PEER");
    return false;
  }
  
  // Get my MAC
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String myMacStr = macToHexString(myMac);
  
  // Split path into array
  std::vector<String> pathVec;
  int start = 0;
  int end = -1;
  while ((end = stream->path.indexOf(',', start)) != -1) {
    pathVec.push_back(stream->path.substring(start, end));
    start = end + 1;
  }
  pathVec.push_back(stream->path.substring(start));
  
  // Find my position in path
  int myIdx = -1;
  for (size_t j = 0; j < pathVec.size(); j++) {
    if (pathVec[j] == myMacStr) {
      myIdx = j;
      break;
    }
  }
  
  DEBUG_ESPNOWF("[PEER_FWD] Path: '%s', myMac: '%s', myIdx: %d", 
               stream->path.c_str(), myMacStr.c_str(), myIdx);
  
  if (myIdx <= 0) {
    if (myIdx == 0) {
      DEBUG_ESPNOWF("[PEER_FWD] I am master, should process locally");
    } else {
      DEBUG_ESPNOWF("[PEER_FWD] My MAC not found in path, cannot forward");
    }
    return false;
  }
  
  // Get previous hop
  String prevHopStr = pathVec[myIdx - 1];
  uint8_t prevHop[6];
  macFromHexString(prevHopStr.c_str(), prevHop);
  
  // Decrement TTL and forward
  doc["ttl"] = ttl - 1;
  String forwarded;
  serializeJson(doc, forwarded);
  
  DEBUG_ESPNOWF("[PEER_FWD] Forwarding to previous hop: %s (ttl=%d->%d)", 
               prevHopStr.c_str(), ttl, ttl - 1);
  
  esp_err_t result = esp_now_send(prevHop, (uint8_t*)forwarded.c_str(), forwarded.length());
  DEBUG_ESPNOWF("[PEER_FWD] Forward result: %s", result == ESP_OK ? "OK" : "FAILED");
  
  return (result == ESP_OK);
}

// processBufferedPeersForStream removed - unused

// Helper: Cleanup expired buffered PEERs (call periodically)
void cleanupExpiredBufferedPeers() {
  unsigned long now = millis();
  int cleanedCount = 0;
  
  for (int i = 0; i < MAX_BUFFERED_PEERS; i++) {
    if (gPeerBuffer[i].active && (now - gPeerBuffer[i].receivedMs > 10000)) {
      DEBUG_ESPNOWF("[PEER_BUFFER] Timeout: Discarding buffered PEER from slot %d (reqId=%lu, age=%lums)",
                    i, (unsigned long)gPeerBuffer[i].reqId, now - gPeerBuffer[i].receivedMs);
      gPeerBuffer[i].active = false;
      gPeerBuffer[i].message = "";
      cleanedCount++;
    }
  }
  
  if (cleanedCount > 0) {
    DEBUG_ESPNOWF("[PEER_BUFFER] Cleaned up %d expired buffer(s)", cleanedCount);
  }
}

// ============================================================================
// ESP-NOW COMMAND FUNCTIONS (Migrated from .ino file)
// ============================================================================
// These functions were moved from HardwareOne.ino to fix linker errors
// caused by the Arduino IDE not properly exporting symbols from large .ino files.
//
// Migration date: 2024
// Reason: Arduino IDE preprocessor limitation with files >1MB
// ============================================================================

// Helper: Get mesh role as string
const char* getMeshRoleString(uint8_t role) {
  switch (role) {
    case MESH_ROLE_MASTER: return "master";
    case MESH_ROLE_BACKUP_MASTER: return "backup";
    case MESH_ROLE_WORKER:
    default: return "worker";
  }
}

// Helper: Format MAC address into caller-provided buffer (no heap allocation)
// Buffer must be at least 18 bytes (17 chars + null terminator)
void formatMacAddressBuf(const uint8_t* mac, char* buf, size_t bufSize) {
  if (bufSize < 18) {
    if (bufSize > 0) buf[0] = '\0';
    return;
  }
  snprintf(buf, bufSize, "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Helper: Format MAC address as string (convenience wrapper, allocates String)
String formatMacAddress(const uint8_t* mac) {
  char buf[18];
  formatMacAddressBuf(mac, buf, sizeof(buf));
  return String(buf);
}

// Helper: Parse MAC address from string (flexible format)
bool parseMacAddress(const String& macStr, uint8_t mac[6]) {
  String cleanMac = macStr;
  cleanMac.toUpperCase();

  // Handle different separators
  cleanMac.replace("-", ":");
  cleanMac.replace(" ", ":");

  // Split by colons and parse each byte
  int byteIndex = 0;
  int startPos = 0;

  for (int i = 0; i <= cleanMac.length() && byteIndex < 6; i++) {
    if (i == cleanMac.length() || cleanMac[i] == ':') {
      if (byteIndex >= 6) return false;

      String byteStr = cleanMac.substring(startPos, i);
      byteStr.trim();

      if (byteStr.length() == 0 || byteStr.length() > 2) return false;

      char* endPtr;
      long val = strtol(byteStr.c_str(), &endPtr, 16);
      if (*endPtr != '\0' || val < 0 || val > 255) return false;

      mac[byteIndex] = (uint8_t)val;
      byteIndex++;
      startPos = i + 1;
    }
  }

  return (byteIndex == 6);
}

// Helper: Resolve device name or MAC address to MAC bytes
// Note: Not static - used by System_ImageManager for imagesend command
bool resolveDeviceNameOrMac(const String& nameOrMac, uint8_t mac[6]) {
  if (!gEspNow) return false;
  
  // First try to find by device name (case-insensitive)
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (gEspNow->devices[i].name.equalsIgnoreCase(nameOrMac)) {
      memcpy(mac, gEspNow->devices[i].mac, 6);
      return true;
    }
  }
  
  // If not found by name, try to parse as MAC address
  if (parseMacAddress(nameOrMac, mac)) {
    // Verify the MAC is in the paired device list
    for (int i = 0; i < gEspNow->deviceCount; i++) {
      if (memcmp(gEspNow->devices[i].mac, mac, 6) == 0) {
        return true;
      }
    }
  }
  
  return false;  // Not found by name or MAC, or not paired
}

// Helper: Add ESP-NOW device to registry
static void addEspNowDevice(const uint8_t* mac, const String& name, bool encrypted, const uint8_t* key) {
  if (!gEspNow || gEspNow->deviceCount >= 16) return;
  
  // Check if device already exists
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (memcmp(gEspNow->devices[i].mac, mac, 6) == 0) {
      // Update existing device
      gEspNow->devices[i].name = name;
      gEspNow->devices[i].encrypted = encrypted;
      if (encrypted && key) {
        memcpy(gEspNow->devices[i].key, key, 16);
      }
      return;
    }
  }
  
  // Add new device
  memcpy(gEspNow->devices[gEspNow->deviceCount].mac, mac, 6);
  gEspNow->devices[gEspNow->deviceCount].name = name;
  gEspNow->devices[gEspNow->deviceCount].encrypted = encrypted;
  if (encrypted && key) {
    memcpy(gEspNow->devices[gEspNow->deviceCount].key, key, 16);
  } else {
    memset(gEspNow->devices[gEspNow->deviceCount].key, 0, 16);
  }
  gEspNow->deviceCount++;
}

// Helper: Remove device from unpaired list
static void removeFromUnpairedList(const uint8_t* mac) {
  if (!gEspNow) return;
  for (int i = 0; i < gEspNow->unpairedDeviceCount; i++) {
    if (memcmp(gEspNow->unpairedDevices[i].mac, mac, 6) == 0) {
      // Shift remaining devices down
      for (int j = i; j < gEspNow->unpairedDeviceCount - 1; j++) {
        gEspNow->unpairedDevices[j] = gEspNow->unpairedDevices[j + 1];
      }
      gEspNow->unpairedDeviceCount--;
      return;
    }
  }
}

// Helper: Check if device is paired
static bool isPairedDevice(const uint8_t* mac) {
  if (!gEspNow) return false;
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (memcmp(gEspNow->devices[i].mac, mac, 6) == 0) {
      return true;
    }
  }
  return false;
}

// Helper: Check if ESP-NOW peer exists
static bool espnowPeerExists(const uint8_t* mac) {
  esp_now_peer_info_t peer;
  return esp_now_get_peer(mac, &peer) == ESP_OK;
}

// Helper: Request topology discovery (stub - implemented in mesh system)
 

// Helper: Cleanup stale topology streams (call periodically)
static void cleanupStaleTopoStreams() {
  TopoStreamsGuard guard("cleanupStaleTopoStreams");
  unsigned long now = millis();
  for (int i = 0; i < MAX_CONCURRENT_TOPO_STREAMS; i++) {
    if (gTopoStreams[i].active && (now - gTopoStreams[i].startTime > 10000)) {
      DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO] Timeout: Cleaning up stale stream from %s (reqId=%lu)",
                    gTopoStreams[i].senderName, (unsigned long)gTopoStreams[i].reqId);
      gTopoStreams[i].active = false;
    }
  }
}

// Helper: Clean up timed-out chunk buffers
void cleanupTimedOutChunks() {
  if (!gEspNow) return;
  
  unsigned long now = millis();
  const unsigned long timeout = 5000;  // 5 seconds
  
  for (int i = 0; i < 4; i++) {
    if (gEspNow->chunkBuffers[i].active) {
      if (now - gEspNow->chunkBuffers[i].lastChunkTime > timeout) {
        DEBUGF(DEBUG_ESPNOW_STREAM,
               "[Router] Chunk buffer %d timed out (msgId %lu, %lu/%lu chunks)",
               i,
               (unsigned long)gEspNow->chunkBuffers[i].msgId,
               (unsigned long)gEspNow->chunkBuffers[i].receivedChunks,
               (unsigned long)gEspNow->chunkBuffers[i].totalChunks);
        gEspNow->routerMetrics.chunksTimedOut++;
        gEspNow->chunkBuffers[i].reset();
      }
    }
  }
}

// ============================================================================
// ESP-NOW COMMAND FUNCTIONS
// ============================================================================
// All ESP-NOW command functions are implemented here in espnow_system.cpp
// Settings struct is now in settings.h for shared access
// ============================================================================

// Helper: Check ESP-NOW first-time setup
const char* checkEspNowFirstTimeSetup() {
  if (gSettings.espnowDeviceName.length() > 0) {
    if (!gSettings.espnowFirstTimeSetup) {
      setSetting(gSettings.espnowFirstTimeSetup, true);
    }
    return "";
  }
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  snprintf(getDebugBuffer(), 1024,
    "\n"
    "╔════════════════════════════════════════════════════════════╗\n"
    "║          ESP-NOW First-Time Setup Required                ║\n"
    "╚════════════════════════════════════════════════════════════╝\n"
    "\n"
    "Before initializing ESP-NOW, you must set a device name.\n"
    "This name will identify this device in topology displays.\n"
    "\n"
    "To set the device name, use:\n"
    "  espnow setname <name>\n"
    "\n"
    "Example:\n"
    "  espnow setname darkblue\n"
    "\n"
    "Requirements:\n"
    "  - 1-20 characters\n"
    "  - Letters, numbers, hyphens, underscores only\n"
    "  - No spaces\n"
    "\n"
    "After setting the name, run 'openespnow' again.\n"
  );
  
  return getDebugBuffer();
}

// Load named ESP-NOW devices (paired devices with names/keys) from filesystem
static void loadEspNowDevices() {
  if (!gEspNow) return;
  if (!LittleFS.exists(ESPNOW_DEVICES_FILE)) {
    DEBUGF(DEBUG_ESPNOW_MESH, "[ESPNOW] No saved devices file at %s", ESPNOW_DEVICES_FILE);
    return;
  }
  File f = LittleFS.open(ESPNOW_DEVICES_FILE, "r");
  if (!f) {
    WARN_ESPNOWF("[ESPNOW] Failed to open %s for reading", ESPNOW_DEVICES_FILE);
    return;
  }
  String content = f.readString();
  f.close();
  if (content.isEmpty()) return;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, content);
  if (err) {
    WARN_ESPNOWF("[ESPNOW] Failed to parse %s: %s", ESPNOW_DEVICES_FILE, err.c_str());
    return;
  }

  JsonArray arr = doc["devices"].as<JsonArray>();
  if (!arr) return;

  int count = 0;
  for (JsonObject entry : arr) {
    if (gEspNow->deviceCount >= 16) break;
    const char* macStr = entry["mac"] | "";
    const char* name   = entry["name"] | "";
    if (!macStr[0]) continue;

    uint8_t mac[6];
    if (!parseMacAddress(String(macStr), mac)) continue;

    // Check if this MAC is already loaded (prevents duplicates from corrupted JSON)
    bool alreadyLoaded = false;
    for (int i = 0; i < gEspNow->deviceCount; i++) {
      if (memcmp(gEspNow->devices[i].mac, mac, 6) == 0) {
        alreadyLoaded = true;
        WARN_ESPNOWF("[ESPNOW] Skipping duplicate device in saved file: %s (%s)", name, macStr);
        break;
      }
    }
    if (alreadyLoaded) continue;

    EspNowDevice& dev = gEspNow->devices[gEspNow->deviceCount];
    memcpy(dev.mac, mac, 6);
    dev.name      = String(name);
    dev.encrypted = entry["encrypted"] | false;
    memset(dev.key, 0, 16);
    const char* keyHex = entry["key"] | "";
    if (dev.encrypted && strlen(keyHex) == 32) {
      for (int i = 0; i < 16; i++) {
        char byte[3] = { keyHex[i*2], keyHex[i*2+1], '\0' };
        dev.key[i] = (uint8_t)strtol(byte, nullptr, 16);
      }
    }
    gEspNow->deviceCount++;
    count++;
  }
  DEBUGF(DEBUG_ESPNOW_MESH, "[ESPNOW] Loaded %d device(s) from %s", count, ESPNOW_DEVICES_FILE);
}

// Dequeue a mesh retry entry by msgId (called when ACK is received)
static void meshRetryDequeue(uint32_t msgId) {
  if (!gMeshRetryMutex) return;
  if (xSemaphoreTake(gMeshRetryMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
  for (int i = 0; i < MESH_RETRY_QUEUE_SIZE; i++) {
    if (gMeshRetryQueue[i].active && gMeshRetryQueue[i].msgId == msgId) {
      gMeshRetryQueue[i].active = false;
      DEBUGF(DEBUG_ESPNOW_MESH, "[MESH_RETRY] Dequeued msgId=%lu on ACK", (unsigned long)msgId);
      break;
    }
  }
  xSemaphoreGive(gMeshRetryMutex);
}

// ============================================================================
// RESTORED PUBLIC HELPER FUNCTIONS (removed during V2 cleanup, rebuilt)
// ============================================================================

// Convert 6-byte MAC to colon-separated hex string ("AA:BB:CC:DD:EE:FF")
String macToHexString(const uint8_t mac[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// Parse MAC string ("AA:BB:CC:DD:EE:FF") into byte array (fills zeros on parse failure)
void macFromHexString(const String& s, uint8_t out[6]) {
  unsigned int b[6] = {0, 0, 0, 0, 0, 0};
  sscanf(s.c_str(), "%02x:%02x:%02x:%02x:%02x:%02x",
         &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
  for (int i = 0; i < 6; i++) out[i] = (uint8_t)b[i];
}

// Find (or optionally create) a MeshPeerHealth slot for a given MAC
MeshPeerHealth* getMeshPeerHealth(const uint8_t mac[6], bool createIfMissing) {
  if (!gMeshPeers) return nullptr;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeers[i].isActive && memcmp(gMeshPeers[i].mac, mac, 6) == 0)
      return &gMeshPeers[i];
  }
  if (!createIfMissing) return nullptr;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (!gMeshPeers[i].isActive) {
      memset(&gMeshPeers[i], 0, sizeof(MeshPeerHealth));
      memcpy(gMeshPeers[i].mac, mac, 6);
      gMeshPeers[i].isActive = true;
      return &gMeshPeers[i];
    }
  }
  return nullptr;
}

// Check if a mesh peer is considered alive (heartbeat within timeout window)
bool isMeshPeerAlive(const MeshPeerHealth* peer) {
  if (!peer || !peer->isActive) return false;
  if (peer->lastHeartbeatMs == 0) return false;
  return (millis() - peer->lastHeartbeatMs) < MESH_PEER_TIMEOUT_MS;
}

// Get device display name for a MAC (from runtime meta, then paired registry)
String getEspNowDeviceName(const uint8_t* mac) {
  if (gMeshPeerMeta) {
    for (int i = 0; i < gMeshPeerSlots; i++) {
      if (gMeshPeerMeta[i].isActive && memcmp(gMeshPeerMeta[i].mac, mac, 6) == 0) {
        if (gMeshPeerMeta[i].name[0]) return String(gMeshPeerMeta[i].name);
        break;
      }
    }
  }
  if (gEspNow) {
    for (int i = 0; i < gEspNow->deviceCount; i++) {
      if (memcmp(gEspNow->devices[i].mac, mac, 6) == 0)
        return gEspNow->devices[i].name;
    }
  }
  return "";
}

// Remove a device from the paired device registry by MAC
void removeEspNowDevice(const uint8_t* mac) {
  if (!gEspNow) return;
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (memcmp(gEspNow->devices[i].mac, mac, 6) == 0) {
      for (int j = i; j < gEspNow->deviceCount - 1; j++)
        gEspNow->devices[j] = gEspNow->devices[j + 1];
      gEspNow->deviceCount--;
      return;
    }
  }
}

// Initialize a JsonDocument as a V2-style JSON envelope with standard fields
void v2_init_envelope(JsonDocument& doc, const char* type, uint32_t msgId,
                      const char* src, const char* dst, int ttl) {
  doc["type"] = type;
  doc["id"]   = msgId;
  doc["src"]  = src;
  if (dst && dst[0]) doc["dst"] = dst;
  if (ttl >= 0)      doc["ttl"] = ttl;
}

// Send a serialized JSON string to all active mesh peers via V3 TEXT frames
void meshSendEnvelopeToPeers(const String& envelope) {
  if (!gEspNow || !gEspNow->initialized || !gMeshPeers) return;
  const uint8_t* data = (const uint8_t*)envelope.c_str();
  uint16_t len = (uint16_t)envelope.length();
  uint32_t msgId = generateMessageId();
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac))
      v3_send_chunked(gMeshPeers[i].mac, ESPNOW_V3_TYPE_TEXT, 0, msgId, data, len, 3);
  }
}

// Build a V2-style JSON heartbeat string
String buildHeartbeat(uint32_t msgId, const char* src) {
  JsonDocument doc;
  v2_init_envelope(doc, MSG_TYPE_HB, msgId, src, "", 1);
  String out;
  serializeJson(doc, out);
  return out;
}

// Build a V2-style JSON boot notification string
String buildBootNotification(uint32_t msgId, const char* src,
                             uint32_t bootCounter, uint32_t timestamp) {
  JsonDocument doc;
  v2_init_envelope(doc, MSG_TYPE_BOOT, msgId, src, "", 3);
  JsonObject pld = doc["pld"].to<JsonObject>();
  pld["boot"] = bootCounter;
  pld["ts"]   = timestamp;
  String out;
  serializeJson(doc, out);
  return out;
}

// Send V3 topology discovery requests to all active peers
void requestTopologyDiscovery() {
  if (!gEspNow || !gEspNow->initialized || !gMeshPeers) return;
  gTopoRequestId        = generateMessageId();
  gTopoRequestTimeout   = millis() + 10000;
  gTopoLastResponseTime = 0;
  gTopoResponsesReceived = 0;
  gTopoResultsBuffer    = "";
  gLastTopoRequest      = millis();
  V3PayloadTopoReq req  = {};
  req.reqId = gTopoRequestId;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac))
      v3_send_frame(gMeshPeers[i].mac, ESPNOW_V3_TYPE_TOPO_REQ, 0,
                    generateMessageId(), (const uint8_t*)&req, sizeof(req), 3);
  }
  DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO] Discovery request sent (reqId=%lu)",
         (unsigned long)gTopoRequestId);
}

// Check if the topology collection window has expired; finalize any open streams
void checkTopologyCollectionWindow() {
  if (gTopoRequestId == 0) return;
  if ((uint32_t)millis() < gTopoRequestTimeout) return;
  for (int i = 0; i < MAX_CONCURRENT_TOPO_STREAMS; i++) {
    if (gTopoStreams[i].active)
      finalizeTopologyStream(&gTopoStreams[i]);
  }
  gTopoRequestId = 0;
}

// ============================================================================
// ESP-NOW HEARTBEAT TASK
// ============================================================================

static TaskHandle_t gEspNowHbTaskHandle = nullptr;

// Main heartbeat task body: drain RX ring, send periodic HB, process queues
void processMeshHeartbeats() {
  // 1. Drain the inbound RX ring buffer
  uint8_t ringSize = (uint8_t)(sizeof(gEspNowRxRing) / sizeof(gEspNowRxRing[0]));
  while (gEspNowRxHead != gEspNowRxTail) {
    uint8_t tail = gEspNowRxTail;
    InboundRxItem& item = gEspNowRxRing[tail];
    uint8_t dstMac[6] = {};
    wifi_pkt_rx_ctrl_t rxCtrl = {};
    rxCtrl.rssi = item.rssi;
    esp_now_recv_info_t ri = {};
    ri.src_addr = item.src;
    ri.des_addr = dstMac;
    ri.rx_ctrl = &rxCtrl;
    onEspNowRawRecv(&ri, item.data, (int)item.len);
    gEspNowRxTail = (uint8_t)((tail + 1) % ringSize);
  }

  if (!gEspNow || !gEspNow->initialized || gMeshActivitySuspended) return;

  // 2. Send periodic V3 mesh heartbeat (only if we have active peers)
  static const uint32_t HB_INTERVAL_MS = 5000;
  uint32_t now = (uint32_t)millis();
  if (now - gLastHeartbeatSentMs >= HB_INTERVAL_MS) {
    gLastHeartbeatSentMs = now;
    // Count active peers first — skip heartbeat entirely if nobody to send to
    uint8_t activePeerCount = 0;
    for (int i = 0; i < gMeshPeerSlots; i++) {
      if (gMeshPeers && gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac))
        activePeerCount++;
    }
    if (activePeerCount > 0) {
      V3PayloadHeartbeat hb = {};
      hb.role = gSettings.meshRole;
      hb.peerCount = activePeerCount;
      wifi_ap_record_t ap = {};
      hb.rssi      = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : (int8_t)-127;
      hb.uptimeSec = now / 1000;
      hb.freeHeap  = (uint32_t)ESP.getFreeHeap();
      strncpy(hb.deviceName, gSettings.espnowDeviceName.c_str(), sizeof(hb.deviceName) - 1);
      v3_broadcast(ESPNOW_V3_TYPE_HEARTBEAT, ESPNOW_V3_FLAG_ACK_REQ, generateMessageId(),
                   (const uint8_t*)&hb, (uint16_t)sizeof(hb), 1);
      gEspNow->heartbeatsSent++;
    }
  }

  // 3a. Master: send dedicated unicast heartbeat to backup device
  if (meshEnabled() && gSettings.meshRole == MESH_ROLE_MASTER &&
      gSettings.meshBackupEnabled && gSettings.meshBackupMAC.length() > 0) {
    if (now - gLastBackupHeartbeat >= gSettings.meshMasterHeartbeatInterval) {
      gLastBackupHeartbeat = now;
      uint8_t backupMac[6] = {};
      if (parseMacAddress(gSettings.meshBackupMAC, backupMac)) {
        V3PayloadHeartbeat hb = {};
        hb.role      = MESH_ROLE_MASTER;
        hb.uptimeSec = now / 1000;
        hb.freeHeap  = (uint32_t)ESP.getFreeHeap();
        wifi_ap_record_t ap = {};
        hb.rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : (int8_t)-127;
        strncpy(hb.deviceName, gSettings.espnowDeviceName.c_str(), sizeof(hb.deviceName) - 1);
        v3_send_frame(backupMac, ESPNOW_V3_TYPE_HEARTBEAT, ESPNOW_V3_FLAG_ACK_REQ,
                      generateMessageId(), (const uint8_t*)&hb, (uint16_t)sizeof(hb), 1);
      }
    }
  }

  // 3b. Backup master: promote self if master silent for failoverTimeout
  if (meshEnabled() && gSettings.meshRole == MESH_ROLE_BACKUP_MASTER &&
      gSettings.meshBackupEnabled && !gBackupPromoted &&
      gLastMasterHeartbeat > 0 &&
      (now - gLastMasterHeartbeat) >= gSettings.meshFailoverTimeout) {
    gBackupPromoted = true;
    gSettings.meshRole = MESH_ROLE_MASTER;  // Runtime only — not persisted, reboot restores backup role
    BROADCAST_PRINTF("[BACKUP] Master silent for %lums — promoted to master",
                     (unsigned long)gSettings.meshFailoverTimeout);
  }

#if ENABLE_BONDED_MODE
  // 3. Send periodic paired-mode heartbeat if bonded
  if (gSettings.bondModeEnabled && gSettings.bondPeerMac.length() > 0) {
    if (now - gLastBondHeartbeatSentMs >= BOND_HEARTBEAT_INTERVAL_MS) {
      gLastBondHeartbeatSentMs = now;
      uint8_t bondMac[6] = {};
      if (parseMacAddress(gSettings.bondPeerMac, bondMac)) {
        V3PayloadBondHeartbeat phb = {};
        phb.role      = gSettings.bondRole;
        phb.uptimeSec = now / 1000;
        phb.freeHeap  = (uint32_t)ESP.getFreeHeap();
        phb.seqNum    = ++gBondHeartbeatSeqNum;
        extern uint32_t gBootCounter;
        phb.bootCounter = gBootCounter;
        phb.settingsHash = gEspNow ? gEspNow->bondLocalSettingsHash : 0;
        wifi_ap_record_t ap2 = {};
        phb.rssi = (esp_wifi_sta_get_ap_info(&ap2) == ESP_OK) ? ap2.rssi : (int8_t)-127;
        bool sent = v3_send_frame(bondMac, ESPNOW_V3_TYPE_BOND_HEARTBEAT, 0,
                      generateMessageId(), (const uint8_t*)&phb, (uint16_t)sizeof(phb), 1);
        gEspNow->bondHeartbeatsSent++;
        // Log every 6th heartbeat (every 30s) or first one, plus full state
        if (gBondHeartbeatSeqNum <= 2 || gBondHeartbeatSeqNum % 6 == 0) {
          BROADCAST_PRINTF("[BOND_HB_TX] seq=%lu sent=%d to=%s role=%d synced=%d peerOnline=%d hbRx=%lu",
                           (unsigned long)gBondHeartbeatSeqNum, (int)sent,
                           gSettings.bondPeerMac.c_str(), (int)gSettings.bondRole,
                           (int)isBondSynced(), (int)gEspNow->bondPeerOnline,
                           (unsigned long)gEspNow->bondHeartbeatsReceived);
        }
      } else {
        BROADCAST_PRINTF("[BOND_HB_TX] ERROR: parseMacAddress failed for '%s'", gSettings.bondPeerMac.c_str());
      }
    }
  } else if (gSettings.bondModeEnabled && gSettings.bondPeerMac.length() == 0) {
    // Log once that bond mode is enabled but no peer MAC
    static bool sLoggedNoPeerMac = false;
    if (!sLoggedNoPeerMac) {
      BROADCAST_PRINTF("[BOND] WARNING: bondModeEnabled=true but bondPeerMac is empty!");
      sLoggedNoPeerMac = true;
    }
  }

  if (gSettings.bondModeEnabled && gEspNow && gEspNow->bondPeerOnline &&
      gEspNow->lastBondHeartbeatReceivedMs > 0 &&
      (now - (uint32_t)gEspNow->lastBondHeartbeatReceivedMs) >= BOND_HEARTBEAT_TIMEOUT_MS) {
    gEspNow->bondPeerOnline = false;
    gEspNow->bondLastOfflineMs = now;
    resetBondSync();
  }
#endif // ENABLE_BONDED_MODE

  // 3c. Mark stale mesh peers offline (mirrors bond's bondPeerOnline = false pattern)
  if (gMeshPeers) {
    for (int i = 0; i < gMeshPeerSlots; i++) {
      if (gMeshPeers[i].isActive && !isMeshPeerAlive(&gMeshPeers[i])) {
        gMeshPeers[i].isActive = false;
      }
    }
  }

  // 4. Process queued messages and retry queue
  processMessageQueue();

  // 5. Check topology collection window
  checkTopologyCollectionWindow();
  
  // 6. Check broadcast tracker timeouts
  static uint32_t sLastBroadcastCheck = 0;
  if (now - sLastBroadcastCheck >= 500) {  // Check every 500ms
    sLastBroadcastCheck = now;
    broadcast_tracker_check_timeouts();
  }
  
  // 6b. Clean up abandoned file transfers (no FILE_END received within 30s)
  if (gActiveFileTransfer && gActiveFileTransfer->active &&
      (now - gActiveFileTransfer->startTime) > 30000) {
    BROADCAST_PRINTF("[V3_FILE] Abandoned transfer '%s' after 30s (%u/%u chunks)",
                     gActiveFileTransfer->filename,
                     (unsigned)gActiveFileTransfer->receivedChunks,
                     (unsigned)gActiveFileTransfer->totalChunks);
    if (gActiveFileTransfer->chunkMap) heap_caps_free(gActiveFileTransfer->chunkMap);
    if (gActiveFileTransfer->dataBuffer) heap_caps_free(gActiveFileTransfer->dataBuffer);
    delete gActiveFileTransfer;
    gActiveFileTransfer = nullptr;
  }
  
  // 7. Process deferred CMD (remote command received from another device)
  if (gEspNow->deferredCmdPending) {
    gEspNow->deferredCmdPending = false;
    v3_handle_cmd(gEspNow->deferredCmdSrcMac, gEspNow->deferredCmdDeviceName,
                  gEspNow->deferredCmdMsgId, gEspNow->deferredCmdPayload);
  }
  
  // 7b. Drain stream queue (remote command output received via V3 STREAM frames)
  {
    int tail = gEspNow->streamQueueTail;
    int processed = 0;
    while (tail != gEspNow->streamQueueHead && processed < 8) {
      auto& entry = gEspNow->streamQueue[tail];
      if (entry.used) {
        String devName = String(entry.deviceName);
        if (devName.length() == 0) devName = formatMacAddress(entry.srcMac);
        storeMessageInPeerHistory(entry.srcMac,
                                  devName.c_str(),
                                  entry.content,
                                  true,
                                  MSG_TEXT);
        BROADCAST_PRINTF("[STREAM:%s] %s", devName.c_str(), entry.content);
        entry.used = false;
      }
      tail = (tail + 1) & (EspNowState::STREAM_QUEUE_SIZE - 1);
      processed++;
    }
    gEspNow->streamQueueTail = tail;
  }
  
  // 8. Process deferred CMD_RESP (response to our remote command)
  if (gEspNow->deferredCmdRespPending) {
    gEspNow->deferredCmdRespPending = false;
    String deviceName = String(gEspNow->deferredCmdRespDeviceName);
    if (deviceName.length() == 0) deviceName = formatMacAddress(gEspNow->deferredCmdRespSrcMac);
    storeMessageInPeerHistory(gEspNow->deferredCmdRespSrcMac,
                              deviceName.c_str(),
                              gEspNow->deferredCmdRespResult,
                              true,
                              MSG_TEXT);
    
    if (gEspNow->deferredCmdRespSuccess) {
      BROADCAST_PRINTF("[ESP-NOW] Command result from %s: %s", deviceName.c_str(), gEspNow->deferredCmdRespResult);
    } else {
      BROADCAST_PRINTF("[ESP-NOW] Command FAILED from %s: %s", deviceName.c_str(), gEspNow->deferredCmdRespResult);
    }
  }
  
#if ENABLE_BONDED_MODE
  // =========================================================================
  // BOND SYNC TICK (Option B) — master-driven, idempotent "fetch what's missing"
  // Replaces the old 9a/9c/retry linear handshake logic.
  // Both roles: respond to deferred request flags (9b, 9d, 9e).
  // Master only: drive CAP → MANIFEST → SETTINGS fetch sequence.
  // =========================================================================
  
  // --- Master sync tick: decide what to request next ---
  if (gSettings.bondRole == 1 && gEspNow->bondPeerOnline && gSettings.bondModeEnabled) {
    uint8_t peerMac[6];
    bool macOk = (gSettings.bondPeerMac.length() > 0 && parseMacAddress(gSettings.bondPeerMac, peerMac));
    
    // Consume the "peer came online" trigger — just marks that we should start syncing
    if (gEspNow->bondNeedsCapabilityRequest) {
      gEspNow->bondNeedsCapabilityRequest = false;
      BROADCAST_PRINTF("[BOND_SYNC] Peer online trigger consumed, starting sync tick | bootCtr=%lu",
                       (unsigned long)gEspNow->bondPeerBootCounter);
    }
    
    // Consume received capability — just log, sync tick handles the rest
    if (gEspNow->bondReceivedCapability) {
      gEspNow->bondReceivedCapability = false;
      BROADCAST_PRINTF("[BOND_SYNC] CAP_RESP received | fwHash=%02X%02X%02X%02X featureMask=0x%08lX",
                       gEspNow->lastRemoteCap.fwHash[0], gEspNow->lastRemoteCap.fwHash[1],
                       gEspNow->lastRemoteCap.fwHash[2], gEspNow->lastRemoteCap.fwHash[3],
                       (unsigned long)gEspNow->lastRemoteCap.featureMask);
    }
    
    // Cooldown after retry exhaustion — don't re-request immediately
    static const uint32_t BOND_SYNC_COOLDOWN_MS = 15000;
    bool inCooldown = (gEspNow->bondSyncLastAttemptMs > 0 &&
                       (now - gEspNow->bondSyncLastAttemptMs) < BOND_SYNC_COOLDOWN_MS);
    
    if (macOk && gEspNow->bondSyncInFlight == BOND_SYNC_NONE && !inCooldown) {
      // Decide what's missing and request it (priority order: CAP > MANIFEST > SETTINGS)
      bool haveCap = gEspNow->lastRemoteCapValid;
      bool haveManifest = gEspNow->bondManifestReceived;
      bool haveSettings = gEspNow->bondSettingsReceived;
      
      if (!haveCap) {
        // Need capabilities
        uint32_t reqId = generateMessageId();
        v3_send_frame(peerMac, ESPNOW_V3_TYPE_BOND_CAP_REQ, ESPNOW_V3_FLAG_ACK_REQ, reqId, nullptr, 0, 1);
        gEspNow->bondSyncInFlight = BOND_SYNC_CAP;
        gEspNow->bondSyncLastAttemptMs = now;
        gEspNow->bondSyncRetryCount = 1;
        BROADCAST_PRINTF("[BOND_SYNC] Requesting CAP (msgId=%lu)", (unsigned long)reqId);
      } else if (!haveManifest) {
        // Have cap, need manifest — check cache first
        bool haveCached = false;
        String cached = loadManifestFromCache(gEspNow->lastRemoteCap.fwHash);
        haveCached = (cached.length() > 0);
        if (haveCached) {
          gEspNow->bondManifestReceived = true;
          BROADCAST_PRINTF("[BOND_SYNC] Manifest found in cache, skipping request");
        } else {
          uint32_t msgId = generateMessageId();
          v3_send_frame(peerMac, ESPNOW_V3_TYPE_MANIFEST_REQ, ESPNOW_V3_FLAG_ACK_REQ, msgId, nullptr, 0, 1);
          gEspNow->bondSyncInFlight = BOND_SYNC_MANIFEST;
          gEspNow->bondSyncLastAttemptMs = now;
          gEspNow->bondSyncRetryCount = 1;
          BROADCAST_PRINTF("[BOND_SYNC] Requesting MANIFEST (msgId=%lu)", (unsigned long)msgId);
        }
      } else if (!haveSettings) {
        // Have cap + manifest, need settings
        requestBondSettings(peerMac);
        gEspNow->bondSyncRetryCount = 1;
        BROADCAST_PRINTF("[BOND_SYNC] Requesting SETTINGS");
      }
      // else: all synced — handshake complete is handled in processBondSettings
    }
    
    // Retry logic for in-flight requests
    if (macOk && gEspNow->bondSyncInFlight != BOND_SYNC_NONE &&
        gEspNow->bondSyncLastAttemptMs > 0 &&
        (now - gEspNow->bondSyncLastAttemptMs >= BOND_SYNC_RETRY_MS)) {
      if (gEspNow->bondSyncRetryCount < 3) {
        gEspNow->bondSyncLastAttemptMs = now;
        gEspNow->bondSyncRetryCount++;
        if (gEspNow->bondSyncInFlight == BOND_SYNC_CAP) {
          v3_send_frame(peerMac, ESPNOW_V3_TYPE_BOND_CAP_REQ, ESPNOW_V3_FLAG_ACK_REQ, generateMessageId(), nullptr, 0, 1);
          BROADCAST_PRINTF("[BOND_SYNC] Retry CAP_REQ (%d/3)", (int)gEspNow->bondSyncRetryCount);
        } else if (gEspNow->bondSyncInFlight == BOND_SYNC_MANIFEST) {
          v3_send_frame(peerMac, ESPNOW_V3_TYPE_MANIFEST_REQ, ESPNOW_V3_FLAG_ACK_REQ, generateMessageId(), nullptr, 0, 1);
          BROADCAST_PRINTF("[BOND_SYNC] Retry MANIFEST_REQ (%d/3)", (int)gEspNow->bondSyncRetryCount);
        } else if (gEspNow->bondSyncInFlight == BOND_SYNC_SETTINGS) {
          requestBondSettings(peerMac);
          BROADCAST_PRINTF("[BOND_SYNC] Retry SETTINGS_REQ (%d/3)", (int)gEspNow->bondSyncRetryCount);
        }
      } else {
        // Max retries exhausted — reset in-flight but enforce cooldown before re-requesting.
        // Leave bondSyncLastAttemptMs set so the cooldown check below prevents instant retry.
        BROADCAST_PRINTF("[BOND_SYNC] %d exhausted %d retries, cooldown 15s before re-request",
                         (int)gEspNow->bondSyncInFlight, (int)gEspNow->bondSyncRetryCount);
        gEspNow->bondSyncInFlight = BOND_SYNC_NONE;
        gEspNow->bondSyncRetryCount = 0;
        gEspNow->bondSyncLastAttemptMs = now;  // Start cooldown period
      }
    }
  } else {
    // Worker: just consume stale flags silently (master drives everything)
    if (gEspNow->bondNeedsCapabilityRequest) {
      gEspNow->bondNeedsCapabilityRequest = false;
    }
    if (gEspNow->bondReceivedCapability) {
      gEspNow->bondReceivedCapability = false;
    }
  }

  // 9b. Bond: peer requested our capabilities — send capability response (both roles respond)
  if (gEspNow->bondNeedsCapabilityResponse) {
    gEspNow->bondNeedsCapabilityResponse = false;
    BROADCAST_PRINTF("[BOND] 9b: CAP_RESP sending | role=%d dest=%s",
                     (int)gSettings.bondRole,
                     MAC_STR(gEspNow->bondPendingResponseMac));
    CapabilitySummary cap;
    buildCapabilitySummary(cap);
    uint32_t respId = generateMessageId();
    bool sent = v3_send_frame(gEspNow->bondPendingResponseMac, ESPNOW_V3_TYPE_BOND_CAP_RESP,
                  ESPNOW_V3_FLAG_ACK_REQ, respId,
                  (const uint8_t*)&cap, (uint16_t)sizeof(cap), 1);
    gEspNow->bondCapSent = true;
    BROADCAST_PRINTF("[BOND] 9b: CAP_RESP sent=%d featureMask=0x%08lX", (int)sent, (unsigned long)cap.featureMask);
  }

  // 9d. Bond: peer requested our manifest — generate and send via file transfer
  if (gEspNow->bondNeedsManifestResponse) {
    gEspNow->bondNeedsManifestResponse = false;
    BROADCAST_PRINTF("[BOND] 9d: bondNeedsManifestResponse consumed | role=%d dest=%s",
                     (int)gSettings.bondRole,
                     MAC_STR(gEspNow->bondPendingResponseMac));
    String manifest = generateDeviceManifest();
    BROADCAST_PRINTF("[BOND] 9d: manifest generated len=%d", manifest.length());
    String tempPath = "/system/_manifest_out.json";
    {
      FsLockGuard guard("bond.manifest.send");
      extern bool filesystemReady;
      if (filesystemReady) {
        if (!LittleFS.exists("/system")) LittleFS.mkdir("/system");
        File f = LittleFS.open(tempPath.c_str(), "w");
        if (f) { f.print(manifest); f.close(); BROADCAST_PRINTF("[BOND] 9d: manifest written to %s", tempPath.c_str()); }
        else { BROADCAST_PRINTF("[BOND] 9d: ERROR failed to write manifest file"); }
      } else {
        BROADCAST_PRINTF("[BOND] 9d: ERROR filesystem not ready");
      }
    }
    uint8_t destMac[6];
    memcpy(destMac, gEspNow->bondPendingResponseMac, 6);
    extern bool sendFileToMac(const uint8_t* mac, const String& localPath);
    bool fileSent = sendFileToMac(destMac, tempPath);
    BROADCAST_PRINTF("[BOND] 9d: sendFileToMac result=%d", (int)fileSent);
    {
      FsLockGuard guard("bond.manifest.cleanup");
      LittleFS.remove(tempPath.c_str());
    }
    if (fileSent) {
      BROADCAST_PRINTF("[BOND] 9d: manifest sent to peer");
    }
  }

  // 9e. Bond: peer requested our settings — send settings via file transfer
  if (gEspNow->bondNeedsSettingsResponse) {
    gEspNow->bondNeedsSettingsResponse = false;
    BROADCAST_PRINTF("[BOND] 9e: bondNeedsSettingsResponse consumed | role=%d dest=%s",
                     (int)gSettings.bondRole,
                     MAC_STR(gEspNow->bondPendingResponseMac));
    uint8_t destMac[6];
    memcpy(destMac, gEspNow->bondPendingResponseMac, 6);
    sendBondSettings(destMac);
    gEspNow->bondSettingsSent = true;
    
    // Worker sync-complete: capSent + settingsSent + capValid → isBondSynced() now true
    if (gSettings.bondRole == 0 && isBondSynced() && !gEspNow->bondSessionTokenValid) {
      uint8_t pMac[6];
      if (parseMacAddress(gSettings.bondPeerMac, pMac)) {
        computeBondSessionToken(pMac);
      }
      BROADCAST_PRINTF("[BOND_SYNC] *** SYNC COMPLETE *** role=0 (worker)");
    }
  }

  // 9f. Bond: streaming setup — master pushes saved streaming prefs to worker after handshake
  if (gEspNow->bondNeedsStreamingSetup) {
    gEspNow->bondNeedsStreamingSetup = false;
    BROADCAST_PRINTF("[BOND] 9f: bondNeedsStreamingSetup consumed | role=%d",
                     (int)gSettings.bondRole);
    
    // Only master pushes streaming prefs to worker (after full sync)
    if (gSettings.bondRole == 1 && isBondSynced()) {
      extern bool sendBondStreamCtrl(RemoteSensorType sensorType, bool enable);
      struct { const char* name; bool enabled; RemoteSensorType type; } streams[] = {
        { "thermal",  gSettings.bondStreamThermal,  REMOTE_SENSOR_THERMAL },
        { "tof",      gSettings.bondStreamTof,      REMOTE_SENSOR_TOF },
        { "imu",      gSettings.bondStreamImu,      REMOTE_SENSOR_IMU },
        { "gps",      gSettings.bondStreamGps,      REMOTE_SENSOR_GPS },
        { "gamepad",  gSettings.bondStreamGamepad,   REMOTE_SENSOR_GAMEPAD },
        { "fmradio",  gSettings.bondStreamFmradio,   REMOTE_SENSOR_FMRADIO },
        { "rtc",      gSettings.bondStreamRtc,       REMOTE_SENSOR_RTC },
        { "presence", gSettings.bondStreamPresence,  REMOTE_SENSOR_PRESENCE },
      };
      for (auto& s : streams) {
        if (s.enabled) {
          BROADCAST_PRINTF("[BOND] 9f: Sending STREAM_CTRL %s ON to worker", s.name);
          sendBondStreamCtrl(s.type, true);
          vTaskDelay(pdMS_TO_TICKS(20));  // Small gap between sends
        }
      }
    }
  }

  // 9f2. Bond: deferred STREAM_CTRL received — worker applies stream control from master
  if (gEspNow->bondDeferredStreamCtrlPending) {
    gEspNow->bondDeferredStreamCtrlPending = false;
    RemoteSensorType sType = (RemoteSensorType)gEspNow->bondDeferredStreamCtrlSensor;
    bool enable = (gEspNow->bondDeferredStreamCtrlEnable != 0);
    
    extern const char* sensorTypeToString(RemoteSensorType type);
    BROADCAST_PRINTF("[BOND] STREAM_CTRL: %s %s (from master)",
                     sensorTypeToString(sType), enable ? "ON" : "OFF");
    
    if (sType < REMOTE_SENSOR_MAX) {
      if (enable) {
        startSensorDataStreaming(sType);
      } else {
        stopSensorDataStreaming(sType);
      }
    }
  }

  // 9h. Bond: log any unpaired rejection events (deferred from ISR context)
  {
    static uint32_t sLastReportedRejectCount = 0;
    if (gEspNow->bondUnpairedRejectCount > sLastReportedRejectCount) {
      uint32_t newRejects = gEspNow->bondUnpairedRejectCount - sLastReportedRejectCount;
      sLastReportedRejectCount = gEspNow->bondUnpairedRejectCount;
      BROADCAST_PRINTF("[BOND] REJECTED %lu bond msg(s) from UNPAIRED %02X:%02X:%02X:%02X:%02X:%02X (type=%d, total=%lu) — run 'bond connect' or 'espnow pair'!",
                       (unsigned long)newRejects,
                       gEspNow->bondUnpairedRejectMac[0], gEspNow->bondUnpairedRejectMac[1],
                       gEspNow->bondUnpairedRejectMac[2], gEspNow->bondUnpairedRejectMac[3],
                       gEspNow->bondUnpairedRejectMac[4], gEspNow->bondUnpairedRejectMac[5],
                       (int)gEspNow->bondUnpairedRejectType,
                       (unsigned long)gEspNow->bondUnpairedRejectCount);
    }
  }

  // 9i. Bond: periodic state dump (every 30s when bond mode active)
  {
    static uint32_t sLastBondStateDump = 0;
    if (gSettings.bondModeEnabled && (now - sLastBondStateDump >= 30000)) {
      sLastBondStateDump = now;
      BROADCAST_PRINTF("[BOND_STATE] role=%d synced=%d peerOnline=%d hbTx=%lu hbRx=%lu rssi=%d rejects=%lu peer='%s' capValid=%d capSent=%d statusValid=%d connMask=0x%04X",
                       (int)gSettings.bondRole, (int)isBondSynced(),
                       (int)gEspNow->bondPeerOnline,
                       (unsigned long)gEspNow->bondHeartbeatsSent,
                       (unsigned long)gEspNow->bondHeartbeatsReceived,
                       (int)gEspNow->bondRssiLast,
                       (unsigned long)gEspNow->bondUnpairedRejectCount,
                       gSettings.bondPeerMac.c_str(),
                       (int)gEspNow->lastRemoteCapValid,
                       (int)gEspNow->bondCapSent,
                       (int)gEspNow->bondPeerStatusValid,
                       (unsigned)gEspNow->bondPeerStatus.sensorConnectedMask);
    }
  }

  // 9j. Bond: deferred status response — peer requested our live status
  if (gEspNow->bondNeedsStatusResponse) {
    gEspNow->bondNeedsStatusResponse = false;
    BondPeerStatus localStatus;
    buildLocalBondStatus(localStatus);
    uint32_t respId = generateMessageId();
    v3_send_frame(gEspNow->bondPendingResponseMac, ESPNOW_V3_TYPE_BOND_STATUS_RESP,
                  0, respId, (const uint8_t*)&localStatus, sizeof(localStatus), 1);
    BROADCAST_PRINTF("[BOND] 9j: Sent status response enabled=0x%04X connected=0x%04X heap=%lu",
           localStatus.sensorEnabledMask, localStatus.sensorConnectedMask,
           (unsigned long)localStatus.freeHeap);
  }

  // 9j2. Bond: proactive status push — sensor state changed locally, push to peer immediately
  if (gEspNow->bondNeedsProactiveStatus) {
    gEspNow->bondNeedsProactiveStatus = false;
    if (gSettings.bondModeEnabled && isBondSynced()) {
      uint8_t peerMac[6];
      if (parseMacAddress(gSettings.bondPeerMac, peerMac)) {
        BondPeerStatus localStatus;
        buildLocalBondStatus(localStatus);
        uint32_t respId = generateMessageId();
        v3_send_frame(peerMac, ESPNOW_V3_TYPE_BOND_STATUS_RESP,
                      0, respId, (const uint8_t*)&localStatus, sizeof(localStatus), 1);
        BROADCAST_PRINTF("[BOND] 9j2: Proactive status push enabled=0x%04X connected=0x%04X",
               localStatus.sensorEnabledMask, localStatus.sensorConnectedMask);
      }
    }
  }

  // 9k. Bond: periodic status request (~30s) — poll bonded peer for live status
  {
    static const uint32_t BOND_STATUS_POLL_MS = 30000;
    if (gSettings.bondModeEnabled && isBondSynced() &&
        (now - gEspNow->bondLastStatusReqMs >= BOND_STATUS_POLL_MS)) {
      gEspNow->bondLastStatusReqMs = now;
      uint8_t peerMac[6];
      if (parseMacAddress(gSettings.bondPeerMac, peerMac)) {
        uint32_t reqId = generateMessageId();
        v3_send_frame(peerMac, ESPNOW_V3_TYPE_BOND_STATUS_REQ, 0, reqId, nullptr, 0, 1);
        DEBUGF(DEBUG_ESPNOW_MESH, "[BOND] 9k: Sent status request to peer");
      }
    }
  }
#endif // ENABLE_BONDED_MODE

  // 9. Process deferred metadata response (peer requested our metadata)
  if (gEspNow->bondNeedsMetadataResponse) {
    gEspNow->bondNeedsMetadataResponse = false;
    DEBUG_ESPNOW_METADATAF("[METADATA] Task: sending deferred RESP to %s",
      MAC_STR(gEspNow->metadataPendingResponseMac));
    sendMetadata(gEspNow->metadataPendingResponseMac, false, true);
  }
  
  // 10. Process deferred received metadata (store in gMeshPeerMeta)
  if (gEspNow->deferredMetadataPending) {
    gEspNow->deferredMetadataPending = false;
    DEBUG_ESPNOW_METADATAF("[METADATA] Task: calling processMetadata for %s gMeshPeerMeta=%p slots=%d",
      MAC_STR(gEspNow->deferredMetadataSrcMac), gMeshPeerMeta, gMeshPeerSlots);
    processMetadata(gEspNow->deferredMetadataSrcMac, (const V3PayloadMetadata*)gEspNow->deferredMetadataPayload);
    DEBUG_ESPNOW_METADATAF("[METADATA] Task: processMetadata complete");
    
    // Log metadata to message history for web UI
    const V3PayloadMetadata* meta = (const V3PayloadMetadata*)gEspNow->deferredMetadataPayload;
    char metaMsg[384];
    snprintf(metaMsg, sizeof(metaMsg), 
             "Metadata: name=%s friendly=%s room=%s zone=%s tags=%s stationary=%d",
             meta->deviceName, meta->friendlyName, meta->room, meta->zone, 
             meta->tags, (int)meta->stationary);
    
    String devName = String(meta->deviceName);
    if (devName.length() == 0) devName = formatMacAddress(gEspNow->deferredMetadataSrcMac);
    storeMessageInPeerHistory(gEspNow->deferredMetadataSrcMac, devName.c_str(), 
                              metaMsg, false, MSG_TEXT);
  }
  
  // 11. Drain text message queue
  {
    int tail = gEspNow->textQueueTail;
    int processed = 0;
    while (tail != gEspNow->textQueueHead && processed < 4) {
      auto& entry = gEspNow->textQueue[tail];
      if (entry.used) {
        String devName = String(entry.deviceName);
        if (devName.length() == 0) devName = formatMacAddress(entry.srcMac);
        storeMessageInPeerHistory(entry.srcMac, devName.c_str(),
                                  entry.content, entry.encrypted, MSG_TEXT);
        BROADCAST_PRINTF("[%s%s] %s", devName.c_str(),
                         entry.encrypted ? " [enc]" : "", entry.content);
        entry.used = false;
      }
      tail = (tail + 1) & (EspNowState::TEXT_QUEUE_SIZE - 1);
      processed++;
    }
    gEspNow->textQueueTail = tail;
  }
}

static void espnowHeartbeatTaskFn(void* pvParam) {
  (void)pvParam;
  for (;;) {
    processMeshHeartbeats();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

bool startEspNowTask() {
  if (gEspNowHbTaskHandle != nullptr) return true;
  BaseType_t ret = xTaskCreatePinnedToCore(
    espnowHeartbeatTaskFn,
    "espnow_task",
    ESPNOW_HB_STACK_WORDS,
    nullptr,
    5,
    &gEspNowHbTaskHandle,
    0
  );
  return (ret == pdPASS);
}

void stopEspNowTask() {
  if (gEspNowHbTaskHandle) {
    vTaskDelete(gEspNowHbTaskHandle);
    gEspNowHbTaskHandle = nullptr;
  }
}

TaskHandle_t getEspNowTaskHandle() {
  return gEspNowHbTaskHandle;
}

// Helper: Initialize ESP-NOW subsystem (static - internal use only)
static bool initEspNow() {
  // Capture heap before initialization
  size_t heapBefore = ESP.getFreeHeap();
  
  // Set runtime peer slot count from settings (capped to compile-time ceiling)
  gMeshPeerSlots = gSettings.meshPeerMax;
  if (gMeshPeerSlots < 1) gMeshPeerSlots = 1;
  if (gMeshPeerSlots > MESH_PEER_MAX) gMeshPeerSlots = MESH_PEER_MAX;

  // Allocate mesh peer arrays (dynamic based on meshPeerMax setting)
  if (!gMeshPeers) {
    size_t healthSize = sizeof(MeshPeerHealth) * gMeshPeerSlots;
    gMeshPeers = (MeshPeerHealth*)ps_alloc(healthSize, AllocPref::PreferPSRAM, "mesh.peers");
    if (gMeshPeers) {
      memset(gMeshPeers, 0, healthSize);
    } else {
      broadcastOutput("[ESP-NOW] ERROR: Failed to allocate mesh peer health");
      return false;
    }
  }
  if (!gMeshPeerMeta) {
    size_t metaSize = sizeof(MeshPeerMeta) * gMeshPeerSlots;
    gMeshPeerMeta = (MeshPeerMeta*)ps_alloc(metaSize, AllocPref::PreferPSRAM, "mesh.meta");
    if (gMeshPeerMeta) {
      for (int i = 0; i < gMeshPeerSlots; i++) gMeshPeerMeta[i].clear();
    } else {
      broadcastOutput("[ESP-NOW] ERROR: Failed to allocate mesh peer meta");
      return false;
    }
  }

  // Allocate ESP-NOW state on first use
  if (!gEspNow) {
    gEspNow = (EspNowState*)ps_alloc(sizeof(EspNowState), AllocPref::PreferPSRAM, "espnow.state");
    if (!gEspNow) {
      broadcastOutput("[ESP-NOW] ERROR: Failed to allocate state structure");
      return false;
    }
    memset(gEspNow, 0, sizeof(EspNowState));

    // Allocate small PSRAM buffers pulled out of the EspNowState struct
    gEspNow->listBuffer = (char*)ps_alloc(1024, AllocPref::PreferPSRAM, "espnow.listBuf");
    if (gEspNow->listBuffer) {
      memset(gEspNow->listBuffer, 0, 1024);
    } else {
      BROADCAST_PRINTF("[ESP-NOW] WARNING: Failed to allocate listBuffer");
    }
    gEspNow->deferredCmdRespResult = (char*)ps_alloc(2048, AllocPref::PreferPSRAM, "espnow.cmdResp");
    if (gEspNow->deferredCmdRespResult) {
      memset(gEspNow->deferredCmdRespResult, 0, 2048);
    } else {
      BROADCAST_PRINTF("[ESP-NOW] WARNING: Failed to allocate deferredCmdRespResult");
    }

    // Allocate per-device message history (biggest single allocation)
    size_t histSize = sizeof(PeerMessageHistory) * gMeshPeerSlots;
    gEspNow->peerMessageHistories = (PeerMessageHistory*)ps_alloc(histSize, AllocPref::PreferPSRAM, "espnow.msgHist");
    if (gEspNow->peerMessageHistories) {
      // Placement-init each entry (has constructor)
      for (int i = 0; i < gMeshPeerSlots; i++) {
        new (&gEspNow->peerMessageHistories[i]) PeerMessageHistory();
      }
    } else {
      BROADCAST_PRINTF("[ESP-NOW] WARNING: Failed to allocate message history (%u bytes)", (unsigned)histSize);
      // Non-fatal — messaging will be limited but ESP-NOW still works
    }

    size_t totalBytes = sizeof(EspNowState) + histSize +
                        sizeof(MeshPeerHealth) * gMeshPeerSlots +
                        sizeof(MeshPeerMeta) * gMeshPeerSlots;
    BROADCAST_PRINTF("[ESP-NOW] Allocated state (%u bytes, %d peer slots)", (unsigned)totalBytes, gMeshPeerSlots);
  }

  // Allocate V3 reassembly buffers in PSRAM (saves ~6.4KB of internal BSS)
  if (!gV3Reasm) {
    size_t reasмSize = sizeof(V3ReasmEntry) * V3_REASM_MAX;
    gV3Reasm = (V3ReasmEntry*)ps_alloc(reasмSize, AllocPref::PreferPSRAM, "espnow.reasm");
    if (gV3Reasm) {
      memset(gV3Reasm, 0, reasмSize);
    } else {
      broadcastOutput("[ESP-NOW] WARNING: Failed to allocate reassembly buffers in PSRAM — fragmentation disabled");
    }
  }

  if (gEspNow->initialized) {
    broadcastOutput("[ESP-NOW] Already initialized");
    return true;
  }

  const char* setupError = checkEspNowFirstTimeSetup();
  if (setupError && strlen(setupError) > 0) {
    broadcastOutput(setupError);
    return false;
  }

  // Pause I2C sensor polling during WiFi mode change.
  // WiFi.mode(WIFI_AP_STA) temporarily disables interrupts on Core 0;
  // if an I2C transaction's ISR is pending, the interrupt watchdog fires.
  I2CDeviceManager* mgr = I2CDeviceManager::getInstance();
  if (mgr) mgr->pausePolling();
  vTaskDelay(pdMS_TO_TICKS(50));  // Let in-flight I2C transactions complete

  // Set WiFi mode to STA+AP to enable ESP-NOW
  WiFi.mode(WIFI_AP_STA);

  if (mgr) mgr->resumePolling();

  // Hide the soft AP so it doesn't broadcast an SSID (ESP_XXXXXX) in WiFi scans.
  // ESP-NOW requires AP mode but we don't want devices appearing as access points.
  {
    wifi_config_t ap_config;
    memset(&ap_config, 0, sizeof(ap_config));
    esp_wifi_get_config(WIFI_IF_AP, &ap_config);
    ap_config.ap.ssid_hidden = 1;       // Hide SSID from scan results
    ap_config.ap.max_connection = 0;     // Reject all STA connections
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
  }

  // Get current WiFi channel and use it for ESP-NOW
  wifi_config_t conf;
  esp_wifi_get_config(WIFI_IF_STA, &conf);
  gEspNow->channel = conf.sta.channel;
  if (gEspNow->channel == 0) {
    // Fallback: get channel from WiFi status
    gEspNow->channel = WiFi.channel();
  }
  if (gEspNow->channel == 0) {
    gEspNow->channel = 1;  // Final fallback
  }

  // Initialize ESP-NOW
  if (esp_now_init() != ESP_OK) {
    broadcastOutput("[ESP-NOW] Failed to initialize ESP-NOW");
    return false;
  }

  // Register callbacks (direct handler)
  esp_now_register_recv_cb(onEspNowDataReceived);
  esp_now_register_send_cb(onEspNowDataSent);

  gEspNow->initialized = true;

  // Apply persisted mesh/direct mode from settings (applySettings runs before gEspNow exists at boot)
  gEspNow->mode = gSettings.espnowmesh ? ESPNOW_MODE_MESH : ESPNOW_MODE_DIRECT;

  // Initialize retry queue mutex
  if (!gMeshRetryMutex) {
    gMeshRetryMutex = xSemaphoreCreateMutex();
    if (gMeshRetryMutex) {
      // Clear retry queue
      memset(gMeshRetryQueue, 0, sizeof(gMeshRetryQueue));
      broadcastOutput("[ESP-NOW] Retry queue initialized (8 slots, 3s timeout, 2 retries)");
    } else {
      broadcastOutput("[ESP-NOW] WARNING: Failed to create retry queue mutex - retries disabled");
    }
  }

  // Allocate chunked message buffer (only when ESP-NOW is active)
  if (!gActiveMessage) {
    gActiveMessage = (ChunkedMessage*)ps_alloc(sizeof(ChunkedMessage), AllocPref::PreferPSRAM, "espnow.chunk");
    if (gActiveMessage) {
      memset(gActiveMessage, 0, sizeof(ChunkedMessage));
      gActiveMessage->active = false;  // Explicitly ensure not active
      BROADCAST_PRINTF("[ESP-NOW] Allocated chunked message buffer (%u bytes)", (unsigned)sizeof(ChunkedMessage));
    } else {
      DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] WARNING: Failed to allocate chunked message buffer - remote commands may fail");
    }
  }
  
  // Initialize broadcast tracker (static allocation)
  memset(gBroadcastTrackers, 0, sizeof(gBroadcastTrackers));
  gBroadcastsTracked = 0;
  gBroadcastsCompleted = 0;
  gBroadcastsTimedOut = 0;
  BROADCAST_PRINTF("[ESP-NOW] Initialized broadcast tracker (%u bytes, %d slots)", 
                   (unsigned)sizeof(gBroadcastTrackers), BROADCAST_TRACKER_SLOTS);

  DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] Initialized successfully on channel %d", gEspNow->channel);
  // Use BROADCAST_PRINTF for user-visible init message (safe, doesn't trigger streaming)
  BROADCAST_PRINTF("[ESP-NOW] Initialized successfully on channel %d", gEspNow->channel);

  // Restore encryption passphrase from settings (if previously set)
  if (gSettings.espnowPassphrase.length() > 0) {
    gEspNow->passphrase = gSettings.espnowPassphrase;
    deriveKeyFromPassphrase(gSettings.espnowPassphrase, gEspNow->derivedKey);
    DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] Restored encryption passphrase from settings");
  }

  // Add broadcast peer for public heartbeat mode
  esp_now_peer_info_t broadcastPeer;
  memset(&broadcastPeer, 0, sizeof(broadcastPeer));
  memset(broadcastPeer.peer_addr, 0xFF, 6);  // FF:FF:FF:FF:FF:FF
  broadcastPeer.channel = gEspNow->channel;
  broadcastPeer.encrypt = false;
  
  esp_err_t addStatus = esp_now_add_peer(&broadcastPeer);
  if (addStatus == ESP_OK) {
    DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] Broadcast peer (FF:FF:FF:FF:FF:FF) registered for public heartbeat mode");
  } else if (addStatus == ESP_ERR_ESPNOW_EXIST) {
    // Already exists, that's fine
  } else {
    BROADCAST_PRINTF("[ESP-NOW] WARNING: Failed to add broadcast peer (error %d)", (int)addStatus);
  }

  // Load and restore saved devices
  loadEspNowDevices();
  restoreEspNowPeers();
  
  // Load mesh peer health data
  loadMeshPeers();
  
#if ENABLE_BONDED_MODE
  // Dump bond settings at init so we can verify what was loaded from flash
  computeBondLocalSettingsHash();
  BROADCAST_PRINTF("[BOND_INIT] bondModeEnabled=%d bondPeerMac='%s' bondRole=%d settingsHash=0x%08lX",
                   (int)gSettings.bondModeEnabled, gSettings.bondPeerMac.c_str(), (int)gSettings.bondRole,
                   (unsigned long)gEspNow->bondLocalSettingsHash);
  if (gSettings.bondModeEnabled && gSettings.bondPeerMac.length() > 0) {
    uint8_t testMac[6];
    bool parseOk = parseMacAddress(gSettings.bondPeerMac, testMac);
    BROADCAST_PRINTF("[BOND_INIT] peerMac parse=%d -> %02X:%02X:%02X:%02X:%02X:%02X",
                     (int)parseOk, testMac[0], testMac[1], testMac[2], testMac[3], testMac[4], testMac[5]);
    // Check if peer is in our device list (required for isPaired check)
    bool peerInDevList = false;
    for (int i = 0; i < gEspNow->deviceCount; i++) {
      if (memcmp(gEspNow->devices[i].mac, testMac, 6) == 0) {
        peerInDevList = true;
        BROADCAST_PRINTF("[BOND_INIT] peer found in devices[%d] name='%s'", i, gEspNow->devices[i].name.c_str());
        break;
      }
    }
    if (!peerInDevList) {
      BROADCAST_PRINTF("[BOND_INIT] WARNING: bond peer NOT in device list! isPaired will be false — heartbeats will be ignored!");
    }
  }
#endif
  
  // Register own device name for topology display
  // Use the device name from settings (set via 'espnow setname')
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String myName = gSettings.espnowDeviceName;
  
  if (myName.length() > 0) {
    // Check if already registered
    bool alreadyRegistered = false;
    for (int i = 0; i < gEspNow->deviceCount; i++) {
      if (memcmp(gEspNow->devices[i].mac, myMac, 6) == 0) {
        alreadyRegistered = true;
        // Update name if it changed
        if (gEspNow->devices[i].name != myName) {
          gEspNow->devices[i].name = myName;
          saveMeshPeers();
          BROADCAST_PRINTF("[ESP-NOW] Updated own device name: %s", myName.c_str());
        }
        break;
      }
    }
    
    if (!alreadyRegistered) {
      addEspNowDevice(myMac, myName, false, nullptr);
      saveMeshPeers();
      BROADCAST_PRINTF("[ESP-NOW] Registered own device name: %s", myName.c_str());
    }
  } else {
    broadcastOutput("[ESP-NOW] WARNING: Device name not set in settings");
  }

  // Start ESP-NOW heartbeat task (parallel processing)
  if (!startEspNowTask()) {
    broadcastOutput("[ESP-NOW] WARNING: Failed to start heartbeat task - mesh features may not work");
    return false;
  }
  
  // Calculate heap usage and warn user
  size_t heapAfter = ESP.getFreeHeap();
  size_t heapUsed = heapBefore - heapAfter;
  
  broadcastOutput("[ESP-NOW] System initialized successfully");
  BROADCAST_PRINTF("[ESP-NOW] Heap allocated: ~%u KB (includes task stack, buffers, peer storage)", (unsigned)(heapUsed / 1024));
  broadcastOutput("[ESP-NOW] NOTE: This heap remains allocated until device reboot. Disable and re-init will not free all memory.");

  // Broadcast boot notification to all peers
  extern uint32_t gBootCounter;
  time_t now = time(nullptr);
  uint32_t timestamp = (now > 1609459200) ? now : 0;  // Valid if after 2021-01-01
  
  String bootMsg = buildBootNotification(generateMessageId(), gEspNow->deviceName.c_str(), gBootCounter, timestamp);
  meshSendEnvelopeToPeers(bootMsg);
  BROADCAST_PRINTF("[ESP-NOW] Boot notification sent (counter=%lu)", (unsigned long)gBootCounter);

  notifyEspNowStarted(true);
  return true;
}

// ESP-NOW init command
const char* cmd_espnow_init(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (gEspNow && gEspNow->initialized) {
    return "ESP-NOW already initialized";
  }

  // Check memory before initializing ESP-NOW (task stack + state struct)
  if (!checkMemoryAvailable("espnow", nullptr)) {
    return "Insufficient memory for ESP-NOW (need ~40KB DRAM + ~320KB PSRAM)";
  }

  if (initEspNow()) {
    return "ESP-NOW initialized successfully";
  } else {
    return "Failed to initialize ESP-NOW";
  }
}

// Helper: Deinitialize ESP-NOW subsystem (static - internal use only)
static bool deinitEspNow() {
  if (!gEspNow || !gEspNow->initialized) {
    broadcastOutput("[ESP-NOW] Not initialized, nothing to deinit");
    return false;
  }

  size_t heapBefore = ESP.getFreeHeap();

  // 1. Stop heartbeat/mesh task
  stopEspNowTask();
  broadcastOutput("[ESP-NOW] Heartbeat task stopped");

  // 2. Stop any active output streaming
  if (gEspNow->streamActive) {
    gEspNow->streamActive = false;
    gEspNow->streamTarget = nullptr;
    broadcastOutput("[ESP-NOW] Output streaming stopped");
  }

  // 3. Cleanup active file transfer
  if (gActiveFileTransfer) {
    if (gActiveFileTransfer->chunkMap) {
      heap_caps_free(gActiveFileTransfer->chunkMap);
    }
    if (gActiveFileTransfer->dataBuffer) {
      heap_caps_free(gActiveFileTransfer->dataBuffer);
    }
    delete gActiveFileTransfer;
    gActiveFileTransfer = nullptr;
    broadcastOutput("[ESP-NOW] Active file transfer cleaned up");
  }

  // 4. Cleanup chunked message buffer
  if (gActiveMessage) {
    free(gActiveMessage);
    gActiveMessage = nullptr;
    broadcastOutput("[ESP-NOW] Chunked message buffer freed");
  }

  // 5. Clear retry queue entries
  if (gMeshRetryMutex && xSemaphoreTake(gMeshRetryMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    memset(gMeshRetryQueue, 0, sizeof(gMeshRetryQueue));
    xSemaphoreGive(gMeshRetryMutex);
  }

  // 6. Unregister callbacks and deinit ESP-NOW
  esp_now_unregister_recv_cb();
  esp_now_unregister_send_cb();
  esp_err_t err = esp_now_deinit();
  if (err != ESP_OK) {
    BROADCAST_PRINTF("[ESP-NOW] WARNING: esp_now_deinit returned error %d", (int)err);
  }

  // 7. Reset state (keep gEspNow struct allocated for potential re-init)
  gEspNow->initialized = false;
  gEspNow->channel = 0;
  gEspNow->encryptionEnabled = false;
  gEspNow->passphrase = "";
  memset(gEspNow->derivedKey, 0, sizeof(gEspNow->derivedKey));

  size_t heapAfter = ESP.getFreeHeap();
  size_t heapFreed = heapAfter - heapBefore;
  BROADCAST_PRINTF("[ESP-NOW] Deinitialized. Freed ~%u KB heap", (unsigned)(heapFreed / 1024));

  notifyEspNowStopped();
  return true;
}

// ESP-NOW deinit command
const char* cmd_espnow_deinit(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow || !gEspNow->initialized) {
    return "ESP-NOW is not initialized";
  }

  if (deinitEspNow()) {
    return "ESP-NOW deinitialized successfully";
  } else {
    return "Failed to deinitialize ESP-NOW";
  }
}

// ESP-NOW status command
const char* cmd_espnow_status(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  char* p = getDebugBuffer();
  size_t remaining = 1024;

  int n = snprintf(p, remaining, "ESP-NOW Status:\n");
  p += n;
  remaining -= n;

  n = snprintf(p, remaining, "  Initialized: %s\n", gEspNow->initialized ? "Yes" : "No");
  p += n;
  remaining -= n;

  n = snprintf(p, remaining, "  Channel: %d\n", gEspNow->channel);
  p += n;
  remaining -= n;

  if (gEspNow->initialized) {
    n = snprintf(p, remaining, "  MAC Address: ");
    p += n;
    remaining -= n;

    uint8_t mac[6];
    WiFi.macAddress(mac);
    for (int i = 0; i < 6; i++) {
      if (i > 0) {
        n = snprintf(p, remaining, ":");
        p += n;
        remaining -= n;
      }
      n = snprintf(p, remaining, "%02X", mac[i]);
      p += n;
      remaining -= n;
    }

    n = snprintf(p, remaining, "\n");
    p += n;
    remaining -= n;

    // Subtract 1 to exclude self from paired device count
    int pairedCount = (gEspNow->deviceCount > 0) ? (gEspNow->deviceCount - 1) : 0;
    n = snprintf(p, remaining, "  Paired Devices: %d\n", pairedCount);
    p += n;
    remaining -= n;
  }

  return getDebugBuffer();
}

// ESP-NOW statistics command
const char* cmd_espnow_stats(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  
  // Output each line separately to avoid DEBUG_MSG_SIZE (256 byte) truncation
  broadcastOutput("ESP-NOW Statistics:");
  BROADCAST_PRINTF("  Messages Sent: %lu", (unsigned long)gEspNow->routerMetrics.messagesSent);
  BROADCAST_PRINTF("  Messages Received: %lu", (unsigned long)gEspNow->routerMetrics.messagesReceived);
  BROADCAST_PRINTF("  Send Failures: %lu", (unsigned long)gEspNow->routerMetrics.messagesFailed);
  BROADCAST_PRINTF("  Receive Errors: %lu", (unsigned long)gEspNow->receiveErrors);
  BROADCAST_PRINTF("  Stream Sent: %lu", (unsigned long)gEspNow->streamSentCount);
  BROADCAST_PRINTF("  Stream Received: %lu", (unsigned long)gEspNow->streamReceivedCount);
  BROADCAST_PRINTF("  Stream Dropped: %lu", (unsigned long)gEspNow->streamDroppedCount);
  
  if (meshEnabled()) {
    BROADCAST_PRINTF("  Heartbeats Sent: %lu", (unsigned long)gEspNow->heartbeatsSent);
    BROADCAST_PRINTF("  Heartbeats Received: %lu", (unsigned long)gEspNow->heartbeatsReceived);
    BROADCAST_PRINTF("  Mesh Forwards: %lu", (unsigned long)gEspNow->meshForwards);
  }
  
  BROADCAST_PRINTF("  Files Sent: %lu", (unsigned long)gEspNow->fileTransfersSent);
  BROADCAST_PRINTF("  Files Received: %lu", (unsigned long)gEspNow->fileTransfersReceived);
  
  if (gEspNow->lastResetTime > 0) {
    unsigned long uptime = (millis() - gEspNow->lastResetTime) / 1000;
    BROADCAST_PRINTF("  Uptime: %lus", uptime);
  } else {
    BROADCAST_PRINTF("  Uptime: %lus (since boot)", millis() / 1000);
  }
  
  return "OK";
}

// ESP-NOW broadcast tracking statistics command
const char* cmd_espnow_broadcaststats(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  
  broadcastOutput("Broadcast ACK Tracking Statistics:");
  BROADCAST_PRINTF("  Broadcasts Tracked: %lu", (unsigned long)gBroadcastsTracked);
  BROADCAST_PRINTF("  Broadcasts Completed (100%%): %lu", (unsigned long)gBroadcastsCompleted);
  BROADCAST_PRINTF("  Broadcasts Timed Out: %lu", (unsigned long)gBroadcastsTimedOut);
  
  if (gBroadcastsTracked > 0) {
    float successRate = 100.0f * gBroadcastsCompleted / gBroadcastsTracked;
    BROADCAST_PRINTF("  Success Rate: %.1f%%", successRate);
  }
  
  broadcastOutput("\nActive Trackers:");
  int activeCount = 0;
  for (int i = 0; i < BROADCAST_TRACKER_SLOTS; i++) {
    if (gBroadcastTrackers[i].active) {
      activeCount++;
      uint32_t elapsed = millis() - gBroadcastTrackers[i].startMs;
      BROADCAST_PRINTF("  [%d] msgId=%lu: %u/%u ACKs (%.1f%%) elapsed=%lums",
                      i, (unsigned long)gBroadcastTrackers[i].msgId,
                      gBroadcastTrackers[i].receivedCount,
                      gBroadcastTrackers[i].expectedCount,
                      (gBroadcastTrackers[i].expectedCount > 0) 
                        ? (100.0f * gBroadcastTrackers[i].receivedCount / gBroadcastTrackers[i].expectedCount) : 0.0f,
                      (unsigned long)elapsed);
    }
  }
  
  if (activeCount == 0) {
    BROADCAST_PRINTF("  No active trackers");
  }
  
  return "OK";
}

// ESP-NOW router statistics command
const char* cmd_espnow_routerstats(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  
  broadcastOutput("=== ESP-NOW Router Statistics ===");
  BROADCAST_PRINTF("Messages Sent: %lu", (unsigned long)gEspNow->routerMetrics.messagesSent);
  BROADCAST_PRINTF("Messages Received: %lu", (unsigned long)gEspNow->routerMetrics.messagesReceived);
  BROADCAST_PRINTF("Messages Failed: %lu", (unsigned long)gEspNow->routerMetrics.messagesFailed);
  BROADCAST_PRINTF("Messages Retried: %lu", (unsigned long)gEspNow->routerMetrics.messagesRetried);
  BROADCAST_PRINTF("Messages Dropped: %lu", (unsigned long)gEspNow->routerMetrics.messagesDropped);
  
  broadcastOutput("\nRouting:");
  uint32_t totalRoutes = gEspNow->routerMetrics.directRoutes + gEspNow->routerMetrics.meshRoutes;
  if (totalRoutes > 0) {
    uint32_t directPct = (gEspNow->routerMetrics.directRoutes * 100) / totalRoutes;
    uint32_t meshPct = (gEspNow->routerMetrics.meshRoutes * 100) / totalRoutes;
    BROADCAST_PRINTF("  Direct Routes: %lu (%lu%%)",
                     (unsigned long)gEspNow->routerMetrics.directRoutes,
                     (unsigned long)directPct);
    BROADCAST_PRINTF("  Mesh Routes: %lu (%lu%%)",
                     (unsigned long)gEspNow->routerMetrics.meshRoutes,
                     (unsigned long)meshPct);
  } else {
    broadcastOutput("  No routes yet");
  }
  
  broadcastOutput("\nQueue/Retry:");
  BROADCAST_PRINTF("  Current Queue Size: %d", (int)(gEspNow->queueSize));
  BROADCAST_PRINTF("  Messages Queued: %lu", (unsigned long)gEspNow->routerMetrics.messagesQueued);
  BROADCAST_PRINTF("  Messages Dequeued: %lu", (unsigned long)gEspNow->routerMetrics.messagesDequeued);
  BROADCAST_PRINTF("  Retries Attempted: %lu", (unsigned long)gEspNow->routerMetrics.retriesAttempted);
  BROADCAST_PRINTF("  Retries Succeeded: %lu", (unsigned long)gEspNow->routerMetrics.retriesSucceeded);
  BROADCAST_PRINTF("  Queue Overflows: %lu", (unsigned long)gEspNow->routerMetrics.queueOverflows);

  broadcastOutput("\nChunking (Send):");
  BROADCAST_PRINTF("  Chunked Messages: %lu", (unsigned long)gEspNow->routerMetrics.chunkedMessages);
  BROADCAST_PRINTF("  Chunks Sent: %lu", (unsigned long)gEspNow->routerMetrics.chunksSent);
  BROADCAST_PRINTF("  Chunks Dropped: %lu", (unsigned long)gEspNow->routerMetrics.chunksDropped);
  
  broadcastOutput("\nChunking (Receive):");
  BROADCAST_PRINTF("  Chunks Received: %lu", (unsigned long)gEspNow->routerMetrics.chunksReceived);
  BROADCAST_PRINTF("  Messages Reassembled: %lu", (unsigned long)gEspNow->routerMetrics.chunksReassembled);
  BROADCAST_PRINTF("  Chunks Timed Out: %lu", (unsigned long)gEspNow->routerMetrics.chunksTimedOut);
  
  
  int activeBuffers = 0;
  for (int i = 0; i < 4; i++) {
    if (gEspNow->chunkBuffers[i].active) {
      activeBuffers++;
    }
  }
  if (activeBuffers > 0) {
    BROADCAST_PRINTF("  Active Buffers: %d/4", activeBuffers);
    for (int i = 0; i < 4; i++) {
      if (gEspNow->chunkBuffers[i].active) {
        BROADCAST_PRINTF("    Buffer %d: msgId=%lu, %lu/%lu chunks, age=%lus",
                        i,
                        (unsigned long)gEspNow->chunkBuffers[i].msgId,
                        (unsigned long)gEspNow->chunkBuffers[i].receivedChunks,
                        (unsigned long)gEspNow->chunkBuffers[i].totalChunks,
                        (millis() - gEspNow->chunkBuffers[i].lastChunkTime) / 1000);
      }
    }
  }
  
  broadcastOutput("\nPerformance:");
  BROADCAST_PRINTF("  Avg Send Time: %lu µs", (unsigned long)gEspNow->routerMetrics.avgSendTimeUs);
  BROADCAST_PRINTF("  Max Send Time: %lu µs", (unsigned long)gEspNow->routerMetrics.maxSendTimeUs);
  
  broadcastOutput("\nMessage IDs:");
  BROADCAST_PRINTF("  Next Message ID: %lu", (unsigned long)gEspNow->nextMessageId);
  
  return "OK";
}

// ESP-NOW reset statistics command
const char* cmd_espnow_resetstats(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  
  gEspNow->receiveErrors = 0;
  gEspNow->heartbeatsSent = 0;
  gEspNow->heartbeatsReceived = 0;
  gEspNow->meshForwards = 0;
  gEspNow->streamSentCount = 0;
  gEspNow->streamReceivedCount = 0;
  gEspNow->streamDroppedCount = 0;
  gEspNow->fileTransfersSent = 0;
  gEspNow->fileTransfersReceived = 0;
  gEspNow->lastResetTime = millis();
  
  gEspNow->routerMetrics = RouterMetrics();
  
  return "ESP-NOW statistics reset (including router metrics)";
}

// ESP-NOW pair device command
const char* cmd_espnow_pair(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "ESP-NOW not initialized. Run 'openespnow' first.";
  }

  String args = argsIn;
  args.trim();

  int firstSpace = args.indexOf(' ');
  if (firstSpace < 0) return "Usage: espnow pair <mac> <name>";

  String macStr = args.substring(0, firstSpace);
  String name = args.substring(firstSpace + 1);
  macStr.trim();
  name.trim();

  if (macStr.length() == 0 || name.length() == 0) {
    return "Usage: espnow pair <mac> <name>";
  }

  uint8_t mac[6];
  if (!parseMacAddress(macStr, mac)) {
    return "Invalid MAC address format. Use AA:BB:CC:DD:EE:FF";
  }
  // Prevent pairing with self MAC (STA or AP interface)
  {
    uint8_t selfSta[6];
    uint8_t selfAp[6];
    esp_wifi_get_mac(WIFI_IF_STA, selfSta);
    esp_wifi_get_mac(WIFI_IF_AP, selfAp);
    if (memcmp(mac, selfSta, 6) == 0 || memcmp(mac, selfAp, 6) == 0) {
      return "Cannot pair with self MAC address.";
    }
  }

  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (memcmp(gEspNow->devices[i].mac, mac, 6) == 0) {
      if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
      snprintf(getDebugBuffer(), 1024, "Device already paired. Use 'espnow unpair %s' first.", macStr.c_str());
      return getDebugBuffer();
    }
  }

  if (gEspNow->deviceCount >= 16) {
    return "Maximum number of devices (16) already paired.";
  }

  if (!addEspNowPeerWithEncryption(mac, false, nullptr)) {
    return "Failed to add unencrypted peer to ESP-NOW.";
  }

  memcpy(gEspNow->devices[gEspNow->deviceCount].mac, mac, 6);
  gEspNow->devices[gEspNow->deviceCount].name = name;
  gEspNow->devices[gEspNow->deviceCount].encrypted = false;
  memset(gEspNow->devices[gEspNow->deviceCount].key, 0, 16);
  gEspNow->deviceCount++;

  removeFromUnpairedList(mac);

  // Seed gMeshPeers immediately so v3_broadcast reaches this peer on the next
  // heartbeat tick (without this, two freshly-paired devices never exchange
  // heartbeats until one reboots, so mesh topology stays empty)
  if (meshEnabled()) {
    MeshPeerHealth* ph = getMeshPeerHealth(mac, true);
    if (ph) ph->lastHeartbeatMs = millis();
    V3PayloadHeartbeat hb = {};
    hb.role = gSettings.meshRole;
    hb.uptimeSec = (uint32_t)(millis() / 1000);
    hb.freeHeap  = (uint32_t)ESP.getFreeHeap();
    strncpy(hb.deviceName, gSettings.espnowDeviceName.c_str(), sizeof(hb.deviceName) - 1);
    wifi_ap_record_t ap = {};
    hb.rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : (int8_t)-127;
    v3_send_frame(mac, ESPNOW_V3_TYPE_HEARTBEAT, ESPNOW_V3_FLAG_ACK_REQ, generateMessageId(),
                  (const uint8_t*)&hb, (uint16_t)sizeof(hb), 1);
  }
  saveMeshPeers();
  saveEspNowDevices();

  snprintf(getDebugBuffer(), 1024, "Unencrypted device paired successfully: %s (%s)", name.c_str(), macStr.c_str());
  return getDebugBuffer();
}

// Mesh TTL command
const char* cmd_espnow_meshttl(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsIn;
  args.trim();
  
  if (args.length() == 0) {
    if (!ensureDebugBuffer()) return "Error";
    int peerCount = (gEspNow ? (int)gEspNow->deviceCount : 0);
    snprintf(getDebugBuffer(), 1024, "Mesh TTL: %d\nAdaptive mode: %s\nActive peers: %d", 
             gSettings.meshTTL, gSettings.meshAdaptiveTTL ? "enabled" : "disabled", peerCount);
    return getDebugBuffer();
  }
  
  // Check for 'adaptive' command
  args.toLowerCase();
  if (args == "adaptive") {
    setSetting(gSettings.meshAdaptiveTTL, !gSettings.meshAdaptiveTTL);
    
    snprintf(getDebugBuffer(), 1024, "Adaptive TTL %s (TTL now %d)", 
             gSettings.meshAdaptiveTTL ? "enabled" : "disabled", gSettings.meshTTL);
    return getDebugBuffer();
  }
  
  int ttl = args.toInt();
  if (ttl < 1 || ttl > 10) {
    return "Error: TTL must be between 1 and 10, or 'adaptive' to toggle";
  }
  
  // Setting a manual TTL disables adaptive mode
  setSetting(gSettings.meshTTL, (uint8_t)ttl);
  setSetting(gSettings.meshAdaptiveTTL, false);
  
  snprintf(getDebugBuffer(), 1024, "Mesh TTL set to %d (adaptive mode disabled)", gSettings.meshTTL);
  return getDebugBuffer();
}

// Mesh metrics command
const char* cmd_espnow_meshmetrics(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!ensureDebugBuffer()) return "Error: Buffer allocation failed";
  
  RouterMetrics& m = gEspNow->routerMetrics;
  
  int pos = 0;
  char* buf = getDebugBuffer();
  pos += snprintf(buf + pos, 1024 - pos, "=== Mesh Routing Metrics ===\n\n");
  
  // Mesh routing overview
  pos += snprintf(buf + pos, 1024 - pos, "Routing:\n");
  pos += snprintf(buf + pos, 1024 - pos, "  Mesh routes: %lu\n", (unsigned long)m.meshRoutes);
  pos += snprintf(buf + pos, 1024 - pos, "  Direct routes: %lu\n", (unsigned long)m.directRoutes);
  pos += snprintf(buf + pos, 1024 - pos, "  Total forwards: %lu\n\n", (unsigned long)gEspNow->meshForwards);
  
  // Forwards by message type
  pos += snprintf(buf + pos, 1024 - pos, "Forwards by type:\n");
  const char* typeNames[] = {"HB", "ACK", "MESH_SYS", "FILE", "CMD", "TEXT", "RESPONSE", "STREAM"};
  for (int i = 0; i < 8; i++) {
    if (m.meshForwardsByType[i] > 0) {
      pos += snprintf(buf + pos, 1024 - pos, "  %s: %lu\n", 
                     typeNames[i], (unsigned long)m.meshForwardsByType[i]);
    }
  }
  
  // Path statistics
  pos += snprintf(buf + pos, 1024 - pos, "\nPath statistics:\n");
  if (m.meshPathLengthCount > 0) {
    float avgPathLen = (float)m.meshPathLengthSum / (float)m.meshPathLengthCount;
    pos += snprintf(buf + pos, 1024 - pos, "  Avg path length: %.1f\n", avgPathLen);
    pos += snprintf(buf + pos, 1024 - pos, "  Max path length: %d\n", m.meshMaxPathLength);
  } else {
    pos += snprintf(buf + pos, 1024 - pos, "  No path data yet\n");
  }
  
  // Drop statistics
  pos += snprintf(buf + pos, 1024 - pos, "\nDrops:\n");
  pos += snprintf(buf + pos, 1024 - pos, "  TTL exhausted: %lu\n", (unsigned long)m.meshTTLExhausted);
  pos += snprintf(buf + pos, 1024 - pos, "  Loop detected: %lu\n", (unsigned long)m.meshLoopDetected);
  
  // Current configuration
  int peerCount = (gEspNow ? (int)gEspNow->deviceCount : 0);
  pos += snprintf(buf + pos, 1024 - pos, "Configuration:\n");
  pos += snprintf(buf + pos, 1024 - pos, "  Active peers: %d\n", peerCount);
  pos += snprintf(buf + pos, 1024 - pos, "  Adaptive TTL: %s\n", 
                 gSettings.meshAdaptiveTTL ? "enabled" : "disabled");
  pos += snprintf(buf + pos, 1024 - pos, "  Current TTL: %d\n", gSettings.meshTTL);
  
  return getDebugBuffer();
}

// ESP-NOW mode command
const char* cmd_espnow_mode(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsIn;
  args.trim();
  if (args.length() == 0) {
    snprintf(getDebugBuffer(), 1024, "ESP-NOW mode: %s", getEspNowModeString());
    return getDebugBuffer();
  }
  args.toLowerCase();
  if (args == "direct") {
    setSetting(gSettings.espnowmesh, false);
    if (gEspNow) {
      gEspNow->mode = ESPNOW_MODE_DIRECT;  // Update runtime state immediately
    }
    saveMeshPeers();
    BROADCAST_PRINTF("[ESP-NOW] mode set to %s", getEspNowModeString());
    return "ESP-NOW mode set to direct";
  } else if (args == "mesh") {
    setSetting(gSettings.espnowmesh, true);
    if (gEspNow) {
      gEspNow->mode = ESPNOW_MODE_MESH;  // Update runtime state immediately
    }
    saveMeshPeers();
    BROADCAST_PRINTF("[ESP-NOW] mode set to %s", getEspNowModeString());
    return "ESP-NOW mode set to mesh";
  }
  return "Usage: espnow mode [direct|mesh]";
}

// ESP-NOW setname command
const char* cmd_espnow_setname(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsIn;
  args.trim();
  
  if (args.length() == 0) {
    if (gSettings.espnowDeviceName.length() > 0) {
      snprintf(getDebugBuffer(), 1024, "Device name: %s", gSettings.espnowDeviceName.c_str());
    } else {
      snprintf(getDebugBuffer(), 1024, "Device name: (not set)");
    }
    return getDebugBuffer();
  }
  
  if (args.length() > 20) {
    return "Error: Device name must be 20 characters or less";
  }
  
  for (size_t i = 0; i < args.length(); i++) {
    char c = args.charAt(i);
    if (!isalnum(c) && c != '-' && c != '_') {
      return "Error: Device name can only contain letters, numbers, hyphens, and underscores";
    }
  }
  
  setSetting(gSettings.espnowDeviceName, args);
  setSetting(gSettings.espnowFirstTimeSetup, true);
  
  if (gEspNow && gEspNow->initialized) {
    uint8_t myMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, myMac);
    
    bool found = false;
    for (int i = 0; i < gEspNow->deviceCount; i++) {
      if (memcmp(gEspNow->devices[i].mac, myMac, 6) == 0) {
        gEspNow->devices[i].name = args;
        found = true;
        break;
      }
    }
    
    if (!found) {
      addEspNowDevice(myMac, args, false, nullptr);
    }
    
    saveMeshPeers();
  }
  
  snprintf(getDebugBuffer(), 1024, "Device name set to: %s", args.c_str());
  return getDebugBuffer();
}

// ==========================
// ESP-NOW Device Metadata CLI Commands
// ==========================

// Generic get/set for a string setting with max length
static const char* metaGetSet(const String& args, String& field, const char* fieldName, size_t maxLen) {
  String trimmed = args;
  trimmed.trim();
  if (trimmed.length() == 0) {
    // Display current value
    snprintf(getDebugBuffer(), 1024, "%s: %s", fieldName, field.length() > 0 ? field.c_str() : "(not set)");
    return getDebugBuffer();
  }
  if (trimmed == "clear") {
    field = "";
    gMetadataChanged = true;  // Mark metadata as dirty
    snprintf(getDebugBuffer(), 1024, "%s cleared", fieldName);
    return getDebugBuffer();
  }
  // Remove quotes if present
  if (trimmed.startsWith("\"") && trimmed.endsWith("\"") && trimmed.length() >= 2) {
    trimmed = trimmed.substring(1, trimmed.length() - 1);
  }
  if (trimmed.length() > maxLen) {
    snprintf(getDebugBuffer(), 1024, "Error: %s too long (max %zu chars)", fieldName, maxLen);
    return getDebugBuffer();
  }
  field = trimmed;
  gMetadataChanged = true;  // Mark metadata as dirty
  snprintf(getDebugBuffer(), 1024, "%s set to: %s", fieldName, field.c_str());
  return getDebugBuffer();
}

const char* cmd_espnow_room(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return metaGetSet(argsIn, gSettings.espnowRoom, "Room", 31);
}

const char* cmd_espnow_zone(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return metaGetSet(argsIn, gSettings.espnowZone, "Zone", 31);
}

const char* cmd_espnow_tags(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return metaGetSet(argsIn, gSettings.espnowTags, "Tags", 63);
}

const char* cmd_espnow_friendlyname(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return metaGetSet(argsIn, gSettings.espnowFriendlyName, "Friendly name", 47);
}

const char* cmd_espnow_stationary(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsIn;
  args.trim();
  if (args.length() == 0) {
    snprintf(getDebugBuffer(), 1024, "Stationary: %s", gSettings.espnowStationary ? "true" : "false");
    return getDebugBuffer();
  }
  bool val = (args == "1" || args == "true" || args == "on");
  setSetting(gSettings.espnowStationary, val);
  snprintf(getDebugBuffer(), 1024, "Stationary set to: %s", val ? "true" : "false");
  return getDebugBuffer();
}

const char* cmd_espnow_deviceinfo(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  char* buf = getDebugBuffer();
  int pos = 0;
  pos += snprintf(buf + pos, 1024 - pos, "=== Device Metadata ===\n");
  pos += snprintf(buf + pos, 1024 - pos, "Name:          %s\n",
                  gSettings.espnowDeviceName.length() > 0 ? gSettings.espnowDeviceName.c_str() : "(not set)");
  pos += snprintf(buf + pos, 1024 - pos, "Friendly Name: %s\n",
                  gSettings.espnowFriendlyName.length() > 0 ? gSettings.espnowFriendlyName.c_str() : "(not set)");
  pos += snprintf(buf + pos, 1024 - pos, "Room:          %s\n",
                  gSettings.espnowRoom.length() > 0 ? gSettings.espnowRoom.c_str() : "(not set)");
  pos += snprintf(buf + pos, 1024 - pos, "Zone:          %s\n",
                  gSettings.espnowZone.length() > 0 ? gSettings.espnowZone.c_str() : "(not set)");
  pos += snprintf(buf + pos, 1024 - pos, "Tags:          %s\n",
                  gSettings.espnowTags.length() > 0 ? gSettings.espnowTags.c_str() : "(none)");
  pos += snprintf(buf + pos, 1024 - pos, "Stationary:    %s\n",
                  gSettings.espnowStationary ? "true" : "false");

  // Show mesh role
  const char* roleStr = "worker";
  if (gSettings.meshRole == MESH_ROLE_MASTER) roleStr = "master";
  else if (gSettings.meshRole == MESH_ROLE_BACKUP_MASTER) roleStr = "backup";
  pos += snprintf(buf + pos, 1024 - pos, "Mesh Role:     %s\n", roleStr);

  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  pos += snprintf(buf + pos, 1024 - pos, "MAC:           %s", MAC_STR(myMac));

  return buf;
}

// ==========================
// Master Aggregation CLI Commands (Phase 3)
// ==========================

const char* cmd_espnow_devices(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  char* buf = getDebugBuffer();
  int pos = 0;
  pos += snprintf(buf + pos, 1024 - pos, "=== Mesh Devices ===\n");

  int count = 0;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (!gMeshPeerMeta[i].isActive) continue;
    count++;

    // Determine online status from MeshPeerHealth
    MeshPeerHealth* health = getMeshPeerHealth(gMeshPeerMeta[i].mac, false);
    bool alive = health ? isMeshPeerAlive(health) : false;
    uint32_t ageSec = health ? ((millis() - health->lastHeartbeatMs) / 1000) : 0;

    const char* displayName = gMeshPeerMeta[i].friendlyName[0]
      ? gMeshPeerMeta[i].friendlyName : gMeshPeerMeta[i].name;
    if (!displayName[0]) displayName = MAC_STR(gMeshPeerMeta[i].mac);

    pos += snprintf(buf + pos, 1024 - pos, "  %s%s [%s%s] %s",
                    displayName,
                    gMeshPeerMeta[i].room[0] ? "" : "",
                    gMeshPeerMeta[i].room[0] ? gMeshPeerMeta[i].room : "unassigned",
                    gMeshPeerMeta[i].zone[0] ? (String("/") + gMeshPeerMeta[i].zone).c_str() : "",
                    alive ? "(online" : "(offline");
    if (alive && health) {
      pos += snprintf(buf + pos, 1024 - pos, ", %lus ago", (unsigned long)ageSec);
    }
    pos += snprintf(buf + pos, 1024 - pos, ")\n");
    if (pos >= 900) { pos += snprintf(buf + pos, 1024 - pos, "  ... (truncated)\n"); break; }
  }

  if (count == 0) {
    pos += snprintf(buf + pos, 1024 - pos, "  (no peer metadata received yet)\n");
  }
  pos += snprintf(buf + pos, 1024 - pos, "Total: %d devices", count);
  return buf;
}

const char* cmd_espnow_rooms(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  char* buf = getDebugBuffer();
  int pos = 0;
  pos += snprintf(buf + pos, 1024 - pos, "=== Rooms ===\n");

  // Collect unique room names
  const char* rooms[MESH_PEER_MAX];
  int roomCount = 0;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (!gMeshPeerMeta[i].isActive) continue;
    const char* room = gMeshPeerMeta[i].room[0] ? gMeshPeerMeta[i].room : "Unassigned";
    bool found = false;
    for (int r = 0; r < roomCount; r++) {
      if (strcmp(rooms[r], room) == 0) { found = true; break; }
    }
    if (!found && roomCount < MESH_PEER_MAX) rooms[roomCount++] = room;
  }

  for (int r = 0; r < roomCount; r++) {
    pos += snprintf(buf + pos, 1024 - pos, "%s:\n", rooms[r]);
    for (int i = 0; i < gMeshPeerSlots; i++) {
      if (!gMeshPeerMeta[i].isActive) continue;
      const char* room = gMeshPeerMeta[i].room[0] ? gMeshPeerMeta[i].room : "Unassigned";
      if (strcmp(room, rooms[r]) != 0) continue;

      MeshPeerHealth* health = getMeshPeerHealth(gMeshPeerMeta[i].mac, false);
      bool alive = health ? isMeshPeerAlive(health) : false;

      const char* displayName = gMeshPeerMeta[i].friendlyName[0]
        ? gMeshPeerMeta[i].friendlyName : gMeshPeerMeta[i].name;
      if (!displayName[0]) displayName = MAC_STR(gMeshPeerMeta[i].mac);

      pos += snprintf(buf + pos, 1024 - pos, "  %s [%s] (%s)\n",
                      displayName,
                      gMeshPeerMeta[i].tags[0] ? gMeshPeerMeta[i].tags : "-",
                      alive ? "online" : "offline");
      if (pos >= 900) { pos += snprintf(buf + pos, 1024 - pos, "  ...\n"); break; }
    }
    if (pos >= 900) break;
  }

  if (roomCount == 0) {
    pos += snprintf(buf + pos, 1024 - pos, "  (no peer metadata received yet)");
  }
  return buf;
}

const char* cmd_espnow_find(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String query = argsIn;
  query.trim();
  if (query.length() == 0) return "Usage: espnow find <query> — search by name, room, or tag";

  query.toLowerCase();
  char* buf = getDebugBuffer();
  int pos = 0;
  int matches = 0;

  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (!gMeshPeerMeta[i].isActive) continue;

    // Check name, friendlyName, room, zone, tags (case-insensitive)
    String name = gMeshPeerMeta[i].name; name.toLowerCase();
    String friendly = gMeshPeerMeta[i].friendlyName; friendly.toLowerCase();
    String room = gMeshPeerMeta[i].room; room.toLowerCase();
    String zone = gMeshPeerMeta[i].zone; zone.toLowerCase();
    String tags = gMeshPeerMeta[i].tags; tags.toLowerCase();

    if (name.indexOf(query) >= 0 || friendly.indexOf(query) >= 0 ||
        room.indexOf(query) >= 0 || zone.indexOf(query) >= 0 ||
        tags.indexOf(query) >= 0) {
      matches++;
      const char* displayName = gMeshPeerMeta[i].friendlyName[0]
        ? gMeshPeerMeta[i].friendlyName : gMeshPeerMeta[i].name;
      pos += snprintf(buf + pos, 1024 - pos, "  %s [%s/%s] tags=%s\n",
                      displayName,
                      gMeshPeerMeta[i].room[0] ? gMeshPeerMeta[i].room : "-",
                      gMeshPeerMeta[i].zone[0] ? gMeshPeerMeta[i].zone : "-",
                      gMeshPeerMeta[i].tags[0] ? gMeshPeerMeta[i].tags : "-");
      if (pos >= 900) break;
    }
  }

  if (matches == 0) {
    snprintf(buf, 1024, "No devices matching '%s'", argsIn.c_str());
  }
  return buf;
}

const char* cmd_espnow_roomcmd(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsIn;
  args.trim();

  // Parse: <room> <user> <pass> <command>
  int s1 = args.indexOf(' ');
  if (s1 < 0) return "Usage: espnow roomcmd <room> <user> <pass> <command>";
  int s2 = args.indexOf(' ', s1 + 1);
  if (s2 < 0) return "Usage: espnow roomcmd <room> <user> <pass> <command>";
  int s3 = args.indexOf(' ', s2 + 1);
  if (s3 < 0) return "Usage: espnow roomcmd <room> <user> <pass> <command>";

  String targetRoom = args.substring(0, s1);
  String user = args.substring(s1 + 1, s2);
  String pass = args.substring(s2 + 1, s3);
  String command = args.substring(s3 + 1);
  command.trim();
  if (command.length() == 0) return "Usage: espnow roomcmd <room> <user> <pass> <command>";

  char* buf = getDebugBuffer();
  int pos = 0;
  int sent = 0;

  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (!gMeshPeerMeta[i].isActive) continue;
    if (!gMeshPeerMeta[i].room[0]) continue;

    String room = gMeshPeerMeta[i].room;
    if (!room.equalsIgnoreCase(targetRoom)) continue;

    // Call cmd_espnow_remote with: <mac> <user> <pass> <cmd>
    String mac = macToHexString(gMeshPeerMeta[i].mac);
    String remoteArgs = mac + " " + user + " " + pass + " " + command;
    cmd_espnow_remote(remoteArgs);
    sent++;

    const char* displayName = gMeshPeerMeta[i].friendlyName[0]
      ? gMeshPeerMeta[i].friendlyName : gMeshPeerMeta[i].name;
    pos += snprintf(buf + pos, 1024 - pos, "  -> %s (%s)\n", displayName, mac.c_str());
    if (pos >= 900) break;
  }

  if (sent == 0) {
    snprintf(buf, 1024, "No devices found in room '%s'", targetRoom.c_str());
  } else {
    pos += snprintf(buf + pos, 1024 - pos, "Sent '%s' to %d device(s) in %s",
                    command.c_str(), sent, targetRoom.c_str());
  }
  return buf;
}

const char* cmd_espnow_tagcmd(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsIn;
  args.trim();

  // Parse: <tag> <user> <pass> <command>
  int s1 = args.indexOf(' ');
  if (s1 < 0) return "Usage: espnow tagcmd <tag> <user> <pass> <command>";
  int s2 = args.indexOf(' ', s1 + 1);
  if (s2 < 0) return "Usage: espnow tagcmd <tag> <user> <pass> <command>";
  int s3 = args.indexOf(' ', s2 + 1);
  if (s3 < 0) return "Usage: espnow tagcmd <tag> <user> <pass> <command>";

  String targetTag = args.substring(0, s1);
  String user = args.substring(s1 + 1, s2);
  String pass = args.substring(s2 + 1, s3);
  String command = args.substring(s3 + 1);
  command.trim();
  if (command.length() == 0) return "Usage: espnow tagcmd <tag> <user> <pass> <command>";

  targetTag.toLowerCase();
  char* buf = getDebugBuffer();
  int pos = 0;
  int sent = 0;

  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (!gMeshPeerMeta[i].isActive) continue;
    if (!gMeshPeerMeta[i].tags[0]) continue;

    // Check if target tag is in the comma-separated list
    String tags = gMeshPeerMeta[i].tags;
    tags.toLowerCase();
    bool match = false;
    int start = 0;
    while (start < (int)tags.length()) {
      int end = tags.indexOf(',', start);
      if (end < 0) end = tags.length();
      String tag = tags.substring(start, end);
      tag.trim();
      if (tag == targetTag) { match = true; break; }
      start = end + 1;
    }
    if (!match) continue;

    String mac = macToHexString(gMeshPeerMeta[i].mac);
    String remoteArgs = mac + " " + user + " " + pass + " " + command;
    cmd_espnow_remote(remoteArgs);
    sent++;

    const char* displayName = gMeshPeerMeta[i].friendlyName[0]
      ? gMeshPeerMeta[i].friendlyName : gMeshPeerMeta[i].name;
    pos += snprintf(buf + pos, 1024 - pos, "  -> %s (%s)\n", displayName, mac.c_str());
    if (pos >= 900) break;
  }

  if (sent == 0) {
    snprintf(buf, 1024, "No devices found with tag '%s'", targetTag.c_str());
  } else {
    pos += snprintf(buf + pos, 1024 - pos, "Sent '%s' to %d device(s) with tag '%s'",
                    command.c_str(), sent, targetTag.c_str());
  }
  return buf;
}

// ESP-NOW heartbeat mode command
const char* cmd_espnow_hbmode(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsIn;
  args.trim();
  
  if (args.length() == 0) {
    const char* mode = gSettings.meshHeartbeatBroadcast ? "public" : "private";
    const char* desc = gSettings.meshHeartbeatBroadcast 
      ? "Heartbeats broadcast to all devices (discovery enabled)"
      : "Heartbeats sent only to paired devices (discovery disabled)";
    snprintf(getDebugBuffer(), 1024, "Heartbeat mode: %s\n%s", mode, desc);
    return getDebugBuffer();
  }
  
  args.toLowerCase();
  if (args == "public" || args == "broadcast") {
    setSetting(gSettings.meshHeartbeatBroadcast, true);
    return "Heartbeat mode set to public (broadcast). Unpaired devices can now be discovered.";
  } else if (args == "private" || args == "unicast") {
    setSetting(gSettings.meshHeartbeatBroadcast, false);
    return "Heartbeat mode set to private (unicast). Only paired devices will receive heartbeats.";
  }
  
  return "Usage: espnow hbmode [public|private]";
}

// Mesh role command
const char* cmd_espnow_meshrole(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsIn;
  args.trim();
  
  if (args.length() == 0) {
    String output = String("Mesh role: ") + getMeshRoleString(gSettings.meshRole);
    if (gSettings.meshMasterMAC.length() > 0) {
      output += String("\nMaster MAC: ") + gSettings.meshMasterMAC;
    }
    output += String("\nBackup enabled: ") + (gSettings.meshBackupEnabled ? "yes" : "no");
    if (gSettings.meshBackupEnabled && gSettings.meshBackupMAC.length() > 0) {
      output += String("\nBackup MAC: ") + gSettings.meshBackupMAC;
    }
    snprintf(getDebugBuffer(), 1024, "%s", output.c_str());
    return getDebugBuffer();
  }
  
  args.toLowerCase();
  if (args == "worker") {
    setSetting(gSettings.meshRole, (uint8_t)MESH_ROLE_WORKER);
    BROADCAST_PRINTF("[MESH] Role set to worker");
    return "Role set to worker";
  } else if (args == "master") {
    setSetting(gSettings.meshRole, (uint8_t)MESH_ROLE_MASTER);
    setSetting(gSettings.meshMasterMAC, String(""));
    BROADCAST_PRINTF("[MESH] Role set to master");
    return "Role set to master";
  } else if (args == "backup") {
    setSetting(gSettings.meshRole, (uint8_t)MESH_ROLE_BACKUP_MASTER);
    BROADCAST_PRINTF("[MESH] Role set to backup master");
    return "Role set to backup master";
  }
  
  return "Usage: espnow meshrole [worker|master|backup]";
}


// Worker status configuration
const char* cmd_espnow_worker(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  String args = argsIn;
  args.trim();
  
  // Show current configuration
  if (args.length() == 0 || args == "show") {
    snprintf(getDebugBuffer(), 1024,
             "Worker Status Config:\n"
             "  enabled: %s\n"
             "  interval: %u ms\n"
             "  fields: heap=%s rssi=%s thermal=%s imu=%s",
             gWorkerStatusConfig.enabled ? "on" : "off",
             gWorkerStatusConfig.intervalMs,
             gWorkerStatusConfig.includeHeap ? "on" : "off",
             gWorkerStatusConfig.includeRssi ? "on" : "off",
             gWorkerStatusConfig.includeThermal ? "on" : "off",
             gWorkerStatusConfig.includeImu ? "on" : "off");
    return getDebugBuffer();
  }
  
  // Enable/disable worker status
  if (args == "on" || args == "enable") {
    gWorkerStatusConfig.enabled = true;
    return "Worker status reporting enabled";
  }
  if (args == "off" || args == "disable") {
    gWorkerStatusConfig.enabled = false;
    return "Worker status reporting disabled";
  }
  
  // Set interval
  if (args.startsWith("interval ")) {
    String intervalStr = args.substring(9);
    intervalStr.trim();
    long interval = intervalStr.toInt();
    if (interval < 1000) return "Error: interval must be >= 1000 ms";
    if (interval > 300000) return "Error: interval must be <= 300000 ms (5 min)";
    gWorkerStatusConfig.intervalMs = (uint16_t)interval;
    snprintf(getDebugBuffer(), 1024, "Worker status interval set to %u ms", gWorkerStatusConfig.intervalMs);
    return getDebugBuffer();
  }
  
  // Configure fields
  if (args.startsWith("fields ")) {
    String fields = args.substring(7);
    fields.trim();
    fields.toLowerCase();
    
    // Reset all fields to off
    gWorkerStatusConfig.includeHeap = false;
    gWorkerStatusConfig.includeRssi = false;
    gWorkerStatusConfig.includeThermal = false;
    gWorkerStatusConfig.includeImu = false;
    
    // Parse comma-separated field list
    int start = 0;
    while (start < fields.length()) {
      int comma = fields.indexOf(',', start);
      String field = (comma >= 0) ? fields.substring(start, comma) : fields.substring(start);
      field.trim();
      
      if (field == "heap") gWorkerStatusConfig.includeHeap = true;
      else if (field == "rssi") gWorkerStatusConfig.includeRssi = true;
      else if (field == "thermal") gWorkerStatusConfig.includeThermal = true;
      else if (field == "imu") gWorkerStatusConfig.includeImu = true;
      else if (field.length() > 0) {
        snprintf(getDebugBuffer(), 1024, "Error: unknown field '%s'", field.c_str());
        return getDebugBuffer();
      }
      
      if (comma < 0) break;
      start = comma + 1;
    }
    
    return "Worker status fields updated";
  }
  
  return "Usage: espnow worker [show|on|off|interval <ms>|fields <heap,rssi,thermal,imu>]";
}

// V2 fragmentation is now mandatory - command removed

// Mesh master command
const char* cmd_espnow_meshmaster(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsIn;
  args.trim();
  
  if (args.length() == 0) {
    if (gSettings.meshMasterMAC.length() > 0) {
      BROADCAST_PRINTF("Master MAC: %s", gSettings.meshMasterMAC.c_str());
    } else {
      BROADCAST_PRINTF("No master assigned");
    }
    return "OK";
  }
  
  if (args.length() != 17) {
    return "Invalid MAC address format. Use: XX:XX:XX:XX:XX:XX";
  }
  
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String myMacStr = macToHexString(myMac);
  if (args.equalsIgnoreCase(myMacStr)) {
    return "Error: Cannot set your own MAC as master MAC";
  }
  
  { String upper = args; upper.toUpperCase(); setSetting(gSettings.meshMasterMAC, upper); }
  BROADCAST_PRINTF("[MESH] Master MAC set to %s", gSettings.meshMasterMAC.c_str());
  return "OK";
}

// Mesh backup command
const char* cmd_espnow_meshbackup(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsIn;
  args.trim();
  
  if (args.length() == 0) {
    if (gSettings.meshBackupMAC.length() > 0) {
      BROADCAST_PRINTF("Backup MAC: %s", gSettings.meshBackupMAC.c_str());
    } else {
      BROADCAST_PRINTF("No backup assigned");
    }
    return "OK";
  }
  
  if (args.length() != 17) {
    return "Invalid MAC address format. Use: XX:XX:XX:XX:XX:XX";
  }
  
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String myMacStr = macToHexString(myMac);
  if (args.equalsIgnoreCase(myMacStr)) {
    return "Error: Cannot set your own MAC as backup MAC";
  }
  
  { String upper = args; upper.toUpperCase(); setSetting(gSettings.meshBackupMAC, upper); }
  BROADCAST_PRINTF("[MESH] Backup MAC set to %s", gSettings.meshBackupMAC.c_str());
  return "OK";
}

// Backup master enable/disable command
const char* cmd_espnow_backupenable(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsIn;
  args.trim();
  args.toLowerCase();

  if (args.length() == 0) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024, "Backup master: %s", gSettings.meshBackupEnabled ? "enabled" : "disabled");
    return getDebugBuffer();
  }

  bool enable = (args == "on" || args == "1" || args == "true" || args == "enable");
  bool disable = (args == "off" || args == "0" || args == "false" || args == "disable");
  if (!enable && !disable) {
    return "Usage: espnow backupenable [on|off]";
  }

  setSetting(gSettings.meshBackupEnabled, enable);
  return enable ? "Backup master enabled" : "Backup master disabled";
}

// Mesh topology discovery command
const char* cmd_espnow_meshtopo(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!meshEnabled()) {
    return "Mesh mode not enabled. Use 'espnow mode mesh' first.";
  }
  
  int peerCount = 0;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac)) {
      peerCount++;
    }
  }
  
  if (peerCount == 0) {
    esp_now_peer_num_t peerNum;
    esp_now_get_peer_num(&peerNum);
    int pairedCount = peerNum.total_num;
    
    if (pairedCount > 0) {
      broadcastOutput("No mesh peers discovered yet.");
      BROADCAST_PRINTF("You have %d paired device(s), but they haven't sent heartbeats.", pairedCount);
      broadcastOutput("Mesh peers are auto-discovered when devices send heartbeats.");
      broadcastOutput("Ensure paired devices are powered on and in mesh mode.");
    } else {
      broadcastOutput("No mesh peers available.");
      broadcastOutput("Pair devices using 'espnow pair' or 'espnow pairsecure' first.");
    }
    return "ERROR";
  }
  
  BROADCAST_PRINTF("[TOPO] Initiating topology discovery for %d peer(s)...", peerCount);
  requestTopologyDiscovery();
  return "Topology discovery initiated. Use 'espnow toporesults' to view responses.";
}

// Time sync command
const char* cmd_espnow_timesync(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!meshEnabled()) {
    return "Mesh mode not enabled. Use 'espnow mode mesh' first.";
  }
  
  uint32_t epoch = (uint32_t)time(nullptr);
  if (epoch < 100000) {
    return "No valid NTP time available. Ensure WiFi is connected and NTP is synced.";
  }
  
  DEBUG_ESPNOWF("[TIME_SYNC] Broadcasting time sync: epoch=%lu", (unsigned long)epoch);
  
  // Use V3 binary protocol instead of V2 JSON
  uint32_t millisAtEpoch = millis();
  bool sent = v3_broadcast_time_sync(epoch, millisAtEpoch);
  
  if (sent) {
    BROADCAST_PRINTF("Time sync broadcast sent (epoch: %lu)", (unsigned long)epoch);
    return "OK";
  } else {
    return "Failed to send time sync";
  }
}

// Time status command
const char* cmd_espnow_timestatus(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "ERROR";
  
  if (gTimeIsSynced) {
    uint32_t epoch = (uint32_t)time(nullptr);
    uint32_t secondsSinceSync = (millis() - gLastTimeSyncMs) / 1000;
    snprintf(getDebugBuffer(), 1024, 
             "Time Status:\n  Synced: Yes\n  Epoch: %lu\n  Last sync: %lu seconds ago",
             (unsigned long)epoch, (unsigned long)secondsSinceSync);
  } else {
    snprintf(getDebugBuffer(), 1024, "Time Status:\n  Synced: No\n  Use 'espnow timesync' on master to sync");
  }
  
  return getDebugBuffer();
}

// Mesh save command
const char* cmd_espnow_meshsave(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!meshEnabled()) {
    return "Mesh mode not enabled.";
  }
  
  saveMeshPeers();
  return "Mesh peer topology saved to filesystem.";
}

// Topology results command
const char* cmd_espnow_toporesults(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  uint32_t now = millis();
  bool collectionActive = (gTopoRequestId != 0 && now < gTopoRequestTimeout);
  bool withinCollectionWindow = (gTopoLastResponseTime > 0 && 
                                  (now - gTopoLastResponseTime) < TOPO_COLLECTION_WINDOW_MS);
  
  if (collectionActive && (withinCollectionWindow || gTopoLastResponseTime == 0)) {
    return "WAIT";
  }
  
  if (gTopoResultsBuffer.length() == 0) {
    return "ERROR";
  }
  
  static char* topoOutputBuffer = nullptr;
  if (!topoOutputBuffer) {
    topoOutputBuffer = (char*)ps_alloc(2048, AllocPref::PreferPSRAM, "topo.output");
    if (!topoOutputBuffer) {
      broadcastOutput("Memory allocation failed for topology output");
      return "ERROR";
    }
  }
  
  char* p = topoOutputBuffer;
  size_t remaining = 2048;
  int written = 0;
  
  written = snprintf(p, remaining, "\n=== Mesh Topology Discovery Results ===\nResponses received: %d\nRequest ID: %lu\n\n",
                     gTopoResponsesReceived, (unsigned long)gTopoRequestId);
  if (written > 0 && (size_t)written < remaining) {
    p += written;
    remaining -= written;
  }
  
  if (gTopoResultsBuffer.length() < remaining - 50) {
    written = snprintf(p, remaining, "%s\n", gTopoResultsBuffer.c_str());
    if (written > 0 && (size_t)written < remaining) {
      p += written;
      remaining -= written;
    }
  }
  
  snprintf(p, remaining, "=======================================\n\nChain Interpretation:\n  Devices with mutual peer connections form a chain.\n  Example: If A lists B as peer, and B lists A and C,\n  then the chain is: A ↔ B ↔ C\n");
  
  // Note: result is returned via topoOutputBuffer to the HTTP caller.
  // No broadcastOutput here — avoids serial spam when the web UI polls repeatedly.
  
  return topoOutputBuffer;
}

// ============================================================================
// ESP-NOW Test Commands
// ============================================================================

// Test stream management
const char* cmd_test_streams(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  broadcastOutput("\n=== Testing Stream Management ===");
  
  uint8_t fakeMac1[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  uint8_t fakeMac2[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
  uint8_t fakeMac3[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x03};
  
  BROADCAST_PRINTF("Creating stream 1 (MAC: aa:bb:cc:dd:ee:01, reqId: 100)");
  TopologyStream* s1 = findOrCreateTopoStream(fakeMac1, 100);
  BROADCAST_PRINTF("  Result: %p, active=%d", s1, s1 ? s1->active : 0);
  
  BROADCAST_PRINTF("Creating stream 2 (MAC: aa:bb:cc:dd:ee:02, reqId: 200)");
  TopologyStream* s2 = findOrCreateTopoStream(fakeMac2, 200);
  BROADCAST_PRINTF("  Result: %p, active=%d", s2, s2 ? s2->active : 0);
  
  BROADCAST_PRINTF("Creating stream 3 (MAC: aa:bb:cc:dd:ee:03, reqId: 300)");
  TopologyStream* s3 = findOrCreateTopoStream(fakeMac3, 300);
  BROADCAST_PRINTF("  Result: %p, active=%d", s3, s3 ? s3->active : 0);
  
  BROADCAST_PRINTF("\nTesting findTopoStream for stream 1:");
  TopologyStream* s1_again = findTopoStream(fakeMac1, 100);
  BROADCAST_PRINTF("  Found same pointer: %s", s1 == s1_again ? "YES" : "NO");
  
  BROADCAST_PRINTF("\nTesting findTopoStream for non-existent stream:");
  TopologyStream* s_none = findTopoStream(fakeMac1, 999);
  BROADCAST_PRINTF("  Result: %s", s_none ? "FOUND (ERROR!)" : "NULL (correct)");
  
  broadcastOutput("\nActive streams:");
  int activeCount = 0;
  for (int i = 0; i < MAX_CONCURRENT_TOPO_STREAMS; i++) {
    if (gTopoStreams[i].active) {
      activeCount++;
      BROADCAST_PRINTF("  Slot %d: reqId=%lu, MAC=%s", 
                      i, (unsigned long)gTopoStreams[i].reqId,
                      MAC_STR(gTopoStreams[i].senderMac));
    }
  }
  BROADCAST_PRINTF("Total active streams: %d/%d", activeCount, MAX_CONCURRENT_TOPO_STREAMS);
  
  broadcastOutput("\n=== Test Complete ===");
  return "OK";
}

// Test concurrent streams
const char* cmd_test_concurrent(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  broadcastOutput("\n=== Testing Concurrent Streams (Simulated) ===");
  
  uint8_t mac1[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01};
  uint8_t mac2[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x02};
  uint8_t mac3[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x03};
  
  gTopoRequestId = 12345;
  gTopoRequestTimeout = millis() + 10000;
  gTopoResultsBuffer = "";
  gTopoResponsesReceived = 0;
  
  BROADCAST_PRINTF("Simulating topology request (reqId=%lu)", (unsigned long)gTopoRequestId);
  
  BROADCAST_PRINTF("\nDevice 1 (2 peers):");
  TopologyStream* s1 = findOrCreateTopoStream(mac1, 12345);
  strcpy(s1->senderName, "TestDevice1");
  s1->totalPeers = 2;
  s1->receivedPeers = 2;
  s1->accumulatedData = "  → Peer1 (aa:bb:cc:dd:ee:11)\n    Heartbeats: 10, Last seen: 5s ago\n";
  s1->accumulatedData += "  → Peer2 (aa:bb:cc:dd:ee:12)\n    Heartbeats: 8, Last seen: 3s ago\n";
  finalizeTopologyStream(s1);
  BROADCAST_PRINTF("  Finalized");
  
  BROADCAST_PRINTF("\nDevice 2 (1 peer):");
  TopologyStream* s2 = findOrCreateTopoStream(mac2, 12345);
  strcpy(s2->senderName, "TestDevice2");
  s2->totalPeers = 1;
  s2->receivedPeers = 1;
  s2->accumulatedData = "  → Peer1 (aa:bb:cc:dd:ee:21)\n    Heartbeats: 15, Last seen: 2s ago\n";
  finalizeTopologyStream(s2);
  BROADCAST_PRINTF("  Finalized");
  
  BROADCAST_PRINTF("\nDevice 3 (0 peers):");
  TopologyStream* s3 = findOrCreateTopoStream(mac3, 12345);
  strcpy(s3->senderName, "TestDevice3");
  s3->totalPeers = 0;
  s3->receivedPeers = 0;
  finalizeTopologyStream(s3);
  BROADCAST_PRINTF("  Finalized");
  
  BROADCAST_PRINTF("\n=== Simulation Complete ===");
  BROADCAST_PRINTF("Results buffer length: %d bytes", gTopoResultsBuffer.length());
  BROADCAST_PRINTF("Responses received: %d", gTopoResponsesReceived);
  broadcastOutput("\nRun 'espnow toporesults' to view the simulated results");
  
  return "OK";
}

// Test cleanup
const char* cmd_test_cleanup(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  broadcastOutput("\n=== Testing Stream Cleanup ===");
  
  int activeBefore = 0;
  for (int i = 0; i < MAX_CONCURRENT_TOPO_STREAMS; i++) {
    if (gTopoStreams[i].active) {
      activeBefore++;
      BROADCAST_PRINTF("Before: Slot %d active (reqId=%lu, age=%lums)", 
                      i, (unsigned long)gTopoStreams[i].reqId,
                      millis() - gTopoStreams[i].startTime);
    }
  }
  BROADCAST_PRINTF("Active streams before cleanup: %d", activeBefore);
  
  broadcastOutput("\nRunning cleanupStaleTopoStreams()...");
  cleanupStaleTopoStreams();
  
  int activeAfter = 0;
  for (int i = 0; i < MAX_CONCURRENT_TOPO_STREAMS; i++) {
    if (gTopoStreams[i].active) {
      activeAfter++;
      BROADCAST_PRINTF("After: Slot %d still active (reqId=%lu, age=%lums)", 
                      i, (unsigned long)gTopoStreams[i].reqId,
                      millis() - gTopoStreams[i].startTime);
    }
  }
  BROADCAST_PRINTF("Active streams after cleanup: %d", activeAfter);
  BROADCAST_PRINTF("Cleaned up: %d streams", activeBefore - activeAfter);
  
  broadcastOutput("\n=== Cleanup Test Complete ===");
  return "OK";
}

// acquireFileTransferLock and releaseFileTransferLock removed - unused

// Test file lock
const char* cmd_test_filelock(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  broadcastOutput("\n=== Testing File Transfer Lock ===");
  
  uint8_t testMac[6] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  
  // Check for stale locks (30 second timeout)
  const unsigned long LOCK_TIMEOUT_MS = 30000;
  if (gFileTransferLocked && (millis() - gFileTransferLockTime > LOCK_TIMEOUT_MS)) {
    broadcastOutput("[WARN] Stale lock detected (>30s), auto-releasing...");
    gFileTransferLocked = false;
    memset(gFileTransferOwnerMac, 0, 6);
  }
  
  BROADCAST_PRINTF("Lock status: %s", gFileTransferLocked ? "LOCKED" : "FREE");
  
  if (gFileTransferLocked) {
    BROADCAST_PRINTF("Lock owner: %s", MAC_STR(gFileTransferOwnerMac));
    BROADCAST_PRINTF("Lock age: %lums", millis() - gFileTransferLockTime);
  }
  
  if (!gFileTransferLocked) {
    broadcastOutput("\nAcquiring lock...");
    gFileTransferLocked = true;
    memcpy(gFileTransferOwnerMac, testMac, 6);
    gFileTransferLockTime = millis();
    BROADCAST_PRINTF("✓ Lock acquired by: %s", MAC_STR(gFileTransferOwnerMac));
  } else {
    broadcastOutput("\nLock already held, releasing...");
    gFileTransferLocked = false;
    memset(gFileTransferOwnerMac, 0, 6);
    broadcastOutput("✓ Lock released");
  }
  
  broadcastOutput("\n=== File Lock Test Complete ===");
  broadcastOutput("Run again to toggle lock state");
  return "OK";
}

// ESP-NOW Device Management Commands
// ============================================================================

// List paired devices
const char* cmd_espnow_list(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "{\"error\":\"ESP-NOW not initialized\"}";
  if (!gEspNow->initialized) {
    return "{\"error\":\"ESP-NOW not initialized\"}";
  }

  // Build JSON from gEspNow->devices[] — the authoritative source of truth.
  // esp_now_fetch_peer() only reflects the hardware peer table which may lag
  // behind after a reboot until loadMeshPeers() re-registers all peers.
  DynamicJsonDocument doc(2048);
  JsonArray devices = doc.createNestedArray("devices");

  int listedCount = 0;
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (isSelfMac(gEspNow->devices[i].mac)) continue;  // Don't list self
    JsonObject d = devices.createNestedObject();
    d["mac"]       = formatMacAddress(gEspNow->devices[i].mac);
    d["name"]      = gEspNow->devices[i].name;
    d["encrypted"] = gEspNow->devices[i].encrypted;
    listedCount++;
  }

  doc["count"] = listedCount;

  size_t needed = measureJson(doc) + 1;
  static const size_t bufSize = 1024;
  if (needed > bufSize) needed = bufSize;
  if (!gEspNow->listBuffer) return "{}";
  serializeJson(doc, gEspNow->listBuffer, needed);

  DEBUGF(DEBUG_HTTP, "[ESPNOW] list: %d devices", gEspNow->deviceCount);
  return gEspNow->listBuffer;
}

// Mesh status command
const char* cmd_espnow_meshstatus(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "{\"error\":\"ESP-NOW not initialized\"}";
  if (!gEspNow->initialized) {
    return "{\"error\":\"ESP-NOW not initialized\"}";
  }

  if (!meshEnabled()) {
    return "{\"error\":\"Mesh mode not enabled\"}";
  }

  // Use ArduinoJson to avoid String concatenation heap fragmentation
  // Use DynamicJsonDocument to avoid stack overflow in cmd_exec task
  DynamicJsonDocument doc(2048);
  JsonArray peers = doc.createNestedArray("peers");
  
  uint32_t now = millis();
  int activePeers = 0;

  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (!gMeshPeers[i].isActive || isSelfMac(gMeshPeers[i].mac)) continue;

    JsonObject peer = peers.createNestedObject();
    String deviceName = getEspNowDeviceName(gMeshPeers[i].mac);
    uint32_t elapsed = now - gMeshPeers[i].lastHeartbeatMs;
    if (elapsed > 0x80000000UL) elapsed = 0;
    bool alive = isMeshPeerAlive(&gMeshPeers[i]);

    peer["mac"] = macToHexString(gMeshPeers[i].mac);
    peer["name"] = deviceName.length() > 0 ? deviceName : "Unknown";
    peer["alive"] = alive;
    peer["lastHeartbeat"] = gMeshPeers[i].lastHeartbeatMs;
    peer["lastAck"] = gMeshPeers[i].lastAckMs;
    peer["heartbeatCount"] = gMeshPeers[i].heartbeatCount;
    peer["ackCount"] = gMeshPeers[i].ackCount;
    peer["secondsSinceHeartbeat"] = elapsed / 1000;

    activePeers++;
  }

  doc["totalPeers"] = activePeers;

  JsonArray unpaired = doc.createNestedArray("unpaired");
  int unpairedCount = 0;
  
  for (int i = 0; i < gEspNow->unpairedDeviceCount; i++) {
    if (isPairedDevice(gEspNow->unpairedDevices[i].mac)) continue;
    
    JsonObject dev = unpaired.createNestedObject();
    uint32_t elapsed = now - gEspNow->unpairedDevices[i].lastSeenMs;
    
    dev["mac"] = macToHexString(gEspNow->unpairedDevices[i].mac);
    dev["name"] = gEspNow->unpairedDevices[i].name.length() > 0 ? gEspNow->unpairedDevices[i].name : "Unknown";
    dev["rssi"] = gEspNow->unpairedDevices[i].rssi;
    dev["heartbeatCount"] = gEspNow->unpairedDevices[i].heartbeatCount;
    dev["secondsSinceLastSeen"] = elapsed / 1000;
    
    unpairedCount++;
  }
  
  doc["totalUnpaired"] = unpairedCount;

  JsonArray retryQueue = doc.createNestedArray("retryQueue");
  int activeRetries = 0;
  
  if (gMeshRetryMutex && xSemaphoreTake(gMeshRetryMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    for (int i = 0; i < MESH_RETRY_QUEUE_SIZE; i++) {
      if (!gMeshRetryQueue[i].active) continue;
      
      JsonObject retry = retryQueue.createNestedObject();
      uint32_t elapsed = now - gMeshRetryQueue[i].sentMs;
      
      retry["msgId"] = gMeshRetryQueue[i].msgId;
      retry["dst"] = formatMacAddress(gMeshRetryQueue[i].dstMac);
      retry["retryCount"] = gMeshRetryQueue[i].retryCount;
      retry["secondsWaiting"] = elapsed / 1000;
      
      activeRetries++;
    }
    xSemaphoreGive(gMeshRetryMutex);
  }
  
  doc["activeRetries"] = activeRetries;

  // Serialize to gDebugBuffer
  if (!ensureDebugBuffer()) return "{\"error\":\"Buffer unavailable\"}";
  
  size_t len = serializeJson(doc, getDebugBuffer(), 1024);
  if (len >= 1024) return "{\"error\":\"Response too large\"}";
  
  return getDebugBuffer();
}

// Unpair device command
const char* cmd_espnow_unpair(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "ESP-NOW not initialized. Run 'openespnow' first.";
  }

  String target = argsIn;
  target.trim();
  
  if (target.length() == 0) {
    return "Usage: espnow unpair <name_or_mac>";
  }

  uint8_t mac[6];
  if (!resolveDeviceNameOrMac(target, mac)) {
    static char errBuf[256];
    snprintf(errBuf, sizeof(errBuf), 
             "Device '%s' not found. Use 'espnow devices' to see paired devices.", 
             target.c_str());
    return errBuf;
  }

  String deviceName = getEspNowDeviceName(mac);

  esp_err_t result = esp_now_del_peer(mac);
  if (result != ESP_OK) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024, "Failed to unpair device: %d", result);
    return getDebugBuffer();
  }

  removeEspNowDevice(mac);

  if (meshEnabled()) {
    for (int i = 0; i < gMeshPeerSlots; i++) {
      if (gMeshPeers[i].isActive && macEqual6(gMeshPeers[i].mac, mac)) {
        gMeshPeers[i].isActive = false;
        DEBUG_ESPNOWF("[MESH] Removed peer from mesh list: %s", MAC_STR(mac));
        break;
      }
    }
    saveMeshPeers();
  }

  saveMeshPeers();
  saveEspNowDevices();

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  if (deviceName.length() > 0) {
    snprintf(getDebugBuffer(), 1024, "Unpaired device: %s (%s)",
             deviceName.c_str(), formatMacAddress(mac).c_str());
  } else {
    snprintf(getDebugBuffer(), 1024, "Unpaired device: %s",
             formatMacAddress(mac).c_str());
  }
  return getDebugBuffer();
}

// ============================================================================
// ESP-NOW Messaging Commands
// ============================================================================

// Broadcast command
const char* cmd_espnow_broadcast(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "ESP-NOW not initialized. Run 'openespnow' first.";
  }

  String message = argsIn;
  message.trim();

  if (message.length() == 0) {
    return "Usage: espnow broadcast <message>";
  }

  // Build v2 JSON TEXT message for plain text
  String payload;
  if (message.startsWith("{")) {
    // Already JSON, send as-is
    payload = message;
  } else {
    // Plain text - send directly via V3
    payload = message;
  }

  // Send to all mesh peers via V3 broadcast
  bool result = v3_broadcast_text(payload.c_str(), payload.length());
  int sent = result ? 1 : 0;
  int failed = result ? 0 : 1;

  if (sent == 0 && failed == 0) {
    return "No paired devices to broadcast to";
  }

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  if (failed > 0) {
    snprintf(getDebugBuffer(), 1024, "Broadcast sent to %d device(s) (%d failed)", sent, failed);
  } else {
    snprintf(getDebugBuffer(), 1024, "Broadcast sent to %d device(s)", sent);
  }
  return getDebugBuffer();
}

// Helper function to send a file to a specific MAC address via v3 binary protocol
// Used by FILE_BROWSE fetch and other internal functions
bool sendFileToMac(const uint8_t* mac, const String& localPath) {
  if (!gEspNow || !gEspNow->initialized) {
    return false;
  }

  // Security: Block sending sensitive files (credentials, passwords, keys)
  if (!canRead(localPath)) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FILE_TX] SECURITY: Blocked sending sensitive file: %s", localPath.c_str());
    broadcastOutput("[ESP-NOW] SECURITY: Cannot send file containing credentials: " + localPath);
    return false;
  }

  {
    FsLockGuard guard("espnow.send_file.exists");
    if (!LittleFS.exists(localPath)) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FILE_TX] File not found: %s", localPath.c_str());
      return false;
    }
  }

  FsLockGuard fsGuard("espnow.send_file.open");
  File file = LittleFS.open(localPath, "r");
  if (!file) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FILE_TX] Cannot open file: %s", localPath.c_str());
    return false;
  }
  
  uint32_t fileSize = file.size();
  
  // V3 chunk size is 224 bytes (ESPNOW_V3_MAX_PAYLOAD - 2 for chunkIndex)
  const uint16_t v3ChunkSize = ESPNOW_V3_MAX_PAYLOAD - 2;
  uint32_t maxFileSize = 65535 * v3ChunkSize;  // 16-bit chunk count max
  if (fileSize > maxFileSize) {
    file.close();
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FILE_TX] File too large: %lu bytes (max %lu)", 
           (unsigned long)fileSize, (unsigned long)maxFileSize);
    return false;
  }
  
  String filename = localPath;
  int lastSlash = localPath.lastIndexOf('/');
  if (lastSlash >= 0) {
    filename = localPath.substring(lastSlash + 1);
  }
  
  uint16_t totalChunks = (fileSize > 0) ? (uint16_t)((fileSize + v3ChunkSize - 1) / v3ChunkSize) : 0;
  
  uint32_t transferId = generateMessageId();
  
  // Build and send FILE_START
  V3PayloadFileStart startPayload = {};
  startPayload.fileSize = fileSize;
  startPayload.chunkCount = totalChunks;
  startPayload.chunkSize = v3ChunkSize;
  strncpy(startPayload.filename, filename.c_str(), sizeof(startPayload.filename) - 1);
  
  if (!v3_send_frame(mac, ESPNOW_V3_TYPE_FILE_START, ESPNOW_V3_FLAG_ACK_REQ, transferId,
                     (const uint8_t*)&startPayload, sizeof(startPayload), 1)) {
    file.close();
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FILE_TX] Failed to send FILE_START");
    return false;
  }
  
  DEBUG_ESPNOWF("[V3_FILE_TX] START: %s (%lu bytes, %u chunks, chunkSize=%u) to %s, transferId=%lu",
         filename.c_str(), (unsigned long)fileSize, totalChunks, v3ChunkSize, formatMacAddress(mac).c_str(), (unsigned long)transferId);
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FILE_TX] Transfer initiated: %s -> %s", filename.c_str(), formatMacAddress(mac).c_str());
  
  vTaskDelay(pdMS_TO_TICKS(100));  // Give receiver more time to set up
  
  // Send chunks - use stack buffer sized for v3 payload
  uint8_t chunkPayload[ESPNOW_V3_MAX_PAYLOAD];
  uint16_t chunkIdx = 0;
  while (file.available() && chunkIdx < totalChunks) {
    V3PayloadFileData* fd = (V3PayloadFileData*)chunkPayload;
    fd->chunkIndex = chunkIdx;
    
    int bytesRead = file.read(fd->data, v3ChunkSize);
    if (bytesRead <= 0) break;
    
    uint16_t payloadLen = 2 + bytesRead;  // chunkIndex (2) + data
    
    bool sent = false;
    for (int attempt = 0; attempt < 3 && !sent; attempt++) {
      sent = v3_send_frame(mac, ESPNOW_V3_TYPE_FILE_DATA, 0, transferId,
                           chunkPayload, payloadLen, 1);
      if (!sent) {
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[V3_FILE_TX] Chunk %u send failed, retry %d", chunkIdx, attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(20));
      } else {
        DEBUG_ESPNOWF("[V3_FILE_TX] Chunk %u sent: %u bytes (attempt %d)", chunkIdx, payloadLen, attempt + 1);
      }
    }
    
    if (!sent) {
      ERROR_ESPNOWF("[V3_FILE_TX] Chunk %u failed after 3 retries", chunkIdx);
    }
    
    chunkIdx++;
    
    // Pace chunks - SLOWER for reliability (ESP-NOW can drop packets if sent too fast)
    vTaskDelay(pdMS_TO_TICKS(15));
    
    // Yield every 10 chunks with longer delay for receiver to process
    if ((chunkIdx % 10) == 0) {
      DEBUG_ESPNOWF("[V3_FILE_TX] Progress: %u/%u chunks sent", chunkIdx, totalChunks);
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
  file.close();
  
  // Small delay before FILE_END to ensure last chunks are processed
  vTaskDelay(pdMS_TO_TICKS(100));
  
  // Send FILE_END with retries for reliability
  V3PayloadFileEnd endPayload = {};
  endPayload.crc32 = 0;  // CRC not implemented yet
  endPayload.success = 1;
  
  bool endSent = false;
  for (int attempt = 0; attempt < 3 && !endSent; attempt++) {
    endSent = v3_send_frame(mac, ESPNOW_V3_TYPE_FILE_END, ESPNOW_V3_FLAG_ACK_REQ, transferId,
                            (const uint8_t*)&endPayload, sizeof(endPayload), 1);
    if (!endSent) {
      WARN_ESPNOWF("[V3_FILE_TX] FILE_END send failed, retry %d", attempt + 1);
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
  
  DEBUG_ESPNOWF("[V3_FILE_TX] COMPLETE: %s (%u chunks) to %s, END_sent=%d", 
         filename.c_str(), chunkIdx, formatMacAddress(mac).c_str(), endSent);
  return true;
}

#if ENABLE_BONDED_MODE

// Check if bond mode is active and peer is online
// NOTE: This function may be called from ISR context - NO debug prints here!
bool isBondModeOnline() {
  if (!gEspNow || !gEspNow->initialized) return false;
  if (!gSettings.bondModeEnabled) return false;
  if (gSettings.bondPeerMac.length() == 0) return false;
  return gEspNow->bondPeerOnline;
}

// Check if bond mode is active, peer is online, AND fully synced.
// Master: pulled cap + manifest + settings FROM worker.
// Worker: exchanged caps + sent settings TO master (worker never receives manifest/settings).
bool isBondSynced() {
  if (!isBondModeOnline() || !gEspNow->lastRemoteCapValid) return false;
  if (gSettings.bondRole == 1) {
    // Master: pulled everything from worker
    return gEspNow->bondManifestReceived && gEspNow->bondSettingsReceived;
  } else {
    // Worker: responded to master's requests
    return gEspNow->bondCapSent && gEspNow->bondSettingsSent;
  }
}

// Send binary sensor data to bonded master via v3 protocol
// sensorType: RemoteSensorType enum value
// data: JSON-encoded sensor data (will be sent as-is)
// dataLen: length of data
static uint32_t gBondSensorSeqNum = 0;

bool sendBondedSensorData(uint8_t sensorType, const uint8_t* data, uint16_t dataLen) {
  if (!isBondModeOnline()) return false;
  
  // Only workers should send sensor data to master
  if (gSettings.bondRole == 1) return false;
  
  // Get peer MAC
  uint8_t peerMac[6];
  if (!parseMacAddress(gSettings.bondPeerMac, peerMac)) return false;
  
  // Build payload: header + data
  // V3PayloadSensorData is 8 bytes header, max payload is 226, so max data is 218 bytes
  const uint16_t maxDataLen = ESPNOW_V3_MAX_PAYLOAD - sizeof(V3PayloadSensorData);
  if (dataLen > maxDataLen) {
    DEBUGF(DEBUG_ESPNOW_MESH, "[V3_SENSOR_TX] Data too large: %u > %u", dataLen, maxDataLen);
    return false;
  }
  
  // Allocate payload on stack
  uint8_t payloadBuf[ESPNOW_V3_MAX_PAYLOAD];
  V3PayloadSensorData* sd = (V3PayloadSensorData*)payloadBuf;
  sd->sensorType = sensorType;
  sd->flags = 0x01;  // Valid flag
  sd->dataLen = dataLen;
  sd->seqNum = ++gBondSensorSeqNum;
  
  if (data && dataLen > 0) {
    memcpy(sd->data, data, dataLen);
  }
  
  uint16_t totalLen = sizeof(V3PayloadSensorData) + dataLen;
  uint32_t msgId = generateMessageId();
  
  bool sent = v3_send_frame(peerMac, ESPNOW_V3_TYPE_SENSOR_DATA, 0, msgId, payloadBuf, totalLen, 1);
  
  // Single concise debug line only on success/failure
  DEBUGF(DEBUG_ESPNOW_MESH, "[BOND] Sensor TX type=%u len=%u seq=%lu %s",
         sensorType, dataLen, (unsigned long)sd->seqNum, sent ? "OK" : "FAIL");
  
  return sent;
}

// Send stream control message to bonded peer (master -> worker)
bool sendBondStreamCtrl(RemoteSensorType sensorType, bool enable) {
  if (!isBondSynced()) return false;
  
  // Only master sends stream control to worker
  if (gSettings.bondRole != 1) return false;
  
  uint8_t peerMac[6];
  if (!parseMacAddress(gSettings.bondPeerMac, peerMac)) return false;
  
  V3PayloadStreamCtrl ctrl = {};
  ctrl.sensorType = (uint8_t)sensorType;
  ctrl.enable = enable ? 1 : 0;
  
  uint32_t msgId = generateMessageId();
  bool sent = v3_send_frame(peerMac, ESPNOW_V3_TYPE_STREAM_CTRL, ESPNOW_V3_FLAG_ACK_REQ, 
                            msgId, (const uint8_t*)&ctrl, sizeof(ctrl), 1);
  
  DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_STREAM_CTRL] TX %s %s -> %s",
         sensorTypeToString(sensorType), enable ? "ON" : "OFF", sent ? "OK" : "FAIL");
  return sent;
}

#endif // ENABLE_BONDED_MODE

// Sendfile command
const char* cmd_espnow_sendfile(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "ESP-NOW not initialized. Run 'openespnow' first.";
  }

  String args = argsIn;
  args.trim();

  int firstSpace = args.indexOf(' ');
  if (firstSpace < 0) return "Usage: espnow sendfile <name_or_mac> <filepath>";

  String target = args.substring(0, firstSpace);
  String filepath = args.substring(firstSpace + 1);

  target.trim();
  filepath.trim();

  if (target.length() == 0 || filepath.length() == 0) {
    return "Usage: espnow sendfile <name_or_mac> <filepath>";
  }

  uint8_t mac[6];
  if (!resolveDeviceNameOrMac(target, mac)) {
    static char errBuf[256];
    snprintf(errBuf, sizeof(errBuf), 
             "Device '%s' not found. Use 'espnow devices' to see paired devices.", 
             target.c_str());
    return errBuf;
  }

  String deviceName = getEspNowDeviceName(mac);
  if (deviceName.length() == 0) {
    deviceName = formatMacAddress(mac);
  }

  static char sendfileBuffer[512];

  if (isMeshMode()) {
    if (!espnowPeerExists(mac)) {
      BROADCAST_PRINTF("[ESP-NOW][mesh] file send rejected: no peer entry MAC=%s", formatMacAddress(mac).c_str());
      snprintf(sendfileBuffer, sizeof(sendfileBuffer), "Rejected (mesh): destination not in ESP-NOW peer table.");
      return sendfileBuffer;
    }
    BROADCAST_PRINTF("[ESP-NOW][mesh] file send accepted MAC=%s", formatMacAddress(mac).c_str());
  }

  if (!LittleFS.exists(filepath)) {
    snprintf(sendfileBuffer, sizeof(sendfileBuffer), "Error: File not found: %s", filepath.c_str());
    return sendfileBuffer;
  }

  // Get file info for reporting
  File file = LittleFS.open(filepath, "r");
  if (!file) {
    snprintf(sendfileBuffer, sizeof(sendfileBuffer), "Error: Cannot open file: %s", filepath.c_str());
    return sendfileBuffer;
  }
  uint32_t fileSize = file.size();
  file.close();

  String filename = filepath;
  int lastSlash = filepath.lastIndexOf('/');
  if (lastSlash >= 0) {
    filename = filepath.substring(lastSlash + 1);
  }

  BROADCAST_PRINTF("[ESP-NOW] Sending file to %s: %s (%lu bytes) via v3", deviceName.c_str(), filename.c_str(), (unsigned long)fileSize);

  // Use unified v3 file transfer
  if (!sendFileToMac(mac, filepath)) {
    snprintf(sendfileBuffer, sizeof(sendfileBuffer), "Error: Failed to send file via v3");
    return sendfileBuffer;
  }

  gEspNow->fileTransfersSent++;
  logFileTransferEvent(mac, deviceName.c_str(), filename.c_str(), MSG_FILE_SEND_SUCCESS);
  
  snprintf(sendfileBuffer, sizeof(sendfileBuffer), "File sent successfully via v3: %s (%lu bytes)",
           filename.c_str(), (unsigned long)fileSize);
  return sendfileBuffer;
}

// ============================================================================
// ESP-NOW Encryption Commands
// ============================================================================

// Set passphrase command
const char* cmd_espnow_setpassphrase(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "ESP-NOW not initialized. Run 'openespnow' first.";
  }

  String passphrase = argsIn;
  passphrase.trim();

  if (passphrase.length() == 0) {
    return "Usage: espnow setpassphrase \"your_passphrase_here\"\n"
           "       espnow setpassphrase clear";
  }

  if (passphrase == "clear") {
    setEspNowPassphrase("");
    return "ESP-NOW encryption disabled. All future pairings will be unencrypted.";
  }

  if (passphrase.startsWith("\"") && passphrase.endsWith("\"")) {
    passphrase = passphrase.substring(1, passphrase.length() - 1);
  }

  if (passphrase.length() < 8) {
    return "Error: Passphrase must be at least 8 characters long.";
  }

  if (passphrase.length() > 128) {
    return "Error: Passphrase must be 128 characters or less.";
  }

  setEspNowPassphrase(passphrase);
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  snprintf(getDebugBuffer(), 1024,
           "ESP-NOW encryption passphrase set. Use 'espnow pairsecure' to pair with encryption.\n"
           "Key derived from: %s...%s",
           passphrase.substring(0, 3).c_str(),
           passphrase.substring(passphrase.length() - 3).c_str());
  return getDebugBuffer();
}

// Encryption status command
const char* cmd_espnow_encstatus(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "ESP-NOW not initialized. Run 'openespnow' first.";
  }

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  char* p = getDebugBuffer();
  size_t remaining = 1024;

  int n = snprintf(p, remaining, "ESP-NOW Encryption Status:\n");
  p += n;
  remaining -= n;

  n = snprintf(p, remaining, "  Encryption Enabled: %s\n",
               gEspNow->encryptionEnabled ? "Yes" : "No");
  p += n;
  remaining -= n;

  if (gEspNow->encryptionEnabled) {
    n = snprintf(p, remaining, "  Passphrase Set: %s\n",
                 gEspNow->passphrase.length() > 0 ? "Yes" : "No");
    p += n;
    remaining -= n;

    if (gEspNow->passphrase.length() > 0) {
      String hint = gEspNow->passphrase.substring(0, 3) + "..." + gEspNow->passphrase.substring(gEspNow->passphrase.length() - 3);
      n = snprintf(p, remaining, "  Passphrase Hint: %s\n", hint.c_str());
      p += n;
      remaining -= n;
    }

    n = snprintf(p, remaining, "  Key Fingerprint: ");
    p += n;
    remaining -= n;

    for (int i = 0; i < 4; i++) {
      n = snprintf(p, remaining, "%02X", gEspNow->derivedKey[i]);
      p += n;
      remaining -= n;
    }

    n = snprintf(p, remaining, "...\n");
    p += n;
    remaining -= n;
  }

  return getDebugBuffer();
}

// Secure pairing command
const char* cmd_espnow_pairsecure(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "ESP-NOW not initialized. Run 'openespnow' first.";
  }

  if (!gEspNow->encryptionEnabled) {
    return "Encryption not enabled. Run 'espnow setpassphrase \"your_phrase\"' first.";
  }

  String args = argsIn;
  args.trim();

  if (args.length() == 0) {
    return "Usage: espnow pairsecure <mac_address> <device_name>";
  }

  int spacePos = args.indexOf(' ');
  if (spacePos < 0) {
    return "Usage: espnow pairsecure <mac_address> <device_name>";
  }

  String macStr = args.substring(0, spacePos);
  String deviceName = args.substring(spacePos + 1);
  macStr.trim();
  deviceName.trim();

  if (macStr.length() == 0 || deviceName.length() == 0) {
    return "Usage: espnow pairsecure <mac_address> <device_name>";
  }

  uint8_t mac[6];
  if (!parseMacAddress(macStr, mac)) {
    return "Invalid MAC address format. Use AA:BB:CC:DD:EE:FF";
  }
  // Prevent pairing with self MAC (STA or AP interface)
  {
    uint8_t selfSta[6];
    uint8_t selfAp[6];
    esp_wifi_get_mac(WIFI_IF_STA, selfSta);
    esp_wifi_get_mac(WIFI_IF_AP, selfAp);
    if (memcmp(mac, selfSta, 6) == 0 || memcmp(mac, selfAp, 6) == 0) {
      return "Cannot pair with self MAC address.";
    }
  }

  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (memcmp(gEspNow->devices[i].mac, mac, 6) == 0) {
      if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
      snprintf(getDebugBuffer(), 1024,
               "Device already paired. Use 'espnow unpair %s' first.", macStr.c_str());
      return getDebugBuffer();
    }
  }

  if (gEspNow->deviceCount >= 16) {
    return "Maximum number of devices (16) already paired.";
  }

  if (!addEspNowPeerWithEncryption(mac, true, gEspNow->derivedKey)) {
    return "Failed to add encrypted peer to ESP-NOW.";
  }

  memcpy(gEspNow->devices[gEspNow->deviceCount].mac, mac, 6);
  gEspNow->devices[gEspNow->deviceCount].name = deviceName;
  gEspNow->devices[gEspNow->deviceCount].encrypted = true;
  memcpy(gEspNow->devices[gEspNow->deviceCount].key, gEspNow->derivedKey, 16);
  gEspNow->deviceCount++;

  removeFromUnpairedList(mac);

  // Seed gMeshPeers immediately so v3_broadcast reaches this peer on the next
  // heartbeat tick (without this, two freshly-paired devices never exchange
  // heartbeats until one reboots, so mesh topology stays empty)
  if (meshEnabled()) {
    MeshPeerHealth* ph = getMeshPeerHealth(mac, true);
    if (ph) ph->lastHeartbeatMs = millis();
    V3PayloadHeartbeat hb = {};
    hb.role = gSettings.meshRole;
    hb.uptimeSec = (uint32_t)(millis() / 1000);
    hb.freeHeap  = (uint32_t)ESP.getFreeHeap();
    strncpy(hb.deviceName, gSettings.espnowDeviceName.c_str(), sizeof(hb.deviceName) - 1);
    wifi_ap_record_t ap = {};
    hb.rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : (int8_t)-127;
    v3_send_frame(mac, ESPNOW_V3_TYPE_HEARTBEAT, ESPNOW_V3_FLAG_ACK_REQ, generateMessageId(),
                  (const uint8_t*)&hb, (uint16_t)sizeof(hb), 1);
  }
  saveMeshPeers();
  saveEspNowDevices();

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  snprintf(getDebugBuffer(), 1024,
           "Encrypted device paired successfully: %s (%s)\nKey fingerprint: %02X%02X%02X%02X...",
           deviceName.c_str(), macStr.c_str(),
           gEspNow->derivedKey[0], gEspNow->derivedKey[1], gEspNow->derivedKey[2], gEspNow->derivedKey[3]);
  return getDebugBuffer();
}

// ============================================================================
// ESP-NOW Metadata Request Command
// ============================================================================

extern void requestMetadata(const uint8_t* peerMac, bool force);

const char* cmd_espnow_requestmeta(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow || !gEspNow->initialized) return "Error: ESP-NOW not initialized";
  
  String args = argsIn;
  args.trim();
  if (args.length() == 0) return "Usage: espnow requestmeta <name_or_mac>";
  
  uint8_t targetMac[6];
  if (!resolveDeviceNameOrMac(args, targetMac)) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024, "Error: Device '%s' not found", args.c_str());
    return getDebugBuffer();
  }
  
  // Check for self-targeting (metadata request to own MAC won't work)
  {
    uint8_t selfSta[6], selfAp[6];
    esp_wifi_get_mac(WIFI_IF_STA, selfSta);
    esp_wifi_get_mac(WIFI_IF_AP, selfAp);
    if (memcmp(targetMac, selfSta, 6) == 0 || memcmp(targetMac, selfAp, 6) == 0) {
      return "Error: Cannot request metadata from self. This device is paired with its own MAC address. Unpair and pair with the correct remote device.";
    }
  }
  
  requestMetadata(targetMac, true);
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  snprintf(getDebugBuffer(), 1024, "Metadata request sent to %s", args.c_str());
  return getDebugBuffer();
}

// ============================================================================
// ESP-NOW Remote Execution & Streaming Commands
// ============================================================================

// Remote file browse
const char* cmd_espnow_browse(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "ESP-NOW not initialized. Run 'openespnow' first.";
  }

  if (!gEspNow->encryptionEnabled) {
    return "ESP-NOW encryption required. Set a passphrase with 'espnow setpassphrase \"your_phrase\"' and pair securely.";
  }

  String args = argsIn;
  args.trim();

  // Parse: <target> <user> <pass> [path]
  int firstSpace = args.indexOf(' ');
  if (firstSpace < 0) return "Usage: espnow browse <target> <username> <password> [path]";

  int secondSpace = args.indexOf(' ', firstSpace + 1);
  if (secondSpace < 0) return "Usage: espnow browse <target> <username> <password> [path]";

  int thirdSpace = args.indexOf(' ', secondSpace + 1);
  if (thirdSpace < 0) return "Usage: espnow browse <target> <username> <password> [path]";

  String target = args.substring(0, firstSpace);
  String username = args.substring(firstSpace + 1, secondSpace);
  
  // Find fourth space (optional path)
  int fourthSpace = args.indexOf(' ', thirdSpace + 1);
  String password, path;
  if (fourthSpace < 0) {
    password = args.substring(secondSpace + 1, thirdSpace);
    path = args.substring(thirdSpace + 1);
    if (path.length() == 0) path = "/";  // Default to root
  } else {
    password = args.substring(secondSpace + 1, thirdSpace);
    path = args.substring(thirdSpace + 1);
  }

  target.trim();
  username.trim();
  password.trim();
  path.trim();

  if (target.length() == 0 || username.length() == 0 || password.length() == 0) {
    return "Usage: espnow browse <target> <username> <password> [path]";
  }

  if (path.length() == 0) path = "/";

  uint8_t targetMac[6];
  if (!resolveDeviceNameOrMac(target, targetMac)) {
    static char browseBuffer[256];
    snprintf(browseBuffer, sizeof(browseBuffer),
             "Target device '%s' not found or not paired. Pair the device first (prefer 'espnow pairsecure').",
             target.c_str());
    return browseBuffer;
  }

  // Build V3 CMD payload: "user:pass:files /path"
  char cmdPayload[ESPNOW_V3_MAX_PAYLOAD];
  int payloadLen = snprintf(cmdPayload, sizeof(cmdPayload), "%s:%s:files %s",
                            username.c_str(), password.c_str(), path.c_str());
  if (payloadLen >= (int)sizeof(cmdPayload)) payloadLen = sizeof(cmdPayload) - 1;

  if (isMeshMode()) {
    if (!isPairedDevice(targetMac)) {
      BROADCAST_PRINTF("[ESP-NOW][mesh] browse send rejected: not paired MAC=%s", formatMacAddress(targetMac).c_str());
      return "Rejected (mesh): device not paired. Use 'espnow pair' first.";
    }
    if (!espnowPeerExists(targetMac)) {
      BROADCAST_PRINTF("[ESP-NOW][mesh] browse send rejected: no peer entry MAC=%s", formatMacAddress(targetMac).c_str());
      return "Rejected (mesh): destination not in ESP-NOW peer table.";
    }
  }

  // Send via V3 CMD (receiver parses user:pass:cmd format)
  uint32_t msgId = generateMessageId();
  bool sent = v3_send_frame(targetMac, ESPNOW_V3_TYPE_CMD, ESPNOW_V3_FLAG_ACK_REQ, msgId,
                            (const uint8_t*)cmdPayload, (uint16_t)payloadLen, 1);
  
  static char browseBuffer[256];
  if (!sent) {
    snprintf(browseBuffer, sizeof(browseBuffer), "Failed to send V3 browse request");
    return browseBuffer;
  }

  snprintf(browseBuffer, sizeof(browseBuffer), "File browse request sent to %s for path: %s",
           target.c_str(), path.c_str());
  return browseBuffer;
}

// Remote file fetch (pull a file from remote device)
const char* cmd_espnow_fetch(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "ESP-NOW not initialized. Run 'openespnow' first.";
  }

  if (!gEspNow->encryptionEnabled) {
    return "ESP-NOW encryption required. Set a passphrase with 'espnow setpassphrase \"your_phrase\"' and pair securely.";
  }

  String args = argsIn;
  args.trim();

  // Parse: <target> <user> <pass> <path>
  int firstSpace = args.indexOf(' ');
  if (firstSpace < 0) return "Usage: espnow fetch <target> <username> <password> <path>";

  int secondSpace = args.indexOf(' ', firstSpace + 1);
  if (secondSpace < 0) return "Usage: espnow fetch <target> <username> <password> <path>";

  int thirdSpace = args.indexOf(' ', secondSpace + 1);
  if (thirdSpace < 0) return "Usage: espnow fetch <target> <username> <password> <path>";

  String target = args.substring(0, firstSpace);
  String username = args.substring(firstSpace + 1, secondSpace);
  String password = args.substring(secondSpace + 1, thirdSpace);
  String path = args.substring(thirdSpace + 1);

  target.trim();
  username.trim();
  password.trim();
  path.trim();

  if (target.length() == 0 || username.length() == 0 || password.length() == 0 || path.length() == 0) {
    return "Usage: espnow fetch <target> <username> <password> <path>";
  }

  uint8_t targetMac[6];
  if (!resolveDeviceNameOrMac(target, targetMac)) {
    static char fetchBuffer[256];
    snprintf(fetchBuffer, sizeof(fetchBuffer),
             "Target device '%s' not found or not paired. Pair the device first (prefer 'espnow pairsecure').",
             target.c_str());
    return fetchBuffer;
  }

  // Build V3 CMD payload: "user:pass:espnow sendfile <our_name> <path>"
  // This tells the remote device to send the file back to us via V3 binary file transfer
  char cmdPayload[ESPNOW_V3_MAX_PAYLOAD];
  int payloadLen = snprintf(cmdPayload, sizeof(cmdPayload), "%s:%s:espnow sendfile %s %s",
                            username.c_str(), password.c_str(),
                            gSettings.espnowDeviceName.c_str(), path.c_str());
  if (payloadLen >= (int)sizeof(cmdPayload)) payloadLen = sizeof(cmdPayload) - 1;

  if (isMeshMode()) {
    if (!isPairedDevice(targetMac)) {
      BROADCAST_PRINTF("[ESP-NOW][mesh] fetch send rejected: not paired MAC=%s", formatMacAddress(targetMac).c_str());
      return "Rejected (mesh): device not paired. Use 'espnow pair' first.";
    }
    if (!espnowPeerExists(targetMac)) {
      BROADCAST_PRINTF("[ESP-NOW][mesh] fetch send rejected: no peer entry MAC=%s", formatMacAddress(targetMac).c_str());
      return "Rejected (mesh): destination not in ESP-NOW peer table.";
    }
  }

  // Send via V3 CMD (receiver parses user:pass:cmd format)
  uint32_t msgId = generateMessageId();
  bool sent = v3_send_frame(targetMac, ESPNOW_V3_TYPE_CMD, ESPNOW_V3_FLAG_ACK_REQ, msgId,
                            (const uint8_t*)cmdPayload, (uint16_t)payloadLen, 1);
  
  static char fetchBuffer[256];
  if (!sent) {
    snprintf(fetchBuffer, sizeof(fetchBuffer), "Failed to send V3 fetch request");
    return fetchBuffer;
  }

  snprintf(fetchBuffer, sizeof(fetchBuffer), "File fetch request sent to %s for: %s",
           target.c_str(), path.c_str());
  return fetchBuffer;
}

// Remote command execution
const char* cmd_espnow_remote(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "ESP-NOW not initialized. Run 'openespnow' first.";
  }

  if (!gEspNow->encryptionEnabled) {
    return "ESP-NOW encryption required. Set a passphrase with 'espnow setpassphrase "
           "your_phrase"
           "' and pair securely.";
  }

  String args = argsIn;
  args.trim();

  // Parse: <target> <username> <password> <command>
  int firstSpace = args.indexOf(' ');
  if (firstSpace < 0) return "Usage: espnow remote <target> <username> <password> <command>";

  int secondSpace = args.indexOf(' ', firstSpace + 1);
  if (secondSpace < 0) return "Usage: espnow remote <target> <username> <password> <command>";

  int thirdSpace = args.indexOf(' ', secondSpace + 1);
  if (thirdSpace < 0) return "Usage: espnow remote <target> <username> <password> <command>";

  String target = args.substring(0, firstSpace);
  String username = args.substring(firstSpace + 1, secondSpace);
  String password = args.substring(secondSpace + 1, thirdSpace);
  String command = args.substring(thirdSpace + 1);

  target.trim();
  username.trim();
  password.trim();
  command.trim();

  if (target.length() == 0 || username.length() == 0 || password.length() == 0 || command.length() == 0) {
    return "Usage: espnow remote <target> <username> <password> <command>";
  }

  uint8_t targetMac[6];
  if (!resolveDeviceNameOrMac(target, targetMac)) {
    static char remoteBuffer[256];
    snprintf(remoteBuffer, sizeof(remoteBuffer),
             "Target device '%s' not found or not paired. Pair the device first (prefer 'espnow pairsecure').",
             target.c_str());
    return remoteBuffer;
  }

  // Check for self-targeting
  {
    uint8_t selfSta[6], selfAp[6];
    esp_wifi_get_mac(WIFI_IF_STA, selfSta);
    esp_wifi_get_mac(WIFI_IF_AP, selfAp);
    if (memcmp(targetMac, selfSta, 6) == 0 || memcmp(targetMac, selfAp, 6) == 0) {
      return "Error: Cannot send remote command to self. This device is paired with its own MAC. Unpair and pair with the correct remote device.";
    }
  }

  // V3 binary CMD message (credentials checked on receiver)
  // Build command payload: "user:pass:cmd"
  char cmdPayload[ESPNOW_V3_MAX_PAYLOAD];
  int payloadLen = snprintf(cmdPayload, sizeof(cmdPayload), "%s:%s:%s", 
                            username.c_str(), password.c_str(), command.c_str());
  if (payloadLen >= (int)sizeof(cmdPayload)) payloadLen = sizeof(cmdPayload) - 1;

  uint32_t msgId = generateMessageId();
  /* V2 ACK WAIT DEPRECATED
  msgAckWaitRegister(msgId);
  */
  
  bool success = false;
  for (int attempt = 0; attempt < 2; attempt++) {
    success = v3_send_frame(targetMac, ESPNOW_V3_TYPE_CMD, ESPNOW_V3_FLAG_ACK_REQ, msgId,
                            (const uint8_t*)cmdPayload, (uint16_t)payloadLen, 1);
    if (!success) continue;
    /* V2 ACK WAIT DEPRECATED
    if (msgAckWaitBlock(msgId, 350)) break;
    */
    if (success) break; // V3 has internal ACK handling
  }
  
  static char remoteBuffer[256];
  if (!success) {
    snprintf(remoteBuffer, sizeof(remoteBuffer), "Failed to send remote command");
    return remoteBuffer;
  }

  // Show "Running" notification so the user sees immediate feedback
  // The CMD_RESP will update this in-place to OK/FAIL when the result arrives
  notifyRemoteCommandReceived(target.c_str(), command.c_str());

  snprintf(remoteBuffer, sizeof(remoteBuffer), "Remote command sent via V3 to %s: %s",
           target.c_str(), command.c_str());
  return remoteBuffer;
}

// Start stream command
const char* cmd_espnow_startstream(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";

  if (!gExecAuthContext.ip.startsWith("espnow:")) {
    return "Error: 'startstream' only works via ESP-NOW remote execution.\n"
           "Usage from Device A: espnow remote DeviceB admin pass startstream";
  }

  const uint8_t* senderMac = (const uint8_t*)gExecAuthContext.opaque;

  if (!gEspNow->streamTarget) {
    gEspNow->streamTarget = (uint8_t*)ps_alloc(6, AllocPref::PreferPSRAM, "espnow.mac");
    if (!gEspNow->streamTarget) {
      return "Error: Failed to allocate memory for stream target";
    }
  }

  memcpy(gEspNow->streamTarget, senderMac, 6);
  gEspNow->streamActive = true;
  gEspNow->lastStreamSendTime = 0;
  gEspNow->streamDroppedCount = 0;
  gEspNow->streamSentCount = 0;

  String senderName = getEspNowDeviceName(senderMac);
  if (senderName.length() == 0) {
    senderName = formatMacAddress(senderMac);
  }

  DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] Activated: target=%s name=%s active=%d counters_reset=YES",
         formatMacAddress(gEspNow->streamTarget).c_str(), senderName.c_str(), gEspNow->streamActive);

  static char streamBuffer[512];
  snprintf(streamBuffer, sizeof(streamBuffer),
           "Stream started - all output will be sent to %s\n"
           "Rate limited to 10 messages/second.\n"
           "Large messages (>200 bytes) use chunked transmission for complete delivery.\n"
           "Use 'espnow remote %s admin pass stopstream' to stop.",
           senderName.c_str(), senderName.c_str());
  return streamBuffer;
}

// Stop stream command
const char* cmd_espnow_stopstream(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";

  if (!gEspNow->streamActive) {
    return "No active stream to stop.";
  }

  // Get target info before clearing
  String targetName = "unknown";
  if (gEspNow->streamTarget) {
    targetName = getEspNowDeviceName(gEspNow->streamTarget);
    if (targetName.length() == 0) {
      targetName = formatMacAddress(gEspNow->streamTarget);
    }
  }

  // Report statistics before stopping
  static char streamBuffer[512];
  int pos = snprintf(streamBuffer, sizeof(streamBuffer),
                     "Stream stopped - output no longer sent to %s\n"
                     "Statistics: %lu messages sent, %lu dropped (rate limiting)",
                     targetName.c_str(),
                     (unsigned long)gEspNow->streamSentCount,
                     (unsigned long)gEspNow->streamDroppedCount);

  if (gEspNow->streamDroppedCount > 0 && pos < (int)sizeof(streamBuffer)) {
    float dropRate = (100.0f * gEspNow->streamDroppedCount) / (gEspNow->streamSentCount + gEspNow->streamDroppedCount);
    snprintf(streamBuffer + pos, sizeof(streamBuffer) - pos, "\nDrop rate: %.1f%%", dropRate);
  }

  DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] Deactivated: target=%s sent=%lu dropped=%lu",
         targetName.c_str(),
         (unsigned long)gEspNow->streamSentCount,
         (unsigned long)gEspNow->streamDroppedCount);

  // Stop streaming and free resources
  gEspNow->streamActive = false;
  if (gEspNow->streamTarget) {
    free(gEspNow->streamTarget);
    gEspNow->streamTarget = nullptr;
  }

  return streamBuffer;
}

// ESP-NOW send message command (uses Message Router)
const char* cmd_espnow_send(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "ESP-NOW not initialized. Run 'openespnow' first.";
  }

  // Parse: <name_or_mac> <message>
  String args = argsIn;
  args.trim();
  
  DEBUGF(DEBUG_ESPNOW_STREAM, "[cmd_espnow_send] args.length()=%d", args.length());

  int firstSpace = args.indexOf(' ');
  if (firstSpace < 0) return "Usage: espnow send <name_or_mac> <message>";

  String target = args.substring(0, firstSpace);
  String message = args.substring(firstSpace + 1);
  target.trim();
  message.trim();
  
  DEBUGF(DEBUG_ESPNOW_STREAM, "[cmd_espnow_send] message.length()=%d", message.length());

  if (target.length() == 0 || message.length() == 0) {
    return "Usage: espnow send <name_or_mac> <message>";
  }

  // Resolve device name or MAC address
  uint8_t mac[6];
  if (!resolveDeviceNameOrMac(target, mac)) {
    static char errBuf[256];
    snprintf(errBuf, sizeof(errBuf), 
             "Device '%s' not found. Use 'espnow devices' to see paired devices.", 
             target.c_str());
    return errBuf;
  }

  // Check if trying to send to self
  uint8_t selfSta[6];
  uint8_t selfAp[6];
  esp_wifi_get_mac(WIFI_IF_STA, selfSta);
  esp_wifi_get_mac(WIFI_IF_AP, selfAp);
  if (memcmp(mac, selfSta, 6) == 0 || memcmp(mac, selfAp, 6) == 0) {
    return "Cannot send message to self. Use a different device MAC address.";
  }

  // V3 binary TEXT message (no JSON, no heap allocation)
  size_t msgLen = message.length();
  if (msgLen > ESPNOW_V3_MAX_PAYLOAD - 1) msgLen = ESPNOW_V3_MAX_PAYLOAD - 1;
  
  uint32_t msgId = generateMessageId();
  /* V2 ACK WAIT DEPRECATED
  msgAckWaitRegister(msgId);
  */
  
  bool success = false;
  for (int attempt = 0; attempt < 2; attempt++) {
    success = v3_send_frame(mac, ESPNOW_V3_TYPE_TEXT, ESPNOW_V3_FLAG_ACK_REQ, msgId,
                            (const uint8_t*)message.c_str(), (uint16_t)msgLen, 1);
    if (!success) continue;
    /* V2 ACK WAIT DEPRECATED
    if (msgAckWaitBlock(msgId, 350)) break;
    */
    if (success) break; // V3 has internal ACK handling
  }
  
  if (success) {
    if (!ensureDebugBuffer()) return "Message sent";
    snprintf(getDebugBuffer(), 1024, "Message sent via V3 (ID: %lu)", (unsigned long)msgId);
    return getDebugBuffer();
  } else {
    return "Failed to send message";
  }
}

// Send a synthetic large text payload to trigger fragmentation paths
const char* cmd_espnow_bigsend(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) return "ESP-NOW not initialized. Run 'openespnow' first.";

  String args = argsIn; // format: "<mac|name> <bytes>"
  args.trim();
  int firstSpace = args.indexOf(' ');
  if (firstSpace < 0) return "Usage: espnow bigsend <name_or_mac> <bytes>";

  String target = args.substring(0, firstSpace);
  String sizeStr = args.substring(firstSpace + 1);
  target.trim(); sizeStr.trim();
  if (target.length() == 0 || sizeStr.length() == 0) return "Usage: espnow bigsend <name_or_mac> <bytes>";

  long size = sizeStr.toInt();
  if (size <= 0) return "Error: bytes must be > 0";
  // Keep within v2 fragment defaults (~2880 decoded bytes across 32 frags)
  if (size > 2500) size = 2500;

  // Resolve MAC
  uint8_t mac[6];
  if (!resolveDeviceNameOrMac(target, mac)) {
    return "Error: Unknown device (use paired name or MAC)";
  }

  // Build v2 JSON TEXT payload with repeated 'A' characters
  String textContent;
  textContent.reserve(size);
  for (int i = 0; i < size; i++) textContent += 'A';
  
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String payload = textContent;

  // Send via V3
  uint32_t msgId = generateMessageId();
  bool ok = v3_send_chunked(mac, ESPNOW_V3_TYPE_TEXT, ESPNOW_V3_FLAG_ACK_REQ, msgId,
                            (const uint8_t*)payload.c_str(), payload.length(), 1);
  if (!ensureDebugBuffer()) return ok ? "OK" : "Failed";
  snprintf(getDebugBuffer(), 1024, "bigsend: %s (id=%lu, bytes=%ld)", ok ? "OK" : "FAILED", (unsigned long)msgId, size);
  return getDebugBuffer();
}

#if ENABLE_BONDED_MODE

static void resetBondSync() {
  if (!gEspNow) return;
  gEspNow->bondSyncInFlight = BOND_SYNC_NONE;
  gEspNow->bondSyncRetryCount = 0;
  gEspNow->bondSyncLastAttemptMs = 0;
  gEspNow->bondCapSent = false;
  gEspNow->bondManifestReceived = false;
  gEspNow->bondSettingsReceived = false;
  gEspNow->bondSettingsSent = false;
  gEspNow->lastRemoteCapValid = false;
  // Clear all pending deferred flags — stale messages from the previous
  // session must not be processed after a role swap or sync reset.
  gEspNow->bondNeedsCapabilityRequest = false;
  gEspNow->bondNeedsCapabilityResponse = false;
  gEspNow->bondReceivedCapability = false;
  gEspNow->bondNeedsManifestResponse = false;
  gEspNow->bondNeedsSettingsResponse = false;
  gEspNow->bondPeerStatusValid = false;
  gEspNow->bondNeedsProactiveStatus = false;
  clearBondSessionToken();
}

// ============================================================================
// Bond Mode CLI Commands
// ============================================================================

/**
 * Request capability summary from bonded peer
 */
const char* cmd_bond_requestcap(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gSettings.bondModeEnabled || gSettings.bondPeerMac.length() == 0) {
    return "Not connected in bond mode. Use 'bond connect <device>' first.";
  }
  
  uint8_t peerMac[6];
  if (!parseMacAddress(gSettings.bondPeerMac, peerMac)) {
    return "Invalid peer MAC address in settings.";
  }

  uint32_t reqId = generateMessageId();
  /* V2 ACK WAIT DEPRECATED
  msgAckWaitRegister(reqId);
  */
  bool sent = false;
  for (int attempt = 0; attempt < 2; attempt++) {
    sent = v3_send_frame(peerMac, ESPNOW_V3_TYPE_BOND_CAP_REQ, ESPNOW_V3_FLAG_ACK_REQ, reqId, nullptr, 0, 1);
    if (!sent) continue;
    /* V2 ACK WAIT DEPRECATED
    if (msgAckWaitBlock(reqId, 350)) break;
    */
    if (sent) break; // V3 has internal ACK handling
  }
  return sent ? "Capability request sent. Check output for response." : "Failed to send capability request.";
}

/**
 * Show local capability summary
 */
const char* cmd_bond_showcap(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  CapabilitySummary cap;
  buildCapabilitySummary(cap);
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  int pos = 0;
  char* buf = getDebugBuffer();
  
  pos += snprintf(buf + pos, 1024 - pos, "=== Capability Summary ===\n");
  pos += snprintf(buf + pos, 1024 - pos, "Device: %s\n", cap.deviceName);
  pos += snprintf(buf + pos, 1024 - pos, "MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  cap.mac[0], cap.mac[1], cap.mac[2], cap.mac[3], cap.mac[4], cap.mac[5]);
  pos += snprintf(buf + pos, 1024 - pos, "Role: %s\n", cap.role == 1 ? "master" : "worker");
  pos += snprintf(buf + pos, 1024 - pos, "FW Hash: %02X%02X%02X%02X...\n",
                  cap.fwHash[0], cap.fwHash[1], cap.fwHash[2], cap.fwHash[3]);
  pos += snprintf(buf + pos, 1024 - pos, "Features: 0x%08lX\n", (unsigned long)cap.featureMask);
  pos += snprintf(buf + pos, 1024 - pos, "Services: 0x%08lX\n", (unsigned long)cap.serviceMask);
  pos += snprintf(buf + pos, 1024 - pos, "Sensors: 0x%08lX\n", (unsigned long)cap.sensorMask);
  pos += snprintf(buf + pos, 1024 - pos, "Flash: %lu MB, PSRAM: %lu MB\n",
                  (unsigned long)cap.flashSizeMB, (unsigned long)cap.psramSizeMB);
  pos += snprintf(buf + pos, 1024 - pos, "WiFi Ch: %u\n", cap.wifiChannel);
  pos += snprintf(buf + pos, 1024 - pos, "Uptime: %lu sec\n", (unsigned long)cap.uptimeSeconds);
  
  return getDebugBuffer();
}

/**
 * Request full manifest from bonded peer (v3 binary protocol)
 */
const char* cmd_bond_requestmanifest(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gSettings.bondModeEnabled || gSettings.bondPeerMac.length() == 0) {
    return "Not connected in bond mode. Use 'bond connect <device>' first.";
  }
  
  uint8_t peerMac[6];
  if (!parseMacAddress(gSettings.bondPeerMac, peerMac)) {
    return "Invalid peer MAC address in settings.";
  }
  
  // Send v3 manifest request
  uint32_t msgId = generateMessageId();
  if (v3_send_frame(peerMac, ESPNOW_V3_TYPE_MANIFEST_REQ, ESPNOW_V3_FLAG_ACK_REQ, msgId, nullptr, 0, 1)) {
    return "Manifest request sent (v3). Response will arrive via file transfer.";
  } else {
    return "Failed to send manifest request.";
  }
}

/**
 * Show cached remote manifests: bond showremotemanifest [fwHash]
 * Without args: list all cached manifests
 * With fwHash arg: show specific manifest
 */
const char* cmd_bond_showremotemanifest(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  extern bool filesystemReady;
  if (!filesystemReady) {
    return "Filesystem not ready.";
  }
  
  String args = argsIn;
  args.trim();
  
  const char* manifestDir = "/system/manifests";
  
  // If no argument, list all cached manifests
  if (args.length() == 0) {
    if (!LittleFS.exists(manifestDir)) {
      return "No cached manifests. Use 'bond requestmanifest' to fetch from peer.";
    }
    
    File dir = LittleFS.open(manifestDir);
    if (!dir || !dir.isDirectory()) {
      return "Cannot open manifests directory.";
    }
    
    broadcastOutput("=== Cached Remote Manifests ===");
    int count = 0;
    File entry;
    while ((entry = dir.openNextFile())) {
      if (!entry.isDirectory()) {
        String name = entry.name();
        size_t sz = entry.size();
        // Extract fwHash from filename (remove .json)
        if (name.endsWith(".json")) {
          String fwHash = name.substring(0, name.length() - 5);
          BROADCAST_PRINTF("  %s (%d bytes)", fwHash.c_str(), (int)sz);
          count++;
        }
      }
      entry.close();
    }
    dir.close();
    
    if (count == 0) {
      return "No cached manifests found.";
    }
    BROADCAST_PRINTF("Total: %d manifest(s)", count);
    return "Use 'bond showremotemanifest <fwHash>' to view details.";
  }
  
  // Show specific manifest by fwHash
  String path = String(manifestDir) + "/" + args + ".json";
  if (!LittleFS.exists(path.c_str())) {
    return "Manifest not found. Use 'bond showremotemanifest' to list available.";
  }
  
  FsLockGuard guard("bond.manifest.read");
  File f = LittleFS.open(path.c_str(), "r");
  if (!f) {
    return "Failed to open manifest file.";
  }
  
  String manifest = f.readString();
  f.close();
  
  broadcastOutput("=== Remote Manifest: " + args + " ===");
  BROADCAST_PRINTF("Size: %u bytes", (unsigned)manifest.length());
  
  // Broadcast manifest in 200-byte chunks
  const char* ptr = manifest.c_str();
  size_t remaining = manifest.length();
  char chunkBuf[201];
  while (remaining > 0) {
    size_t chunk = (remaining > 200) ? 200 : remaining;
    memcpy(chunkBuf, ptr, chunk);
    chunkBuf[chunk] = '\0';
    broadcastOutput(chunkBuf);
    ptr += chunk;
    remaining -= chunk;
  }
  
  return "Manifest displayed above.";
}

/**
 * Show local device manifest (for debugging)
 */
const char* cmd_bond_showmanifest(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String manifest = generateDeviceManifest();
  
  broadcastOutput("=== Device Manifest ===");
  BROADCAST_PRINTF("Size: %u bytes", (unsigned)manifest.length());
  
  // Broadcast manifest in 200-byte chunks (debug queue buffer is 256 bytes)
  const char* ptr = manifest.c_str();
  size_t remaining = manifest.length();
  char chunkBuf[201];
  while (remaining > 0) {
    size_t chunk = (remaining > 200) ? 200 : remaining;
    memcpy(chunkBuf, ptr, chunk);
    chunkBuf[chunk] = '\0';
    broadcastOutput(chunkBuf);
    ptr += chunk;
    remaining -= chunk;
  }
  
  return "Manifest displayed above.";
}

/**
 * Connect to a bonded peer device: bond connect <mac_or_name>
 */
const char* cmd_bond_connect(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow || !gEspNow->initialized) {
    return "ESP-NOW not initialized. Run 'openespnow' first.";
  }
  
  String args = argsIn;
  args.trim();
  
  if (args.length() == 0) {
    return "Usage: bond connect <mac_or_name>";
  }
  
  // Resolve device name or MAC to MAC bytes
  uint8_t peerMac[6];
  if (!resolveDeviceNameOrMac(args, peerMac)) {
    return "Device not found. Use 'espnow list' to see paired devices.";
  }
  
  // Check if already connected
  if (gSettings.bondModeEnabled && 
      gSettings.bondPeerMac == formatMacAddress(peerMac)) {
    return "Already connected to this device in bond mode.";
  }
  
  // Enable bond mode and set peer MAC
  // Role is determined by MAC address comparison (higher MAC = master)
  // This ensures deterministic role assignment when both devices run 'bond connect'
  // Role-based handshake sequencing:
  // - Master (role=1) waits for worker's data, then sends its own
  // - Worker (role=0) sends data first when entering each exchange state
  // Set peer MAC and role BEFORE enabling bond mode to avoid race window
  // where espnow task sees bondModeEnabled=true with empty peerMac
  setSetting(gSettings.bondPeerMac, formatMacAddress(peerMac));
  
  // Determine role by comparing our MAC with peer MAC (higher MAC = master)
  uint8_t ourMac[6];
  WiFi.macAddress(ourMac);
  int cmp = memcmp(ourMac, peerMac, 6);
  setSetting(gSettings.bondRole, (uint8_t)((cmp > 0) ? 1 : 0));  // Higher MAC becomes MASTER
  
  // Enable bond mode last — peerMac and role are already set
  setSetting(gSettings.bondModeEnabled, true);
  INFO_ESPNOWF("[PAIR] Role assigned by MAC comparison: %s (our=%02X:%02X:%02X:%02X:%02X:%02X, peer=%02X:%02X:%02X:%02X:%02X:%02X)",
                gSettings.bondRole == 1 ? "MASTER" : "WORKER",
                ourMac[0], ourMac[1], ourMac[2], ourMac[3], ourMac[4], ourMac[5],
                peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5]);
  gEspNow->bondPeerOnline = false;  // Will be set true when heartbeat received
  gEspNow->lastBondHeartbeatReceivedMs = 0;
  resetBondSync();  // Reset handshake state for fresh start
  
  String deviceName = getEspNowDeviceName(peerMac);
  if (deviceName.length() == 0) deviceName = formatMacAddress(peerMac);
  
  // Don't request capabilities/manifest here - wait for peer to come online via heartbeat
  // The heartbeat handler will trigger capability/manifest exchange when peer is detected
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  snprintf(getDebugBuffer(), 1024, 
           "Bond mode enabled. Waiting for peer: %s (%s)\nRole: %s\nCapabilities + manifest will be requested when peer comes online.",
           deviceName.c_str(), formatMacAddress(peerMac).c_str(),
           gSettings.bondRole == 1 ? "master" : "worker");
  return getDebugBuffer();
}

/**
 * Disconnect from bonded peer: bond disconnect
 */
const char* cmd_bond_disconnect(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gSettings.bondModeEnabled) {
    return "Not currently in bond mode.";
  }
  
  String prevPeer = gSettings.bondPeerMac;
  setSetting(gSettings.bondModeEnabled, false);
  setSetting(gSettings.bondPeerMac, String(""));
  resetBondSync();  // Reset handshake state
  if (gEspNow) gEspNow->bondPeerOnline = false;
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  snprintf(getDebugBuffer(), 1024, "Disconnected from bonded device: %s", prevPeer.c_str());
  return getDebugBuffer();
}

/**
 * Show bond mode status: bond status
 * NOTE: Output is split into multiple lines to avoid DEBUG_MSG_SIZE (256 byte) truncation
 */
const char* cmd_bond_status(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gSettings.bondModeEnabled) {
    broadcastOutput("Bond mode: DISABLED");
    BROADCAST_PRINTF("Role: %s", gSettings.bondRole == 1 ? "master" : "worker");
    return "OK";
  }
  
  String deviceName = "";
  uint8_t peerMac[6];
  if (parseMacAddress(gSettings.bondPeerMac, peerMac)) {
    deviceName = getEspNowDeviceName(peerMac);
  }
  if (deviceName.length() == 0) deviceName = gSettings.bondPeerMac;
  
  // Calculate heartbeat timing
  uint32_t now = millis();
  uint32_t lastRecvAgo = 0;
  const char* peerStatus = "UNKNOWN";
  
  if (gEspNow) {
    if (gEspNow->lastBondHeartbeatReceivedMs > 0) {
      lastRecvAgo = (now - gEspNow->lastBondHeartbeatReceivedMs) / 1000;
    }
    peerStatus = gEspNow->bondPeerOnline ? "ONLINE" : "OFFLINE";
  }
  
  // Compute sync status from helper
  bool capOk = gEspNow ? gEspNow->lastRemoteCapValid : false;
  bool manOk = gEspNow ? gEspNow->bondManifestReceived : false;
  bool setOk = gEspNow ? gEspNow->bondSettingsReceived : false;
  const char* syncLabel = isBondSynced() ? "SYNCED" : "SYNCING";
  if (!gEspNow || !gEspNow->bondPeerOnline) syncLabel = "OFFLINE";
  
  // Output each line separately to avoid DEBUG_MSG_SIZE (256 byte) truncation
  broadcastOutput("Bond mode: ENABLED");
  BROADCAST_PRINTF("Role: %s", gSettings.bondRole == 1 ? "master (display/gamepad)" : "worker (compute/network)");
  BROADCAST_PRINTF("Peer: %s (%s)", deviceName.c_str(), gSettings.bondPeerMac.c_str());
  BROADCAST_PRINTF("Peer status: %s", peerStatus);
  bool setSent = gEspNow ? gEspNow->bondSettingsSent : false;
  BROADCAST_PRINTF("Sync: %s (cap=%d manifest=%d settRx=%d settTx=%d)", syncLabel, (int)capOk, (int)manOk, (int)setOk, (int)setSent);
  BROADCAST_PRINTF("Flags: capSent=%d syncInFlight=%d retries=%d",
                   gEspNow ? gEspNow->bondCapSent : 0,
                   gEspNow ? (int)gEspNow->bondSyncInFlight : 0,
                   gEspNow ? (int)gEspNow->bondSyncRetryCount : 0);
  BROADCAST_PRINTF("Heartbeats sent: %lu", gEspNow ? (unsigned long)gEspNow->bondHeartbeatsSent : 0UL);
  BROADCAST_PRINTF("Heartbeats received: %lu", gEspNow ? (unsigned long)gEspNow->bondHeartbeatsReceived : 0UL);
  BROADCAST_PRINTF("Last heartbeat: %lus ago", (unsigned long)lastRecvAgo);
  
  return "OK";
}

/**
 * Set bond mode role: bond role <master|worker>
 */
const char* cmd_bond_role(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String args = argsIn;
  args.trim();
  args.toLowerCase();
  
  if (args.length() == 0) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024, "Current role: %s",
             gSettings.bondRole == 1 ? "master" : "worker");
    return getDebugBuffer();
  }
  
  uint8_t newRole = 255;
  if (args == "master" || args == "1") {
    newRole = 1;
  } else if (args == "worker" || args == "0") {
    newRole = 0;
  } else {
    return "Usage: bond role <master|worker>";
  }
  
  bool changed = (newRole != gSettings.bondRole);
  setSetting(gSettings.bondRole, newRole);
  
  // Reset handshake when role changes — sequencing is role-dependent
  // (worker=initiator, master=responder). Without reset, both sides
  // could end up waiting or both initiating simultaneously.
  if (changed && gEspNow && gEspNow->initialized) {
    resetBondSync();
    // NOTE: lastRemoteCapValid is intentionally NOT cleared here — the peer's
    // capabilities (name, sensors, features) haven't changed because our local
    // role changed. Keeping it valid means peerName stays correct in the UI
    // while the new handshake negotiates. It will be updated when CAP_RESP arrives.
    gEspNow->bondPeerStatusValid = false;
    
    // Disable all sensor streaming — don't carry old master's prefs into new role
    setSetting(gSettings.bondStreamThermal, false);
    setSetting(gSettings.bondStreamTof, false);
    setSetting(gSettings.bondStreamImu, false);
    setSetting(gSettings.bondStreamGps, false);
    setSetting(gSettings.bondStreamGamepad, false);
    setSetting(gSettings.bondStreamFmradio, false);
    setSetting(gSettings.bondStreamRtc, false);
    setSetting(gSettings.bondStreamPresence, false);
    // Clear runtime streaming flags too
    for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
      stopSensorDataStreaming((RemoteSensorType)i);
    }
    
    // If peer is already online and we're now master, trigger sync tick
    if (gEspNow->bondPeerOnline && newRole == 1) {
      gEspNow->bondNeedsCapabilityRequest = true;  // Consumed by sync tick
      BROADCAST_PRINTF("[BOND] Role changed to master — sync tick will drive handshake");
    } else {
      BROADCAST_PRINTF("[BOND] Role changed to %s — handshake reset, will re-negotiate when peer online",
                       newRole == 1 ? "master" : "worker");
    }
  }
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  snprintf(getDebugBuffer(), 1024, "Bond role set to: %s%s",
           newRole == 1 ? "master (display/gamepad)" : "worker (compute/network)",
           changed ? " (handshake reset)" : " (unchanged)");
  return getDebugBuffer();
}

/**
 * Stream sensor data to bonded peer: bond stream <sensor> <on|off>
 * Works on both master and worker - bidirectional streaming supported
 */
const char* cmd_bond_stream(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gSettings.bondModeEnabled) {
    return "Not in bond mode. Use 'bond connect <device>' first.";
  }
  
  String args = argsIn;
  args.trim();
  
  // Remove command prefix if present
  if (args.startsWith("bond stream")) {
    args = args.substring(11);
    args.trim();
  }
  
  if (args.length() == 0) {
    // Show current streaming status with detailed diagnostics
    broadcastOutput("[BOND] Sensor streaming diagnostics (bidirectional):");
    BROADCAST_PRINTF("  Bond mode enabled: %s", gSettings.bondModeEnabled ? "YES" : "NO");
    BROADCAST_PRINTF("  Our role: %s", gSettings.bondRole == 1 ? "MASTER" : "WORKER");
    BROADCAST_PRINTF("  Peer MAC: %s", gSettings.bondPeerMac.length() > 0 ? gSettings.bondPeerMac.c_str() : "(none)");
    BROADCAST_PRINTF("  ESP-NOW init: %s", (gEspNow && gEspNow->initialized) ? "YES" : "NO");
    BROADCAST_PRINTF("  Peer online: %s", gEspNow ? (gEspNow->bondPeerOnline ? "YES" : "NO") : "N/A");
    BROADCAST_PRINTF("  isBondModeOnline(): %s", isBondModeOnline() ? "YES" : "NO");
    BROADCAST_PRINTF("  V3PayloadSensorData size: %u bytes", (unsigned)sizeof(V3PayloadSensorData));
    BROADCAST_PRINTF("  Max sensor data: %u bytes", (unsigned)(ESPNOW_V3_MAX_PAYLOAD - sizeof(V3PayloadSensorData)));
    broadcastOutput("");
    broadcastOutput("  Sensor streaming (runtime / saved):");
    bool savedFlags[] = {gSettings.bondStreamThermal, gSettings.bondStreamTof, gSettings.bondStreamImu, 
                         gSettings.bondStreamGps, gSettings.bondStreamGamepad, gSettings.bondStreamFmradio,
                         gSettings.bondStreamRtc, gSettings.bondStreamPresence};
    const char* sensors[] = {"thermal", "tof", "imu", "gps", "gamepad", "fmradio", "rtc", "presence"};
    for (int i = 0; i < 8; i++) {
      RemoteSensorType type = stringToSensorType(sensors[i]);
      bool runtime = isSensorDataStreamingEnabled(type);
      bool saved = savedFlags[i];
      String status = runtime ? "STREAMING" : "off";
      if (saved && !runtime) status = "off (will auto-start)";
      else if (saved && runtime) status = "STREAMING (auto)";
      BROADCAST_PRINTF("    %s: %s", sensors[i], status.c_str());
    }
    return "OK: Streaming diagnostics displayed";
  }
  
  // Parse: bond stream <sensor> <on|off>
  int spaceIdx = args.indexOf(' ');
  if (spaceIdx < 0) {
    return "Usage: bond stream <sensor> <on|off>\n       bond stream (show status)";
  }
  
  String sensorName = args.substring(0, spaceIdx);
  sensorName.trim();
  sensorName.toLowerCase();
  
  String action = args.substring(spaceIdx + 1);
  action.trim();
  action.toLowerCase();
  
  // Validate sensor name
  RemoteSensorType sensorType = stringToSensorType(sensorName.c_str());
  if (strcmp(sensorTypeToString(sensorType), sensorName.c_str()) != 0) {
    return "Unknown sensor. Valid: thermal, tof, imu, gps, gamepad, fmradio, presence";
  }
  
  // Parse action
  bool enable = false;
  if (action == "on" || action == "1" || action == "start") {
    enable = true;
  } else if (action == "off" || action == "0" || action == "stop") {
    enable = false;
  } else {
    return "Usage: bond stream <sensor> <on|off>";
  }
  
  if (enable) {
    startSensorDataStreaming(sensorType);
  } else {
    stopSensorDataStreaming(sensorType);
  }
  
  // Update persistent settings
  switch (sensorType) {
    case REMOTE_SENSOR_THERMAL:  setSetting(gSettings.bondStreamThermal, enable); break;
    case REMOTE_SENSOR_TOF:      setSetting(gSettings.bondStreamTof, enable); break;
    case REMOTE_SENSOR_IMU:      setSetting(gSettings.bondStreamImu, enable); break;
    case REMOTE_SENSOR_GPS:      setSetting(gSettings.bondStreamGps, enable); break;
    case REMOTE_SENSOR_GAMEPAD:  setSetting(gSettings.bondStreamGamepad, enable); break;
    case REMOTE_SENSOR_FMRADIO:  setSetting(gSettings.bondStreamFmradio, enable); break;
    case REMOTE_SENSOR_RTC:      setSetting(gSettings.bondStreamRtc, enable); break;
    case REMOTE_SENSOR_PRESENCE: setSetting(gSettings.bondStreamPresence, enable); break;
    default: break;
  }
  BROADCAST_PRINTF("[ESP-NOW] %s streaming %s sensor data", enable ? "Started" : "Stopped", sensorName.c_str());
  
  return enable ? "OK: Started streaming sensor data to bonded master" : "OK: Stopped streaming sensor data";
}

/**
 * Test v3 sensor streaming: bond testsensor [type]
 * Sends a dummy sensor data packet to verify v3 communication
 */
const char* cmd_bond_testsensor(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gSettings.bondModeEnabled) {
    return "Not in bond mode. Use 'bond connect <device>' first.";
  }
  
  String args = argsIn;
  args.trim();
  
  // Remove command prefix if present
  if (args.startsWith("bond testsensor")) {
    args = args.substring(15);
    args.trim();
  }
  
  // Default to thermal sensor for testing
  RemoteSensorType testType = REMOTE_SENSOR_THERMAL;
  if (args.length() > 0) {
    testType = stringToSensorType(args.c_str());
  }
  
  broadcastOutput("[BOND_TEST] Testing v3 sensor data transmission...");
  BROADCAST_PRINTF("  Sensor type: %s (%d)", sensorTypeToString(testType), (int)testType);
  BROADCAST_PRINTF("  Role: %s", gSettings.bondRole == 1 ? "MASTER" : "WORKER");
  BROADCAST_PRINTF("  Peer MAC: %s", gSettings.bondPeerMac.c_str());
  BROADCAST_PRINTF("  isBondModeOnline(): %s", isBondModeOnline() ? "YES" : "NO");
  
  // Build a test JSON payload
  char testJsonBuf[128];
  snprintf(testJsonBuf, sizeof(testJsonBuf), "{\"test\":true,\"type\":\"%s\",\"timestamp\":%lu,\"value\":42.5}",
           sensorTypeToString(testType), (unsigned long)millis());
  String testJson = testJsonBuf;
  
  BROADCAST_PRINTF("  Test payload (%u bytes): %s", (unsigned)testJson.length(), testJson.c_str());
  
  bool sent = sendBondedSensorData((uint8_t)testType, 
                                    (const uint8_t*)testJson.c_str(), 
                                    (uint16_t)testJson.length());
  
  if (sent) {
    broadcastOutput("[BOND_TEST] SUCCESS: Test packet sent via v3");
    return "OK: Test sensor packet sent";
  } else {
    broadcastOutput("[BOND_TEST] FAILED: sendBondedSensorData returned false");
    return "FAILED: Could not send test packet (check debug output)";
  }
}

#endif // ENABLE_BONDED_MODE

// ============================================================================
// ESP-NOW Buffer Size Configuration Command
// ============================================================================

/**
 * Show/adjust ESP-NOW buffer sizes: espnow buffers [tx|rx|chunk|filechunk] [value]
 * Without args: show current settings
 * With args: adjust specific buffer size
 */
const char* cmd_espnow_buffers(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  String args = argsIn;
  args.trim();
  
  // No args: show current buffer settings
  if (args.length() == 0) {
    char* buf = getDebugBuffer();
    int pos = 0;
    pos += snprintf(buf + pos, 1024 - pos, "=== ESP-NOW Buffer Settings ===\n");
    pos += snprintf(buf + pos, 1024 - pos, "TX Queue Size:     %u (1-16, default: 8)\n", gSettings.espnowTxQueueSize);
    pos += snprintf(buf + pos, 1024 - pos, "RX Buffer Size:    %u (64-512, default: 256)\n", gSettings.espnowRxBufferSize);
    pos += snprintf(buf + pos, 1024 - pos, "Chunk Size:        %u (100-220, default: 200)\n", gSettings.espnowChunkSize);
    pos += snprintf(buf + pos, 1024 - pos, "File Chunk Size:   %u (100-224, default: 224)\n", gSettings.espnowFileChunkSize);
    pos += snprintf(buf + pos, 1024 - pos, "\nV3 Protocol Constants:\n");
    pos += snprintf(buf + pos, 1024 - pos, "  Max Payload:     %d bytes\n", ESPNOW_V3_MAX_PAYLOAD);
    pos += snprintf(buf + pos, 1024 - pos, "  Dedup Buffer:    %d entries\n", V3_DEDUP_SIZE);
    pos += snprintf(buf + pos, 1024 - pos, "\nNote: Changes take effect after ESP-NOW reinit or reboot.");
    return buf;
  }
  
  // Parse: espnow buffers <type> [value]
  int spaceIdx = args.indexOf(' ');
  String bufType = (spaceIdx >= 0) ? args.substring(0, spaceIdx) : args;
  bufType.trim();
  bufType.toLowerCase();
  
  // If no value provided, show just that setting
  if (spaceIdx < 0) {
    if (bufType == "tx") {
      snprintf(getDebugBuffer(), 1024, "TX Queue Size: %u (range: 1-16)", gSettings.espnowTxQueueSize);
    } else if (bufType == "rx") {
      snprintf(getDebugBuffer(), 1024, "RX Buffer Size: %u (range: 64-512)", gSettings.espnowRxBufferSize);
    } else if (bufType == "chunk") {
      snprintf(getDebugBuffer(), 1024, "Chunk Size: %u (range: 100-220)", gSettings.espnowChunkSize);
    } else if (bufType == "filechunk") {
      snprintf(getDebugBuffer(), 1024, "File Chunk Size: %u (range: 100-224)", gSettings.espnowFileChunkSize);
    } else {
      return "Usage: espnow buffers [tx|rx|chunk|filechunk] [value]";
    }
    return getDebugBuffer();
  }
  
  // Parse value
  String valStr = args.substring(spaceIdx + 1);
  valStr.trim();
  int value = valStr.toInt();
  
  // Set the appropriate buffer size
  if (bufType == "tx") {
    if (value < 1 || value > 16) return "Error: TX queue size must be 1-16";
    setSetting(gSettings.espnowTxQueueSize, (uint16_t)value);
    snprintf(getDebugBuffer(), 1024, "TX Queue Size set to %d (takes effect after reinit)", value);
  } else if (bufType == "rx") {
    if (value < 64 || value > 512) return "Error: RX buffer size must be 64-512";
    setSetting(gSettings.espnowRxBufferSize, (uint16_t)value);
    snprintf(getDebugBuffer(), 1024, "RX Buffer Size set to %d (takes effect after reinit)", value);
  } else if (bufType == "chunk") {
    if (value < 100 || value > 220) return "Error: Chunk size must be 100-220";
    setSetting(gSettings.espnowChunkSize, (uint16_t)value);
    snprintf(getDebugBuffer(), 1024, "Chunk Size set to %d (takes effect after reinit)", value);
  } else if (bufType == "filechunk") {
    if (value < 100 || value > 224) return "Error: File chunk size must be 100-224";
    setSetting(gSettings.espnowFileChunkSize, (uint16_t)value);
    snprintf(getDebugBuffer(), 1024, "File Chunk Size set to %d (takes effect after reinit)", value);
  } else {
    return "Unknown buffer type. Use: tx, rx, chunk, filechunk";
  }
  
  return getDebugBuffer();
}

// ============================================================================
// ESP-NOW Command Registry
// ============================================================================

extern const char* cmd_espnow_sensorstream(const String& cmd);
extern const char* cmd_espnow_sensorstatus(const String& cmd);
extern const char* cmd_espnow_sensorbroadcast(const String& cmd);
extern const char* cmd_espnow_usersync(const String& cmd);
// Device metadata commands
extern const char* cmd_espnow_room(const String& cmd);
extern const char* cmd_espnow_zone(const String& cmd);
extern const char* cmd_espnow_tags(const String& cmd);
extern const char* cmd_espnow_friendlyname(const String& cmd);
extern const char* cmd_espnow_stationary(const String& cmd);
extern const char* cmd_espnow_deviceinfo(const String& cmd);
// Master aggregation commands
extern const char* cmd_espnow_devices(const String& cmd);
extern const char* cmd_espnow_rooms(const String& cmd);
extern const char* cmd_espnow_find(const String& cmd);
extern const char* cmd_espnow_roomcmd(const String& cmd);
extern const char* cmd_espnow_tagcmd(const String& cmd);

extern const CommandEntry espNowCommands[] = {
  // ---- ESP-NOW Status & Statistics ----
  { "espnowread", "Read ESP-NOW status and configuration.", false, cmd_espnow_status },
  { "espnowstatus", "Show ESP-NOW status and configuration.", false, cmd_espnow_status },
  { "espnow stats", "Show ESP-NOW statistics (messages, errors, etc.).", false, cmd_espnow_stats },
  { "espnow routerstats", "Show message router statistics and metrics.", false, cmd_espnow_routerstats },
  { "espnow broadcaststats", "Show broadcast ACK tracking statistics.", false, cmd_espnow_broadcaststats },
  { "espnow resetstats", "Reset ESP-NOW statistics counters.", true, cmd_espnow_resetstats },
  
  // ---- ESP-NOW Initialization & Pairing ----
  { "openespnow", "Initialize ESP-NOW communication.", true, cmd_espnow_init },
  { "closeespnow", "Deinitialize ESP-NOW and free resources.", true, cmd_espnow_deinit },
  { "espnow pair", "Pair ESP-NOW device: 'espnow pair <mac> <name>'.", true, cmd_espnow_pair, "Usage: espnow pair <mac> <name>" },
  { "espnow unpair", "Unpair ESP-NOW device: 'espnow unpair <name_or_mac>'.", true, cmd_espnow_unpair, "Usage: espnow unpair <name_or_mac>" },
  { "espnow list", "List all paired ESP-NOW devices.", false, cmd_espnow_list },
  
  // ---- ESP-NOW Mesh Configuration ----
  { "espnow meshstatus", "Show mesh peer health (heartbeats & ACKs).", false, cmd_espnow_meshstatus },
  { "espnow meshmetrics", "Show mesh routing metrics (forwards, path stats, drops).", false, cmd_espnow_meshmetrics },
  { "espnow mode", "Get/set ESP-NOW mode: 'espnow mode [direct|mesh]'.", false, cmd_espnow_mode, "Usage: espnow mode [direct|mesh]" },
  { "espnow meshttl", "Get/set mesh TTL: 'espnow meshttl [1-10|adaptive]'.", false, cmd_espnow_meshttl },
  { "espnow setname", "Get/set device name: 'espnow setname [name]'.", false, cmd_espnow_setname },
  { "espnow hbmode", "Get/set heartbeat mode: 'espnow hbmode [public|private]'.", false, cmd_espnow_hbmode, "Usage: espnow hbmode [public|private]" },
  { "espnow meshrole", "Get/set mesh role: 'espnow meshrole [worker|master|backup]'.", false, cmd_espnow_meshrole, "Usage: espnow meshrole [worker|master|backup]" },
  { "espnow meshmaster", "Get/set master MAC: 'espnow meshmaster [MAC]'.", false, cmd_espnow_meshmaster },
  { "espnow meshbackup", "Get/set backup MAC: 'espnow meshbackup [MAC]'.", false, cmd_espnow_meshbackup },
  { "espnow backupenable", "Enable/disable backup master feature: 'espnow backupenable [on|off]'.", false, cmd_espnow_backupenable },
  { "espnow meshtopo", "Discover mesh topology (master only).", false, cmd_espnow_meshtopo },
  { "espnow toporesults", "Get topology discovery results.", false, cmd_espnow_toporesults },
  { "espnow timesync", "Broadcast NTP time to mesh (master only).", false, cmd_espnow_timesync },
  { "espnow timestatus", "Show time synchronization status.", false, cmd_espnow_timestatus },
  { "espnow meshsave", "Manually save mesh peer topology to filesystem.", false, cmd_espnow_meshsave },
  
  // ---- Device Metadata ----
  { "espnow room", "Get/set device room: 'espnow room [name]'.", false, cmd_espnow_room, "Usage: espnow room [Kitchen|Bedroom|...]\n       espnow room clear" },
  { "espnow zone", "Get/set device zone: 'espnow zone [name]'.", false, cmd_espnow_zone, "Usage: espnow zone [Counter|Door|Ceiling|...]\n       espnow zone clear" },
  { "espnow tags", "Get/set device tags: 'espnow tags [tag1,tag2,...]'.", false, cmd_espnow_tags, "Usage: espnow tags stationary,thermal\n       espnow tags clear" },
  { "espnow friendlyname", "Get/set friendly display name: 'espnow friendlyname [name]'.", false, cmd_espnow_friendlyname },
  { "espnow stationary", "Get/set stationary flag: 'espnow stationary [0|1]'.", false, cmd_espnow_stationary },
  { "espnow deviceinfo", "Show all local device metadata.", false, cmd_espnow_deviceinfo },
  
  // ---- Master Aggregation ----
  { "espnow devices", "List all mesh devices with room/zone/tags/status (master).", false, cmd_espnow_devices },
  { "espnow rooms", "List rooms and their devices (master).", false, cmd_espnow_rooms },
  { "espnow find", "Find devices by name, room, or tag: 'espnow find <query>'.", false, cmd_espnow_find, "Usage: espnow find <query>" },
  { "espnow roomcmd", "Run command on all devices in a room.", true, cmd_espnow_roomcmd, "Usage: espnow roomcmd <room> <user> <pass> <command>" },
  { "espnow tagcmd", "Run command on all devices with a tag.", true, cmd_espnow_tagcmd, "Usage: espnow tagcmd <tag> <user> <pass> <command>" },
  
  // ---- ESP-NOW Communication ----
  { "espnow send", "Send message (auto-routes via mesh if enabled): 'espnow send <name_or_mac> <message>'.", false, cmd_espnow_send, "Usage: espnow send <name_or_mac> <message>" },
  { "espnow broadcast", "Broadcast message: 'espnow broadcast <message>'.", false, cmd_espnow_broadcast, "Usage: espnow broadcast <message>" },
  { "espnow sendfile", "Send file: 'espnow sendfile <name_or_mac> <filepath>'.", false, cmd_espnow_sendfile, "Usage: espnow sendfile <name_or_mac> <filepath>" },
  { "espnow browse", "Browse remote files: 'espnow browse <name_or_mac> <user> <pass> [path]'.", false, cmd_espnow_browse, "Usage: espnow browse <target> <username> <password> [path]" },
  { "espnow fetch", "Fetch remote file: 'espnow fetch <name_or_mac> <user> <pass> <path>'.", false, cmd_espnow_fetch, "Usage: espnow fetch <target> <username> <password> <path>" },
  { "espnow remote", "Execute remote command: 'espnow remote <name_or_mac> <user> <pass> <cmd>'.", false, cmd_espnow_remote, "Usage: espnow remote <target> <username> <password> <command>" },
  { "openstream", "Start streaming all output to ESP-NOW caller (admin, remote only).", true, cmd_espnow_startstream },
  { "closestream", "Stop streaming output to ESP-NOW device (admin).", true, cmd_espnow_stopstream },
  { "espnow worker", "Configure worker status reporting: 'espnow worker [show|on|off|interval <ms>|fields <list>]'.", false, cmd_espnow_worker, "Usage: espnow worker [show|on|off|interval <ms>|fields <heap,rssi,thermal,imu>]" },
  { "espnow sensorstream", "Enable/disable sensor data streaming to master (worker only): 'espnow sensorstream <sensor> <on|off>'.", false, cmd_espnow_sensorstream },
  { "espnow sensorstatus", "Show remote sensor cache (master) or worker streaming status (worker).", false, cmd_espnow_sensorstatus },
  { "espnow sensorbroadcast", "Enable/disable all sensor ESP-NOW communication: 'espnow sensorbroadcast <on|off>'.", false, cmd_espnow_sensorbroadcast },
  { "espnow usersync", "Enable/disable user credential sync: 'espnow usersync [on|off]'.", false, cmd_espnow_usersync },
  { "espnow requestmeta", "Request metadata from peer: 'espnow requestmeta <name_or_mac>'.", false, cmd_espnow_requestmeta, "Usage: espnow requestmeta <name_or_mac>" },
  
#if ENABLE_BONDED_MODE
  // ---- Bond Mode Commands (1:1 handshake relationship) ----
  { "bond connect", "Connect to bonded peer device: 'bond connect <mac_or_name>'.", false, cmd_bond_connect, "Usage: bond connect <mac_or_name>" },
  { "bond disconnect", "Disconnect from bonded peer device.", false, cmd_bond_disconnect },
  { "bond status", "Show bond mode status and configuration.", false, cmd_bond_status },
  { "bond role", "Set bond mode role: 'bond role <master|worker>'.", false, cmd_bond_role, "Usage: bond role <master|worker>" },
  { "bond showcap", "Show local device capability summary.", false, cmd_bond_showcap },
  { "bond requestcap", "Request capability summary from bonded peer.", false, cmd_bond_requestcap },
  { "bond showmanifest", "Show local device manifest (UI apps + CLI commands).", false, cmd_bond_showmanifest },
  { "bond requestmanifest", "Request full manifest from bonded peer.", false, cmd_bond_requestmanifest },
  { "bond showremotemanifest", "Show cached remote manifest(s): 'bond showremotemanifest [fwHash]'.", false, cmd_bond_showremotemanifest },
  { "bond stream", "Stream sensor data to bonded master (worker only): 'bond stream <sensor> <on|off>'.", false, cmd_bond_stream, "Usage: bond stream <sensor> <on|off>\n       bond stream (show status)" },
  { "bond testsensor", "Test v3 sensor data transmission: 'bond testsensor [sensor_type]'.", false, cmd_bond_testsensor, "Usage: bond testsensor [thermal|tof|imu|gps|gamepad|fmradio]" },
#endif
  
  // ---- ESP-NOW Encryption ----
  { "espnow setpassphrase", "Set encryption passphrase: 'espnow setpassphrase \"phrase\"'.", true, cmd_espnow_setpassphrase, "Usage: espnow setpassphrase \"your_passphrase_here\"\n       espnow setpassphrase clear" },
  { "espnow encstatus", "Show ESP-NOW encryption status and key fingerprint.", true, cmd_espnow_encstatus },
  { "espnow pairsecure", "Pair device with encryption: 'espnow pairsecure <mac> <name>'.", true, cmd_espnow_pairsecure, "Usage: espnow pairsecure <mac_address> <device_name>" },
  
  // ---- ESP-NOW Testing Commands ----
  { "test streams", "Test topology stream management functions.", false, cmd_test_streams },
  { "test concurrent", "Test concurrent topology streams (simulated).", false, cmd_test_concurrent },
  { "test cleanup", "Test cleanup of stale topology streams.", false, cmd_test_cleanup },
  { "test filelock", "Test file transfer lock acquire/release.", false, cmd_test_filelock },
  
  // ---- ESP-NOW Settings ----
  { "espnowenabled", "Enable/disable ESP-NOW (0|1, takes effect after reboot).", true, cmd_espnowenabled },
  { "espnow buffers", "Show/adjust ESP-NOW buffer sizes: 'espnow buffers [tx|rx|chunk|filechunk] [value]'.", false, cmd_espnow_buffers },
};

extern const size_t espNowCommandsCount = sizeof(espNowCommands) / sizeof(espNowCommands[0]);

// Auto-register with command system
static CommandModuleRegistrar _espnow_cmd_registrar(espNowCommands, espNowCommandsCount, "espnow");

// ============================================================================
// ESP-NOW Settings Module (for modular settings registry)
// ============================================================================

static const SettingEntry espnowSettingEntries[] = {
  { "enabled",                    SETTING_BOOL,   &gSettings.espnowenabled,              true, 0, nullptr, 0, 1, "ESP-NOW Enabled", nullptr },
  { "mesh",                       SETTING_BOOL,   &gSettings.espnowmesh,                 true, 0, nullptr, 0, 1, "Mesh Mode", nullptr },
  { "userSyncEnabled",            SETTING_BOOL,   &gSettings.espnowUserSyncEnabled,      false, 0, nullptr, 0, 1, "User Sync Enabled", nullptr },
  { "deviceName",                 SETTING_STRING, &gSettings.espnowDeviceName,           0, 0, "", 0, 0, "Device Name", nullptr },
  { "room",                       SETTING_STRING, &gSettings.espnowRoom,                 0, 0, "", 0, 0, "Room", nullptr },
  { "zone",                       SETTING_STRING, &gSettings.espnowZone,                 0, 0, "", 0, 0, "Zone", nullptr },
  { "tags",                       SETTING_STRING, &gSettings.espnowTags,                 0, 0, "", 0, 0, "Tags", nullptr },
  { "friendlyName",               SETTING_STRING, &gSettings.espnowFriendlyName,         0, 0, "", 0, 0, "Friendly Name", nullptr },
  { "stationary",                 SETTING_BOOL,   &gSettings.espnowStationary,           false, 0, nullptr, 0, 1, "Stationary", nullptr },
  { "firstTimeSetup",             SETTING_BOOL,   &gSettings.espnowFirstTimeSetup,       false, 0, nullptr, 0, 1, "First Time Setup", nullptr },
  { "passphrase",                  SETTING_STRING, &gSettings.espnowPassphrase,           0, 0, "", 0, 0, "Passphrase", nullptr, true },
  { "meshRole",                   SETTING_INT,    &gSettings.meshRole,                   0, 0, nullptr, 0, 2, "Mesh Role", nullptr },
  { "masterMAC",                  SETTING_STRING, &gSettings.meshMasterMAC,              0, 0, "", 0, 0, "Master MAC", nullptr },
  { "backupMAC",                  SETTING_STRING, &gSettings.meshBackupMAC,              0, 0, "", 0, 0, "Backup MAC", nullptr },
  { "backupEnabled",              SETTING_BOOL,   &gSettings.meshBackupEnabled,           false, 0, nullptr, 0, 1, "Backup Master Enabled", nullptr },
  { "masterHeartbeatInterval",    SETTING_INT,    &gSettings.meshMasterHeartbeatInterval,10000, 0, nullptr, 1000, 60000, "Heartbeat Interval (ms)", nullptr },
  { "failoverTimeout",            SETTING_INT,    &gSettings.meshFailoverTimeout,        20000, 0, nullptr, 5000, 120000, "Failover Timeout (ms)", nullptr },
  { "workerStatusInterval",       SETTING_INT,    &gSettings.meshWorkerStatusInterval,   30000, 0, nullptr, 5000, 120000, "Worker Status Interval (ms)", nullptr },
  { "topoDiscoveryInterval",      SETTING_INT,    &gSettings.meshTopoDiscoveryInterval,  0, 0, nullptr, 0, 300000, "Topo Discovery Interval (ms)", nullptr },
  { "topoAutoRefresh",            SETTING_BOOL,   &gSettings.meshTopoAutoRefresh,        false, 0, nullptr, 0, 1, "Auto Refresh Topology", nullptr },
  { "heartbeatBroadcast",         SETTING_BOOL,   &gSettings.meshHeartbeatBroadcast,     false, 0, nullptr, 0, 1, "Heartbeat Broadcast", nullptr },
  { "meshTTL",                    SETTING_INT,    &gSettings.meshTTL,                    3, 0, nullptr, 1, 10, "TTL", nullptr },
  { "meshAdaptiveTTL",            SETTING_BOOL,   &gSettings.meshAdaptiveTTL,            false, 0, nullptr, 0, 1, "Adaptive TTL", nullptr },
  { "meshPeerMax",                SETTING_INT,    &gSettings.meshPeerMax,                8, 0, nullptr, 1, 16, "Max Peer Slots (reboot)", nullptr },
  { "sensorBroadcastIntervalMs",  SETTING_INT,    &gSettings.sensorBroadcastIntervalMs,  1000, 0, nullptr, 100, 10000, "Sensor Broadcast Interval (ms)", nullptr },
#if ENABLE_BONDED_MODE
  { "bondModeEnabled",          SETTING_BOOL,   &gSettings.bondModeEnabled,          false, 0, nullptr, 0, 1, "Bond Mode Enabled", nullptr },
  { "bondRole",                 SETTING_INT,    &gSettings.bondRole,                 0, 0, nullptr, 0, 1, "Bond Role", nullptr },
  { "bondPeerMac",              SETTING_STRING, &gSettings.bondPeerMac,              0, 0, "", 0, 0, "Bond Peer MAC", nullptr },
  { "bondStreamThermal",          SETTING_BOOL,   &gSettings.bondStreamThermal,          false, 0, nullptr, 0, 1, "Auto-stream Thermal", nullptr },
  { "bondStreamTof",              SETTING_BOOL,   &gSettings.bondStreamTof,              false, 0, nullptr, 0, 1, "Auto-stream ToF", nullptr },
  { "bondStreamImu",              SETTING_BOOL,   &gSettings.bondStreamImu,              false, 0, nullptr, 0, 1, "Auto-stream IMU", nullptr },
  { "bondStreamGps",              SETTING_BOOL,   &gSettings.bondStreamGps,              false, 0, nullptr, 0, 1, "Auto-stream GPS", nullptr },
  { "bondStreamGamepad",          SETTING_BOOL,   &gSettings.bondStreamGamepad,          false, 0, nullptr, 0, 1, "Auto-stream Gamepad", nullptr },
  { "bondStreamFmradio",          SETTING_BOOL,   &gSettings.bondStreamFmradio,          false, 0, nullptr, 0, 1, "Auto-stream FM Radio", nullptr },
  { "bondStreamRtc",              SETTING_BOOL,   &gSettings.bondStreamRtc,              false, 0, nullptr, 0, 1, "Auto-stream RTC", nullptr },
  { "bondStreamPresence",         SETTING_BOOL,   &gSettings.bondStreamPresence,         false, 0, nullptr, 0, 1, "Auto-stream Presence", nullptr },
#endif
  // Buffer size settings (requires reinit to take effect)
  { "txQueueSize",                SETTING_INT,    (int*)&gSettings.espnowTxQueueSize,    8, 0, nullptr, 1, 16, "TX Queue Size", nullptr },
  { "rxBufferSize",               SETTING_INT,    (int*)&gSettings.espnowRxBufferSize,   256, 0, nullptr, 64, 512, "RX Buffer Size", nullptr },
  { "chunkSize",                  SETTING_INT,    (int*)&gSettings.espnowChunkSize,      200, 0, nullptr, 100, 220, "Chunk Size", nullptr },
  { "fileChunkSize",              SETTING_INT,    (int*)&gSettings.espnowFileChunkSize,  224, 0, nullptr, 100, 224, "File Chunk Size", nullptr }
};

extern const SettingsModule espnowSettingsModule = {
  "espnow", "espnow", espnowSettingEntries,
  sizeof(espnowSettingEntries) / sizeof(espnowSettingEntries[0])
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================
// ESP-NOW User Sync Toggle Command (merged from System_ESPNow_UserSync.cpp)
// ============================================================================

/**
 * Toggle user sync feature: espnow usersync [on|off]
 */
const char* cmd_espnow_usersync(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  String args = argsIn;
  args.trim();
  args.toLowerCase();
  
  if (args.length() == 0) {
    // Show current status
    snprintf(getDebugBuffer(), 1024, "User sync: %s", 
             gSettings.espnowUserSyncEnabled ? "ENABLED" : "DISABLED");
    return getDebugBuffer();
  }
  
  if (args == "on" || args == "1" || args == "true" || args == "enable") {
    setSetting(gSettings.espnowUserSyncEnabled, true);
    INFO_ESPNOWF("[USER_SYNC] User sync ENABLED");
    return "User sync ENABLED - admins can now sync users across devices";
  } else if (args == "off" || args == "0" || args == "false" || args == "disable") {
    setSetting(gSettings.espnowUserSyncEnabled, false);
    INFO_ESPNOWF("[USER_SYNC] User sync DISABLED");
    return "User sync DISABLED - credential propagation blocked";
  } else {
    return "Usage: espnow usersync [on|off]";
  }
}

// ============================================================================
// Per-Device Message Buffer Management (merged from espnow_message_buffer.cpp)
// ============================================================================

// Helper: Find or create peer message history for a given MAC address
PeerMessageHistory* findOrCreatePeerHistory(uint8_t* peerMac) {
  if (!gEspNow || !gEspNow->peerMessageHistories) return nullptr;
  
  // First, try to find existing history for this peer
  for (int i = 0; i < gMeshPeerSlots; i++) {
    PeerMessageHistory& history = gEspNow->peerMessageHistories[i];
    if (history.active && memcmp(history.peerMac, peerMac, 6) == 0) {
      return &history;
    }
  }
  
  // Not found, create new entry
  for (int i = 0; i < gMeshPeerSlots; i++) {
    PeerMessageHistory& history = gEspNow->peerMessageHistories[i];
    if (!history.active) {
      memcpy(history.peerMac, peerMac, 6);
      history.head = 0;
      history.tail = 0;
      history.count = 0;
      history.active = true;
      return &history;
    }
  }
  
  // No free slots
  return nullptr;
}

// Store a message in the per-device buffer
bool storeMessageInPeerHistory(
  uint8_t* peerMac,
  const char* peerName,
  const char* message,
  bool encrypted,
  LogMessageType msgType
) {
  if (!gEspNow) return false;
  
  PeerMessageHistory* history = findOrCreatePeerHistory(peerMac);
  if (!history) {
    broadcastOutput("[ESP-NOW] ERROR: No free peer history slots");
    return false;
  }
  
  // Get the next slot in the ring buffer
  ReceivedTextMessage& slot = history->messages[history->head];
  
  // Copy data
  memcpy(slot.senderMac, peerMac, 6);
  strncpy(slot.senderName, peerName, 31);
  slot.senderName[31] = '\0';
  
  size_t msgLen = strlen(message);
  size_t copyLen = msgLen < 255 ? msgLen : 255;
  memcpy(slot.message, message, copyLen);
  slot.message[copyLen] = '\0';
  
  slot.timestamp = millis();
  slot.encrypted = encrypted;
  slot.seqNum = ++gEspNow->globalMessageSeqNum;
  slot.msgType = msgType;
  slot.active = true;
  
  // Advance head pointer
  history->head = (history->head + 1) % MESSAGES_PER_DEVICE;
  
  // If buffer is full, advance tail (drop oldest message)
  if (history->count < MESSAGES_PER_DEVICE) {
    history->count++;
  } else {
    history->tail = (history->tail + 1) % MESSAGES_PER_DEVICE;
  }
  
  return true;
}

// Log a file transfer event to the message buffer
void logFileTransferEvent(
  uint8_t* peerMac,
  const char* peerName,
  const char* filename,
  LogMessageType eventType
) {
  if (!gEspNow) return;
  
  char message[128];
  
  switch (eventType) {
    case MSG_FILE_SEND_START:
      snprintf(message, sizeof(message), "Sending file: %s", filename);
      break;
    case MSG_FILE_SEND_SUCCESS:
      snprintf(message, sizeof(message), "File sent: %s", filename);
      break;
    case MSG_FILE_SEND_FAILED:
      snprintf(message, sizeof(message), "Failed to send: %s", filename);
      break;
    case MSG_FILE_RECV_SUCCESS:
      snprintf(message, sizeof(message), "Received file: %s", filename);
      break;
    case MSG_FILE_RECV_FAILED:
      snprintf(message, sizeof(message), "Failed to receive: %s", filename);
      break;
    default:
      return;
  }
  
  storeMessageInPeerHistory(peerMac, peerName, message, false, eventType);
  
  // Also broadcast to serial/web for immediate visibility
  String deviceName = String(peerName);
  if (deviceName.length() == 0) {
    deviceName = formatMacAddress(peerMac);
  }
  BROADCAST_PRINTF("[ESP-NOW] %s: %s", deviceName.c_str(), message);
}

// Get all messages for a specific peer (for web UI API)
int getPeerMessages(uint8_t* peerMac, ReceivedTextMessage* outMessages, int maxMessages, uint32_t sinceSeq) {
  if (!gEspNow || !outMessages) return 0;
  
  PeerMessageHistory* history = findOrCreatePeerHistory(peerMac);
  if (!history || history->count == 0) return 0;
  
  int copied = 0;
  
  // Walk through ring buffer from tail to head
  for (int i = 0; i < history->count && copied < maxMessages; i++) {
    uint8_t idx = (history->tail + i) % MESSAGES_PER_DEVICE;
    ReceivedTextMessage& msg = history->messages[idx];
    
    if (!msg.active) continue;
    if (msg.seqNum <= sinceSeq) continue;
    
    // Copy message to output array
    memcpy(&outMessages[copied], &msg, sizeof(ReceivedTextMessage));
    copied++;
  }
  
  return copied;
}

// Get all messages from all peers (for global view)
int getAllMessages(ReceivedTextMessage* outMessages, int maxMessages, uint32_t sinceSeq) {
  if (!gEspNow || !outMessages || !gEspNow->peerMessageHistories) return 0;
  
  int copied = 0;
  
  // Collect messages from all peer histories
  for (int p = 0; p < gMeshPeerSlots && copied < maxMessages; p++) {
    PeerMessageHistory& history = gEspNow->peerMessageHistories[p];
    if (!history.active || history.count == 0) continue;
    
    for (int i = 0; i < history.count && copied < maxMessages; i++) {
      uint8_t idx = (history.tail + i) % MESSAGES_PER_DEVICE;
      ReceivedTextMessage& msg = history.messages[idx];
      
      if (!msg.active) continue;
      if (msg.seqNum <= sinceSeq) continue;
      
      memcpy(&outMessages[copied], &msg, sizeof(ReceivedTextMessage));
      copied++;
    }
  }
  
  // Sort by sequence number (simple bubble sort, good enough for small arrays)
  for (int i = 0; i < copied - 1; i++) {
    for (int j = 0; j < copied - i - 1; j++) {
      if (outMessages[j].seqNum > outMessages[j + 1].seqNum) {
        ReceivedTextMessage temp = outMessages[j];
        outMessages[j] = outMessages[j + 1];
        outMessages[j + 1] = temp;
      }
    }
  }
  
  return copied;
}

#endif // ENABLE_ESPNOW
