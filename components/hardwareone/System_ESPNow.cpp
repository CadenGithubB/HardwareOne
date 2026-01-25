/**
 * ESP-NOW System - COMPLETE Implementation
 * Extracted from HardwareOnev2.1.ino
 */

#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

#include <ArduinoJson.h>
#include <esp_wifi.h>
#include <LittleFS.h>
#include <Preferences.h>
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

#include "OLED_Display.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_ESPNow.h"
#include "System_ESPNow_Sensors.h"
#include "System_MemUtil.h"
#include "System_Mutex.h"
#include "System_SensorStubs.h"
#include "System_Settings.h"
#include "System_UserSettings.h"
#include "System_Utils.h"
#include "WebServer_Server.h"

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
extern void printToTFT(const String& s);
extern volatile uint32_t gOutputFlags;
extern bool gCLIValidateOnly;
// gDebugBuffer, gDebugFlags, ensureDebugBuffer now from debug_system.h

// Debug flags (defined in .ino)
// Removed local DEBUG_HTTP define; use debug_system.h as single source of truth

// V2 protocol is now mandatory - no runtime toggles
// Fragmentation, reliability, and deduplication are always enabled
static bool gV2LogEnabled = false;  // runtime log toggle for v2 decoding
// Deduplication is MANDATORY - no runtime toggle (required for mesh flood forwarding)
struct V2DedupEntry { uint8_t src[6]; uint32_t id; uint32_t ts; bool active; };
#define V2_DEDUP_SIZE 32
static V2DedupEntry gV2Dedup[V2_DEDUP_SIZE];
static int gV2DedupIdx = 0;
// Forward declaration for ACK sender used by fragment reassembly
static void v2_send_ack(const uint8_t* dst, uint32_t id);

// Ack wait table (small, lock-free)
struct V2AckWait { uint32_t id; volatile bool got; uint32_t ts; bool active; };
#define V2_ACK_WAIT_MAX 8
static V2AckWait gV2AckWait[V2_ACK_WAIT_MAX];
static int v2_ack_wait_register(uint32_t id) {
  // reuse slot with same id if present
  for (int i = 0; i < V2_ACK_WAIT_MAX; i++) {
    if (gV2AckWait[i].active && gV2AckWait[i].id == id) {
      gV2AckWait[i].got = false; gV2AckWait[i].ts = millis();
      broadcastOutput("[ACK_WAIT] Reusing slot " + String(i) + " for msgId=" + String((unsigned long)id));
      return i;
    }
  }
  for (int i = 0; i < V2_ACK_WAIT_MAX; i++) {
    if (!gV2AckWait[i].active) {
      gV2AckWait[i].active = true;
      gV2AckWait[i].id = id;
      gV2AckWait[i].got = false;
      gV2AckWait[i].ts = millis();
      broadcastOutput("[ACK_WAIT] Registered slot " + String(i) + " for msgId=" + String((unsigned long)id));
      return i;
    }
  }
  broadcastOutput("[ACK_WAIT] ERROR: All slots full, cannot register msgId=" + String((unsigned long)id));
  return -1;
}
static bool v2_ack_wait_block(uint32_t id, uint32_t timeoutMs) {
  broadcastOutput("[ACK_WAIT] Blocking for msgId=" + String((unsigned long)id) + " timeout=" + String(timeoutMs) + "ms");
  unsigned long start = millis();
  while ((millis() - start) < timeoutMs) {
    for (int i = 0; i < V2_ACK_WAIT_MAX; i++) {
      if (gV2AckWait[i].active && gV2AckWait[i].id == id && gV2AckWait[i].got) {
        broadcastOutput("[ACK_WAIT] ✓ ACK received for msgId=" + String((unsigned long)id) + " after " + String(millis() - start) + "ms");
        return true;
      }
    }
    // Use vTaskDelay to properly yield to RTOS scheduler and feed watchdog
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  broadcastOutput("[ACK_WAIT] ✗ TIMEOUT waiting for msgId=" + String((unsigned long)id) + " after " + String(timeoutMs) + "ms");
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
bool routerSend(Message& msg);

// Forward declaration for internal addEspNowPeerWithEncryption (different from extern version in .ino)
static bool addEspNowPeerWithEncryption(const uint8_t* mac, bool useEncryption, const uint8_t* encryptionKey);

// Forward declarations for structures from main .ino
// AuthContext and CommandSource now in user_system.h
struct CommandContext;
struct ConnectedDevice;

// External globals for device tracking
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

// Note: gEspNow is defined in HardwareOnev2.1.ino as non-static (extern in header)

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
  bool includeRssi = true;          // Include WiFi RSSI
  bool includeThermal = false;      // Include thermal sensor status
  bool includeImu = false;          // Include IMU sensor status
};
static WorkerStatusConfig gWorkerStatusConfig;

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

MeshPeerHealth gMeshPeers[MESH_PEER_MAX];
uint32_t gLastHeartbeatSentMs = 0;
static MeshRetryEntry gMeshRetryQueue[MESH_RETRY_QUEUE_SIZE];
// Note: gMeshRetryMutex is now defined in mutex_system.cpp
// Send flow control
[[maybe_unused]] static bool isEspNowInitializedFlag() {
  return gEspNow && gEspNow->initialized;
}
static const int FILE_ACK_INTERVAL = 10;       // send ACK every N chunks

// ESP-NOW chunked message support
#define MAX_CHUNKS 20              // For remote command responses (buffered in RAM)
#define MAX_FILE_CHUNKS 4096       // For file transfers (written directly to disk, no RAM buffer)
#define CHUNK_SIZE 200             // Generic message chunk size (fits ESP-NOW payload)
#define FILE_CHUNK_DATA_BYTES 150  // Raw bytes per FILE chunk before base64 (keeps packet <= ~230B)
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
  char hash[16];            // Transfer hash for validation
  unsigned long startTime;  // Transfer start time
  bool active;              // Transfer in progress
  uint8_t senderMac[6];     // MAC address of sender
};
static FileTransfer* gActiveFileTransfer = nullptr;
static File gActiveFileTransferFile;  // File handle kept separate to avoid copy issues

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

#define V2LOG(flag, fmt, ...) do { if (gV2LogEnabled) { DEBUGF(flag, fmt, ##__VA_ARGS__); } } while (0)

struct EspNowV2InboundPacket {
  const esp_now_recv_info* info;
  const uint8_t* data;
  int len;
  uint32_t recvMs;
};

enum EspNowV2Kind {
  V2K_Unknown = 0,
  V2K_Command,
  V2K_File,
  V2K_Topology,
  V2K_Time,
  V2K_Ack,
  V2K_Heartbeat,
  V2K_MeshSys
};

struct EspNowV2Message {
  uint8_t v;
  EspNowV2Kind kind;
  uint32_t id;
  uint8_t ttl;
  String src;
  String dst;
};

static bool v2_decode_message_string(const String& s, EspNowV2Message& out) {
  out.v = 0;
  out.kind = V2K_Unknown;
  out.id = 0;
  out.ttl = 0;
  out.src = "";
  out.dst = "";
  if (!s.startsWith("{")) return false;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, s);
  if (err) return false;
  out.v = (uint8_t)(doc["v"] | (uint8_t)1);
  const char* k = doc["k"] | doc["type"] | "";
  if (strcmp(k, "cmd") == 0) out.kind = V2K_Command;
  else if (strcmp(k, "File") == 0) out.kind = V2K_File;
  else if (strcmp(k, "topo") == 0 || strcmp(k, PAYLOAD_TOPO_REQ) == 0 || strcmp(k, PAYLOAD_TOPO_RESP) == 0) out.kind = V2K_Topology;
  else if (strcmp(k, PAYLOAD_TIME_SYNC) == 0 || strcmp(k, "time") == 0) out.kind = V2K_Time;
  else if (strcmp(k, MSG_TYPE_ACK) == 0 || strcmp(k, "ack") == 0) out.kind = V2K_Ack;
  else if (strcmp(k, MSG_TYPE_HB) == 0 || strcmp(k, "hb") == 0) out.kind = V2K_Heartbeat;
  else if (strcmp(k, MSG_TYPE_MESH_SYS) == 0) out.kind = V2K_MeshSys;
  else out.kind = V2K_Unknown;
  out.id = (uint32_t)(doc["id"] | doc["msgId"] | 0);
  out.ttl = (uint8_t)(doc["ttl"] | 0);
  out.src = (const char*)(doc["src"] | "");
  out.dst = (const char*)(doc["dst"] | "");
  return true;
}

static bool v2_handle_incoming(const EspNowV2InboundPacket& pkt) {
  if (!gV2LogEnabled) return false;
  if (!pkt.info) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_RX] ERROR: pkt.info is NULL");
    return false;
  }
  // Create String directly from buffer to avoid character-by-character concatenation
  int msgLen = pkt.len < 250 ? pkt.len : 250;
  char* tempBuf = (char*)malloc(msgLen + 1);
  String s;
  if (tempBuf) {
    memcpy(tempBuf, pkt.data, msgLen);
    tempBuf[msgLen] = '\0';
    s = String(tempBuf);
    free(tempBuf);
  }
  EspNowV2Message m;
  if (v2_decode_message_string(s, m)) {
    String src = macToHexString(pkt.info->src_addr);
    V2LOG(DEBUG_ESPNOW_ROUTER, "[V2_RX] v=%u kind=%d id=%lu ttl=%u src=%s dst=%s",
          (unsigned)m.v, (int)m.kind, (unsigned long)m.id, (unsigned)m.ttl,
          src.c_str(), m.dst.c_str());
  } else {
    String src = macToHexString(pkt.info->src_addr);
    V2LOG(DEBUG_ESPNOW_ROUTER, "[V2_RX] undecoded len=%d src=%s", pkt.len, src.c_str());
  }
  return false;
}

// --------------------------
// V2 Fragmentation Reassembly (JSON v1 fragments)
// --------------------------

#define V2_REASM_MAX 4
#define V2_FRAG_MAX 32
#define V2_REASM_TIMEOUT_MS 5000

struct V2ReasmEntry {
  bool active;
  uint8_t src[6];
  uint32_t id;
  uint16_t n;
  uint16_t received;
  uint32_t startMs;
  String parts[V2_FRAG_MAX];
  bool have[V2_FRAG_MAX];
};

static V2ReasmEntry gV2Reasm[V2_REASM_MAX];

static void v2_reasm_reset(V2ReasmEntry& e) {
  e.active = false;
  e.id = 0;
  e.n = 0;
  e.received = 0;
  e.startMs = 0;
  memset(e.src, 0, 6);
  for (int i = 0; i < V2_FRAG_MAX; i++) {
    e.parts[i] = "";
    e.have[i] = false;
  }
}

static void v2_reasm_gc(uint32_t nowMs) {
  for (int i = 0; i < V2_REASM_MAX; i++) {
    if (gV2Reasm[i].active && (nowMs - gV2Reasm[i].startMs) > V2_REASM_TIMEOUT_MS) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG] GC timeout id=%lu from %02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned long)gV2Reasm[i].id,
             gV2Reasm[i].src[0], gV2Reasm[i].src[1], gV2Reasm[i].src[2],
             gV2Reasm[i].src[3], gV2Reasm[i].src[4], gV2Reasm[i].src[5]);
      if (gEspNow) { gEspNow->routerMetrics.v2FragRxGc++; }
      v2_reasm_reset(gV2Reasm[i]);
    }
  }
}

static V2ReasmEntry* v2_reasm_find_or_alloc(const uint8_t* src, uint32_t id, uint16_t n) {
  // Find
  for (int i = 0; i < V2_REASM_MAX; i++) {
    if (gV2Reasm[i].active && gV2Reasm[i].id == id && memcmp(gV2Reasm[i].src, src, 6) == 0) {
      return &gV2Reasm[i];
    }
  }
  // Alloc
  for (int i = 0; i < V2_REASM_MAX; i++) {
    if (!gV2Reasm[i].active) {
      v2_reasm_reset(gV2Reasm[i]);
      gV2Reasm[i].active = true;
      memcpy(gV2Reasm[i].src, src, 6);
      gV2Reasm[i].id = id;
      gV2Reasm[i].n = n;
      gV2Reasm[i].received = 0;
      gV2Reasm[i].startMs = millis();
      return &gV2Reasm[i];
    }
  }
  return nullptr;
}

// Try to reassemble a v2-fragmented JSON message. If a complete payload is assembled,
// returns true and sets outCompleted to the reconstructed JSON string.
static bool v2_frag_try_reassembly(const esp_now_recv_info* recv_info, const String& s, String& outCompleted) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, s);
  if (err) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_REASM] JSON parse error, not a fragment");
    return false;
  }
  JsonObject frag = doc["frag"].as<JsonObject>();
  if (!frag || frag.isNull()) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_REASM] No 'frag' field, not a fragment");
    return false;
  }
  uint32_t id = (uint32_t)(doc["id"] | doc["msgId"] | 0);
  uint16_t i = (uint16_t)(frag["i"] | 0);
  uint16_t n = (uint16_t)(frag["n"] | 0);
  const char* data = doc["data"] | "";
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_REASM] Fragment detected: id=%lu, i=%u, n=%u",
         (unsigned long)id, (unsigned)i, (unsigned)n);
  
  if (n == 0 || i >= n) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_REASM] Invalid fragment indices: i=%u, n=%u", (unsigned)i, (unsigned)n);
    return false;
  }
  if (n > V2_FRAG_MAX) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG] n=%u exceeds max=%u, dropping", (unsigned)n, (unsigned)V2_FRAG_MAX);
    return false;
  }

  v2_reasm_gc(millis());

  V2ReasmEntry* e = v2_reasm_find_or_alloc(recv_info->src_addr, id, n);
  if (!e) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_REASM] ERROR: No reassembly slot available (max=%u)", (unsigned)V2_REASM_MAX);
    return false;
  }

  if (!e->have[i]) {
    e->parts[i] = data;
    e->have[i] = true;
    if (gEspNow) { gEspNow->routerMetrics.v2FragRx++; }
    e->received++;
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_REASM] Stored fragment %u/%u (id=%lu, received=%u/%u)",
           (unsigned)i+1, (unsigned)n, (unsigned long)id, (unsigned)e->received, (unsigned)e->n);
  } else {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_REASM] Duplicate fragment %u/%u (id=%lu), ignoring",
           (unsigned)i+1, (unsigned)n, (unsigned long)id);
  }

  if (e->received < e->n) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_REASM] Waiting for more fragments: %u/%u received",
           (unsigned)e->received, (unsigned)e->n);
    return false;  // not complete yet
  }
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_REASM] All fragments received! Reassembling...");
  
  // Calculate total length and reserve space to avoid repeated allocations
  size_t totalLen = 0;
  for (uint16_t idx = 0; idx < e->n; idx++) {
    totalLen += e->parts[idx].length();
  }
  
  // Concatenate raw JSON segments to final message string
  String reconstructed;
  reconstructed.reserve(totalLen + 1);
  for (uint16_t idx = 0; idx < e->n; idx++) {
    reconstructed += e->parts[idx];
  }
  outCompleted = reconstructed;
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG] Reassembly complete: id=%lu, bytes=%d",
         (unsigned long)e->id, reconstructed.length());
  // Send ACK for the completed message
  v2_send_ack(recv_info->src_addr, e->id);
  if (gEspNow) { gEspNow->routerMetrics.v2FragRxCompleted++; }
  v2_reasm_reset(*e);
  return true;
}

// --------------------------
// V2 Reliability (Ack/Dedup)
// --------------------------
static bool v2_dedup_seen_and_insert(const uint8_t* src, uint32_t id) {
  if (id == 0) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_DEDUP] Skipping dedup check for id=0");
    return false;
  }
  
  String srcMac = formatMacAddress(src);
  
  // search
  for (int i = 0; i < V2_DEDUP_SIZE; i++) {
    if (gV2Dedup[i].active && gV2Dedup[i].id == id && memcmp(gV2Dedup[i].src, src, 6) == 0) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_DEDUP] ✗ DUPLICATE DETECTED: id=%lu from %s",
             (unsigned long)id, srcMac.c_str());
      return true; // duplicate
    }
  }
  
  // insert/replace
  V2DedupEntry& e = gV2Dedup[gV2DedupIdx];
  memcpy(e.src, src, 6);
  e.id = id;
  e.ts = millis();
  e.active = true;
  gV2DedupIdx = (gV2DedupIdx + 1) % V2_DEDUP_SIZE;
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_DEDUP] ✓ New message: id=%lu from %s (stored in slot %d)",
         (unsigned long)id, srcMac.c_str(), gV2DedupIdx - 1);
  
  return false;
}

static void v2_send_ack(const uint8_t* dst, uint32_t id) {
  String dstMac = formatMacAddress(dst);
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_ACK_TX] Sending ACK for id=%lu to %s",
         (unsigned long)id, dstMac.c_str());
  
  JsonDocument doc;
  doc["v"] = 2;
  doc["k"] = "ack";
  doc["id"] = id;
  String frame; serializeJson(doc, frame);
  
  if (gEspNow) { gEspNow->routerMetrics.v2AckTx++; }
  
  broadcastOutput("[ACK_TX] Sending ACK frame: " + frame + " to " + dstMac);
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_ACK_TX] ACK frame: %s", frame.c_str());
  gEspNow->txDone = false;
  yield();
  esp_err_t result = esp_now_send(dst, (uint8_t*)frame.c_str(), frame.length());
  if (result != ESP_OK) {
    broadcastOutput("[ACK_TX] ERROR: esp_now_send failed with code " + String(result));
  }
}

static bool v2_try_handle_ack(const String& s) {
  if (!s.startsWith("{")) return false;
  JsonDocument doc; if (deserializeJson(doc, s)) return false;
  if (doc["k"].isNull()) return false;
  const char* k = doc["k"] | "";
  if (strcmp(k, "ack") == 0) {
    if (gEspNow) { gEspNow->routerMetrics.v2AckRx++; }
    uint32_t id = doc["id"] | 0;
    broadcastOutput("[ACK_RX] Received ACK for msgId=" + String((unsigned long)id));
    if (id != 0) {
      bool found = false;
      for (int i = 0; i < V2_ACK_WAIT_MAX; i++) {
        if (gV2AckWait[i].active && gV2AckWait[i].id == id) {
          gV2AckWait[i].got = true;
          found = true;
          broadcastOutput("[ACK_RX] Matched waiter slot " + String(i) + " for msgId=" + String((unsigned long)id));
          break;
        }
      }
      if (!found) {
        broadcastOutput("[ACK_RX] WARNING: No active waiter found for msgId=" + String((unsigned long)id));
      }
    }
    return true;
  }
  return false;
}

// --------------------------
// Time Synchronization Helpers
// --------------------------

// Get current epoch time in seconds (returns 0 if not synced)
static uint32_t getEpochTime() {
  if (!gTimeIsSynced) return 0;
  return (uint32_t)((millis() + gTimeOffset) / 1000);
}

// Get current epoch time in milliseconds (returns 0 if not synced)
[[maybe_unused]] static uint64_t getEpochTimeMs() {
  if (!gTimeIsSynced) return 0;
  return (uint64_t)(millis() + gTimeOffset);
}

// Helper: Initialize unified v2 logical envelope
// Schema (logical intent):
// {
//   "v": 2,
//   "id": <msgId>,        // global logical message id
//   "msgId": <msgId>,     // legacy alias used by existing handlers
//   "src": "<src>",
//   "dst": "<dst>",
//   "ttl": <ttl>,
//   "path": ["<mac1>", "<mac2>", ...],  // MACs that have handled this message (for mesh loop prevention)
//   "type": "<type>",
//   ... (pld/msg/etc. populated by caller)
// }
void v2_init_envelope(JsonDocument& doc,
                      const char* type,
                      uint32_t msgId,
                      const char* src,
                      const char* dst,
                      int ttl) {
  doc["v"] = 2;
  doc["type"] = type;
  if (msgId == 0) {
    msgId = (uint32_t)millis();
    broadcastOutput("[MSG_ID] Generated new msgId=" + String((unsigned long)msgId) + " for type=" + String(type));
  }
  doc["id"] = msgId;
  doc["msgId"] = msgId;  // keep msgId for existing consumers

  if (src && src[0]) {
    doc["src"] = src;
  }

  if (dst && dst[0]) {
    doc["dst"] = dst;
  }

  if (ttl >= 0) {
    doc["ttl"] = ttl;
  }
}

// Helper: Build time sync message
// Example: {"v":2,"type":"MESH_SYS","id":123,"msgId":123,
//           "src":"...","dst":"broadcast",
//           "pld":{"kind":"timeSync","epoch":1700000000,"millis":123456}}
// Note: TTL will be added by routerSend() when mesh routing is used
static String buildTimeSyncMessage(uint32_t msgId, const char* src) {
  JsonDocument doc;
  // Time sync is a mesh-broadcast system message
  v2_init_envelope(doc, MSG_TYPE_MESH_SYS, msgId, src, "broadcast", -1);
  
  JsonObject payload = doc["pld"].to<JsonObject>();
  payload["kind"] = PAYLOAD_TIME_SYNC;
  payload["epoch"] = (uint32_t)time(nullptr);  // Current epoch time from NTP
  payload["millis"] = (uint32_t)millis();      // Current millis() for offset calculation
  
  String output;
  serializeJson(doc, output);
  return output;
}

// --------------------------
// Mesh peer health tracking (heartbeat & ACK monitoring)
// --------------------------
// Note: MESH_PEER_MAX, MESH_PEER_TIMEOUT_MS, and MeshPeerHealth struct are now in espnow_system.h
// Duplicate declarations removed to avoid redefinition errors
extern const uint32_t MESH_HEARTBEAT_INTERVAL_MS = 10000;  // Send heartbeat every 10 seconds

// Forward declarations for retry queue functions (defined after variable declarations)
static bool meshRetryEnqueue(uint32_t msgId, const uint8_t dstMac[6], const String& envelope);
static void meshRetryDequeue(uint32_t msgId);
static void meshRetryProcess();

// Helper: Find or create peer health entry
MeshPeerHealth* getMeshPeerHealth(const uint8_t mac[6], bool createIfMissing) {
  // First, try to find existing entry
  for (int i = 0; i < MESH_PEER_MAX; i++) {
    if (gMeshPeers[i].isActive && macEqual6(gMeshPeers[i].mac, mac)) {
      return &gMeshPeers[i];
    }
  }

  // If not found and createIfMissing, find empty slot
  if (createIfMissing) {
    for (int i = 0; i < MESH_PEER_MAX; i++) {
      if (!gMeshPeers[i].isActive) {
        memcpy(gMeshPeers[i].mac, mac, 6);
        gMeshPeers[i].lastHeartbeatMs = millis();
        gMeshPeers[i].lastAckMs = 0;
        gMeshPeers[i].heartbeatCount = 0;
        gMeshPeers[i].ackCount = 0;
        gMeshPeers[i].isActive = true;
        
        // Save peer list to filesystem when new peer discovered (topology change)
        // Only save if this is not the self-entry (self-entry is transient)
        if (!isSelfMac(mac)) {
          // Note: Can't use DEBUGF here as debug flags not yet defined
          // Debug output will be available later in the mesh system
          saveMeshPeers();
        }
        
        return &gMeshPeers[i];
      }
    }
  }

  return nullptr;  // Not found and no space
}

// Helper: Check if peer is alive (received heartbeat within timeout)
bool isMeshPeerAlive(const MeshPeerHealth* peer) {
  if (!peer || !peer->isActive) return false;
  uint32_t now = millis();
  uint32_t elapsed = now - peer->lastHeartbeatMs;
  // Handle millis() rollover
  if (elapsed > 0x80000000UL) elapsed = 0;
  return elapsed < MESH_PEER_TIMEOUT_MS;
}

void macFromHexString(const String& s, uint8_t out[6]) {
  // Accept formats like "aa:bb:cc:dd:ee:ff" or "aabbccddeeff"
  int idx = 0;
  int oi = 0;
  while (oi < 6 && idx < (int)s.length()) {
    while (idx < (int)s.length() && s[idx] == ':') idx++;
    int hi = idx < (int)s.length() ? s[idx++] : '0';
    int lo = idx < (int)s.length() ? s[idx++] : '0';
    auto hexVal = [](int c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      c |= 32;
      if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
      return 0;
    };
    out[oi++] = (uint8_t)((hexVal(hi) << 4) | hexVal(lo));
  }
  while (oi < 6) out[oi++] = 0;
}

String macToHexString(const uint8_t mac[6]) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// Helper: Convert MAC to colonless hex string (12 chars, e.g., "E89F6D32384C")
// Used for efficient path storage in mesh messages
String macToHexStringCompact(const uint8_t mac[6]) {
  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// Helper: Expand compact MAC (12 chars) to colon format (17 chars) in a static buffer
// Input: "E89F6D32384C" -> Output: "E8:9F:6D:32:38:4C"
// Returns pointer to static buffer (reused on each call, no heap allocation)
static const char* expandCompactMac(const char* compact) {
  static char expanded[18]; // 17 chars + null terminator
  if (!compact || strlen(compact) != 12) {
    expanded[0] = '\0';
    return expanded;
  }
  
  // Format: XX:XX:XX:XX:XX:XX
  expanded[0] = compact[0];
  expanded[1] = compact[1];
  expanded[2] = ':';
  expanded[3] = compact[2];
  expanded[4] = compact[3];
  expanded[5] = ':';
  expanded[6] = compact[4];
  expanded[7] = compact[5];
  expanded[8] = ':';
  expanded[9] = compact[6];
  expanded[10] = compact[7];
  expanded[11] = ':';
  expanded[12] = compact[8];
  expanded[13] = compact[9];
  expanded[14] = ':';
  expanded[15] = compact[10];
  expanded[16] = compact[11];
  expanded[17] = '\0';
  
  return expanded;
}

[[maybe_unused]] static String macToHexNoSep(const uint8_t mac[6]) {
  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// --------------------------
// Mesh Message Builders (moved from .ino)
// --------------------------

// Helper: Get count of active mesh peers (excluding self)
static int getMeshPeerCount() {
  int count = 0;
  for (int i = 0; i < MESH_PEER_MAX; i++) {
    if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac)) {
      count++;
    }
  }
  return count;
}

// Helper: Calculate adaptive TTL based on peer count
// Formula: ceil(log2(peerCount)) + 1
// Examples: 1-2 peers=2, 3-4 peers=3, 5-8 peers=4, 9-16 peers=5
static uint8_t calculateAdaptiveTTL() {
  int peerCount = getMeshPeerCount();
  if (peerCount <= 0) return 1;  // Minimum TTL
  if (peerCount == 1) return 2;  // Direct peer
  
  // Calculate ceil(log2(peerCount)) + 1
  int ttl = 1;
  int n = peerCount - 1;
  while (n > 0) {
    ttl++;
    n >>= 1;  // Divide by 2
  }
  ttl++;  // Add 1 for safety margin
  
  // Cap at 10
  if (ttl > 10) ttl = 10;
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[ADAPTIVE_TTL] Calculated TTL=%d for %d peers", ttl, peerCount);
  return (uint8_t)ttl;
}

// Helper: Build JSON mesh envelope
// Returns serialized JSON string (caller must check length < 250)
[[maybe_unused]] static String buildMeshEnvelope(const char* type, uint32_t msgId, const char* src, const char* dst, int ttl, JsonObject payload) {
  JsonDocument doc;
  v2_init_envelope(doc, type, msgId, src, dst, ttl);

  if (!payload.isNull()) {
    doc["pld"] = payload;
  }
  
  String output;
  serializeJson(doc, output);
  return output;
}

// Helper: Build heartbeat message (exported for loop())
// Example: {"v":2,"type":"HB","id":123,"msgId":123,"src":"..."}
String buildHeartbeat(uint32_t msgId, const char* src) {
  JsonDocument doc;
  // Heartbeats are logical messages; routing layer decides direct vs mesh/ttl
  v2_init_envelope(doc, MSG_TYPE_HB, msgId, src, "", -1);
  
  String output;
  serializeJson(doc, output);
  return output;
}

String buildMeshSysMasterHeartbeat(uint32_t msgId, const char* src) {
  JsonDocument doc;
  v2_init_envelope(doc, MSG_TYPE_MESH_SYS, msgId, src, "", -1);
  JsonObject payload = doc["pld"].to<JsonObject>();
  payload["kind"] = "masterHb";
  String output;
  serializeJson(doc, output);
  return output;
}

// Helper: Build mesh system worker status telemetry message.
// Schema (compact, see also handler in handleJsonMessage):
//   v: 2
//   type: "MESH_SYS"
//   id/msgId: logical message id
//   src: worker MAC (hex string)
//   pld.kind: "workerStatus"
//   pld.name: short human-friendly worker name (max 16 chars)
//   pld.free: free heap bytes
//   pld.total: total heap bytes
//   pld.rssi: WiFi RSSI in dBm (optional based on config)
//   pld.thermal: whether thermal sensor is enabled (optional based on config)
//   pld.imu: whether IMU is enabled (optional based on config)
String buildMeshSysWorkerStatus(uint32_t msgId,
                                const char* src,
                                const char* name,
                                uint32_t freeHeap,
                                uint32_t totalHeap,
                                int rssi,
                                bool thermalEnabled,
                                bool imuEnabled) {
  JsonDocument doc;
  v2_init_envelope(doc, MSG_TYPE_MESH_SYS, msgId, src, "", -1);
  JsonObject payload = doc["pld"].to<JsonObject>();
  payload["kind"] = "workerStatus";
  payload["name"] = name;
  
  // Add fields based on configuration
  if (gWorkerStatusConfig.includeHeap) {
    payload["free"] = freeHeap;
    payload["total"] = totalHeap;
  }
  if (gWorkerStatusConfig.includeRssi) {
    payload["rssi"] = rssi;
  }
  if (gWorkerStatusConfig.includeThermal) {
    payload["thermal"] = thermalEnabled;
  }
  if (gWorkerStatusConfig.includeImu) {
    payload["imu"] = imuEnabled;
  }
  
  String output;
  serializeJson(doc, output);
  return output;
}

// Helper: Build ACK message
// Example: {"v":2,"type":"ACK","id":123,"msgId":123,
//           "ackFor":122,"src":"...","dst":"..."}
[[maybe_unused]] static String buildAck(uint32_t msgId, uint32_t ackFor, const char* src, const char* dst) {
  JsonDocument doc;
  v2_init_envelope(doc, MSG_TYPE_ACK, msgId, src, dst, -1);
  doc["ackFor"] = ackFor;
  
  String output;
  serializeJson(doc, output);
  return output;
}

// Helper: Build FILE start message
// Schema: {"v":2,"type":"FILE","src":"<mac>","pld":{"kind":"start","name":"<filename>","size":<bytes>,"chunks":<n>,"hash":"<sessionId>"}}
static String buildFileStartMessage(const char* src, const char* filename, uint32_t fileSize, uint16_t totalChunks, const String& hash) {
  JsonDocument doc;
  v2_init_envelope(doc, MSG_TYPE_FILE_STR, 0, src, "", -1);
  JsonObject payload = doc["pld"].to<JsonObject>();
  payload["kind"] = "start";
  payload["name"] = filename;
  payload["size"] = fileSize;
  payload["chunks"] = totalChunks;
  payload["hash"] = hash;
  String output;
  serializeJson(doc, output);
  return output;
}

// Helper: Build FILE chunk message
// Schema: {"v":2,"type":"FILE","src":"<mac>","pld":{"kind":"chunk","idx":<1-based>,"hash":"<sessionId>","data":"<base64>"}}
static String buildFileChunkMessage(const char* src, uint16_t chunkIndex, const String& hash, const String& base64Data) {
  JsonDocument doc;
  v2_init_envelope(doc, MSG_TYPE_FILE_STR, 0, src, "", -1);
  JsonObject payload = doc["pld"].to<JsonObject>();
  payload["kind"] = "chunk";
  payload["idx"] = chunkIndex;
  payload["hash"] = hash;
  payload["data"] = base64Data;
  String output;
  serializeJson(doc, output);
  return output;
}

// Helper: Build FILE end message
// Schema: {"v":2,"type":"FILE","src":"<mac>","pld":{"kind":"end","hash":"<sessionId>"}}
static String buildFileEndMessage(const char* src, const String& hash) {
  JsonDocument doc;
  v2_init_envelope(doc, MSG_TYPE_FILE_STR, 0, src, "", -1);
  JsonObject payload = doc["pld"].to<JsonObject>();
  payload["kind"] = "end";
  payload["hash"] = hash;
  String output;
  serializeJson(doc, output);
  return output;
}

// Helper: Build FILE ack message
// Schema: {"v":2,"type":"FILE","src":"<receiver>","dst":"<sender>","pld":{"kind":"ack","idx":<1-based>,"hash":"<sessionId>"}}
static String buildFileAckMessage(const char* src, const char* dst, uint16_t chunkIndex, const String& hash) {
  JsonDocument doc;
  v2_init_envelope(doc, MSG_TYPE_FILE_STR, 0, src, dst, -1);
  JsonObject payload = doc["pld"].to<JsonObject>();
  payload["kind"] = "ack";
  payload["idx"] = chunkIndex;
  payload["hash"] = hash;
  String output;
  serializeJson(doc, output);
  return output;
}

// Helper: Build CMD (remote command) message
// Schema: {"v":2,"type":"CMD","src":"<caller>","dst":"<target>","pld":{"user":"<username>","pass":"<password>","cmd":"<command>"}}
static String buildCommandMessage(const char* src, const char* dst, const char* username, const char* password, const char* command) {
  JsonDocument doc;
  v2_init_envelope(doc, MSG_TYPE_CMD, 0, src, dst, -1);
  JsonObject payload = doc["pld"].to<JsonObject>();
  payload["user"] = username;
  payload["pass"] = password;
  payload["cmd"] = command;
  String output;
  serializeJson(doc, output);
  return output;
}

// Helper: Build RESPONSE (remote command result) message
// Schema: {"v":2,"type":"RESPONSE","src":"<device>","dst":"<caller>","pld":{"kind":"remoteCmdResult","ok":<bool>,"msg":"<result text>"}}
static String buildResponseMessage(const char* src, const char* dst, bool ok, const String& resultMsg) {
  JsonDocument doc;
  v2_init_envelope(doc, MSG_TYPE_RESPONSE, 0, src, dst, -1);
  JsonObject payload = doc["pld"].to<JsonObject>();
  payload["kind"] = "remoteCmdResult";
  payload["ok"] = ok;
  payload["msg"] = resultMsg;
  String output;
  serializeJson(doc, output);
  return output;
}

// Helper: Build TEXT (plain text) message
// Schema: {"v":2,"type":"TEXT","src":"<sender>","dst":"<target|broadcast>","pld":{"msg":"<text content>"}}
static String buildTextMessage(const char* src, const char* dst, const String& text) {
  JsonDocument doc;
  v2_init_envelope(doc, MSG_TYPE_TEXT, 0, src, dst, -1);
  JsonObject payload = doc["pld"].to<JsonObject>();
  payload["msg"] = text;
  String output;
  serializeJson(doc, output);
  return output;
}

// Helper: Build topology request
// Example: {"type":"MESH_SYS","msgId":123,"src":"...","dst":"broadcast","pld":{"kind":"topoReq","reqId":123}}
// Note: TTL will be added by routerSend() when mesh routing is used
static String buildTopoRequest(uint32_t msgId, const char* src, uint32_t reqId) {
  JsonDocument doc;
  v2_init_envelope(doc, MSG_TYPE_MESH_SYS, msgId, src, "broadcast", -1);
  
  JsonObject payload = doc["pld"].to<JsonObject>();
  JsonObject topoReq = payload["topoReq"].to<JsonObject>();
  topoReq["req"] = reqId;
  
  // Initialize path with the originator (master) - serves as both path and dedup
  JsonArray path = topoReq["pth"].to<JsonArray>();
  path.add(src);
  
  String output;
  serializeJson(doc, output);
  return output;
}

// Helper: Get list of direct peers with RSSI and health data (legacy string format)
[[maybe_unused]] static String getDirectPeersList() {
  String result = "";
  int count = 0;
  
  for (int i = 0; i < MESH_PEER_MAX; i++) {
    if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac)) {
      if (count > 0) result += ",";
      result += macToHexString(gMeshPeers[i].mac);
      result += ":";
      result += String((int)gMeshPeers[i].lastHeartbeatMs);
      result += ":";
      result += String((int)gMeshPeers[i].heartbeatCount);
      count++;
    }
  }
  
  return result;
}

// Helper: Get list of direct peers as JSON array
// Uses short field names and MAC suffix to minimize message size (ESP-NOW limit: 250 bytes)
[[maybe_unused]] static void getDirectPeersJson(JsonArray& peers) {
  for (int i = 0; i < MESH_PEER_MAX; i++) {
    if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac)) {
      JsonObject peer = peers.add<JsonObject>();
      // Use last 6 hex digits of MAC to save space (e.g., "32:38:4c" instead of "e8:9f:6d:32:38:4c")
      String fullMac = macToHexString(gMeshPeers[i].mac);
      peer["m"] = fullMac.substring(9);  // Last 8 chars: "32:38:4c"
      peer["h"] = gMeshPeers[i].lastHeartbeatMs;
      peer["c"] = gMeshPeers[i].heartbeatCount;
    }
  }
}

// Helper: Clear topology device cache (called at start of discovery)
static void clearTopoDeviceCache() {
  for (int i = 0; i < MAX_TOPO_DEVICE_CACHE; i++) {
    gTopoDeviceCache[i].active = false;
  }
}

// Helper: Send topology request to all peers (moved from .ino)
void requestTopologyDiscovery() {
  if (!meshEnabled()) {
    BROADCAST_PRINTF("[TOPO] Mesh not enabled");
    return;
  }
  
  // Rate limiting: only allow once per 60s
  uint32_t now = millis();
  if (now - gLastTopoRequest < 60000 && gLastTopoRequest != 0) {
    BROADCAST_PRINTF("[TOPO] Rate limited, try again in %lu seconds", (60000 - (now - gLastTopoRequest)) / 1000);
    return;
  }
  
  gLastTopoRequest = now;
  gTopoRequestId = now;  // Use timestamp as unique ID
  gTopoResponsesReceived = 0;
  gTopoRequestTimeout = now + 10000;  // 10 second absolute timeout
  gTopoLastResponseTime = 0;  // Reset last response time
  
  // Clear old topology data
  gMeshTopology.clear();
  gTopoResultsBuffer = "";  // Clear results buffer
  clearTopoDeviceCache();  // Clear device name cache for fresh discovery
  
  // Count active peers (excluding self)
  int peerCount = 0;
  for (int i = 0; i < MESH_PEER_MAX; i++) {
    if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac)) {
      peerCount++;
    }
  }
  
  // Handle zero peers case immediately
  if (peerCount == 0) {
    BROADCAST_PRINTF("[TOPO] No mesh peers available - topology discovery complete");
    gTopoResultsBuffer = "No mesh peers found. Pair devices using 'espnow pair' or 'espnow pairsecure' first.";
    return;
  }
  
  // Build JSON topology request
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String myMacCompact = macToHexStringCompact(myMac);
  
  String request = buildTopoRequest(gMeshMsgCounter++, myMacCompact.c_str(), gTopoRequestId);
  
  DEBUGF_BROADCAST(DEBUG_ESPNOW_STREAM, "[TOPO] JSON topology request reqId=%lu (%d bytes)", (unsigned long)gTopoRequestId, request.length());
  meshSendEnvelopeToPeers(request);
}

// Helper: Send topology response to master (chunked, one peer per message) (moved from .ino)
void sendTopologyResponse(uint32_t reqId, const uint8_t masterMac[6], JsonArray requestPath) {
  DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_SEND_DEBUG] === sendTopologyResponse START (CHUNKED) ===");
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String myMacStr = macToHexString(myMac);
  String masterMacStr = macToHexString(masterMac);
  
  // Count active peers
  int peerCount = 0;
  for (int i = 0; i < MESH_PEER_MAX; i++) {
    if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac)) {
      peerCount++;
    }
  }
  
  DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_SEND_DEBUG] Sending %d peer(s) as separate messages", peerCount);
  
  // Send START message with total peer count
  JsonDocument startDoc;
  startDoc["type"] = MSG_TYPE_MESH_SYS;
  startDoc["msgId"] = gMeshMsgCounter++;
  startDoc["src"] = myMacStr;
  startDoc["dst"] = masterMacStr;
  startDoc["ttl"] = 3;
  
  JsonObject pld = startDoc["pld"].to<JsonObject>();
  JsonObject tStart = pld["tStart"].to<JsonObject>();
  tStart["req"] = reqId;
  tStart["tot"] = peerCount;
  tStart["last"] = (peerCount == 0);  // NEW: isLast true if no peers (edge case)
  
  // Include sender's device name so master can display it
  String myName = getEspNowDeviceName(myMac);
  if (myName.length() > 0) {
    tStart["n"] = myName;  // Device name
  }
  
  // Copy path from request and append this device
  JsonArray pth = tStart["pth"].to<JsonArray>();
  if (requestPath) {
    for (JsonVariant v : requestPath) {
      pth.add(v.as<const char*>());
    }
  }
  pth.add(myMacStr);  // Append self to path
  DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PATH_DEBUG] START path has %d hops", pth.size());
  
  String startMsg;
  serializeJson(startDoc, startMsg);
  DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_SEND_DEBUG] START message (len=%d): %s", startMsg.length(), startMsg.c_str());
  meshSendEnvelopeToPeers(startMsg);
  delay(10);  // Small delay between messages
  
  // Send one message per peer
  int peerIndex = 0;
  for (int i = 0; i < MESH_PEER_MAX; i++) {
    if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac)) {
      JsonDocument peerDoc;
      peerDoc["type"] = MSG_TYPE_MESH_SYS;
      peerDoc["msgId"] = gMeshMsgCounter++;
      peerDoc["src"] = myMacStr;
      peerDoc["dst"] = masterMacStr;
      peerDoc["ttl"] = 3;
      
      JsonObject peerPayload = peerDoc["pld"].to<JsonObject>();
      JsonObject topoPeer = peerPayload["tPeer"].to<JsonObject>();
      topoPeer["req"] = reqId;
      topoPeer["idx"] = peerIndex;
      
      // Peer data (full MAC needed - devices may have different prefixes)
      String fullMac = macToHexString(gMeshPeers[i].mac);
      topoPeer["m"] = fullMac;  // Full MAC address
      topoPeer["h"] = gMeshPeers[i].lastHeartbeatMs;
      topoPeer["c"] = gMeshPeers[i].heartbeatCount;
      
      // Include peer name so requesting device doesn't need to look it up
      String peerName = getEspNowDeviceName(gMeshPeers[i].mac);
      if (peerName.length() > 0) {
        topoPeer["n"] = peerName;  // Optional name field
      }
      
      // NEW: Check if this is the last peer
      bool isLastPeer = (peerIndex == peerCount - 1);
      topoPeer["isLast"] = isLastPeer;
      
      // NOTE: Path is NOT included in PEER messages to save space (already in START)
      // Path is stored in the TopologyStream from the START message
      
      String peerMsg;
      serializeJson(peerDoc, peerMsg);
      DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_SEND_DEBUG] PEER %d/%d (len=%d) isLast=%s", 
                    peerIndex+1, peerCount, peerMsg.length(), isLastPeer ? "true" : "false");
      meshSendEnvelopeToPeers(peerMsg);
      
      peerIndex++;
      delay(10);  // Small delay between messages
    }
  }
  
  DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_SEND_DEBUG] === sendTopologyResponse END: sent %d peers ===", peerIndex);
  DEBUGF_BROADCAST(DEBUG_ESPNOW_STREAM, "[TOPO] Sent topology response: %d peer(s) in %d messages", peerCount, peerCount + 1);
}

// --------------------------
// Mesh Deduplication & Topology Functions (moved from .ino)
// --------------------------

// Helper: Check if message has been seen (deduplication)
[[maybe_unused]] static bool meshSeenCheckAndInsert(const uint8_t src[6], uint32_t msgId) {
  if (!meshEnabled()) return false;  // no dedup needed when disabled
  
  // Fast path: check for duplicate (linear search, but typically small dataset)
  for (int i = 0; i < MESH_DEDUP_SIZE; i++) {
    if (gMeshSeen[i].msgId == msgId && macEqual6(gMeshSeen[i].src, src)) {
      DEBUGF(DEBUG_ESPNOW_STREAM, "[MESH] DEDUP: seen msgid=%lu", (unsigned long)msgId);
      return true;  // seen
    }
  }
  
  // Insert new entry (ring buffer)
  memcpy(gMeshSeen[gMeshSeenIndex].src, src, 6);
  gMeshSeen[gMeshSeenIndex].msgId = msgId;
  int insertedSlot = gMeshSeenIndex;
  gMeshSeenIndex = (gMeshSeenIndex + 1) % MESH_DEDUP_SIZE;
  
  DEBUGF(DEBUG_ESPNOW_STREAM, "[MESH] DEDUP: insert msgid=%lu slot=%d", (unsigned long)msgId, insertedSlot);
  return false;
}

// Helper: Finalize topology stream and add to results
static void finalizeTopologyStream(TopologyStream* stream);  // Forward decl

// Helper: Build nested chain view from topology streams
static String buildNestedChainView();  // Forward decl

// Helper: Check collection window and finalize streams when timeout expires
void checkTopologyCollectionWindow();  // Exported for loop() to call

// Forward declarations for functions defined later in this file (always available when ENABLE_ESPNOW)
static bool sendV2Fragmented(const uint8_t* mac, const String& payload, uint32_t msgId, bool isEncrypted, const String& deviceName, bool isMesh);
static bool sendV2Unfragmented(const uint8_t* mac, const String& payload, uint32_t msgId, bool isEncrypted, const String& deviceName, bool isMesh);
static bool parseMacAddress(const String& str, uint8_t mac[6]);
static bool parseJsonMessage(const String& message, JsonDocument& doc);
static void updateUnpairedDevice(const uint8_t* mac, const String& name, int rssi);
static TopologyStream* findOrCreateTopoStream(const uint8_t* senderMac, uint32_t reqId);
static TopologyStream* findTopoStream(const uint8_t* senderMac, uint32_t reqId);
static bool forwardTopologyPeer(const String& message, TopologyStream* stream);
static bool bufferPeerMessage(const String& message, uint32_t reqId, const uint8_t* masterMac);
static String getTopoDeviceName(const uint8_t* mac);
static void addTopoDeviceName(const uint8_t* mac, const char* name);
static ChunkBuffer* findOrAllocateChunkBuffer(uint32_t msgId, const uint8_t* senderMac);
static bool dispatchMessage(const ReceivedMessage& ctx);
static bool handleJsonMessage(const ReceivedMessage& ctx);
// Milestone 2: single dispatch entry wrapper (no behavior change)
static bool handleIncomingV2(const esp_now_recv_info* recv_info,
                             const uint8_t* incomingData,
                             int len,
                             const String& message,
                             bool isPaired,
                             bool isEncrypted,
                             const String& deviceName,
                             const String& macStr);

static bool handleIncomingV2(const esp_now_recv_info* recv_info,
                             const uint8_t* incomingData,
                             int len,
                             const String& message,
                             bool isPaired,
                             bool isEncrypted,
                             const String& deviceName,
                             const String& macStr) {
  broadcastOutput("[RX] Message from " + deviceName + " len=" + String(len) + " encrypted=" + String(isEncrypted ? "YES" : "NO"));
  ReceivedMessage ctx;
  ctx.recvInfo = recv_info;
  ctx.rawData = incomingData;
  ctx.dataLen = len;
  ctx.message = message;
  ctx.isPaired = isPaired;
  ctx.isEncrypted = isEncrypted;
  ctx.deviceName = deviceName;
  ctx.macStr = macStr;
  if (dispatchMessage(ctx)) {
    broadcastOutput("[RX] Message handled successfully");
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_DEBUG] Message handled by dispatch system");
    return true;
  }
  broadcastOutput("[RX] Message NOT handled by v2 dispatch");
  return false;
}
// File transfer message handler - Receives files via ESP-NOW
// Handles FILE_START, FILE_CHUNK, FILE_END, FILE_ACK messages
static void handleFileTransferMessage(const String& message, const uint8_t* senderMac) {
  // Handle FILE_START: filename:totalChunks:totalSize:hash
  if (message.startsWith("FILE_START:")) {
    String payload = message.substring(11);
    int colon1 = payload.indexOf(':');
    int colon2 = payload.indexOf(':', colon1 + 1);
    int colon3 = payload.indexOf(':', colon2 + 1);
    
    if (colon1 < 0 || colon2 < 0 || colon3 < 0) {
      ERROR_ESPNOWF("Invalid FILE_START format");
      return;
    }
    
    String filename = payload.substring(0, colon1);
    uint32_t totalChunks = payload.substring(colon1 + 1, colon2).toInt();
    uint32_t totalSize = payload.substring(colon2 + 1, colon3).toInt();
    String hash = payload.substring(colon3 + 1);
    
    // Cleanup any existing transfer
    if (gActiveFileTransfer) {
      if (gActiveFileTransferFile) {
        gActiveFileTransferFile.close();
      }
      delete gActiveFileTransfer;
      gActiveFileTransfer = nullptr;
    }
    
    // Allocate new transfer
    gActiveFileTransfer = new FileTransfer();
    if (!gActiveFileTransfer) {
      ERROR_ESPNOWF("Failed to allocate FileTransfer");
      return;
    }
    
    memset(gActiveFileTransfer, 0, sizeof(FileTransfer));
    strncpy(gActiveFileTransfer->filename, filename.c_str(), 63);
    gActiveFileTransfer->filename[63] = '\0';
    gActiveFileTransfer->totalSize = totalSize;
    gActiveFileTransfer->receivedBytes = 0;
    gActiveFileTransfer->receivedChunks = 0;
    gActiveFileTransfer->totalChunks = totalChunks;
    strncpy(gActiveFileTransfer->hash, hash.c_str(), 15);
    gActiveFileTransfer->hash[15] = '\0';
    gActiveFileTransfer->active = true;
    memcpy(gActiveFileTransfer->senderMac, senderMac, 6);
    
    // Don't create folder/file yet - wait for first chunk to ensure transfer actually starts
    // File will be opened on first FILE_CHUNK
    
    DEBUG_ESPNOWF("[FILE] Starting transfer: %s (%lu bytes, %lu chunks, hash=%s)",
                  filename.c_str(), (unsigned long)totalSize, (unsigned long)totalChunks, hash.c_str());
    
    String senderMacStr = formatMacAddress(senderMac);
    broadcastOutput("[FILE] Receiving file from " + senderMacStr + ": " + filename + " (" + String(totalSize) + " bytes)");
    return;
  }
  
  // Handle FILE_CHUNK: chunkNum:base64data
  if (message.startsWith("FILE_CHUNK:")) {
    if (!gActiveFileTransfer || !gActiveFileTransfer->active) {
      ERROR_ESPNOWF("Received chunk without active transfer");
      return;
    }
    
    String payload = message.substring(11);
    int colonPos = payload.indexOf(':');
    if (colonPos < 0) {
      ERROR_ESPNOWF("Invalid FILE_CHUNK format");
      return;
    }
    
    uint32_t chunkNum = payload.substring(0, colonPos).toInt();
    String b64data = payload.substring(colonPos + 1);
    
    // Open file on first chunk (lazy creation)
    if (!gActiveFileTransferFile) {
      // Create device-specific folder for received files
      String senderMacStr = macToHexString(gActiveFileTransfer->senderMac);
      senderMacStr.replace(":", "");  // Remove colons for folder name
      String deviceDir = String("/espnow/received/") + senderMacStr;

      {
        FsLockGuard guard("espnow.recvfile.open");
        LittleFS.mkdir("/espnow");
        LittleFS.mkdir("/espnow/received");
        LittleFS.mkdir(deviceDir.c_str());

        // Open file for writing in device-specific folder
        String filepath = deviceDir + String("/") + gActiveFileTransfer->filename;
        gActiveFileTransferFile = LittleFS.open(filepath, "w");
      }
      if (!gActiveFileTransferFile) {
        String filepath = deviceDir + String("/") + gActiveFileTransfer->filename;
        ERROR_ESPNOWF("Cannot open file for writing: %s", filepath.c_str());
        delete gActiveFileTransfer;
        gActiveFileTransfer = nullptr;
        return;
      }
      {
        String filepath = deviceDir + String("/") + gActiveFileTransfer->filename;
        DEBUG_ESPNOWF("[FILE] Created file: %s", filepath.c_str());
      }
    }
    
    // Decode base64
    String decoded = base64Decode(b64data);
    
    // Write to file
    if (gActiveFileTransferFile) {
      FsLockGuard guard("espnow.recvfile.write");
      size_t written = gActiveFileTransferFile.write((const uint8_t*)decoded.c_str(), decoded.length());
      if (written != decoded.length()) {
        ERROR_ESPNOWF("Write failed (expected %d, wrote %d)", decoded.length(), written);
      } else {
        gActiveFileTransfer->receivedBytes += written;
        gActiveFileTransfer->receivedChunks++;
        
        // Send ACK every FILE_ACK_INTERVAL chunks
        if ((chunkNum % FILE_ACK_INTERVAL) == 0 || chunkNum == gActiveFileTransfer->totalChunks) {
          uint8_t myMac[6];
          esp_wifi_get_mac(WIFI_IF_STA, myMac);
          String srcMac = macToHexStringCompact(myMac);
          String dstMac = macToHexStringCompact(gActiveFileTransfer->senderMac);
          String ackMsg = buildFileAckMessage(srcMac.c_str(), dstMac.c_str(), chunkNum, String(gActiveFileTransfer->hash));
          Message msg; memcpy(msg.dstMac, gActiveFileTransfer->senderMac, 6); msg.payload = ackMsg; (void)routerSend(msg);
          DEBUG_ESPNOWF("[FILE] Sent ACK for chunk %lu", (unsigned long)chunkNum);
        }
      }
    }
    return;
  }
  
  // Handle FILE_END: hash
  if (message.startsWith("FILE_END:")) {
    if (!gActiveFileTransfer || !gActiveFileTransfer->active) {
      ERROR_ESPNOWF("Received FILE_END without active transfer");
      return;
    }
    
    String receivedHash = message.substring(9);
    
    // Close file
    if (gActiveFileTransferFile) {
      FsLockGuard guard("espnow.recvfile.close");
      gActiveFileTransferFile.close();
    }
    
    // Verify hash
    bool hashMatch = (receivedHash == String(gActiveFileTransfer->hash));
    
    DEBUG_ESPNOWF("[FILE] Transfer complete: %s (%lu bytes received, %lu chunks)",
                  gActiveFileTransfer->filename,
                  (unsigned long)gActiveFileTransfer->receivedBytes,
                  (unsigned long)gActiveFileTransfer->receivedChunks);
    
    if (hashMatch) {
      DEBUG_ESPNOWF("[FILE] Hash verification: PASS");
      String senderMacStr = formatMacAddress(gActiveFileTransfer->senderMac);
      broadcastOutput("[FILE] Transfer complete: " + String(gActiveFileTransfer->filename) + 
                     " (" + String(gActiveFileTransfer->receivedBytes) + " bytes)");
      
      // Log file transfer success to message buffer
      logFileTransferEvent(gActiveFileTransfer->senderMac, senderMacStr.c_str(), 
                          gActiveFileTransfer->filename, MSG_FILE_RECV_SUCCESS);
      
      // Increment counter
      if (gEspNow) {
        gEspNow->fileTransfersReceived++;
      }
    } else {
      DEBUG_ESPNOWF("[FILE] Hash verification: FAIL (expected %s, got %s)",
                   gActiveFileTransfer->hash, receivedHash.c_str());
      
      // Log file transfer failure to message buffer
      String senderMacStr = formatMacAddress(gActiveFileTransfer->senderMac);
      logFileTransferEvent(gActiveFileTransfer->senderMac, senderMacStr.c_str(), 
                          gActiveFileTransfer->filename, MSG_FILE_RECV_FAILED);
    }
    
    // Send final ACK
    uint8_t myMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, myMac);
    String srcMac = macToHexStringCompact(myMac);
    String dstMac = macToHexStringCompact(gActiveFileTransfer->senderMac);
    String ackMsg = buildFileAckMessage(srcMac.c_str(), dstMac.c_str(), gActiveFileTransfer->receivedChunks, receivedHash);
    { Message msg; memcpy(msg.dstMac, gActiveFileTransfer->senderMac, 6); msg.payload = ackMsg; (void)routerSend(msg); }
    
    // Cleanup
    delete gActiveFileTransfer;
    gActiveFileTransfer = nullptr;
    return;
  }
  
  // Handle FILE_ACK: chunkNum:hash (sender side)
  if (message.startsWith("FILE_ACK:")) {
    String payload = message.substring(9);
    int colonPos = payload.indexOf(':');
    if (colonPos < 0) {
      ERROR_ESPNOWF("Invalid FILE_ACK format");
      return;
    }
    
    uint16_t ackChunkNum = payload.substring(0, colonPos).toInt();
    String ackHash = payload.substring(colonPos + 1);
    
    // Verify hash matches expected
    if (gEspNow && String(gEspNow->fileAckHashExpected) == ackHash) {
      gEspNow->fileAckLast = ackChunkNum;
      DEBUGF(DEBUG_ESPNOW_STREAM, "[FILE] ACK received: chunk %u", ackChunkNum);
    } else {
      DEBUG_ESPNOWF("[FILE] WARNING: ACK hash mismatch (expected '%s', got '%s')",
                   gEspNow ? gEspNow->fileAckHashExpected : "N/A", ackHash.c_str());
    }
    return;
  }
  
  ERROR_ESPNOWF("Unknown file transfer message type");
}

// Note: handleFileTransferMessage(const ReceivedMessage&) is implemented later in this file
// Helper: Add message to retry queue
[[maybe_unused]] static bool meshRetryEnqueue(uint32_t msgId, const uint8_t dstMac[6], const String& envelope) {
  if (!gMeshRetryMutex) return false;
  if (xSemaphoreTake(gMeshRetryMutex, pdMS_TO_TICKS(10)) != pdTRUE) return false;
  
  // Find empty slot
  for (int i = 0; i < MESH_RETRY_QUEUE_SIZE; i++) {
    if (!gMeshRetryQueue[i].active) {
      gMeshRetryQueue[i].msgId = msgId;
      memcpy(gMeshRetryQueue[i].dstMac, dstMac, 6);
      gMeshRetryQueue[i].envelope = envelope;
      gMeshRetryQueue[i].sentMs = millis();
      gMeshRetryQueue[i].retryCount = 0;
      gMeshRetryQueue[i].active = true;
      xSemaphoreGive(gMeshRetryMutex);
      DEBUGF(DEBUG_ESPNOW_STREAM, "[MESH] Retry queue: enqueued msgid=%lu", (unsigned long)msgId);
      return true;
    }
  }
  
  xSemaphoreGive(gMeshRetryMutex);
  DEBUGF(DEBUG_ESPNOW_STREAM, "[MESH] Retry queue: FULL, cannot enqueue msgid=%lu", (unsigned long)msgId);
  return false;
}

// Helper: Remove message from retry queue (ACK received)
static void meshRetryDequeue(uint32_t msgId) {
  if (!gMeshRetryMutex) return;
  if (xSemaphoreTake(gMeshRetryMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
  
  for (int i = 0; i < MESH_RETRY_QUEUE_SIZE; i++) {
    if (gMeshRetryQueue[i].active && gMeshRetryQueue[i].msgId == msgId) {
      gMeshRetryQueue[i].active = false;
      DEBUGF(DEBUG_ESPNOW_STREAM, "[MESH] Retry queue: dequeued msgid=%lu (ACK received)", (unsigned long)msgId);
      break;
    }
  }
  
  xSemaphoreGive(gMeshRetryMutex);
}

// Helper: Process retry queue (called from loop)
static void meshRetryProcess() {
  if (!meshEnabled() || !gMeshRetryMutex) return;
  if (xSemaphoreTake(gMeshRetryMutex, pdMS_TO_TICKS(5)) != pdTRUE) return;
  
  uint32_t now = millis();
  for (int i = 0; i < MESH_RETRY_QUEUE_SIZE; i++) {
    if (!gMeshRetryQueue[i].active) continue;
    
    uint32_t elapsed = now - gMeshRetryQueue[i].sentMs;
    if (elapsed >= MESH_ACK_TIMEOUT_MS) {
      if (gMeshRetryQueue[i].retryCount < MESH_MAX_RETRIES) {
        // Retry
        gMeshRetryQueue[i].retryCount++;
        gMeshRetryQueue[i].sentMs = now;
        
        String macStr = formatMacAddress(gMeshRetryQueue[i].dstMac);
        DEBUGF(DEBUG_ESPNOW_STREAM, "[MESH] Retry queue: retrying msgid=%lu to %s (attempt %d/%d)",
               (unsigned long)gMeshRetryQueue[i].msgId, macStr.c_str(),
               gMeshRetryQueue[i].retryCount + 1, MESH_MAX_RETRIES + 1);
        
        // Resend envelope
        meshSendEnvelopeToPeers(gMeshRetryQueue[i].envelope);
      } else {
        // Max retries exceeded, give up
        String macStr = formatMacAddress(gMeshRetryQueue[i].dstMac);
        BROADCAST_PRINTF("[MESH] Message delivery failed to %s after %d attempts (msgid=%lu)",
                        macStr.c_str(), MESH_MAX_RETRIES + 1, (unsigned long)gMeshRetryQueue[i].msgId);
        gMeshRetryQueue[i].active = false;
      }
    }
  }
  
  xSemaphoreGive(gMeshRetryMutex);
}

// Helper: Send envelope to all ESP-NOW peers (broadcast)
// Now a simple wrapper that delegates to the unified transport layer
void meshSendEnvelopeToPeers(const String& envelope) {
  // Extract message ID from envelope for tracking
  uint32_t msgId = 0;
  JsonDocument doc;
  if (!deserializeJson(doc, envelope)) {
    msgId = (uint32_t)(doc["id"] | doc["msgId"] | 0);
  }
  if (msgId == 0) {
    msgId = generateMessageId();
  }
  
  DEBUGF_BROADCAST(DEBUG_ESPNOW_STREAM, "[MESH_V2] TX ENVELOPE: id=%lu, len=%d | %.80s", 
                   (unsigned long)msgId, envelope.length(), envelope.c_str());
  
  // Determine if we need fragmentation (single size check)
  bool needsFrag = shouldChunk(envelope.length());
  
  // Use unified transport layer - always compiled under ENABLE_ESPNOW
  bool success = false;
  if (needsFrag) {
    success = sendV2Fragmented(nullptr, envelope, msgId, false, String(), true);
  } else {
    success = sendV2Unfragmented(nullptr, envelope, msgId, false, String(), true);
  }
  
  if (!success) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[MESH_V2] ERROR: Failed to send envelope to peers");
  }
}

// --------------------------
// Mesh Heartbeat Processing (FreeRTOS Task)
// --------------------------

// Task handle for ESP-NOW heartbeat processor
static TaskHandle_t gEspNowTaskHandle = nullptr;

// Helper: Cleanup stale unpaired devices (called from heartbeat)
static void cleanupStaleUnpairedDevices() {
  if (!gEspNow) return;
  unsigned long now = millis();
  const unsigned long STALE_TIMEOUT_MS = 60000;  // 60 seconds
  
  for (int i = gEspNow->unpairedDeviceCount - 1; i >= 0; i--) {
    if (now - gEspNow->unpairedDevices[i].lastSeenMs > STALE_TIMEOUT_MS) {
      // Shift remaining devices down
      for (int j = i; j < gEspNow->unpairedDeviceCount - 1; j++) {
        gEspNow->unpairedDevices[j] = gEspNow->unpairedDevices[j + 1];
      }
      gEspNow->unpairedDeviceCount--;
    }
  }
}

// --------------------------
// Mesh Heartbeat Processing (moved from loop())
// --------------------------

/**
 * @brief Process all mesh heartbeat and role-based messaging
 * @note Called from loop() when mesh is enabled and not suspended
 * @note Handles: regular heartbeats, master/backup failover, worker status, auto-discovery, retry queue
 */
void processMeshHeartbeats() {
  if (!meshEnabled() || gMeshActivitySuspended) return;
  
  uint32_t now = millis();
  espnowSensorStatusPeriodicTick();
  
  // Regular heartbeat broadcast to all peers
  if (now - gLastHeartbeatSentMs >= MESH_HEARTBEAT_INTERVAL_MS) {
    gLastHeartbeatSentMs = now;

    // Build JSON heartbeat message (use compact MAC format)
    uint8_t myMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, myMac);
    String myMacCompact = macToHexStringCompact(myMac);
    String heartbeat = buildHeartbeat(gMeshMsgCounter++, myMacCompact.c_str());

    // Send heartbeat based on broadcast mode setting
    if (gSettings.meshHeartbeatBroadcast) {
      // Public mode: Broadcast to FF:FF:FF:FF:FF:FF for device discovery (keep plain for unpaired discovery)
      uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
      esp_now_send(broadcastMac, (uint8_t*)heartbeat.c_str(), heartbeat.length());
      DEBUGF(DEBUG_ESPNOW_STREAM, "[MESH] Broadcast heartbeat sent (public mode, %d bytes)", heartbeat.length());
    } else {
      // Private mode: Only send if we have paired peers
      esp_now_peer_info_t peer;
      esp_err_t ret = esp_now_fetch_peer(true, &peer);
      if (ret == ESP_OK) {
        // We have at least one peer, send heartbeat
        meshSendEnvelopeToPeers(heartbeat);
        DEBUGF(DEBUG_ESPNOW_STREAM, "[MESH] Heartbeat sent to paired peers via v2 transport (private mode, %d bytes)", heartbeat.length());
      } else {
        DEBUGF(DEBUG_ESPNOW_STREAM, "[MESH] Skipping heartbeat - no paired peers (private mode)");
      }
    }

    // Update self-entry in peer table (for accurate topology display)
    // This creates a special entry that represents "me" so topology results
    // can show accurate "last seen" times for this device's peers
    MeshPeerHealth* selfPeer = getMeshPeerHealth(myMac, true);
    if (selfPeer) {
      selfPeer->lastHeartbeatMs = now;
      selfPeer->heartbeatCount++;
    }
    
    // Cleanup stale unpaired devices (piggybacking on heartbeat interval)
    cleanupStaleUnpairedDevices();
  }
  
  // Master/Backup heartbeat and failover logic
  if (gSettings.meshRole == MESH_ROLE_MASTER) {
    // Master: Send periodic heartbeat to backup
    if (gSettings.meshBackupMAC.length() == 17 && 
        now - gLastMasterHeartbeat >= gSettings.meshMasterHeartbeatInterval) {
      gLastMasterHeartbeat = now;
      
      uint8_t myMac[6];
      esp_wifi_get_mac(WIFI_IF_STA, myMac);
      String myMacCompact = macToHexStringCompact(myMac);

      String envelope = buildMeshSysMasterHeartbeat(gMeshMsgCounter++, myMacCompact.c_str());
      
      // Parse backup MAC and send via routerSend
      uint8_t backupMac[6];
      macFromHexString(gSettings.meshBackupMAC, backupMac);
      
      Message msg;
      memcpy(msg.dstMac, backupMac, 6);
      msg.payload = envelope;
      msg.priority = PRIORITY_HIGH;
      msg.type = MSG_TYPE_HEARTBEAT;
      msg.requiresAck = false;
      msg.msgId = generateMessageId();
      msg.ttl = gSettings.meshTTL;
      msg.timestamp = millis();
      msg.maxRetries = 0;
      
      bool sent = routerSend(msg);
      if (sent) {
        if (gEspNow) {
          gEspNow->heartbeatsSent++;
        }
        DEBUGF(DEBUG_ESPNOW_STREAM, "[MASTER] JSON heartbeat sent to backup %s", gSettings.meshBackupMAC.c_str());
      } else {
        DEBUGF(DEBUG_ESPNOW_STREAM, "[MASTER] ERROR sending JSON heartbeat to backup %s", gSettings.meshBackupMAC.c_str());
      }
    }
  } else if (gSettings.meshRole == MESH_ROLE_BACKUP_MASTER) {
    // Backup: Monitor master heartbeat and promote if timeout
    if (gSettings.meshMasterMAC.length() > 0 && gLastMasterHeartbeat > 0) {
      if (now - gLastMasterHeartbeat >= gSettings.meshFailoverTimeout && !gBackupPromoted) {
        gBackupPromoted = true;
        gSettings.meshRole = MESH_ROLE_MASTER;
        String oldMaster = gSettings.meshMasterMAC;
        gSettings.meshMasterMAC = "";
        writeSettingsJson();
        
        BROADCAST_PRINTF("[FAILOVER] Master %s timeout! Backup promoted to master.", oldMaster.c_str());
        DEBUGF(DEBUG_ESPNOW_STREAM, "[FAILOVER] Backup promoted after %lu ms timeout", 
               (unsigned long)(now - gLastMasterHeartbeat));
      }
    }
  } else if (gSettings.meshRole == MESH_ROLE_WORKER) {
    // Worker: Send periodic status reports to master (if enabled)
    if (gWorkerStatusConfig.enabled &&
        gSettings.meshMasterMAC.length() == 17 && 
        now - gLastWorkerStatusReport >= gWorkerStatusConfig.intervalMs) {
      gLastWorkerStatusReport = now;
      
      uint8_t myMac[6];
      esp_wifi_get_mac(WIFI_IF_STA, myMac);
      String myMacCompact = macToHexStringCompact(myMac);
      String myName = getEspNowDeviceName(myMac);
      if (myName.length() == 0) myName = macToHexString(myMac);  // Use colon format for display name fallback
      // Cap worker name length to keep workerStatus payload bounded
      if (myName.length() > 20) myName = myName.substring(0, 20);

      uint32_t freeHeap = (uint32_t)ESP.getFreeHeap();
      uint32_t totalHeap = (uint32_t)ESP.getHeapSize();
      int rssi = WiFi.RSSI();

      String status = buildMeshSysWorkerStatus(gMeshMsgCounter++,
                                               myMacCompact.c_str(),
                                               myName.c_str(),
                                               freeHeap,
                                               totalHeap,
                                               rssi,
                                               thermalEnabled,
                                               imuEnabled);

      // Parse master MAC and send via routerSend
      uint8_t masterMac[6];
      macFromHexString(gSettings.meshMasterMAC, masterMac);
      
      Message msg;
      memcpy(msg.dstMac, masterMac, 6);
      msg.payload = status;
      msg.priority = PRIORITY_NORMAL;
      msg.type = MSG_TYPE_DATA;
      msg.requiresAck = false;
      msg.msgId = generateMessageId();
      msg.ttl = gSettings.meshTTL;
      msg.timestamp = millis();
      msg.maxRetries = 0;
      
      bool sent = routerSend(msg);
      if (sent) {
        DEBUGF(DEBUG_ESPNOW_STREAM, "[WORKER] JSON status sent to master %s (heap=%lu rssi=%d)", 
               gSettings.meshMasterMAC.c_str(), (unsigned long)freeHeap, rssi);
      } else {
        DEBUGF(DEBUG_ESPNOW_STREAM, "[WORKER] ERROR sending JSON status to master %s", 
               gSettings.meshMasterMAC.c_str());
      }
    }
  }
  
  // Auto topology discovery (if enabled and master)
  if (gSettings.meshRole == MESH_ROLE_MASTER && 
      gSettings.meshTopoAutoRefresh && 
      gSettings.meshTopoDiscoveryInterval > 0) {
    if (now - gLastTopoRequest >= gSettings.meshTopoDiscoveryInterval) {
      requestTopologyDiscovery();
    }
  }
  
  // Process retry queue (check for timeouts and resend)
  meshRetryProcess();
}

/**
 * @brief FreeRTOS task for ESP-NOW mesh heartbeat processing
 * @note Runs in parallel with main loop, checks heartbeat intervals and processes mesh protocol
 */
static void espnowHeartbeatTask(void* parameter) {
  DEBUGF(DEBUG_ESPNOW_STREAM, "[ESPNOW_TASK] Heartbeat task started");
  unsigned long lastStackLog = 0;
  
  while (true) {
    // Stack/heap monitoring (every 30 seconds)
    unsigned long nowMs = millis();
    if ((nowMs - lastStackLog) >= 30000) {
      lastStackLog = nowMs;
      if (isDebugFlagSet(DEBUG_PERFORMANCE)) {
        UBaseType_t watermark = uxTaskGetStackHighWaterMark(nullptr);
        DEBUG_PERFORMANCEF("[STACK] espnow_hb watermark=%u words", (unsigned)watermark);
      }
      if (isDebugFlagSet(DEBUG_MEMORY)) {
        DEBUG_MEMORYF("[HEAP] espnow_hb: free=%u min=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
      }
    }
    
    // Run heartbeat processing (handles timing checks internally)
    processMeshHeartbeats();
    
    // Check topology collection window
    checkTopologyCollectionWindow();

    // Drain raw ESP-NOW RX ring (process heavy parsing/routing in safe task context)
    uint32_t __saved_out = gOutputFlags;
    gOutputFlags = __saved_out & ~OUTPUT_FILE;
    while (gEspNowRxTail != gEspNowRxHead) {
      InboundRxItem it = gEspNowRxRing[gEspNowRxTail];
      gEspNowRxTail = (uint8_t)((gEspNowRxTail + 1) % (uint8_t)(sizeof(gEspNowRxRing)/sizeof(gEspNowRxRing[0])));
      esp_now_recv_info info; memset(&info, 0, sizeof(info));
      info.src_addr = it.src;
      wifi_pkt_rx_ctrl_t rxctrl; memset(&rxctrl, 0, sizeof(rxctrl));
      rxctrl.rssi = it.rssi;
      info.rx_ctrl = &rxctrl;
      onEspNowRawRecv(&info, it.data, it.len);
    }
    gOutputFlags = __saved_out;
    
    // Note: Messages are now stored in per-device buffers and broadcast
    // immediately when received in handleJsonMessage(). No need to
    // process them here in the heartbeat task.
    
    // Sleep for 100ms to yield to other tasks (heartbeat interval is 5s, so this is fine)
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

/**
 * @brief Start ESP-NOW heartbeat task
 * @return true if task created successfully, false otherwise
 */
bool startEspNowTask() {
  if (gEspNowTaskHandle != nullptr) {
    DEBUGF(DEBUG_ESPNOW_STREAM, "[ESPNOW_TASK] Task already running");
    return true;
  }
  
  const uint32_t stackSize = 8192;  // 32KB stack for mesh processing (JSON, MAC operations, retry queue)
  BaseType_t result = xTaskCreate(
    espnowHeartbeatTask,
    "espnow_hb",
    stackSize,
    nullptr,
    1,  // Priority 1 (same as other background tasks)
    &gEspNowTaskHandle
  );
  
  if (result != pdPASS) {
    DEBUGF(DEBUG_ESPNOW_STREAM, "[ESPNOW_TASK] ERROR: Failed to create heartbeat task");
    return false;
  }
  
  DEBUGF(DEBUG_ESPNOW_STREAM, "[ESPNOW_TASK] Heartbeat task created successfully");
  return true;
}

/**
 * @brief Stop ESP-NOW heartbeat task
 */
void stopEspNowTask() {
  if (gEspNowTaskHandle != nullptr) {
    vTaskDelete(gEspNowTaskHandle);
    gEspNowTaskHandle = nullptr;
    DEBUGF(DEBUG_ESPNOW_STREAM, "[ESPNOW_TASK] Heartbeat task stopped");
  }
}

// --------------------------
// Topology Collection & Formatting (moved from .ino)
// --------------------------

// Helper: Finalize topology stream and add to results
static void finalizeTopologyStream(TopologyStream* stream) {
  if (!stream || !stream->active) return;
  
  // NEW FORMAT: Store raw data, format later in buildNestedChainView()
  // Just mark the stream as complete and increment counter
  gTopoResponsesReceived++;
  
  // Update last response time to extend collection window
  gTopoLastResponseTime = millis();
  
  DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_STREAM_DEBUG] Finalized stream from %s: %d peers (total responses: %d)", 
                stream->senderName, stream->receivedPeers, gTopoResponsesReceived);
  DEBUGF_BROADCAST(DEBUG_ESPNOW_STREAM, "[TOPO] Complete response from %s: %d peer(s)", 
                  stream->senderName, stream->receivedPeers);
  
  // DON'T mark stream as inactive yet - buildNestedChainView() needs the data
  // Streams will be cleaned up after buildNestedChainView() is called
}

// Helper: Build nested chain view from topology streams
static String buildNestedChainView() {
  // Pre-allocate result buffer to avoid repeated allocations
  String result;
  result.reserve(1024);  // Reserve space for typical topology
  
  // Get my MAC (master node)
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String myMacStr = macToHexString(myMac);
  String myName = getEspNowDeviceName(myMac);
  if (myName.length() == 0) myName = myMacStr;
  
  // Start with master node
  result += myName + " (" + myMacStr + ")\n";
  
  // Track visited devices to avoid loops
  std::vector<String> visited;
  visited.push_back(myMacStr);
  
  // Helper lambda to render a device's peers recursively
  std::function<void(const String&, int)> renderPeers;
  renderPeers = [&](const String& deviceMac, int indentLevel) {
    uint8_t mac[6];
    macFromHexString(deviceMac.c_str(), mac);
    
    // Limit recursion depth to prevent stack overflow and excessive concatenation
    if (indentLevel > 10) return;
    
    // Build indent string using char buffer to avoid String concatenation
    char indent[32];
    int indentChars = indentLevel * 2;
    if (indentChars > 30) indentChars = 30;
    for (int j = 0; j < indentChars; j++) indent[j] = ' ';
    indent[indentChars] = '\0';
    
    // Special case: if this is the master node, use gMeshPeers instead of topology streams
    if (macEqual6(mac, myMac)) {
      // Render master's direct peers from gMeshPeers
      for (int i = 0; i < MESH_PEER_MAX; i++) {
        if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac)) {
          String peerMacStr = macToHexString(gMeshPeers[i].mac);
          
          // Check if already visited
          bool alreadyVisited = false;
          for (const auto& v : visited) {
            if (v == peerMacStr) {
              alreadyVisited = true;
              break;
            }
          }
          
          if (!alreadyVisited) {
            visited.push_back(peerMacStr);
            
            // Get peer name
            String peerName = getEspNowDeviceName(gMeshPeers[i].mac);
            if (peerName.length() == 0) peerName = peerMacStr;
            
            // Render this peer using char buffer for indent
            result += indent;
            result += peerName + " (" + peerMacStr + ")\n";
            
            // Recursively render this peer's peers
            renderPeers(peerMacStr, indentLevel + 1);
          }
        }
      }
      return;
    }
    
    // For non-master devices, find their topology stream
    TopologyStream* stream = nullptr;
    for (int i = 0; i < MAX_CONCURRENT_TOPO_STREAMS; i++) {
      if (gTopoStreams[i].reqId == gTopoRequestId && macEqual6(gTopoStreams[i].senderMac, mac)) {
        stream = &gTopoStreams[i];
        break;
      }
    }
    
    if (!stream) return;
    
    // Parse peers from accumulated data
    String accumulated = stream->accumulatedData;
    int pos = 0;
    while (pos < accumulated.length()) {
      // Find peer line (starts with "  → ")
      int peerStart = accumulated.indexOf("  \xe2\x86\x92 ", pos);
      if (peerStart == -1) break;
      
      int peerEnd = accumulated.indexOf('\n', peerStart);
      if (peerEnd == -1) peerEnd = accumulated.length();
      
      String peerLine = accumulated.substring(peerStart + 5, peerEnd);
      
      // Extract MAC from peer line (format: "name (mac)")
      int macStart = peerLine.indexOf('(');
      int macEnd = peerLine.indexOf(')');
      if (macStart != -1 && macEnd != -1) {
        String peerMacStr = peerLine.substring(macStart + 1, macEnd);
        String peerName = peerLine.substring(0, macStart - 1);
        
        // Check if already visited
        bool alreadyVisited = false;
        for (const auto& v : visited) {
          if (v == peerMacStr) {
            alreadyVisited = true;
            break;
          }
        }
        
        if (!alreadyVisited) {
          visited.push_back(peerMacStr);
          
          // Render this peer using char buffer for indent
          result += indent;
          result += peerName + " (" + peerMacStr + ")\n";
          
          // Recursively render this peer's peers
          renderPeers(peerMacStr, indentLevel + 1);
        }
      }
      
      // Find heartbeat line (next line) and skip it
      int hbStart = peerEnd + 1;
      int hbEnd = accumulated.indexOf('\n', hbStart);
      if (hbEnd == -1) hbEnd = accumulated.length();
      
      pos = hbEnd + 1;
    }
  };
  
  // Render peers of master node
  renderPeers(myMacStr, 1);
  
  // Fallback: if no hierarchy was built, show all devices flat
  if (result.indexOf('\n', result.indexOf('\n') + 1) == -1) {
    result = "";
    for (int i = 0; i < MAX_CONCURRENT_TOPO_STREAMS; i++) {
      if (gTopoStreams[i].reqId == gTopoRequestId) {
        result += String(gTopoStreams[i].senderName) + " (" + 
                  macToHexString(gTopoStreams[i].senderMac) + ")\n";
      }
    }
  }
  
  return result;
}

// Helper: Check collection window and finalize streams when timeout expires
void checkTopologyCollectionWindow() {
  // Only check if we have an active topology request
  if (gTopoRequestId == 0) return;
  
  unsigned long now = millis();
  
  // Check if we're past the overall request timeout (10 seconds)
  if (now >= gTopoRequestTimeout) {
    DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_COLLECTION] Request timeout reached, finalizing all active streams");
    // Finalize all active streams for this request
    for (int i = 0; i < MAX_CONCURRENT_TOPO_STREAMS; i++) {
      if (gTopoStreams[i].active && gTopoStreams[i].reqId == gTopoRequestId) {
        finalizeTopologyStream(&gTopoStreams[i]);
      }
    }
    
    // Build nested chain view from all collected streams
    gTopoResultsBuffer = buildNestedChainView();
    
    // Now clean up all streams for this request
    for (int i = 0; i < MAX_CONCURRENT_TOPO_STREAMS; i++) {
      if (gTopoStreams[i].reqId == gTopoRequestId) {
        gTopoStreams[i].active = false;
      }
    }
    
    // Clear request ID to stop checking
    gTopoRequestId = 0;
    return;
  }
  
  // Check if collection window has expired (3 seconds since last PEER)
  if (gTopoLastResponseTime > 0 && (now - gTopoLastResponseTime) >= TOPO_COLLECTION_WINDOW_MS) {
    DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_COLLECTION] Collection window expired (%lums since last PEER), finalizing all active streams",
                 (unsigned long)(now - gTopoLastResponseTime));
    
    // Finalize all active streams for this request
    int finalizedCount = 0;
    for (int i = 0; i < MAX_CONCURRENT_TOPO_STREAMS; i++) {
      if (gTopoStreams[i].active && gTopoStreams[i].reqId == gTopoRequestId) {
        finalizeTopologyStream(&gTopoStreams[i]);
        finalizedCount++;
      }
    }
    
    DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_COLLECTION] Finalized %d stream(s), topology discovery complete", finalizedCount);
    
    // Build nested chain view from all collected streams
    gTopoResultsBuffer = buildNestedChainView();
    
    // Now clean up all streams for this request
    for (int i = 0; i < MAX_CONCURRENT_TOPO_STREAMS; i++) {
      if (gTopoStreams[i].reqId == gTopoRequestId) {
        gTopoStreams[i].active = false;
      }
    }
    
    BROADCAST_PRINTF("[OK] Topology discovery complete: %d device(s) responded", gTopoResponsesReceived);
    
    // Clear request ID to stop checking
    gTopoRequestId = 0;
  }
}

/**
 * @brief Remove device from ESP-NOW device registry
 * @param mac MAC address of device to remove (6 bytes)
 * @note Does not persist to filesystem - call saveEspNowDevices() to persist
 * @note Silently ignores if device not found
 */
void removeEspNowDevice(const uint8_t* mac) {
  if (!gEspNow) return;
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (memcmp(gEspNow->devices[i].mac, mac, 6) == 0) {
      // Shift remaining devices down
      for (int j = i; j < gEspNow->deviceCount - 1; j++) {
        gEspNow->devices[j] = gEspNow->devices[j + 1];
      }
      gEspNow->deviceCount--;
      return;
    }
  }
}

/**
 * @brief Look up device name by MAC address in paired device registry
 * @param mac MAC address to look up (6 bytes)
 * @return Device name if found, empty string otherwise
 * @note Searches gEspNowDevices array (O(n) lookup, max 16 devices)
 */
String getEspNowDeviceName(const uint8_t* mac) {
  if (!gEspNow) return "";
  // Look up name in device registry (including our own MAC)
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (memcmp(gEspNow->devices[i].mac, mac, 6) == 0) {
      return gEspNow->devices[i].name;
    }
  }
  return "";  // No name found
}

/**
 * @brief Get the first username from users.json (device owner)
 * @return Username of first user, or empty string if not found
 * @note Used for device self-registration in ESP-NOW topology
 * @note Thread-safe: uses FsLockGuard for filesystem access
 */
static String getFirstUsername() {
  extern bool filesystemReady;
  if (!filesystemReady) return "";
  FsLockGuard fsGuard("espnow.users.first");
  if (!LittleFS.exists(USERS_JSON_FILE)) return "";

  File f = LittleFS.open(USERS_JSON_FILE, "r");
  if (!f) return "";
  
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  
  if (err) return "";
  
  JsonArray users = doc["users"];
  if (users.size() > 0) {
    return users[0]["username"].as<String>();
  }
  return "";
}

// Save ESP-NOW devices to filesystem
static void saveEspNowDevices() {
  if (!gEspNow) return;
  extern bool filesystemReady;
  if (!filesystemReady) return;

  // Pause sensor polling during file I/O
  extern volatile bool gSensorPollingPaused;
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;

  FsLockGuard fsGuard("espnow.devices.save");
  File file = LittleFS.open(ESPNOW_DEVICES_FILE, "w");
  if (!file) {
    gSensorPollingPaused = wasPaused;
    return;
  }

  file.println("{");
  file.println("  \"devices\": [");

  for (int i = 0; i < gEspNow->deviceCount; i++) {
    file.print("    {");
    file.print("\"mac\": \"");
    file.print(formatMacAddress(gEspNow->devices[i].mac));
    file.print("\", \"name\": \"");
    file.print(gEspNow->devices[i].name);
    file.print("\", \"encrypted\": ");
    file.print(gEspNow->devices[i].encrypted ? "true" : "false");

    if (gEspNow->devices[i].encrypted) {
      file.print(", \"key\": \"");
      for (int j = 0; j < 16; j++) {
        if (gEspNow->devices[i].key[j] < 16) file.print("0");
        file.print(String(gEspNow->devices[i].key[j], HEX));
      }
      file.print("\"");
    }

    file.print("}");
    if (i < gEspNow->deviceCount - 1) file.print(",");
    file.println();
  }

  file.println("  ]");
  file.println("}");
  file.close();
  
  gSensorPollingPaused = wasPaused;
}

// Load ESP-NOW devices from filesystem
static void loadEspNowDevices() {
  if (!gEspNow) return;
  extern bool filesystemReady;
  if (!filesystemReady) return;

  // Pause sensor polling during file I/O
  extern volatile bool gSensorPollingPaused;
  bool wasPaused = gSensorPollingPaused;
  gSensorPollingPaused = true;

  FsLockGuard fsGuard("espnow.devices.load");
  File file = LittleFS.open(ESPNOW_DEVICES_FILE, "r");
  if (!file) {
    gSensorPollingPaused = wasPaused;
    return;
  }

  String content = file.readString();
  file.close();
  
  // Resume sensor polling before parsing (parsing doesn't need I/O pause)
  gSensorPollingPaused = wasPaused;

  // Simple JSON parsing for device list
  gEspNow->deviceCount = 0;
  int pos = 0;

  while ((pos = content.indexOf("\"mac\":", pos)) >= 0) {
    if (gEspNow->deviceCount >= 16) break;  // Max devices reached

    // Extract MAC address
    int macStart = content.indexOf("\"", pos + 6) + 1;
    int macEnd = content.indexOf("\"", macStart);
    if (macStart <= 0 || macEnd <= macStart) break;

    String macStr = content.substring(macStart, macEnd);

    // Extract device name
    int namePos = content.indexOf("\"name\":", macEnd);
    if (namePos < 0) break;

    int nameStart = content.indexOf("\"", namePos + 7) + 1;
    int nameEnd = content.indexOf("\"", nameStart);
    if (nameStart <= 0 || nameEnd <= nameStart) break;

    String name = content.substring(nameStart, nameEnd);

    // Check for encryption flag
    bool encrypted = false;
    int encPos = content.indexOf("\"encrypted\":", nameEnd);
    if (encPos >= 0 && encPos < content.indexOf("}", nameEnd)) {
      int encStart = content.indexOf(":", encPos) + 1;
      String encValue = content.substring(encStart, content.indexOf(",", encStart));
      encValue.trim();
      encrypted = (encValue == "true");
    }

    // Parse MAC address and store device
    uint8_t mac[6];
    if (parseMacAddress(macStr, mac)) {
      memcpy(gEspNow->devices[gEspNow->deviceCount].mac, mac, 6);
      gEspNow->devices[gEspNow->deviceCount].name = name;
      gEspNow->devices[gEspNow->deviceCount].encrypted = encrypted;

      // Load encryption key if present
      if (encrypted) {
        int keyPos = content.indexOf("\"key\":", nameEnd);
        if (keyPos >= 0 && keyPos < content.indexOf("}", nameEnd)) {
          int keyStart = content.indexOf("\"", keyPos + 6) + 1;
          int keyEnd = content.indexOf("\"", keyStart);
          if (keyStart > 0 && keyEnd > keyStart) {
            String keyHex = content.substring(keyStart, keyEnd);
            // Parse hex key (32 hex chars = 16 bytes)
            if (keyHex.length() == 32) {
              for (int j = 0; j < 16; j++) {
                String byteStr = keyHex.substring(j * 2, j * 2 + 2);
                gEspNow->devices[gEspNow->deviceCount].key[j] = strtol(byteStr.c_str(), NULL, 16);
              }
            }
          }
        }
      } else {
        // Clear key for unencrypted devices
        memset(gEspNow->devices[gEspNow->deviceCount].key, 0, 16);
      }

      gEspNow->deviceCount++;
    }

    pos = nameEnd;
  }
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
  for (int i = 0; i < MESH_PEER_MAX; i++) {
    if (!gMeshPeers[i].isActive || isSelfMac(gMeshPeers[i].mac)) continue;  // Don't save self-entry
    
    if (count > 0) file.println(",");
    
    file.print("    {\"mac\": \"");
    file.print(macToHexString(gMeshPeers[i].mac));
    file.print("\"}");
    
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

// Load mesh peer MAC addresses from filesystem (topology only)
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
  for (int i = 0; i < MESH_PEER_MAX; i++) {
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
      broadcastOutput("[ESP-NOW] Restored device: " + gEspNow->devices[i].name + " (" + formatMacAddress(gEspNow->devices[i].mac) + ")" + encStatus);
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

  char keyStr[33];
  snprintf(keyStr, sizeof(keyStr), "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
           key[0], key[1], key[2], key[3], key[4], key[5], key[6], key[7],
           key[8], key[9], key[10], key[11], key[12], key[13], key[14], key[15]);

  DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] DEBUG KEY DERIVATION:");
  DEBUGF(DEBUG_ESPNOW_STREAM, "  Device MAC: %s (not used in key derivation)", macStr.c_str());
  DEBUGF(DEBUG_ESPNOW_STREAM, "  Passphrase: %s", passphrase.c_str());
  DEBUGF(DEBUG_ESPNOW_STREAM, "  Salt Input: %s", saltedInput.c_str());
  DEBUGF(DEBUG_ESPNOW_STREAM, "  Derived Key: %s", keyStr);
  broadcastOutput("[ESP-NOW] Encryption key derived from passphrase");
}

// Set ESP-NOW passphrase and derive encryption key
static void setEspNowPassphrase(const String& passphrase) {
  if (!gEspNow) return;
  gEspNow->passphrase = passphrase;
  // TODO: Add espnowPassphrase field to Settings struct for persistence
  // gSettings.espnowPassphrase = passphrase;
  deriveKeyFromPassphrase(passphrase, gEspNow->derivedKey);
  writeSettingsJson();  // Save to filesystem
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

    // DEBUG: Show encryption key being used for this peer
    char keyStr[33];
    snprintf(keyStr, sizeof(keyStr), "%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X",
             encryptionKey[0], encryptionKey[1], encryptionKey[2], encryptionKey[3],
             encryptionKey[4], encryptionKey[5], encryptionKey[6], encryptionKey[7],
             encryptionKey[8], encryptionKey[9], encryptionKey[10], encryptionKey[11],
             encryptionKey[12], encryptionKey[13], encryptionKey[14], encryptionKey[15]);

    DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] DEBUG PEER ENCRYPTION:");
    DEBUGF(DEBUG_ESPNOW_STREAM, "  Peer MAC: %s", formatMacAddress(mac).c_str());
    DEBUGF(DEBUG_ESPNOW_STREAM, "  Encryption Key: %s", keyStr);
    broadcastOutput("[ESP-NOW] Adding encrypted peer: " + formatMacAddress(mac));
  } else {
    peerInfo.encrypt = false;
    broadcastOutput("[ESP-NOW] Adding unencrypted peer: " + formatMacAddress(mac));
  }

  esp_err_t result = esp_now_add_peer(&peerInfo);
  if (result != ESP_OK) {
    broadcastOutput("[ESP-NOW] Failed to add peer: " + String(result));
    return false;
  }

  return true;
}

// Send ESP-NOW response via router (v2 JSON RESPONSE messages)
void sendChunkedResponse(const uint8_t* targetMac, bool success, const String& result, const String& senderName) {
  if (!gEspNow) return;
  
  // Temporarily suspend streaming to prevent feedback during response transmission
  bool wasStreaming = gEspNow->streamingSuspended;
  gEspNow->streamingSuspended = true;

  // Build v2 JSON RESPONSE message (use compact MAC format)
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String srcMac = macToHexStringCompact(myMac);
  String dstMac = macToHexStringCompact(targetMac);
  
  String statusPrefix = success ? "[SUCCESS] " : "[FAILED] ";
  String fullResult = statusPrefix + result;
  String responseMessage = buildResponseMessage(srcMac.c_str(), dstMac.c_str(), success, fullResult);
  
  broadcastOutput("[ESP-NOW] Sending response to " + senderName + " (" + String(result.length()) + " bytes)");

  // Send via router (handles chunking automatically if needed)
  Message msg;
  memcpy(msg.dstMac, targetMac, 6);
  msg.payload = responseMessage;
  msg.type = MSG_TYPE_RESPONSE_ENUM;
  msg.priority = PRIORITY_HIGH;  // Command responses are high priority
  
  bool sent = routerSend(msg);

  if (sent) {
    broadcastOutput("[ESP-NOW] Response sent (ID: " + String(msg.msgId) + ")");
    DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] Response sent successfully to %s", senderName.c_str());
  } else {
    broadcastOutput("[ESP-NOW] WARNING: Response transmission failed");
  }

  // Restore streaming state
  gEspNow->streamingSuspended = wasStreaming;
}

// Send plain text message via router (v2 JSON TEXT messages)
void sendTextMessage(const uint8_t* targetMac, const String& text) {
  if (!gEspNow) return;
  
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String srcMac = macToHexStringCompact(myMac);
  String dstMac = macToHexStringCompact(targetMac);
  
  String textMessage = buildTextMessage(srcMac.c_str(), dstMac.c_str(), text);
  
  Message msg;
  memcpy(msg.dstMac, targetMac, 6);
  msg.payload = textMessage;
  msg.type = MSG_TYPE_RESPONSE_ENUM;  // Reuse response enum for now
  msg.priority = PRIORITY_NORMAL;
  
  routerSend(msg);
}

// Cleanup expired chunked messages (5 second timeout)
void cleanupExpiredChunkedMessage() {
  if (!gActiveMessage) return;  // Not allocated

  if (gActiveMessage->active && (millis() - gActiveMessage->startTime > 5000)) {
    // Only show timeout if we actually started receiving a real message (totalChunks > 0)
    if (gActiveMessage->totalChunks > 0) {
      broadcastOutput("[ESP-NOW] Chunked message timeout from " + String(gActiveMessage->deviceName) + " - showing partial result:");

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
      broadcastOutput("[ESP-NOW] Error: Incomplete message (" + String(gActiveMessage->receivedChunks) + "/" + String(gActiveMessage->totalChunks) + " chunks received)");
    }

    // Reset state (even if we didn't show timeout - cleanup stale data)
    gActiveMessage->active = false;
    memset(gActiveMessage->buffer, 0, MAX_RESULT_BYTES);
  }
}

// Send stream message to ESP-NOW target (called by broadcastOutput) - v2 JSON TEXT messages
void sendEspNowStreamMessage(const String& message) {
  if (!gEspNow) return;
  DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] sendEspNowStreamMessage: len=%d active=%d target=%s init=%d suspended=%d",
         message.length(), gEspNow->streamActive, gEspNow->streamTarget ? "SET" : "NULL",
         gEspNow->initialized, gEspNow->streamingSuspended);

  if (!gEspNow->streamActive || !gEspNow->streamTarget || !gEspNow->initialized) {
    DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] Exit early - not active/initialized");
    return;  // Streaming not active or not initialized
  }

  // Recursion guard: Don't stream from within ESP-NOW callbacks
  if (gEspNow->streamingSuspended) {
    DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] Exit early - suspended");
    return;  // Prevent feedback loops
  }

  // Rate limiting: max 10 messages/second
  unsigned long now = millis();
  if (now - gEspNow->lastStreamSendTime < STREAM_MIN_INTERVAL_MS) {
    gEspNow->streamDroppedCount++;
    DEBUGF(DEBUG_ESPNOW_STREAM,
           "[STREAM] DROPPED (rate limit) - %lums since last | dropped=%lu sent=%lu | msg: %.50s",
           now - gEspNow->lastStreamSendTime,
           (unsigned long)gEspNow->streamDroppedCount,
           (unsigned long)gEspNow->streamSentCount,
           message.c_str());
    return;  // Drop message (too fast)
  }
  gEspNow->lastStreamSendTime = now;

  // Build v2 JSON TEXT message (use compact MAC format)
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String srcMac = macToHexStringCompact(myMac);
  String dstMac = macToHexStringCompact(gEspNow->streamTarget);
  String streamMsg = buildTextMessage(srcMac.c_str(), dstMac.c_str(), message);
  
  DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] Sending message (%d bytes) target=%s",
         streamMsg.length(), formatMacAddress(gEspNow->streamTarget).c_str());

  Message msg;
  memcpy(msg.dstMac, gEspNow->streamTarget, 6);
  msg.payload = streamMsg;
  msg.type = MSG_TYPE_STREAM_ENUM;
  msg.priority = PRIORITY_LOW;  // Stream output is low priority
  
  bool sent = routerSend(msg);

  if (sent) {
    gEspNow->streamSentCount++;
    DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] SENT successfully | sent=%lu msgId=%lu | %.50s",
           (unsigned long)gEspNow->streamSentCount,
           (unsigned long)msg.msgId,
           message.c_str());
  } else {
    DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] SEND FAILED | sent=%lu dropped=%lu",
           (unsigned long)gEspNow->streamSentCount,
           (unsigned long)gEspNow->streamDroppedCount);
  }
}

// Generic handler for chunked message assembly (TYPE_START/CHUNK/END)
// Returns true if message was handled, false if not recognized
static bool handleGenericChunkedMessage(const String& message, const String& msgType,
                                        const String& deviceName, bool hasStatusField) {
  if (!gActiveMessage) return false;

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
        broadcastOutput("[" + msgType + "] Warning: Missing " + String(gActiveMessage->totalChunks - gActiveMessage->receivedChunks) + " chunks");
      }

      gActiveMessage->active = false;
      memset(gActiveMessage->buffer, 0, MAX_RESULT_BYTES);
    } else {
      broadcastOutput("[" + msgType + "] Error: Hash mismatch");
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

static void onEspNowRawRecv(const esp_now_recv_info* recv_info, const uint8_t* incomingData, int len) {
  // Increment receive counter
  if (gEspNow) gEspNow->routerMetrics.messagesReceived++;
  
  // CRITICAL DEBUG: Log EVERY message received at ESP-NOW layer
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_CALLBACK] ========================================");
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_CALLBACK] === ESP-NOW RECEIVE CALLBACK ENTRY ===");
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_CALLBACK] ========================================");
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_CALLBACK] Message length: %d bytes", len);
  
  if (!recv_info) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_CALLBACK] CRITICAL ERROR: recv_info is NULL!");
    return;
  }
  
  if (!incomingData) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_CALLBACK] CRITICAL ERROR: incomingData is NULL!");
    return;
  }
  
  // Build MAC address string
  String macStr = formatMacAddress(recv_info->src_addr);
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_CALLBACK] Source MAC: %s", macStr.c_str());
  
  // Log RSSI if available
  if (recv_info->rx_ctrl) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_CALLBACK] RSSI: %d dBm", recv_info->rx_ctrl->rssi);
  } else {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_CALLBACK] WARNING: rx_ctrl is NULL (no RSSI)");
  }

  // Check if this device is encrypted and paired
  bool isEncrypted = false;
  bool isPaired = false;
  String deviceName = "";
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (memcmp(gEspNow->devices[i].mac, recv_info->src_addr, 6) == 0) {
      isPaired = true;
      isEncrypted = gEspNow->devices[i].encrypted;
      deviceName = gEspNow->devices[i].name;
      break;
    }
  }
  
  // Use MAC string as device name if not paired
  if (deviceName.length() == 0) {
    deviceName = macStr;
  }

  // Parse message string - create directly from buffer to avoid concatenation
  int msgLen = len < 250 ? len : 250;
  char* tempBuf = (char*)malloc(msgLen + 1);
  String message;
  if (tempBuf) {
    memcpy(tempBuf, incomingData, msgLen);
    tempBuf[msgLen] = '\0';
    message = String(tempBuf);
    free(tempBuf);
  }
  
  {
    EspNowV2InboundPacket v2pkt{recv_info, incomingData, len, (uint32_t)millis()};
    v2_handle_incoming(v2pkt);
  }

  DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_CALLBACK] Device paired: %s, encrypted: %s, name: %s",
         isPaired ? "YES" : "NO",
         isEncrypted ? "YES" : "NO",
         deviceName.c_str());
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_CALLBACK] Raw message (first 80 chars): %.80s", message.c_str());
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_CALLBACK] Message type detection starting...");

  // Attempt v2 JSON-fragment reassembly (always enabled)
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_CALLBACK] Checking for v2 fragments");
  String completed;
  if (v2_frag_try_reassembly(recv_info, message, completed)) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_RX] ✓ Reassembly complete: %d bytes", completed.length());
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_RX] Reassembled content (first 80 chars): %.80s", completed.c_str());
    message = completed;
  } else {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_RX] No complete reassembly (waiting for more fragments or not a fragment)");
  }

process_message:  // Label for re-processing after mesh envelope unwrapping
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_DEBUG] Processing message type check...");

  // Milestone 2: use single dispatch entry wrapper (handles all message types)
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_CALLBACK] Attempting handleIncomingV2 dispatch...");
  if (handleIncomingV2(recv_info, incomingData, len, message,
                       isPaired, isEncrypted, deviceName, macStr)) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_CALLBACK] ✓ Message handled by v2 dispatch system");
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_CALLBACK] ========================================");
    return;
  }
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_CALLBACK] Message NOT handled by v2 dispatch, falling through to legacy...");
  // Fall through to legacy handling ONLY for complex MESH routing (topology, time sync, commands)
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX_DEBUG] Using legacy handler for MESH routing");

  // JSON message handling - check for v2 envelope + mesh TTL/forwarding
  if (message.startsWith("{")) {
    JsonDocument doc;
    if (parseJsonMessage(message, doc)) {
      // Unified v2 envelope: handle ACK/dedup and mesh TTL here, but do NOT
      // unwrap any nested base64 payloads. The on-air schema is the logical
      // v2 JSON itself.
      int v = doc["v"] | 0;
      if (v == 2) {
        // First, let reliability layer consume pure ACK frames (k=="ack")
        if (v2_try_handle_ack(message)) {
          return;  // ACK consumed
        }

        uint32_t msgId = (uint32_t)(doc["id"] | doc["msgId"] | 0);
        int ttl = doc["ttl"] | 0;
        const char* src = doc["src"];
        const char* dst = doc["dst"];
        const char* msgType = doc["type"];

        DEBUGF(DEBUG_ESPNOW_ROUTER, "[MESH_V2_RX] ========================================");
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[MESH_V2_RX] V2 ENVELOPE DETECTED");
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[MESH_V2_RX] ========================================");
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[MESH_V2_RX] Message ID: %lu", (unsigned long)msgId);
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[MESH_V2_RX] TTL: %d", ttl);
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[MESH_V2_RX] Type: %s", msgType ? msgType : "none");
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[MESH_V2_RX] Source: %s", src ? src : "unknown");
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[MESH_V2_RX] Destination: %s", dst ? dst : "unknown");

        // Dedup check for v2 messages (mesh or direct) based on id/src - MANDATORY
        if (msgId != 0) {
          if (v2_dedup_seen_and_insert(recv_info->src_addr, msgId)) {
            if (gEspNow) { gEspNow->routerMetrics.v2DedupDrops++; }
            DEBUGF(DEBUG_ESPNOW_ROUTER, "[MESH_V2_RX] Duplicate detected, dropping id=%lu", (unsigned long)msgId);
            return;
          }
        }

        // Apply TTL-based mesh forwarding for ANY message with TTL > 0
        // Mesh is a transport method, not a message type - any type can be mesh-routed
        bool hasDst = (dst && dst[0] != '\0');
        bool hasTtl = (ttl != 0);
        if (hasDst && hasTtl) {
          uint8_t myMac[6];
          esp_wifi_get_mac(WIFI_IF_STA, myMac);
          // Expand compact dst MAC locally for comparison (no heap allocation)
          const char* dstExpanded = expandCompactMac(dst);
          String myMacStr = macToHexString(myMac);
          bool isForMe = (strcmp(dstExpanded, myMacStr.c_str()) == 0) || (strcmp(dst, "broadcast") == 0);

          if (!isForMe) {
            if (ttl > 0) {
              // Check path to prevent loops - if we're already in the path, drop it
              // Path stores compact MACs (12 chars, no colons)
              String myCompactMac = macToHexStringCompact(myMac);
              JsonArray path = doc["path"].as<JsonArray>();
              if (path) {
                for (JsonVariant mac : path) {
                  const char* pathMac = mac.as<const char*>();
                  if (pathMac && strcmp(pathMac, myCompactMac.c_str()) == 0) {
                    DEBUGF(DEBUG_ESPNOW_ROUTER, "[MESH_V2_RX] ✗ Loop detected: already in path, dropping id=%lu type=%s", 
                           (unsigned long)msgId, msgType ? msgType : "unknown");
                    if (gEspNow) { gEspNow->routerMetrics.meshLoopDetected++; }
                    return;
                  }
                }
              }
              
              // Not for us and not in path - forward with decremented TTL and append our MAC to path
              doc["ttl"] = ttl - 1;
              
              // Add our MAC to the path array (compact format without colons)
              if (!path) {
                path = doc["path"].to<JsonArray>();
              }
              String compactMac = macToHexStringCompact(myMac);
              path.add(compactMac);
              
              // Track path length metrics
              if (gEspNow) {
                uint8_t pathLen = path.size();
                gEspNow->routerMetrics.meshPathLengthSum += pathLen;
                gEspNow->routerMetrics.meshPathLengthCount++;
                if (pathLen > gEspNow->routerMetrics.meshMaxPathLength) {
                  gEspNow->routerMetrics.meshMaxPathLength = pathLen;
                }
              }
              
              String forwarded;
              serializeJson(doc, forwarded);
              DEBUGF(DEBUG_ESPNOW_ROUTER, "[MESH_V2_RX] → Forwarding: id=%lu, type=%s, ttl=%d->%d, dst=%s, path_len=%d",
                     (unsigned long)msgId, msgType ? msgType : "unknown", ttl, ttl - 1, dst ? dst : "unknown", path.size());
              meshSendEnvelopeToPeers(forwarded);
              if (gEspNow) { 
                gEspNow->meshForwards++;
                
                // Track forwards by message type
                int typeIdx = -1;
                if (msgType) {
                  if (strcmp(msgType, MSG_TYPE_HB) == 0) typeIdx = 0;
                  else if (strcmp(msgType, MSG_TYPE_ACK) == 0) typeIdx = 1;
                  else if (strcmp(msgType, MSG_TYPE_MESH_SYS) == 0) typeIdx = 2;
                  else if (strcmp(msgType, MSG_TYPE_FILE_STR) == 0) typeIdx = 3;
                  else if (strcmp(msgType, MSG_TYPE_CMD) == 0) typeIdx = 4;
                  else if (strcmp(msgType, MSG_TYPE_TEXT) == 0) typeIdx = 5;
                  else if (strcmp(msgType, MSG_TYPE_RESPONSE) == 0) typeIdx = 6;
                  else if (strcmp(msgType, MSG_TYPE_STREAM) == 0) typeIdx = 7;
                }
                if (typeIdx >= 0 && typeIdx < 8) {
                  gEspNow->routerMetrics.meshForwardsByType[typeIdx]++;
                }
              }
              return;
            } else {
              DEBUGF(DEBUG_ESPNOW_ROUTER, "[MESH_V2_RX] ✗ TTL expired, dropping id=%lu type=%s", 
                     (unsigned long)msgId, msgType ? msgType : "unknown");
              if (gEspNow) { gEspNow->routerMetrics.meshTTLExhausted++; }
              return;
            }
          }
        }

        // v2 messages that are for us (including mesh-for-me and direct messages)
        // continue into the type-based handlers below.
      }

      // All messages must be v2 - if we get here with v!=2, it's an error
      if (v != 2) {
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[RX] ERROR: Non-v2 message received (v=%d), dropping", v);
        return;
      }
      
      // Get type for legacy handlers
      const char* type = doc["type"];
      
      // Handle heartbeat messages
      if (type && strcmp(type, MSG_TYPE_HB) == 0) {
        if (meshEnabled()) {
          if (isPaired) {
            // Update or create peer health entry for paired device
            MeshPeerHealth* peer = getMeshPeerHealth(recv_info->src_addr, true);
            if (peer) {
              peer->lastHeartbeatMs = millis();
              peer->heartbeatCount++;
              
              uint32_t msgId = doc["msgId"];
              DEBUGF(DEBUG_ESPNOW_STREAM, "[MESH] JSON heartbeat from %s (count=%lu, msgId=%lu)",
                     macStr.c_str(), (unsigned long)peer->heartbeatCount, (unsigned long)msgId);
            }
          } else {
            // Track unpaired device sending heartbeats (for discovery/pairing)
            String srcName = doc["src"] | "";
            if (srcName.length() == 0) {
              srcName = deviceName;  // Use name from earlier lookup if available
            }
            int rssi = recv_info->rx_ctrl ? recv_info->rx_ctrl->rssi : -100;
            updateUnpairedDevice(recv_info->src_addr, srcName, rssi);
            DEBUGF(DEBUG_ESPNOW_STREAM, "[MESH] Unpaired device heartbeat: %s (%s) RSSI=%d",
                   macStr.c_str(), srcName.c_str(), rssi);
          }
        }
        return;  // Don't process as regular message
      }
      
      // Handle ACK messages
      if (type && strcmp(type, MSG_TYPE_ACK) == 0) {
        if (meshEnabled()) {
          uint32_t ackFor = doc["ackFor"];
          
          // Update peer health entry
          MeshPeerHealth* peer = getMeshPeerHealth(recv_info->src_addr, true);
          if (peer) {
            peer->lastAckMs = millis();
            peer->ackCount++;
          }
          
          // Remove from retry queue
          meshRetryDequeue(ackFor);
          
          DEBUGF_BROADCAST(DEBUG_ESPNOW_STREAM, "[MESH] ACK received for msgid=%lu", (unsigned long)ackFor);
        }
        return;
      }
      
      // Handle MESH_SYS routed messages
      if (type && strcmp(type, MSG_TYPE_MESH_SYS) == 0) {
        if (meshEnabled()) {
          JsonObject payload = doc["pld"].as<JsonObject>();
          
          // Handle topology request
          if (!payload["topoReq"].isNull()) {
            JsonObject topoReq = payload["topoReq"].as<JsonObject>();
            if (!topoReq.isNull()) {
              uint32_t reqId = topoReq["req"];
              JsonArray path = topoReq["pth"].as<JsonArray>();
              int ttl = doc["ttl"];
              
              uint8_t myMac[6];
              esp_wifi_get_mac(WIFI_IF_STA, myMac);
              String myMacStr = macToHexString(myMac);
              
              // Check if already processed (my MAC in path = loop detection)
              bool alreadyInPath = false;
              if (path) {
                for (JsonVariant v : path) {
                  if (strcmp(v.as<const char*>(), myMacStr.c_str()) == 0) {
                    alreadyInPath = true;
                    break;
                  }
                }
                DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PATH_DEBUG] Appended %s to JSON request path (now %d hops)", 
                             myMacStr.c_str(), path.size());
              }
              (void)alreadyInPath;
              
              // Modify existing doc: decrement TTL
              doc["ttl"] = ttl - 1;
              
              String fwdRequest;
              serializeJson(doc, fwdRequest);
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_DEBUG] Forwarding REQUEST: reqId=%lu, ttl=%d, msgLen=%d", 
                           (unsigned long)reqId, ttl - 1, fwdRequest.length());
              DEBUGF_BROADCAST(DEBUG_ESPNOW_STREAM, "[TOPO] Forwarding JSON reqId=%lu ttl=%d", (unsigned long)reqId, ttl - 1);
              meshSendEnvelopeToPeers(fwdRequest);
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_DEBUG] REQUEST forwarded");
            } else {
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_DEBUG] TTL=0, not forwarding REQUEST");
            }
            DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_DEBUG] ========================================");
            return;
          }
          
          // Handle time sync
          if (!payload["timeSync"].isNull()) {
            JsonObject timeSync = payload["timeSync"];
            uint32_t epoch = timeSync["epoch"];
            uint32_t senderMillis = timeSync["millis"];
            
            if (epoch > 0) {
              // Calculate time offset: epoch_ms = millis() + offset
              // We received: epoch (in seconds) and senderMillis
              // Convert epoch to milliseconds and calculate offset
              uint64_t epochMs = (uint64_t)epoch * 1000;
              uint32_t myMillis = millis();
              
              // Estimate network delay (assume symmetric, half of round-trip)
              // For simplicity, we'll just use the sender's millis as reference
              gTimeOffset = (int64_t)epochMs - (int64_t)senderMillis;
              gTimeIsSynced = true;
              gLastTimeSyncMs = myMillis;
              
              DEBUG_ESPNOWF("[TIME_SYNC] Received time sync: epoch=%lu, offset=%lld ms", 
                           (unsigned long)epoch, (long long)gTimeOffset);
              DEBUGF_BROADCAST(DEBUG_ESPNOW_STREAM, "[TIME_SYNC] Time synchronized from master (epoch=%lu)", (unsigned long)epoch);
            }
            return;
          }
          
          // Handle topology stream START (NEW PATTERN)
          if (!payload["tStart"].isNull()) {
            JsonObject topoStart = payload["tStart"];
            uint32_t reqId = topoStart["req"];
            uint16_t totalPeers = topoStart["tot"];
            bool isLast = topoStart["last"] | false;  // NEW: explicit completion flag
            const char* src = doc["src"];
            const char* dst = doc["dst"];
            int ttl = doc["ttl"];
            
            DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_STREAM_DEBUG] START from %s: reqId=%lu, total=%d peers, isLast=%s", 
                         src, (unsigned long)reqId, totalPeers, isLast ? "true" : "false");
            
            // Get my MAC for destination check
            uint8_t myMac[6];
            esp_wifi_get_mac(WIFI_IF_STA, myMac);
            String myMacStr = macToHexString(myMac);
            
            // Forward if not for us and TTL > 0 using reverse path
            const char* dstExpanded = expandCompactMac(dst);
            bool isForMe = (strcmp(dstExpanded, myMacStr.c_str()) == 0);
            if (!isForMe && ttl > 0) {
              doc["ttl"] = ttl - 1;
              
              // NOTE: Do NOT create a stream here! Intermediate nodes only create streams
              // when they receive the REQUEST (to store the path for routing PEERs back).
              // START messages are just forwarded - we don't need to track them.
              
              // Use path to route back: find my position and send to previous hop
              JsonArray path = topoStart["pth"];
              if (path && path.size() > 0) {
                // Find my position in the path
                int myIndex = -1;
                for (int i = 0; i < path.size(); i++) {
                  if (strcmp(path[i].as<const char*>(), myMacStr.c_str()) == 0) {
                    myIndex = i;
                    break;
                  }
                }
                
                if (myIndex > 0) {
                  // Send to previous hop in path (unicast)
                  const char* prevHopMac = path[myIndex - 1].as<const char*>();
                  uint8_t prevHopBytes[6];
                  macFromHexString(prevHopMac, prevHopBytes);
                  
                  String forwarded;
                  serializeJson(doc, forwarded);
                  DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PATH_DEBUG] Routing START back to %s (hop %d→%d)", 
                               prevHopMac, myIndex, myIndex - 1);
                  
                  esp_now_send(prevHopBytes, (uint8_t*)forwarded.c_str(), forwarded.length());
                } else {
                  // Intermediate device not in path - broadcast to all peers (including master)
                  DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PATH_DEBUG] My MAC not in path, broadcasting START to all peers");
                  String forwarded;
                  serializeJson(doc, forwarded);
                  meshSendEnvelopeToPeers(forwarded);
                }
              } else {
                DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PATH_DEBUG] WARNING: No path in message, falling back to broadcast");
                String forwarded;
                serializeJson(doc, forwarded);
                meshSendEnvelopeToPeers(forwarded);
              }
            }
            
            // Only process if for us
            if (!isForMe) {
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_STREAM_DEBUG] START not for me, forwarded");
              return;
            }
            
            // Check if this matches our request
            if (reqId != gTopoRequestId || millis() >= gTopoRequestTimeout) {
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_STREAM_DEBUG] START rejected: reqId mismatch or timeout");
              return;
            }
            
            // Convert src MAC string to bytes
            uint8_t srcMacBytes[6];
            macFromHexString(src, srcMacBytes);
            
            // NEW: Find or create stream for this sender+reqId
            TopologyStream* stream = findOrCreateTopoStream(srcMacBytes, reqId);
            if (!stream) {
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_STREAM_DEBUG] ERROR: Could not allocate stream");
              return;
            }
            
            // Check if this stream was already finalized (duplicate message)
            if (!stream->active) {
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_STREAM_DEBUG] START ignored: stream already finalized (duplicate)");
              return;
            }
            // Initialize stream (only if newly created)
            if (stream->receivedPeers == 0) {
              // Get device name from START message (if provided) or look up locally
              String deviceName;
              if (!topoStart["n"].isNull()) {
                deviceName = topoStart["n"].as<String>();
              }
              if (deviceName.length() == 0) {
                deviceName = getTopoDeviceName(srcMacBytes);
                if (deviceName.length() == 0) {
                  deviceName = getEspNowDeviceName(srcMacBytes);
                }
              }
              if (deviceName.length() == 0) {
                deviceName = src;  // Fallback to MAC address
              }
              
              // Add to topology cache for future lookups
              if (deviceName != src) {  // Only cache if we have a real name, not MAC fallback
                addTopoDeviceName(srcMacBytes, deviceName.c_str());
              }
              
              strncpy(stream->senderName, deviceName.c_str(), 31);
              stream->senderName[31] = '\0';
              stream->totalPeers = totalPeers;
              stream->accumulatedData = "";
              
              // Extract and store path from START message
              JsonArray path = topoStart["pth"];
              if (path && path.size() > 0) {
                stream->path = "";
                DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PATH_DEBUG] Extracting path from START message (%d hops)", path.size());
                for (int i = 0; i < path.size(); i++) {
                  const char* hop = path[i].as<const char*>();
                  DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PATH_DEBUG]   Hop %d: %s", i, hop);
                  if (i > 0) stream->path += ",";  // Use comma separator for parsing
                  stream->path += hop;
                }
                DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PATH_DEBUG] Stored path: '%s' (length=%d)", 
                             stream->path.c_str(), stream->path.length());
              } else {
                stream->path = "";
                DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PATH_DEBUG] No path in START message");
              }
              
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_STREAM_DEBUG] Stream initialized for %s: expecting %d peers", 
                           stream->senderName, totalPeers);
            }
            
            // NEW: Check if this is a complete response (0 peers only)
            // Only finalize on START if totalPeers is 0 (edge device with no downstream peers)
            if (isLast && totalPeers == 0) {
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_STREAM_DEBUG] Stream COMPLETE via isLast flag (0 peers - edge device)");
              finalizeTopologyStream(stream);
            } else if (isLast && totalPeers > 0) {
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_STREAM_DEBUG] START has isLast=true but totalPeers=%d, waiting for PEER messages", totalPeers);
              // Update collection window timer
              gTopoLastResponseTime = millis();
            }
            return;
          }
          
          // Handle topology stream PEER data (NEW PATTERN)
          if (!payload["tPeer"].isNull()) {
            JsonObject topoPeer = payload["tPeer"];
            uint32_t reqId = topoPeer["req"];
            uint16_t idx = topoPeer["idx"];
            const char* peerMac = topoPeer["m"];  // Full MAC address
            uint32_t hb = topoPeer["h"];
            uint32_t cnt = topoPeer["c"];
            bool isLast = topoPeer["isLast"] | false;  // NEW: explicit completion flag
            const char* src = doc["src"];
            const char* dst = doc["dst"];
            int ttl = doc["ttl"];

            (void)hb;
            
            DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_STREAM_DEBUG] PEER from %s: reqId=%lu, idx=%d, mac=%s, isLast=%s", 
                         src, (unsigned long)reqId, idx, peerMac, isLast ? "true" : "false");
            
            // Get my MAC for destination check
            uint8_t myMac[6];
            esp_wifi_get_mac(WIFI_IF_STA, myMac);
            String myMacStr = macToHexString(myMac);
            
            // Forward if not for us and TTL > 0 (use stored path for reverse routing)
            const char* dstExpanded = expandCompactMac(dst);
            bool isForMe = (strcmp(dstExpanded, myMacStr.c_str()) == 0);
            DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] dst=%s, myMac=%s, isForMe=%s, ttl=%d",
                         dst, myMacStr.c_str(), isForMe ? "YES" : "NO", ttl);
            
            if (!isForMe && ttl > 0) {
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] ========================================");
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] PEER message needs forwarding");
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] From: %s, To: %s, ReqId: %lu, Idx: %d", src, dst, (unsigned long)reqId, idx);
              
              // Find the stream to get the stored path
              // CRITICAL: Look up by DESTINATION (master) MAC, not sender MAC
              // The stream was created when we forwarded the request FROM the master
              uint8_t dstMacBytes[6];
              macFromHexString(dst, dstMacBytes);
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] Looking for stream: master=%s, reqId=%lu", dst, (unsigned long)reqId);
              TopologyStream* stream = findTopoStream(dstMacBytes, reqId);
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] Stream lookup: %s", stream ? "✓ FOUND" : "✗ NOT FOUND");
              
              if (stream) {
                // Use unified forwarding function
                String originalMsg;
                serializeJson(doc, originalMsg);
                DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] Serialized message: %d bytes", originalMsg.length());
                
                if (!forwardTopologyPeer(originalMsg, stream)) {
                  // Forwarding failed, try broadcast as fallback
                  DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] ⚠ WARNING: Forwarding failed, falling back to broadcast");
                  doc["ttl"] = ttl - 1;  // Still decrement TTL for broadcast
                  String forwarded;
                  serializeJson(doc, forwarded);
                  meshSendEnvelopeToPeers(forwarded);
                }
              } else {
                DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] ✗ No stream found for master=%s, reqId=%lu", dst, (unsigned long)reqId);
                DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] This means REQUEST hasn't arrived yet - buffering PEER");
                
                // Buffer this PEER message instead of broadcasting
                String originalMsg;
                serializeJson(doc, originalMsg);
                if (bufferPeerMessage(originalMsg, reqId, dstMacBytes)) {
                  DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] ✓ PEER buffered successfully");
                } else {
                  DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] ✗ Buffer full, falling back to broadcast");
                  doc["ttl"] = ttl - 1;
                  String forwarded;
                  serializeJson(doc, forwarded);
                  meshSendEnvelopeToPeers(forwarded);
                }
              }
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] PEER handled, exiting handler");
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] ========================================");
            } else {
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] Not forwarding: isForMe=%s, ttl=%d", 
                           isForMe ? "YES" : "NO", ttl);
            }
            
            // Only process if for us
            if (!isForMe) {
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_STREAM_DEBUG] PEER not for me, forwarded (exiting handler)");
              return;
            }
            
            DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] Message IS for me, processing locally...");
            
            // Convert src MAC string to bytes for stream lookup
            uint8_t srcMacBytes[6];
            macFromHexString(src, srcMacBytes);
            
            // For the master, find stream by BOTH src and reqId
            // Each device has its own stream, so we need to match the sender
            DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] Looking up stream for local processing: src=%s, reqId=%lu",
                         src, (unsigned long)reqId);
            
            TopologyStream* stream = findTopoStream(srcMacBytes, reqId);
            if (!stream) {
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] REJECTION: No stream found for reqId=%lu",
                           (unsigned long)reqId);
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] Active streams:");
              for (int i = 0; i < MAX_CONCURRENT_TOPO_STREAMS; i++) {
                if (gTopoStreams[i].active) {
                  String streamSrc = macToHexString(gTopoStreams[i].senderMac);
                  DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG]   [%d] src=%s, reqId=%lu, path='%s'",
                               i, streamSrc.c_str(), (unsigned long)gTopoStreams[i].reqId,
                               gTopoStreams[i].path.c_str());
                }
              }
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_STREAM_DEBUG] PEER rejected: no stream for this reqId");
              return;
            }
            
            DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_PEER_DEBUG] Stream FOUND for local processing: name=%s, totalPeers=%d, received=%d",
                         stream->senderName, stream->totalPeers, stream->receivedPeers);
            
            // Check if stream was already finalized (duplicate message)
            if (!stream->active) {
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_STREAM_DEBUG] PEER ignored: stream already finalized (duplicate)");
              return;
            }
            
            // Check timeout (10 seconds)
            if (millis() - stream->startTime > 10000) {
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_STREAM_DEBUG] PEER rejected: stream timeout");
              stream->active = false;
              return;
            }
            
            // Convert peer MAC for lookups
            uint8_t peerMacBytes[6];
            macFromHexString(peerMac, peerMacBytes);
            
            // Check for duplicate: search accumulatedData for this MAC address
            String macStr = String(peerMac);
            if (stream->accumulatedData.indexOf(macStr) != -1) {
              DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_STREAM_DEBUG] PEER DUPLICATE DETECTED: %s already in stream, skipping", peerMac);
              return;
            }
            
            // Get peer name from message (if provided) or look it up locally
            String peerName;
            if (!topoPeer["n"].isNull()) {
              peerName = topoPeer["n"].as<String>();
            }
            if (peerName.length() == 0) {
              peerName = getTopoDeviceName(peerMacBytes);
              if (peerName.length() == 0) {
                peerName = getEspNowDeviceName(peerMacBytes);
              }
              if (peerName.length() == 0) peerName = "Unknown";
            }
            
            // Add peer to topology cache for future lookups
            if (peerName != "Unknown" && peerName != peerMac) {
              addTopoDeviceName(peerMacBytes, peerName.c_str());
            }
            
            // Calculate "last seen" time (only for devices in local peer table)
            String lastSeenStr = "N/A";
            for (int i = 0; i < MESH_PEER_MAX; i++) {
              if (gMeshPeers[i].isActive && macEqual6(gMeshPeers[i].mac, peerMacBytes)) {
                uint32_t secondsSince = (millis() - gMeshPeers[i].lastHeartbeatMs) / 1000;
                lastSeenStr = String(secondsSince) + "s ago";
                break;
              }
            }
            
            // Accumulate peer info
            String peerInfo = String("  → ") + peerName + " (" + String(peerMac) + ")\n";
            peerInfo += String("    Heartbeats: ") + String(cnt) + ", Last seen: " + lastSeenStr + "\n";
            stream->accumulatedData += peerInfo;
            stream->receivedPeers++;
            
            DEBUG_ESPNOWF("[TOPO_STREAM_DEBUG] Accumulated peer %d/%d", 
                         stream->receivedPeers, stream->totalPeers);
            
            // Update collection window timer - we just received a PEER
            gTopoLastResponseTime = millis();
            DEBUG_ESPNOWF("[TOPO_STREAM_DEBUG] Updated collection window timer (will wait %lums for more PEERs)",
                         (unsigned long)TOPO_COLLECTION_WINDOW_MS);
            
            // NOTE: Do NOT finalize based on isLast flag or count!
            // The isLast flag only means "last peer from THIS node", not "last peer in network"
            // We must wait for the collection window timeout to ensure all PEERs arrive
            // The collection window will finalize when no new PEERs arrive for 3 seconds
            
            return;
          }
          
          if (!payload["tResp"].isNull()) {
            JsonObject topoResp = payload["tResp"];
            uint32_t reqId = topoResp["req"];
            const char* src = doc["src"];  // Use src MAC as device identifier
            JsonArray peers = topoResp["peers"];
            
            DEBUG_ESPNOWF("[TOPO_RESP_DEBUG] JSON topology response from %s: reqId=%lu", 
                         src, (unsigned long)reqId);
            DEBUG_ESPNOWF("[TOPO_RESP_DEBUG] Expected reqId=%lu, timeout=%lu, current millis=%lu",
                         (unsigned long)gTopoRequestId, (unsigned long)gTopoRequestTimeout, (unsigned long)millis());
            
            // Check if response matches our request
            if (reqId == gTopoRequestId && millis() < gTopoRequestTimeout) {
              DEBUG_ESPNOWF("[TOPO_RESP_DEBUG] Response is valid, processing...");
              
              // Format peer data for display
              // Get device name from src MAC
              uint8_t srcMacBytes[6];
              macFromHexString(src, srcMacBytes);
              String deviceName = getEspNowDeviceName(srcMacBytes);
              if (deviceName.length() == 0) deviceName = src;
              
              String peerInfo = String("  Peers: ") + String(peers.size()) + "\n";
              peerInfo += String("Device: ") + String(deviceName) + " (" + String(src) + ")\n";
              
              for (JsonObject peer : peers) {
                // Peer MAC is now short format (last 8 chars), reconstruct full MAC
                const char* peerMacShort = peer["m"];  // e.g., "32:38:4c"
                uint32_t hb = peer["h"];
                uint32_t cnt = peer["c"];
                
                // Reconstruct full MAC by prepending sender's prefix (first 9 chars)
                String srcMacPrefix = String(src).substring(0, 9);  // e.g., "e8:9f:6d:"
                String peerMac = srcMacPrefix + String(peerMacShort);
                
                // Convert MAC string to bytes for getEspNowDeviceName
                uint8_t peerMacBytes[6];
                macFromHexString(peerMac.c_str(), peerMacBytes);
                String peerName = getEspNowDeviceName(peerMacBytes);
                if (peerName.length() == 0) peerName = peerMac;
                
                // Find this peer in our local mesh peer table to get accurate "last seen"
                uint32_t secondsSince = 0;
                bool foundInTable = false;
                for (int i = 0; i < MESH_PEER_MAX; i++) {
                  if (gMeshPeers[i].isActive && macEqual6(gMeshPeers[i].mac, peerMacBytes)) {
                    secondsSince = (millis() - gMeshPeers[i].lastHeartbeatMs) / 1000;
                    foundInTable = true;
                    break;
                  }
                }
                
                // If not in our table, use the reported timestamp (may be inaccurate due to different time bases)
                if (!foundInTable && hb > 0) {
                  secondsSince = (millis() - hb) / 1000;
                }
                
                peerInfo += String("  → ") + peerName + " (" + peerMac + ")\n";
                peerInfo += String("    Heartbeats: ") + String(cnt) + ", Last seen: " + String(secondsSince) + "s ago\n";
              }
              peerInfo += "\n";
              
              gTopoResultsBuffer += peerInfo;
              gTopoResponsesReceived++;
              
              DEBUG_ESPNOWF("[TOPO_RESP_DEBUG] Stored response #%d, buffer length=%d", 
                           gTopoResponsesReceived, gTopoResultsBuffer.length());
              DEBUGF_BROADCAST(DEBUG_ESPNOW_STREAM, "[TOPO] Response from %s: %d peer(s)", deviceName.c_str(), peers.size());
            } else {
              DEBUG_ESPNOWF("[TOPO_RESP_DEBUG] Response REJECTED: reqId mismatch or timeout");
              DEBUG_ESPNOWF("[TOPO_RESP_DEBUG]   reqId match: %s, timeout check: %s",
                           (reqId == gTopoRequestId) ? "YES" : "NO",
                           (millis() < gTopoRequestTimeout) ? "YES" : "NO");
            }
            return;
          }
          
          // Handle command execution
          if (!payload["cmd"].isNull()) {
            const char* cmd = payload["cmd"];
            JsonArray args = payload["args"];
            const char* dst = doc["dst"];
            
            // Check if this message is for us
            uint8_t myMac[6];
            esp_wifi_get_mac(WIFI_IF_STA, myMac);
            String myMacStr = macToHexString(myMac);
            const char* dstExpanded = expandCompactMac(dst);
            
            if (strcmp(dst, "broadcast") == 0 || strcmp(dstExpanded, myMacStr.c_str()) == 0) {
              // Build command string
              String cmdStr = String(cmd);
              for (JsonVariant arg : args) {
                cmdStr += " ";
                cmdStr += arg.as<String>();
              }
              
              DEBUG_ESPNOWF("[MESH_CMD] Executing remote command: %s", cmdStr.c_str());
              DEBUGF_BROADCAST(DEBUG_ESPNOW_STREAM, "[MESH] Remote command from %s: %s", macStr.c_str(), cmdStr.c_str());
              
              // Unwrap and reprocess as a regular message for command execution
              message = cmdStr;
              goto process_message;
            } else {
              // Not for us, forward if TTL > 0
              int ttl = doc["ttl"];
              if (ttl > 0) {
                // Decrement TTL and forward
                doc["ttl"] = ttl - 1;
                String forwarded;
                serializeJson(doc, forwarded);
                
                DEBUGF_BROADCAST(DEBUG_ESPNOW_STREAM, "[MESH] Forwarding command to %s (ttl=%d)", dst, ttl - 1);
                gEspNow->meshForwards++;
                meshSendEnvelopeToPeers(forwarded);
              }
            }
            return;
          }
        }
      }
    }
  }

  // Legacy CHUNK/STREAM removed - v2 fragmentation handles all chunking

  // Check if this is a simple stream message (from remote device with active streaming)
  if (message.startsWith("STREAM:")) {
    String streamContent = message.substring(7);  // Remove "STREAM:" prefix
    String deviceDisplayName = deviceName.length() > 0 ? deviceName : macStr;

    gEspNow->streamReceivedCount++;
    DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] RECEIVED #%lu from %s: len=%d | %.50s",
           (unsigned long)gEspNow->streamReceivedCount,
           deviceDisplayName.c_str(),
           streamContent.length(),
           streamContent.c_str());

    // Display streamed output with clear formatting
    // Note: This may trigger another STREAM send if streaming is bi-directional,
    // Fall through to return; do not print general debug for stream here
    return;
  }

  // General debug for non-stream messages
  if (true) {
    DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] DEBUG MESSAGE RECEIVED:");
    DEBUGF(DEBUG_ESPNOW_STREAM, "  From MAC: %s", macStr.c_str());
    DEBUGF(DEBUG_ESPNOW_STREAM, "  Device Name: %s", (deviceName.length() > 0 ? deviceName.c_str() : "UNKNOWN"));
    DEBUGF(DEBUG_ESPNOW_STREAM, "  Expected Encrypted: %s", (isEncrypted ? "YES" : "NO"));
    DEBUGF(DEBUG_ESPNOW_STREAM, "  Message Length: %d", len);
    DEBUGF(DEBUG_ESPNOW_STREAM, "  Raw Message: '%s'", message.c_str());
  }

  yield();  // Add yield to avoid WDT

  // Legacy CHUNK/STREAM removed - v2 fragmentation handles all chunking
  // Legacy REMOTE: command handling removed - now uses CMD: via dispatch system


  // Display the received message (format compatible with web interface parser)
  String encStatus = isEncrypted ? " [ENCRYPTED]" : " [UNENCRYPTED]";
  if (deviceName.length() > 0) {
    broadcastOutput("[ESP-NOW] Received from " + deviceName + ": " + message + encStatus);
  } else {
    broadcastOutput("[ESP-NOW] Received from " + macStr + ": " + message + encStatus);
  }
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
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[TX_CALLBACK] === ESP-NOW SEND CALLBACK === status=%s",
         status == ESP_NOW_SEND_SUCCESS ? "SUCCESS" : "FAIL");
  
  if (mac_addr) {
    String macStr = formatMacAddress(mac_addr);
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[TX_CALLBACK] Destination MAC: %s", macStr.c_str());
  } else {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[TX_CALLBACK] WARNING: NULL MAC address in callback");
  }
  
  if (!gEspNow) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[TX_CALLBACK] ERROR: gEspNow is NULL");
    return;
  }
  
  // New ESP-IDF v5.x callback signature provides tx_info instead of destination MAC pointer.
  // For compatibility across IDF versions where struct fields may differ, only log status here.
  String statusStr = (status == ESP_NOW_SEND_SUCCESS) ? "Success" : "Failed";

  // Only show failures as operational messages (success is too noisy)
  if (status != ESP_NOW_SEND_SUCCESS) {
    broadcastOutput("[ESP-NOW] Send status: " + statusStr);
    DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] DEBUG: Send callback - status=%s", statusStr.c_str());
  }
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
        JsonDocument doc;
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

      // Choose send path consistent with routerSend() (v2 transport is always compiled)
      bool needsChunking = shouldChunk(qm->msg.payload.length());
      if (needsChunking) {
        success = sendV2Fragmented(qm->msg.dstMac, qm->msg.payload, qm->msg.msgId, isEncrypted, deviceName, false);
      } else {
        success = sendV2Unfragmented(qm->msg.dstMac, qm->msg.payload, qm->msg.msgId, isEncrypted, deviceName, false);
      }
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

// ==========================
// Message Handler Implementations
// ==========================

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
  
  JsonDocument doc;
  if (!parseJsonMessage(ctx.message, doc)) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[Dispatch] Failed to parse JSON");
    return false;
  }
  
  // Handle v2 ACK frames (k=="ack") before type check - ACKs don't have a type field
  if (v2_try_handle_ack(ctx.message)) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[ACK] v2 ACK frame consumed");
    return true;  // ACK consumed
  }
  
  const char* type = doc["type"];
  if (!type) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[Dispatch] JSON missing 'type' field");
    return false;
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

  // Handle FILE transfer messages
  if (strcmp(type, MSG_TYPE_FILE_STR) == 0) {
    JsonObject payload = doc["pld"].as<JsonObject>();
    const char* kind = payload["kind"] | "";

    // FILE_ACK: receiver acknowledging chunk receipt
    if (strcmp(kind, "ack") == 0) {
      uint16_t idx = (uint16_t)(payload["idx"] | 0);
      const char* hash = payload["hash"] | "";
      
      if (strlen(hash) > 0 && strcmp(hash, gEspNow->fileAckHashExpected) == 0) {
        if (idx > gEspNow->fileAckLast) {
          gEspNow->fileAckLast = idx;
        }
        DEBUGF(DEBUG_ESPNOW_STREAM, "[FILE] ACK received: chunk %u, hash %s", idx, hash);
      }
      return true;
    }

    // FILE start/chunk/end: reconstruct legacy string format and delegate to existing handler
    String legacyMsg;
    if (strcmp(kind, "start") == 0) {
      const char* name = payload["name"] | "";
      uint32_t size = (uint32_t)(payload["size"] | 0);
      uint16_t chunks = (uint16_t)(payload["chunks"] | 0);
      const char* hash = payload["hash"] | "";
      legacyMsg = String("FILE_START:") + name + ":" + String(chunks) + ":" + String(size) + ":" + hash;
    } else if (strcmp(kind, "chunk") == 0) {
      uint16_t idx = (uint16_t)(payload["idx"] | 0);
      const char* data = payload["data"] | "";
      legacyMsg = String("FILE_CHUNK:") + String(idx) + ":" + data;
    } else if (strcmp(kind, "end") == 0) {
      const char* hash = payload["hash"] | "";
      legacyMsg = String("FILE_END:") + hash;
    } else {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[FILE] Unknown kind: %s", kind);
      return false;
    }

    // Delegate to existing file transfer handler
    if (legacyMsg.length() > 0) {
      handleFileTransferMessage(legacyMsg, ctx.recvInfo->src_addr);
      return true;
    }
    return false;
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
      broadcastOutput("[ESP-NOW] Remote command: Authentication FAILED for user '" + String(username) + "'");
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[CMD] Auth failed for user '%s'", username);
      return true;
    }

    DEBUGF(DEBUG_ESPNOW_ROUTER, "[CMD] Authentication successful for user '%s'", username);

    // Set execution context for remote command
    gExecAuthContext.user = username;
    gExecAuthContext.ip = "espnow:" + ctx.macStr;
    gExecAuthContext.opaque = (void*)ctx.recvInfo->src_addr;

    // Execute command
    broadcastOutput("[ESP-NOW] Executing remote command from " + ctx.deviceName + ": " + String(command));
    static char cmdResult[2048];
    bool success = executeCommand(gExecAuthContext, command, cmdResult, sizeof(cmdResult));
    const char* result = success ? cmdResult : "Command execution failed";

    // Send response
    bool cmdSuccess = (strstr(result, "[SUCCESS]") != nullptr || strstr(result, "FAILED") == nullptr);
    sendChunkedResponse(ctx.recvInfo->src_addr, cmdSuccess, String(result), ctx.deviceName);

    // Send ACK back to sender
    uint32_t msgId = doc["id"] | doc["msgId"] | 0;
    if (msgId != 0) {
      v2_send_ack(ctx.recvInfo->src_addr, msgId);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[CMD] Sent ACK for msgId=%lu to sender", (unsigned long)msgId);
    }

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
      broadcastOutput("[ESP-NOW] Response from " + deviceName + ":");
      broadcastOutput(String(msg));
      
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[RESPONSE] Remote command result from %s: ok=%s", 
             deviceName.c_str(), ok ? "true" : "false");
      
      // Send ACK back to sender
      uint32_t msgId = doc["id"] | doc["msgId"] | 0;
      if (msgId != 0) {
        v2_send_ack(ctx.recvInfo->src_addr, msgId);
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
          broadcastOutput("[ESP-NOW] User sync SUCCESS from " + deviceName + ": " + String(msg) + 
                         " (user='" + String(username) + "', id=" + String(userId) + ", role=" + String(role) + ")");
        } else {
          INFO_USERF("[USER_SYNC] ✓ %s: %s (user='%s')", deviceName.c_str(), msg, username);
          broadcastOutput("[ESP-NOW] User sync from " + deviceName + ": " + String(msg) + " (user='" + String(username) + "')");
        }
      } else {
        ERROR_USERF("[USER_SYNC] ✗ %s: %s (user='%s')", deviceName.c_str(), msg, username);
        broadcastOutput("[ESP-NOW] User sync FAILED from " + deviceName + ": " + String(msg) + " (user='" + String(username) + "')");
      }
      
      // Send ACK back to sender
      uint32_t msgId = doc["id"] | doc["msgId"] | 0;
      if (msgId != 0) {
        v2_send_ack(ctx.recvInfo->src_addr, msgId);
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[RESPONSE] Sent ACK for msgId=%lu to sender", (unsigned long)msgId);
      }
      
      return true;
    }
    
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[RESPONSE] Unknown kind: %s", kind);
    return false;
  }

  // Handle TEXT (plain text) messages
  if (strcmp(type, MSG_TYPE_TEXT) == 0) {
    JsonObject payload = doc["pld"].as<JsonObject>();
    const char* msg = payload["msg"] | "";
    
    String deviceName = ctx.deviceName.length() > 0 ? ctx.deviceName : ctx.macStr;
    String encStatus = ctx.isEncrypted ? " [ENCRYPTED]" : " [UNENCRYPTED]";
    broadcastOutput("[ESP-NOW] " + deviceName + ": " + String(msg) + encStatus);
    
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[TEXT] Plain text from %s: %.80s", deviceName.c_str(), msg);
    
    // Store in per-device message buffer for web UI and OLED
    storeMessageInPeerHistory(
      (uint8_t*)ctx.recvInfo->src_addr,
      deviceName.c_str(),
      msg,
      ctx.isEncrypted,
      MSG_TEXT
    );
    
    // Send ACK back to sender
    uint32_t msgId = doc["id"] | doc["msgId"] | 0;
    if (msgId != 0) {
      v2_send_ack(ctx.recvInfo->src_addr, msgId);
      broadcastOutput("[ESP-NOW] Sending ACK for msgId=" + String((unsigned long)msgId));
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[TEXT] Sent ACK for msgId=%lu to sender", (unsigned long)msgId);
    } else {
      broadcastOutput("[ESP-NOW] WARNING: TEXT message has no msgId, cannot send ACK");
    }
    
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
        
        broadcastOutput("[ESP-NOW] File listing from " + deviceName + " for path: " + String(resultPath));
        broadcastOutput("--------------------------------------------");
        
        if (files.size() == 0) {
          broadcastOutput("  (empty directory)");
        } else {
          for (JsonVariant file : files) {
            String name = file["name"] | "";
            String type = file["type"] | "file";
            String size = file["size"] | "";
            
            if (type == "folder") {
              broadcastOutput("  [DIR]  " + name + "/");
            } else {
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
        broadcastOutput("[ESP-NOW] File browse FAILED from " + deviceName + ": " + String(error));
      }
      
      // Send ACK
      uint32_t msgId = doc["id"] | doc["msgId"] | 0;
      if (msgId != 0) {
        v2_send_ack(ctx.recvInfo->src_addr, msgId);
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
      broadcastOutput("[ESP-NOW] File browse: Authentication FAILED for user '" + String(username) + "'");
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
      Message msg;
      msg.payload = respStr;
      memcpy(msg.dstMac, ctx.recvInfo->src_addr, 6);
      msg.priority = PRIORITY_HIGH;
      routerSend(msg);
      
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[FILE_BROWSE] Sent list response for path '%s' ok=%d", path, ok);
      broadcastOutput("[ESP-NOW] File browse: Sent directory listing for " + String(path));
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
        
        String respStr;
        serializeJson(respDoc, respStr);
        Message msg;
        msg.payload = respStr;
        memcpy(msg.dstMac, ctx.recvInfo->src_addr, 6);
        msg.priority = PRIORITY_HIGH;
        routerSend(msg);
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
        
        String respStr;
        serializeJson(respDoc, respStr);
        Message msg;
        msg.payload = respStr;
        memcpy(msg.dstMac, ctx.recvInfo->src_addr, 6);
        msg.priority = PRIORITY_HIGH;
        routerSend(msg);
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
        
        String respStr;
        serializeJson(respDoc, respStr);
        Message msg;
        msg.payload = respStr;
        memcpy(msg.dstMac, ctx.recvInfo->src_addr, 6);
        msg.priority = PRIORITY_HIGH;
        routerSend(msg);
      }
      return true;
    }

    INFO_ESPNOWF("[USER_SYNC] Admin authentication successful for '%s'", adminUser);

    // Check if target user already exists
    uint32_t existingUserId = 0;
    if (getUserIdByUsername(String(targetUser), existingUserId)) {
      WARN_ESPNOWF("[USER_SYNC] User '%s' already exists (id=%u) - skipping", targetUser, (unsigned)existingUserId);
      broadcastOutput("[ESP-NOW] User sync: User '" + String(targetUser) + "' already exists");
      
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
        pld["userId"] = existingUserId;
        
        String respStr;
        serializeJson(respDoc, respStr);
        Message msg;
        msg.payload = respStr;
        memcpy(msg.dstMac, ctx.recvInfo->src_addr, 6);
        msg.priority = PRIORITY_HIGH;
        routerSend(msg);
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
        
        String respStr;
        serializeJson(respDoc, respStr);
        Message msg;
        msg.payload = respStr;
        memcpy(msg.dstMac, ctx.recvInfo->src_addr, 6);
        msg.priority = PRIORITY_HIGH;
        routerSend(msg);
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
    newUser["password"] = hashedPassword;
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

    // Create default user settings
    uint32_t createdUserId = (uint32_t)nextId;
    if (createdUserId > 0) {
      String settingsPath = getUserSettingsPath(createdUserId);
      if (!LittleFS.exists(settingsPath.c_str())) {
        JsonDocument defaults;
        defaults["theme"] = "light";
        if (!saveUserSettings(createdUserId, defaults)) {
          WARN_ESPNOWF("[USER_SYNC] Failed to create default settings for userId=%u", (unsigned)createdUserId);
        }
      }
    }

    INFO_ESPNOWF("[USER_SYNC] ✓ Created user '%s' (id=%d, role=%s) from %s", 
           targetUser, nextId, role, ctx.deviceName.c_str());
    broadcastOutput("[ESP-NOW] User sync: Created user '" + String(targetUser) + "' (role=" + String(role) + ") from " + ctx.deviceName);

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
      
      String respStr;
      serializeJson(respDoc, respStr);
      Message msg;
      msg.payload = respStr;
      memcpy(msg.dstMac, ctx.recvInfo->src_addr, 6);
      msg.priority = PRIORITY_HIGH;
      routerSend(msg);
      
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

/**
 * @brief Handle file transfer messages (FILE_START:, FILE_CHUNK:, FILE_END:, FILE_ACK:)
 * @param ctx Received message context
 * @return true if handled
 */
static bool handleFileTransferMessage(const ReceivedMessage& ctx) {
  // CRITICAL: Validate ctx.recvInfo is not NULL before dereferencing
  if (!ctx.recvInfo) {
    DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] ERROR: handleFileTransferMessage called with NULL recvInfo");
    return false;
  }
  
  // Delegate to existing handleFileTransferMessage function
  handleFileTransferMessage(ctx.message, ctx.recvInfo->src_addr);
  return true;
}

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
  sendChunkedResponse(ctx.recvInfo->src_addr, success, resultStr, ctx.deviceName);
  
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
  
  // If using mesh routing, inject TTL into the payload
  String finalPayload = msg.payload;
  if (useMesh) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, msg.payload);
    if (!err) {
      // Add TTL for mesh routing if not already present
      if (doc["ttl"].isNull() || doc["ttl"] == 0) {
        // Update meshTTL if adaptive mode is enabled
        if (gSettings.meshAdaptiveTTL) {
          gSettings.meshTTL = calculateAdaptiveTTL();
        }
        
        doc["ttl"] = gSettings.meshTTL;
        finalPayload = "";
        serializeJson(doc, finalPayload);
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[Router] Added TTL=%d for mesh routing (%s)", 
               gSettings.meshTTL, gSettings.meshAdaptiveTTL ? "adaptive" : "fixed");
      }
    } else {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[Router] WARNING: Failed to parse payload for TTL injection");
    }
  }
  
  // Extract the actual msgId from the JSON payload (it may have been auto-generated)
  uint32_t actualMsgId = msg.msgId;
  if (finalPayload.startsWith("{")) {
    JsonDocument doc;
    if (!deserializeJson(doc, finalPayload)) {
      actualMsgId = doc["id"] | doc["msgId"] | msg.msgId;
      if (actualMsgId != msg.msgId) {
        broadcastOutput("[ROUTER] Extracted msgId=" + String((unsigned long)actualMsgId) + " from JSON (was " + String((unsigned long)msg.msgId) + ")");
      }
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

// Unused helper functions removed (flagged by compiler warnings)

// Unified v2 fragmented send - handles both direct (single MAC) and mesh (broadcast to all peers)
// @param mac - destination MAC for direct send, or NULL for mesh broadcast
// @param payload - full v2 JSON message to fragment
// @param msgId - message ID for dedup/ack
// @param isEncrypted - whether destination uses encryption (direct only)
// @param deviceName - device name for logging (direct only)
// @param isMesh - if true, send to all peers; if false, send to single mac
static bool sendV2Fragmented(const uint8_t* mac, const String& payload, uint32_t msgId,
                             bool isEncrypted, const String& deviceName, bool isMesh) {
  (void)isEncrypted; (void)deviceName;

  // Payload is already the full v2 JSON logical message string.
  // Split it into safe-sized chunks so that each fragment frame stays <250 bytes.
  const size_t perFragmentBytes = 180;  // conservative
  uint16_t n = (payload.length() + perFragmentBytes - 1) / perFragmentBytes;
  if (n == 0) n = 1;
  if (n > V2_FRAG_MAX) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_TX] Too many fragments (n=%u > %u), aborting", (unsigned)n, (unsigned)V2_FRAG_MAX);
    return false;
  }
  
  // Get list of target MACs
  uint8_t targets[MESH_PEER_MAX][6];
  int targetCount = 0;
  
  if (isMesh) {
    // Mesh: send to all paired peers, but filter out those already in the path
    // Parse the payload to extract the path array (compact MACs, no colons)
    JsonDocument pathDoc;
    JsonArray pathArray;
    if (!deserializeJson(pathDoc, payload)) {
      pathArray = pathDoc["path"].as<JsonArray>();
    }
    
    esp_now_peer_info_t peer;
    esp_err_t ret = esp_now_fetch_peer(true, &peer);
    while (ret == ESP_OK && targetCount < MESH_PEER_MAX) {
      // Skip self to prevent sending to own MAC
      if (isSelfMac(peer.peer_addr)) {
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_TX] Skipping self MAC");
        ret = esp_now_fetch_peer(false, &peer);
        continue;
      }
      
      String peerMacCompact = macToHexStringCompact(peer.peer_addr);
      
      // Check if this peer is already in the path (compact format comparison)
      bool inPath = false;
      if (pathArray) {
        for (JsonVariant pathMac : pathArray) {
          const char* pathMacStr = pathMac.as<const char*>();
          if (pathMacStr && strcmp(pathMacStr, peerMacCompact.c_str()) == 0) {
            inPath = true;
            DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_TX] Skipping peer %s (already in path)", peerMacCompact.c_str());
            break;
          }
        }
      }
      
      if (!inPath) {
        memcpy(targets[targetCount], peer.peer_addr, 6);
        targetCount++;
      }
      ret = esp_now_fetch_peer(false, &peer);
    }
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_TX] Mesh mode: sending to %d peers (filtered by path)", targetCount);
  } else {
    // Direct: send to single MAC
    if (mac) {
      memcpy(targets[0], mac, 6);
      targetCount = 1;
    }
  }

  if (targetCount == 0) {
    return false;
  }

  int ackSlot = v2_ack_wait_register(msgId);

  for (uint16_t i = 0; i < n; i++) {
    size_t start = (size_t)i * perFragmentBytes;
    size_t len = (start + perFragmentBytes <= (size_t)payload.length()) ? perFragmentBytes : (size_t)payload.length() - start;
    String part = payload.substring(start, start + len);

    JsonDocument doc;
    doc["v"] = 2;
    doc["id"] = msgId;
    JsonObject frag = doc["frag"].to<JsonObject>();
    frag["i"] = i;
    frag["n"] = n;
    doc["data"] = part;

    String frame;
    serializeJson(doc, frame);

    // Send this fragment to all targets
    for (int t = 0; t < targetCount; t++) {
      gEspNow->txDone = false;
      yield();
      esp_err_t result = esp_now_send(targets[t], (uint8_t*)frame.c_str(), frame.length());
      if (result != ESP_OK) {
        String dstMac = macToHexString(targets[t]);
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_TX] esp_now_send error %d on frag %u/%u to %s", 
               (int)result, (unsigned)(i+1), (unsigned)n, dstMac.c_str());
        continue; // Try next target
      }
      unsigned long startWait = millis();
      const unsigned long timeoutMs = 400;
      while (!gEspNow->txDone && (millis() - startWait) < timeoutMs) { delay(1); yield(); }
      if (!gEspNow->txDone || gEspNow->lastStatus != ESP_NOW_SEND_SUCCESS) {
        String dstMac = macToHexString(targets[t]);
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_TX] send status fail on frag %u/%u to %s", 
               (unsigned)(i+1), (unsigned)n, dstMac.c_str());
        continue; // Try next target
      }
      String dstMac = macToHexString(targets[t]);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_TX] ✓ Fragment %d/%d sent to %s (id=%lu, len=%d)",
             i + 1, n, dstMac.c_str(), (unsigned long)msgId, frame.length());
      if (gEspNow) { gEspNow->routerMetrics.v2FragTx++; }
    }
  }
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_TX] ✓ All %d fragments sent to %d target(s) for id=%lu (total %d bytes)",
         n, targetCount, (unsigned long)msgId, payload.length());
  if (ackSlot >= 0) {
    // Adaptive timeout: 500ms for direct, 1500ms for mesh (fragmented messages need more time)
    uint32_t timeout = isMesh ? 1500 : 500;
    bool got = v2_ack_wait_block(msgId, timeout);
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_FRAG_TX] ACK %s for id=%lu (timeout=%lums)", got ? "OK" : "TIMEOUT", (unsigned long)msgId, (unsigned long)timeout);
    if (gEspNow) {
      if (got) {
        gEspNow->routerMetrics.v2AckRx++;
        gEspNow->lastAckReceived = true;
      } else {
        gEspNow->routerMetrics.v2AckTimeoutFrag++;
        gEspNow->lastAckReceived = false;
      }
    }
    gV2AckWait[ackSlot].active = false;
    if (!got) { return false; }
  }
  return true;
}

// Unified v2 small message send - handles both direct (single MAC) and mesh (broadcast to all peers)
// @param mac - destination MAC for direct send, or NULL for mesh broadcast
// @param payload - full v2 JSON message (must be small enough to fit in one packet)
// @param msgId - message ID for dedup/ack
// @param isEncrypted - whether destination uses encryption (direct only)
// @param deviceName - device name for logging (direct only)
// @param isMesh - if true, send to all peers; if false, send to single mac
static bool sendV2Unfragmented(const uint8_t* mac, const String& payload, uint32_t msgId,
                               bool isEncrypted, const String& deviceName, bool isMesh) {
  (void)isEncrypted; (void)deviceName;
  // Payload is already the v2 logical JSON message string.
  String frame = payload;

  // Get list of target MACs
  uint8_t targets[MESH_PEER_MAX][6];
  int targetCount = 0;
  
  if (isMesh) {
    // Mesh: send to all paired peers, but filter out those already in the path
    // Parse the payload to extract the path array (compact MACs, no colons)
    JsonDocument pathDoc;
    JsonArray pathArray;
    if (!deserializeJson(pathDoc, payload)) {
      pathArray = pathDoc["path"].as<JsonArray>();
    }
    
    esp_now_peer_info_t peer;
    esp_err_t ret = esp_now_fetch_peer(true, &peer);
    while (ret == ESP_OK && targetCount < MESH_PEER_MAX) {
      // Skip self to prevent sending to own MAC
      if (isSelfMac(peer.peer_addr)) {
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_SMALL_TX] Skipping self MAC");
        ret = esp_now_fetch_peer(false, &peer);
        continue;
      }
      
      String peerMacCompact = macToHexStringCompact(peer.peer_addr);
      
      // Check if this peer is already in the path (compact format comparison)
      bool inPath = false;
      if (pathArray) {
        for (JsonVariant pathMac : pathArray) {
          const char* pathMacStr = pathMac.as<const char*>();
          if (pathMacStr && strcmp(pathMacStr, peerMacCompact.c_str()) == 0) {
            inPath = true;
            DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_SMALL_TX] Skipping peer %s (already in path)", peerMacCompact.c_str());
            break;
          }
        }
      }
      
      if (!inPath) {
        memcpy(targets[targetCount], peer.peer_addr, 6);
        targetCount++;
      }
      ret = esp_now_fetch_peer(false, &peer);
    }
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_SMALL_TX] Mesh mode: sending to %d peers (filtered by path)", targetCount);
  } else {
    // Direct: send to single MAC
    if (mac) {
      memcpy(targets[0], mac, 6);
      targetCount = 1;
    }
  }

  if (targetCount == 0) {
    return false;
  }

  // Register ACK waiter
  int ackSlot = v2_ack_wait_register(msgId);

  // Transmit to all targets
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_SMALL_TX] Sending v2 JSON to %d target(s): id=%lu, len=%d bytes", 
         targetCount, (unsigned long)msgId, frame.length());
  
  bool anySuccess = false;
  for (int t = 0; t < targetCount; t++) {
    gEspNow->txDone = false;
    yield();
    esp_err_t result = esp_now_send(targets[t], (uint8_t*)frame.c_str(), frame.length());
    if (result != ESP_OK) {
      String dstMac = macToHexString(targets[t]);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_SMALL_TX] esp_now_send error %d to %s", (int)result, dstMac.c_str());
      continue;
    }
    if (gEspNow) { gEspNow->routerMetrics.v2SmallTx++; }
    unsigned long startWait = millis();
    const unsigned long timeoutMs = 400;
    while (!gEspNow->txDone && (millis() - startWait) < timeoutMs) { delay(1); yield(); }
    if (!gEspNow->txDone || gEspNow->lastStatus != ESP_NOW_SEND_SUCCESS) {
      String dstMac = macToHexString(targets[t]);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_SMALL_TX] send status fail to %s", dstMac.c_str());
      continue;
    }
    anySuccess = true;
    String dstMac = macToHexString(targets[t]);
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_SMALL_TX] ✓ Send successful to %s", dstMac.c_str());
  }

  if (!anySuccess) {
    return false;
  }

  if (ackSlot >= 0) {
    // Adaptive timeout: 200ms for direct, 800ms for mesh
    uint32_t timeout = isMesh ? 800 : 200;
    bool got = v2_ack_wait_block(msgId, timeout);
    if (got) {
      broadcastOutput("[V2_SMALL_TX] ✓ Send SUCCESS with ACK for msgId=" + String((unsigned long)msgId));
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_SMALL_TX] ✓ ACK received for id=%lu (timeout=%lums)", (unsigned long)msgId, (unsigned long)timeout);
      if (gEspNow) { 
        gEspNow->routerMetrics.v2AckRx++;
        gEspNow->lastAckReceived = true;
      }
    } else {
      broadcastOutput("[V2_SMALL_TX] ✗ Send FAILED - ACK timeout for msgId=" + String((unsigned long)msgId));
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V2_SMALL_TX] ✗ ACK timeout for id=%lu", (unsigned long)msgId);
      if (gEspNow) { 
        gEspNow->routerMetrics.v2AckTimeoutSmall++;
        gEspNow->lastAckReceived = false;
      }
    }
    gV2AckWait[ackSlot].active = false;
    return got;
  } else {
    broadcastOutput("[V2_SMALL_TX] WARNING: No ACK slot registered for msgId=" + String((unsigned long)msgId));
  }
  return true;
}

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

// Helper: Find existing topology stream by sender MAC + reqId
static TopologyStream* findTopoStream(const uint8_t* senderMac, uint32_t reqId) {
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
      DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_CACHE] Updated device: %s = %s", macToHexString(mac).c_str(), name);
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
      DEBUGF(DEBUG_ESPNOW_TOPO, "[TOPO_CACHE] Added device: %s = %s", macToHexString(mac).c_str(), name);
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
                    (unsigned long)reqId, macToHexString(masterMac).c_str(), i);
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
  JsonDocument doc;
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
// These functions were moved from HardwareOnev2.1.ino to fix linker errors
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

// Helper: Format MAC address as string
String formatMacAddress(const uint8_t* mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return String(buf);
}

// Helper: Parse MAC address from string (flexible format)
static bool parseMacAddress(const String& macStr, uint8_t mac[6]) {
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
      gSettings.espnowFirstTimeSetup = true;
      (void)writeSettingsJson();
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
    "After setting the name, run 'espnow init' again.\n"
  );
  
  return getDebugBuffer();
}

// Helper: Initialize ESP-NOW subsystem (static - internal use only)
static bool initEspNow() {
  // Capture heap before initialization
  size_t heapBefore = ESP.getFreeHeap();
  
  // Allocate ESP-NOW state on first use (saves ~2KB heap if never used)
  if (!gEspNow) {
    gEspNow = (EspNowState*)ps_alloc(sizeof(EspNowState), AllocPref::PreferPSRAM, "espnow.state");
    if (!gEspNow) {
      broadcastOutput("[ESP-NOW] ERROR: Failed to allocate state structure");
      return false;
    }
    memset(gEspNow, 0, sizeof(EspNowState));
    broadcastOutput("[ESP-NOW] Allocated state structure (" + String(sizeof(EspNowState)) + " bytes)");
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

  // Set WiFi mode to STA+AP to enable ESP-NOW
  WiFi.mode(WIFI_AP_STA);

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
      broadcastOutput("[ESP-NOW] Allocated chunked message buffer (" + String(sizeof(ChunkedMessage)) + " bytes)");
    } else {
      broadcastOutput("[ESP-NOW] WARNING: Failed to allocate chunked message buffer - remote commands may fail");
    }
  }

  broadcastOutput("[ESP-NOW] Initialized successfully on channel " + String(gEspNow->channel));

  // Restore encryption passphrase from settings (if previously set)
  if (gSettings.espnowPassphrase.length() > 0) {
    gEspNow->passphrase = gSettings.espnowPassphrase;
    deriveKeyFromPassphrase(gSettings.espnowPassphrase, gEspNow->derivedKey);
    broadcastOutput("[ESP-NOW] Restored encryption passphrase from settings");
  }

  // Add broadcast peer for public heartbeat mode
  esp_now_peer_info_t broadcastPeer;
  memset(&broadcastPeer, 0, sizeof(broadcastPeer));
  memset(broadcastPeer.peer_addr, 0xFF, 6);  // FF:FF:FF:FF:FF:FF
  broadcastPeer.channel = gEspNow->channel;
  broadcastPeer.encrypt = false;
  
  esp_err_t addStatus = esp_now_add_peer(&broadcastPeer);
  if (addStatus == ESP_OK) {
    broadcastOutput("[ESP-NOW] Broadcast peer (FF:FF:FF:FF:FF:FF) registered for public heartbeat mode");
  } else if (addStatus == ESP_ERR_ESPNOW_EXIST) {
    // Already exists, that's fine
  } else {
    broadcastOutput("[ESP-NOW] WARNING: Failed to add broadcast peer (error " + String(addStatus) + ")");
  }

  // Load and restore saved devices
  loadEspNowDevices();
  restoreEspNowPeers();
  
  // Load mesh peer health data
  loadMeshPeers();
  
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
          saveEspNowDevices();
          broadcastOutput("[ESP-NOW] Updated own device name: " + myName);
        }
        break;
      }
    }
    
    if (!alreadyRegistered) {
      addEspNowDevice(myMac, myName, false, nullptr);
      saveEspNowDevices();
      broadcastOutput("[ESP-NOW] Registered own device name: " + myName);
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
  broadcastOutput("[ESP-NOW] Heap allocated: ~" + String(heapUsed / 1024) + " KB (includes task stack, buffers, peer storage)");
  broadcastOutput("[ESP-NOW] NOTE: This heap remains allocated until device reboot. Disable and re-init will not free all memory.");

  return true;
}

// ESP-NOW init command
const char* cmd_espnow_init(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (gEspNow && gEspNow->initialized) {
    return "ESP-NOW already initialized";
  }

  if (initEspNow()) {
    return "ESP-NOW initialized successfully";
  } else {
    return "Failed to initialize ESP-NOW";
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
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  char* p = getDebugBuffer();
  size_t remaining = 1024;
  
  int n = snprintf(p, remaining, "ESP-NOW Statistics:\n");
  p += n; remaining -= n;
  
  n = snprintf(p, remaining, "  Messages Sent: %lu\n", (unsigned long)gEspNow->routerMetrics.messagesSent);
  p += n; remaining -= n;
  
  n = snprintf(p, remaining, "  Messages Received: %lu\n", (unsigned long)gEspNow->routerMetrics.messagesReceived);
  p += n; remaining -= n;
  
  n = snprintf(p, remaining, "  Send Failures: %lu\n", (unsigned long)gEspNow->routerMetrics.messagesFailed);
  p += n; remaining -= n;
  
  n = snprintf(p, remaining, "  Receive Errors: %lu\n", (unsigned long)gEspNow->receiveErrors);
  p += n; remaining -= n;
  
  n = snprintf(p, remaining, "  Stream Sent: %lu\n", (unsigned long)gEspNow->streamSentCount);
  p += n; remaining -= n;
  
  n = snprintf(p, remaining, "  Stream Received: %lu\n", (unsigned long)gEspNow->streamReceivedCount);
  p += n; remaining -= n;
  
  n = snprintf(p, remaining, "  Stream Dropped: %lu\n", (unsigned long)gEspNow->streamDroppedCount);
  p += n; remaining -= n;
  
  if (meshEnabled()) {
    n = snprintf(p, remaining, "  Heartbeats Sent: %lu\n", (unsigned long)gEspNow->heartbeatsSent);
    p += n; remaining -= n;
    
    n = snprintf(p, remaining, "  Heartbeats Received: %lu\n", (unsigned long)gEspNow->heartbeatsReceived);
    p += n; remaining -= n;
    
    n = snprintf(p, remaining, "  Mesh Forwards: %lu\n", (unsigned long)gEspNow->meshForwards);
    p += n; remaining -= n;
  }
  
  n = snprintf(p, remaining, "  Files Sent: %lu\n", (unsigned long)gEspNow->fileTransfersSent);
  p += n; remaining -= n;
  
  n = snprintf(p, remaining, "  Files Received: %lu\n", (unsigned long)gEspNow->fileTransfersReceived);
  p += n; remaining -= n;
  
  if (gEspNow->lastResetTime > 0) {
    unsigned long uptime = (millis() - gEspNow->lastResetTime) / 1000;
    n = snprintf(p, remaining, "  Uptime: %lus\n", uptime);
  } else {
    n = snprintf(p, remaining, "  Uptime: %lus (since boot)\n", millis() / 1000);
  }
  
  return getDebugBuffer();
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
  
  broadcastOutput("\nV2 Fragments:");
  BROADCAST_PRINTF("  TX Fragments: %lu", (unsigned long)gEspNow->routerMetrics.v2FragTx);
  BROADCAST_PRINTF("  RX Fragments: %lu", (unsigned long)gEspNow->routerMetrics.v2FragRx);
  BROADCAST_PRINTF("  RX Completed: %lu", (unsigned long)gEspNow->routerMetrics.v2FragRxCompleted);
  BROADCAST_PRINTF("  RX GC: %lu", (unsigned long)gEspNow->routerMetrics.v2FragRxGc);
  
  broadcastOutput("\nV2 Reliability:");
  BROADCAST_PRINTF("  Small TX: %lu", (unsigned long)gEspNow->routerMetrics.v2SmallTx);
  BROADCAST_PRINTF("  Ack TX: %lu", (unsigned long)gEspNow->routerMetrics.v2AckTx);
  BROADCAST_PRINTF("  Ack RX: %lu", (unsigned long)gEspNow->routerMetrics.v2AckRx);
  BROADCAST_PRINTF("  Dedup Drops: %lu", (unsigned long)gEspNow->routerMetrics.v2DedupDrops);
  BROADCAST_PRINTF("  Ack Timeout (Small): %lu", (unsigned long)gEspNow->routerMetrics.v2AckTimeoutSmall);
  BROADCAST_PRINTF("  Ack Timeout (Frag): %lu", (unsigned long)gEspNow->routerMetrics.v2AckTimeoutFrag);
  
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
    return "ESP-NOW not initialized. Run 'espnow init' first.";
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
    int peerCount = getMeshPeerCount();
    snprintf(getDebugBuffer(), 1024, "Mesh TTL: %d\nAdaptive mode: %s\nActive peers: %d", 
             gSettings.meshTTL, gSettings.meshAdaptiveTTL ? "enabled" : "disabled", peerCount);
    return getDebugBuffer();
  }
  
  // Check for 'adaptive' command
  args.toLowerCase();
  if (args == "adaptive") {
    gSettings.meshAdaptiveTTL = !gSettings.meshAdaptiveTTL;
    
    // If enabling adaptive, update TTL immediately
    if (gSettings.meshAdaptiveTTL) {
      gSettings.meshTTL = calculateAdaptiveTTL();
    }
    
    (void)writeSettingsJson();
    snprintf(getDebugBuffer(), 1024, "Adaptive TTL %s (TTL now %d)", 
             gSettings.meshAdaptiveTTL ? "enabled" : "disabled", gSettings.meshTTL);
    return getDebugBuffer();
  }
  
  int ttl = args.toInt();
  if (ttl < 1 || ttl > 10) {
    return "Error: TTL must be between 1 and 10, or 'adaptive' to toggle";
  }
  
  // Setting a manual TTL disables adaptive mode
  gSettings.meshTTL = (uint8_t)ttl;
  gSettings.meshAdaptiveTTL = false;
  (void)writeSettingsJson();
  
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
  pos += snprintf(buf + pos, 1024 - pos, "  Dedup drops: %lu\n\n", (unsigned long)m.v2DedupDrops);
  
  // Current configuration
  int peerCount = getMeshPeerCount();
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
    gSettings.espnowmesh = false;
    if (gEspNow) {
      gEspNow->mode = ESPNOW_MODE_DIRECT;  // Update runtime state immediately
    }
    (void)writeSettingsJson();
    saveMeshPeers();
    BROADCAST_PRINTF("[ESP-NOW] mode set to %s", getEspNowModeString());
    return "ESP-NOW mode set to direct";
  } else if (args == "mesh") {
    gSettings.espnowmesh = true;
    if (gEspNow) {
      gEspNow->mode = ESPNOW_MODE_MESH;  // Update runtime state immediately
    }
    (void)writeSettingsJson();
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
  
  gSettings.espnowDeviceName = args;
  gSettings.espnowFirstTimeSetup = true;
  (void)writeSettingsJson();
  
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
    
    saveEspNowDevices();
  }
  
  snprintf(getDebugBuffer(), 1024, "Device name set to: %s", args.c_str());
  return getDebugBuffer();
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
    gSettings.meshHeartbeatBroadcast = true;
    (void)writeSettingsJson();
    return "Heartbeat mode set to public (broadcast). Unpaired devices can now be discovered.";
  } else if (args == "private" || args == "unicast") {
    gSettings.meshHeartbeatBroadcast = false;
    (void)writeSettingsJson();
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
    if (gSettings.meshBackupMAC.length() > 0) {
      output += String("\nBackup MAC: ") + gSettings.meshBackupMAC;
    }
    snprintf(getDebugBuffer(), 1024, "%s", output.c_str());
    return getDebugBuffer();
  }
  
  args.toLowerCase();
  if (args == "worker") {
    gSettings.meshRole = MESH_ROLE_WORKER;
    writeSettingsJson();
    BROADCAST_PRINTF("[MESH] Role set to worker");
    return "Role set to worker";
  } else if (args == "master") {
    gSettings.meshRole = MESH_ROLE_MASTER;
    gSettings.meshMasterMAC = "";
    writeSettingsJson();
    BROADCAST_PRINTF("[MESH] Role set to master");
    return "Role set to master";
  } else if (args == "backup") {
    gSettings.meshRole = MESH_ROLE_BACKUP_MASTER;
    writeSettingsJson();
    BROADCAST_PRINTF("[MESH] Role set to backup master");
    return "Role set to backup master";
  }
  
  return "Usage: espnow meshrole [worker|master|backup]";
}

// V2 logging toggle
const char* cmd_espnow_v2log(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  String args = argsIn;
  args.trim();
  if (args.length() == 0) {
    snprintf(getDebugBuffer(), 1024, "v2log: %s", gV2LogEnabled ? "on" : "off");
    return getDebugBuffer();
  }
  args.toLowerCase();
  if (args == "on" || args == "1" || args == "true") {
    gV2LogEnabled = true;
    return "v2log enabled";
  }
  if (args == "off" || args == "0" || args == "false") {
    gV2LogEnabled = false;
    return "v2log disabled";
  }
  return "Usage: espnow v2log [on|off]";
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
  
  gSettings.meshMasterMAC = args;
  gSettings.meshMasterMAC.toUpperCase();
  writeSettingsJson();
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
  
  gSettings.meshBackupMAC = args;
  gSettings.meshBackupMAC.toUpperCase();
  writeSettingsJson();
  BROADCAST_PRINTF("[MESH] Backup MAC set to %s", gSettings.meshBackupMAC.c_str());
  return "OK";
}

// Mesh topology discovery command
const char* cmd_espnow_meshtopo(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!meshEnabled()) {
    return "Mesh mode not enabled. Use 'espnow mode mesh' first.";
  }
  
  if (gSettings.meshRole != MESH_ROLE_MASTER) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    int pos = 0;
    char* buf = getDebugBuffer();
    pos += snprintf(buf + pos, 1024 - pos, "ERROR: Only master node can build topology view.\n");
    pos += snprintf(buf + pos, 1024 - pos, "This device is a %s.\n\n", getMeshRoleString(gSettings.meshRole));
    pos += snprintf(buf + pos, 1024 - pos, "Direct peers:\n");
    
    for (int i = 0; i < MESH_PEER_MAX; i++) {
      if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac)) {
        String peerName = getEspNowDeviceName(gMeshPeers[i].mac);
        String peerMac = macToHexString(gMeshPeers[i].mac);
        if (peerName.length() == 0) peerName = peerMac;
        pos += snprintf(buf + pos, 1024 - pos, "  - %s (%s)\n", peerName.c_str(), peerMac.c_str());
      }
    }
    return getDebugBuffer();
  }
  
  int peerCount = 0;
  for (int i = 0; i < MESH_PEER_MAX; i++) {
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
  
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String myMacCompact = macToHexStringCompact(myMac);
  
  String timeSyncMsg = buildTimeSyncMessage(gMeshMsgCounter++, myMacCompact.c_str());
  
  DEBUG_ESPNOWF("[TIME_SYNC] Broadcasting time sync: epoch=%lu", (unsigned long)epoch);
  meshSendEnvelopeToPeers(timeSyncMsg);
  
  BROADCAST_PRINTF("Time sync broadcast sent (epoch: %lu)", (unsigned long)epoch);
  return "OK";
}

// Time status command
const char* cmd_espnow_timestatus(const String& originalCmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "ERROR";
  
  if (gTimeIsSynced) {
    uint32_t epoch = getEpochTime();
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
  
  if (collectionActive && withinCollectionWindow) {
    uint32_t timeRemaining = TOPO_COLLECTION_WINDOW_MS - (now - gTopoLastResponseTime);
    BROADCAST_PRINTF("Collection in progress... waiting %lums for more responses", timeRemaining);
    BROADCAST_PRINTF("   Received %d response(s) so far", gTopoResponsesReceived);
    return "WAIT";
  }
  
  if (gTopoResultsBuffer.length() == 0) {
    broadcastOutput("No topology results available. Run 'espnow meshtopo' first.");
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
  
  snprintf(p, remaining, "=======================================\n\nℹ️  Chain Interpretation:\n  Devices with mutual peer connections form a chain.\n  Example: If A lists B as peer, and B lists A and C,\n  then the chain is: A ↔ B ↔ C\n");
  
  broadcastOutput("\n=== Mesh Topology Discovery Results ===");
  BROADCAST_PRINTF("Responses received: %d", gTopoResponsesReceived);
  BROADCAST_PRINTF("Request ID: %lu", (unsigned long)gTopoRequestId);
  broadcastOutput("");
  broadcastOutput(gTopoResultsBuffer.c_str());
  broadcastOutput("=======================================");
  broadcastOutput("");
  broadcastOutput("ℹ️  Chain Interpretation:");
  broadcastOutput("  Devices with mutual peer connections form a chain.");
  broadcastOutput("  Example: If A lists B as peer, and B lists A and C,");
  broadcastOutput("  then the chain is: A ↔ B ↔ C");
  
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
                      macToHexString(gTopoStreams[i].senderMac).c_str());
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
    broadcastOutput("⚠️  Stale lock detected (>30s), auto-releasing...");
    gFileTransferLocked = false;
    memset(gFileTransferOwnerMac, 0, 6);
  }
  
  BROADCAST_PRINTF("Lock status: %s", gFileTransferLocked ? "LOCKED" : "FREE");
  
  if (gFileTransferLocked) {
    BROADCAST_PRINTF("Lock owner: %s", macToHexString(gFileTransferOwnerMac).c_str());
    BROADCAST_PRINTF("Lock age: %lums", millis() - gFileTransferLockTime);
  }
  
  if (!gFileTransferLocked) {
    broadcastOutput("\nAcquiring lock...");
    gFileTransferLocked = true;
    memcpy(gFileTransferOwnerMac, testMac, 6);
    gFileTransferLockTime = millis();
    BROADCAST_PRINTF("✓ Lock acquired by: %s", macToHexString(gFileTransferOwnerMac).c_str());
  } else {
    broadcastOutput("\nLock already held, releasing...");
    gFileTransferLocked = false;
    memset(gFileTransferOwnerMac, 0, 6);
    broadcastOutput("✓ Lock released");
  }
  
  broadcastOutput("\n=== File Lock Test Complete ===");
  broadcastOutput("ℹ️  Run again to toggle lock state");
  return "OK";
}

// ESP-NOW Device Management Commands
// ============================================================================

// List paired devices
const char* cmd_espnow_list(const String& cmd) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "ESP-NOW not initialized. Run 'espnow init' first.";
  }

  unsigned long startMs = millis();
  int pos = 0;
  esp_now_peer_info_t peer;
  int count = 0;

  pos += snprintf(gEspNow->listBuffer + pos, sizeof(gEspNow->listBuffer) - pos, "Paired ESP-NOW Devices:\n");

  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);

  esp_err_t ret = esp_now_fetch_peer(true, &peer);
  while (ret == ESP_OK && pos < 900) {
    if (memcmp(peer.peer_addr, myMac, 6) == 0) {
      ret = esp_now_fetch_peer(false, &peer);
      continue;
    }
    
    String macStr = formatMacAddress(peer.peer_addr);
    String deviceName = getEspNowDeviceName(peer.peer_addr);

    bool isEncrypted = false;
    for (int i = 0; i < gEspNow->deviceCount; i++) {
      if (memcmp(gEspNow->devices[i].mac, peer.peer_addr, 6) == 0) {
        isEncrypted = gEspNow->devices[i].encrypted;
        break;
      }
    }

    const char* encStatus = isEncrypted ? " [ENCRYPTED]" : " [UNENCRYPTED]";

    if (deviceName.length() > 0) {
      pos += snprintf(gEspNow->listBuffer + pos, sizeof(gEspNow->listBuffer) - pos, "  %s (%s) Channel: %d%s\n",
                       deviceName.c_str(), macStr.c_str(), peer.channel, encStatus);
    } else {
      pos += snprintf(gEspNow->listBuffer + pos, sizeof(gEspNow->listBuffer) - pos, "  %s (Channel: %d)%s\n",
                       macStr.c_str(), peer.channel, encStatus);
    }
    count++;

    if (count % 4 == 0) {
      yield();
    }

    ret = esp_now_fetch_peer(false, &peer);
  }

  if (count == 0) {
    pos += snprintf(gEspNow->listBuffer + pos, sizeof(gEspNow->listBuffer) - pos, "  No devices paired\n");
  } else {
    pos += snprintf(gEspNow->listBuffer + pos, sizeof(gEspNow->listBuffer) - pos, "Total: %d device(s)", count);
  }

  unsigned long elapsedMs = millis() - startMs;
  DEBUGF(DEBUG_HTTP, "[ESPNOW_TIMING] list: %d devices enumerated in %lums", count, elapsedMs);

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

  for (int i = 0; i < MESH_PEER_MAX; i++) {
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
    return "ESP-NOW not initialized. Run 'espnow init' first.";
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
    for (int i = 0; i < MESH_PEER_MAX; i++) {
      if (gMeshPeers[i].isActive && macEqual6(gMeshPeers[i].mac, mac)) {
        gMeshPeers[i].isActive = false;
        DEBUG_ESPNOWF("[MESH] Removed peer from mesh list: %s", macToHexString(mac).c_str());
        break;
      }
    }
    saveMeshPeers();
  }

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
    return "ESP-NOW not initialized. Run 'espnow init' first.";
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
    // Plain text - wrap in v2 JSON TEXT envelope (use compact MAC format)
    uint8_t myMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, myMac);
    String srcMac = macToHexStringCompact(myMac);
    payload = buildTextMessage(srcMac.c_str(), "broadcast", message);
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[Router] Built v2 JSON TEXT broadcast message");
  }

  esp_now_peer_info_t peer;
  int sent = 0;
  int failed = 0;

  esp_err_t ret = esp_now_fetch_peer(true, &peer);
  while (ret == ESP_OK) {
    Message msg;
    memcpy(msg.dstMac, peer.peer_addr, 6);
    msg.payload = payload;
    
    if (routerSend(msg)) {
      sent++;
    } else {
      failed++;
    }

    ret = esp_now_fetch_peer(false, &peer);
  }

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

// Helper function to send a file to a specific MAC address
// Used by FILE_BROWSE fetch and other internal functions
bool sendFileToMac(const uint8_t* mac, const String& localPath) {
  if (!gEspNow || !gEspNow->initialized) {
    return false;
  }

  {
    FsLockGuard guard("espnow.send_file.exists");
    if (!LittleFS.exists(localPath)) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[sendFileToMac] File not found: %s", localPath.c_str());
    return false;
    }
  }

  FsLockGuard fsGuard("espnow.send_file.open");
  File file = LittleFS.open(localPath, "r");
  if (!file) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[sendFileToMac] Cannot open file: %s", localPath.c_str());
    return false;
  }
  
  uint32_t fileSize = file.size();
  uint32_t maxFileSize = MAX_FILE_CHUNKS * FILE_CHUNK_DATA_BYTES;
  if (fileSize > maxFileSize) {
    file.close();
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[sendFileToMac] File too large: %lu bytes (max %lu)", 
           (unsigned long)fileSize, (unsigned long)maxFileSize);
    return false;
  }
  
  String filename = localPath;
  int lastSlash = localPath.lastIndexOf('/');
  if (lastSlash >= 0) {
    filename = localPath.substring(lastSlash + 1);
  }
  
  int totalChunks = (fileSize + FILE_CHUNK_DATA_BYTES - 1) / FILE_CHUNK_DATA_BYTES;
  if (totalChunks > (int)MAX_FILE_CHUNKS) totalChunks = MAX_FILE_CHUNKS;
  
  String hash = String(millis() % 10000);
  
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String srcMac = macToHexStringCompact(myMac);
  
  // Send FILE_START
  String startMsg = buildFileStartMessage(srcMac.c_str(), filename.c_str(), fileSize, totalChunks, hash);
  {
    Message msg;
    memcpy(msg.dstMac, mac, 6);
    msg.payload = startMsg;
    if (!routerSend(msg)) {
      file.close();
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[sendFileToMac] Failed to send FILE_START");
      return false;
    }
  }
  
  // Send chunks
  uint8_t chunkBuf[FILE_CHUNK_DATA_BYTES];
  int chunkIdx = 0;
  while (file.available() && chunkIdx < totalChunks) {
    int bytesRead = file.read(chunkBuf, FILE_CHUNK_DATA_BYTES);
    if (bytesRead <= 0) break;
    
    // Base64-encode the chunk data
    String b64 = base64Encode(chunkBuf, bytesRead);
    String chunkMsg = buildFileChunkMessage(srcMac.c_str(), chunkIdx + 1, hash, b64);
    Message msg;
    memcpy(msg.dstMac, mac, 6);
    msg.payload = chunkMsg;
    routerSend(msg);
    
    chunkIdx++;
    vTaskDelay(pdMS_TO_TICKS(20));  // Pace chunks
  }
  file.close();
  
  // Send FILE_END
  String endMsg = buildFileEndMessage(srcMac.c_str(), hash.c_str());
  {
    Message msg;
    memcpy(msg.dstMac, mac, 6);
    msg.payload = endMsg;
    routerSend(msg);
  }
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[sendFileToMac] Sent %s (%d chunks) to %s", 
         filename.c_str(), chunkIdx, formatMacAddress(mac).c_str());
  return true;
}

// Sendfile command
const char* cmd_espnow_sendfile(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "ESP-NOW not initialized. Run 'espnow init' first.";
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

  File file = LittleFS.open(filepath, "r");
  if (!file) {
    snprintf(sendfileBuffer, sizeof(sendfileBuffer), "Error: Cannot open file: %s", filepath.c_str());
    return sendfileBuffer;
  }

  uint32_t fileSize = file.size();

  uint32_t maxFileSize = MAX_FILE_CHUNKS * FILE_CHUNK_DATA_BYTES;
  if (fileSize > maxFileSize) {
    file.close();
    snprintf(sendfileBuffer, sizeof(sendfileBuffer), "Error: File too large (max %luKB). File size: %lu bytes",
             (unsigned long)(maxFileSize / 1024), (unsigned long)fileSize);
    return sendfileBuffer;
  }

  String filename = filepath;
  int lastSlash = filepath.lastIndexOf('/');
  if (lastSlash >= 0) {
    filename = filepath.substring(lastSlash + 1);
  }

  broadcastOutput("[ESP-NOW] Sending file to " + (deviceName.length() > 0 ? deviceName : formatMacAddress(mac)) + ": " + filename + " (" + String(fileSize) + " bytes)");

  int totalChunks = (fileSize + FILE_CHUNK_DATA_BYTES - 1) / FILE_CHUNK_DATA_BYTES;
  if (totalChunks > (int)MAX_FILE_CHUNKS) totalChunks = MAX_FILE_CHUNKS;

  String hash = String(millis() % 10000);

  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String srcMac = macToHexStringCompact(myMac);

  String startMsg = buildFileStartMessage(srcMac.c_str(), filename.c_str(), fileSize, totalChunks, hash);
  {
    Message msg;
    memcpy(msg.dstMac, mac, 6);
    msg.payload = startMsg;
    if (!routerSend(msg)) {
      file.close();
      snprintf(sendfileBuffer, sizeof(sendfileBuffer), "Error: Failed to send FILE_START");
      return sendfileBuffer;
    }
  }
  delay(150);

  memset((void*)gEspNow->fileAckHashExpected, 0, sizeof(gEspNow->fileAckHashExpected));
  strncpy(gEspNow->fileAckHashExpected, hash.c_str(), 15);
  gEspNow->fileAckLast = 0;

  static uint8_t s_fileBuf[FILE_CHUNK_DATA_BYTES];
  int sentChunks = 0;
  int consecutiveFailures = 0;
  for (int i = 0; i < totalChunks; i++) {
    size_t toRead = min((uint32_t)FILE_CHUNK_DATA_BYTES, (uint32_t)(fileSize - (i * FILE_CHUNK_DATA_BYTES)));
    size_t actuallyRead = file.read(s_fileBuf, toRead);
    if (actuallyRead == 0) break;
    String b64 = base64Encode(s_fileBuf, actuallyRead);
    String chunkMsg = buildFileChunkMessage(srcMac.c_str(), i + 1, hash, b64);
    bool sentOk = false;
    for (int attempt = 0; attempt < 3 && !sentOk; attempt++) {
      Message msg;
      memcpy(msg.dstMac, mac, 6);
      msg.payload = chunkMsg;
      sentOk = routerSend(msg);
      if (!sentOk) {
        delay(20 * (attempt + 1));
        yield();
      }
    }
    if (!sentOk) {
      consecutiveFailures++;
    } else {
      const unsigned long ackTimeoutMs = 400;
      int ackAttempts = 0;
      while (gEspNow->fileAckLast < (uint16_t)(i + 1) && ackAttempts < 3) {
        unsigned long tA = millis();
        while ((millis() - tA) < ackTimeoutMs) {
          if (gEspNow->fileAckLast >= (uint16_t)(i + 1)) break;
          delay(5);
          yield();
        }
        if (gEspNow->fileAckLast >= (uint16_t)(i + 1)) break;
        Message msg;
        memcpy(msg.dstMac, mac, 6);
        msg.payload = chunkMsg;
        (void)routerSend(msg);
        ackAttempts++;
        delay(20 * ackAttempts);
        yield();
      }
      if (gEspNow->fileAckLast >= (uint16_t)(i + 1)) {
        sentChunks++;
        consecutiveFailures = 0;
      } else {
        consecutiveFailures++;
      }
    }
    int baseDelay = 2 + min(consecutiveFailures * 8, 100);
    delay(baseDelay);
    if (((i + 1) % 50) == 0) delay(150 + min(consecutiveFailures * 20, 200));
    yield();
  }
  file.close();

  String endMsg = buildFileEndMessage(srcMac.c_str(), hash);
  {
    Message msg; memcpy(msg.dstMac, mac, 6); msg.payload = endMsg; (void)routerSend(msg);
  }

  {
    unsigned long tEnd = millis();
    while ((millis() - tEnd) < 1000 && gEspNow->fileAckLast < (uint16_t)totalChunks) {
      delay(10);
      yield();
    }
  }

  if (sentChunks <= 0) {
    return "Error: Failed to send file";
  }
  gEspNow->fileTransfersSent++;
  
  // Log file transfer success to message buffer
  logFileTransferEvent(mac, deviceName.c_str(), filename.c_str(), MSG_FILE_SEND_SUCCESS);
  
  snprintf(sendfileBuffer, sizeof(sendfileBuffer), "File sent successfully: %s (%lu bytes, %d chunks)",
           filename.c_str(), (unsigned long)fileSize, sentChunks);
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
    return "ESP-NOW not initialized. Run 'espnow init' first.";
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
    return "ESP-NOW not initialized. Run 'espnow init' first.";
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
    return "ESP-NOW not initialized. Run 'espnow init' first.";
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
  saveEspNowDevices();

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  snprintf(getDebugBuffer(), 1024,
           "Encrypted device paired successfully: %s (%s)\nKey fingerprint: %02X%02X%02X%02X...",
           deviceName.c_str(), macStr.c_str(),
           gEspNow->derivedKey[0], gEspNow->derivedKey[1], gEspNow->derivedKey[2], gEspNow->derivedKey[3]);
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
    return "ESP-NOW not initialized. Run 'espnow init' first.";
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

  // Build FILE_BROWSE message
  JsonDocument doc;
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String srcMac = macToHexStringCompact(myMac);
  String dstMac = macToHexStringCompact(targetMac);
  
  v2_init_envelope(doc, MSG_TYPE_FILE_BROWSE, generateMessageId(),
                   gSettings.espnowDeviceName.c_str(), "", -1);
  JsonObject pld = doc["pld"].to<JsonObject>();
  pld["kind"] = "list";
  pld["path"] = path;
  pld["user"] = username;
  pld["pass"] = password;
  
  String browseMessage;
  serializeJson(doc, browseMessage);

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

  Message msg;
  memcpy(msg.dstMac, targetMac, 6);
  msg.payload = browseMessage;
  
  static char browseBuffer[256];
  if (!routerSend(msg)) {
    snprintf(browseBuffer, sizeof(browseBuffer), "Failed to send browse request");
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
    return "ESP-NOW not initialized. Run 'espnow init' first.";
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

  // Build FILE_BROWSE fetch message
  JsonDocument doc;
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  
  v2_init_envelope(doc, MSG_TYPE_FILE_BROWSE, generateMessageId(),
                   gSettings.espnowDeviceName.c_str(), "", -1);
  JsonObject pld = doc["pld"].to<JsonObject>();
  pld["kind"] = "fetch";
  pld["path"] = path;
  pld["user"] = username;
  pld["pass"] = password;
  
  String fetchMessage;
  serializeJson(doc, fetchMessage);

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

  Message msg;
  memcpy(msg.dstMac, targetMac, 6);
  msg.payload = fetchMessage;
  
  static char fetchBuffer[256];
  if (!routerSend(msg)) {
    snprintf(fetchBuffer, sizeof(fetchBuffer), "Failed to send fetch request");
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
    return "ESP-NOW not initialized. Run 'espnow init' first.";
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

  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String srcMac = macToHexStringCompact(myMac);
  String dstMac = macToHexStringCompact(targetMac);

  String remoteMessage = buildCommandMessage(srcMac.c_str(), dstMac.c_str(), username.c_str(), password.c_str(), command.c_str());

  if (isMeshMode()) {
    if (!isPairedDevice(targetMac)) {
      BROADCAST_PRINTF("[ESP-NOW][mesh] remote send rejected: not paired MAC=%s", formatMacAddress(targetMac).c_str());
      return "Rejected (mesh): device not paired. Use 'espnow pair' first.";
    }
    if (!espnowPeerExists(targetMac)) {
      BROADCAST_PRINTF("[ESP-NOW][mesh] remote send rejected: no peer entry MAC=%s", formatMacAddress(targetMac).c_str());
      return "Rejected (mesh): destination not in ESP-NOW peer table.";
    }
    BROADCAST_PRINTF("[ESP-NOW][mesh] remote send accepted MAC=%s", formatMacAddress(targetMac).c_str());
  }

  Message msg;
  memcpy(msg.dstMac, targetMac, 6);
  msg.payload = remoteMessage;
  
  static char remoteBuffer[256];
  if (!routerSend(msg)) {
    snprintf(remoteBuffer, sizeof(remoteBuffer), "Failed to send remote command");
    return remoteBuffer;
  }

  snprintf(remoteBuffer, sizeof(remoteBuffer), "Remote command sent to %s: %s",
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
    return "ESP-NOW not initialized. Run 'espnow init' first.";
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

  // Build v2 JSON TEXT message for plain text
  String payload;
  if (message.startsWith("{")) {
    // Already JSON, send as-is
    payload = message;
  } else {
    // Plain text - wrap in v2 JSON TEXT envelope (use compact MAC format)
    uint8_t myMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, myMac);
    String srcMac = macToHexStringCompact(myMac);
    String dstMac = macToHexStringCompact(mac);
    payload = buildTextMessage(srcMac.c_str(), dstMac.c_str(), message);
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[Router] Built v2 JSON TEXT message");
  }
  
  // Create message and send via router
  Message msg;
  memcpy(msg.dstMac, mac, 6);
  msg.payload = payload;
  
  // Reset ACK flag before sending
  if (gEspNow) gEspNow->lastAckReceived = false;
  
  bool success = routerSend(msg);
  
  if (success) {
    if (!ensureDebugBuffer()) return "Message sent";
    
    // Check if ACK was received (for web UI status)
    bool gotAck = gEspNow && gEspNow->lastAckReceived;
    
    if (gotAck) {
      snprintf(getDebugBuffer(), 1024, "Message sent with ACK (ID: %lu, %s routing)", 
               (unsigned long)msg.msgId, 
               shouldUseMesh(mac) ? "mesh" : "direct");
    } else {
      snprintf(getDebugBuffer(), 1024, "Message sent (ID: %lu, %s routing)", 
               (unsigned long)msg.msgId, 
               shouldUseMesh(mac) ? "mesh" : "direct");
    }
    return getDebugBuffer();
  } else {
    return "Failed to send message";
  }
}

// Send a synthetic large text payload to trigger fragmentation paths
const char* cmd_espnow_bigsend(const String& argsIn) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) return "ESP-NOW not initialized. Run 'espnow init' first.";

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
  String srcMac = macToHexStringCompact(myMac);
  String dstMac = macToHexStringCompact(mac);
  String payload = buildTextMessage(srcMac.c_str(), dstMac.c_str(), textContent);

  Message msg;
  memcpy(msg.dstMac, mac, 6);
  msg.payload = payload;
  bool ok = routerSend(msg);
  if (!ensureDebugBuffer()) return ok ? "OK" : "Failed";
  snprintf(getDebugBuffer(), 1024, "bigsend: %s (id=%lu, bytes=%ld)", ok ? "OK" : "FAILED", (unsigned long)msg.msgId, size);
  return getDebugBuffer();
}

// ============================================================================
// ESP-NOW Command Registry
// ============================================================================

// CommandEntry struct is defined in system_utils.h (included via espnow_system.h)

extern const char* cmd_espnow_sensorstream(const String& cmd);
extern const char* cmd_espnow_sensorstatus(const String& cmd);
extern const char* cmd_espnow_sensorbroadcast(const String& cmd);
extern const char* cmd_espnow_usersync(const String& cmd);

extern const CommandEntry espNowCommands[] = {
  // ---- ESP-NOW Status & Statistics ----
  { "espnow status", "Show ESP-NOW status and configuration.", false, cmd_espnow_status },
  { "espnow stats", "Show ESP-NOW statistics (messages, errors, etc.).", false, cmd_espnow_stats },
  { "espnow routerstats", "Show message router statistics and metrics.", false, cmd_espnow_routerstats },
  { "espnow resetstats", "Reset ESP-NOW statistics counters.", true, cmd_espnow_resetstats },
  
  // ---- ESP-NOW Initialization & Pairing ----
  { "espnow init", "Initialize ESP-NOW communication.", true, cmd_espnow_init },
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
  { "espnow meshtopo", "Discover mesh topology (master only).", false, cmd_espnow_meshtopo },
  { "espnow toporesults", "Get topology discovery results.", false, cmd_espnow_toporesults },
  { "espnow timesync", "Broadcast NTP time to mesh (master only).", false, cmd_espnow_timesync },
  { "espnow timestatus", "Show time synchronization status.", false, cmd_espnow_timestatus },
  { "espnow meshsave", "Manually save mesh peer topology to filesystem.", false, cmd_espnow_meshsave },
  
  // ---- ESP-NOW Communication ----
  { "espnow send", "Send message (auto-routes via mesh if enabled): 'espnow send <name_or_mac> <message>'.", false, cmd_espnow_send, "Usage: espnow send <name_or_mac> <message>" },
  { "espnow broadcast", "Broadcast message: 'espnow broadcast <message>'.", false, cmd_espnow_broadcast, "Usage: espnow broadcast <message>" },
  { "espnow sendfile", "Send file: 'espnow sendfile <name_or_mac> <filepath>'.", false, cmd_espnow_sendfile, "Usage: espnow sendfile <name_or_mac> <filepath>" },
  { "espnow browse", "Browse remote files: 'espnow browse <name_or_mac> <user> <pass> [path]'.", false, cmd_espnow_browse, "Usage: espnow browse <target> <username> <password> [path]" },
  { "espnow fetch", "Fetch remote file: 'espnow fetch <name_or_mac> <user> <pass> <path>'.", false, cmd_espnow_fetch, "Usage: espnow fetch <target> <username> <password> <path>" },
  { "espnow remote", "Execute remote command: 'espnow remote <name_or_mac> <user> <pass> <cmd>'.", false, cmd_espnow_remote, "Usage: espnow remote <target> <username> <password> <command>" },
  { "startstream", "Start streaming all output to ESP-NOW caller (admin, remote only).", true, cmd_espnow_startstream },
  { "stopstream", "Stop streaming output to ESP-NOW device (admin).", true, cmd_espnow_stopstream },
  { "espnow worker", "Configure worker status reporting: 'espnow worker [show|on|off|interval <ms>|fields <list>]'.", false, cmd_espnow_worker, "Usage: espnow worker [show|on|off|interval <ms>|fields <heap,rssi,thermal,imu>]" },
  { "espnow sensorstream", "Enable/disable sensor data streaming to master (worker only): 'espnow sensorstream <sensor> <on|off>'.", false, cmd_espnow_sensorstream },
  { "espnow sensorstatus", "Show remote sensor cache (master) or worker streaming status (worker).", false, cmd_espnow_sensorstatus },
  { "espnow sensorbroadcast", "Enable/disable all sensor ESP-NOW communication: 'espnow sensorbroadcast <on|off>'.", false, cmd_espnow_sensorbroadcast },
  { "espnow v2log", "Enable/disable v2 RX decode logging: 'espnow v2log [on|off]'.", false, cmd_espnow_v2log, "Usage: espnow v2log [on|off]" },
  { "espnow rel", "Show v2 reliability status (ack/dedup mandatory).", false, cmd_espnow_rel },
  { "espnow bigsend", "Send a synthetic large TEXT payload to test frag: 'espnow bigsend <name_or_mac> <bytes>'.", false, cmd_espnow_bigsend, "Usage: espnow bigsend <name_or_mac> <bytes>" },
  { "espnow usersync", "Enable/disable user credential sync: 'espnow usersync [on|off]'.", false, cmd_espnow_usersync },
  
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
  { "firstTimeSetup",             SETTING_BOOL,   &gSettings.espnowFirstTimeSetup,       false, 0, nullptr, 0, 1, "First Time Setup", nullptr },
  { "passphrase",                  SETTING_STRING, &gSettings.espnowPassphrase,           0, 0, "", 0, 0, "Passphrase", nullptr },
  { "meshRole",                   SETTING_INT,    &gSettings.meshRole,                   0, 0, nullptr, 0, 2, "Mesh Role", nullptr },
  { "masterMAC",                  SETTING_STRING, &gSettings.meshMasterMAC,              0, 0, "", 0, 0, "Master MAC", nullptr },
  { "backupMAC",                  SETTING_STRING, &gSettings.meshBackupMAC,              0, 0, "", 0, 0, "Backup MAC", nullptr },
  { "masterHeartbeatInterval",    SETTING_INT,    &gSettings.meshMasterHeartbeatInterval,10000, 0, nullptr, 1000, 60000, "Heartbeat Interval (ms)", nullptr },
  { "failoverTimeout",            SETTING_INT,    &gSettings.meshFailoverTimeout,        20000, 0, nullptr, 5000, 120000, "Failover Timeout (ms)", nullptr },
  { "workerStatusInterval",       SETTING_INT,    &gSettings.meshWorkerStatusInterval,   30000, 0, nullptr, 5000, 120000, "Worker Status Interval (ms)", nullptr },
  { "topoDiscoveryInterval",      SETTING_INT,    &gSettings.meshTopoDiscoveryInterval,  0, 0, nullptr, 0, 300000, "Topo Discovery Interval (ms)", nullptr },
  { "topoAutoRefresh",            SETTING_BOOL,   &gSettings.meshTopoAutoRefresh,        false, 0, nullptr, 0, 1, "Auto Refresh Topology", nullptr },
  { "heartbeatBroadcast",         SETTING_BOOL,   &gSettings.meshHeartbeatBroadcast,     false, 0, nullptr, 0, 1, "Heartbeat Broadcast", nullptr },
  { "meshTTL",                    SETTING_INT,    &gSettings.meshTTL,                    3, 0, nullptr, 1, 10, "TTL", nullptr },
  { "meshAdaptiveTTL",            SETTING_BOOL,   &gSettings.meshAdaptiveTTL,            false, 0, nullptr, 0, 1, "Adaptive TTL", nullptr }
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
    gSettings.espnowUserSyncEnabled = true;
    writeSettingsJson();
    INFO_ESPNOWF("[USER_SYNC] User sync ENABLED");
    return "User sync ENABLED - admins can now sync users across devices";
  } else if (args == "off" || args == "0" || args == "false" || args == "disable") {
    gSettings.espnowUserSyncEnabled = false;
    writeSettingsJson();
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
  if (!gEspNow) return nullptr;
  
  // First, try to find existing history for this peer
  for (int i = 0; i < MESH_PEER_MAX; i++) {
    PeerMessageHistory& history = gEspNow->peerMessageHistories[i];
    if (history.active && memcmp(history.peerMac, peerMac, 6) == 0) {
      return &history;
    }
  }
  
  // Not found, create new entry
  for (int i = 0; i < MESH_PEER_MAX; i++) {
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
  size_t copyLen = msgLen < 127 ? msgLen : 127;
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
  broadcastOutput("[ESP-NOW] " + deviceName + ": " + String(message));
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
  if (!gEspNow || !outMessages) return 0;
  
  int copied = 0;
  
  // Collect messages from all peer histories
  for (int p = 0; p < MESH_PEER_MAX && copied < maxMessages; p++) {
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
