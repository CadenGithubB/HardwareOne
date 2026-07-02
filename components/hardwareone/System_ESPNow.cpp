/**
 * ESP-NOW System - COMPLETE Implementation
 * Extracted from HardwareOne.ino
 */

#include "System_BuildConfig.h"
#include <esp_attr.h>

#if ENABLE_ESPNOW

#include <time.h>
#include <new>                       // placement new — construct EspNowState's C++ members
#include <ArduinoJson.h>
#include <esp_wifi.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_crc.h>                 // esp_crc32_le for bond settings hash
#include <LittleFS.h>
#include <Preferences.h>
#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

#include "OLED_Display.h"
#include "OLED_UI.h"
#include "System_AuthIdentity.h"   // currentAuthContext (CLI handlers + ESP-NOW frame ctx)
#include "System_ESPNow_Saturation.h"  // espnowSaturationTick / espnowSaturationNoteAckRtt
#include "System_Command.h"
#include "System_Notifications.h"
#include "System_Debug.h"
#include "System_ESPNow.h"
#include "System_ESPNow_Wire.h"      // V3 wire schema (Phase 0 extraction)
#include "System_ESPNow_Crypto.h"    // Phase 3.0: libsodium init / RNG / Ed25519 keygen
#include "System_ESPNow_Files.h"    // Phase 4: concurrent file-transfer slot table
#include <sodium.h>                 // Phase 3.5 task #32: sodium_memcmp (constant-time)
#include "System_ESPNow_Handlers_Crypto.h"  // Phase 3.3: KEY_EX handlers + initiator
#include "System_ESPNow_Identity.h"  // Phase 3.0: long-term Ed25519 identity load/store
#include "System_ESPNow_MeshKeys.h"  // Phase 3.1: per-mesh PBKDF2 hash + bootstrap/group key cache
#include "System_ESPNow_Sessions.h"  // Phase 3.4: per-peer-per-mesh session state
#include "System_ESPNow_Sensors.h"
#include "System_ESPNow_Tx.h"        // Single-sender TX dispatcher (Phase 1)
#include "System_MemUtil.h"          // ps_alloc — heap copy of payload for async send
#include "G2_Page_ESPNow.h"        // g2ESPNowAppOnRxText push-kick (inline no-op when BT/G2 off)
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
#include "System_VFS.h"         // For SD-capture routing (captureEspNowFrame)
#if ENABLE_HTTP_SERVER
#include "WebServer_Server.h"
#endif

#if ENABLE_IMU_SENSOR
#include "i2csensor_bno055.h"
#endif
#if ENABLE_THERMAL_SENSOR
#include "i2csensor_mlx90640.h"
#endif

// External dependencies from main .ino - now in espnow_system.h
extern bool isValidUser(const String& username, const String& password);
extern bool isAdminUser(const String& username);
extern void printToSerial(const String& s);
extern volatile uint32_t gOutputFlags;
extern bool gCLIValidateOnly;

// Forward declaration — definition is further down near onEspNowRawRecv. Used
// by the send paths (v4_send_frame, fragmented tx) defined above that point.
static void captureEspNowFrame(const char* direction, const uint8_t* peerMac,
                               int rssi, const uint8_t* data, int len);
// gDebugBuffer, gDebugFlags, ensureDebugBuffer now from debug_system.h

// Debug flags (defined in .ino)
// Removed local DEBUG_HTTP define; use debug_system.h as single source of truth

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
// ESP-NOW V3 Binary Protocol — wire schema moved to System_ESPNow_Wire.h.
// Constants, EspNowV4Type, EspNowV4Flags, EspNowV4Header, and payload structs
// all live there now. Phase 0 of docs/ESPNOW_V4_PLAN.md.
// ============================================================================

// ============================================================================
// V4 FRAGMENTATION CONSTANTS AND REASSEMBLY
// ============================================================================
// Per-fragment plaintext budget. Phase 3.5 task #51 (encrypted fragmentation)
// is shipped: v4_send_encrypted_chunked uses this same 200-byte fragment
// size and adds the 16-byte AEAD tag per fragment, giving 216 wire bytes per
// fragment (still ≤ MAX_PAYLOAD = 218). Plaintext and encrypted fragments
// interleave through the same reassembly buffer pool — the receiver decides
// per frame whether the inner is wrapped (type=32 SESSION_FRAME) or raw.
#define V4_MAX_FRAGMENT_PAYLOAD 200   // Max payload bytes per fragment
#define V4_FRAG_MAX             32    // Max fragments per message (max msg = 6400 bytes)
                                       // Cost: ~12.6 KB PSRAM (2 reassembly slots × 32 × 200 B + overhead).
                                       // DRAM impact: zero — gV4Reasm is ps_alloc'd.
#define V4_REASM_MAX            2     // Max concurrent reassembly contexts
#define V4_REASM_TIMEOUT_MS     5000  // Reassembly context GC timeout (ms)
#define V4_FRAG_ACK_WAIT_MAX    8     // Max concurrent fragment ACK waiters
                                       // Not bumped with V4_FRAG_MAX — at 32 fragments,
                                       // the sender serializes into 4×8 batches (~80 ms vs
                                       // ~20 ms for a 6 KB message). Acceptable trade-off.

struct V4ReasmEntry {
  bool     active;
  uint8_t  src[6];
  uint32_t msgId;
  uint8_t  type;
  uint8_t  fragCount;
  uint8_t  received;
  bool     have[V4_FRAG_MAX];
  uint8_t  buffer[V4_FRAG_MAX * V4_MAX_FRAGMENT_PAYLOAD];
  uint16_t bufferSize;
  uint32_t lastUpdateMs;
};

static V4ReasmEntry* gV4Reasm = nullptr;  // Allocated in PSRAM at init (~6.4KB)

static void v4_reasm_reset(V4ReasmEntry& e) {
  e.active   = false;
  e.msgId    = 0;
  e.received = 0;
  e.fragCount = 0;
  memset(e.src,  0, 6);
  memset(e.have, 0, sizeof(e.have));
}

static void v4_reasm_gc(uint32_t nowMs) {
  if (!gV4Reasm) return;
  for (int i = 0; i < V4_REASM_MAX; i++) {
    if (gV4Reasm[i].active && (nowMs - gV4Reasm[i].lastUpdateMs) > V4_REASM_TIMEOUT_MS) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_REASM_GC] Evicting stale msgId=%lu from %02X:%02X:%02X:%02X:%02X:%02X",
             (unsigned long)gV4Reasm[i].msgId,
             gV4Reasm[i].src[0], gV4Reasm[i].src[1], gV4Reasm[i].src[2],
             gV4Reasm[i].src[3], gV4Reasm[i].src[4], gV4Reasm[i].src[5]);
      if (gEspNow) { gEspNow->routerMetrics.v4FragRxGc++; }
      v4_reasm_reset(gV4Reasm[i]);
    }
  }
}

static V4ReasmEntry* v4_reasm_find_or_alloc(const uint8_t* src, uint32_t msgId, uint8_t type, uint8_t fragCount) {
  if (!gV4Reasm) return nullptr;
  for (int i = 0; i < V4_REASM_MAX; i++) {
    if (gV4Reasm[i].active && gV4Reasm[i].msgId == msgId &&
        memcmp(gV4Reasm[i].src, src, 6) == 0) {
      return &gV4Reasm[i];
    }
  }
  for (int i = 0; i < V4_REASM_MAX; i++) {
    if (!gV4Reasm[i].active) {
      v4_reasm_reset(gV4Reasm[i]);
      memcpy(gV4Reasm[i].src, src, 6);
      gV4Reasm[i].msgId       = msgId;
      gV4Reasm[i].type        = type;
      gV4Reasm[i].fragCount   = (fragCount <= V4_FRAG_MAX) ? fragCount : V4_FRAG_MAX;
      gV4Reasm[i].bufferSize  = (uint16_t)(gV4Reasm[i].fragCount * V4_MAX_FRAGMENT_PAYLOAD);
      gV4Reasm[i].lastUpdateMs = millis();
      gV4Reasm[i].active      = true;
      return &gV4Reasm[i];
    }
  }
  return nullptr;
}

struct V4FragAckWait {
  bool     active;
  uint32_t msgId;
  uint8_t  fragIndex;
  uint8_t  dstMac[6];
  volatile bool acked;   // Set true by RX ACK handler; polled by TX task
  uint32_t sentMs;
};

static V4FragAckWait gV4FragAckWait[V4_FRAG_ACK_WAIT_MAX];

static V4FragAckWait* v4_frag_ack_alloc(const uint8_t* dst, uint32_t msgId, uint8_t fragIdx) {
  // Serialize the slot scan + claim against the RX ACK handler (which marks
  // acked under the same lock) and other fragmented senders. Fragmented sends
  // run off espnow_task (cmd_exec_task / SENSOR_BCAST_TASK), so this array is
  // genuinely multi-task. Brief scan only — never held across the ACK wait
  // (the caller polls ->acked WITHOUT the lock; see v4_send_*_chunked).
  EspNowTxGuard g("fragAckAlloc");
  for (int i = 0; i < V4_FRAG_ACK_WAIT_MAX; i++) {
    if (!gV4FragAckWait[i].active) {
      gV4FragAckWait[i].msgId     = msgId;
      gV4FragAckWait[i].fragIndex = fragIdx;
      gV4FragAckWait[i].acked     = false;
      gV4FragAckWait[i].sentMs    = 0;
      memcpy(gV4FragAckWait[i].dstMac, dst, 6);
      gV4FragAckWait[i].active    = true;
      return &gV4FragAckWait[i];
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

// Forward declaration for v4_send_frame (implemented later).
// Phase 3.5 task #32: flags widened uint8_t -> uint16_t to carry the
// high-byte flag bits (BROADCAST_AUTH=0x0100, SESSION_FRAME=0x0200,
// HANDSHAKE=0x0400). Pre-existing bug: these bits were silently truncated
// on every call — SESSION_OPEN / KEY_EX frames went out with HANDSHAKE
// bit zero. Fixing now since BROADCAST_AUTH would have the same fate.
bool v4_send_frame(const uint8_t* dst, uint8_t type, uint16_t flags, uint32_t msgId,
                   const uint8_t* payload, uint16_t payloadLen, uint8_t ttl);

// Forward declarations for V3 helper functions (non-static for external linkage)
bool v4_broadcast(uint8_t type, uint16_t flags, uint32_t msgId, const uint8_t* payload, uint16_t payloadLen, uint8_t ttl);
// Phase 5 — subscription-aware broadcast: skips peers whose subscribedEvents
// bitmap doesn't include `category`. See System_ESPNow_Identity.h for the
// EspNowEventCategory enum.
bool v4_broadcast_category(uint8_t type, uint16_t flags, uint32_t msgId,
                           const uint8_t* payload, uint16_t payloadLen, uint8_t ttl,
                           uint32_t category, int* outAttempted = nullptr);
// (v4_send_chunked removed 2026-05 — plaintext multi-frame is unreachable now
// that smart is strict encrypt-or-fail. RX-side plaintext reassembly remains
// for one release window to tolerate old-firmware peers, then can be removed.)
//
// Encrypted dispatcher — STRICT encrypt-or-fail. Single-frame fits → smart goes
// through v4_send_encrypted_or_queue (auto-handshake). Multi-frame requires an
// ACTIVE session — caller retries if not up.
bool v4_send_payload_smart(const uint8_t* dst, uint8_t type, uint16_t flags,
                           uint32_t msgId,
                           const uint8_t* payload, uint16_t payloadLen,
                           uint8_t ttl);
// Phase 3.5 task #51 — encrypted multi-frame send. Splits payload into
// fragments of ≤ESPNOW_V4_MAX_PLAINTEXT bytes, AEAD-seals each fragment
// independently under the peer's session key, sends with fragIndex/fragCount
// set in the outer header, ACK-waits per fragment with retry.
bool v4_send_encrypted_chunked(const uint8_t dst[6], uint8_t type, uint16_t baseFlags,
                               uint32_t msgId,
                               const uint8_t* payload, uint16_t payloadLen,
                               uint8_t ttl);
bool v4_broadcast_topo_request(uint32_t reqId);
bool v4_send_command_response(const uint8_t* dst, uint32_t cmdMsgId, bool success, const char* resultText, uint16_t textLen);
bool v4_send_text(const uint8_t* dst, const char* text, uint16_t textLen);
bool v4_broadcast_text(const char* text, uint16_t textLen);
bool v4_broadcast_sensor_status(RemoteSensorType sensorType, bool enabled);
bool v4_broadcast_sensor_data(RemoteSensorType sensorType, const char* jsonData, uint16_t jsonLen, int* outAttempted = nullptr);
bool v4_send_user_sync(const uint8_t* dst, const char* jsonPayload, uint16_t jsonLen);

// MAC address formatting (stack buffer version to reduce heap churn)
void formatMacAddressBuf(const uint8_t* mac, char* buf, size_t bufSize);

// Forward declaration for internal addEspNowPeerWithEncryption (different from extern version in .ino)
static bool addEspNowPeerWithEncryption(const uint8_t* mac, bool useEncryption, const uint8_t* encryptionKey);

// Command types from shared header
#include "System_CommandTypes.h"
// ConnectedDevice is now defined in System_I2C.h (included above)

// External globals for device tracking (defined in System_I2C.cpp)
extern ConnectedDevice connectedDevices[];
extern int connectedDeviceCount;

// Memory allocation helpers

// Note: FsLockGuard is now in mutex_system.h (included above)

// Settings struct is now in settings.h (included via espnow_system.h)

// File paths
static const char* ESPNOW_DEVICES_FILE = "/system/espnow/devices.json";
static const char* MESH_PEERS_FILE = "/system/espnow/mesh_peers.json";

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

// Note: gEspNow is defined in HardwareOne.ino as non-static (extern in header)

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
static bool gMetadataChanged = false;          // Whether metadata changed since last send
static String gLastSentFriendlyName = "";
static String gLastSentRoom = "";
static String gLastSentZone = "";
static String gLastSentTags = "";

// Master/Backup heartbeat tracking
static uint32_t gLastMasterHeartbeat = 0;
static uint32_t gLastBackupHeartbeat = 0;
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
int gMeshPeerSlots = 0;  // Runtime slot count — 0 until the peer arrays are allocated at
                         // init. Kept 0 (not the default max) so every `for (i <
                         // gMeshPeerSlots)` loop is a safe no-op while gMeshPeerMeta /
                         // gMeshPeers are still null (e.g. a CLI/BLE `espnowdevices`
                         // before `openespnow` — that was a null-deref crash at isActive
                         // offset 0xE0). Set to the real count only after both arrays exist.
MeshPeerHealth* gMeshPeers = nullptr;
MeshPeerMeta* gMeshPeerMeta = nullptr;
uint32_t gLastHeartbeatSentMs = 0;

static MeshRetryEntry gMeshRetryQueue[MESH_RETRY_QUEUE_SIZE];
// Note: gMeshRetryMutex is now defined in mutex_system.cpp
// Phase 4: per-transfer state lives in System_ESPNow_Files.{h,cpp}'s slot
// table. The old single-flight FileTransfer struct + gActiveFileTransfer
// pointer + gFileTransferLocked flag are gone.

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
EXT_RAM_BSS_ATTR static TopoDeviceEntry gTopoDeviceCache[MAX_TOPO_DEVICE_CACHE];
static BufferedPeerMessage gPeerBuffer[MAX_BUFFERED_PEERS];

// Phase 4: per-transfer state moved to System_ESPNow_Files (multi-slot).
// gFileTransferLocked / gFileTransferOwnerMac / gFileTransferLockTime
// were removed — the slot table is the new concurrency primitive.

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

// Save mesh peer MAC addresses to filesystem (topology only, not health metrics)
// This is called only when topology changes (new peer discovered, peer removed, mode change)
// Health metrics (timestamps, counters) rebuild naturally from heartbeats after reboot
bool saveMeshPeers() {
  if (!filesystemReady) return false;

  // Pause sensor polling during file I/O (RAII — resumes on every return path;
  // from System_PollPause.h via System_I2C.h).
  PollPauseGuard pollGuard;

  FsLockGuard fsGuard("mesh.peers.save");
  File file = VFS::openGuarded(MESH_PEERS_FILE, "w", VFS::systemAuth("espnow.mesh_peers_save"), true);
  if (!file) {
    return false;
  }
  int skipped = 0;

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
    char rawMacBuf[18];
    formatMacAddressBuf(gMeshPeers[i].mac, rawMacBuf, sizeof(rawMacBuf));
    String encMac = encryptString(String(rawMacBuf));
    if (encMac.length() == 0) {
      ERROR_ESPNOWF("[MESH] Failed to encrypt peer MAC, skipping entry");
      skipped++;
      continue;
    }
    file.print("    {\"mac\": \"");
    file.print(encMac);
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
  // pollGuard resumes sensor polling on return.

  DEBUGF(DEBUG_ESPNOW_MESH, "[MESH] Saved role=%s, %d peer MAC addresses to filesystem",
                getMeshRoleString(gSettings.meshRole), count);
  return skipped == 0;
}

// Save named ESP-NOW devices (paired devices with names/keys) to filesystem
static bool saveEspNowDevices() {
  if (!gEspNow) return false;
  if (!filesystemReady) return false;

  FsLockGuard fsGuard("espnow.devices.save");
  File f = VFS::openGuarded(ESPNOW_DEVICES_FILE, "w", VFS::systemAuth("espnow.devices_save"), true);
  if (!f) return false;
  int skipped = 0;

  f.println("{");
  f.println("  \"devices\": [");
  int count = 0;
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (isSelfMac(gEspNow->devices[i].mac)) continue;
    // Encrypt MAC address at rest (device-bound AES)
    char rawDevMacBuf[18];
    formatMacAddressBuf(gEspNow->devices[i].mac, rawDevMacBuf, sizeof(rawDevMacBuf));
    String encMac = encryptString(String(rawDevMacBuf));
    if (encMac.length() == 0) {
      ERROR_ESPNOWF("[ESP-NOW] Failed to encrypt device MAC for '%s', skipping entry", gEspNow->devices[i].name.c_str());
      skipped++;
      continue;
    }
    if (count > 0) f.println(",");
    f.print("    {\"mac\":\"");
    f.print(encMac);
    f.print("\",\"name\":\"");
    f.print(gEspNow->devices[i].name);
    f.print("\",\"encrypted\":");
    f.print(gEspNow->devices[i].encrypted ? "true" : "false");
    if (gEspNow->devices[i].encrypted) {
      // Build hex key string, then encrypt it
      char keyHex[33];
      for (int k = 0; k < 16; k++) {
        snprintf(keyHex + (k * 2), 3, "%02x", gEspNow->devices[i].key[k]);
      }
      keyHex[32] = '\0';
      String encKey = encryptString(String(keyHex));
      if (encKey.length() == 0) {
        ERROR_ESPNOWF("[ESP-NOW] Failed to encrypt device key for '%s', skipping entry", gEspNow->devices[i].name.c_str());
        skipped++;
        continue;
      }
      f.print(",\"key\":\"");
      f.print(encKey);
      f.print("\"");
    }
    // Cached metadata fields (persist across reboots)
    EspNowDevice& dev = gEspNow->devices[i];
    if (dev.friendlyName.length()) { f.print(",\"friendlyName\":\""); f.print(dev.friendlyName); f.print("\""); }
    if (dev.room.length())         { f.print(",\"room\":\""); f.print(dev.room); f.print("\""); }
    if (dev.zone.length())         { f.print(",\"zone\":\""); f.print(dev.zone); f.print("\""); }
    if (dev.tags.length())         { f.print(",\"tags\":\""); f.print(dev.tags); f.print("\""); }
    if (dev.stationary)            { f.print(",\"stationary\":true"); }
    // Phase 2 multi-mesh — record which mesh this peer belongs to. Omit
    // the key when meshId==0 to keep the on-disk file compact for the
    // common single-mesh case (loader defaults missing field to 0).
    if (dev.meshId != 0)           { f.print(",\"meshId\":"); f.print((int)dev.meshId); }
    f.print("}");
    count++;
  }
  f.println();
  f.println("  ]");
  f.println("}");
  f.close();
  DEBUGF(DEBUG_ESPNOW_MESH, "[ESP-NOW] Saved %d device(s) to %s", count, ESPNOW_DEVICES_FILE);
  return skipped == 0;
}

// Load mesh peer MAC addresses from filesystem (topology only)
// Forward declaration for parseMacAddress (defined later in file)
bool parseMacAddress(const String& macStr, uint8_t mac[6]);

// Health metrics (timestamps, counters) will be initialized to zero and rebuild from heartbeats
static void loadMeshPeers() {
  if (!filesystemReady) return;

  // Pause sensor polling during file I/O. Kept as explicit pause/resume (not the
  // RAII guard) because this function deliberately resumes BEFORE the parse step
  // below — the guard would hold the pause too long. Symbols from
  // System_PollPause.h via System_I2C.h.
  pollPause();

  FsLockGuard fsGuard("mesh.peers.load");
  File file = VFS::openGuarded(MESH_PEERS_FILE, "r", VFS::systemAuth("espnow.mesh_peers_load"));
  if (!file) {
    pollResume();
    DEBUGF(DEBUG_ESPNOW_MESH, "[MESH] No saved peer list found");
    return;
  }

  String content = file.readString();
  file.close();
  
  // Resume sensor polling before parsing
  pollResume();

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
    // Extract MAC address (may be AES-encrypted)
    int macStart = content.indexOf("\"", pos + 6) + 1;
    int macEnd = content.indexOf("\"", macStart);
    if (macStart <= 0 || macEnd <= macStart) break;

    String macStr = content.substring(macStart, macEnd);

    // Decrypt if stored encrypted (AES: prefix)
    if (macStr.startsWith("AES:")) {
      String decrypted = decryptString(macStr);
      if (decrypted.length() > 0) {
        macStr = decrypted;
      } else {
        WARN_ESPNOWF("[MESH] Failed to decrypt peer MAC, skipping");
        pos = macEnd;
        continue;
      }
    }

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
      peer->lastMeshHeartbeatMs = 0;
      peer->lastRxActivityMs = 0;
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
// Forward decl — defined further down with other multi-mesh helpers
static uint16_t meshFingerprintForLabel(const String& label);

static void setEspNowPassphrase(const String& passphrase) {
  if (!gEspNow) return;
  gEspNow->passphrase = passphrase;
  deriveKeyFromPassphrase(passphrase, gEspNow->derivedKey);

  Settings::MeshIdentity& m0 = gSettings.meshes[0];
  setSetting(m0.passphrase, passphrase);
  if (m0.label.length() == 0 && passphrase.length() > 0) {
    setSetting(m0.label, String("primary"));
    setSetting(m0.enabled, true);
    setSetting(m0.isDefault, true);
  }
  if (passphrase.length() == 0) {
    setSetting(m0.enabled, false);
  }
  m0.fingerprint = meshFingerprintForLabel(m0.label);
}

// ============================================================================
// BOND MODE SESSION TOKEN
// ============================================================================
// The bond auth token proves the @BOND/RCE command channel is talking to the
// genuine bonded peer. It used to be HMAC-SHA256(passphrase, sortedMACs) — a
// value that NEVER changed for a given pair, so a single capture of it (back
// when bond traffic was plaintext) granted permanent impersonation.
//
// As of task #33 it is instead derived from the live encrypted session's
// X25519 shared secret: the SAME 32-byte secret that yields the AEAD keys, run
// through one more Blake2b KDF subkey (id 3, context "esp-bond"). Properties:
//   - Fresh per session: every SESSION_OPEN does a new ephemeral DH, so each
//     session yields a brand-new token. A stolen token is useless next session.
//   - Never transmitted: both ends compute it independently from the shared
//     secret (which itself never crosses the wire). The token only ever appears
//     INSIDE encrypted @BOND frames, never on its own.
//   - No static secret: it no longer depends on the passphrase at all.
// Stored in RAM only, never persisted, cleared on disconnect.
//
// NOTE on REKEY: we deliberately do NOT re-derive the token when the session
// rekeys mid-life. The token already rides inside the rotating AEAD layer, so
// its confidentiality is protected regardless; re-deriving on REKEY would open
// a window where an in-flight @BOND command signed with the old token arrives
// after the peer rotated and gets spuriously rejected. Per-session freshness is
// what kills the "permanent secret" problem; intra-session rotation adds risk
// without a confidentiality gain.
//
// `peerMac` must be our bonded peer; called from the session handshake (both
// initiator and responder completion) while `shared` is still in scope.
// ===========================================================================
// Bond auth token — the SESSION is the single source of truth.
//
// The token (KDF subkey id 3 "esp-bond") is derived from the X25519 shared
// secret inside sessionDeriveAeadKeys, at every handshake, for every session,
// and stored in SessionState::bondToken. There is deliberately NO global mirror
// and no separate derive/clear lifecycle: the token exists exactly as long as
// its session does, and dies with it (a fresh session re-derives it instantly).
//
// This is what makes it robust: it no longer matters whether you mesh-paired
// before or after bondconnect, and the many resync events (peer reboot, HB
// timeout, role swap, re-bondconnect) that used to wipe a global copy can't
// touch it. To read/validate the token, just look up the ACTIVE session with
// the configured bond peer.
// ===========================================================================
static SessionState* bondPeerActiveSession() {
  if (!gEspNow || !gSettings.bondModeEnabled) return nullptr;
  if (gSettings.bondPeerMac.length() == 0) return nullptr;
  uint8_t pm[6];
  if (!parseMacAddress(gSettings.bondPeerMac, pm)) return nullptr;
  const PeerIdentity* pid = peerIdentityFindByMac(pm);
  if (!pid) return nullptr;
  SessionState* s = sessionFindByPeer(pm, pid->meshId);
  if (!s || s->state != SESSION_ACTIVE || !s->bondTokenValid) return nullptr;
  return s;
}

static bool validateBondSessionToken(const uint8_t* token, size_t tokenLen) {
  if (tokenLen != 16) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[BOND_AUTH] Validate failed: wrong token length %zu", tokenLen);
    return false;
  }
  SessionState* s = bondPeerActiveSession();
  if (!s) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[BOND_AUTH] Validate failed: no active bond-peer session token");
    return false;
  }
  bool match = (memcmp(token, s->bondToken, 16) == 0);
  if (!match) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[BOND_AUTH] Token mismatch: recv=%02X%02X%02X%02X... local=%02X%02X%02X%02X...",
           token[0], token[1], token[2], token[3],
           s->bondToken[0], s->bondToken[1], s->bondToken[2], s->bondToken[3]);
  } else {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[BOND_AUTH] Token validated OK");
  }
  return match;
}

// Format session token as hex string for sending in commands
// Returns pointer to static buffer (not thread-safe, use immediately)
static const char* formatSessionToken() {
  static char tokenStr[33];
  SessionState* s = bondPeerActiveSession();
  if (!s) return "";
  for (int i = 0; i < 16; i++) {
    sprintf(tokenStr + i*2, "%02X", s->bondToken[i]);
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

// Check if session token is valid (for external callers like OLED menu).
// True iff there's an ACTIVE encrypted session with the configured bond peer
// and that session has a derived bond token.
bool isBondSessionTokenValid() {
  return bondPeerActiveSession() != nullptr;
}

String buildBondedCommandPayload(const String& command) {
  if (!gEspNow) {
    ERROR_ESPNOWF("[BOND_AUTH] buildPayload: gEspNow null");
    return "";
  }
  if (!isBondSessionTokenValid()) {
    WARN_ESPNOWF("[BOND_AUTH] buildPayload: no active bond-peer session token");
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
// Phase 3.5 step 4 — LMK removed. Peers are now ALWAYS added unencrypted at
// the radio layer; confidentiality+integrity come from the application-layer
// SESSION_FRAME AEAD (Phase 3.4/3.5). The `useEncryption` and `encryptionKey`
// arguments are kept in the signature to avoid churn at every call site, but
// they're ignored — `peerInfo.encrypt = false` unconditionally.
// Note: bond-mode token derivation is a separate concern and not touched here.
static bool addEspNowPeerWithEncryption(const uint8_t* mac, bool /*useEncryption*/,
                                        const uint8_t* /*encryptionKey*/) {
  if (esp_now_is_peer_exist(mac)) {
    esp_now_del_peer(mac);
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = gEspNow->channel;
  peerInfo.ifidx   = WIFI_IF_STA;
  peerInfo.encrypt = false;  // LMK removed — confidentiality via SESSION_FRAME

  DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] Adding peer: %s (radio plaintext; AEAD via session if any)",
         formatMacAddress(mac).c_str());

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
  bool sent = v4_send_command_response(targetMac, msgId, success, fullResult.c_str(), fullResult.length());

  if (sent) {
    broadcastOutput("[ESP-NOW] V3 response sent successfully");
    DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] V3 response sent successfully to %s", senderName.c_str());
  } else {
    broadcastOutput("[ESP-NOW] WARNING: V3 response transmission failed");
  }

  // Restore streaming state
  gEspNow->streamingSuspended = wasStreaming;
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

  // Check for session-based streaming first (V3 CMD output).
  //
  // CRITICAL — task gate: gCurrentStreamCmdId is a global set for the whole
  // duration of an async bonded-command execution, and broadcastOutput() is
  // also global. Without restricting to the command's actual task, log lines
  // emitted by *unrelated* tasks during that window get force-streamed via a
  // synchronous AEAD-encrypt + esp_now_send on whatever stack they're running.
  // That (a) pollutes the command response with garbage (heartbeat ACKs, bond
  // status pushes) and (b) overflows small task stacks — e.g. an `opengamepad`
  // bonded command kicks the Seesaw init on sensor_queue_task (11 KB), and a
  // nested STREAM frame send from inside that deep init blew the stack. Stream
  // only the output produced by cmd_exec_task (24 KB), which is where the
  // streamed command actually runs and emits its real output.
  extern TaskHandle_t gCmdExecTaskHandle;
  if (gCurrentStreamCmdId != 0 && xTaskGetCurrentTaskHandle() == gCmdExecTaskHandle) {
    size_t msgLen = message.length();
    if (msgLen > ESPNOW_V4_MAX_PAYLOAD - 1) msgLen = ESPNOW_V4_MAX_PAYLOAD - 1;
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
  if (msgLen > ESPNOW_V4_MAX_PAYLOAD - 1) msgLen = ESPNOW_V4_MAX_PAYLOAD - 1;
  
  // Legacy global stream (startstream/stopstream) is uncorrelated live telemetry,
  // not a reply to any request — send msgId 0 so the receiver stores it as reqId 0
  // ("unsolicited"), exactly as before. (0 is also the dedup-skip sentinel.)
  // Command-session output uses the command's msgId instead (see sendSessionStreamFrame).
  uint32_t msgId = 0;
  bool sent = espnowtx::sendAead(gEspNow->streamTarget, ESPNOW_V4_TYPE_STREAM, 0, msgId,
                                  (const uint8_t*)message.c_str(), (uint16_t)msgLen, 1);

  if (sent) {
    gEspNow->streamSentCount++;
    DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] Legacy sent | sent=%lu | %.50s",
           (unsigned long)gEspNow->streamSentCount, message.c_str());
  }
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
// (enum EspNowV4Type and V4PayloadHeartbeat forward-declared near top of file)
// ============================================================================

// EspNowV4Flags moved to System_ESPNow_Wire.h

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

// Send stream frame to session target with optional flags. Phase 3.5 A2:
// each STREAM chunk goes through v4_send_payload_smart so individual chunks
// (≤ V4_MAX_FRAGMENT_PAYLOAD = 200B, well under MAX_PLAINTEXT) are
// AEAD-wrapped via SESSION_FRAME, closing the input/output encryption
// asymmetry where CMD went encrypted but the streamed output came back
// plaintext.
static void sendSessionStreamFrame(uint32_t cmdMsgId, const char* data, size_t len, uint8_t flags) {
  StreamSession* sess = findStreamSession(cmdMsgId);
  if (!sess || !sess->active) return;

  // Step 3c: fire-and-forget — called from both espnow_task (STREAM_BEGIN at
  // the CMD-RX site, line ~4857) and cmd_exec (output streaming). Fire-and-
  // forget is correct from both contexts: RX-side must not block, and cmd_exec
  // shouldn't block on per-chunk WiFi-TX either.
  espnowtx::sendAead(sess->targetMac, ESPNOW_V4_TYPE_STREAM, flags, cmdMsgId,
                     data ? (const uint8_t*)data : nullptr, (uint16_t)len, 1);
}

// Helper for sendEspNowStreamMessage - tries to send via session, returns true if sent
static bool trySendToStreamSession(uint32_t cmdMsgId, const char* data, size_t len) {
  StreamSession* sess = findStreamSession(cmdMsgId);
  if (!sess || !sess->active) return false;

  // Step 3c: fire-and-forget through clerk (called from cmd_exec output path).
  espnowtx::sendAead(sess->targetMac, ESPNOW_V4_TYPE_STREAM, 0, cmdMsgId,
                     (const uint8_t*)data, (uint16_t)len, 1);
  DEBUGF(DEBUG_ESPNOW_STREAM, "[STREAM] Session %lu sent: %.50s", (unsigned long)cmdMsgId, data);
  return true;
}

// EspNowV4Header, V4PayloadCmdResp, V4PayloadFile* moved to System_ESPNow_Wire.h

struct V4DedupEntry { uint8_t origin[6]; uint32_t id; uint32_t ts; uint8_t fragIndex; bool active; };
#define V4_DEDUP_SIZE 64
static V4DedupEntry gV4Dedup[V4_DEDUP_SIZE];
static int gV4DedupIdx = 0;

// Broadcast ACK tracking
static BroadcastTracker gBroadcastTrackers[BROADCAST_TRACKER_SLOTS];
static uint32_t gBroadcastsTracked = 0;
static uint32_t gBroadcastsCompleted = 0;
static uint32_t gBroadcastsTimedOut = 0;


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

static uint16_t v4_crc16_ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (int b = 0; b < 8; b++) { if (crc & 0x8000) crc = (uint16_t)((crc << 1) ^ 0x1021); else crc = (uint16_t)(crc << 1); }
  }
  return crc;
}

// ============================================================================
// Multi-mesh helpers (Phase 2 of docs/ESPNOW_V4_PLAN.md)
// ============================================================================
//
// Each mesh has a human-readable label (e.g. "primary", "work") and a
// non-cryptographic 16-bit fingerprint = CRC16-CCITT(label). The fingerprint
// is what goes on the wire in the V4 header's meshFingerprint field — local
// mesh indices differ between devices, so we hash a stable string both
// devices agree on.
//
// Phase 2.1 (this commit): infrastructure only. The TX path still emits
// fingerprint=0 (V4 default). RX path doesn't yet validate. meshes[0] gets
// initialized from boot settings.

// Compute the CRC16-CCITT fingerprint of a mesh label. Stable across devices
// — both peers compute the same fingerprint for the same label.
static uint16_t meshFingerprintForLabel(const String& label) {
  if (label.length() == 0) return 0;
  return v4_crc16_ccitt((const uint8_t*)label.c_str(), label.length());
}

// Recompute and cache the fingerprint of every configured mesh. Cheap (CRC16
// over a few bytes per mesh × at most N_MESHES meshes). Called from
// initEspNow and whenever a mesh's label changes.
static void recomputeAllMeshFingerprints() {
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    gSettings.meshes[i].fingerprint =
        meshFingerprintForLabel(gSettings.meshes[i].label);
  }
}

// Look up a local mesh by its on-wire fingerprint. Returns nullptr if no
// enabled mesh matches — caller should drop the frame silently.
static Settings::MeshIdentity* meshByFingerprint(uint16_t fingerprint) {
  if (fingerprint == 0) return nullptr;
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].enabled &&
        gSettings.meshes[i].fingerprint == fingerprint) {
      return &gSettings.meshes[i];
    }
  }
  return nullptr;
}


// Look up the mesh fingerprint to stamp on a frame addressed to `mac`. For
// known peers, uses the peer's stored meshId. For broadcast (FF:FF:...) and
// unknown peers, falls back to the default mesh (the one flagged isDefault,
// or meshes[0] if none flagged). Returns 0 if no mesh is configured —
// frame goes out with fingerprint=0 and receivers may drop it.
static uint16_t fingerprintForPeer(const uint8_t* mac) {
  if (!mac) return 0;
  // Known peer?
  if (gEspNow) {
    for (int i = 0; i < gEspNow->deviceCount; i++) {
      if (memcmp(gEspNow->devices[i].mac, mac, 6) == 0) {
        uint8_t mid = gEspNow->devices[i].meshId;
        if (mid < Settings::N_MESHES && gSettings.meshes[mid].enabled) {
          return gSettings.meshes[mid].fingerprint;
        }
        break;
      }
    }
  }
  // Unknown peer or broadcast — use default mesh
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].enabled && gSettings.meshes[i].isDefault) {
      return gSettings.meshes[i].fingerprint;
    }
  }
  // No default flagged — use first enabled
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].enabled) {
      return gSettings.meshes[i].fingerprint;
    }
  }
  return 0;
}

// ============================================================================
// Phase 2 — Settings JSON persistence for multi-mesh fields
// ============================================================================
//
// The SettingsRegistry pattern (SETTING_INT / SETTING_STRING / ...) handles
// flat scalar fields automatically, but doesn't fit an array-of-structs like
// gSettings.meshes[] or the per-mesh bond arrays. Mirror the BLE_Peers
// pattern: dedicated writeJson/readJson functions called from
// buildSettingsJsonDoc and the settings-load path.
//
// On-disk shape (under doc["espnow"]):
//   "meshes": [
//     {"label":"primary", "passphrase":"...", "enabled":true, "isDefault":true},
//     ...
//   ],
//   "bondsByMesh": [
//     {"enabled":false, "role":0, "peer":""},
//     ...
//   ]
//
// `fingerprint` is NOT persisted — it's a pure function of `label` and is
// recomputed at load time via recomputeAllMeshFingerprints().

void espnowMeshesWriteJson(JsonDocument& doc, bool excludePasswords) {
  JsonArray meshesArr = doc["espnow"]["meshes"].to<JsonArray>();
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    const Settings::MeshIdentity& m = gSettings.meshes[i];
    // Skip fully-empty slots to keep settings.json small. A slot that's
    // disabled but has a label is still persisted (lets users keep a
    // mesh configured-but-off).
    if (!m.enabled && m.label.length() == 0) continue;
    JsonObject e = meshesArr.add<JsonObject>();
    e["label"]     = m.label;
    if (!excludePasswords) {
      // At-rest AES (device key) — was plaintext. The bond-sync path uses
      // excludePasswords=true, so this only encrypts the persisted settings.json;
      // peers are never sent this device-key-encrypted blob.
      putSecret(e, "passphrase", m.passphrase);
    }
    e["enabled"]   = m.enabled;
    e["isDefault"] = m.isDefault;
    // The PBKDF2-stretched hash IS the mesh key material — the group/bootstrap
    // keys derive directly from it (System_ESPNow_MeshKeys.cpp). It is NOT a
    // harmless "irreversible digest": anyone who reads it derives the group key
    // WITHOUT the passphrase. So encrypt it at rest (device key) like the
    // passphrase, and keep it OUT of the sync/web (excludePasswords) paths. If
    // it's absent on load, the hash is re-stretched from the passphrase at boot
    // — so dropping it from those paths is safe.
    if (!excludePasswords && m.passphraseStretchedKeyValid) {
      char hex[65];
      static const char* kHex = "0123456789abcdef";
      for (int k = 0; k < 32; k++) {
        hex[k * 2]     = kHex[(m.passphraseStretchedKey[k] >> 4) & 0xF];
        hex[k * 2 + 1] = kHex[m.passphraseStretchedKey[k] & 0xF];
      }
      hex[64] = '\0';
      putSecret(e, "passphraseStretchedKey", String(hex));
    }
  }
#if ENABLE_BONDED_MODE
  JsonArray bondsArr = doc["espnow"]["bondsByMesh"].to<JsonArray>();
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    JsonObject e = bondsArr.add<JsonObject>();
    e["enabled"] = gSettings.bondModeEnabledMesh[i];
    e["role"]    = gSettings.bondRoleMesh[i];
    e["peer"]    = gSettings.bondPeerMacMesh[i];
  }
#endif
}

void espnowMeshesReadJson(JsonDocument& doc) {
  JsonArrayConst meshesArr = doc["espnow"]["meshes"].as<JsonArrayConst>();
  if (!meshesArr.isNull()) {
    // Reset all slots to defaults first — load is authoritative for what's
    // in the file. Slots not in the file get default (empty/disabled).
    for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
      gSettings.meshes[i] = Settings::MeshIdentity();
    }
    uint8_t idx = 0;
    for (JsonObjectConst e : meshesArr) {
      if (idx >= Settings::N_MESHES) break;
      gSettings.meshes[idx].label      = String((const char*)(e["label"]      | ""));
      gSettings.meshes[idx].passphrase = getSecret(e, "passphrase");  // decrypt at-rest AES
      gSettings.meshes[idx].enabled    = e["enabled"]   | false;
      gSettings.meshes[idx].isDefault  = e["isDefault"] | false;
      // Phase 3.1: load the cached PBKDF2 hash if present. Hex-decode strict;
      // any malformed value falls back to "no cached hash" (will trigger a
      // re-stretch at boot) rather than refusing to load settings.
      gSettings.meshes[idx].passphraseStretchedKeyValid = false;
      String keyHex = getSecret(e, "passphraseStretchedKey");  // decrypt at-rest AES (key material)
      if (keyHex.length() == 64) {
        const char* hp = keyHex.c_str();
        auto nyb = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          return -1;
        };
        bool ok = true;
        for (int k = 0; k < 32 && ok; k++) {
          int hi = nyb(hp[k * 2]);
          int lo = nyb(hp[k * 2 + 1]);
          if (hi < 0 || lo < 0) { ok = false; break; }
          gSettings.meshes[idx].passphraseStretchedKey[k] = (uint8_t)((hi << 4) | lo);
        }
        gSettings.meshes[idx].passphraseStretchedKeyValid = ok;
      }
      // fingerprint recomputed below
      idx++;
    }
    recomputeAllMeshFingerprints();
  }
#if ENABLE_BONDED_MODE
  JsonArrayConst bondsArr = doc["espnow"]["bondsByMesh"].as<JsonArrayConst>();
  if (!bondsArr.isNull()) {
    uint8_t idx = 0;
    for (JsonObjectConst e : bondsArr) {
      if (idx >= Settings::N_MESHES) break;
      gSettings.bondModeEnabledMesh[idx] = e["enabled"] | false;
      gSettings.bondRoleMesh[idx]        = (uint8_t)(e["role"] | 0);
      gSettings.bondPeerMacMesh[idx]     = String((const char*)(e["peer"] | ""));
      idx++;
    }
  }
#endif
}

// Phase 2.1 init shim: populate meshes[0] from the legacy single-mesh fields
// if it hasn't been configured yet. Lets existing user flows (e.g. CLI
// `espnowsetpassphrase`) keep writing the legacy field; meshes[0] picks up
// the value at next boot. Phase 2.x sub-commits will rewire the CLI/web
// commands to write meshes[] directly, at which point this shim becomes
// dead code.
static void initPrimaryMeshFromLegacySettings() {
  Settings::MeshIdentity& m0 = gSettings.meshes[0];
  if (m0.label.length() > 0) {
    // Already configured (probably from a previous boot that already ran
    // this shim and persisted). Just recompute fingerprint and move on.
    m0.fingerprint = meshFingerprintForLabel(m0.label);
    return;
  }
  // Always bootstrap "primary" in slot 0, even on a fresh device with no
  // passphrase yet. Decoupling 'espnowmeshes add' from passphrase setup
  // (Phase 2.8 refactor) means CLI users target a mesh by name first and
  // set its passphrase second — so the named slot has to exist at boot.
  m0.label       = "primary";
  m0.fingerprint = meshFingerprintForLabel(m0.label);
  m0.enabled     = true;
  m0.isDefault   = true;
#if ENABLE_BONDED_MODE
  gSettings.bondModeEnabledMesh[0] = gSettings.bondModeEnabled;
  gSettings.bondRoleMesh[0]        = gSettings.bondRole;
  gSettings.bondPeerMacMesh[0]     = gSettings.bondPeerMac;
#endif
}

// Entries older than this are stale and must not shadow a new message that
// reuses the id. A peer that reboots restarts its msgId counter from ~0, so
// without expiry its reused low ids collide with our pre-reboot entries and get
// silently dropped — including the heartbeats carrying the new bootCounter that
// would trigger recovery. Genuine duplicates (mesh loops, radio re-delivery)
// arrive within milliseconds; bond retries use fresh msgIds — so 5s is ample.
static constexpr uint32_t V4_DEDUP_TTL_MS = 5000;

// fragIndex is part of the key: a multi-fragment message reuses one msgId across
// all its fragments (the group id), so deduping by (origin,msgId) alone would
// drop fragments 1..N-1 as "duplicates" of fragment 0. Keying on fragIndex too
// keeps distinct fragments distinct while still catching a true retransmit of
// the SAME fragment. Single-frame messages always pass fragIndex 0.
static bool v4_dedup_seen_and_insert(const uint8_t* origin, uint32_t id, uint8_t fragIndex = 0) {
  uint32_t now = (uint32_t)millis();
  for (int i = 0; i < V4_DEDUP_SIZE; i++) {
    if (gV4Dedup[i].active && gV4Dedup[i].id == id && gV4Dedup[i].fragIndex == fragIndex &&
        memcmp(gV4Dedup[i].origin, origin, 6) == 0) {
      // Unsigned subtraction is rollover-safe across the ~49.7-day millis() wrap.
      if ((uint32_t)(now - gV4Dedup[i].ts) < V4_DEDUP_TTL_MS) return true;
      // Expired: accept as new and refresh in place so an immediate radio-level
      // duplicate of THIS message is still caught.
      gV4Dedup[i].ts = now;
      return false;
    }
  }
  V4DedupEntry& e = gV4Dedup[gV4DedupIdx];
  memcpy(e.origin, origin, 6); e.id = id; e.fragIndex = fragIndex; e.ts = now; e.active = true;
  gV4DedupIdx = (gV4DedupIdx + 1) % V4_DEDUP_SIZE;
  return false;
}

// Drop all dedup entries from one origin. Called when a peer reboot is detected
// (bootCounter change) so the peer's restarted-from-zero msgIds are not shadowed
// by its pre-reboot entries — instant recovery without waiting for TTL expiry.
static void v4_dedup_flush_origin(const uint8_t* origin) {
  for (int i = 0; i < V4_DEDUP_SIZE; i++) {
    if (gV4Dedup[i].active && memcmp(gV4Dedup[i].origin, origin, 6) == 0) {
      gV4Dedup[i].active = false;
    }
  }
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
        uint32_t rttMs = (uint32_t)(now - t->startMs);
        BROADCAST_PRINTF("[Broadcast] msgId=%lu: %u/%u peers ACK'd (100%%) in %lums",
                        (unsigned long)t->msgId, t->receivedCount, t->expectedCount,
                        (unsigned long)rttMs);
        // Feed the saturation sampler: this is our cleanest source of ACK RTT
        // observations (one per completed broadcast). Sensor-stream traffic is
        // broadcast-shaped, so this directly tracks the stress-test workload.
        espnowSaturationNoteAckRtt(rttMs);
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

// ============================================================================
// SEND-PATH MAP — "which send function do I call?"  (Updated 2026-05; smart
// is now strict encrypt-or-fail, plaintext fallback removed everywhere.)
//
// APPLICATION payloads (the common case):
//   * Bond traffic (any BOND_/SENSOR_DATA/STREAM_CTRL/SETTINGS opcode)
//        -> bondSendEncrypted()        MAC-anchored single-initiator + encrypt;
//                                       the ONLY entry the bond layer should use.
//   * General unicast app payload      -> v4_send_payload_smart()
//                                       strict encrypt-or-fail (no plaintext
//                                       fallback). Auto-handshakes peers it has
//                                       never KEY_EX'd with (queues frame + kicks
//                                       KEY_EX → SESSION_OPEN → drains on CONFIRM).
//                                       The default app entry.
//   * Single frame that must WAIT for a session (queue + kick SESSION_OPEN, drain
//     on CONFIRM)                       -> v4_send_encrypted_or_queue()
//                                       Now also kicks KEY_EX when no peer identity.
//   * Mesh-wide fan-out                 -> v4_broadcast() / v4_broadcast_category()
//                                       Per-peer plaintext with BROADCAST_AUTH HMAC
//                                       (no per-peer session exists for FF:FF:...).
//                                       Authentication, not confidentiality.
//   * Reachability probe                -> espnowprobe CLI verb (KEY_EX as probe).
//
// LOW-LEVEL / INTERNAL (do NOT call from app code — these are the primitives
// that bootstrap the encryption, so they cannot themselves be encrypted):
//   * v4_send_frame            raw single frame. Used for handshakes
//                              (KEY_EX_*, SESSION_OPEN/CONFIRM/REKEY), ACKs,
//                              TOPO_*, and as the primitive under everything else.
//   * v4_send_encrypted_chunked  multi-frame AEAD-sealed; smart calls this for
//                              payloads > MAX_PLAINTEXT when a session is ACTIVE.
//   * v4_send_session_wrapped  AEAD-wrap one frame for an ACTIVE session.
//
// (v4_send_chunked — plaintext multi-frame — was REMOVED 2026-05. No callers
// remain; the RX-side plaintext reassembler stays for one release in case an
// old-firmware peer sends one.)
//
// Rule of thumb: app code calls bondSendEncrypted (bond) or v4_send_payload_smart
// (everything else). If you reach for v4_send_frame in app code, you are almost
// certainly sending plaintext by mistake. Mesh membership gates encryption
// (KEY_EX HMAC); per-peer sessions provide confidentiality. Cross-mesh peers
// can never establish a session, so cross-mesh sends fail silently after the
// pending-frame ring's timeout sweep — which is correct (different mesh =
// locked out by design).
// ============================================================================
bool v4_send_frame(const uint8_t* dst, uint8_t type, uint16_t flags, uint32_t msgId,
                   const uint8_t* payload, uint16_t payloadLen, uint8_t ttl) {
  if (!dst || (payloadLen > 0 && !payload)) return false;
  EspNowV4Header h = {};
  h.magic = (uint16_t)ESPNOW_V4_MAGIC; h.ver = ESPNOW_V4_VERSION; h.type = type; h.flags = flags;
  h.headerLen = (uint8_t)sizeof(EspNowV4Header); h.msgId = msgId;
  uint8_t myMac[6]; esp_wifi_get_mac(WIFI_IF_STA, myMac); memcpy(h.origin, myMac, 6);
  h.ttl = ttl; h.fragIndex = 0; h.fragCount = 1;
  h.meshFingerprint = fingerprintForPeer(dst);  // Phase 2: scope frame to dst's mesh

  // Phase 3.5 task #32 — if the caller flagged BROADCAST_AUTH, append an
  // HMAC-SHA256 tag computed over (header[0..30] || payload) with the mesh
  // group key. Receivers verify and silently drop forgeries. Tag is part of
  // the on-wire payload (CRC covers it too, as a transport-level check).
  uint8_t outBuf[ESPNOW_V4_MAX_PAYLOAD];
  const uint8_t* finalPayload = payload;
  uint16_t       finalPayloadLen = payloadLen;
  if ((h.flags & ESPNOW_V4_FLAG_BROADCAST_AUTH) && h.meshFingerprint != 0) {
    const MeshDerivedKeys* mk = meshKeysFindByFingerprint(h.meshFingerprint);
    if (mk && mk->valid) {
      if (payloadLen + ESPNOW_V4_BROADCAST_AUTH_TAG_LEN > ESPNOW_V4_MAX_PAYLOAD) {
        DEBUGF(DEBUG_ESPNOW_CORE,
               "[V4_TX] BROADCAST_AUTH: payload too large (%u + %u tag > %u) — dropping",
               payloadLen, ESPNOW_V4_BROADCAST_AUTH_TAG_LEN, ESPNOW_V4_MAX_PAYLOAD);
        return false;
      }
      memcpy(outBuf, payload, payloadLen);
      uint8_t hmac[32];
      // HMAC AAD = header bytes 0..29 (everything except crc16, mirroring
      // the SESSION_FRAME AAD convention). Pass header first, payload second.
      if (!espnowCryptoHmacSha256(hmac,
                                   mk->groupKey, 32,
                                   reinterpret_cast<const uint8_t*>(&h), 30,
                                   payload, payloadLen,
                                   nullptr, 0)) {
        ERROR_ESPNOWF("[V4_TX] BROADCAST_AUTH: HMAC compute failed");
        return false;
      }
      memcpy(outBuf + payloadLen, hmac, ESPNOW_V4_BROADCAST_AUTH_TAG_LEN);
      finalPayload    = outBuf;
      finalPayloadLen = payloadLen + ESPNOW_V4_BROADCAST_AUTH_TAG_LEN;
    } else {
      // No mesh group key available — strip the flag and send plaintext.
      // Happens during early-boot before mesh keys derive, or if the peer
      // belongs to a mesh we don't have the passphrase for (shouldn't reach
      // v4_broadcast in that case, but defensive).
      h.flags = (uint16_t)(h.flags & ~ESPNOW_V4_FLAG_BROADCAST_AUTH);
    }
  }

  size_t totalLen = sizeof(EspNowV4Header) + finalPayloadLen;
  if (totalLen > 250) return false;
  uint8_t frame[250];
  h.crc16 = finalPayloadLen > 0 ? v4_crc16_ccitt(finalPayload, finalPayloadLen) : 0;
  memcpy(frame, &h, sizeof(h));
  if (finalPayloadLen > 0) memcpy(frame + sizeof(h), finalPayload, finalPayloadLen);
  captureEspNowFrame("TX", dst, 0, frame, (int)totalLen);
  return esp_now_send(dst, frame, totalLen) == ESP_OK;
}

// Phase 3.5 step 1 — session-wrapped unicast send. Mirrors v4_send_frame's
// signature minus features that don't apply when a frame is AEAD-sealed:
// no CRC (the Poly1305 tag is the integrity check), no v4 ACK tracking via
// the broadcast tracker path, no fragmentation (caller's responsibility to
// keep plaintext under ESPNOW_V4_MAX_PAYLOAD - 16).
//
// Looks up the peer's ACTIVE SessionState by (peerMac, meshId derived from
// PeerIdentity), builds the V4 header, defers to sessionWrapFrame for the
// AEAD seal + flag/sessionId/frameSeq stamping, then sends raw.
//
// Returns true on success. On failure, *errOut (if non-null) is filled with
// a short human-readable reason ("no peer identity", "no active session",
// "payload too large", "AEAD seal failed", "esp_now_send rc=N").
bool v4_send_session_wrapped(const uint8_t dst[6], uint8_t type, uint16_t baseFlags,
                             uint32_t msgId,
                             const uint8_t* plaintext, uint16_t plaintextLen,
                             uint8_t ttl, char* errOut, size_t errOutLen) {
  auto setErr = [&](const char* msg) {
    if (errOut && errOutLen) { strncpy(errOut, msg, errOutLen - 1); errOut[errOutLen - 1] = 0; }
  };
  if (!dst || (plaintextLen > 0 && !plaintext)) {
    setErr("invalid args");
    return false;
  }
  if (plaintextLen > (uint16_t)(ESPNOW_V4_MAX_PAYLOAD - 16)) {
    setErr("payload too large for SESSION_FRAME (max 202 plaintext bytes)");
    return false;
  }
  const PeerIdentity* pid = peerIdentityFindByMac(dst);
  if (!pid) {
    setErr("no peer identity — run espnowkeyex first");
    return false;
  }
  SessionState* s = sessionFindByPeer(dst, pid->meshId);
  if (!s || s->state != SESSION_ACTIVE) {
    setErr("no ACTIVE session — run espnowsessionopen first");
    return false;
  }

  uint8_t frame[sizeof(EspNowV4Header) + ESPNOW_V4_MAX_PAYLOAD];
  EspNowV4Header* h = reinterpret_cast<EspNowV4Header*>(frame);
  memset(h, 0, sizeof(*h));
  h->magic           = (uint16_t)ESPNOW_V4_MAGIC;
  h->ver             = ESPNOW_V4_VERSION;
  h->type            = type;
  h->flags           = baseFlags;  // sessionWrapFrame ORs in ESPNOW_V4_FLAG_SESSION_FRAME
  h->headerLen       = (uint8_t)sizeof(EspNowV4Header);
  h->msgId           = msgId;
  uint8_t selfMac[6]; esp_wifi_get_mac(WIFI_IF_STA, selfMac);
  memcpy(h->origin, selfMac, 6);
  h->ttl             = ttl;
  h->fragIndex       = 0;
  h->fragCount       = 1;
  h->meshFingerprint = fingerprintForPeer(dst);

  uint8_t* outPayload = frame + sizeof(EspNowV4Header);
  // TXDEPTH instrumentation — measures where the shared send path spends stack.
  // uxTaskGetStackHighWaterMark returns the task's lifetime MIN free words, so
  // the value only drops; a drop between checkpoints localizes the consumer.
  // task name lets us compare text (cmd_exec_task) vs sensor (espnow_tx).
  UBaseType_t hwmPre = uxTaskGetStackHighWaterMark(nullptr);
  if (!sessionWrapFrame(s, h, plaintext, plaintextLen, outPayload)) {
    setErr("sessionWrapFrame failed");
    return false;
  }
  UBaseType_t hwmAEAD = uxTaskGetStackHighWaterMark(nullptr);
  size_t frameLen = sizeof(EspNowV4Header) + plaintextLen + 16;
  captureEspNowFrame("TX", dst, 0, frame, (int)frameLen);
  esp_err_t rc = esp_now_send(dst, frame, frameLen);
  UBaseType_t hwmSend = uxTaskGetStackHighWaterMark(nullptr);
  DEBUGF(DEBUG_ESPNOW_CORE,
         "[TXDEPTH] task=%s type=%u len=%u free_words: pre=%u postAEAD=%u postSend=%u",
         pcTaskGetName(nullptr), (unsigned)type, (unsigned)plaintextLen,
         (unsigned)hwmPre, (unsigned)hwmAEAD, (unsigned)hwmSend);
  if (rc != ESP_OK) {
    if (errOut && errOutLen) snprintf(errOut, errOutLen, "esp_now_send rc=%d", (int)rc);
    return false;
  }
  return true;
}

// Phase 3.5 step 3 — encrypted send with auto-kick + queue.
// Behaviour:
//   - If a peer identity is on file AND an ACTIVE session exists, wrap & send
//     immediately (same path as v4_send_session_wrapped).
//   - If a peer identity exists but no ACTIVE session, queue the frame and
//     kick a SESSION_OPEN (unless one is already ESTABLISHING for this peer,
//     in which case just queue). The frame will drain on SESSION_CONFIRM.
//   - If NO peer identity exists, queue the frame and kick a KEY_EX with the
//     default mesh's bootstrap key (the "encrypt-or-wait" foundation, late
//     2026-05). KEY_EX_REPLY's handler will auto-kick SESSION_OPEN when it
//     sees pending frames for the peer, which then drains via the existing
//     SESSION_CONFIRM → pendingFrameDrainForPeer chain.
//
// Single-mesh assumption: we use the configured default mesh (espnowKeyExInitiate
// with nullptr meshLabel) for the bootstrap-key choice. Multi-mesh selection
// (try-all-meshes, topology-cache fingerprint hint) is a TODO — most devices
// are single-mesh today, so this gets the common case right without the
// complexity. A multi-mesh device today still works for any peer it has
// previously seen (existing identity flow), and explicit espnowkeyex / espnowprobe
// with a --mesh arg covers manual fan-out for new peers.
//
// Returns true on the happy paths (sent immediately, queued, OR queued+KEY_EX-kicked).
// Returns false only on hard errors: invalid args, payload too large, queue full,
// or no enabled mesh at all (espnowKeyExInitiate returns false).
//
// `outStatus` (if non-null) is filled with a short status string:
//   "sent"              — immediately wrapped & sent
//   "queued"            — parked for drain after SESSION_OPEN completes
//   "queued; KEY_EX"    — parked + KEY_EX initiated (no peer identity yet)
//   "<error>"           — failure reason on false return
bool v4_send_encrypted_or_queue(const uint8_t dst[6], uint8_t type, uint16_t baseFlags,
                                uint32_t msgId,
                                const uint8_t* plaintext, uint16_t plaintextLen,
                                uint8_t ttl,
                                char* outStatus, size_t outStatusLen) {
  auto setStatus = [&](const char* msg) {
    if (outStatus && outStatusLen) { strncpy(outStatus, msg, outStatusLen - 1); outStatus[outStatusLen - 1] = 0; }
  };
  if (!dst || (plaintextLen > 0 && !plaintext)) {
    setStatus("invalid args");
    return false;
  }
  if (plaintextLen > (uint16_t)(ESPNOW_V4_MAX_PAYLOAD - 16)) {
    setStatus("payload too large for SESSION_FRAME (max 202 plaintext bytes)");
    return false;
  }
  const PeerIdentity* pid = peerIdentityFindByMac(dst);
  if (!pid) {
    // Foundation behavior (encrypt-or-wait): no peer identity → queue + kick KEY_EX.
    // KEY_EX_REPLY's handler will see pending frames and kick SESSION_OPEN;
    // SESSION_CONFIRM will then drain via pendingFrameDrainForPeer.
    if (!pendingFrameQueue(dst, type, baseFlags, msgId, ttl, plaintext, plaintextLen)) {
      setStatus("pending queue full");
      return false;
    }
    if (!keyExIsInFlight(dst)) {
      if (!espnowKeyExInitiate(dst, nullptr)) {
        // KEY_EX kick failed (no enabled mesh at all). The frame stays in the
        // pending ring; the timeout sweep (kPendingFrameTimeoutMs) will eat it
        // eventually. Status reflects what happened.
        setStatus("queued; KEY_EX kick failed (no enabled mesh?)");
        return true;
      }
      INFO_ESPNOWF("encrypted send: no peer identity for %02X:%02X:%02X:%02X:%02X:%02X — "
                   "queued msgId=%lu, kicked KEY_EX",
                   dst[0], dst[1], dst[2], dst[3], dst[4], dst[5],
                   (unsigned long)msgId);
    } else {
      // KEY_EX already in flight for this peer (e.g., a sibling caller just
      // kicked it, or the retry sweep is mid-attempt). Just queue.
      INFO_ESPNOWF("encrypted send: KEY_EX already in flight to %02X:%02X:%02X:%02X:%02X:%02X — "
                   "queued msgId=%lu",
                   dst[0], dst[1], dst[2], dst[3], dst[4], dst[5],
                   (unsigned long)msgId);
    }
    setStatus("queued; KEY_EX");
    return true;
  }
  SessionState* s = sessionFindByPeer(dst, pid->meshId);
  if (s && s->state == SESSION_ACTIVE) {
    // Fast path — already have an active session.
    char err[96] = {0};
    bool ok = v4_send_session_wrapped(dst, type, baseFlags, msgId,
                                      plaintext, plaintextLen, ttl, err, sizeof(err));
    if (!ok) {
      setStatus(err[0] ? err : "wrap-or-send failed");
      return false;
    }
    setStatus("sent");
    return true;
  }
  // Slow path — no active session. Queue the frame, then kick a SESSION_OPEN
  // unless one is already in flight (s exists and is ESTABLISHING).
  if (!pendingFrameQueue(dst, type, baseFlags, msgId, ttl, plaintext, plaintextLen)) {
    setStatus("pending queue full");
    return false;
  }
  if (!s || s->state == SESSION_FREE || s->state == SESSION_CLOSED) {
    // No handshake started yet — kick one.
    if (!espnowSessionOpenInitiate(dst, nullptr)) {
      // Initiate failed (e.g., peer identity vanished mid-call). Yank the
      // frame back out of the queue rather than leaving it to expire.
      // pendingFrameDrainForPeer is a hammer — but it'll log the drain
      // failure (no session) which is informative. Simpler is to just let
      // the timeout sweep eat it; the user already got an error log from
      // espnowSessionOpenInitiate.
      setStatus("queued; SESSION_OPEN kick failed (see log)");
      return true;  // still "queued" — the timeout sweep will clean it up
    }
    INFO_ESPNOWF("encrypted send: no session yet for %02X:%02X:%02X:%02X:%02X:%02X — "
                 "queued msgId=%lu, kicked SESSION_OPEN",
                 dst[0], dst[1], dst[2], dst[3], dst[4], dst[5],
                 (unsigned long)msgId);
  } else {
    // Already ESTABLISHING / REKEYING — just queue, the existing handshake
    // will drain us.
    INFO_ESPNOWF("encrypted send: handshake already in flight to %02X:%02X:%02X:%02X:%02X:%02X — "
                 "queued msgId=%lu",
                 dst[0], dst[1], dst[2], dst[3], dst[4], dst[5],
                 (unsigned long)msgId);
  }
  setStatus("queued");
  return true;
}

/**
 * Broadcast message to all mesh peers
 * Sends to each active peer individually (ESP-NOW doesn't support true broadcast)
 * If ESPNOW_V4_FLAG_ACK_REQ is set, tracks ACKs for delivery confirmation
 */
bool v4_broadcast(uint8_t type, uint16_t flags, uint32_t msgId, const uint8_t* payload, uint16_t payloadLen, uint8_t ttl) {
  if (!gEspNow || !gEspNow->initialized) return false;
  
  bool anySuccess = false;
  int sentCount = 0;
  BroadcastTracker* tracker = nullptr;
  
  // If ACK requested, allocate tracker
  if (flags & ESPNOW_V4_FLAG_ACK_REQ) {
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
  
  // Phase 3.5 task #32 — OR in BROADCAST_AUTH so v4_send_frame appends an
  // HMAC tag keyed by the mesh group key. Receivers verify; outsiders' forged
  // broadcasts drop silently on bad HMAC. Skipped at TX time per-peer if
  // their meshFingerprint is 0 or we don't hold a key for that mesh.
  uint16_t flagsAuthed = (uint16_t)(flags | ESPNOW_V4_FLAG_BROADCAST_AUTH);

  // Send to all active mesh peers
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac)) {
      bool sent = v4_send_frame(gMeshPeers[i].mac, type, flagsAuthed, msgId, payload, payloadLen, ttl);
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
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_BROADCAST] Sent to %d peers (msgId=%lu type=%u tracked=%s)",
         sentCount, (unsigned long)msgId, type, tracker ? "YES" : "NO");

  return anySuccess;
}

// Phase 5 — broadcast variant that respects per-peer subscription bitmaps.
// Functionally identical to v4_broadcast except a peer is skipped if its
// PeerIdentity.subscribedEvents bitmap doesn't include `category`. Unknown
// peers (no PeerIdentity slot) default to "subscribed to everything", so
// pre-Phase-5 peers and freshly-paired peers continue to receive every
// broadcast until they explicitly narrow via SUBSCRIBE_UPDATE.
//
// Designed as an additive wrapper so existing v4_broadcast callsites stay
// untouched. As callers migrate to category-aware broadcasts, traffic to
// uninterested peers drops out.
bool v4_broadcast_category(uint8_t type, uint16_t flags, uint32_t msgId,
                           const uint8_t* payload, uint16_t payloadLen, uint8_t ttl,
                           uint32_t category, int* outAttempted) {
  if (outAttempted) *outAttempted = 0;
  if (!gEspNow || !gEspNow->initialized) return false;

  bool anySuccess = false;
  int sentCount = 0;
  int skippedCount = 0;
  int attemptedCount = 0;  // peers we actually called v4_send_frame on (vs absent/skipped)
  BroadcastTracker* tracker = nullptr;

  if (flags & ESPNOW_V4_FLAG_ACK_REQ) {
    tracker = broadcast_tracker_alloc();
    if (tracker) {
      tracker->msgId = msgId;
      tracker->startMs = millis();
      tracker->expectedCount = 0;
      tracker->receivedCount = 0;
      tracker->active = true;
      tracker->reported = false;
      gBroadcastsTracked++;
    }
  }

  uint16_t flagsAuthed = (uint16_t)(flags | ESPNOW_V4_FLAG_BROADCAST_AUTH);

  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (!gMeshPeers[i].isActive || isSelfMac(gMeshPeers[i].mac)) continue;
    // Phase 5 gate: peer must want this category.
    if (!peerIdentityWantsEvent(gMeshPeers[i].mac, category)) {
      skippedCount++;
      continue;
    }
    attemptedCount++;
    bool sent = v4_send_frame(gMeshPeers[i].mac, type, flagsAuthed, msgId,
                              payload, payloadLen, ttl);
    if (sent) {
      anySuccess = true;
      sentCount++;
      if (tracker && tracker->expectedCount < BROADCAST_TRACKER_MAX_PEERS) {
        memcpy(tracker->peerMacs[tracker->expectedCount], gMeshPeers[i].mac, 6);
        tracker->ackReceived[tracker->expectedCount] = false;
        tracker->expectedCount++;
      }
    }
  }

  DEBUGF(DEBUG_ESPNOW_ROUTER,
         "[V4_BROADCAST_GATED] type=%u msgId=%lu category=0x%08lX sent=%d skipped=%d",
         type, (unsigned long)msgId, (unsigned long)category, sentCount, skippedCount);
  if (outAttempted) *outAttempted = attemptedCount;
  return anySuccess;
}

// Phase 3.5 task #51 — encrypted multi-frame send.
//
// Splits `payload` into fragments of up to ESPNOW_V4_MAX_PLAINTEXT bytes each
// and sends them as independent SESSION_FRAMEs sharing the same `msgId`. The
// outer header carries fragIndex / fragCount per fragment; each fragment gets
// its own AEAD seal (fresh frameSeq, fresh nonce). The receiver decrypts each
// fragment as it arrives and reassembles the plaintext slices into the final
// payload via the existing fragmentation reassembler.
//
// Why per-fragment seal instead of seal-then-chunk: AEAD is all-or-nothing —
// you can't open a partial ciphertext. Sealing once and slicing the cipher
// across frames would force the receiver to buffer everything before
// authenticating, which both delays detection of tampered fragments and
// requires a separate "deferred-auth" buffer pool. Per-fragment sealing
// reuses every existing knob (replay window, ACK retry, GC) and authenticates
// each frame on arrival.
//
// Single-frame fast path: if payloadLen ≤ MAX_PLAINTEXT we just delegate to
// v4_send_session_wrapped (no fragmentation overhead).
//
// Returns true only if every fragment was ACKed within retry budget.
bool v4_send_encrypted_chunked(const uint8_t dst[6], uint8_t type, uint16_t baseFlags,
                               uint32_t msgId,
                               const uint8_t* payload, uint16_t payloadLen,
                               uint8_t ttl) {
  if (!dst || (payloadLen > 0 && !payload)) return false;

  // Single-frame fast path — no need to fragment.
  if (payloadLen <= ESPNOW_V4_MAX_PLAINTEXT) {
    char err[64] = {0};
    return v4_send_session_wrapped(dst, type, baseFlags, msgId,
                                   payload, payloadLen, ttl, err, sizeof(err));
  }

  // fragIndex/fragCount are uint8_t in the header, so the wire caps us at
  // 255 fragments × 200 plaintext bytes = ~51 KB. CMD_RESP / STREAM /
  // METADATA_PUSH are the realistic callers — they should be well under this.
  //
  // Important: fragSize must equal V4_MAX_FRAGMENT_PAYLOAD (200) — the
  // existing reassembler uses that stride to compute buffer offsets.
  // Plaintext+encrypted-chunked are interleaved through the same buffer pool,
  // and the wire cipher is fragSize + 16 (AEAD tag) = 216 ≤ MAX_PAYLOAD (218).
  uint16_t fragSize = V4_MAX_FRAGMENT_PAYLOAD;
  uint32_t fragCountU = (payloadLen + fragSize - 1) / fragSize;
  if (fragCountU > 255) {
    WARN_ESPNOWF("[V4_ENC_FRAG_TX] payload too large: %u bytes requires %u frags (max 255)",
                 payloadLen, (unsigned)fragCountU);
    return false;
  }
  uint8_t fragCount = (uint8_t)fragCountU;

  // Look up session up-front so we fail fast if encryption isn't set up.
  const PeerIdentity* pid = peerIdentityFindByMac(dst);
  if (!pid) {
    WARN_ESPNOWF("[V4_ENC_FRAG_TX] no peer identity for dst — cannot encrypt");
    return false;
  }
  SessionState* s = sessionFindByPeer(dst, pid->meshId);
  if (!s || s->state != SESSION_ACTIVE) {
    WARN_ESPNOWF("[V4_ENC_FRAG_TX] no ACTIVE session for dst — cannot encrypt");
    return false;
  }

  char dstMac[18];
  formatMacAddressBuf(dst, dstMac, sizeof(dstMac));
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_ENC_FRAG_TX] ==============================");
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_ENC_FRAG_TX] Starting encrypted fragmented send to %s", dstMac);
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_ENC_FRAG_TX] msgId=%lu type=%u payloadLen=%u fragCount=%u",
         (unsigned long)msgId, type, payloadLen, fragCount);

  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);

  const uint8_t MAX_RETRIES = 3;
  const uint32_t ACK_TIMEOUT_MS = 200;

  uint16_t offset = 0;
  for (uint8_t fragIdx = 0; fragIdx < fragCount; fragIdx++) {
    uint16_t fragLen = (offset + fragSize <= payloadLen) ? fragSize
                                                          : (uint16_t)(payloadLen - offset);

    bool fragSent = false;
    for (uint8_t retry = 0; retry < MAX_RETRIES && !fragSent; retry++) {
      if (retry > 0) {
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_ENC_FRAG_TX] retry %u/%u for fragment %u/%u",
               retry + 1, MAX_RETRIES, fragIdx + 1, fragCount);
      }

      // Build the outer header for this fragment. sessionWrapFrame will
      // OR in SESSION_FRAME, write the AEAD tag, and stamp sessionId /
      // frameSeq from the session state.
      uint8_t frame[sizeof(EspNowV4Header) + ESPNOW_V4_MAX_PAYLOAD];
      EspNowV4Header* h = reinterpret_cast<EspNowV4Header*>(frame);
      memset(h, 0, sizeof(*h));
      h->magic           = (uint16_t)ESPNOW_V4_MAGIC;
      h->ver             = ESPNOW_V4_VERSION;
      h->type            = type;
      h->flags           = baseFlags | ESPNOW_V4_FLAG_ACK_REQ;  // per-fragment ACK
      h->headerLen       = (uint8_t)sizeof(EspNowV4Header);
      h->msgId           = msgId;
      memcpy(h->origin, myMac, 6);
      h->ttl             = ttl;
      h->fragIndex       = fragIdx;
      h->fragCount       = fragCount;
      h->meshFingerprint = fingerprintForPeer(dst);

      uint8_t* outPayload = frame + sizeof(EspNowV4Header);
      if (!sessionWrapFrame(s, h, payload + offset, fragLen, outPayload)) {
        WARN_ESPNOWF("[V4_ENC_FRAG_TX] AEAD seal failed for frag %u/%u — aborting",
                     fragIdx + 1, fragCount);
        return false;
      }

      // Allocate ACK wait slot — same machinery as plaintext fragmentation.
      // ACKs themselves are plaintext (TYPE_ACK opcode), matched by
      // (msgId, fragIndex). The encrypted-vs-plaintext distinction is
      // invisible at the ACK layer.
      V4FragAckWait* ackWait = v4_frag_ack_alloc(dst, msgId, fragIdx);
      if (!ackWait) {
        WARN_ESPNOWF("[V4_ENC_FRAG_TX] no ACK slot for frag %u/%u", fragIdx + 1, fragCount);
        return false;
      }
      ackWait->acked = false;
      ackWait->sentMs = millis();

      size_t totalLen = sizeof(EspNowV4Header) + fragLen + ESPNOW_V4_AEAD_TAG_LEN;
      captureEspNowFrame("TX", dst, 0, frame, (int)totalLen);
      esp_err_t rc = esp_now_send(dst, frame, totalLen);
      if (rc != ESP_OK) {
        WARN_ESPNOWF("[V4_ENC_FRAG_TX] esp_now_send rc=%d for frag %u/%u (retry %u)",
                     (int)rc, fragIdx + 1, fragCount, retry);
        { EspNowTxGuard g("fragAckFree"); ackWait->active = false; }  // free under lock; delay outside
        vTaskDelay(pdMS_TO_TICKS(50 * (retry + 1)));
        continue;
      }
      if (gEspNow) { gEspNow->routerMetrics.v4FragTx++; }

      uint32_t waitStart = millis();
      while ((millis() - waitStart) < ACK_TIMEOUT_MS) {
        if (ackWait->acked) {
          fragSent = true;
          DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_ENC_FRAG_TX] ✓ frag %u/%u ACK in %lu ms",
                 fragIdx + 1, fragCount, (unsigned long)(millis() - waitStart));
          break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      { EspNowTxGuard g("fragAckFree"); ackWait->active = false; }  // free under lock (poll above was lock-free)

      if (!fragSent) {
        WARN_ESPNOWF("[V4_ENC_FRAG_TX] ✗ frag %u/%u ACK timeout (retry %u)",
                     fragIdx + 1, fragCount, retry);
      }
    }

    if (!fragSent) {
      WARN_ESPNOWF("[V4_ENC_FRAG_TX] FAILED: frag %u/%u no ACK after %u retries — aborting send",
                   fragIdx + 1, fragCount, MAX_RETRIES);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_ENC_FRAG_TX] ==============================");
      return false;
    }

    offset += fragLen;
  }

  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_ENC_FRAG_TX] ✓ SUCCESS: all %u fragments sent + ACKed", fragCount);
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_ENC_FRAG_TX] ==============================");
  // A fragmented send is ACKed per-fragment (the waiter above), which never
  // reaches the broadcast-tracker ACK path that flips the web UI's delivery
  // bubble. Mark it delivered here so a long (chunked) message shows
  // ✓✓ Delivered instead of a false "No ACK (timeout)". No-op for sends that
  // weren't registered via sendStatusRegister (only user TEXT sends are).
  sendStatusMarkDelivered(msgId, dst);
  return true;
}

// Encrypted dispatcher — STRICT encrypt-or-fail (2026-05, encrypt-or-wait
// foundation complete). No plaintext fallback exists anywhere in this path.
// Mesh membership gates encryption (KEY_EX HMAC requires the mesh's bootstrap
// key); per-peer sessions provide confidentiality. If neither is achievable,
// the send fails — callers retry, log, or surface to the user.
//
// Routing:
//   - Single-frame (≤ MAX_PLAINTEXT) → v4_send_encrypted_or_queue. That helper
//     auto-handles every "no session / no peer identity" case: queue + kick
//     KEY_EX (if no identity) or SESSION_OPEN (if no session), drain on
//     SESSION_CONFIRM. So single-frame sends to same-mesh peers always succeed
//     eventually (queue + handshake + drain) and fail fast for cross-mesh
//     peers (KEY_EX HMAC mismatch → no REPLY → timeout sweep eats the frame).
//   - Multi-frame (> MAX_PLAINTEXT) → v4_send_encrypted_chunked. Requires an
//     ACTIVE session up-front (the chunked path doesn't queue across handshake
//     completion). Caller is responsible for ensuring a session exists or
//     retrying after one establishes — typical pattern is to do a single-frame
//     send first (which auto-handshakes) and then issue the multi-frame.
//
// Failure modes that return false:
//   - invalid args (null dst, missing payload)
//   - payload > MAX_PLAINTEXT with no peer identity or no active session
//   - encryption hard error (queue full, AEAD seal failed)
//
// Cross-mesh peers: KEY_EX HMAC fails on the receiver, no REPLY comes back,
// the pending frame ages out. From the sender's API perspective it returns true
// (queued) but never delivers. That's correct — different mesh = locked out.
bool v4_send_payload_smart(const uint8_t* dst, uint8_t type, uint16_t flags,
                           uint32_t msgId,
                           const uint8_t* payload, uint16_t payloadLen,
                           uint8_t ttl) {
  if (!dst) return false;

  if (payloadLen <= ESPNOW_V4_MAX_PLAINTEXT) {
    char status[64] = {0};
    bool ok = v4_send_encrypted_or_queue(dst, type, flags, msgId, payload, payloadLen,
                                          ttl, status, sizeof(status));
    if (!ok) {
      DEBUGF(DEBUG_ESPNOW_CORE,
             "[V4_TX] smart-send: encrypted-or-queue HARD-FAILED (%s) for type=%u msgId=%lu",
             status[0] ? status : "no detail", type, (unsigned long)msgId);
    }
    return ok;
  }

  // Multi-frame: requires ACTIVE session — no queueing for multi-frame today.
  const PeerIdentity* pid = peerIdentityFindByMac(dst);
  if (pid) {
    SessionState* s = sessionFindByPeer(dst, pid->meshId);
    if (s && s->state == SESSION_ACTIVE) {
      if (v4_send_encrypted_chunked(dst, type, flags, msgId, payload, payloadLen, ttl)) {
        return true;
      }
      DEBUGF(DEBUG_ESPNOW_CORE,
             "[V4_TX] smart-send: encrypted-chunked FAILED for type=%u msgId=%lu (%u bytes)",
             type, (unsigned long)msgId, payloadLen);
      return false;
    }
  }
  // No identity, or identity but no ACTIVE session. Kick whichever level is
  // missing so the next attempt has a chance, then fail this send so the caller
  // knows to retry.
  if (!pid) {
    (void)espnowKeyExInitiate(dst, nullptr);
  } else {
    (void)espnowSessionOpenInitiate(dst, nullptr);
  }
  DEBUGF(DEBUG_ESPNOW_CORE,
         "[V4_TX] smart-send: multi-frame send to %02X:%02X:%02X:%02X:%02X:%02X requires "
         "ACTIVE session (type=%u msgId=%lu, %u bytes) — kicked %s, dropping. Caller should retry.",
         dst[0], dst[1], dst[2], dst[3], dst[4], dst[5],
         type, (unsigned long)msgId, payloadLen,
         pid ? "SESSION_OPEN" : "KEY_EX");
  return false;
}

static bool v4_send_ack(const uint8_t* dst, uint32_t ackFor) {
  char dstMac[18];
  formatMacAddressBuf(dst, dstMac, sizeof(dstMac));
  DEBUGF(DEBUG_ESPNOW_CORE, "[V4_ACK_TX] Sending ACK to %s for msgId=%lu", dstMac, (unsigned long)ackFor);
  bool result = v4_send_frame(dst, ESPNOW_V4_TYPE_ACK, 0, ackFor, nullptr, 0, 1);
  DEBUGF(DEBUG_ESPNOW_CORE, "[V4_ACK_TX] Result: %s", result ? "SUCCESS" : "FAILED");
  return result;
}

// Phase 4: tell a sender that a file transfer it started will not complete.
// The sender is fire-and-forget (it returns "sent" once frames are handed off,
// without waiting for completion), so this is a post-hoc failure notice keyed
// by the original transfer's msgId (echoed in the header). Plaintext control
// frame, mirroring v4_send_ack — it reveals only "a transfer failed" + reason.
static bool v4_send_file_cancel(const uint8_t* dst, uint32_t msgId, uint8_t reason) {
  V4PayloadFileCancel p;
  p.reason = reason;
  return v4_send_frame(dst, ESPNOW_V4_TYPE_FILE_CANCEL, 0, msgId,
                       (const uint8_t*)&p, sizeof(p), 1);
}

// Send ACK for a specific fragment
static bool v4_send_frag_ack(const uint8_t* dst, uint32_t msgId, uint8_t fragIndex, uint8_t fragCount) {
  char dstMac[18];
  formatMacAddressBuf(dst, dstMac, sizeof(dstMac));
  DEBUGF(DEBUG_ESPNOW_CORE, "[V4_FRAG_ACK_TX] Sending fragment ACK to %s", dstMac);
  DEBUGF(DEBUG_ESPNOW_CORE, "[V4_FRAG_ACK_TX] msgId=%lu fragIdx=%u fragCnt=%u",
         (unsigned long)msgId, fragIndex, fragCount);
  
  // Build ACK frame with fragment info in header
  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  
  EspNowV4Header h = {};
  h.magic = (uint16_t)ESPNOW_V4_MAGIC;
  h.ver = ESPNOW_V4_VERSION;
  h.type = ESPNOW_V4_TYPE_ACK;
  h.flags = 0;
  h.headerLen = (uint8_t)sizeof(EspNowV4Header);
  h.msgId = msgId;
  memcpy(h.origin, myMac, 6);
  h.ttl = 1;
  h.fragIndex = fragIndex;
  h.fragCount = fragCount;
  h.meshFingerprint = fingerprintForPeer(dst);  // Phase 2: ACK rides the requester's mesh
  h.crc16 = 0;

  DEBUGF(DEBUG_ESPNOW_CORE, "[V4_FRAG_ACK_TX] Calling esp_now_send: headerLen=%u", (unsigned)sizeof(h));
  captureEspNowFrame("TX", dst, 0, (const uint8_t*)&h, (int)sizeof(h));
  esp_err_t result = esp_now_send(dst, (uint8_t*)&h, sizeof(h));
  
  if (result == ESP_OK) {
    DEBUGF(DEBUG_ESPNOW_CORE, "[V4_FRAG_ACK_TX] ✓ Fragment ACK sent successfully");
  } else {
    DEBUGF(DEBUG_ESPNOW_CORE, "[V4_FRAG_ACK_TX] ✗ Fragment ACK send failed: esp_err=%d", result);
  }
  
  return (result == ESP_OK);
}

// V3 sender functions for mesh system messages

// TIME_SYNC broadcast — 2026-05-19: route through v4_broadcast so BROADCAST_AUTH
// HMAC is appended; receiver verifies group-key knowledge before applying the
// clock update. Previously this called v4_send_time_sync(FF:...) which just
// sent plaintext to the broadcast address — completely spoofable.
static bool v4_broadcast_time_sync(uint32_t epochTime, int32_t timeOffset) {
  V4PayloadTimeSync payload;
  payload.epochTime = epochTime;
  payload.timeOffset = timeOffset;
  uint32_t msgId = generateMessageId();
  DEBUGF(DEBUG_ESPNOW_MESH, "[V4_TX_TIME_SYNC] Broadcasting (BROADCAST_AUTH) msgId=%lu", (unsigned long)msgId);
  return v4_broadcast(ESPNOW_V4_TYPE_TIME_SYNC, 0, msgId,
                      (const uint8_t*)&payload, sizeof(payload), 2);
}

static bool v4_send_topo_request(const uint8_t* dst, uint32_t reqId) {
  V4PayloadTopoReq payload;
  payload.reqId = reqId;
  memset(payload.reserved, 0, sizeof(payload.reserved));
  
  uint32_t msgId = generateMessageId();
  char dstMac[18];
  formatMacAddressBuf(dst, dstMac, sizeof(dstMac));
  DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_TX_TOPO_REQ] Sending to %s msgId=%lu reqId=%lu",
         dstMac, (unsigned long)msgId, (unsigned long)reqId);
  bool result = v4_send_frame(dst, ESPNOW_V4_TYPE_TOPO_REQ, 0, msgId, (const uint8_t*)&payload, sizeof(payload), 2);
  DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_TX_TOPO_REQ] Result: %s", result ? "SUCCESS" : "FAILED");
  return result;
}

bool v4_broadcast_topo_request(uint32_t reqId) {
  DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_BROADCAST_TOPO_REQ] Broadcasting reqId=%lu", (unsigned long)reqId);
  uint8_t broadcastMac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  return v4_send_topo_request(broadcastMac, reqId);
}

// ===========================================================================
// Bond confidentiality (2026-05-21): every bond unicast frame rides an AEAD
// SESSION_FRAME. Two rules live in bondSendEncrypted():
//   1) SINGLE INITIATOR — the higher-MAC device initiates SESSION_OPEN; the
//      lower-MAC device only responds. This is anchored on the MAC comparison
//      (sessionIsASide), NOT on the mutable bond master/worker role — so a
//      misconfigured two-master or two-worker pair can never both initiate and
//      trigger the broken simultaneous-open (which leaves two mismatched
//      sessions that can't decrypt each other). Exactly one side ever initiates.
//   2) ENCRYPT-OR-WAIT — session ACTIVE: send encrypted. No session + I am the
//      initiator: kick SESSION_OPEN (single-frame queue path). No session + I am
//      the responder: skip this send; the initiator will bring the session up.
// Receivers enforce the matching rule via V4_OPC_FLAG_REQ_SESSION_ENC.

// Async sibling of bondSendEncrypted. Same role + initiator-gating rules, but
// the actual AEAD seal / capture / esp_now_send happens on espnow_tx instead
// of the caller's stack. Returns true if the job was queued, false if:
//   * we're the responder + no session (same silent-skip as bondSendEncrypted)
//   * ps_alloc failed (out of PSRAM)
//   * espnow_tx queue is full (drop, log)
//
// Caller must NOT free `payload` — this helper owns the lifetime semantics
// internally (copies to a fresh PSRAM buffer; espnow_tx frees that copy).
// On a false return there is no leak.
//
// Used by: sensor_bcast → sendBondedSensorData; the super-loop bond stages
// 9b/9j/9k + heartbeat + CAP/MANIFEST/STATUS (Step 2); the streaming-setup
// path (Step 2 tight-fix: sendBondStreamCtrl, firePostSyncSideEffects);
// requestBondSettings + requestBondSchema (Step 2/3a); the bond CLI commands
// (Step 3a: cmd_bond_requestcap/manifest/settings/schema and cmd_bond_resync's
// worker branch). The sync bondSendEncrypted above is now an unused reference
// implementation kept for Step 6 cleanup (along with EspNowTxGuard deletion).
static bool bondSendEncryptedAsync(const uint8_t* mac, uint8_t type, uint16_t flags,
                                   uint32_t msgId, const uint8_t* payload, uint16_t len) {
  if (!gEspNow || !mac) return false;
  uint8_t selfMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, selfMac);
  const bool iAmInitiator = !sessionIsASide(selfMac, mac);
  const PeerIdentity* pid = peerIdentityFindByMac(mac);
  SessionState* s = pid ? sessionFindByPeer(mac, pid->meshId) : nullptr;
  const bool active = (s && s->state == SESSION_ACTIVE);
  if (!active && !iAmInitiator) {
    // Same silent-skip as the sync sibling: responder without an active session
    // never initiates. Caller treats this as a "drop this frame, not an error."
    return false;
  }

  // Copy payload to a fresh PSRAM-backed buffer. The job descriptor in the
  // queue carries only the pointer; espnow_tx frees after send.
  uint8_t* psBuf = nullptr;
  if (len > 0) {
    psBuf = (uint8_t*)ps_alloc((size_t)len, AllocPref::PreferPSRAM, "espnow.tx.payload");
    if (!psBuf) {
      DEBUGF(DEBUG_ESPNOW_CORE, "[BOND_TX_ASYNC] ps_alloc failed len=%u", (unsigned)len);
      return false;
    }
    memcpy(psBuf, payload, len);
  }

  espnowtx::Job job = {};
  job.kind       = espnowtx::JOB_AEAD_SMART;
  memcpy(job.peerMac, mac, 6);
  job.type       = type;
  job.flags      = flags;
  job.msgId      = msgId;
  job.ttl        = 1;
  job.payloadLen = len;
  job.payload    = psBuf;

  if (!espnowtx::submit(job)) {
    // Queue full — drop the frame and free the PSRAM copy we just made.
    if (psBuf) free(psBuf);
    return false;
  }
  return true;
}

// True only when an encrypted session with `mac` is currently ACTIVE on THIS
// device. Used to gate the master's bond-sync sequence: firing CAP/MANIFEST/
// SETTINGS requests before our own session is ACTIVE just parks them in the
// single-frame queue (initiator path), where they can be overwritten/swept
// before the session opens — burning the retry budget for nothing. The master
// is the session initiator (higher MAC), so by the time OUR session is ACTIVE
// the worker (responder) has already been ACTIVE since slightly earlier and can
// decrypt our first request. Gating here makes the first request land for real.
static bool bondSessionActiveWith(const uint8_t* mac) {
  if (!gEspNow || !mac) return false;
  const PeerIdentity* pid = peerIdentityFindByMac(mac);
  SessionState* s = pid ? sessionFindByPeer(mac, pid->meshId) : nullptr;
  return (s && s->state == SESSION_ACTIVE);
}

// Invoked from the session layer (cmd_exec_task) the moment a session with
// `peerMac` reaches ACTIVE — whether we were initiator (got CONFIRM) or
// responder (completed OPEN). Replaces the old "plaintext heartbeat == peer
// online" discovery: the encrypted session itself is now the liveness signal.
// The master kicks the capability exchange here. Filtered to the bonded peer so
// sessions opened for non-bond reasons don't spuriously start a bond sync.
void bondNotifySessionEstablished(const uint8_t* peerMac) {
#if ENABLE_BONDED_MODE
  if (!gEspNow || !gSettings.bondModeEnabled || !peerMac) return;
  uint8_t bm[6];
  if (!parseMacAddress(gSettings.bondPeerMac, bm) || memcmp(bm, peerMac, 6) != 0) return;
  bool wasOffline = !gEspNow->bondPeerOnline;
  gEspNow->bondPeerOnline = true;
  gEspNow->lastBondHeartbeatReceivedMs = millis();  // session ACTIVE counts as liveness
  if (wasOffline) {
#if ENABLE_HTTP_SERVER
    // Security audit: the bonded (RCE-capable) command channel just went live with
    // the configured peer. The bond token was derived during the encrypted
    // handshake; this transition is the point command-execution over the bond
    // becomes usable. One line per session (guarded by wasOffline, not per
    // heartbeat) and already filtered to the configured bond peer above. Fires on
    // both master and worker. Runs on cmd_exec_task — safe for the log file write.
    extern void logAuthAttempt(bool, const char*, const String&, const String&, const String&);
    String bondWho = getEspNowDeviceName(peerMac);
    if (bondWho.length() == 0) bondWho = formatMacAddress(peerMac);
    logAuthAttempt(true, "espnow/bond", bondWho, String("espnow"),
                   String("Bond session active (role=") +
                       (isBondMaster() ? "master" : "worker") + ")");
#endif
    if (isBondMaster()) {
      gEspNow->bondNeedsCapabilityRequest = true;
      INFO_ESPNOWF("[BOND] session ACTIVE with peer — master kicking capability sync");
    }
  }
#else
  (void)peerMac;
#endif
}

// (v4_send_worker_status removed 2026-05-21: WORKER_STATUS opcode 83 has no
// receive handler in kV4HandlerTable and the sender had zero callers — fully
// dead path. Reintroduce a sender only alongside a real RX handler.)

static bool v4_send_topo_start(const uint8_t* dst, uint32_t reqId, uint8_t peerCount) {
  V4PayloadTopoStart payload;
  payload.reqId = reqId;
  payload.peerCount = peerCount;
  memset(payload.reserved, 0, sizeof(payload.reserved));
  
  uint32_t msgId = generateMessageId();
  return v4_send_frame(dst, ESPNOW_V4_TYPE_TOPO_START, 0, msgId, (const uint8_t*)&payload, sizeof(payload), 2);
}

static bool v4_send_topo_peer(const uint8_t* dst, uint32_t reqId, uint8_t peerIndex, 
                               bool isLast, const uint8_t* peerMac, int8_t rssi, 
                               bool encrypted, const char* peerName) {
  V4PayloadTopoPeer payload;
  payload.reqId = reqId;
  payload.peerIndex = peerIndex;
  payload.isLast = isLast ? 1 : 0;
  memcpy(payload.mac, peerMac, 6);
  payload.rssi = rssi;
  payload.encrypted = encrypted ? 1 : 0;
  strncpy(payload.name, peerName, sizeof(payload.name) - 1);
  payload.name[sizeof(payload.name) - 1] = '\0';
  
  uint32_t msgId = generateMessageId();
  return v4_send_frame(dst, ESPNOW_V4_TYPE_TOPO_PEER, 0, msgId, (const uint8_t*)&payload, sizeof(payload), 2);
}

// Broadcast sensor status to all mesh peers
bool v4_broadcast_sensor_status(RemoteSensorType sensorType, bool enabled) {
  V4PayloadSensorStatus payload;
  payload.sensorType = (uint8_t)sensorType;
  payload.enabled = enabled ? 1 : 0;
  memset(payload.reserved, 0, sizeof(payload.reserved));
  
  uint32_t msgId = generateMessageId();
  extern const char* sensorTypeToString(RemoteSensorType type);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V4_BROADCAST_SENSOR_STATUS] Broadcasting msgId=%lu", (unsigned long)msgId);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V4_BROADCAST_SENSOR_STATUS] sensor=%s enabled=%s",
         sensorTypeToString(sensorType), enabled ? "YES" : "NO");
  // Phase 5: gate on per-peer SENSOR subscription.
  bool result = v4_broadcast_category(ESPNOW_V4_TYPE_SENSOR_STATUS, 0, msgId,
                                       (const uint8_t*)&payload, sizeof(payload), 2,
                                       ESPNOW_EVT_SENSOR);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V4_BROADCAST_SENSOR_STATUS] Result: %s", result ? "SUCCESS" : "FAILED");
  return result;
}

// Broadcast sensor data to all mesh peers
bool v4_broadcast_sensor_data(RemoteSensorType sensorType, const char* jsonData, uint16_t jsonLen, int* outAttempted) {
  if (outAttempted) *outAttempted = 0;
  if (!jsonData || jsonLen == 0 || jsonLen > 200) {
    DEBUGF(DEBUG_ESPNOW_MESH, "[V4_BROADCAST_SENSOR_DATA] ERROR: Invalid params (jsonData=%p len=%u)",
           jsonData, jsonLen);
    return false;
  }
  
  // Build payload: struct + JSON data
  uint8_t buffer[256];
  V4PayloadSensorBroadcast* payload = (V4PayloadSensorBroadcast*)buffer;
  payload->sensorType = (uint8_t)sensorType;
  payload->dataLen = jsonLen;
  memcpy(payload->data, jsonData, jsonLen);
  
  uint16_t totalLen = sizeof(V4PayloadSensorBroadcast) + jsonLen;
  uint32_t msgId = generateMessageId();
  extern const char* sensorTypeToString(RemoteSensorType type);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V4_BROADCAST_SENSOR_DATA] Broadcasting msgId=%lu", (unsigned long)msgId);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V4_BROADCAST_SENSOR_DATA] sensor=%s jsonLen=%u totalLen=%u",
         sensorTypeToString(sensorType), jsonLen, totalLen);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V4_BROADCAST_SENSOR_DATA] JSON (first 80 chars): %.80s", jsonData);
  // Phase 5: gate on per-peer SENSOR subscription.
  int attempted = 0;
  bool result = v4_broadcast_category(ESPNOW_V4_TYPE_SENSOR_BROADCAST, 0, msgId,
                                       buffer, totalLen, 2, ESPNOW_EVT_SENSOR, &attempted);
  if (outAttempted) *outAttempted = attempted;
  DEBUGF(DEBUG_ESPNOW_MESH, "[V4_BROADCAST_SENSOR_DATA] Result: %s",
         result ? "SUCCESS" : (attempted == 0 ? "NO LIVE PEERS" : "FAILED"));
  return result;
}

// Send user sync to specific device (JSON payload)
// Phase 3.5 task #6 — helper functions for app-unicast opcodes now route
// through v4_send_encrypted_or_queue: wraps in SESSION_FRAME if a session
// exists, queues + auto-kicks SESSION_OPEN otherwise. Returns false if the
// peer has no long-term identity yet (KEY_EX never completed) — strict
// rather than silently falling back to plaintext, since these opcodes can
// carry credentials (CMD payload format is "user:pass:command").

bool v4_send_user_sync(const uint8_t* dst, const char* jsonPayload, uint16_t jsonLen) {
  if (!jsonPayload || jsonLen == 0) {
    DEBUGF(DEBUG_ESPNOW_MESH, "[V4_TX_USER_SYNC] ERROR: Invalid params (jsonPayload=%p len=%u)",
           jsonPayload, jsonLen);
    return false;
  }
  uint32_t msgId = generateMessageId();
  char dstMac[18];
  formatMacAddressBuf(dst, dstMac, sizeof(dstMac));
  DEBUGF(DEBUG_ESPNOW_MESH, "[V4_TX_USER_SYNC] Sending (encrypted) to %s msgId=%lu len=%u", dstMac, (unsigned long)msgId, jsonLen);
  // USER_SYNC carries user records (possibly hashed credentials). Smart is
  // now strict encrypt-or-fail (2026-05 cleanup) — no plaintext fallback — so
  // routing through it gets the right semantics for free. Smart also auto-kicks
  // KEY_EX/SESSION_OPEN if needed (encrypt-or-wait foundation).
  bool result = v4_send_payload_smart(dst, ESPNOW_V4_TYPE_USER_SYNC, ESPNOW_V4_FLAG_ACK_REQ,
                                       msgId, (const uint8_t*)jsonPayload, jsonLen, 3);
  DEBUGF(DEBUG_ESPNOW_MESH, "[V4_TX_USER_SYNC] Result: %s", result ? "SUCCESS" : "FAILED");
  return result;
}

bool v4_send_text(const uint8_t* dst, const char* text, uint16_t textLen) {
  if (!text || textLen == 0) return false;
  uint32_t msgId = generateMessageId();
  char dstMac[18];
  formatMacAddressBuf(dst, dstMac, sizeof(dstMac));
  DEBUGF(DEBUG_ESPNOW_CORE, "[V4_TX_TEXT] Sending (encrypted) to %s msgId=%lu len=%u", dstMac, (unsigned long)msgId, textLen);
  // Smart is now strict encrypt-or-fail (2026-05 cleanup) — no plaintext
  // fallback anywhere. v4_send_text's original "refuse if not encryptable"
  // contract holds for free now. Smart also auto-handshakes for unknown peers.
  bool result = v4_send_payload_smart(dst, ESPNOW_V4_TYPE_TEXT, ESPNOW_V4_FLAG_ACK_REQ,
                                       msgId, (const uint8_t*)text, textLen, 1);
  DEBUGF(DEBUG_ESPNOW_CORE, "[V4_TX_TEXT] Result: %s", result ? "SUCCESS" : "FAILED");
  return result;
}

bool v4_send_command_response(const uint8_t* dst, uint32_t cmdMsgId, bool success, const char* resultText, uint16_t textLen) {
  if (!resultText || textLen == 0) return false;

  // Build response payload: success byte + result text
  uint16_t totalLen = 1 + textLen;
  char dstMac[18];
  formatMacAddressBuf(dst, dstMac, sizeof(dstMac));

  // BEST-EFFORT encrypt — CMD_RESP carries the command's output, which is
  // typically less sensitive than the CMD request itself (which carries
  // credentials). v4_send_payload_smart prefers encrypted (single-frame or
  // encrypted-chunked) and falls back to plaintext if no session is up — so
  // the response still gets to the requester even between un-handshaked
  // peers. Buffer sized to the worst case (totalLen could exceed
  // MAX_PLAINTEXT for large outputs; smart_send handles the fragmentation).
  uint8_t* buffer = (uint8_t*)malloc(totalLen);
  if (!buffer) return false;
  buffer[0] = success ? 1 : 0;
  memcpy(buffer + 1, resultText, textLen);
  DEBUGF(DEBUG_ESPNOW_CORE, "[V4_TX_CMD_RESP] Sending to %s cmdMsgId=%lu len=%u (encrypt-preferred)",
         dstMac, (unsigned long)cmdMsgId, totalLen);
  bool result = v4_send_payload_smart(dst, ESPNOW_V4_TYPE_CMD_RESP, ESPNOW_V4_FLAG_ACK_REQ,
                                       cmdMsgId, buffer, totalLen, 1);
  free(buffer);
  DEBUGF(DEBUG_ESPNOW_CORE, "[V4_TX_CMD_RESP] Result: %s", result ? "SUCCESS" : "FAILED");
  return result;
}


bool v4_broadcast_text(const char* text, uint16_t textLen) {
  if (!text || textLen == 0 || textLen > ESPNOW_V4_MAX_PAYLOAD) return false;
  
  uint32_t msgId = generateMessageId();
  DEBUGF(DEBUG_ESPNOW_CORE, "[V4_BROADCAST_TEXT] Broadcasting msgId=%lu len=%u", (unsigned long)msgId, textLen);
  
  bool result = v4_broadcast(ESPNOW_V4_TYPE_TEXT, 0, msgId, (const uint8_t*)text, textLen, 2);
  DEBUGF(DEBUG_ESPNOW_CORE, "[V4_BROADCAST_TEXT] Result: %s", result ? "SUCCESS" : "FAILED");
  return result;
}

// Forward declarations for static functions defined later in this file
static TopologyStream* findTopoStream(const uint8_t* senderMac, uint32_t reqId);
static TopologyStream* findOrCreateTopoStream(const uint8_t* senderMac, uint32_t reqId);
static void            addTopoDeviceName(const uint8_t* mac, const char* name);
static bool            getTopoDeviceName(const uint8_t* mac, char* outBuf, size_t outLen);
static void            finalizeTopologyStream(TopologyStream* stream);
#if ENABLE_BONDED_MODE
static bool            cacheManifestToLittleFS(const uint8_t fwHash[16], const String& manifest);
#endif
#if ENABLE_BONDED_MODE
static void            processBondSettings(const uint8_t* srcMac, const String& deviceName, const String& settingsStr);
static void            processBondSchema(const uint8_t* srcMac, const String& deviceName, const String& schemaStr);
static void            requestBondSettings(const uint8_t* peerMac);
#endif
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
static void v4_handle_cmd(const uint8_t* srcMac, const char* deviceName, uint32_t msgId, const char* cmd, bool wasSessionEncrypted);

// ============================================================================
// V3 RX dispatch table (Phase 0 of docs/ESPNOW_V4_PLAN.md).
//
// The legacy if-ladder in v4_try_handle_incoming() is being replaced
// incrementally by a handler table. For each opcode in this table, the
// corresponding if-branch in v4_try_handle_incoming() has been removed —
// the dispatcher below runs first and claims the frame.
//
// New opcodes get migrated by:
//   1. Implementing a `static void v4h_<name>(const V4RxCtx&)` function.
//   2. Adding one row to kV4HandlerTable[].
//   3. Deleting the corresponding `if (h->type == ESPNOW_V4_TYPE_<X>)` branch
//      from v4_try_handle_incoming().
//
// The dispatcher preserves the exact behavior of the original branches,
// including the per-branch reassembly cleanup that ran at the end of each.
// That cleanup is centralized in the dispatcher so handlers don't repeat it.
// ============================================================================

struct V4RxCtx {
  const esp_now_recv_info* recv_info;
  const EspNowV4Header*    h;
  const uint8_t*           payload;
  uint16_t                 payloadLen;
  bool                     isPaired;
  // 2026-05-19: `isAuthenticated` is true when this frame proves the sender
  // holds either our session key (SESSION_FRAME unwrap succeeded) or the mesh
  // group key (BROADCAST_AUTH HMAC verified). Plaintext unicast and plain-
  // broadcast (no BROADCAST_AUTH tag) leave it false. Handlers that mutate
  // device-level state from the frame (clock, master role, peer identity
  // claims) MUST gate on this — otherwise anyone in radio range can spoof.
  bool                     isAuthenticated;
  // `isSessionEncrypted` is the narrower signal: true ONLY when the frame
  // arrived AEAD-wrapped in a SESSION_FRAME (confidential). A BROADCAST_AUTH
  // frame is authenticated-but-plaintext, so it sets isAuthenticated=true but
  // isSessionEncrypted=false. Use this (not the legacy, now-never-set
  // ESPNOW_V4_FLAG_ENCRYPTED header bit) for any "was this encrypted?" report.
  bool                     isSessionEncrypted;
  const char*              deviceName;
};

using V4OpcodeHandler = void (*)(const V4RxCtx& ctx);

// Flags on handler-table entries
static constexpr uint8_t V4_OPC_FLAG_REQ_PAIRED        = 0x01;  // require src to be a paired peer
static constexpr uint8_t V4_OPC_FLAG_REQ_BOND_MODE     = 0x02;  // require gSettings.bondModeEnabled
// Require the frame to be authenticated (came in via SESSION_FRAME unwrap or
// BROADCAST_AUTH HMAC verify). Drops plaintext unicast and plain broadcasts
// silently. Used by opcodes whose handler mutates device-level state from
// payload contents (e.g., TIME_SYNC moves the clock).
static constexpr uint8_t V4_OPC_FLAG_REQ_AUTHENTICATED = 0x04;
// Require the frame to have arrived AEAD-wrapped in a SESSION_FRAME (confidential
// + per-peer authenticated). Stricter than REQ_AUTHENTICATED, which also accepts
// BROADCAST_AUTH (authenticated-but-plaintext). Used by every bond opcode: bond
// traffic carries the @BOND token, sensor data, and privileged state, so it must
// be confidential AND unforgeable. Senders route through bondSendEncrypted, so a
// legitimate bond frame is always encrypted; plaintext bond frames are dropped.
static constexpr uint8_t V4_OPC_FLAG_REQ_SESSION_ENC   = 0x08;

struct V4OpcodeEntry {
  uint8_t          opcode;
  uint8_t          flags;
  V4OpcodeHandler  handler;
};

// ----- Per-opcode handlers (Phase 0 migrations) -----

// TIME_SYNC — synchronous, updates gTimeOffset / gTimeIsSynced.
static void v4h_time_sync(const V4RxCtx& ctx) {
  DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_TIME_SYNC] Received from %s msgId=%lu payloadLen=%u",
         ctx.deviceName, (unsigned long)ctx.h->msgId, ctx.payloadLen);
  if (ctx.payloadLen >= sizeof(V4PayloadTimeSync)) {
    const V4PayloadTimeSync* ts = (const V4PayloadTimeSync*)ctx.payload;
    DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_TIME_SYNC] epochTime=%lu timeOffset=%ld",
           (unsigned long)ts->epochTime, (long)ts->timeOffset);
    DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_TIME_SYNC] Previous time state: synced=%s offset=%lld",
           gTimeIsSynced ? "YES" : "NO", (long long)gTimeOffset);
    gTimeOffset = (int64_t)ts->timeOffset;
    gTimeIsSynced = true;
    DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_TIME_SYNC] Time sync applied successfully");
  } else {
    DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_TIME_SYNC] ERROR: Payload too small (%u < %u)",
           ctx.payloadLen, (unsigned)sizeof(V4PayloadTimeSync));
  }
}

// TEXT — deferred to task via ring buffer (gEspNow->textQueue).
static void v4h_text(const V4RxCtx& ctx) {
  // Bound the printf by both 80 chars AND the actual payloadLen — the
  // SESSION_FRAME unwrap path delivers a non-null-terminated buffer, so a
  // plain `%.80s` would scan past the valid bytes into adjacent stack memory.
  int printLen = ctx.payloadLen > 80 ? 80 : (int)ctx.payloadLen;
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_RX] TEXT message detected, payload: %.*s",
         ctx.payloadLen > 0 ? printLen : 7,
         ctx.payloadLen > 0 ? (const char*)ctx.payload : "(empty)");
  // Accept reassembled multi-frame text up to ESPNOW_TEXT_MAX_LEN — a long
  // espnowsend arrives here as one rebuilt payload (the reassembler is
  // type-agnostic), so the cap mirrors the sender's, not the single-frame size.
  if (ctx.payloadLen > 0 && ctx.payloadLen <= ESPNOW_TEXT_MAX_LEN && gEspNow) {
    int head = gEspNow->textQueueHead;
    int nextHead = (head + 1) & (EspNowState::TEXT_QUEUE_SIZE - 1);
    if (nextHead != gEspNow->textQueueTail) {
      auto& slot = gEspNow->textQueue[head];
      size_t copyLen = (ctx.payloadLen < sizeof(slot.content) - 1) ? ctx.payloadLen : sizeof(slot.content) - 1;
      memcpy(slot.content, ctx.payload, copyLen);
      slot.content[copyLen] = '\0';
      memcpy(slot.srcMac, ctx.recv_info->src_addr, 6);
      strncpy(slot.deviceName, ctx.deviceName, sizeof(slot.deviceName) - 1);
      slot.deviceName[sizeof(slot.deviceName) - 1] = '\0';
      // Reflect real transit confidentiality: a SESSION_FRAME-unwrapped TEXT
      // is encrypted; broadcast-auth / plaintext TEXT is not. (Was reading the
      // legacy ESPNOW_V4_FLAG_ENCRYPTED header bit, never set anymore, so the
      // web UI's message bubble always showed "not encrypted".)
      slot.encrypted = ctx.isSessionEncrypted;
      // A frame that arrived under the dedicated BOOT type is a device/system
      // notice, not chat — tag it so the drain stores MSG_SYSTEM_EVENT and clients
      // filter it out of the conversation (same class as the metadata snapshot).
      slot.msgType = (ctx.h->type == ESPNOW_V4_TYPE_BOOT) ? MSG_SYSTEM_EVENT : MSG_TEXT;
      // Carry the fragment tags so the drain can store this piece as its own
      // record. Single-frame text has fragCount==1 → stored as piece 1/1.
      slot.msgId     = ctx.h->msgId;
      slot.fragIndex = ctx.h->fragIndex;
      slot.fragCount = ctx.h->fragCount;
      slot.used = true;
      gEspNow->textQueueHead = nextHead;
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_RX] TEXT message enqueued slot=%d (encrypted=%s)",
             head, slot.encrypted ? "YES" : "NO");
      if (ctx.isPaired && meshEnabled()) {
        noteMeshPeerRxActivity(ctx.recv_info->src_addr, EspNowMeshRxKind::RxActivity);
      }
    } else {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_RX] TEXT queue full, message dropped");
    }
  } else {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_RX] TEXT message REJECTED: payloadLen=%u gEspNow=%p",
           ctx.payloadLen, gEspNow);
  }
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_RX] ========================================");
}

// CMD — deferred to task via gEspNow->deferredCmd* fields (one in flight).
// Auth/validation runs later in v4_handle_cmd() on the espnow task.
static void v4h_cmd(const V4RxCtx& ctx) {
  if (ctx.payloadLen > 0 && ctx.payloadLen <= ESPNOW_V4_MAX_PAYLOAD && gEspNow && !gEspNow->deferredCmdPending) {
    size_t copyLen = (ctx.payloadLen < sizeof(gEspNow->deferredCmdPayload) - 1)
                       ? ctx.payloadLen
                       : sizeof(gEspNow->deferredCmdPayload) - 1;
    memcpy(gEspNow->deferredCmdPayload, ctx.payload, copyLen);
    gEspNow->deferredCmdPayload[copyLen] = '\0';
    memcpy(gEspNow->deferredCmdSrcMac, ctx.recv_info->src_addr, 6);
    strncpy(gEspNow->deferredCmdDeviceName, ctx.deviceName, sizeof(gEspNow->deferredCmdDeviceName) - 1);
    gEspNow->deferredCmdDeviceName[sizeof(gEspNow->deferredCmdDeviceName) - 1] = '\0';
    gEspNow->deferredCmdMsgId = ctx.h->msgId;
    // Carry the confidentiality signal to v4_handle_cmd. Bond (@BOND token)
    // commands are rejected unless this is true — a plaintext token = RCE.
    gEspNow->deferredCmdWasEncrypted = ctx.isSessionEncrypted;
    gEspNow->deferredCmdPending = true;
  }
}

// CMD_RESP — deferred to task via gEspNow->deferredCmdResp* fields.
static void v4h_cmd_resp(const V4RxCtx& ctx) {
  if (ctx.payloadLen >= 1 && gEspNow) {
    const V4PayloadCmdResp* resp = (const V4PayloadCmdResp*)ctx.payload;
    size_t resultLen = ctx.payloadLen - 1;
    if (resultLen > 6143) resultLen = 6143;  // V4 fragmentation budget: 32 × 200 = 6400 B (matches V4_FRAG_MAX)
    if (!gEspNow->deferredCmdRespResult) { gEspNow->deferredCmdRespPending = false; return; }
    memcpy(gEspNow->deferredCmdRespResult, resp->result, resultLen);
    gEspNow->deferredCmdRespResult[resultLen] = '\0';
    memcpy(gEspNow->deferredCmdRespSrcMac, ctx.recv_info->src_addr, 6);
    strncpy(gEspNow->deferredCmdRespDeviceName, ctx.deviceName, sizeof(gEspNow->deferredCmdRespDeviceName) - 1);
    gEspNow->deferredCmdRespDeviceName[sizeof(gEspNow->deferredCmdRespDeviceName) - 1] = '\0';
    gEspNow->deferredCmdRespSuccess = resp->success;
    gEspNow->deferredCmdRespReqId = ctx.h->msgId;  // correlate this result back to the request that produced it
    gEspNow->deferredCmdRespPending = true;
  }
}

// HEARTBEAT — mesh peer heartbeat; updates rx activity, mesh-backup tracking,
// and sends ACK if requested. Note: ACK_REQ frames also get an auto-ACK from
// the early auto-ACK block in v4_try_handle_incoming; preserving the
// double-send here to match original behavior exactly.
// Layer-2 self-heal for the session pre-warm. A peer whose identity diverged
// (e.g. it was re-flashed/factory-erased) rejects our SESSION_OPEN forever; the
// heartbeat pre-warm would otherwise re-send one every ~5s indefinitely. Track
// per-peer fresh attempts; after kSessionPrewarmMaxFails with no ACTIVE session,
// back off for kSessionPrewarmCooldownMs (radio-silent) and emit ONE actionable
// WARN. Reset the moment a session reaches ACTIVE. Stays fully within the TOFU
// guard — never auto-re-keys and never auto-sends KEY_EX.
static constexpr uint8_t  kSessionPrewarmMaxFails   = 3;
static constexpr uint32_t kSessionPrewarmCooldownMs = 60000;
struct SessionPrewarm { uint8_t mac[6]; uint8_t fails; uint32_t cooldownUntilMs; bool used; };
static SessionPrewarm gSessionPrewarm[6] = {};

static SessionPrewarm* sessionPrewarmSlot(const uint8_t mac[6]) {
  int idx = -1, oldest = 0;
  for (int i = 0; i < 6; i++) {
    if (gSessionPrewarm[i].used && memcmp(gSessionPrewarm[i].mac, mac, 6) == 0) return &gSessionPrewarm[i];
    if (!gSessionPrewarm[i].used && idx < 0) idx = i;
    if (gSessionPrewarm[i].cooldownUntilMs < gSessionPrewarm[oldest].cooldownUntilMs) oldest = i;
  }
  if (idx < 0) idx = oldest;  // no match and no free slot — evict the stalest
  SessionPrewarm& s = gSessionPrewarm[idx];
  memcpy(s.mac, mac, 6); s.fails = 0; s.cooldownUntilMs = 0; s.used = true;
  return &s;
}
static void sessionPrewarmReset(const uint8_t mac[6]) {
  for (int i = 0; i < 6; i++) {
    if (gSessionPrewarm[i].used && memcmp(gSessionPrewarm[i].mac, mac, 6) == 0) {
      gSessionPrewarm[i].fails = 0;
      gSessionPrewarm[i].cooldownUntilMs = 0;
      return;
    }
  }
}

static void v4h_heartbeat(const V4RxCtx& ctx) {
  if (ctx.payloadLen >= sizeof(V4PayloadHeartbeat)) {
    const V4PayloadHeartbeat* hb = (const V4PayloadHeartbeat*)ctx.payload;
    noteMeshPeerRxActivity(ctx.recv_info->src_addr, EspNowMeshRxKind::MeshHeartbeat, hb->rssi);
    if (gEspNow) gEspNow->heartbeatsReceived++;

    // Proactive session pre-warm. If this heartbeat is from a peer we've ALREADY
    // paired with (a long-term identity exists) and we don't yet have an ACTIVE
    // session, kick SESSION_OPEN now so the encrypted channel is ready BEFORE the
    // first message. Without this, sessions are established lazily on first send —
    // and a multi-frame (long) message sent first gets dropped, because the
    // chunked transport requires an active session and can't queue mid-stream.
    // Gated on pid!=null so we ONLY auto-handshake with known peers (never
    // strangers heard via broadcast) and only ever send SESSION_OPEN, never
    // KEY_EX (no passphrase material on the wire). espnowSessionOpenInitiate is
    // non-blocking and a no-op while a handshake is in flight.
    //
    // Self-heal (Layer 2): if the peer's identity diverged it rejects our OPEN,
    // which would loop forever — so a per-peer attempt table bounds it (N fresh
    // tries, then a radio-silent cooldown + one actionable WARN). A fresh attempt
    // only counts when there is NO session AND none ESTABLISHING (a live handshake
    // is left to run); a session reaching ACTIVE clears the backoff.
    {
      const PeerIdentity* pid = peerIdentityFindByMac(ctx.recv_info->src_addr);
      if (pid) {
        const uint8_t* pmac = ctx.recv_info->src_addr;
        SessionState* s = sessionFindByPeer(pmac, pid->meshId);
        if (s && s->state == SESSION_ACTIVE) {
          sessionPrewarmReset(pmac);                 // session up - clear backoff
        } else if (s && (s->state == SESSION_ESTABLISHING || s->state == SESSION_REKEYING)) {
          // A handshake OR a rekey is in flight - leave it alone. REKEYING still
          // holds the live working keys, so re-kicking SESSION_OPEN here would
          // tear down a healthy session (sessionAllocate zeroes the AEAD keys);
          // and it must not count as a fail against a healthy peer. (The old
          // pre-warm only excluded ACTIVE, so it wrongly nuked rekeys too.)
        } else {
          // No live session - a fresh pre-warm is due.
          uint32_t now = millis();
          SessionPrewarm* pw = sessionPrewarmSlot(pmac);
          if (now >= pw->cooldownUntilMs) {          // not currently backing off
            espnowSessionOpenInitiate(pmac, nullptr);
            if (++pw->fails >= kSessionPrewarmMaxFails) {
              pw->cooldownUntilMs = now + kSessionPrewarmCooldownMs;
              pw->fails = 0;
              WARN_ESPNOWF("Encrypted session to %02X:%02X:%02X:%02X:%02X:%02X not establishing after "
                           "%u tries - peer likely does not hold our identity (re-flashed/erased?). "
                           "Backing off %us. Re-pair: 'espnowforget' + 'espnowpairsecure' on BOTH devices.",
                           pmac[0], pmac[1], pmac[2], pmac[3], pmac[4], pmac[5],
                           (unsigned)kSessionPrewarmMaxFails, (unsigned)(kSessionPrewarmCooldownMs / 1000));
            }
          }
          // else: in cooldown - stay silent, don't re-kick the radio
        }
      }
    }

    // Backup master failover: track heartbeats from the configured master MAC.
    // 2026-05-19 — REQUIRE authentication on the frame before trusting that
    // the source MAC really belongs to the master. Pre-fix, an attacker who
    // knew the master's MAC could send plaintext unicast heartbeats and
    // either (a) keep the backup from promoting (DoS) or (b) demote a
    // legitimately-promoted backup. Now we accept the master-liveness signal
    // only when the frame proves session-key knowledge (SESSION_FRAME unwrap
    // from a per-peer session) OR mesh-group-key knowledge (BROADCAST_AUTH
    // HMAC). Plain plaintext heartbeats from an attacker-spoofed MAC are
    // silently ignored for the purposes of backup-master state.
    if (ctx.isAuthenticated &&
        meshEnabled() && gSettings.meshBackupEnabled &&
        gSettings.meshMasterMAC.length() > 0) {
      bool isBackupOrPromoted = (gSettings.meshRole == MESH_ROLE_BACKUP_MASTER) ||
                                 (gSettings.meshRole == MESH_ROLE_MASTER && gBackupPromoted);
      if (isBackupOrPromoted) {
        uint8_t masterMac[6] = {};
        if (parseMacAddress(gSettings.meshMasterMAC, masterMac) &&
            memcmp(ctx.recv_info->src_addr, masterMac, 6) == 0) {
          gLastMasterHeartbeat = millis();
          if (gBackupPromoted) {
            // Original master is back — demote to backup role (runtime only, not saved)
            gBackupPromoted = false;
            setMeshRole(MESH_ROLE_BACKUP_MASTER, "backup.master_returned");
            BROADCAST_PRINTF("[BACKUP] Master returned — demoted back to backup role");
          }
        }
      }
    }
  }
  // Send ACK if requested
  if (ctx.h->flags & ESPNOW_V4_FLAG_ACK_REQ) {
    v4_send_ack(ctx.recv_info->src_addr, ctx.h->msgId);
  }
}

// SENSOR_STATUS — remote sensor enable/disable announcement (mesh broadcast).
static void v4h_sensor_status(const V4RxCtx& ctx) {
  DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_SENSOR_STATUS] Received from %s msgId=%lu payloadLen=%u",
         ctx.deviceName, (unsigned long)ctx.h->msgId, ctx.payloadLen);
  if (ctx.payloadLen >= sizeof(V4PayloadSensorStatus)) {
    const V4PayloadSensorStatus* ss = (const V4PayloadSensorStatus*)ctx.payload;
    RemoteSensorType sensorType = (RemoteSensorType)ss->sensorType;
    bool enabled = (ss->enabled != 0);
    DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_SENSOR_STATUS] sensor=%s enabled=%s",
           sensorTypeToString(sensorType), enabled ? "YES" : "NO");

    DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_SENSOR_STATUS] Updating remote sensor status");
    extern void updateRemoteSensorStatus(const uint8_t* mac, const char* name, RemoteSensorType type, bool enabled);
    updateRemoteSensorStatus(ctx.recv_info->src_addr, ctx.deviceName, sensorType, enabled);
    DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_SENSOR_STATUS] Status update complete");
  } else {
    DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_SENSOR_STATUS] ERROR: Payload too small (%u < %u)",
           ctx.payloadLen, (unsigned)sizeof(V4PayloadSensorStatus));
  }
}

// SENSOR_BROADCAST — remote sensor data broadcast to mesh; cache to remote-sensor table.
static void v4h_sensor_broadcast(const V4RxCtx& ctx) {
  DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_SENSOR_BROADCAST] Received from %s msgId=%lu payloadLen=%u",
         ctx.deviceName, (unsigned long)ctx.h->msgId, ctx.payloadLen);
  if (ctx.payloadLen >= sizeof(V4PayloadSensorBroadcast)) {
    const V4PayloadSensorBroadcast* sb = (const V4PayloadSensorBroadcast*)ctx.payload;
    RemoteSensorType sensorType = (RemoteSensorType)sb->sensorType;
    uint16_t dataLen = sb->dataLen;
    DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_SENSOR_BROADCAST] sensor=%s dataLen=%u",
           sensorTypeToString(sensorType), dataLen);

    if (dataLen > 0 && ctx.payloadLen >= (sizeof(V4PayloadSensorBroadcast) + dataLen)) {
      const char* jsonData = (const char*)sb->data;
      DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_SENSOR_BROADCAST] JSON (%u bytes): %.*s", (unsigned)dataLen, (int)dataLen, jsonData);

      DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_SENSOR_BROADCAST] Caching sensor data");
      RemoteSensorData* entry = findOrCreateCacheEntry(ctx.recv_info->src_addr, ctx.deviceName, sensorType);
      if (entry) {
        size_t copyLen = (dataLen < REMOTE_SENSOR_BUFFER_SIZE - 1) ? dataLen : REMOTE_SENSOR_BUFFER_SIZE - 1;
        memcpy(entry->jsonData, jsonData, copyLen);
        entry->jsonData[copyLen] = '\0';
        entry->jsonLength = (uint16_t)copyLen;
        entry->lastUpdate = millis();
        entry->lastSeen = entry->lastUpdate;
        entry->valid = true;
        entry->enabled = true;     // live data implies the sensor is running
        entry->connected = true;
        DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_SENSOR_BROADCAST] Data cached successfully (%u bytes)", (unsigned)copyLen);
      } else {
        DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_SENSOR_BROADCAST] ERROR: Failed to allocate cache entry");
      }
    } else {
      DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_SENSOR_BROADCAST] ERROR: Invalid data length (%u) or truncated payload",
             dataLen);
    }
  } else {
    DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_SENSOR_BROADCAST] ERROR: Payload too small (%u < %u)",
           ctx.payloadLen, (unsigned)sizeof(V4PayloadSensorBroadcast));
  }
}

#if ENABLE_BONDED_MODE
// BOND_HEARTBEAT — bond-mode keepalive from paired peer; updates bond peer
// online state, boot counter, settings hash, RSSI; may trigger sync recovery.
static void v4h_bond_heartbeat(const V4RxCtx& ctx) {
  DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_HB_RX] from %s payloadLen=%u (need %u) isPaired=%d",
         ctx.deviceName, ctx.payloadLen, (unsigned)sizeof(V4PayloadBondHeartbeat), (int)ctx.isPaired);
  if (ctx.payloadLen < sizeof(V4PayloadBondHeartbeat)) return;

  const V4PayloadBondHeartbeat* hb = (const V4PayloadBondHeartbeat*)ctx.payload;
  if (!gEspNow) return;

  bool bootChanged = (hb->bootCounter != 0 && gEspNow->bondPeerBootCounter != 0 &&
                      hb->bootCounter != gEspNow->bondPeerBootCounter);
  bool wasOffline = !gEspNow->bondPeerOnline || bootChanged;
  uint32_t oldSettingsHash = gEspNow->bondPeerSettingsHash;

  gEspNow->bondPeerBootCounter = hb->bootCounter;
  gEspNow->bondPeerSettingsHash = hb->settingsHash;
  gEspNow->bondPeerUptime = hb->uptimeSec;

  if (bootChanged) {
    // Peer restarted its msgId counter; clear its stale dedup entries so its
    // reused low ids stop being dropped as duplicates. Clock-independent
    // backstop to the TTL expiry in v4_dedup_seen_and_insert.
    v4_dedup_flush_origin(ctx.recv_info->src_addr);
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
  if (ctx.recv_info->rx_ctrl) {
    gEspNow->bondRssiLast = ctx.recv_info->rx_ctrl->rssi;
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
  if (isBondWorker() && !isBondSessionTokenValid() &&
      gEspNow->lastRemoteCapValid && gEspNow->bondCapSent &&
      !gEspNow->bondSettingsSent && capExchangedLongEnough) {
    gEspNow->bondSettingsSent = true;
    if (isBondSynced()) {
      // Bond token is derived from the encrypted session at handshake time
      // (sessionDeriveAeadKeys → SessionState::bondToken), so it becomes valid
      // automatically once the session is ACTIVE — no recompute needed here.
      BROADCAST_PRINTF("[BOND_SYNC] *** SYNC COMPLETE *** role=0 (worker, recovered)");
    }
  }

  if (wasOffline) {
    // Deferred: master starts sync tick
    if (isBondMaster()) {
      gEspNow->bondNeedsCapabilityRequest = true;
      // bondNeedsStreamingSetup is set after sync completes in processBondSettings()
    }
  }
}

// STREAM_CTRL — bond mode master->worker: start/stop sensor streaming.
// Deferred to task because startSensorDataStreaming creates tasks/mutexes.
static void v4h_stream_ctrl(const V4RxCtx& ctx) {
  if (ctx.payloadLen >= 2 && gEspNow) {
    gEspNow->bondDeferredStreamCtrlSensor = ctx.payload[0];  // sensorType
    gEspNow->bondDeferredStreamCtrlEnable = ctx.payload[1];  // enable
    gEspNow->bondDeferredStreamCtrlPending = true;
    DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_STREAM_CTRL_RX] sensor=%u enable=%u (deferred)",
           ctx.payload[0], ctx.payload[1]);
  }
}
#endif // ENABLE_BONDED_MODE

// TOPO_REQ — peer is asking for our topology; respond with TOPO_START + N×TOPO_PEER.
static void v4h_topo_req(const V4RxCtx& ctx) {
  DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_REQ] Received from %s msgId=%lu payloadLen=%u",
         ctx.deviceName, (unsigned long)ctx.h->msgId, ctx.payloadLen);
  if (ctx.payloadLen < sizeof(V4PayloadTopoReq)) {
    DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_REQ] ERROR: Payload too small (%u < %u)",
           ctx.payloadLen, (unsigned)sizeof(V4PayloadTopoReq));
    return;
  }
  const V4PayloadTopoReq* tr = (const V4PayloadTopoReq*)ctx.payload;
  DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_REQ] reqId=%lu requesterMeshFp=0x%04X",
         (unsigned long)tr->reqId, ctx.h->meshFingerprint);

  // Phase 2: filter peers by requester's mesh. If the requester stamped a
  // mesh fingerprint, only report peers belonging to that mesh. fingerprint=0
  // (transitional / pre-mesh) → no filter, report all peers as in V3.
  uint16_t requesterFp = ctx.h->meshFingerprint;
  auto peerIsInRequesterMesh = [&](const uint8_t* mac) -> bool {
    if (requesterFp == 0) return true;  // no scope → report all
    if (!gEspNow) return false;
    for (int i = 0; i < gEspNow->deviceCount; i++) {
      if (memcmp(gEspNow->devices[i].mac, mac, 6) == 0) {
        uint8_t mid = gEspNow->devices[i].meshId;
        if (mid < Settings::N_MESHES) {
          return gSettings.meshes[mid].fingerprint == requesterFp;
        }
        return false;
      }
    }
    // Peer not in our paired-device table — only in mesh routing layer.
    // Phase 2 transitional: treat as default-mesh member. Phase 2.x may
    // tighten this once gMeshPeers gains a meshId field of its own.
    for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
      if (gSettings.meshes[i].enabled && gSettings.meshes[i].isDefault) {
        return gSettings.meshes[i].fingerprint == requesterFp;
      }
    }
    return false;
  };

  // Count active peers (excluding self) that are in the requester's mesh
  int peerCount = 0;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac) &&
        peerIsInRequesterMesh(gMeshPeers[i].mac)) {
      peerCount++;
    }
  }

  DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_REQ] Responding with %d peer(s) in requester's mesh", peerCount);
  v4_send_topo_start(ctx.recv_info->src_addr, tr->reqId, (uint8_t)peerCount);

  int peerIndex = 0;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac) &&
        peerIsInRequesterMesh(gMeshPeers[i].mac)) {
      bool isLast = (peerIndex == peerCount - 1);
      // String must outlive v4_send_topo_peer's read of c_str() — kept on
      // stack for the duration of this scope, which the call below honors.
      String resolvedName = getEspNowDeviceName(gMeshPeers[i].mac);
      const char* peerNamePtr = resolvedName.length() ? resolvedName.c_str() : "Unknown";
      MeshPeerHealth* ph = getMeshPeerHealth(gMeshPeers[i].mac, false);
      int8_t rssi = ph ? ph->rssi : 0;
      v4_send_topo_peer(ctx.recv_info->src_addr, tr->reqId, (uint8_t)peerIndex,
                        isLast, gMeshPeers[i].mac, rssi,
                        false, peerNamePtr);
      peerIndex++;
    }
  }
  DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_REQ] Sent %d TOPO_PEER frame(s)", peerIndex);
}

// TOPO_START — incoming topology response start (peer count). Initializes stream.
static void v4h_topo_start(const V4RxCtx& ctx) {
  DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_START] Received from %s msgId=%lu payloadLen=%u",
         ctx.deviceName, (unsigned long)ctx.h->msgId, ctx.payloadLen);
  if (ctx.payloadLen < sizeof(V4PayloadTopoStart)) {
    DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_START] ERROR: Payload too small (%u < %u)",
           ctx.payloadLen, (unsigned)sizeof(V4PayloadTopoStart));
    return;
  }
  const V4PayloadTopoStart* ts = (const V4PayloadTopoStart*)ctx.payload;
  DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_START] reqId=%lu peerCount=%u",
         (unsigned long)ts->reqId, ts->peerCount);
  if (ts->reqId != gTopoRequestId || millis() >= gTopoRequestTimeout) {
    DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_START] Rejected: reqId mismatch or timeout");
    return;
  }
  TopologyStream* stream = findOrCreateTopoStream(ctx.recv_info->src_addr, ts->reqId);
  if (!stream || !stream->active) {
    DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_START] ERROR: Could not allocate stream");
    return;
  }
  if (stream->receivedPeers == 0 && stream->totalPeers == 0) {
    const char* senderNamePtr = (ctx.deviceName && ctx.deviceName[0]) ? ctx.deviceName : nullptr;
    char senderNameFallBuf[48];
    if (!senderNamePtr && gMeshPeerMeta) {
      for (int _sni = 0; _sni < gMeshPeerSlots; _sni++) {
        if (gMeshPeerMeta[_sni].isActive && memcmp(gMeshPeerMeta[_sni].mac, ctx.recv_info->src_addr, 6) == 0 && gMeshPeerMeta[_sni].name[0]) {
          senderNamePtr = gMeshPeerMeta[_sni].name; break;
        }
      }
    }
    if (!senderNamePtr && gEspNow) {
      for (int _sni = 0; _sni < gEspNow->deviceCount; _sni++) {
        if (memcmp(gEspNow->devices[_sni].mac, ctx.recv_info->src_addr, 6) == 0 && gEspNow->devices[_sni].name.length()) {
          strlcpy(senderNameFallBuf, gEspNow->devices[_sni].name.c_str(), sizeof(senderNameFallBuf));
          senderNamePtr = senderNameFallBuf; break;
        }
      }
    }
    if (!senderNamePtr) {
      formatMacAddressBuf(ctx.recv_info->src_addr, senderNameFallBuf, sizeof(senderNameFallBuf));
      senderNamePtr = senderNameFallBuf;
    }
    strncpy(stream->senderName, senderNamePtr, 31);
    stream->senderName[31] = '\0';
    stream->totalPeers = ts->peerCount;
    stream->accumulatedData = "";
    addTopoDeviceName(ctx.recv_info->src_addr, senderNamePtr);
    DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_START] Stream initialized for %s: expecting %d peers",
           stream->senderName, ts->peerCount);
  }
  if (ts->peerCount == 0) {
    DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_START] 0 peers - edge device, finalizing");
    finalizeTopologyStream(stream);
  }
}

// TOPO_PEER — incoming topology peer entry. Appends to accumulated data.
static void v4h_topo_peer(const V4RxCtx& ctx) {
  DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_PEER] Received from %s msgId=%lu payloadLen=%u",
         ctx.deviceName, (unsigned long)ctx.h->msgId, ctx.payloadLen);
  if (ctx.payloadLen < sizeof(V4PayloadTopoPeer)) {
    DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_PEER] ERROR: Payload too small (%u < %u)",
           ctx.payloadLen, (unsigned)sizeof(V4PayloadTopoPeer));
    return;
  }
  const V4PayloadTopoPeer* tp = (const V4PayloadTopoPeer*)ctx.payload;
  char peerMacStr[18];
  formatMacAddressBuf(tp->mac, peerMacStr, sizeof(peerMacStr));
  DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_PEER] reqId=%lu idx=%u isLast=%u mac=%s name=%s",
         (unsigned long)tp->reqId, tp->peerIndex, tp->isLast, peerMacStr, tp->name);
  if (tp->reqId != gTopoRequestId || millis() >= gTopoRequestTimeout) {
    DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_PEER] Rejected: reqId mismatch or timeout");
    return;
  }
  TopologyStream* stream = findTopoStream(ctx.recv_info->src_addr, tp->reqId);
  if (!stream || !stream->active) {
    DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_PEER] No active stream for this sender");
    return;
  }
  if (stream->accumulatedData.indexOf(peerMacStr) != -1) {
    DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_PEER] Duplicate peer %s, skipping", peerMacStr);
    return;
  }
  const char* peerNamePtr2 = (tp->name[0] && strcmp(tp->name, "Unknown") != 0) ? tp->name : nullptr;
  char peerNameFallBuf[48];
  if (!peerNamePtr2) {
    if (getTopoDeviceName(tp->mac, peerNameFallBuf, sizeof(peerNameFallBuf)) && peerNameFallBuf[0]) {
      peerNamePtr2 = peerNameFallBuf;
    }
  }
  if (!peerNamePtr2 && gMeshPeerMeta) {
    for (int _tni = 0; _tni < gMeshPeerSlots; _tni++) {
      if (gMeshPeerMeta[_tni].isActive && memcmp(gMeshPeerMeta[_tni].mac, tp->mac, 6) == 0 && gMeshPeerMeta[_tni].name[0]) {
        peerNamePtr2 = gMeshPeerMeta[_tni].name; break;
      }
    }
  }
  if (!peerNamePtr2 && gEspNow) {
    for (int _tni = 0; _tni < gEspNow->deviceCount; _tni++) {
      if (memcmp(gEspNow->devices[_tni].mac, tp->mac, 6) == 0 && gEspNow->devices[_tni].name.length()) {
        strlcpy(peerNameFallBuf, gEspNow->devices[_tni].name.c_str(), sizeof(peerNameFallBuf));
        peerNamePtr2 = peerNameFallBuf; break;
      }
    }
  }
  if (peerNamePtr2) {
    addTopoDeviceName(tp->mac, peerNamePtr2);
  }
  char peerInfoBuf[128];
  snprintf(peerInfoBuf, sizeof(peerInfoBuf), "  \xe2\x86\x92 %s (%s)\n    RSSI: %d dBm\n",
           peerNamePtr2 ? peerNamePtr2 : "Unknown", peerMacStr, (int)tp->rssi);
  stream->accumulatedData += peerInfoBuf;
  stream->receivedPeers++;
  gTopoLastResponseTime = millis();
  DEBUGF(DEBUG_ESPNOW_TOPO, "[V4_RX_TOPO_PEER] Accumulated peer %d/%d: %s",
         stream->receivedPeers, stream->totalPeers, peerNamePtr2 ? peerNamePtr2 : "Unknown");
}

// USER_SYNC — propagates user credentials between bonded peers; requires
// ESPNOW_V4_FLAG_ENCRYPTED, admin authentication, and a setting opt-in.
// All early-rejection paths send their own ACK or CMD_RESP back.
// Defined in System_Utils.cpp; queues a callback onto cmd_exec_task. Declared
// here (same extern as System_ESPNow_Handlers_Crypto.cpp) so USER_SYNC can
// defer its heavy body off espnow_task.
extern bool submitDeferredToCmdExec(ExecReq::DeferredFn fn, void* arg);

// ---------------------------------------------------------------------------
// Failed bond-auth audit — the mirror of the espnow/bond SUCCESS line.
// A peer presenting a bad/malformed/missing bond token, or sending bond traffic
// while unpaired, is a failed attempt to gain the RCE/command channel; it belongs
// in failed_login.log exactly like a bad password does. Two constraints shape
// this: (1) the failures are detected on espnow_task (RX drain / super-loop)
// where synchronous FS writes are forbidden — so we defer the actual write to
// cmd_exec_task via submitDeferredToCmdExec (same model as runDeferredSession-
// Confirm); (2) a hostile or looping peer can spam attempts, so we throttle to
// one logged failure per window to protect the 680 KB-capped log file.
struct BondAuthFailLog { char who[40]; char ip[20]; char reason[72]; };

static void runDeferredBondAuthFailLog(void* arg) {
  auto* w = static_cast<BondAuthFailLog*>(arg);
#if ENABLE_HTTP_SERVER
  extern void logAuthAttempt(bool, const char*, const String&, const String&, const String&);
  logAuthAttempt(false, "espnow/bond", String(w->who), String(w->ip), String(w->reason));
#endif
  free(w);
}

static void logBondAuthFailure(const uint8_t* mac, const char* reason) {
  static uint32_t sLastFailLogMs = 0;
  uint32_t now = millis();
  if (sLastFailLogMs != 0 && (now - sLastFailLogMs) < 5000) return;  // throttle: 1 / 5s
  sLastFailLogMs = now;
  auto* w = static_cast<BondAuthFailLog*>(
      ps_alloc(sizeof(BondAuthFailLog), AllocPref::PreferPSRAM, "espnow.bondfail.log"));
  if (!w) return;
  String who = mac ? getEspNowDeviceName(mac) : String("");
  if (who.length() == 0 && mac) who = formatMacAddress(mac);
  if (who.length() == 0) who = "unknown";
  strncpy(w->who, who.c_str(), sizeof(w->who) - 1);     w->who[sizeof(w->who) - 1]     = '\0';
  strncpy(w->ip, "espnow", sizeof(w->ip) - 1);          w->ip[sizeof(w->ip) - 1]       = '\0';
  strncpy(w->reason, reason ? reason : "Bond auth failed", sizeof(w->reason) - 1);
  w->reason[sizeof(w->reason) - 1] = '\0';
  if (!submitDeferredToCmdExec(runDeferredBondAuthFailLog, w)) free(w);
}

// USER_SYNC is heavy: JSON parse, admin auth (password verify), users.json
// read/modify/write, target password hashing, per-user settings write. All of
// that is CPU- and FS-bound and must NOT run inline on espnow_task (it stalls
// the RX-ring drain and can starve session handshakes). We snapshot the frame
// into PSRAM and defer the whole body to cmd_exec_task — same model as
// runDeferredSessionConfirm. The deferred body also OWNS all the response
// sends, so they run off the dispatcher.
struct DeferredUserSyncWork {
  uint8_t  srcAddr[6];
  uint32_t msgId;
  bool     isSessionEncrypted;
  char     deviceName[33];
  uint16_t payloadLen;
  uint8_t  payload[];  // flexible array; alloc = sizeof(*this) + payloadLen
};

static void doUserSyncWork(const DeferredUserSyncWork* w) {
  const uint8_t* srcAddr      = w->srcAddr;
  const uint32_t msgId        = w->msgId;
  const char*    deviceName   = w->deviceName;
  const uint8_t* payload      = w->payload;
  const uint16_t payloadLen   = w->payloadLen;

  DEBUGF(DEBUG_ESPNOW_MESH, "[V4_RX_USER_SYNC] Processing from %s msgId=%lu encrypted=%s",
         deviceName, (unsigned long)msgId, w->isSessionEncrypted ? "YES" : "NO");

  // Security: must have arrived AEAD-encrypted in a SESSION_FRAME. Pre-fix
  // this checked the legacy ESPNOW_V4_FLAG_ENCRYPTED bit, which the LMK rip
  // (task #47) stopped ever setting — so USER_SYNC was ALWAYS rejected here
  // even for correctly session-encrypted frames (espnowusersync was broken
  // end-to-end). isSessionEncrypted is the real signal.
  if (!w->isSessionEncrypted) {
    ERROR_ESPNOWF("[USER_SYNC] SECURITY: Rejected from %s — must be session-encrypted", deviceName);
    broadcastOutput("[ESP-NOW] SECURITY: User sync rejected - encryption required");
    v4_send_ack(srcAddr, msgId);
    return;
  }

  // User sync must be enabled
  if (!gSettings.espnowUserSyncEnabled) {
    WARN_ESPNOWF("[USER_SYNC] Disabled — rejecting from %s", deviceName);
    broadcastOutput("[ESP-NOW] User sync DISABLED - enable with 'espnowusersync on'");
    v4_send_ack(srcAddr, msgId);
    return;
  }

  if (payloadLen == 0) {
    WARN_ESPNOWF("[USER_SYNC] Empty payload from %s", deviceName);
    v4_send_command_response(srcAddr, msgId, false, "Empty payload", strlen("Empty payload"));
    return;
  }

  PSRAM_JSON_DOC(doc);
  String jsonStr((const char*)payload, payloadLen);
  if (deserializeJson(doc, jsonStr) != DeserializationError::Ok) {
    WARN_ESPNOWF("[USER_SYNC] Malformed JSON from %s", deviceName);
    v4_send_command_response(srcAddr, msgId, false, "Malformed JSON", strlen("Malformed JSON"));
    return;
  }

  // admin_user/admin_pass carry the RECEIVING device's admin (i.e. THIS device).
  // The sender supplies the target's admin credentials and we validate them
  // against our OWN user store below — the same model as a remote command
  // (v4_handle_cmd: isValidUser against the local DB, session-encryption required).
  const char* adminUser  = doc["recv_admin_user"] | "";
  const char* adminPass  = doc["recv_admin_pass"] | "";
  const char* targetUser = doc["target_user"]     | "";
  const char* targetPass = doc["target_pass"]     | "";
  const char* role       = doc["role"]            | "user";

  if (!strlen(adminUser) || !strlen(adminPass) || !strlen(targetUser) || !strlen(targetPass)) {
    WARN_ESPNOWF("[USER_SYNC] Missing required fields from %s", deviceName);
    v4_send_command_response(srcAddr, msgId, false, "Missing required fields", strlen("Missing required fields"));
    return;
  }

  if (!isValidUser(String(adminUser), String(adminPass))) {
    ERROR_ESPNOWF("[USER_SYNC] Admin auth FAILED for '%s' from %s", adminUser, deviceName);
    broadcastOutput("[ESP-NOW] User sync: Admin authentication FAILED");
    v4_send_command_response(srcAddr, msgId, false, "Admin authentication failed", strlen("Admin authentication failed"));
    return;
  }

  if (!isAdminUser(String(adminUser))) {
    ERROR_ESPNOWF("[USER_SYNC] '%s' is not admin — sync rejected from %s", adminUser, deviceName);
    broadcastOutput("[ESP-NOW] User sync: Admin privileges required");
    v4_send_command_response(srcAddr, msgId, false, "Admin privileges required", strlen("Admin privileges required"));
    return;
  }

  uint32_t existingId = 0;
  if (getUserIdByUsername(String(targetUser), existingId)) {
    WARN_ESPNOWF("[USER_SYNC] User '%s' already exists (id=%u) — skipping", targetUser, (unsigned)existingId);
    BROADCAST_PRINTF("[ESP-NOW] User sync: '%s' already exists", targetUser);
    v4_send_command_response(srcAddr, msgId, true, "User already exists (skipped)", strlen("User already exists (skipped)"));
    return;
  }

  if (!filesystemReady) {
    ERROR_ESPNOWF("[USER_SYNC] Filesystem not ready");
    v4_send_command_response(srcAddr, msgId, false, "Filesystem not ready", strlen("Filesystem not ready"));
    return;
  }

  // Create the user. See original branch for the trust-model rationale —
  // V3 USER_SYNC has already validated encryption + admin auth above; the
  // local users.json mutation runs under systemAuth() like every other
  // users.json mutation in System_User.cpp.
  {
    FsLockGuard guard("user_sync.create");
    AuthContext userSyncCtx = VFS::systemAuth("user_sync.create");

    if (!VFS::existsGuarded(USERS_JSON_FILE, userSyncCtx)) {
      ERROR_ESPNOWF("[USER_SYNC] users.json not found");
      v4_send_command_response(srcAddr, msgId, false, "users.json not found", strlen("users.json not found"));
      return;
    }

    File f = VFS::openGuarded(USERS_JSON_FILE, "r", userSyncCtx);
    if (!f) {
      ERROR_ESPNOWF("[USER_SYNC] Could not open users.json");
      v4_send_command_response(srcAddr, msgId, false, "Could not open users.json", strlen("Could not open users.json"));
      return;
    }

    PSRAM_JSON_DOC(userDoc);
    DeserializationError err = deserializeJson(userDoc, f);
    f.close();

    if (err || !userDoc["users"]) {
      ERROR_ESPNOWF("[USER_SYNC] Malformed users.json");
      v4_send_command_response(srcAddr, msgId, false, "Malformed users.json", strlen("Malformed users.json"));
      return;
    }

    int nextId = userDoc["nextId"] | 2;
    JsonArray users = userDoc["users"];

    JsonObject newUser = users.add<JsonObject>();
    newUser["id"]        = nextId;
    newUser["username"]  = targetUser;
    // SECURITY: synced accounts are always created as a standard user — never
    // admin, even when they are admin on the source device. Privilege must be
    // granted deliberately on THIS device (userpromote), not propagated by a
    // sync. The source role is read only to log a downgrade notice below.
    newUser["role"]      = "user";
    // Stamp the real creation time when the wall clock is available (we are
    // creating the user right now). Otherwise this is written null and resolved
    // lazily by resolvePendingUserCreationTimes(), which runs only at boot /
    // NTP-sync — so a user synced after this boot's NTP sync would otherwise
    // stay createdAt=null until the next reboot.
    char createdAtBuf[24];
    time_t nowT = time(nullptr);
    struct tm tmUtc;
    if (nowT > 1577836800 && gmtime_r(&nowT, &tmUtc) &&
        strftime(createdAtBuf, sizeof(createdAtBuf), "%Y-%m-%dT%H:%M:%SZ", &tmUtc) > 0) {
      newUser["createdAt"]       = createdAtBuf;    // exact creation time
      newUser["createdAtSource"] = "clock";         // stamped live from the wall clock
    } else {
      newUser["createdAt"]       = (const char*)nullptr;  // no clock yet → lazy resolve
      newUser["createdAtSource"] = "pending";
    }
    char createdByBuf[48];
    snprintf(createdByBuf, sizeof(createdByBuf), "espnow:%s", deviceName);
    newUser["createdBy"] = createdByBuf;
    newUser["createdMs"] = millis();
    extern uint32_t gNTPAnchorId;
    extern uint32_t gBootCounter;
    newUser["ntpAnchorId"] = gNTPAnchorId;
    newUser["bootCount"]   = gBootCounter;
    userDoc["nextId"] = nextId + 1;

    f = VFS::openGuarded(USERS_JSON_FILE, "w", userSyncCtx);
    if (!f || serializeJson(userDoc, f) == 0) {
      if (f) f.close();
      ERROR_ESPNOWF("[USER_SYNC] Failed to write users.json");
      v4_send_command_response(srcAddr, msgId, false, "Failed to write users.json", strlen("Failed to write users.json"));
      return;
    }
    f.close();

    String hashedPassword = hashUserPassword(String(targetPass));
    PSRAM_JSON_DOC(defaults);
    defaults["theme"]    = "light";
    defaults["password"] = hashedPassword;
    saveUserSettings((uint32_t)nextId, defaults);

    bool srcWasAdmin = (strcmp(role, "user") != 0);
    INFO_ESPNOWF("[USER_SYNC] Created user '%s' (id=%d) as standard user from %s%s",
                 targetUser, nextId, deviceName,
                 srcWasAdmin ? " [source was admin — not propagated]" : "");
    BROADCAST_PRINTF("[ESP-NOW] User sync: Created '%s' as standard user from %s%s",
                     targetUser, deviceName,
                     srcWasAdmin ? " (was admin on source; use userpromote to elevate)" : "");

    char respBuf[128];
    snprintf(respBuf, sizeof(respBuf), "User '%s' created (id=%d)", targetUser, nextId);
    v4_send_command_response(srcAddr, msgId, true, respBuf, strlen(respBuf));
  }

  // The new account must be visible to the auth layer immediately. The canonical
  // creation path (adminCreateUser) bumps the identity generation to invalidate
  // auth caches; the hand-rolled sync write above must do the same, or the synced
  // user may fail to authenticate until the next bump / reboot.
  extern void bumpIdentityGeneration(const char* reason);
  bumpIdentityGeneration("user.sync");
}

static void runDeferredUserSync(void* arg) {
  auto* w = static_cast<DeferredUserSyncWork*>(arg);
  doUserSyncWork(w);
  free(w);
}

// On-RX (espnow_task): snapshot frame into PSRAM and defer. Lightweight only.
static void v4h_user_sync(const V4RxCtx& ctx) {
  const size_t need = sizeof(DeferredUserSyncWork) + ctx.payloadLen;
  auto* w = static_cast<DeferredUserSyncWork*>(
      ps_alloc(need, AllocPref::PreferPSRAM, "espnow.usersync.defer"));
  if (!w) {
    ERROR_ESPNOWF("[USER_SYNC] PSRAM alloc failed (defer drop) from %s", ctx.deviceName);
    return;
  }
  memcpy(w->srcAddr, ctx.recv_info->src_addr, 6);
  w->msgId              = ctx.h->msgId;
  w->isSessionEncrypted = ctx.isSessionEncrypted;
  strlcpy(w->deviceName, ctx.deviceName ? ctx.deviceName : "", sizeof(w->deviceName));
  w->payloadLen = ctx.payloadLen;
  if (ctx.payloadLen) memcpy(w->payload, ctx.payload, ctx.payloadLen);
  if (!submitDeferredToCmdExec(runDeferredUserSync, w)) {
    ERROR_ESPNOWF("[USER_SYNC] cmd_exec queue full, dropping from %s", ctx.deviceName);
    free(w);
  }
}

#if ENABLE_BONDED_MODE
// BOND_CAP_REQ — peer asks for our capability summary. Defer to task.
static void v4h_bond_cap_req(const V4RxCtx& ctx) {
  DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_CAP_REQ_RX] from %s role=%d capSent=%d",
         ctx.deviceName, (int)gSettings.bondRole,
         gEspNow ? (int)gEspNow->bondCapSent : -1);
  if (gEspNow) {
    memcpy(gEspNow->bondPendingResponseMac, ctx.recv_info->src_addr, 6);
    gEspNow->bondNeedsCapabilityResponse = true;
  }
}

// BOND_CAP_RESP — peer's capability summary. Cache it; queue our own back
// if we haven't sent one yet. NOTE: extra payloadLen check is done here
// (was in the original if-condition) since the dispatcher can't express it.
static void v4h_bond_cap_resp(const V4RxCtx& ctx) {
  if (ctx.payloadLen != sizeof(CapabilitySummary)) return;
  const CapabilitySummary* cap = (const CapabilitySummary*)ctx.payload;
  DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_CAP_RESP_RX] from %s role=%d capSent=%d sensorMask=0x%04lX devName='%.16s'",
         ctx.deviceName, (int)gSettings.bondRole,
         gEspNow ? (int)gEspNow->bondCapSent : -1,
         (unsigned long)cap->sensorMask, cap->deviceName);
  if (!gEspNow) return;
  memcpy(&gEspNow->lastRemoteCap, cap, sizeof(CapabilitySummary));
  gEspNow->lastRemoteCapValid = true;
  gEspNow->lastRemoteCapTime = millis();
  gEspNow->bondReceivedCapability = true;
  gEspNow->bondSyncInFlight = BOND_SYNC_NONE;
  gEspNow->bondSyncRetryCount = 0;
  gEspNow->bondSyncLastAttemptMs = 0;
  if (!gEspNow->bondCapSent) {
    memcpy(gEspNow->bondPendingResponseMac, ctx.recv_info->src_addr, 6);
    gEspNow->bondNeedsCapabilityResponse = true;
    DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_CAP_RESP_RX] queued reciprocal CAP_RESP (bondCapSent was false)");
  } else {
    DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_CAP_RESP_RX] NOT sending reciprocal (bondCapSent already true)");
  }
}

// SENSOR_DATA — bond mode binary sensor data; cache to remote sensor entry.
static void v4h_sensor_data(const V4RxCtx& ctx) {
  if (ctx.payloadLen < sizeof(V4PayloadSensorData)) return;
  const V4PayloadSensorData* sd = (const V4PayloadSensorData*)ctx.payload;
  size_t totalExpected = sizeof(V4PayloadSensorData) + sd->dataLen;
  if (sd->dataLen == 0 || totalExpected > ctx.payloadLen) return;
  RemoteSensorType sensorType = (RemoteSensorType)sd->sensorType;
  if (sensorType >= REMOTE_SENSOR_MAX) return;
  RemoteSensorData* entry = findOrCreateCacheEntry(ctx.recv_info->src_addr, ctx.deviceName, sensorType);
  if (!entry) return;
  size_t copyLen = (sd->dataLen < REMOTE_SENSOR_BUFFER_SIZE - 1) ? sd->dataLen : REMOTE_SENSOR_BUFFER_SIZE - 1;
  memcpy(entry->jsonData, sd->data, copyLen);
  entry->jsonData[copyLen] = '\0';
  entry->jsonLength = (uint16_t)copyLen;
  entry->lastUpdate = millis();
  entry->lastSeen = entry->lastUpdate;
  entry->valid = true;
  entry->enabled = true;     // live data implies the sensor is running
  entry->connected = true;
  DEBUGF(DEBUG_ESPNOW_MESH, "[BOND] Sensor %s from %s len=%u seq=%lu",
         sensorTypeToString(sensorType), ctx.deviceName, (unsigned)copyLen, (unsigned long)sd->seqNum);
}

// SETTINGS_REQ — peer asks for our settings file. Defer to task.
static void v4h_settings_req(const V4RxCtx& ctx) {
  DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_SETTINGS_REQ_RX] from %s isPaired=%d", ctx.deviceName, (int)ctx.isPaired);
  if (gEspNow) {
    memcpy(gEspNow->bondPendingResponseMac, ctx.recv_info->src_addr, 6);
    gEspNow->bondNeedsSettingsResponse = true;
    DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_SETTINGS_REQ_RX] set bondNeedsSettingsResponse=true");
  }
}

// SCHEMA_REQ — peer asks for our settings schema as a file. Mirrors
// SETTINGS_REQ: handler is a small flag-flip, the actual JSON generation +
// file transfer happens later in the tick (section 9f) where there's stack
// headroom and we can gate on an encrypted session.
static void v4h_schema_req(const V4RxCtx& ctx) {
  DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_SCHEMA_REQ_RX] from %s isPaired=%d", ctx.deviceName, (int)ctx.isPaired);
  if (gEspNow) {
    memcpy(gEspNow->bondPendingResponseMac, ctx.recv_info->src_addr, 6);
    gEspNow->bondNeedsSchemaResponse = true;
    DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_SCHEMA_REQ_RX] set bondNeedsSchemaResponse=true");
  }
}

// BOND_STATUS_REQ — peer asks for live status. Defer to task.
static void v4h_bond_status_req(const V4RxCtx& ctx) {
  DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_STATUS_REQ_RX] from %s", ctx.deviceName);
  if (gEspNow) {
    memcpy(gEspNow->bondPendingResponseMac, ctx.recv_info->src_addr, 6);
    gEspNow->bondNeedsStatusResponse = true;
  }
}

// BOND_STATUS_RESP — peer's live status; cache it.
static void v4h_bond_status_resp(const V4RxCtx& ctx) {
  if (ctx.payloadLen < sizeof(BondPeerStatus) || !gEspNow) return;
  memcpy(&gEspNow->bondPeerStatus, ctx.payload, sizeof(BondPeerStatus));
  gEspNow->bondPeerStatusValid = true;
  gEspNow->bondPeerStatusTimeMs = millis();
  BROADCAST_PRINTF("[BOND_STATUS_RESP_RX] from %s enabled=0x%04X connected=0x%04X heap=%lu",
         ctx.deviceName, gEspNow->bondPeerStatus.sensorEnabledMask,
         gEspNow->bondPeerStatus.sensorConnectedMask,
         (unsigned long)gEspNow->bondPeerStatus.freeHeap);
}

// MANIFEST_REQ — peer asks for our manifest file. Defer to task.
static void v4h_manifest_req(const V4RxCtx& ctx) {
  DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_MANIFEST_REQ_RX] from %s isPaired=%d", ctx.deviceName, (int)ctx.isPaired);
  if (gEspNow) {
    memcpy(gEspNow->bondPendingResponseMac, ctx.recv_info->src_addr, 6);
    gEspNow->bondNeedsManifestResponse = true;
    DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_MANIFEST_REQ_RX] set bondNeedsManifestResponse=true");
  }
}
#endif // ENABLE_BONDED_MODE

// METADATA_REQ — peer asks for our metadata. Defer to task.
// Phase 5 — SUBSCRIBE_UPDATE handler. Peer is telling us which event
// categories they want to receive from US. We persist the bitmask into the
// peer's PeerIdentity slot; broadcast paths gate on it.
//
// The opcode is REQ_PAIRED, so by the time we get here the sender has a
// PeerIdentity (the dispatcher rejected the frame otherwise). We still
// re-check peerIdentitySetSubscriptions's return value because race-y
// FS_LIST / FS_STAT / FS_GET — thin shims that unpack V4RxCtx and call into
// System_ESPNow_FsList.cpp where the real work lives. All six opcodes are
// gated on REQ_PAIRED + REQ_BOND_MODE + REQ_SESSION_ENC by the dispatch
// table, so by the time we reach these handlers the sender is a bonded
// peer over an encrypted session.
extern void fsListOnRequestReceived(const uint8_t srcMac[6],
                                    const uint8_t* payload, uint16_t payloadLen);
extern void fsListOnReplyReceived(const uint8_t srcMac[6],
                                  const uint8_t* payload, uint16_t payloadLen);
extern void fsStatOnRequestReceived(const uint8_t srcMac[6],
                                    const uint8_t* payload, uint16_t payloadLen);
extern void fsStatOnReplyReceived(const uint8_t srcMac[6],
                                  const uint8_t* payload, uint16_t payloadLen);
extern void fsGetOnRequestReceived(const uint8_t srcMac[6],
                                   const uint8_t* payload, uint16_t payloadLen);
extern void fsGetOnAckReceived(const uint8_t srcMac[6],
                               const uint8_t* payload, uint16_t payloadLen);
static void v4h_fs_list_req(const V4RxCtx& ctx) {
  fsListOnRequestReceived(ctx.recv_info->src_addr, ctx.payload, ctx.payloadLen);
}
static void v4h_fs_list_reply(const V4RxCtx& ctx) {
  fsListOnReplyReceived(ctx.recv_info->src_addr, ctx.payload, ctx.payloadLen);
}
static void v4h_fs_stat_req(const V4RxCtx& ctx) {
  fsStatOnRequestReceived(ctx.recv_info->src_addr, ctx.payload, ctx.payloadLen);
}
static void v4h_fs_stat_reply(const V4RxCtx& ctx) {
  fsStatOnReplyReceived(ctx.recv_info->src_addr, ctx.payload, ctx.payloadLen);
}
static void v4h_fs_get_req(const V4RxCtx& ctx) {
  fsGetOnRequestReceived(ctx.recv_info->src_addr, ctx.payload, ctx.payloadLen);
}
static void v4h_fs_get_ack(const V4RxCtx& ctx) {
  fsGetOnAckReceived(ctx.recv_info->src_addr, ctx.payload, ctx.payloadLen);
}

// concurrent espnowforget could remove the slot between dispatch and here.
static void v4h_subscribe_update(const V4RxCtx& ctx) {
  if (ctx.payloadLen < sizeof(V4PayloadSubscribe)) {
    WARN_ESPNOWF("[SUBSCRIBE] payload too short (%u < %u) from %s — dropping",
                 ctx.payloadLen, (unsigned)sizeof(V4PayloadSubscribe),
                 MAC_STR(ctx.recv_info->src_addr));
    return;
  }
  V4PayloadSubscribe p;
  memcpy(&p, ctx.payload, sizeof(p));
  // Reserved bytes must be zero — defends against future-flag pollution.
  for (size_t i = 0; i < sizeof(p.reserved); i++) {
    if (p.reserved[i] != 0) {
      WARN_ESPNOWF("[SUBSCRIBE] reserved bytes nonzero from %s — dropping",
                   MAC_STR(ctx.recv_info->src_addr));
      return;
    }
  }
  if (peerIdentitySetSubscriptions(ctx.recv_info->src_addr, p.requestedEvents)) {
    INFO_ESPNOWF("[SUBSCRIBE] %s updated subs to 0x%08lX",
                 MAC_STR(ctx.recv_info->src_addr),
                 (unsigned long)p.requestedEvents);
  }
  // ACK if requested — flag is cleared on the headerCopy in v4_try_handle_incoming
  // for SESSION_FRAME unwrap path, but plaintext SUBSCRIBE_UPDATE still
  // triggers the downstream ACK send.
}

static void v4h_metadata_req(const V4RxCtx& ctx) {
  bool isEncrypted = false;
  if (gEspNow) {
    for (int i = 0; i < gEspNow->deviceCount; i++) {
      if (memcmp(gEspNow->devices[i].mac, ctx.recv_info->src_addr, 6) == 0) {
        isEncrypted = gEspNow->devices[i].encrypted;
        break;
      }
    }
  }
  DEBUG_ESPNOW_METADATAF("[METADATA] REQ received from %s (%s) msgId=%lu isPaired=%d isEncrypted=%d",
    ctx.deviceName, MAC_STR(ctx.recv_info->src_addr), (unsigned long)ctx.h->msgId,
    (int)ctx.isPaired, (int)isEncrypted);
  if (gEspNow) {
    memcpy(gEspNow->metadataPendingResponseMac, ctx.recv_info->src_addr, 6);
    gEspNow->bondNeedsMetadataResponse = true;
    DEBUG_ESPNOW_METADATAF("[METADATA] bondNeedsMetadataResponse=true, will RESP to %s",
      MAC_STR(ctx.recv_info->src_addr));
  } else {
    WARN_ESPNOWF("[METADATA] REQ from %s ignored: gEspNow is null", ctx.deviceName);
  }
}

// METADATA_RESP / METADATA_PUSH — shared handler. Type comes from header.
static void v4h_metadata_resp_push(const V4RxCtx& ctx) {
  const char* metaType = (ctx.h->type == ESPNOW_V4_TYPE_METADATA_PUSH) ? "PUSH" : "RESP";
  DEBUG_ESPNOW_METADATAF("[METADATA] %s received from %s (%s) msgId=%lu payloadLen=%u (need %u) isPaired=%d",
    metaType, ctx.deviceName, MAC_STR(ctx.recv_info->src_addr),
    (unsigned long)ctx.h->msgId, ctx.payloadLen, (unsigned)sizeof(V4PayloadMetadata), (int)ctx.isPaired);
  if (ctx.payloadLen < sizeof(V4PayloadMetadata)) {
    WARN_ESPNOWF("[METADATA] %s from %s REJECTED: payload too small (%u < %u)",
      metaType, ctx.deviceName, ctx.payloadLen, (unsigned)sizeof(V4PayloadMetadata));
    return;
  }
  const V4PayloadMetadata* meta = (const V4PayloadMetadata*)ctx.payload;
  DEBUG_ESPNOW_METADATAF("[METADATA] %s payload: name='%s' friendlyName='%s' room='%s' zone='%s' tags='%s' stationary=%d",
    metaType, meta->deviceName, meta->friendlyName, meta->room, meta->zone, meta->tags, (int)meta->stationary);
  if (!gEspNow) {
    WARN_ESPNOWF("[METADATA] %s from %s dropped: gEspNow is null", metaType, ctx.deviceName);
    return;
  }
  bool wasPending = gEspNow->deferredMetadataPending;
  memcpy(gEspNow->deferredMetadataSrcMac, ctx.recv_info->src_addr, 6);
  memcpy(&gEspNow->deferredMetadataPayload, meta, sizeof(V4PayloadMetadata));
  gEspNow->deferredMetadataPending = true;
  DEBUG_ESPNOW_METADATAF("[METADATA] %s deferred for task processing (overwrote=%d)",
    metaType, (int)wasPending);
}

// STREAM — incoming stream output; ring-buffer enqueue to task. Bypasses
// dedup (see special-cased dedup bypass in v4_try_handle_incoming).
static void v4h_stream(const V4RxCtx& ctx) {
  if (ctx.payloadLen == 0 || ctx.payloadLen > ESPNOW_V4_MAX_PAYLOAD || !gEspNow || !gEspNow->streamQueue) return;
  int head = gEspNow->streamQueueHead;
  int nextHead = (head + 1) & gEspNow->streamQueueMask;
  if (nextHead != gEspNow->streamQueueTail) {
    auto& entry = gEspNow->streamQueue[head];
    size_t copyLen = (ctx.payloadLen < sizeof(entry.content) - 1) ? ctx.payloadLen : sizeof(entry.content) - 1;
    memcpy(entry.content, ctx.payload, copyLen);
    entry.content[copyLen] = '\0';
    memcpy(entry.srcMac, ctx.recv_info->src_addr, 6);
    strncpy(entry.deviceName, ctx.deviceName, sizeof(entry.deviceName) - 1);
    entry.deviceName[sizeof(entry.deviceName) - 1] = '\0';
    // Carry the command correlation id (frame msgId). For remote command output
    // this is the command's reqId; for legacy startstream it's 0 (unsolicited).
    entry.cmdMsgId = ctx.h->msgId;
    entry.used = true;
    gEspNow->streamQueueHead = nextHead;
  } else {
    // Queue full — drop this frame (better than overwriting). Counted into
    // streamDroppedCount so espnowstats / espnowsaturation surface the overflow
    // accurately; without this, drops were silent and the "Stream Dropped"
    // counter stayed at 0 forever (latent observability bug).
    gEspNow->streamDroppedCount++;
  }
  gEspNow->streamReceivedCount++;
}

// FILE_START — initialize a file transfer; allocate PSRAM buffer + chunk bitmap.
// Rejects if a different sender already has an active transfer. Stale transfers
// (>30s) and same-sender restarts are tolerated by cleaning up first.
// Phase 4: route into the multi-slot file-transfer table. Single global
// gActiveFileTransfer pointer + gFileTransferLocked flag are gone; up to
// kFileSlots concurrent transfers from distinct (peerMac, msgId) pairs.
// Same-destination-path conflict rejected here via fileSlotsAllocate.
static void v4h_file_start(const V4RxCtx& ctx) {
  if (ctx.payloadLen < sizeof(V4PayloadFileStart)) return;
  const V4PayloadFileStart* fs = (const V4PayloadFileStart*)ctx.payload;

  const char* err = nullptr;
  FileTransferSlot* slot = fileSlotsAllocate(ctx.recv_info->src_addr,
                                              ctx.h->msgId,
                                              fs->filename,
                                              fs->fileSize,
                                              fs->chunkCount,
                                              fs->chunkSize,
                                              &err);
  if (!slot) {
    if (err && strcmp(err, kFileSlotErrTooBig) == 0) {
      ERROR_ESPNOWF("[V4_FILE] FILE_START rejected: '%s' is %lu bytes, exceeds the %lu MB transfer limit (from %s)",
                    fs->filename, (unsigned long)fs->fileSize,
                    (unsigned long)(kFileSlotMaxStreamSize / (1024UL * 1024UL)), ctx.deviceName);
    } else {
      ERROR_ESPNOWF("[V4_FILE] FILE_START rejected for '%s' from %s: %s",
                    fs->filename, ctx.deviceName, err ? err : "(unknown)");
    }
    return;
  }
  DEBUG_ESPNOWF("[V4_FILE_RX] FILE_START: %s (%lu bytes, %u chunks, chunkSize=%u) from %s",
               fs->filename, (unsigned long)fs->fileSize, fs->chunkCount, fs->chunkSize, ctx.deviceName);
}

// FILE_DATA — route the chunk to its owning slot in the multi-slot table.
// Original branch had inline `if (!isPaired) return true;` — using REQ_PAIRED
// flag yields identical behavior (silent drop for unpaired senders).
static void v4h_file_data(const V4RxCtx& ctx) {
  if (ctx.payloadLen < 3) return;
  FileTransferSlot* slot = fileSlotsFindByMsg(ctx.recv_info->src_addr, ctx.h->msgId);
  if (!slot) {
    DEBUG_ESPNOWF("[V4_FILE_RX] FILE_DATA ignored: no slot for msgId=%lu from %s",
                 (unsigned long)ctx.h->msgId, ctx.deviceName);
    return;
  }
  const V4PayloadFileData* fd = (const V4PayloadFileData*)ctx.payload;
  uint16_t dataLen = ctx.payloadLen - 2;

  // Streaming slot (file > 128 KB): espnow_task ONLY memcpy's the chunk into a
  // double-buffer — the flash write is deferred to cmd_exec so the RX drain is never
  // blocked on flash (that's what was dropping chunks at ~87). A gap / writer-
  // backpressure / queue-full aborts the transfer (cleanup is queued on cmd_exec by
  // the append); we just tell the fire-and-forget sender to stop.
  if (fileSlotsIsStreaming(slot)) {
    StreamAppendResult r = fileSlotsStreamAppend(slot, fd->chunkIndex, fd->data, dataLen);
    if (r == STREAM_APPEND_FAIL) {
      // Use the frame's src_addr (== this slot's peer) for the cancel so we don't
      // race cmd_exec, which may already be tearing the slot down.
      String senderMacStr = formatMacAddress(ctx.recv_info->src_addr);
      v4_send_file_cancel(ctx.recv_info->src_addr, ctx.h->msgId, FILE_CANCEL_INCOMPLETE);
      logFileTransferEvent((uint8_t*)ctx.recv_info->src_addr, senderMacStr.c_str(),
                           fileSlotsGetFilename(slot), MSG_FILE_RECV_FAILED);
    }
    return;
  }

  if (!fileSlotsWriteChunk(slot, fd->chunkIndex, fd->data, dataLen)) {
    return;  // bounds violation already logged inside the helper
  }
  uint16_t recv = fileSlotsGetReceivedChunks(slot);
  if ((recv % 10) == 0) {
    DEBUG_ESPNOWF("[V4_FILE_RX] Progress: %u/%u chunks, %lu/%lu bytes",
                 recv,
                 fileSlotsGetTotalChunks(slot),
                 (unsigned long)fileSlotsGetReceivedBytes(slot),
                 (unsigned long)fileSlotsGetTotalSize(slot));
  }
}

// cmd_exec finalize for a STREAMING receive (submitted by v4h_file_end). Drains any
// buffers still in flight, closes the .part, renames it into the inbox, ACKs the
// fire-and-forget sender, logs, and releases the slot — all OFF espnow_task so the RX
// drain is never blocked on flash. arg = the FileTransferSlot* (stable until released
// here; the slot is COMPLETING, so nothing else touches it). Mirrors the old inline
// FILE_END streaming publish, just moved to cmd_exec.
static void fileWriterFinalizeJob(void* arg) {
  FileTransferSlot* slot = (FileTransferSlot*)arg;
  if (!slot) return;
  // Snapshot identity before any teardown (the slot is wiped at the end).
  uint8_t  sndMac[6];
  memcpy(sndMac, fileSlotsGetSenderMac(slot), 6);
  char filename[64];
  strncpy(filename, fileSlotsGetFilename(slot), sizeof(filename) - 1);
  filename[sizeof(filename) - 1] = '\0';
  uint32_t recvBytes = fileSlotsGetReceivedBytes(slot);
  uint32_t msgId     = fileSlotsGetMsgId(slot);
  String   senderMacStr = formatMacAddress(sndMac);

  bool ok = fileSlotsStreamFinalizeWrite(slot);  // drain remaining buffers + close .part
  if (ok) {
    char senderToken[13];
    macToPathToken(sndMac, senderToken);
    char deviceDir[64];
    snprintf(deviceDir, sizeof(deviceDir), "/espnow/received/%s", senderToken);
    char filepath[160];
    snprintf(filepath, sizeof(filepath), "%s/%s", deviceDir, filename);
    AuthContext wrCtx = VFS::systemAuth(VFS::Scopes::ESPNOW_RECEIVED, "espnow.v4_file_stream_publish");
    {
      FsLockGuard guard("v4file.stream.publish");
      VFS::mkdirGuarded("/espnow/received", wrCtx);
      VFS::mkdirGuarded(deviceDir, wrCtx);
      ok = VFS::renameGuarded(fileSlotsGetPartPath(slot), filepath, wrCtx);
    }
    if (!ok) ERROR_ESPNOWF("[V4_FILE] stream publish rename failed -> %s", filepath);
  }

  if (ok) {
    BROADCAST_PRINTF("[V4_FILE] Complete (streamed): %s (%lu bytes)", filename, (unsigned long)recvBytes);
    logFileTransferEvent(sndMac, senderMacStr.c_str(), filename, MSG_FILE_RECV_SUCCESS);
#if ENABLE_OLED_DISPLAY
    {
      char toastMsg[40];
      snprintf(toastMsg, sizeof(toastMsg), "Received: %.28s", filename);
      oledToastShow(toastMsg);
    }
#endif
    if (gEspNow) gEspNow->fileTransfersReceived++;
    v4_send_ack(sndMac, msgId);
  } else {
    // Drained/closed/renamed but something failed — correct the fire-and-forget
    // sender instead of letting it assume success.
    v4_send_file_cancel(sndMac, msgId, FILE_CANCEL_WRITE_FAILED);
    logFileTransferEvent(sndMac, senderMacStr.c_str(), filename, MSG_FILE_RECV_FAILED);
  }
  fileSlotsRelease(slot);
}

// FILE_END — finalize the slot's transfer; route manifest/settings to bond
// processors, otherwise write to filesystem. Sends ACK back; releases slot.
// Slot identity: (peerMac, msgId) — the FILE_START's msgId is the transferId
// used by every chunk and the FILE_END itself.
static void v4h_file_end(const V4RxCtx& ctx) {
  FileTransferSlot* slot = fileSlotsFindByMsg(ctx.recv_info->src_addr, ctx.h->msgId);
  if (!slot) {
    ERROR_ESPNOWF("[V4_FILE] FILE_END for unknown msgId=%lu from %s — slot already released or never started",
                  (unsigned long)ctx.h->msgId, ctx.deviceName);
    return;
  }
  const V4PayloadFileEnd* fe = (const V4PayloadFileEnd*)ctx.payload;
  const uint8_t* sndMac    = fileSlotsGetSenderMac(slot);
  const char*    filename  = fileSlotsGetFilename(slot);
  const uint8_t* dataBuf   = fileSlotsGetBuffer(slot);
  uint32_t       recvBytes = fileSlotsGetReceivedBytes(slot);
  String senderMacStr = formatMacAddress(sndMac);

  DEBUG_ESPNOWF("[V4_FILE_RX] FILE_END: %s (%lu bytes, %u/%u chunks, success=%d)",
               filename, (unsigned long)recvBytes,
               fileSlotsGetReceivedChunks(slot),
               fileSlotsGetTotalChunks(slot),
               fe->success);

  // ---- Streaming slot (file > 128 KB): bytes are buffered to a .part on flash via
  // cmd_exec. FILE_END hands finalize (drain remaining buffers, close, rename, ACK,
  // release) to cmd_exec too, so espnow_task does no FS here. A streaming slot is
  // never a bond config file (those are small → RAM path below) → no special
  // processing: stream files are plain inbox files. dataBuf is null here. ----
  if (fileSlotsIsStreaming(slot)) {
    bool complete = fileSlotsIsComplete(slot);
    if (complete && fe->success && fileSlotsStreamBeginFinalize(slot)) {
      // Off-task finalize: the job owns drain + close + rename + ACK + release.
      if (!submitDeferredToCmdExec(fileWriterFinalizeJob, slot)) {
        ERROR_ESPNOWF("[V4_FILE] stream finalize submit failed (queue full) — aborting '%s'", filename);
        v4_send_file_cancel(sndMac, ctx.h->msgId, FILE_CANCEL_WRITE_FAILED);
        logFileTransferEvent((uint8_t*)sndMac, senderMacStr.c_str(), filename, MSG_FILE_RECV_FAILED);
        // Slot is COMPLETING; free it via a plain abort job, falling back to an
        // inline release only if that submit also fails.
        if (!submitDeferredToCmdExec(fileSlotsStreamAbortJob, slot)) fileSlotsRelease(slot);
      }
    } else {
      if (!complete) {
        BROADCAST_PRINTF("[V4_FILE] REJECTED incomplete stream '%s': %u/%u chunks, %lu/%lu bytes",
                     filename,
                     (unsigned)fileSlotsGetReceivedChunks(slot),
                     (unsigned)fileSlotsGetTotalChunks(slot),
                     (unsigned long)recvBytes,
                     (unsigned long)fileSlotsGetTotalSize(slot));
      }
      v4_send_file_cancel(sndMac, ctx.h->msgId, complete ? FILE_CANCEL_WRITE_FAILED : FILE_CANCEL_INCOMPLETE);
      logFileTransferEvent((uint8_t*)sndMac, senderMacStr.c_str(), filename, MSG_FILE_RECV_FAILED);
      fileSlotsStreamFail(slot);  // queue close/delete/free on cmd_exec
    }
    return;
  }

  if (!fileSlotsIsComplete(slot)) {
    BROADCAST_PRINTF("[V4_FILE] REJECTED incomplete transfer '%s': %u/%u chunks, %lu/%lu bytes",
                 filename,
                 (unsigned)fileSlotsGetReceivedChunks(slot),
                 (unsigned)fileSlotsGetTotalChunks(slot),
                 (unsigned long)recvBytes,
                 (unsigned long)fileSlotsGetTotalSize(slot));
    // Sender is fire-and-forget and would otherwise assume success — tell it.
    v4_send_file_cancel(sndMac, ctx.h->msgId, FILE_CANCEL_INCOMPLETE);
    logFileTransferEvent((uint8_t*)sndMac, senderMacStr.c_str(), filename, MSG_FILE_RECV_FAILED);
    fileSlotsRelease(slot);
    return;
  }

  if (fe->success && dataBuf) {
#if ENABLE_BONDED_MODE
    if (strcmp(filename, "_manifest_out.json") == 0) {
      String manifestStr((char*)dataBuf, recvBytes);
      processBondModeManifestResp((uint8_t*)sndMac, senderMacStr, manifestStr);
      BROADCAST_PRINTF("[V4_FILE] Manifest processed: %lu bytes", (unsigned long)recvBytes);
    } else if (strcmp(filename, "_settings_out.json") == 0) {
      DEBUG_ESPNOWF("[FILE_END] Detected settings file: %s (%lu bytes)", filename, (unsigned long)recvBytes);
      String settingsStr((char*)dataBuf, recvBytes);
      DEBUG_ESPNOWF("[FILE_END] Calling processBondSettings (settingsStr len=%d)", settingsStr.length());
      processBondSettings((uint8_t*)sndMac, senderMacStr, settingsStr);
      BROADCAST_PRINTF("[V4_FILE] Settings processed: %lu bytes", (unsigned long)recvBytes);
    } else if (strcmp(filename, "_schema_out.json") == 0) {
      DEBUG_ESPNOWF("[FILE_END] Detected schema file: %s (%lu bytes)", filename, (unsigned long)recvBytes);
      String schemaStr((char*)dataBuf, recvBytes);
      processBondSchema((uint8_t*)sndMac, senderMacStr, schemaStr);
      BROADCAST_PRINTF("[V4_FILE] Schema processed: %lu bytes", (unsigned long)recvBytes);
    } else
#endif // ENABLE_BONDED_MODE
    {
      char senderToken[13];
      macToPathToken(sndMac, senderToken);  // canonical PATH TOKEN form
      char deviceDir[64];
      snprintf(deviceDir, sizeof(deviceDir), "/espnow/received/%s", senderToken);
      char filepath[160];
      snprintf(filepath, sizeof(filepath), "%s/%s", deviceDir, filename);
      // Atomic publish: stage to a flat ".part-<msgId>" temp in the inbox root,
      // verify the FULL write, then rename into place. The rename is the only
      // mutation of `filepath`, so a crash / ENOSPC / short write never leaves a
      // truncated destination. (The old path wrote straight to `filepath` with
      // "w" — truncating on open — and ACKed success even when write() failed.)
      char tmpPath[48];
      snprintf(tmpPath, sizeof(tmpPath), "/espnow/received/.part-%08lx", (unsigned long)ctx.h->msgId);
      AuthContext wrCtx = VFS::systemAuth(VFS::Scopes::ESPNOW_RECEIVED, "espnow.v4_file_write");
      bool wrote = false;
      {
        FsLockGuard guard("v4file.write");
        // /espnow is created at boot; confine every inbound-file write to the
        // received inbox so a (peer-controlled) filename can never direct a write
        // outside it — defense-in-depth on top of normalize()'s ".." rejection.
        VFS::mkdirGuarded("/espnow/received", VFS::systemAuth(VFS::Scopes::ESPNOW_RECEIVED, "espnow.v4_file_write_mkdir"));
        VFS::mkdirGuarded(deviceDir, VFS::systemAuth(VFS::Scopes::ESPNOW_RECEIVED, "espnow.v4_file_write_mkdir"));
        File f = VFS::openGuarded(tmpPath, "w", wrCtx, true);
        if (f) {
          size_t wn = f.write(dataBuf, recvBytes);
          f.flush();
          f.close();
          if (wn == recvBytes) {
            wrote = VFS::renameGuarded(tmpPath, filepath, wrCtx);
            if (!wrote) {
              ERROR_ESPNOWF("[V4_FILE] rename %s -> %s failed", tmpPath, filepath);
              VFS::removeGuarded(tmpPath, wrCtx);
            }
          } else {
            ERROR_ESPNOWF("[V4_FILE] short write %u/%lu to %s",
                          (unsigned)wn, (unsigned long)recvBytes, tmpPath);
            VFS::removeGuarded(tmpPath, wrCtx);
          }
        } else {
          ERROR_ESPNOWF("[V4_FILE] cannot open staging file %s", tmpPath);
        }
      }
      if (!wrote) {
        // Don't ACK success; tell the fire-and-forget sender it actually failed.
        v4_send_file_cancel(sndMac, ctx.h->msgId, FILE_CANCEL_WRITE_FAILED);
        logFileTransferEvent((uint8_t*)sndMac, senderMacStr.c_str(), filename, MSG_FILE_RECV_FAILED);
        fileSlotsRelease(slot);
        return;
      }
      {
        BROADCAST_PRINTF("[V4_FILE] Complete: %s (%lu bytes)", filename, (unsigned long)recvBytes);

        if (strcmp(filename, "automations.json") == 0) {
          String senderName = getEspNowDeviceName(sndMac);
          if (senderName.length() == 0) senderName = formatMacAddress(sndMac);
          String jsonStr((char*)dataBuf, recvBytes);
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

    logFileTransferEvent((uint8_t*)sndMac, senderMacStr.c_str(), filename, MSG_FILE_RECV_SUCCESS);
#if ENABLE_OLED_DISPLAY
    {
      // On-device confirmation for any inbound file (OLED FS_GET download, push, etc.).
      // oledToastShow is thread-safe + self-marks dirty, so it surfaces regardless of
      // which screen is showing.
      char toastMsg[40];
      snprintf(toastMsg, sizeof(toastMsg), "Received: %.28s", filename);
      oledToastShow(toastMsg);
    }
#endif
    if (gEspNow) gEspNow->fileTransfersReceived++;
  } else {
    logFileTransferEvent((uint8_t*)sndMac, senderMacStr.c_str(), filename, MSG_FILE_RECV_FAILED);
  }

  v4_send_ack(ctx.recv_info->src_addr, ctx.h->msgId);
  fileSlotsRelease(slot);
}

// Phase 4: a peer we sent a file TO reports the transfer failed on its end
// (timeout, staging-write failure, or missing chunks). Our sender is
// fire-and-forget and already logged "sent", so this corrects the record:
// surface it to the operator and count a failed send. msgId echoes the
// original FILE_START so the operator can correlate.
static void v4h_file_cancel(const V4RxCtx& ctx) {
  uint8_t reason = 0;
  if (ctx.payload && ctx.payloadLen >= sizeof(V4PayloadFileCancel)) {
    reason = ((const V4PayloadFileCancel*)ctx.payload)->reason;
  }
  const char* why = (reason == FILE_CANCEL_TIMEOUT)      ? "timeout" :
                    (reason == FILE_CANCEL_WRITE_FAILED) ? "write failed" :
                    (reason == FILE_CANCEL_INCOMPLETE)   ? "incomplete" : "unknown";
  String macStr = formatMacAddress(ctx.recv_info->src_addr);
  BROADCAST_PRINTF("[V4_FILE_TX] file to %s FAILED on receiver (msgId=%lu): %s",
                   macStr.c_str(), (unsigned long)ctx.h->msgId, why);
  logFileTransferEvent((uint8_t*)ctx.recv_info->src_addr, macStr.c_str(), "(remote)", MSG_FILE_SEND_FAILED);
}

// ----- Handler table ------
// Stable static table; lookup is linear over a small N. Adding a new opcode
// is one row here plus its v4h_<name> function. No edits to the dispatcher.
//
// THREADING / DEFERRAL RULE (Seam 2 invariant):
// Handlers run INLINE on espnow_task, whose sole job is to drain the RX ring
// fast. A handler may run inline ONLY if it is (a) bounded-time, (b) allocation-
// light, and (c) does NO filesystem I/O, NO deserializeJson of attacker-sized
// input, and NO heavy crypto beyond a single HMAC verify. Anything heavier MUST
// snapshot its inputs into PSRAM and hand off via submitDeferredToCmdExec(), so
// the slow work runs on cmd_exec_task and never stalls RX.
//
// Handlers that DEFER today (heavy crypto / FS+JSON): USER_SYNC, SESSION_OPEN,
// SESSION_CONFIRM, SESSION_REKEY. (v4h_cmd also "defers" by snapshotting into
// gEspNow->deferredCmd* and letting the command run off-task.)
//
// KNOWN INLINE EXCEPTIONS to the rule (accepted, not oversights):
//   * KEY_EX_HELLO/REPLY/CONFIRM call peerIdentityPersist() (small FS write).
//     Bounded + once-per-pairing, so the RX-stall cost is negligible.
//   * v4h_file_end runs processBondSettings/manifest caching + a deserializeJson
//     of automations.json inline — the one genuine violation. Left inline
//     deliberately: it is HW-validated and the bond SEND side still does the same
//     heavy FS in processMeshHeartbeats (the super-loop), so deferring file_end
//     alone buys no clean RX task. Revisit together with the super-loop split.
static const V4OpcodeEntry kV4HandlerTable[] = {
  // Phase 3.3: KEY_EX handshake. NOT REQ_PAIRED — these *establish* pairing.
  // Handler verifies HMAC via mesh bootstrap key; loud-rejects bad frames.
  { ESPNOW_V4_TYPE_KEY_EX_HELLO,     0,                                                    v4hKeyExHello         },
  { ESPNOW_V4_TYPE_KEY_EX_REPLY,     0,                                                    v4hKeyExReply         },
  { ESPNOW_V4_TYPE_KEY_EX_CONFIRM,   0,                                                    v4hKeyExConfirm       },
  // Phase 3.4: SESSION establishment. NOT REQ_PAIRED for the same reason — sessions
  // are negotiated between peers that have completed KEY_EX (verified via
  // Ed25519 signature against stored long-term pubkey, no LMK dependency).
  { ESPNOW_V4_TYPE_SESSION_OPEN,     0,                                                    v4hSessionOpen        },
  { ESPNOW_V4_TYPE_SESSION_CONFIRM,  0,                                                    v4hSessionConfirm     },
  // Phase 3.6: SESSION_REKEY refreshes AEAD keys for an active session. NOT
  // REQ_PAIRED — handler verifies Ed25519 sig against the peer's stored
  // long-term pubkey (same trust path as SESSION_OPEN/CONFIRM).
  { ESPNOW_V4_TYPE_SESSION_REKEY,    0,                                                    v4hSessionRekey       },
  // TIME_SYNC moves the device clock — REQ_AUTHENTICATED to defend against
  // on-air spoofing. Senders go through v4_send_encrypted_or_queue (unicast)
  // or v4_broadcast (broadcast, BROADCAST_AUTH HMAC). Plaintext TIME_SYNC
  // from any source is silently dropped here.
  { ESPNOW_V4_TYPE_TIME_SYNC,        V4_OPC_FLAG_REQ_AUTHENTICATED,                        v4h_time_sync         },
  { ESPNOW_V4_TYPE_TEXT,             0,                                                    v4h_text              },
  { ESPNOW_V4_TYPE_BOOT,             0,                                                    v4h_text              },  // boot/online notice; v4h_text tags it MSG_SYSTEM_EVENT
  { ESPNOW_V4_TYPE_CMD,              V4_OPC_FLAG_REQ_PAIRED,                               v4h_cmd               },
  { ESPNOW_V4_TYPE_CMD_RESP,         0,                                                    v4h_cmd_resp          },
  { ESPNOW_V4_TYPE_HEARTBEAT,        0,                                                    v4h_heartbeat         },
  { ESPNOW_V4_TYPE_SENSOR_STATUS,    0,                                                    v4h_sensor_status     },
  { ESPNOW_V4_TYPE_SENSOR_BROADCAST, 0,                                                    v4h_sensor_broadcast  },
  { ESPNOW_V4_TYPE_TOPO_REQ,         0,                                                    v4h_topo_req          },
  { ESPNOW_V4_TYPE_TOPO_START,       0,                                                    v4h_topo_start        },
  { ESPNOW_V4_TYPE_TOPO_PEER,        0,                                                    v4h_topo_peer         },
  { ESPNOW_V4_TYPE_USER_SYNC,        0,                                                    v4h_user_sync         },
  { ESPNOW_V4_TYPE_METADATA_REQ,     0,                                                    v4h_metadata_req      },
  { ESPNOW_V4_TYPE_METADATA_RESP,    0,                                                    v4h_metadata_resp_push},
  { ESPNOW_V4_TYPE_METADATA_PUSH,    0,                                                    v4h_metadata_resp_push},
  { ESPNOW_V4_TYPE_STREAM,           0,                                                    v4h_stream            },
  { ESPNOW_V4_TYPE_FILE_START,       V4_OPC_FLAG_REQ_PAIRED,                               v4h_file_start        },
  { ESPNOW_V4_TYPE_FILE_DATA,        V4_OPC_FLAG_REQ_PAIRED,                               v4h_file_data         },
  { ESPNOW_V4_TYPE_FILE_END,         V4_OPC_FLAG_REQ_PAIRED,                               v4h_file_end          },
  { ESPNOW_V4_TYPE_FILE_CANCEL,      V4_OPC_FLAG_REQ_PAIRED,                               v4h_file_cancel       },
  // Phase 5: SUBSCRIBE_UPDATE — peer tells us which event categories they
  // want from us. REQ_PAIRED so unknown peers can't pollute our broadcast
  // gating (and to ensure peerIdentitySetSubscriptions finds the slot).
  { ESPNOW_V4_TYPE_SUBSCRIBE_UPDATE, V4_OPC_FLAG_REQ_PAIRED,                               v4h_subscribe_update  },
  // Remote filesystem (FsList) is a BASE ESP-NOW capability — browse/stat/fetch a
  // paired peer's files over an active encrypted session. NOT bond-gated (bonding
  // piggybacks on it). Gated only by REQ_PAIRED + REQ_SESSION_ENC: any securely
  // paired peer with a live session may enumerate/pull files (served under SYSTEM
  // identity). Both REQ and REPLY/ACK rows live here so a requester whose own bond
  // mode is off still accepts the peer's replies.
  { ESPNOW_V4_TYPE_FS_LIST_REQ,      V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_SESSION_ENC,   v4h_fs_list_req       },
  { ESPNOW_V4_TYPE_FS_LIST_REPLY,    V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_SESSION_ENC,   v4h_fs_list_reply     },
  { ESPNOW_V4_TYPE_FS_STAT_REQ,      V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_SESSION_ENC,   v4h_fs_stat_req       },
  { ESPNOW_V4_TYPE_FS_STAT_REPLY,    V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_SESSION_ENC,   v4h_fs_stat_reply     },
  { ESPNOW_V4_TYPE_FS_GET_REQ,       V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_SESSION_ENC,   v4h_fs_get_req        },
  { ESPNOW_V4_TYPE_FS_GET_ACK,       V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_SESSION_ENC,   v4h_fs_get_ack        },
#if ENABLE_BONDED_MODE
  { ESPNOW_V4_TYPE_BOND_HEARTBEAT,   V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_BOND_MODE | V4_OPC_FLAG_REQ_SESSION_ENC,   v4h_bond_heartbeat    },
  { ESPNOW_V4_TYPE_STREAM_CTRL,      V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_BOND_MODE | V4_OPC_FLAG_REQ_SESSION_ENC,   v4h_stream_ctrl       },
  { ESPNOW_V4_TYPE_BOND_CAP_REQ,     V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_BOND_MODE | V4_OPC_FLAG_REQ_SESSION_ENC,   v4h_bond_cap_req      },
  { ESPNOW_V4_TYPE_BOND_CAP_RESP,    V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_BOND_MODE | V4_OPC_FLAG_REQ_SESSION_ENC,   v4h_bond_cap_resp     },
  { ESPNOW_V4_TYPE_SENSOR_DATA,      V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_BOND_MODE | V4_OPC_FLAG_REQ_SESSION_ENC,   v4h_sensor_data       },
  { ESPNOW_V4_TYPE_SETTINGS_REQ,     V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_BOND_MODE | V4_OPC_FLAG_REQ_SESSION_ENC,   v4h_settings_req      },
  { ESPNOW_V4_TYPE_SCHEMA_REQ,       V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_BOND_MODE | V4_OPC_FLAG_REQ_SESSION_ENC,   v4h_schema_req        },
  { ESPNOW_V4_TYPE_BOND_STATUS_REQ,  V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_BOND_MODE | V4_OPC_FLAG_REQ_SESSION_ENC,   v4h_bond_status_req   },
  { ESPNOW_V4_TYPE_BOND_STATUS_RESP, V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_BOND_MODE | V4_OPC_FLAG_REQ_SESSION_ENC,   v4h_bond_status_resp  },
  { ESPNOW_V4_TYPE_MANIFEST_REQ,     V4_OPC_FLAG_REQ_PAIRED | V4_OPC_FLAG_REQ_BOND_MODE | V4_OPC_FLAG_REQ_SESSION_ENC,   v4h_manifest_req      },
  // (FS_LIST / FS_STAT / FS_GET rows moved above — now base ESP-NOW, not bond-gated.)
#endif
};
static constexpr size_t kV4HandlerTableSize = sizeof(kV4HandlerTable) / sizeof(kV4HandlerTable[0]);

// Lookup by opcode; nullptr if not migrated yet (falls through to legacy ladder).
static const V4OpcodeEntry* v4_dispatch_lookup(uint8_t opcode) {
  for (size_t i = 0; i < kV4HandlerTableSize; i++) {
    if (kV4HandlerTable[i].opcode == opcode) return &kV4HandlerTable[i];
  }
  return nullptr;
}

// Centralized reassembly cleanup that each migrated branch used to do inline.
static void v4_dispatch_post_cleanup(const esp_now_recv_info* recv_info, const EspNowV4Header* h) {
  if (h->fragCount > 1) {
    for (int i = 0; i < V4_REASM_MAX; i++) {
      if (gV4Reasm[i].active && gV4Reasm[i].msgId == h->msgId &&
          memcmp(gV4Reasm[i].src, recv_info->src_addr, 6) == 0) {
        v4_reasm_reset(gV4Reasm[i]);
        break;
      }
    }
  }
}

// Returns true if the dispatch table claimed the frame; false to fall through
// to the legacy if-ladder for opcodes not yet migrated.
static bool v4_dispatch_table_try(const esp_now_recv_info* recv_info, const EspNowV4Header* h,
                                  const uint8_t* payload, uint16_t payloadLen,
                                  bool isPaired, bool isAuthenticated,
                                  bool isSessionEncrypted, const char* deviceName) {
  const V4OpcodeEntry* e = v4_dispatch_lookup(h->type);
  if (!e) return false;
  if ((e->flags & V4_OPC_FLAG_REQ_PAIRED) && !isPaired) {
    // Match original behavior: branch was gated `&& isPaired`, so unpaired frames
    // for this opcode were silently dropped (no other branch matched). Same here.
    v4_dispatch_post_cleanup(recv_info, h);
    return true;
  }
  if ((e->flags & V4_OPC_FLAG_REQ_BOND_MODE) && !gSettings.bondModeEnabled) {
    // Bond-mode-only opcodes were gated `&& gSettings.bondModeEnabled` originally.
    // When bond mode is off, drop silently (no other branch handles these types).
    v4_dispatch_post_cleanup(recv_info, h);
    return true;
  }
  if ((e->flags & V4_OPC_FLAG_REQ_AUTHENTICATED) && !isAuthenticated) {
    // Opcode-handler insists on a cryptographically-authenticated frame
    // (SESSION_FRAME unwrap OR BROADCAST_AUTH HMAC). Plain plaintext from
    // any source — including paired peers pre-KEY_EX — is dropped to defend
    // against on-air spoofing of state-mutating opcodes (TIME_SYNC, etc).
    WARN_ESPNOWF("[V4_RX] type=%u from %02X:%02X:%02X:%02X:%02X:%02X dropped: "
                 "REQ_AUTHENTICATED set but frame was plaintext (no SESSION_FRAME "
                 "or BROADCAST_AUTH)",
                 h->type,
                 recv_info->src_addr[0], recv_info->src_addr[1], recv_info->src_addr[2],
                 recv_info->src_addr[3], recv_info->src_addr[4], recv_info->src_addr[5]);
    v4_dispatch_post_cleanup(recv_info, h);
    return true;
  }
  if ((e->flags & V4_OPC_FLAG_REQ_SESSION_ENC) && !isSessionEncrypted) {
    // Bond opcode that arrived plaintext (or only BROADCAST_AUTH). All bond
    // traffic must ride a SESSION_FRAME — drop loudly. A legitimate sender uses
    // bondSendEncrypted, so this only fires on spoofed/downgraded frames.
    WARN_ESPNOWF("[V4_RX] type=%u from %02X:%02X:%02X:%02X:%02X:%02X dropped: "
                 "REQ_SESSION_ENC set but frame was not session-encrypted",
                 h->type,
                 recv_info->src_addr[0], recv_info->src_addr[1], recv_info->src_addr[2],
                 recv_info->src_addr[3], recv_info->src_addr[4], recv_info->src_addr[5]);
    v4_dispatch_post_cleanup(recv_info, h);
    return true;
  }

  // Phase 3.5 task #51 — SESSION_FRAME unwrap was previously performed here,
  // gated on h->flags & SESSION_FRAME. It now lives upstream in
  // v4_try_handle_incoming (right after BROADCAST_AUTH), so encrypted-chunked
  // payloads can be unwrapped per-fragment before the reassembler. By the
  // time we reach this point, `payload` is already plaintext and the
  // SESSION_FRAME flag has been cleared in the local header copy.
  V4RxCtx ctx{recv_info, h, payload, payloadLen, isPaired, isAuthenticated, isSessionEncrypted, deviceName};
  e->handler(ctx);
  v4_dispatch_post_cleanup(recv_info, h);
  return true;
}

static bool v4_try_handle_incoming(const esp_now_recv_info* recv_info, const uint8_t* data, int len) {
  if (!recv_info || !data || len < (int)sizeof(EspNowV4Header)) return false;
  const EspNowV4Header* h = (const EspNowV4Header*)data;
  if (h->magic != (uint16_t)ESPNOW_V4_MAGIC) return false;
  if (h->ver != ESPNOW_V4_VERSION || h->headerLen != sizeof(EspNowV4Header)) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_RX] REJECTED: ver=%u headerLen=%u (expected ver=%u headerLen=%u)",
           h->ver, h->headerLen, (unsigned)ESPNOW_V4_VERSION, (unsigned)sizeof(EspNowV4Header));
    return true;
  }
  // V4 derives payloadLen from the ESPNOW radio length minus the fixed header
  // size (V3 carried a redundant payloadLen field in the header; V4 saves
  // those 2 bytes since the radio already gives us the frame length).
  const uint8_t* payload = data + sizeof(EspNowV4Header);
  uint16_t payloadLen = (uint16_t)(len - sizeof(EspNowV4Header));
  // SESSION_FRAME-wrapped payloads (Phase 3.5) use the AEAD tag as their
  // integrity check; the CRC field is zeroed on the wire and would never
  // match the encrypted payload, so skip CRC validation for them.
  const bool isSessionFrame = (h->flags & ESPNOW_V4_FLAG_SESSION_FRAME) != 0;
  if (!isSessionFrame && payloadLen > 0 &&
      v4_crc16_ccitt(payload, payloadLen) != h->crc16) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_RX] REJECTED: CRC mismatch (got=0x%04X expected=0x%04X) payloadLen=%u",
           v4_crc16_ccitt(payload, payloadLen), h->crc16, payloadLen);
    return true;
  }

  // === Mesh fingerprint validation (Phase 2) ===
  // fingerprint=0 is "no mesh scope" — allowed for compatibility (e.g.,
  // first-boot devices that haven't yet configured a mesh). Non-zero
  // fingerprints must match one of our enabled meshes; mismatches drop
  // silently per V4 plan decision #5 (no log spam from neighboring fleets).
  if (h->meshFingerprint != 0 && meshByFingerprint(h->meshFingerprint) == nullptr) {
    // Silent drop. Frame is for a mesh we're not a member of.
    return true;
  }

  // 2026-05-19: track whether the frame proves any cryptographic key
  // possession. Set true below when either BROADCAST_AUTH HMAC verifies OR
  // SESSION_FRAME AEAD unwrap succeeds. Plaintext frames leave it false.
  // Plumbed into V4RxCtx so per-opcode handlers (and the dispatch-table
  // REQ_AUTHENTICATED gate) can refuse state-mutating frames that weren't
  // authenticated.
  bool wasAuthenticated = false;
  // Narrower than wasAuthenticated: true only for an AEAD-decrypted
  // SESSION_FRAME (confidential). BROADCAST_AUTH leaves this false (it's
  // authenticated plaintext). Drives the honest "encrypted" reporting below.
  bool wasSessionEncrypted = false;

  // === Phase 3.5 task #32 — BROADCAST_AUTH HMAC verification ===
  // If the BROADCAST_AUTH flag is set, the last 32 bytes of the payload are
  // an HMAC-SHA256 tag keyed by the mesh group key. Verify before any
  // dispatch — forged broadcasts drop silently here. After verification we
  // strip the tag so downstream handlers see only the core payload.
  if (h->flags & ESPNOW_V4_FLAG_BROADCAST_AUTH) {
    if (h->fragCount > 1) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_RX] BROADCAST_AUTH cannot be fragmented — dropping");
      return true;
    }
    if (payloadLen < ESPNOW_V4_BROADCAST_AUTH_TAG_LEN) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_RX] BROADCAST_AUTH payload too short for tag (%u) — dropping",
             payloadLen);
      return true;
    }
    if (h->meshFingerprint == 0) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_RX] BROADCAST_AUTH requires mesh fingerprint — dropping");
      return true;
    }
    const MeshDerivedKeys* mk = meshKeysFindByFingerprint(h->meshFingerprint);
    if (!mk || !mk->valid) {
      // Mesh known (passed fingerprint check above) but no key cached. Could
      // happen during early-boot before passphrase stretch completes.
      // Silent drop — sender will retry on next heartbeat tick.
      return true;
    }
    uint16_t corePayloadLen = (uint16_t)(payloadLen - ESPNOW_V4_BROADCAST_AUTH_TAG_LEN);
    const uint8_t* receivedTag = payload + corePayloadLen;
    uint8_t expectedTag[32];
    if (!espnowCryptoHmacSha256(expectedTag,
                                 mk->groupKey, 32,
                                 reinterpret_cast<const uint8_t*>(h), 30,
                                 payload, corePayloadLen,
                                 nullptr, 0)) {
      ERROR_ESPNOWF("[V4_RX] BROADCAST_AUTH HMAC compute failed (sessionId=N/A, broadcast)");
      return true;
    }
    if (sodium_memcmp(expectedTag, receivedTag, 32) != 0) {
      WARN_ESPNOWF("[V4_RX] BROADCAST_AUTH HMAC mismatch from %02X:%02X:%02X:%02X:%02X:%02X — "
                   "forged or wrong group key, dropping",
                   recv_info->src_addr[0], recv_info->src_addr[1], recv_info->src_addr[2],
                   recv_info->src_addr[3], recv_info->src_addr[4], recv_info->src_addr[5]);
      return true;
    }
    // Tag verified — strip it so downstream handlers see only the real payload.
    payloadLen = corePayloadLen;
    wasAuthenticated = true;
  }

  // === Phase 3.5 task #51 — SESSION_FRAME per-fragment early unwrap ===
  //
  // For fragmented encrypted sends, each fragment carries its own AEAD seal
  // (the cipher cannot be sliced across frames — AEAD is all-or-nothing). We
  // decrypt each fragment HERE, before the reassembly path below, so the
  // reassembler accumulates plaintext slices. A single-fragment SESSION_FRAME
  // (fragCount == 1) is also unwrapped here, which moves the previous
  // dispatch-time unwrap forward; the dispatch path is now strictly
  // plaintext.
  //
  // We mutate a local header copy to clear the SESSION_FRAME flag for
  // downstream code (which checks h->flags & SESSION_FRAME to decide whether
  // to unwrap again). The original wire-buffer header is left untouched.
  //
  // sessionUnwrapFrame performs replay-window insertion per fragment —
  // fragments and their retries all consume distinct frameSeqs, so the
  // 64-slot window only ever sees forward motion.
  EspNowV4Header headerCopy;
  // PSRAM-backed plaintext scratch for SESSION_FRAME AEAD unwrap. Single
  // shared buffer is safe because v4_try_handle_incoming runs ONLY on
  // espnow_task (called from processMeshHeartbeats's RX ring drain — see
  // System_ESPNow.cpp:7280; the raw ESP-NOW recv callback only pushes to the
  // ring, it does not call this function). Sequential iteration through the
  // ring guarantees no overlap between invocations. Moving this off-stack
  // frees ~218 B of espnow_task stack on every encrypted RX — important
  // because the call chain at dispatch time can be deep (v4_send_ack →
  // v4_send_frame's own 250 B frame[] buffer is still on stack, plus the
  // handler's own frame). Pairs with the FS_LIST defer-to-cmd_exec refactor
  // (see System_ESPNow_FsList.cpp::captureDeferred) — same stack-relief
  // theme.
  EXT_RAM_BSS_ATTR static uint8_t plainBuf[ESPNOW_V4_MAX_PAYLOAD];
  if (h->flags & ESPNOW_V4_FLAG_SESSION_FRAME) {
    SessionState* s = sessionFindBySessionId(h->sessionId, recv_info->src_addr);
    if (!s || (s->state != SESSION_ACTIVE && s->state != SESSION_REKEYING)) {
      WARN_ESPNOWF("[V4_RX] SESSION_FRAME no active session for sessionId=%u "
                   "from %02X:%02X:%02X:%02X:%02X:%02X — dropping (frag %u/%u)",
                   (unsigned)h->sessionId,
                   recv_info->src_addr[0], recv_info->src_addr[1], recv_info->src_addr[2],
                   recv_info->src_addr[3], recv_info->src_addr[4], recv_info->src_addr[5],
                   h->fragIndex + 1, h->fragCount);
      return true;
    }
    uint16_t plainLen = 0;
    if (!sessionUnwrapFrame(s, h, payload, payloadLen, plainBuf, &plainLen)) {
      // sessionUnwrapFrame already logged the reason (AEAD-fail or replay).
      // (Self-heal of a desynced session is TX-driven — see sendStatusSweep in
      // System_ESPNow_Sessions.cpp — so received frames can't trigger it.)
      return true;
    }
    // Acknowledge requested ACK *only* for authenticated frames (we mustn't
    // ACK a forged SESSION_FRAME — the AEAD verify is the gate). The plain-
    // text ACK-send below still fires for non-SESSION frames; we replicate
    // the per-fragment-or-not branch here for clarity.
    if (h->flags & ESPNOW_V4_FLAG_ACK_REQ) {
      if (h->fragCount > 1) {
        v4_send_frag_ack(recv_info->src_addr, h->msgId, h->fragIndex, h->fragCount);
      } else {
        v4_send_ack(recv_info->src_addr, h->msgId);
      }
    }
    headerCopy = *h;
    headerCopy.flags &= ~ESPNOW_V4_FLAG_SESSION_FRAME;
    headerCopy.flags &= ~ESPNOW_V4_FLAG_ACK_REQ;  // already ACKed above
    h = &headerCopy;
    payload = plainBuf;
    payloadLen = plainLen;
    wasAuthenticated = true;
    wasSessionEncrypted = true;  // AEAD-decrypted — genuinely confidential
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_RX] SESSION_FRAME unwrapped: type=%u msgId=%lu frag=%u/%u plainLen=%u",
           h->type, (unsigned long)h->msgId, h->fragIndex + 1, h->fragCount, plainLen);
  }

  // === V4 FRAGMENTATION REASSEMBLY ===
  //
  // EXCLUDE ACK frames. v4_send_frag_ack() encodes the ORIGINAL message's
  // fragIndex/fragCount into the ACK frame's header so the sender can match
  // a per-fragment ACK waiter (gV4FragAckWait[].fragIndex). An ACK is never
  // itself fragmented — it's a single 0-length frame. Without the type guard
  // below, a frag-ACK (e.g. type=ACK msgId=38 fragIdx=0 fragCount=2) falls
  // into this reassembly block, gets stored as a phantom "fragment 0", and
  // NEVER reaches the ACK handler at the `h->type == ESPNOW_V4_TYPE_ACK`
  // branch further down — so the matching V4FragAckWait slot never flips to
  // acked and the V4_ENC_FRAG_TX sender times out + retries 3× + aborts.
  // That broke every encrypted send larger than one frame (notably the
  // 368-byte FS_LIST_REPLY → "io_error" in the bonded file browser). The
  // per-fragment ACK machinery (waiter table with a fragIndex field, the
  // ACK handler's fragIdx match, its fragCount>1 reassembly-cleanup branch)
  // was all already written for exactly this path — it just couldn't be
  // reached. This guard lets ACKs fall through to it.
  // TEXT is deliberately excluded from device-side reassembly: long text is
  // stored per-fragment (each fragment becomes its own small history record,
  // tagged msgId/fragIndex/fragCount) and reassembled CLIENT-side. This keeps
  // the device from ever materializing a multi-KB lump in RAM. So multi-frame
  // TEXT falls through to the normal per-frame dispatch below; v4h_text stashes
  // each fragment's tags and the drain stores them as separate pieces.
  if (h->fragCount > 1 && h->type != ESPNOW_V4_TYPE_ACK && h->type != ESPNOW_V4_TYPE_TEXT) {
    // Multi-fragment message - reassemble
    if (gEspNow) { gEspNow->routerMetrics.v4FragRx++; }
    
    char srcMac[18];
    formatMacAddressBuf(recv_info->src_addr, srcMac, sizeof(srcMac));
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] ==============================");
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] Fragment received from %s", srcMac);
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] Fragment %u/%u msgId=%lu type=%u len=%u",
           h->fragIndex + 1, h->fragCount, (unsigned long)h->msgId, h->type, payloadLen);
    
    // Run GC
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] Running reassembly GC...");
    v4_reasm_gc(millis());
    
    // Find or allocate reassembly buffer
    V4ReasmEntry* e = v4_reasm_find_or_alloc(recv_info->src_addr, h->msgId, h->type, h->fragCount);
    if (!e) {
      WARN_ESPNOWF("[V4_FRAG_RX] No reassembly slot available - all %u slots in use", V4_REASM_MAX);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] ==============================");
      return true;
    }
    
    bool isNewEntry = (e->received == 0);
    if (isNewEntry) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] Allocated new reassembly buffer: bufSize=%u", e->bufferSize);
    } else {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] Using existing reassembly buffer: %u/%u fragments already received",
             e->received, e->fragCount);
    }
    
    // Store fragment if not duplicate
    if (h->fragIndex >= h->fragCount) {
      WARN_ESPNOWF("[V4_FRAG_RX] Invalid fragment index %u (max %u)", h->fragIndex, h->fragCount - 1);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] ==============================");
      return true;
    }
    
    if (e->have[h->fragIndex]) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] Duplicate fragment %u - ignoring", h->fragIndex + 1);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] ==============================");
      return true;
    }
    
    // Copy fragment data to buffer
    uint16_t offset = h->fragIndex * V4_MAX_FRAGMENT_PAYLOAD;
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] Storing at offset=%u len=%u (bufSize=%u)",
           offset, payloadLen, e->bufferSize);
    if (offset + payloadLen > e->bufferSize) {
      WARN_ESPNOWF("[V4_FRAG_RX] Fragment overflow: offset=%u len=%u bufSize=%u", offset, payloadLen, e->bufferSize);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] Resetting corrupted reassembly buffer");
      v4_reasm_reset(*e);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] ==============================");
      return true;
    }
    
    memcpy(e->buffer + offset, payload, payloadLen);
    e->have[h->fragIndex] = true;
    e->received++;
    e->lastUpdateMs = millis();
    
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] Fragment %u stored successfully", h->fragIndex + 1);
    
    // Check if complete
    if (e->received < e->fragCount) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] Progress: %u/%u fragments received",
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
          DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] Still waiting for fragments: %s", missing);
        }
      }
      
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] ==============================");
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
        reassembledSize += V4_MAX_FRAGMENT_PAYLOAD;
      }
    }
    
    unsigned long totalTime = millis() - (e->lastUpdateMs - (millis() - e->lastUpdateMs));
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] ✓✓✓ REASSEMBLY COMPLETE ✓✓✓");
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] All %u fragments received! Total size: %u bytes",
           e->fragCount, reassembledSize);
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] Proceeding to handle reassembled message type=%u", h->type);
    if (gEspNow) { gEspNow->routerMetrics.v4FragRxCompleted++; }
    
    // Update pointers to point to reassembled buffer
    payload = e->buffer;
    payloadLen = reassembledSize;
    
    // CRITICAL: Mark this reassembly entry for cleanup after message processing
    // We can't free it immediately because payload points to e->buffer
    // The cleanup happens at the end of this function before returning
    // Note: Store the entry pointer for cleanup (h->fragCount check at end will handle it)
  }
  
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_RX] ========================================");
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_RX] V4 BINARY MESSAGE RECEIVED");
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_RX] Type=%u MsgId=%lu PayloadLen=%u Flags=0x%02X",
         h->type, (unsigned long)h->msgId, payloadLen, h->flags);
  
  // Handle ACK (no dedup needed)
  if (h->type == ESPNOW_V4_TYPE_ACK) {
    char srcMac[18];
    formatMacAddressBuf(recv_info->src_addr, srcMac, sizeof(srcMac));
    DEBUGF(DEBUG_ESPNOW_CORE, "[V4_ACK_RX] ACK received from %s for msgId=%lu fragIdx=%u fragCnt=%u",
           srcMac, (unsigned long)h->msgId, h->fragIndex, h->fragCount);
    
    // Update peer health ACK tracking
    noteMeshPeerRxActivity(recv_info->src_addr, EspNowMeshRxKind::Ack);
    
    // Check broadcast tracker
    broadcast_tracker_record_ack(h->msgId, recv_info->src_addr);

    // Phase 3.5 task #49 — mark any tracked CLI-originated send as delivered
    // so the web UI's chat bubble can transition from ✓ Sent to ✓✓ Delivered
    // on the next /api/espnow/messages poll cycle.
    sendStatusMarkDelivered(h->msgId, recv_info->src_addr);
    
    // Check V3 fragment ACK waiters. Same lock as the sender's alloc/free
    // (v4_frag_ack_alloc) so this scan can't match a slot mid-reclaim. Runs on
    // espnow_task; the brief hold may make a concurrent sender's seal wait a
    // few microseconds — never the reverse (no deadlock: this path doesn't
    // block while holding the lock).
    bool foundV3 = false;
    {
      EspNowTxGuard g("fragAckMark");
      for (int i = 0; i < V4_FRAG_ACK_WAIT_MAX; i++) {
        if (gV4FragAckWait[i].active && gV4FragAckWait[i].msgId == h->msgId &&
            gV4FragAckWait[i].fragIndex == h->fragIndex) {
          gV4FragAckWait[i].acked = true;
          foundV3 = true;
          DEBUGF(DEBUG_ESPNOW_CORE, "[V4_ACK_RX] Matched V3 fragment ACK waiter slot %d (msgId=%lu fragIdx=%u)",
                 i, (unsigned long)h->msgId, h->fragIndex);
          break;
        }
      }
    }

    // ESP-NOW App page ping bookkeeping — see espnowAppPingStart below.
    // One in-flight slot; we only match if the msgId AND source MAC line up.
    // Stays in this branch (no early return above) because the per-peer
    // health update + tracker recording above are still legitimate side
    // effects of an ACK regardless of whether anyone is pinging.
    extern void espnowAppPingNoteAck(const uint8_t* src, uint32_t msgId);
    espnowAppPingNoteAck(recv_info->src_addr, h->msgId);

    if (!foundV3) {
      DEBUGF(DEBUG_ESPNOW_CORE, "[V4_ACK_RX] No matching ACK waiter found (msgId=%lu fragIdx=%u)",
             (unsigned long)h->msgId, h->fragIndex);
    }
    
    // Cleanup reassembly buffer if this was a fragmented message
    if (h->fragCount > 1) {
      for (int i = 0; i < V4_REASM_MAX; i++) {
        if (gV4Reasm[i].active && gV4Reasm[i].msgId == h->msgId && 
            memcmp(gV4Reasm[i].src, recv_info->src_addr, 6) == 0) {
          v4_reasm_reset(gV4Reasm[i]);
          break;
        }
      }
    }
    return true;
  }
  
  // Send ACK if requested
  if (h->flags & ESPNOW_V4_FLAG_ACK_REQ) {
    // For fragmented messages, send fragment-specific ACK
    if (h->fragCount > 1) {
      v4_send_frag_ack(recv_info->src_addr, h->msgId, h->fragIndex, h->fragCount);
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_ACK] Sent ACK for fragment %u/%u msgId=%lu",
             h->fragIndex + 1, h->fragCount, (unsigned long)h->msgId);
    } else {
      v4_send_ack(recv_info->src_addr, h->msgId);
    }
  }
  
  // Dedup check
  // The streaming-response family (STREAM, STREAM_CTRL, CMD_RESP) intentionally
  // reuses msgId=cmdMsgId for correlation and spans multiple frames. If we dedup
  // any of them by (origin,msgId), the first frame inserts the id and every later
  // frame with the same id — including the CMD_RESP that carries the actual command
  // output (e.g. a bonded `files /` listing) — is dropped as a "duplicate". That
  // silently empties the bonded-device file/CLI views on the master. FILE_* frames
  // are likewise multi-frame under one msgId.
  if (h->type != ESPNOW_V4_TYPE_STREAM &&
      h->type != ESPNOW_V4_TYPE_STREAM_CTRL &&
      h->type != ESPNOW_V4_TYPE_CMD_RESP &&
      h->type != ESPNOW_V4_TYPE_FILE_START &&
      h->type != ESPNOW_V4_TYPE_FILE_DATA &&
      h->type != ESPNOW_V4_TYPE_FILE_END) {
    if (h->msgId != 0 && v4_dedup_seen_and_insert(h->origin, h->msgId, h->fragIndex)) {
      DEBUG_ESPNOWF("[V4_DEDUP] Dropped duplicate: type=%u msgId=%lu frag=%u", h->type, (unsigned long)h->msgId, h->fragIndex);
      return true;
    }
  } else {
    DEBUG_ESPNOWF("[V4_DEDUP] Bypassed for type=%u msgId=%lu", h->type, (unsigned long)h->msgId);
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
  
  // enc = AEAD-decrypted SESSION_FRAME (confidential); auth = that OR a
  // verified BROADCAST_AUTH HMAC (authenticated, maybe plaintext). The old
  // log read ESPNOW_V4_FLAG_ENCRYPTED, a legacy LMK bit that's never set
  // anymore, so it always printed "encrypted=NO" — even for AEAD frames.
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_RX] Source: %s (paired=%s enc=%s auth=%s)",
         deviceName, isPaired ? "YES" : "NO",
         wasSessionEncrypted ? "YES" : "NO",
         wasAuthenticated ? "YES" : "NO");

#if ENABLE_BONDED_MODE
  // Track when bond-type messages arrive but would be rejected due to isPaired=false
  // Can't use BROADCAST_PRINTF here (ISR-like context) — use deferred counter instead
  if (!isPaired && (h->type == ESPNOW_V4_TYPE_BOND_HEARTBEAT ||
                    h->type == ESPNOW_V4_TYPE_BOND_CAP_REQ ||
                    h->type == ESPNOW_V4_TYPE_BOND_CAP_RESP ||
                    h->type == ESPNOW_V4_TYPE_MANIFEST_REQ ||
                    h->type == ESPNOW_V4_TYPE_SETTINGS_REQ)) {
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

  // === Handler-table dispatch (Phase 0 of docs/ESPNOW_V4_PLAN.md) ===
  // Opcodes registered in kV4HandlerTable are handled here; anything not
  // in the table falls through to the legacy if-ladder below.
  // `wasAuthenticated` proves the frame holds either our session key
  // (SESSION_FRAME unwrap) or the mesh group key (BROADCAST_AUTH HMAC).
  if (v4_dispatch_table_try(recv_info, h, payload, payloadLen,
                            isPaired, wasAuthenticated, wasSessionEncrypted, deviceName)) {
    return true;
  }

  // All opcodes are now registered in kV4HandlerTable; nothing reaches the
  // point below except genuinely-unknown types. Two wire-behavior notes worth
  // keeping (they're not separate opcodes on the dispatch path):
  //   - SETTINGS_RESP / SETTINGS_PUSH are unused on the wire — settings arrive
  //     as a FILE_END for _settings_out.json (handled in v4h_file_end).
  //   - MANIFEST_RESP travels as a FILE_END for _manifest_out.json — also
  //     handled inside v4h_file_end.

  // Unknown V4 type - log and ignore
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_RX] Unknown type %d from %s", h->type, deviceName);
  
  // CRITICAL: Cleanup reassembly buffer for ALL fragmented messages (not just unknown types)
  // This was previously only cleaning up at the end of the function, causing memory leaks
  // for message types that returned early (TEXT, CMD, HEARTBEAT, etc.)
  // Now we cleanup immediately after processing any fragmented message
  if (h->fragCount > 1) {
    for (int i = 0; i < V4_REASM_MAX; i++) {
      if (gV4Reasm[i].active && gV4Reasm[i].msgId == h->msgId && 
          memcmp(gV4Reasm[i].src, recv_info->src_addr, 6) == 0) {
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FRAG_RX] Cleaning up reassembly buffer for msgId=%lu",
               (unsigned long)h->msgId);
        v4_reasm_reset(gV4Reasm[i]);
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

// ExecAsyncCallback, CommandOrigin, CommandContext, Command defined in System_CommandTypes.h (included above)

// External async command submission
extern bool submitCommandAsync(const Command& cmd, ExecAsyncCallback callback, void* userData);

// Async callback for V3 CMD results - called on cmd_exec task (large stack)
static void v4CmdResultCallback(bool ok, const char* result, void* userData) {
  V3CmdAsyncCtx* ctx = (V3CmdAsyncCtx*)userData;
  if (!ctx) return;
  
  size_t resultLen = result ? strlen(result) : 0;
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4] CMD result callback: ok=%d len=%zu to %s (msgId=%lu)",
         ok, resultLen, ctx->deviceName, (unsigned long)ctx->cmdMsgId);
  
  // Clear the current stream session ID (stops any further streaming)
  gCurrentStreamCmdId = 0;
  
  // Send STREAM_END frame to signal output is complete
  sendSessionStreamFrame(ctx->cmdMsgId, nullptr, 0, ESPNOW_V4_FLAG_STREAM_END);
  
  // Send CMD_RESP with success/fail byte + actual command result text.
  // v4_send_encrypted_chunked handles fragmentation for large results
  // automatically. The receiver reassembles fragments before processing.
  const char* resultText = (result && resultLen > 0) ? result : 
                           (ctx->cmdName[0] ? ctx->cmdName : (ok ? "OK" : "FAIL"));
  size_t textLen = strlen(resultText);
  // Cap at 6KB — within V4 fragmentation budget (32 frags × 200 B = 6400 B,
  // minus 1 success byte + 1 null = 6398 B usable; rounded to 6143 for margin).
  // ExecReq.out is 4KB so the handler bottleneck moves there for results
  // larger than 4KB (but those rarely happen; see Phase 1 plan for the
  // streaming alternative if ever needed).
  if (textLen > 6143) textLen = 6143;
  
  // Allocate payload: 1 byte success + result text + null terminator
  size_t payloadLen = 1 + textLen + 1;
  uint8_t* respPayload = (uint8_t*)malloc(payloadLen);
  if (respPayload) {
    respPayload[0] = ok ? 1 : 0;
    memcpy(respPayload + 1, resultText, textLen);
    respPayload[1 + textLen] = '\0';
    
    // Step 3c: cmd_exec context (callback fires after deferred CMD finishes).
    // sendAeadSync provides backpressure — the caller blocks until the wire
    // send completes, so subsequent CMD callbacks don't pile up frames in the
    // clerk queue faster than they can drain.
    espnowtx::sendAeadSync(ctx->srcMac, ESPNOW_V4_TYPE_CMD_RESP, ESPNOW_V4_FLAG_ACK_REQ,
                           ctx->cmdMsgId, respPayload, payloadLen, 1, 2000);
    free(respPayload);
  } else {
    // Fallback: send just command name if malloc fails
    uint8_t fallback[64];
    fallback[0] = ok ? 1 : 0;
    const char* name = ctx->cmdName[0] ? ctx->cmdName : (ok ? "OK" : "FAIL");
    size_t nameLen = strlen(name) + 1;
    if (nameLen > sizeof(fallback) - 1) nameLen = sizeof(fallback) - 1;
    memcpy(fallback + 1, name, nameLen);
    // Step 3c: cmd_exec context — same reasoning as the alloc'd-path send above.
    espnowtx::sendAeadSync(ctx->srcMac, ESPNOW_V4_TYPE_CMD_RESP, ESPNOW_V4_FLAG_ACK_REQ,
                           ctx->cmdMsgId, fallback, 1 + nameLen, 1, 2000);
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
static void v4_handle_cmd(const uint8_t* srcMac, const char* deviceName, uint32_t msgId, const char* cmdPayload, bool wasSessionEncrypted) {
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
  
  // SECURITY (applies to BOTH auth methods below): every remote command carries
  // a credential — a bond token OR a username:password — and runs a command on
  // this device. Require the frame to have arrived AEAD-wrapped in a
  // SESSION_FRAME. Plaintext means the credential is exposed on-air AND the
  // command is forgeable/replayable (token-replay RCE, or password capture).
  // Senders route commands through v4_send_encrypted_or_queue, so a legitimate
  // command is always encrypted. One gate, both paths — reject plaintext loudly
  // before we even look at the credential.
  if (!wasSessionEncrypted) {
    ERROR_ESPNOWF("[ESP-NOW] REJECTED plaintext remote command from %s — session "
                  "encryption required (credential exposure / replay)", deviceName);
    BROADCAST_PRINTF("[ESP-NOW] SECURITY: rejected unencrypted remote command from %s", deviceName);
    uint8_t respPayload[64];
    respPayload[0] = 0;
    const char* errMsg = "Remote command must be session-encrypted";
    memcpy(respPayload + 1, errMsg, strlen(errMsg) + 1);
    // Step 3c: espnow_task RX-handler context — MUST NOT block (would stall
    // the RX drainer per the processMeshHeartbeats architecture invariant).
    // Echo the request's msgId so the requester can correlate this rejection.
    espnowtx::sendAead(srcMac, ESPNOW_V4_TYPE_CMD_RESP, ESPNOW_V4_FLAG_ACK_REQ,
                       msgId, respPayload, 1 + strlen(errMsg) + 1, 1);
    return;
  }

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
          // The bonded master is a trusted administrator for the life of the bond
          // (bond is the auth/RCE channel). Run its commands under the reserved
          // bond-admin identity so admin-gated commands (files, system logs, …)
          // are authorized — isAdminUser(kBondAdminUser) is true only while this
          // live session exists. Replaces the old non-admin "espnow" identity that
          // silently failed admin commands.
          strncpy(username, kBondAdminUser, sizeof(username) - 1);
          username[sizeof(username) - 1] = '\0';
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
      // Failed bond-channel auth (bad/malformed/wrong-length token) — record it
      // in failed_login.log, mirroring the espnow/bond success line. Deferred +
      // throttled inside logBondAuthFailure (we are on espnow_task here).
      logBondAuthFailure(srcMac, "Invalid bond session token");
      uint8_t respPayload[48];
      respPayload[0] = 0;
      const char* errMsg = "Session token auth failed";
      memcpy(respPayload + 1, errMsg, strlen(errMsg) + 1);
      // Step 3c: espnow_task RX-handler context — fire-and-forget.
      // Echo the request's msgId so the requester can correlate this failure.
      espnowtx::sendAead(srcMac, ESPNOW_V4_TYPE_CMD_RESP, ESPNOW_V4_FLAG_ACK_REQ,
                         msgId, respPayload, 1 + strlen(errMsg) + 1, 1);
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
      // Step 3c: espnow_task RX-handler context — fire-and-forget.
      // Echo the request's msgId so the requester can correlate this failure.
      espnowtx::sendAead(srcMac, ESPNOW_V4_TYPE_CMD_RESP, ESPNOW_V4_FLAG_ACK_REQ,
                         msgId, respPayload, 1 + strlen(errMsg) + 1, 1);
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
    // Step 3c: espnow_task RX-handler context — fire-and-forget.
    espnowtx::sendAead(srcMac, ESPNOW_V4_TYPE_CMD_RESP, ESPNOW_V4_FLAG_ACK_REQ,
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
  sendSessionStreamFrame(msgId, nullptr, 0, ESPNOW_V4_FLAG_STREAM_BEGIN);
  
  // Set the current stream session ID (cmd_exec task will use this)
  gCurrentStreamCmdId = msgId;
  
  // Build command for cmd_exec task
  Command cmd;
  cmd.line = actualCmd;
  cmd.ctx.origin = ORIGIN_ESPNOW;
  cmd.ctx.auth.transport = SOURCE_ESPNOW;
  cmd.ctx.auth.path = actualCmd;
  cmd.ctx.auth.user = username;
  cmd.ctx.auth.sid = "";
  cmd.ctx.auth.opaque = (void*)asyncCtx->srcMac;
  // V4: stamp the sender MAC in `ip` so log lines + gates that expect the
  // "espnow:..." prefix (e.g. cmd_espnow_startstream) work correctly.
  {
    char ipBuf[28];
    snprintf(ipBuf, sizeof(ipBuf), "espnow:%02X:%02X:%02X:%02X:%02X:%02X",
             srcMac[0], srcMac[1], srcMac[2], srcMac[3], srcMac[4], srcMac[5]);
    cmd.ctx.auth.ip = ipBuf;
  }
  cmd.ctx.id = msgId;
  cmd.ctx.timestampMs = millis();
  cmd.ctx.outputMask = CMD_OUT_WEB | CMD_OUT_LOG;  // local sinks; output also streams via V3 STREAM frames
  cmd.ctx.validateOnly = false;
  cmd.ctx.replyHandle = nullptr;
  cmd.ctx.httpReq = nullptr;
  
  // Queue for execution on cmd_exec task (has large stack)
  if (!submitCommandAsync(cmd, v4CmdResultCallback, asyncCtx)) {
    BROADCAST_PRINTF("[ESP-NOW] Failed to queue CMD from %s", deviceName);
    gCurrentStreamCmdId = 0;
    destroyStreamSession(msgId);
    free(asyncCtx);
    uint8_t respPayload[32];
    respPayload[0] = 0;
    const char* errMsg = "Queue failed";
    memcpy(respPayload + 1, errMsg, strlen(errMsg) + 1);
    // Step 3c: espnow_task RX-handler context — fire-and-forget.
    espnowtx::sendAead(srcMac, ESPNOW_V4_TYPE_CMD_RESP, ESPNOW_V4_FLAG_ACK_REQ,
                       msgId, respPayload, 1 + strlen(errMsg) + 1, 1);
  }
}

// ============================================================================
// ESP-NOW traffic capture to SD card
// ============================================================================
// When gSettings.espnowCaptureToSd is true AND an SD card is mounted, every
// incoming and outgoing V3 frame is appended to /sd/espnow/capture-<bootTs>.log
// in a human-readable one-line format. Payload is base64-encoded; encrypted
// frames are saved as encrypted bytes (future decoder can unseal them if
// given the passphrase).
//
// Heartbeats (types 7 and 14) dominate volume and are skipped by default via
// gSettings.espnowCaptureSkipHeartbeats. A reasonable session capture stays
// well under the 16 MB per-file cap even over several hours.
//
// Called from the recv callback (onEspNowRawRecv) and each esp_now_send path.

// Tiny base64 encoder — no external dep. Writes at most outLen-1 bytes; always
// null-terminates. Returns actual encoded length.
static size_t espnowCaptureBase64(const uint8_t* src, size_t srcLen, char* out, size_t outLen) {
  static const char tbl[65] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (!out || outLen == 0) return 0;
  size_t o = 0;
  for (size_t i = 0; i < srcLen; i += 3) {
    if (o + 4 >= outLen) break;
    uint32_t v = ((uint32_t)src[i]) << 16;
    if (i + 1 < srcLen) v |= ((uint32_t)src[i + 1]) << 8;
    if (i + 2 < srcLen) v |= ((uint32_t)src[i + 2]);
    out[o++] = tbl[(v >> 18) & 0x3F];
    out[o++] = tbl[(v >> 12) & 0x3F];
    out[o++] = (i + 1 < srcLen) ? tbl[(v >> 6) & 0x3F] : '=';
    out[o++] = (i + 2 < srcLen) ? tbl[v & 0x3F] : '=';
  }
  out[o] = '\0';
  return o;
}

// Session-scoped capture file path. Built on first write of each boot.
// Format: /sd/espnow/capture-<unixBootSeconds>.log
static String gEspNowCapturePath;
static size_t gEspNowCaptureWritten = 0;
static int    gEspNowCapturePart    = 0;
static constexpr size_t ESPNOW_CAPTURE_MAX_BYTES = 16 * 1024 * 1024;  // 16 MB per file
static bool   gEspNowCaptureMkdirTried = false;

static bool ensureCaptureFile(time_t now) {
  if (!gEspNowCapturePath.length()) {
    char buf[96];
    snprintf(buf, sizeof(buf), "/sd/espnow/capture-%lu.log", (unsigned long)now);
    gEspNowCapturePath = buf;
    gEspNowCaptureWritten = 0;
    gEspNowCapturePart = 0;
  }
  if (gEspNowCaptureWritten >= ESPNOW_CAPTURE_MAX_BYTES) {
    gEspNowCapturePart++;
    char buf[112];
    int dot = gEspNowCapturePath.lastIndexOf('.');
    String stem = (dot > 0) ? gEspNowCapturePath.substring(0, dot) : gEspNowCapturePath;
    snprintf(buf, sizeof(buf), "%s-part%d.log", stem.c_str(), gEspNowCapturePart);
    gEspNowCapturePath = buf;
    gEspNowCaptureWritten = 0;
  }
  if (!gEspNowCaptureMkdirTried) {
    gEspNowCaptureMkdirTried = true;
    if (!VFS::existsGuarded("/sd/espnow", VFS::systemAuth("espnow.capture_mkdir"))) VFS::mkdirGuarded("/sd/espnow", VFS::systemAuth("espnow.capture_mkdir"));
  }
  return true;
}

// direction: "RX" or "TX". peerMac must be 6 bytes. data/len is the full V3
// frame (header + payload). rssi is only valid for RX; pass 0 for TX.
static void captureEspNowFrame(const char* direction, const uint8_t* peerMac,
                               int rssi, const uint8_t* data, int len) {
  if (!gSettings.espnowCaptureToSd) return;
  if (!VFS::isSDAvailable()) return;
  if (!data || len < (int)sizeof(EspNowV4Header)) return;

  const EspNowV4Header* h = (const EspNowV4Header*)data;
  // Validate magic quickly — avoid capturing random junk if something non-V3 sneaks in.
  if (h->magic != (uint16_t)ESPNOW_V4_MAGIC || h->ver != 3) return;

  // Heartbeat filter (types 7=HEARTBEAT, 14=BOND_HEARTBEAT).
  if (gSettings.espnowCaptureSkipHeartbeats &&
      (h->type == ESPNOW_V4_TYPE_HEARTBEAT || h->type == ESPNOW_V4_TYPE_BOND_HEARTBEAT)) {
    return;
  }

  time_t now = time(nullptr);
  if (now <= 0) now = (time_t)(millis() / 1000);  // fallback if NTP not synced yet
  if (!ensureCaptureFile(now)) return;

  // Resolve peer name (meta first, paired registry fallback).
  String peerName = peerMac ? getEspNowDeviceName(peerMac) : String();

  char macStr[20];
  if (peerMac) {
    macToDisplay(peerMac, macStr, sizeof(macStr));  // canonical DISPLAY form
  } else {
    strcpy(macStr, "??:??:??:??:??:??");
  }

  // Base64-encode the full frame (header + payload). Per-frame scratch when
  // SD capture is enabled (off by default) — PSRAM is fine here.
  EXT_RAM_BSS_ATTR static char b64[512];
  espnowCaptureBase64(data, (size_t)len, b64, sizeof(b64));

  // One line: timestamp, direction, peer, rssi, V3 type, flags, len, payload.
  char line[700];
  int n = snprintf(line, sizeof(line),
    "%lu %s PEER=%s NAME=%s RSSI=%d TYPE=%u FLAGS=0x%02x LEN=%d FRAG=%u/%u B64=%s\n",
    (unsigned long)now, direction, macStr,
    peerName.length() ? peerName.c_str() : "-",
    rssi, (unsigned)h->type, (unsigned)h->flags, len,
    (unsigned)h->fragIndex, (unsigned)h->fragCount, b64);
  if (n <= 0) return;

  File f = VFS::openGuarded(gEspNowCapturePath, "a", VFS::systemAuth("espnow.capture_append"), true);
  if (!f) return;
  f.write((const uint8_t*)line, (size_t)n);
  f.close();
  gEspNowCaptureWritten += (size_t)n;
}

static void onEspNowRawRecv(const esp_now_recv_info* recv_info, const uint8_t* incomingData, int len) {
  // Increment receive counter
  if (gEspNow) gEspNow->routerMetrics.messagesReceived++;

  // Capture RX traffic if enabled (before dispatch — we want to see even frames
  // V3 doesn't handle).
  if (recv_info && incomingData && len > 0) {
    captureEspNowFrame("RX", recv_info->src_addr,
                       recv_info->rx_ctrl ? recv_info->rx_ctrl->rssi : 0,
                       incomingData, len);
  }

  // === V3-ONLY MODE ===
  // Try v3 binary protocol (only handler enabled)
  if (recv_info && incomingData && len > 0 && v4_try_handle_incoming(recv_info, incomingData, len)) {
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
 * @note Updates routerMetrics TX counters (messagesSent / messagesFailed)
 * @warning Do not call blocking functions or allocate memory
 */
// IDF 5.4+ changed esp_now_send_cb_t: first arg is now const esp_now_send_info_t*
// (= wifi_tx_info_t; dest MAC at tx_info->des_addr) instead of const uint8_t* mac.
// This callback doesn't use the MAC, so it's a straight signature swap.
void onEspNowDataSent(const esp_now_send_info_t* tx_info, esp_now_send_status_t status) {
  // This callback fires once per esp_now_send() completion regardless of the
  // call path (single-frame / fragmented / encrypted / ACK / …) and is the only
  // single point where TX accounting can be done correctly without touching
  // every send-site. Cumulative counters here feed both `espnowstats` and
  // `espnowsaturation` (frames/sec, fail-rate). Without this the previous
  // "Stream Sent" reading was stuck at the constructor's 0 (latent stats bug).
  (void)tx_info;
  if (!gEspNow) return;

  if (status == ESP_NOW_SEND_SUCCESS) {
    gEspNow->routerMetrics.messagesSent++;
  } else {
    gEspNow->routerMetrics.messagesFailed++;
  }
}

/**
 * @brief Check if ESP-NOW first-time setup is needed
 * @return Error message if setup needed, empty string if ready to proceed
 * @note Displays setup instructions and returns error to block initialization
 */


#if ENABLE_BONDED_MODE
// ==========================
// Bond Mode Helper Functions
// ==========================

/**
 * Build BondPeerStatus snapshot from local device state
 * Called in task context when responding to BOND_STATUS_REQ
 */
void buildLocalBondStatus(BondPeerStatus& status) {
  memset(&status, 0, sizeof(BondPeerStatus));
  
  status.uptimeSec = millis() / 1000;
  status.freeHeap = (uint32_t)ESP.getFreeHeap();
  status.minFreeHeap = (uint32_t)ESP.getMinFreeHeap();
  
  // Build sensor enabled mask from runtime booleans
  extern bool gThermalEnabled, gTofEnabled, gImuEnabled, gInputEnabled;
  extern bool gGpsEnabled, gPresenceEnabled;
  extern bool gRtcEnabled, gApdsEnabled, gFmRadioEnabled;
  uint16_t enabled = 0;
#if ENABLE_THERMAL_SENSOR
  if (gThermalEnabled)  enabled |= CAP_SENSOR_THERMAL;
#endif
#if ENABLE_TOF_SENSOR
  if (gTofEnabled)      enabled |= CAP_SENSOR_TOF;
#endif
#if ENABLE_IMU_SENSOR
  if (gImuEnabled)      enabled |= CAP_SENSOR_IMU;
#endif
#if ENABLE_OLED_INPUT  // gInputEnabled is set by either the gamepad or ANO driver
  if (gInputEnabled)  enabled |= CAP_SENSOR_INPUT;
#endif
#if ENABLE_GPS_SENSOR
  if (gGpsEnabled)      enabled |= CAP_SENSOR_GPS;
#endif
#if ENABLE_PRESENCE_SENSOR
  if (gPresenceEnabled) enabled |= CAP_SENSOR_PRESENCE;
#endif
#if ENABLE_RTC_SENSOR
  if (gRtcEnabled)      enabled |= CAP_SENSOR_RTC;
#endif
#if ENABLE_APDS_SENSOR
  if (gApdsEnabled)     enabled |= CAP_SENSOR_APDS;
#endif
#if ENABLE_FM_RADIO
  if (gFmRadioEnabled)  enabled |= CAP_SENSOR_FMRADIO;
#endif
  status.sensorEnabledMask = enabled;

  // Build sensor connected mask
  extern bool gThermalConnected, gTofConnected, gImuConnected, gInputConnected;
  extern bool gGpsConnected, gPresenceConnected;
  extern bool gRtcConnected, gApdsConnected, gFmRadioConnected;
  uint16_t connected = 0;
#if ENABLE_THERMAL_SENSOR
  if (gThermalConnected)  connected |= CAP_SENSOR_THERMAL;
#endif
#if ENABLE_TOF_SENSOR
  if (gTofConnected)      connected |= CAP_SENSOR_TOF;
#endif
#if ENABLE_IMU_SENSOR
  if (gImuConnected)      connected |= CAP_SENSOR_IMU;
#endif
#if ENABLE_OLED_INPUT  // gInputConnected is set by either the gamepad or ANO driver
  if (gInputConnected)  connected |= CAP_SENSOR_INPUT;
#endif
#if ENABLE_GPS_SENSOR
  if (gGpsConnected)      connected |= CAP_SENSOR_GPS;
#endif
#if ENABLE_PRESENCE_SENSOR
  if (gPresenceConnected) connected |= CAP_SENSOR_PRESENCE;
#endif
#if ENABLE_RTC_SENSOR
  if (gRtcConnected)      connected |= CAP_SENSOR_RTC;
#endif
#if ENABLE_APDS_SENSOR
  if (gApdsConnected)     connected |= CAP_SENSOR_APDS;
#endif
#if ENABLE_FM_RADIO
  if (gFmRadioConnected)  connected |= CAP_SENSOR_FMRADIO;
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
  // Tell the peer which input device we have compiled in (gamepad vs ANO
  // encoder vs none). Mirrors INPUT_DEVICE_TYPE from System_BuildConfig.h
  // so the bonded device's UI can render the correct label ("Gamepad" or
  // "ANO Encoder") for our Remote Sensors row instead of a generic "Input".
  cap.inputDeviceType = INPUT_DEVICE_TYPE;

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
#if ENABLE_OLED_INPUT  // capability advertised to peers — true for either input driver
  cap.sensorMask |= CAP_SENSOR_INPUT;
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
#if ENABLE_FM_RADIO
  cap.sensorMask |= CAP_SENSOR_FMRADIO;
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
  device["role"] = isBondMaster() ? "master" : "worker";
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
  bool inputConnected = false, apdsConnected = false, gpsConnected = false;
  bool rtcConnected = false, presenceConnected = false;

  for (int i = 0; i < connectedDeviceCount; i++) {
    if (!connectedDevices[i].isConnected) continue;
    switch (connectedDevices[i].address) {
      case 0x33: thermalConnected = true; break;  // MLX90640
      case 0x29: tofConnected = true; break;      // VL53L0X
      case 0x28: imuConnected = true; break;      // BNO055
      // Input device: either the Seesaw gamepad (0x50) OR the ANO rotary
      // encoder (0x49) — they're mutually exclusive at compile time but the
      // local I2C scanner doesn't know which one is compiled in, so it
      // accepts either address. Previously only 0x50 was matched, which
      // made the input-connected flag always read false on ANO builds.
      case 0x49: inputConnected = true; break;    // ANO Rotary Encoder
      case 0x50: inputConnected = true; break;    // Seesaw Gamepad
      case 0x39: apdsConnected = true; break;     // APDS9960
      case 0x10: gpsConnected = true; break;      // PA1010D
      case 0x68: rtcConnected = true; break;      // DS3231
      case 0x61: presenceConnected = true; break; // LD2410
    }
  }

  sensorStatus["thermal"] = thermalConnected;
  sensorStatus["tof"] = tofConnected;
  sensorStatus["imu"] = imuConnected;
  sensorStatus["input"] = inputConnected;
  sensorStatus["apds"] = apdsConnected;
  sensorStatus["gps"] = gpsConnected;
  sensorStatus["rtc"] = rtcConnected;
  sensorStatus["presence"] = presenceConnected;
#endif
  
  // Add fwHash as hex string for cache keying
  char keyHex[33];
  for (int i = 0; i < 16; i++) {
    snprintf(keyHex + (i * 2), 3, "%02x", cap.fwHash[i]);
  }
  keyHex[32] = '\0';
  capObj["fwHash"] = keyHex;
  
  // UI apps section (curated list of OLED modes)
  JsonArray apps = doc["uiApps"].to<JsonArray>();
  
#if ENABLE_OLED_DISPLAY
  // Add OLED menu items as UI apps (from category arrays)
  extern const OLEDMenuItem oledMenuCategory1[], oledMenuCategory2[], oledMenuCategory3[], oledMenuCategory4[], oledMenuCategory5[];
  extern const int oledMenuCategory1Count, oledMenuCategory2Count, oledMenuCategory3Count, oledMenuCategory4Count, oledMenuCategory5Count;
  
  const struct { const OLEDMenuItem* items; int count; } cats[] = {
    { oledMenuCategory1, oledMenuCategory1Count },
    { oledMenuCategory2, oledMenuCategory2Count },
    { oledMenuCategory3, oledMenuCategory3Count },
    { oledMenuCategory4, oledMenuCategory4Count },
    { oledMenuCategory5, oledMenuCategory5Count },
  };
  for (int c = 0; c < (int)(sizeof(cats)/sizeof(cats[0])); c++) {
    for (int i = 0; i < cats[c].count; i++) {
      JsonObject app = apps.add<JsonObject>();
      // ArduinoJson stores const char* by reference (no copy) and calls
      // strlen() at serialize time — a null pointer => strlen(NULL) crash.
      // Guard every raw const char* the same way `description` is below.
      app["name"] = cats[c].items[i].name     ? cats[c].items[i].name     : "";
      app["icon"] = cats[c].items[i].iconName ? cats[c].items[i].iconName : "";
      app["mode"] = (int)cats[c].items[i].targetMode;
    }
  }
#endif
  
  // CLI commands section (full command dump)
  JsonArray cliModules = doc["cliModules"].to<JsonArray>();
  
  size_t moduleCount = 0;
  const CommandModule* modules = getCommandModules(moduleCount);
  
  for (size_t m = 0; m < moduleCount; m++) {
    JsonObject module = cliModules.add<JsonObject>();
    module["name"] = modules[m].name ? modules[m].name : "";
    module["description"] = modules[m].description ? modules[m].description : "";

    JsonArray commands = module["commands"].to<JsonArray>();
    for (size_t c = 0; c < modules[m].count; c++) {
      JsonObject cmd = commands.add<JsonObject>();
      // Guard each const char*: a null name/help => strlen(NULL) crash in
      // ArduinoJson serialize (it stores the pointer, not a copy).
      cmd["name"] = modules[m].commands[c].name ? modules[m].commands[c].name : "";
      cmd["help"] = modules[m].commands[c].help ? modules[m].commands[c].help : "";
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
  if (!filesystemReady) return false;
  
  // Build filename from hash
  char keyHex[33];
  for (int i = 0; i < 16; i++) {
    snprintf(keyHex + (i * 2), 3, "%02x", fwHash[i]);
  }
  keyHex[32] = '\0';
  
  char path[64];
  snprintf(path, sizeof(path), "/system/manifests/%s.json", keyHex);
  
  // Ensure directory exists
  if (!VFS::existsGuarded("/system/manifests", VFS::systemAuth("espnow.manifest_cache_mkdir"))) {
    VFS::mkdirGuarded("/system/manifests", VFS::systemAuth("espnow.manifest_cache_mkdir"));
  }

  // Write manifest
  FsLockGuard fsGuard("pair.manifest.cache");
  File f = VFS::openGuarded(path, "w", VFS::systemAuth("espnow.manifest_cache_write"), true);
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
  
  BROADCAST_PRINTF("[BOND] Cached manifest to %s (%u bytes)", path, (unsigned)written);
  return true;
}

// ============================================================================
// Settings Sync for Bond Mode
// ============================================================================

// Forward declaration — generateDeviceSettings() is defined below but used by
// computeBondLocalSettingsHash() to hash the exact wire payload.
static String generateDeviceSettings();

/**
 * Compute a CRC32 over the exact bytes a bonded peer would receive from us
 * via generateDeviceSettings(). Stored in gEspNow->bondLocalSettingsHash and
 * sent in every bond heartbeat. The peer hashes the file content it receives
 * during a settings sync the same way, so the two values match by
 * construction — any user-visible setting change (any field, any subsection,
 * including tz/ntp/loglevel which the old curated FNV missed) flips the hash.
 *
 * Cost: re-serializes settings on each call. Acceptable because the function
 * fires only at init + after writeSettingsJson (uncommon), not per-heartbeat
 * — heartbeats just read the cached uint32 value.
 */
void computeBondLocalSettingsHash() {
  if (!gEspNow) return;
  String payload = generateDeviceSettings();
  if (payload.length() == 0) {
    gEspNow->bondLocalSettingsHash = 0;
    return;
  }
  gEspNow->bondLocalSettingsHash =
    esp_crc32_le(0, (const uint8_t*)payload.c_str(), payload.length());
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
  doc["_bondRole"] = isBondMaster() ? "master" : "worker";
  
  String settings;
  serializeJson(doc, settings);
  return settings;
}

/**
 * Cache settings to LittleFS for a specific peer MAC
 */
static bool cacheSettingsToLittleFS(const uint8_t* peerMac, const String& settings) {
  DEBUG_ESPNOWF("[SETTINGS_CACHE] cacheSettingsToLittleFS: fsReady=%d len=%d",
                filesystemReady ? 1 : 0, settings.length());
  if (!filesystemReady || !peerMac) {
    DEBUG_ESPNOWF("[SETTINGS_CACHE] EXIT: filesystemReady or peerMac invalid");
    return false;
  }
  
  char dirPath[48];
  peerCacheDir(peerMac, dirPath, sizeof(dirPath));
  char filePath[64];
  peerCachePath(peerMac, "settings.json", filePath, sizeof(filePath));
  DEBUG_ESPNOWF("[SETTINGS_CACHE] Target path: %s", filePath);
  
  // Ensure per-peer directory exists (parent dirs created at filesystem init)
  FsLockGuard fsGuard("pair.settings.cache");
  if (!VFS::existsGuarded(dirPath, VFS::systemAuth("espnow.settings_cache_mkdir"))) { VFS::mkdirGuarded(dirPath, VFS::systemAuth("espnow.settings_cache_mkdir")); }

  // Write settings
  File f = VFS::openGuarded(filePath, "w", VFS::systemAuth("espnow.settings_cache_write"), true);
  if (!f) {
    ERROR_ESPNOWF("[SETTINGS_CACHE] Failed to open %s for writing", filePath);
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
  DEBUG_ESPNOWF("[SETTINGS_LOAD] loadSettingsFromCache called: fsReady=%d peerMac=%p",
                filesystemReady ? 1 : 0, peerMac);
  if (!filesystemReady || !peerMac) {
    DEBUG_ESPNOWF("[SETTINGS_LOAD] EXIT: filesystemReady or peerMac invalid");
    return "";
  }
  
  char filePath[64];
  peerCachePath(peerMac, "settings.json", filePath, sizeof(filePath));
  DEBUG_ESPNOWF("[SETTINGS_LOAD] Checking path: %s", filePath);
  
  if (!VFS::existsGuarded(filePath, VFS::systemAuth("espnow.settings_cache_load"))) {
    DEBUG_ESPNOWF("[SETTINGS_LOAD] File does not exist");
    return "";  // Not cached
  }

  FsLockGuard fsGuard("pair.settings.load");
  File f = VFS::openGuarded(filePath, "r", VFS::systemAuth("espnow.settings_cache_load"));
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
  if (!isBondMaster()) return;

  uint32_t now = millis();
  gEspNow->bondSyncInFlight = BOND_SYNC_SETTINGS;
  gEspNow->bondSyncLastAttemptMs = now;
  // NOTE: retry count is managed by the sync tick retry block — don't increment here
  
  uint32_t msgId = generateMessageId();
  // Runs on espnow_task (called only from the processMeshHeartbeats super-loop),
  // so it routes through espnow_tx like the other super-loop sends. `sent` now
  // means "queued"; it only selects the log line below.
  bool sent = bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_SETTINGS_REQ, 0, msgId, nullptr, 0);

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
    File f = VFS::openGuarded(tempPath, "w", VFS::systemAuth("espnow.settings_send_temp"), true);
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
    VFS::removeGuarded(tempPath, VFS::systemAuth("espnow.settings_send_cleanup"));
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
    if (filesystemReady && VFS::existsGuarded(AUTOMATIONS_JSON_FILE, VFS::systemAuth("espnow.bond_automations_check"))) {
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

// ============================================================================
// Bond schema transfer (mirrors bond settings transfer above)
// ============================================================================
//
// Architecture mirrors sendBondSettings exactly: worker writes JSON to a temp
// file in /system/espnow/this_device/, hands it to sendFileToMac, deletes
// the temp file after the chunked transfer is initiated. Receiver picks the
// file up in v4h_file_end → processBondSchema, which caches it at
// /system/espnow/peers/<MAC>/schema.json.
//
// Why a separate file (not folded into settings.json): schema is compile-time
// metadata that only changes on reflash; settings.json changes on every save.
// Coupling them would force every save to rewrite ~8 KB of unchanging schema
// and every settings reader to step past it. Two files, same pipeline.

static uint32_t sLastSchemaSendMs = 0;
static volatile bool sSchemaTransferInProgress = false;
static const uint32_t SCHEMA_DEBOUNCE_MS = 3000;  // 3s cooldown between schema sends
static const uint32_t SCHEMA_MIN_HEAP    = 20000; // 20KB heap headroom for JSON build

// Send schema to bonded peer (response to SCHEMA_REQ). The peer caches it
// under peers/<MAC>/schema.json for the bonded settings panel to render
// against. Worker-side temp file lives in /system/espnow/this_device/, the
// folder we mkdir at FS init for this device's own bond-self files.
static void sendBondSchema(const uint8_t* peerMac) {
  DEBUG_ESPNOWF("[SCHEMA_SEND] sendBondSchema called");
  if (!peerMac || !gEspNow) {
    ERROR_ESPNOWF("[SCHEMA_SEND] EXIT: peerMac or gEspNow is NULL");
    return;
  }
  if (!gEspNow->bondPeerOnline) {
    DEBUG_ESPNOWF("[SCHEMA_SEND] SKIP: peer offline");
    return;
  }
  if (sSchemaTransferInProgress) {
    DEBUG_ESPNOWF("[SCHEMA_SEND] SKIP: transfer already in progress");
    return;
  }
  uint32_t now = millis();
  if (now - sLastSchemaSendMs < SCHEMA_DEBOUNCE_MS) {
    DEBUG_ESPNOWF("[SCHEMA_SEND] SKIP: debounced (last=%lums ago)",
                  (unsigned long)(now - sLastSchemaSendMs));
    return;
  }
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < SCHEMA_MIN_HEAP) {
    WARN_ESPNOWF("[SCHEMA_SEND] SKIP: low heap (%u < %lu required)",
                 freeHeap, (unsigned long)SCHEMA_MIN_HEAP);
    return;
  }

  sSchemaTransferInProgress = true;
  sLastSchemaSendMs = now;

  vTaskDelay(pdMS_TO_TICKS(10));  // yield before heavy build

  // Build schema JSON → write to temp → release String before transfer. The
  // scoped braces ensure the String destructor runs (freeing its internal
  // heap) before sendFileToMac fires — same pattern sendBondSettings uses.
  String tempPath = "/system/espnow/this_device/_schema_out.json";
  size_t schemaLen = 0;
  {
    PSRAM_JSON_DOC(doc);
    buildSettingsSchemaJson(doc);
    String schema;
    serializeJson(doc, schema);
    schemaLen = schema.length();
    DEBUG_ESPNOWF("[SCHEMA_SEND] Generated schema JSON: %d bytes", schemaLen);

    FsLockGuard guard("bond.schema.send");
    File f = VFS::openGuarded(tempPath, "w", VFS::systemAuth("espnow.schema_send_temp"), true);
    if (!f) {
      ERROR_ESPNOWF("[SCHEMA_SEND] Cannot create %s", tempPath.c_str());
      sSchemaTransferInProgress = false;
      return;
    }
    f.print(schema);
    f.close();
    DEBUG_ESPNOWF("[SCHEMA_SEND] Wrote %d bytes to %s", schemaLen, tempPath.c_str());
    // schema + doc destructors run here — memory freed before transfer
  }

  DEBUG_ESPNOWF("[SCHEMA_SEND] Calling sendFileToMac for %s", tempPath.c_str());
  bool sent = sendFileToMac(peerMac, tempPath);
  if (sent) {
    DEBUG_ESPNOWF("[SCHEMA_SEND] SUCCESS: File transfer initiated (%d bytes)", schemaLen);
  } else {
    ERROR_ESPNOWF("[SCHEMA_SEND] sendFileToMac failed");
  }

  {
    FsLockGuard guard("bond.schema.cleanup");
    VFS::removeGuarded(tempPath, VFS::systemAuth("espnow.schema_send_cleanup"));
    DEBUG_ESPNOWF("[SCHEMA_SEND] Cleaned up temp file");
  }

  sSchemaTransferInProgress = false;
}

// ============================================================================
// Bond file-prep deferral — 9d/9e/9e2 in processMeshHeartbeats used to build
// JSON (~8 KB schema, ~10 KB settings, up to ~44 KB manifest), write it to a
// temp file under FsLockGuard, kick sendFileToMac, and clean up — all inline
// on the espnow_task super-loop's stack. That stalled the RX drainer for tens
// to hundreds of ms per fire. cmd_exec has the deeper stack and already runs
// Ed25519/X25519 deferred work, so the canonical "heavy infrequent handler"
// pattern from the architecture invariant at the top of processMeshHeartbeats
// applies. Each 9x stage now snapshots the dest MAC, submits to cmd_exec, and
// returns; cmd_exec installs the matching identity scope and runs the chain.
//
// On submit failure (queue full / ps_alloc fail) the caller re-sets its
// "needs response" flag so the next super-loop tick retries instead of
// dropping a peer request silently.
//
// NOTE: sendFileToMac itself is still synchronous today (Step 4 will move its
// chunk loop onto espnow_tx with ACK-driven progression). Moving build+write
// to cmd_exec is independent of that — even with sync sendFileToMac, the
// espnow_task RX-drain budget is what we're protecting here.
// ============================================================================

enum BondFileKind : uint8_t {
  BOND_FILE_KIND_MANIFEST = 0,
  BOND_FILE_KIND_SETTINGS = 1,
  BOND_FILE_KIND_SCHEMA   = 2,
};

struct BondFileSendWork {
  uint8_t destMac[6];
  uint8_t kind;  // BondFileKind
};

// Matches the sSchemaTransferInProgress / sSettingsTransferInProgress pattern.
// Prevents a second deferred manifest run from overlapping the first if RX
// receives another MANIFEST_REQ while cmd_exec is still draining the queue.
static volatile bool sManifestTransferInProgress = false;

// Build → temp-write → sendFileToMac → cleanup for the bond manifest.
// Extracted verbatim from the inline 9d block; behavior is identical, only
// the executing task changes. Caller installs the identity scope; systemAuth()
// on each VFS call keeps mkdir/openGuarded/remove independent of the outer
// scope anyway, matching the prior inline semantics.
static void sendBondManifest(const uint8_t* peerMac) {
  if (!peerMac || !gEspNow) {
    BROADCAST_PRINTF("[BOND] manifest-send: peerMac/gEspNow null");
    return;
  }
  if (sManifestTransferInProgress) {
    BROADCAST_PRINTF("[BOND] manifest-send: SKIP transfer in progress");
    return;
  }
  sManifestTransferInProgress = true;

  BROADCAST_PRINTF("[BOND] manifest-send: building dest=%s", MAC_STR(peerMac));
  String manifest = generateDeviceManifest();
  BROADCAST_PRINTF("[BOND] manifest-send: built len=%d", manifest.length());

  String tempPath = "/system/_manifest_out.json";
  {
    FsLockGuard guard("bond.manifest.send");
    if (!filesystemReady) {
      BROADCAST_PRINTF("[BOND] manifest-send: ERROR fs not ready");
      sManifestTransferInProgress = false;
      return;
    }
    if (!VFS::existsGuarded("/system", VFS::systemAuth("espnow.bond_manifest_send"))) {
      VFS::mkdirGuarded("/system", VFS::systemAuth("espnow.bond_manifest_send"));
    }
    File f = VFS::openGuarded(tempPath, "w", VFS::systemAuth("espnow.bond_manifest_send"), true);
    if (!f) {
      BROADCAST_PRINTF("[BOND] manifest-send: ERROR open %s failed", tempPath.c_str());
      sManifestTransferInProgress = false;
      return;
    }
    f.print(manifest);
    f.close();
    BROADCAST_PRINTF("[BOND] manifest-send: wrote %s len=%d", tempPath.c_str(), manifest.length());
  }

  bool fileSent = sendFileToMac(peerMac, tempPath);
  BROADCAST_PRINTF("[BOND] manifest-send: sendFileToMac rc=%d", (int)fileSent);

  {
    FsLockGuard guard("bond.manifest.cleanup");
    VFS::removeGuarded(tempPath, VFS::systemAuth("espnow.bond_manifest_cleanup"));
  }

  sManifestTransferInProgress = false;
}

// cmd_exec_task runner — dispatches to the right sendBondX() by kind. Each
// branch installs the same identity scope the previously-inline super-loop
// code installed around the same call, so canRead()/canWrite() gates on
// /system/_*_out.json behave identically.
static void runDeferredBondFileSend(void* arg) {
  auto* w = (BondFileSendWork*)arg;
  if (!w) return;
  switch (w->kind) {
    case BOND_FILE_KIND_MANIFEST: {
      SYSTEM_IDENTITY_SCOPE("espnow.bond_manifest_send");
      sendBondManifest(w->destMac);
      break;
    }
    case BOND_FILE_KIND_SETTINGS: {
      SYSTEM_IDENTITY_SCOPE("espnow.bond_settings_send");
      sendBondSettings(w->destMac);
      break;
    }
    case BOND_FILE_KIND_SCHEMA: {
      SYSTEM_IDENTITY_SCOPE("espnow.bond_schema_send");
      sendBondSchema(w->destMac);
      break;
    }
    default:
      BROADCAST_PRINTF("[BOND] file-send deferred: unknown kind=%u", (unsigned)w->kind);
      break;
  }
  free(w);
}

// Allocate + snapshot dest MAC + enqueue. Returns false on transient backpressure
// (queue full / ps_alloc fail); caller re-sets its "needs response" flag so the
// next super-loop tick retries. MAC is snapshotted at submit time because
// gEspNow->bondPendingResponseMac can change between submit and run.
static bool submitBondFileSend(const uint8_t destMac[6], uint8_t kind) {
  auto* w = (BondFileSendWork*)ps_alloc(sizeof(BondFileSendWork),
                                        AllocPref::PreferPSRAM,
                                        "espnow.bond.file.send");
  if (!w) {
    BROADCAST_PRINTF("[BOND] file-send: ps_alloc failed kind=%u", (unsigned)kind);
    return false;
  }
  memcpy(w->destMac, destMac, 6);
  w->kind = kind;
  if (!submitDeferredToCmdExec(runDeferredBondFileSend, w)) {
    free(w);
    BROADCAST_PRINTF("[BOND] file-send: submitDeferredToCmdExec FAILED kind=%u", (unsigned)kind);
    return false;
  }
  return true;
}

// (Step 3c moved the per-file static sendEncryptedSync — and the long-removed
// sendRawSync — to espnowtx::sendAeadSync and espnowtx::sendAead in
// System_ESPNow_Tx.h, so System_ESPNow_FsList.cpp and any other consumer can
// use them too. Both come in two variants: sendAeadSync (blocking, returns
// the dispatcher's OK/FAIL) for cmd_exec callers that want backpressure or
// branch on the send outcome, and sendAead (fire-and-forget, returns the
// queue-accepted bool) for RX-handler callers on espnow_task that must NOT
// block the RX drainer. See header for full task-context rules.)

// Master-side: ask the bonded peer to send its schema. Symmetric with
// requestBondSettings — same encrypted sender, no payload. Public (declared
// in System_ESPNow.h) so the WebPage_Bond.cpp endpoint can trigger schema
// resync without going through the sync tick. Schema rarely changes, so a
// direct trigger keeps it out of the sync state machine.
bool requestBondSchema(const uint8_t* peerMac) {
  if (!peerMac || !gEspNow || !gSettings.bondModeEnabled) return false;
  if (!isBondMaster()) return false;

  uint32_t msgId = generateMessageId();
  bool sent = bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_SCHEMA_REQ, 0, msgId, nullptr, 0);
  if (sent) {
    DEBUG_ESPNOWF("[SCHEMA_REQ] Sent (msgId=%lu)", (unsigned long)msgId);
  } else {
    ERROR_ESPNOWF("[SCHEMA_REQ] Failed to send");
  }
  return sent;
}

/**
 * Process received schema from bonded peer (master-side).
 * Validates JSON shape and caches under peers/<MAC>/schema.json. Sets
 * bondSchemaReceived so polling endpoints (handleBondSettingsSchemaSync)
 * see completion.
 */
static void processBondSchema(const uint8_t* srcMac, const String& deviceName, const String& schemaStr) {
  DEBUG_ESPNOWF("[SCHEMA_PROC] processBondSchema: srcMac=%p len=%d", srcMac, schemaStr.length());
  if (!srcMac) {
    DEBUG_ESPNOWF("[SCHEMA_PROC] EXIT: srcMac is NULL");
    return;
  }
  // Validate JSON envelope — reject truncated/corrupt transfers (mirrors
  // processBondSettings's check).
  if (schemaStr.length() < 2 || schemaStr[0] != '{' || schemaStr[schemaStr.length() - 1] != '}') {
    BROADCAST_PRINTF("[BOND_SYNC] REJECTED corrupt schema (len=%d, not valid JSON object)", schemaStr.length());
    return;
  }

  // Cache to peers/<MAC>/schema.json — same directory layout the settings
  // cache uses (cacheSettingsToLittleFS), just a different filename.
  char dirPath[48];
  peerCacheDir(srcMac, dirPath, sizeof(dirPath));
  char filePath[64];
  peerCachePath(srcMac, "schema.json", filePath, sizeof(filePath));
  DEBUG_ESPNOWF("[SCHEMA_PROC] Target path: %s", filePath);

  {
    FsLockGuard fsGuard("pair.schema.cache");
    if (!VFS::existsGuarded(dirPath, VFS::systemAuth("espnow.schema_cache_mkdir"))) {
      VFS::mkdirGuarded(dirPath, VFS::systemAuth("espnow.schema_cache_mkdir"));
    }
    File f = VFS::openGuarded(filePath, "w", VFS::systemAuth("espnow.schema_cache_write"), true);
    if (!f) {
      ERROR_ESPNOWF("[SCHEMA_PROC] Failed to open %s for writing", filePath);
      return;
    }
    size_t written = f.print(schemaStr);
    f.close();
    if (written != schemaStr.length()) {
      ERROR_ESPNOWF("[SCHEMA_PROC] Incomplete write (wrote %d of %d)", written, schemaStr.length());
      return;
    }
  }

  if (gEspNow) gEspNow->bondSchemaReceived = true;
  BROADCAST_PRINTF("[BOND_SYNC] Schema cached: %d bytes from %s", schemaStr.length(), deviceName.c_str());
}

/**
 * Fire the one-shot post-sync side effects: send the initial STATUS_REQ so the
 * peer's bondPeerStatusValid can populate, and (on master) queue STREAM_CTRL
 * replay so the worker mirrors the master's saved streaming prefs.
 *
 * Called from three places, all guarded by bondStatusReqSentOnce so it's
 * a no-op after the first successful fire per sync session:
 *   1) processBondSettings() — original site, on fresh settings file arrival
 *   2) cmd_bond_role()        — direct fire after role swap snapshot/restore
 *                              (processBondSettings won't run because we have
 *                              everything cached, so the side effects would
 *                              otherwise never trigger and the UI would hang
 *                              on "Establishing Bond" step 4 of 4 forever)
 *   3) Bond tick guard        — fallback for cases (1) and (2) where the
 *                              encrypted send failed because the session
 *                              wasn't yet ACTIVE; retries on next tick.
 *
 * If bondSendEncrypted fails (typically because the session isn't yet up),
 * we leave bondStatusReqSentOnce=false so a retry path can fire. On success
 * we set it true to prevent duplicate STATUS_REQs from the other call sites.
 */
static void firePostSyncSideEffects(const uint8_t peerMac[6]) {
  if (!gEspNow || !gSettings.bondModeEnabled) return;
  if (gEspNow->bondStatusReqSentOnce) return;  // already fired this sync session

  uint32_t statusReqId = generateMessageId();
  // Step 2 migration: route through espnow_tx. All callers (super-loop 7856,
  // processBondSettings, cmd_bond_role) already gate on session-ACTIVE upstream,
  // so the responder-no-session path that returned false in the sync version
  // can't fire here. Remaining false cases (queue full / ps_alloc fail) keep
  // the same "leave gate clear so a retry fires next tick" semantics.
  bool sent = bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_BOND_STATUS_REQ, 0,
                                     statusReqId, nullptr, 0);
  if (!sent) {
    // Queue full or alloc failed. Leave bondStatusReqSentOnce false so the
    // tick guard or next caller retries. No log spam — it's expected to retry.
    return;
  }
  gEspNow->bondStatusReqSentOnce = true;
  gEspNow->bondLastStatusReqMs = millis();

  // Master: push saved streaming prefs to worker now that sync is done.
  // Idempotent on worker side — STREAM_CTRL OFF on an off sensor is a no-op.
  if (isBondMaster()) {
    gEspNow->bondNeedsStreamingSetup = true;
  }
  BROADCAST_PRINTF("[BOND_SYNC] *** SYNC COMPLETE *** role=%d", (int)gSettings.bondRole);
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
  bool cached = cacheSettingsToLittleFS(srcMac, settingsStr);
  if (cached) {
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

    // Snapshot the CRC32 of the bytes we just cached. The peer's heartbeat
    // carries CRC32(generateDeviceSettings()), so a later heartbeat-reported
    // hash that differs from this snapshot means the worker has changed
    // settings since this cache write — the basis for "dirty" detection in
    // /api/bond/status and the bonded settings panel banner.
    if (cached) {
      gEspNow->bondCachedPeerSettingsHash =
        esp_crc32_le(0, (const uint8_t*)settingsStr.c_str(), settingsStr.length());
    }
    
    bool synced = isBondSynced();
    BROADCAST_PRINTF("[BOND_SYNC] Settings received, synced=%d role=%d", (int)synced, (int)gSettings.bondRole);
    
    if (synced) {
      // Bond token is derived from the encrypted session at handshake time
      // (sessionDeriveAeadKeys → SessionState::bondToken), so it is already
      // valid by first sync. Delegate the STATUS_REQ kick + streaming setup
      // to the shared helper so cmd_bond_role and the tick guard can fire
      // the same side effects after a snapshot/restore role swap.
      uint8_t pMac[6];
      if (parseMacAddress(gSettings.bondPeerMac, pMac)) {
        firePostSyncSideEffects(pMac);
      }
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
  if (!filesystemReady) return "";
  
  // Build filename from hash
  char keyHex[33];
  for (int i = 0; i < 16; i++) {
    snprintf(keyHex + (i * 2), 3, "%02x", fwHash[i]);
  }
  keyHex[32] = '\0';
  
  char path[64];
  snprintf(path, sizeof(path), "/system/manifests/%s.json", keyHex);
  
  if (!VFS::existsGuarded(path, VFS::systemAuth("espnow.manifest_load"))) {
    return "";  // Not cached
  }

  FsLockGuard fsGuard("pair.manifest.load");
  File f = VFS::openGuarded(path, "r", VFS::systemAuth("espnow.manifest_load"));
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
static void buildMetadataPayload(V4PayloadMetadata* payload) {
  if (!payload) return;
  
  memset(payload, 0, sizeof(V4PayloadMetadata));
  
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
  DEBUG_ESPNOW_METADATAF("[METADATA] Sending REQ (encrypted-or-queue) to %s msgId=%lu force=%d",
    MAC_STR(peerMac), (unsigned long)msgId, (int)force);
  // Phase 3.5 task #6 — METADATA_REQ rides SESSION_FRAME. Returns false if the
  // peer has no long-term identity yet (KEY_EX not done), no active session
  // and queue full, etc. — capture the real status so the WARN below isn't the
  // misleading "peer not in hw table?" (the actual cause is almost always
  // "no peer identity — run espnowkeyex").
  char status[96] = {0};
  bool sent = v4_send_encrypted_or_queue(peerMac, ESPNOW_V4_TYPE_METADATA_REQ, 0, msgId,
                                          nullptr, 0, 1, status, sizeof(status));

  if (sent) {
    DEBUG_ESPNOW_METADATAF("[METADATA] REQ sent OK to %s msgId=%lu (status=%s)",
      MAC_STR(peerMac), (unsigned long)msgId, status[0] ? status : "sent");
  } else {
    WARN_ESPNOWF("[METADATA] REQ FAILED to send to %s: %s",
      MAC_STR(peerMac), status[0] ? status : "encrypted send failed");
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
  
  V4PayloadMetadata payload;
  buildMetadataPayload(&payload);
  
  uint32_t msgId = generateMessageId();
  uint8_t type = isPush ? ESPNOW_V4_TYPE_METADATA_PUSH : ESPNOW_V4_TYPE_METADATA_RESP;
  DEBUG_ESPNOW_METADATAF("[METADATA] Sending %s (encrypted-or-queue) to %s msgId=%lu payloadLen=%u name='%s' room='%s' zone='%s' tags='%s' force=%d",
    isPush ? "PUSH" : "RESP", MAC_STR(peerMac), (unsigned long)msgId,
    (unsigned)sizeof(payload), payload.deviceName, payload.room, payload.zone, payload.tags, (int)force);

  // 2026-05-19 — flip to v4_send_encrypted_or_queue to mirror requestMetadata
  // (REQ direction was already encrypted post-Phase-3.5 task #6). Asymmetric
  // encryption let the response leak deviceName / room / zone / tags / etc.
  // in clear even when the REQ was protected. Strict-encrypt: if the peer
  // doesn't have an identity yet, the RESP/PUSH won't go out — but the only
  // path that can reach this function via REQ→RESP already required the REQ
  // to come encrypted, so the session already exists at the responder side.
  char status[96] = {0};
  bool sent = v4_send_encrypted_or_queue(peerMac, type, 0, msgId,
                                          (const uint8_t*)&payload, sizeof(payload), 1,
                                          status, sizeof(status));

  if (sent) {
    DEBUG_ESPNOW_METADATAF("[METADATA] %s sent OK to %s msgId=%lu (status=%s)",
      isPush ? "PUSH" : "RESP", MAC_STR(peerMac), (unsigned long)msgId,
      status[0] ? status : "sent");
  } else {
    WARN_ESPNOWF("[METADATA] %s FAILED to send to %s: %s",
      isPush ? "PUSH" : "RESP", MAC_STR(peerMac),
      status[0] ? status : "encrypted send failed");
  }
}

/**
 * Process received metadata - store in gMeshPeerMeta
 */
static void processMetadata(const uint8_t* srcMac, const V4PayloadMetadata* metadata) {
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
  
  // Sync metadata into paired device entry (for persistence across reboots)
  if (gEspNow) {
    for (int i = 0; i < gEspNow->deviceCount; i++) {
      if (memcmp(gEspNow->devices[i].mac, srcMac, 6) == 0) {
        bool changed = false;
        if (metadata->friendlyName[0] && gEspNow->devices[i].friendlyName != metadata->friendlyName) {
          gEspNow->devices[i].friendlyName = metadata->friendlyName; changed = true;
        }
        if (metadata->room[0] && gEspNow->devices[i].room != metadata->room) {
          gEspNow->devices[i].room = metadata->room; changed = true;
        }
        if (metadata->zone[0] && gEspNow->devices[i].zone != metadata->zone) {
          gEspNow->devices[i].zone = metadata->zone; changed = true;
        }
        if (metadata->tags[0] && gEspNow->devices[i].tags != metadata->tags) {
          gEspNow->devices[i].tags = metadata->tags; changed = true;
        }
        bool newStationary = (metadata->stationary != 0);
        if (gEspNow->devices[i].stationary != newStationary) {
          gEspNow->devices[i].stationary = newStationary; changed = true;
        }
        if (changed) {
          saveEspNowDevices();
        }
        break;
      }
    }
  }
}


// ==========================
// Message Handler Implementations
// ==========================

// Helper: Generate unique message ID
uint32_t generateMessageId() {
  if (!gEspNow) return 0;
  // Atomic post-increment: called by every sender on every task (espnow_task,
  // cmd_exec_task, SENSOR_BCAST_TASK). A plain `++` is a non-atomic
  // read-modify-write — two cores racing it mint duplicate msgIds, which the
  // receiver's (origin,msgId) dedup then drops as a false duplicate. The GCC
  // builtin keeps the struct member a plain uint32_t (no std::atomic copyability
  // constraint on EspNowState, and the read in printEspNowStatus stays a normal
  // aligned load). RELAXED is correct: msgIds only need uniqueness, not ordering
  // against other memory.
  return __atomic_fetch_add(&gEspNow->nextMessageId, 1, __ATOMIC_RELAXED);
}

// Helper: Check if chunking is needed
bool shouldChunk(size_t size) {
  return size > 250;
}

// Helper: Update unpaired device tracking

// ============================================================================
// TOPOLOGY STREAM MANAGEMENT (moved from .ino)
// ============================================================================


// Finalize a topology stream: flush accumulated data into global results buffer
static void finalizeTopologyStream(TopologyStream* stream) {
  if (!stream) return;
  char macBuf[18];
  formatMacAddressBuf(stream->senderMac, macBuf, sizeof(macBuf));
  char entryHeader[64];
  snprintf(entryHeader, sizeof(entryHeader), "%s (%s):\n", stream->senderName, macBuf);
  gTopoResultsBuffer += entryHeader;
  if (stream->accumulatedData.length() > 0) {
    gTopoResultsBuffer += stream->accumulatedData;
  } else {
    gTopoResultsBuffer += "  (no peers)\n";
  }
  gTopoResultsBuffer += "\n";
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
static bool getTopoDeviceName(const uint8_t* mac, char* outBuf, size_t outLen) {
  if (!mac || !outBuf || outLen == 0) return false;
  outBuf[0] = '\0';
  for (int i = 0; i < MAX_TOPO_DEVICE_CACHE; i++) {
    if (gTopoDeviceCache[i].active && memcmp(gTopoDeviceCache[i].mac, mac, 6) == 0) {
      strlcpy(outBuf, gTopoDeviceCache[i].name, outLen);
      return true;
    }
  }
  return false;
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
  macToDisplay(mac, buf, bufSize);  // canonical DISPLAY form (System_Utils.h)
}

// Helper: Format MAC address as string (convenience wrapper, allocates String)
String formatMacAddress(const uint8_t* mac) {
  return macToDisplayStr(mac);  // canonical DISPLAY form (System_Utils.h)
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
  EspNowDevice& newDev = gEspNow->devices[gEspNow->deviceCount];
  memcpy(newDev.mac, mac, 6);
  newDev.name = name;
  newDev.encrypted = encrypted;
  if (encrypted && key) {
    memcpy(newDev.key, key, 16);
  } else {
    memset(newDev.key, 0, 16);
  }
  newDev.friendlyName = "";
  newDev.room = "";
  newDev.zone = "";
  newDev.tags = "";
  newDev.stationary = false;
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
  
  // No device name configured — require the user to set one before ESP-NOW can start.
  return "Error: No device name configured. Set one with: espnow setname <name>";
}

// Load named ESP-NOW devices (paired devices with names/keys) from filesystem
static void loadEspNowDevices() {
  if (!gEspNow) return;
  if (!VFS::existsGuarded(ESPNOW_DEVICES_FILE, VFS::systemAuth("espnow.devices_load"))) {
    DEBUGF(DEBUG_ESPNOW_MESH, "[ESP-NOW] No saved devices file at %s", ESPNOW_DEVICES_FILE);
    return;
  }
  File f = VFS::openGuarded(ESPNOW_DEVICES_FILE, "r", VFS::systemAuth("espnow.devices_load"));
  if (!f) {
    WARN_ESPNOWF("[ESP-NOW] Failed to open %s for reading", ESPNOW_DEVICES_FILE);
    return;
  }
  String content = f.readString();
  f.close();
  if (content.isEmpty()) return;

  PSRAM_JSON_DOC(doc);
  DeserializationError err = deserializeJson(doc, content);
  if (err) {
    WARN_ESPNOWF("[ESP-NOW] Failed to parse %s: %s", ESPNOW_DEVICES_FILE, err.c_str());
    return;
  }

  JsonArray arr = doc["devices"].as<JsonArray>();
  if (!arr) return;

  int count = 0;
  for (JsonObject entry : arr) {
    if (gEspNow->deviceCount >= 16) break;
    const char* rawMac = entry["mac"] | "";
    const char* name   = entry["name"] | "";
    if (!rawMac[0]) continue;

    // Decrypt MAC if stored encrypted
    String macStr = String(rawMac);
    if (macStr.startsWith("AES:")) {
      String decrypted = decryptString(macStr);
      if (decrypted.length() > 0) {
        macStr = decrypted;
      } else {
        WARN_ESPNOWF("[ESP-NOW] Failed to decrypt device MAC for '%s', skipping", name);
        continue;
      }
    }

    uint8_t mac[6];
    if (!parseMacAddress(macStr, mac)) continue;

    // Check if this MAC is already loaded (prevents duplicates from corrupted JSON)
    bool alreadyLoaded = false;
    for (int i = 0; i < gEspNow->deviceCount; i++) {
      if (memcmp(gEspNow->devices[i].mac, mac, 6) == 0) {
        alreadyLoaded = true;
        WARN_ESPNOWF("[ESP-NOW] Skipping duplicate device in saved file: %s (%s)", name, macStr.c_str());
        break;
      }
    }
    if (alreadyLoaded) continue;

    EspNowDevice& dev = gEspNow->devices[gEspNow->deviceCount];
    memcpy(dev.mac, mac, 6);
    dev.name      = String(name);
    dev.encrypted = entry["encrypted"] | false;
    memset(dev.key, 0, 16);

    // Decrypt encryption key if present
    String keyStr = String(entry["key"] | "");
    if (dev.encrypted && keyStr.length() > 0) {
      if (keyStr.startsWith("AES:")) {
        String decryptedKey = decryptString(keyStr);
        if (decryptedKey.length() == 32) {
          keyStr = decryptedKey;
        } else {
          WARN_ESPNOWF("[ESP-NOW] Failed to decrypt encryption key for '%s'", name);
          keyStr = "";
        }
      }
      if (keyStr.length() == 32) {
        for (int i = 0; i < 16; i++) {
          char byte[3] = { keyStr[i*2], keyStr[i*2+1], '\0' };
          dev.key[i] = (uint8_t)strtol(byte, nullptr, 16);
        }
      }
    }
    // Restore cached metadata fields (backwards-compatible: missing fields default to empty)
    dev.friendlyName = String(entry["friendlyName"] | "");
    dev.room         = String(entry["room"] | "");
    dev.zone         = String(entry["zone"] | "");
    dev.tags         = String(entry["tags"] | "");
    dev.stationary   = entry["stationary"] | false;
    // Phase 2 multi-mesh — meshId persisted on save; default 0 (primary mesh)
    // when missing for backwards-compat with pre-Phase-2 devices.json files.
    {
      uint8_t loadedMid = (uint8_t)(entry["meshId"] | 0);
      dev.meshId = (loadedMid < Settings::N_MESHES) ? loadedMid : 0;
    }
    gEspNow->deviceCount++;
    count++;
  }
  DEBUGF(DEBUG_ESPNOW_MESH, "[ESP-NOW] Loaded %d device(s) from %s", count, ESPNOW_DEVICES_FILE);
}


// ============================================================================
// Public helper functions
// ============================================================================

// Convert 6-byte MAC to colon-separated hex string ("AA:BB:CC:DD:EE:FF")
String macToHexString(const uint8_t mac[6]) {
  return macToDisplayStr(mac);  // canonical DISPLAY form (System_Utils.h)
}

// Parse MAC string ("AA:BB:CC:DD:EE:FF") into byte array (fills zeros on parse failure)
void macFromHexString(const String& s, uint8_t out[6]) {
  // Canonical lenient parser (System_Utils.h). Preserve the historical
  // contract: zero-fill the output on any parse failure.
  if (!macParse(s.c_str(), out)) {
    memset(out, 0, 6);
  }
}

// Find (or optionally create) a MeshPeerHealth slot for a given MAC.
//
// INVARIANT: when this function creates a new health slot, it ALSO ensures
// a matching meta slot exists (empty meta is fine — name/room get filled in
// later when the identity/topology message arrives, and displayName() falls
// back to "Unknown" until then). Without this guarantee, the two arrays
// drift: heartbeats and ACKs from an unknown peer go through this path
// (via noteMeshPeerRxActivity) on EVERY frame and create health slots,
// but meta is only created on a separate identity-message path that fires
// much less often and only after the peer has been topologically learned.
// The drift surfaced as the "1/0 devices" status display — countHealthy
// from gMeshPeers vs count of active gMeshPeerMeta slots could disagree
// because they were populated by different code paths.
//
// One-directional only (health→meta): meta-side creation does NOT call
// back here, so no recursion. In practice meta is rarely created without
// an associated activity event, but if a code path ever does create meta
// solo, we'd then also need the inverse — left as a future cleanup if it
// becomes a problem; current call sites don't exercise it.
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
      // Enforce the health⇒meta invariant. Ignore return value: if meta
      // allocation fails (table full) we still return the health slot —
      // counts will be off by one but that's the worst case, not a crash.
      (void)getMeshPeerMeta(mac, true);
      return &gMeshPeers[i];
    }
  }
  return nullptr;
}

// Find (or optionally create) a MeshPeerMeta slot for a given MAC. Mirrors
// getMeshPeerHealth's shape so callers can reach for whichever side of the
// peer state they need (health = liveness counters, meta = identity strings).
// Previously declared in System_ESPNow.h but never implemented — callers
// (OLED_ESPNow, WebPage_ESPNow_Metadata) worked around it by inlining the
// search loop, which is exactly the duplication this consolidation removes.
MeshPeerMeta* getMeshPeerMeta(const uint8_t mac[6], bool createIfMissing) {
  if (!gMeshPeerMeta) return nullptr;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeerMeta[i].isActive && memcmp(gMeshPeerMeta[i].mac, mac, 6) == 0)
      return &gMeshPeerMeta[i];
  }
  if (!createIfMissing) return nullptr;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (!gMeshPeerMeta[i].isActive) {
      gMeshPeerMeta[i].clear();
      memcpy(gMeshPeerMeta[i].mac, mac, 6);
      gMeshPeerMeta[i].isActive = true;
      return &gMeshPeerMeta[i];
    }
  }
  return nullptr;
}

// Count active mesh peers whose room field matches `room` (case-sensitive,
// exact match). Used by room-grouped UI views (OLED room menu, automation
// scoping). NULL/empty `room` returns 0 — "no room" peers aren't aggregated.
int countMeshPeerMetaByRoom(const char* room) {
  if (!gMeshPeerMeta || !room || !room[0]) return 0;
  int count = 0;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeerMeta[i].isActive && strcmp(gMeshPeerMeta[i].room, room) == 0)
      count++;
  }
  return count;
}

void noteMeshPeerRxActivity(const uint8_t* mac, EspNowMeshRxKind kind, int8_t hbRssi) {
  if (!mac || !gMeshPeers) return;
  MeshPeerHealth* peer = getMeshPeerHealth(mac, true);
  if (!peer) return;
  const uint32_t t = millis();
  switch (kind) {
    case EspNowMeshRxKind::MeshHeartbeat:
      peer->lastMeshHeartbeatMs = t;
      peer->lastRxActivityMs = t;
      peer->heartbeatCount++;
      if (hbRssi != (int8_t)-128) peer->rssi = hbRssi;
      break;
    case EspNowMeshRxKind::Ack:
      peer->lastAckMs = t;
      peer->lastRxActivityMs = t;
      peer->ackCount++;
      break;
    case EspNowMeshRxKind::RxActivity:
      peer->lastRxActivityMs = t;
      break;
    case EspNowMeshRxKind::BootstrapLiveness:
      peer->lastMeshHeartbeatMs = t;
      peer->lastRxActivityMs = t;
      break;
    default:
      break;
  }
  peer->isActive = true;
}

// Check if a mesh peer is considered alive (mesh V3 HEARTBEAT within timeout window)
bool isMeshPeerAlive(const MeshPeerHealth* peer) {
  if (!peer || !peer->isActive) return false;
  if (peer->lastMeshHeartbeatMs == 0) return false;
  return (millis() - peer->lastMeshHeartbeatMs) < MESH_PEER_TIMEOUT_MS;
}

// Recent contact via any tracked RX (TEXT, ACK, HEARTBEAT, bootstrap)
bool isMeshPeerRecentlyActive(const MeshPeerHealth* peer) {
  if (!peer || !peer->isActive) return false;
  if (peer->lastRxActivityMs == 0) return false;
  return (millis() - peer->lastRxActivityMs) < MESH_PEER_TIMEOUT_MS;
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

static void fillMeshStatusPeerJsonObject(JsonObject peer, uint32_t now, const uint8_t* mac,
                                        const char* nameOpt, MeshPeerHealth* ph) {
  // MAC formatted into stack buffer — must wrap in String() so ArduinoJson
  // deep-copies (otherwise the stored char* dangles after we return).
  char macBuf[18];
  snprintf(macBuf, sizeof(macBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  peer["mac"] = String(macBuf);
  // Name lookup: caller's override wins; otherwise fall back to the shared
  // meta-then-paired-registry resolver. Wrap in String() unconditionally so
  // ArduinoJson deep-copies and we don't risk a dangling pointer.
  if (nameOpt && nameOpt[0]) {
    peer["name"] = String(nameOpt);
  } else {
    String resolved = getEspNowDeviceName(mac);
    peer["name"] = resolved.length() ? resolved : String("Unknown");
  }
  if (ph) {
    uint32_t elHb = now - ph->lastMeshHeartbeatMs;
    if (elHb > 0x80000000UL) elHb = 0;
    uint32_t elAct = 0;
    if (ph->lastRxActivityMs) {
      elAct = now - ph->lastRxActivityMs;
      if (elAct > 0x80000000UL) elAct = 0;
    }
    peer["alive"] = isMeshPeerAlive(ph);
    peer["activityAlive"] = isMeshPeerRecentlyActive(ph);
    peer["lastHeartbeat"] = ph->lastMeshHeartbeatMs;
    peer["lastRxActivity"] = ph->lastRxActivityMs;
    peer["lastAck"] = ph->lastAckMs;
    peer["heartbeatCount"] = ph->heartbeatCount;
    peer["ackCount"] = ph->ackCount;
    peer["secondsSinceHeartbeat"] = elHb / 1000;
    peer["secondsSinceActivity"] = ph->lastRxActivityMs ? (elAct / 1000) : 0;
  } else {
    peer["alive"] = false;
    peer["activityAlive"] = false;
    peer["lastHeartbeat"] = 0;
    peer["lastRxActivity"] = 0;
    peer["lastAck"] = 0;
    peer["heartbeatCount"] = 0;
    peer["ackCount"] = 0;
    peer["secondsSinceHeartbeat"] = 0;
    peer["secondsSinceActivity"] = 0;
  }
}

void buildMeshStatusPeersJson(JsonArray peers, uint32_t nowMillis, int* outTotalPeers) {
  int n = 0;
  if (!gEspNow || !gMeshPeers) {
    if (outTotalPeers) *outTotalPeers = 0;
    return;
  }
  const uint32_t now = nowMillis;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (!gMeshPeers[i].isActive || isSelfMac(gMeshPeers[i].mac)) continue;
    JsonObject peer = peers.add<JsonObject>();
    fillMeshStatusPeerJsonObject(peer, now, gMeshPeers[i].mac, nullptr, &gMeshPeers[i]);
    n++;
  }
  for (int di = 0; di < gEspNow->deviceCount; di++) {
    const uint8_t* dmac = gEspNow->devices[di].mac;
    if (isSelfMac(dmac)) continue;
    bool inMeshSlots = false;
    for (int j = 0; j < gMeshPeerSlots; j++) {
      if (gMeshPeers[j].isActive && !isSelfMac(gMeshPeers[j].mac) &&
          memcmp(gMeshPeers[j].mac, dmac, 6) == 0) {
        inMeshSlots = true;
        break;
      }
    }
    if (inMeshSlots) continue;
    MeshPeerHealth* ph = getMeshPeerHealth(dmac, false);
    JsonObject peer = peers.add<JsonObject>();
    const char* dn = gEspNow->devices[di].name.length() ? gEspNow->devices[di].name.c_str() : nullptr;
    fillMeshStatusPeerJsonObject(peer, now, dmac, dn, ph);
    n++;
  }
  if (outTotalPeers) *outTotalPeers = n;
}

// Set mesh role at runtime with logging. Does not persist — reboot restores saved role.
void setMeshRole(MeshRole role, const char* reason) {
  if (gSettings.meshRole == (uint8_t)role) return;
  BROADCAST_PRINTF("[MESH_ROLE] %d -> %d | %s", (int)gSettings.meshRole, (int)role, reason ? reason : "");
  gSettings.meshRole = (uint8_t)role;
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
  // ArduinoJson stores const char* by reference and strlen()s it at serialize
  // time — a null pointer => strlen(NULL) crash. Guard every raw pointer.
  doc["type"] = type ? type : "";
  doc["id"]   = msgId;
  doc["src"]  = src ? src : "";
  if (dst && dst[0]) doc["dst"] = dst;
  if (ttl >= 0)      doc["ttl"] = ttl;
}

// Send a serialized JSON string to all active mesh peers via V4 TEXT frames.
// 2026-05: now routes through v4_send_payload_smart (strict encrypt-or-fail),
// not v4_send_chunked (deleted). Peers without an active session get the
// auto-handshake treatment from smart's _or_queue path — first envelope to a
// new peer queues + kicks KEY_EX/SESSION_OPEN, drains when the session is up.
// Cross-mesh peers can't be in gMeshPeers (no shared bootstrap key, so KEY_EX
// would never have established their identity), so this is exclusively
// same-mesh fan-out — encryption always works once the handshake settles.
static void meshBroadcastEnvelopeTyped(const String& envelope, uint8_t v4Type) {
  if (!gEspNow || !gEspNow->initialized || !gMeshPeers) return;
  const uint8_t* data = (const uint8_t*)envelope.c_str();
  uint16_t len = (uint16_t)envelope.length();
  uint32_t msgId = generateMessageId();
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac))
      v4_send_payload_smart(gMeshPeers[i].mac, v4Type, 0, msgId, data, len, 3);
  }
}

// Generic envelope broadcast (legacy v2/v3 wrapper) — goes out as TEXT.
void meshSendEnvelopeToPeers(const String& envelope) {
  meshBroadcastEnvelopeTyped(envelope, ESPNOW_V4_TYPE_TEXT);
}

// Boot/online notice — its OWN V4 type so the receiver files it as a system
// event (MSG_SYSTEM_EVENT), not a chat bubble. (It used to ride
// ESPNOW_V4_TYPE_TEXT, so every reboot showed up as a chat message on peers.)
void meshSendBootToPeers(const String& envelope) {
  meshBroadcastEnvelopeTyped(envelope, ESPNOW_V4_TYPE_BOOT);
}

// Build a JSON boot notification string
String buildBootNotification(uint32_t msgId, const char* src,
                             uint32_t bootCounter, uint32_t timestamp) {
  PSRAM_JSON_DOC(doc);
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
  V4PayloadTopoReq req  = {};
  req.reqId = gTopoRequestId;
  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac))
      v4_send_frame(gMeshPeers[i].mac, ESPNOW_V4_TYPE_TOPO_REQ, 0,
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

// True if a file transfer FROM `mac` is currently mid-flight (RECEIVING).
// Used by the bond-sync retry logic so we don't fire a duplicate MANIFEST_REQ
// (or SETTINGS_REQ) while the answer is already streaming in chunk-by-chunk —
// a manifest is ~220 chunks / several seconds, easily longer than the retry
// window, which otherwise triggers a redundant full re-send.
static bool espnowInboundFileActiveFrom(const uint8_t mac[6]) {
  uint8_t n = fileSlotsSlotCount();
  for (uint8_t i = 0; i < n; i++) {
    FileTransferSlotInfo info;
    if (!fileSlotsSnapshot(i, &info)) continue;
    if (info.state == FILE_SLOT_RECEIVING && memcmp(info.peerMac, mac, 6) == 0)
      return true;
  }
  return false;
}

// Upper bound on how long an outbound-send flag defers a rekey. Generously above
// the worst-case in-cap transfer (4 MB @ ~9 KB/s ≈ 8 min) so a real max-size send
// is never cut short, while a flag left stuck by a task that died mid-send can't
// defer a rotation forever. Raise alongside kFileSlotMaxStreamSize if the cap grows.
static constexpr uint32_t kFileSendMaxDeferMs = 15u * 60u * 1000u;

// True while a file transfer in EITHER direction is in progress with this peer —
// the rekey scheduler defers a key rotation for the peer while this holds, so a
// rotation never lands mid-transfer. Outbound: a flag set by FileSendActiveGuard
// around sendFileToMac (the slot table is inbound-only), bounded by
// kFileSendMaxDeferMs so a stuck flag can't defer forever. Inbound: reuse
// espnowInboundFileActiveFrom — a RECEIVING slot self-bounds via kFileSlotTimeoutMs.
static bool espnowFileTransferActiveWithPeer(const uint8_t* peerMac) {
  if (!peerMac) return false;
  if (gEspNow && gEspNow->fileSendInProgress &&
      memcmp(gEspNow->fileSendPeer, peerMac, 6) == 0 &&
      ((uint32_t)millis() - gEspNow->fileSendStartedMs) < kFileSendMaxDeferMs) {
    return true;
  }
  return espnowInboundFileActiveFrom(peerMac);
}

#if ENABLE_BONDED_MODE
// Event-driven gate for bond manifest/settings file sends. Returns true if the
// send should proceed NOW — either an encrypted session to `mac` is ACTIVE, or
// we've already waited past the deadline and should fall back to plaintext.
// Returns false to DEFER: the caller leaves its pending flag set and retries on
// a later tick.
//
// This MUST NOT block. It runs on espnow_task, which is also the RX-ring
// drainer. Blocking to wait for the session would stall the very task that
// processes the inbound SESSION_CONFIRM, so the session could never reach
// ACTIVE — exactly the self-deadlock the old in-sendFileToMac poll loop hit.
// By deferring instead, espnow_task keeps draining RX, the handshake completes
// (SESSION_CONFIRM → cmd_exec sets the session ACTIVE), and a subsequent tick
// observes SESSION_ACTIVE and sends encrypted.
static bool bondSendReadyOrDeferred(const uint8_t* mac) {
  if (!gEspNow) return true;
  const PeerIdentity* pid = peerIdentityFindByMac(mac);
  if (!pid) {                              // no identity → no session possible
    gEspNow->bondSendWaitDeadlineMs = 0;
    return true;                           // send now (plaintext)
  }
  SessionState* s = sessionFindByPeer(mac, pid->meshId);
  if (s && s->state == SESSION_ACTIVE) {
    gEspNow->bondSendWaitDeadlineMs = 0;
    return true;                           // session up → send encrypted
  }
  uint32_t now = (uint32_t)millis();
  if (gEspNow->bondSendWaitDeadlineMs == 0) {
    // First miss: kick the handshake (no-op if already in flight) and arm the
    // wait, then defer so RX keeps draining and the handshake can complete.
    espnowSessionOpenInitiate(mac, nullptr);
    gEspNow->bondSendWaitDeadlineMs = now + 4000;  // wait up to 4 s for session
    return false;
  }
  if ((int32_t)(gEspNow->bondSendWaitDeadlineMs - now) <= 0) {
    gEspNow->bondSendWaitDeadlineMs = 0;
    return true;                           // deadline passed → plaintext fallback
  }
  return false;                            // still racing → defer to next tick
}
#endif // ENABLE_BONDED_MODE

// Main heartbeat task body: drain RX ring, send periodic HB, process queues.
//
// ARCHITECTURE INVARIANT (espnow_task ownership) — read before adding work here:
//   espnow_task is BOTH the RX-ring drainer (step 1 below) AND the bond/mesh
//   orchestrator. Because it drains RX, it must stay responsive: it must never
//   BLOCK waiting on something that itself depends on RX being processed. The
//   canonical trap is waiting for a session to go ACTIVE — the SESSION_CONFIRM
//   that completes it is delivered via RX and finished on cmd_exec, so blocking
//   here (or on cmd_exec) deadlocks the handshake against itself. Use the
//   event-driven, defer-and-retry pattern (see bondSendReadyOrDeferred) instead.
//   Heavy/long inline work (FS writes, JSON parse, AEAD, multi-second file
//   transfers) stalls RX for its whole duration; prefer submitDeferredToCmdExec
//   for infrequent heavy handlers — but never defer a session-blocking wait to
//   cmd_exec, which serializes against runDeferredSessionConfirm.
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

  // 1b. Phase 3.5 — sweep expired pending encrypted frames (queued while
  // waiting for SESSION_OPEN that never completed). 5-second budget per slot.
  pendingFrameTimeoutSweep((uint32_t)millis());
  // 1b-2. Reset sessions stuck in ESTABLISHING (our SESSION_OPEN got no CONFIRM
  // because the peer was offline/booting when we kicked it). Clearing the slot
  // lets the next bond/encrypted send re-initiate — without this the bond never
  // reconnects after the peer reboots out from under an in-flight handshake.
  sessionEstablishingTimeoutSweep((uint32_t)millis());
  // 1c. Phase 3.5 task #49 — age out tracked-send entries (PENDING → TIMEOUT
  // after 10s; resolved entries cleared after 30s so the polling window has
  // time to surface them to the web UI).
  sendStatusSweep((uint32_t)millis());
  // 1d. Phase 4 — stale file-transfer slot sweep. Slots with no frame in
  // kFileSlotTimeoutMs (30s) are released, freeing their PSRAM buffer. Each
  // expired transfer's sender gets a FILE_CANCEL(TIMEOUT) so it stops assuming
  // the (fire-and-forget) send landed.
  {
    FileSlotExpiry expiredSlots[4];
    uint8_t nExpired = fileSlotsTimeoutSweep((uint32_t)millis(), expiredSlots, 4);
    for (uint8_t i = 0; i < nExpired && i < 4; i++) {
      v4_send_file_cancel(expiredSlots[i].peerMac, expiredSlots[i].msgId, FILE_CANCEL_TIMEOUT);
    }
  }
  // 1d2. Phase 4b — drive the FS_LIST protocol: build deferred replies +
  // time out pending sender requests. Fast path when nothing's queued.
  extern void fsListTick();
  fsListTick();
  // 1d. Phase 3.6 — zero expired prev-keys (held briefly after a REKEY so
  // in-flight frames sent under the old keys still decrypt).
  sessionRekeyPrevKeysSweep((uint32_t)millis());
  // 1d2. F6 — re-send unanswered KEY_EX_HELLOs / expire dead handshakes.
  keyExRetrySweep((uint32_t)millis());
  // 1e. Phase 3.6 — auto-trigger REKEY on threshold (txSeq>=10k OR age>=1h).
  // Walks the SessionState table; uses the kRekeyMinIntervalMs guard inside
  // espnowRekeyInitiate to avoid re-firing while a rekey is in flight.
  {
    uint32_t nowMs2 = (uint32_t)millis();
    uint8_t nSessions = sessionSlotCount();
    for (uint8_t i = 0; i < nSessions; i++) {
      const SessionState* sc = sessionAt(i);
      if (!sc || sc->state != SESSION_ACTIVE) continue;
      bool ageTrigger = (nowMs2 - sc->establishedAtMs) >= kRekeyAgeThresholdMs;
      bool txTrigger  = sc->txSeqNext >= kRekeyTxFramesThreshold;
      bool wantRekey  = ageTrigger || txTrigger;
      if (wantRekey && espnowFileTransferActiveWithPeer(sc->peerMac)) {
        // Defer: a key rotation must not land mid file-transfer. A >2 MB transfer
        // crosses the 10k tx threshold, and rekeying mid-transfer is the one
        // untested interaction. Safe to wait — the transfer adds at most ~21k
        // frames (far from the 4-billion nonce wrap) and the next tick after it
        // ends will rekey. Self-bounding via the send flag's staleness + slot
        // timeouts, so a stuck transfer can't postpone rotation forever.
        DEBUG_ESPNOWF("REKEY deferred (file transfer active with peer) sessionId=%u txSeq=%lu",
                      (unsigned)sc->sessionId, (unsigned long)sc->txSeqNext);
      } else if (wantRekey) {
        // Log once per trigger (the initiate function's guard prevents spam).
        INFO_ESPNOWF("REKEY auto-trigger for sessionId=%u peer=%02X:%02X:%02X:%02X:%02X:%02X "
                     "(reason=%s txSeq=%lu ageMs=%lu)",
                     (unsigned)sc->sessionId,
                     sc->peerMac[0], sc->peerMac[1], sc->peerMac[2],
                     sc->peerMac[3], sc->peerMac[4], sc->peerMac[5],
                     txTrigger ? (ageTrigger ? "tx+age" : "tx") : "age",
                     (unsigned long)sc->txSeqNext,
                     (unsigned long)(nowMs2 - sc->establishedAtMs));
        espnowRekeyInitiate(sc->peerMac);
      }
    }
  }

  // 2. Send periodic V3 mesh heartbeat (if we have active peers OR paired devices)
  static const uint32_t HB_INTERVAL_MS = 5000;
  uint32_t now = (uint32_t)millis();
  if (now - gLastHeartbeatSentMs >= HB_INTERVAL_MS) {
    gLastHeartbeatSentMs = now;
    // Count active runtime peers
    uint8_t activePeerCount = 0;
    for (int i = 0; i < gMeshPeerSlots; i++) {
      if (gMeshPeers && gMeshPeers[i].isActive && !isSelfMac(gMeshPeers[i].mac))
        activePeerCount++;
    }
    // Also count paired devices from registry (bootstrap case: after reboot, gMeshPeers is empty)
    // and — crucially — create a mesh-health slot for each. v4_broadcast_category()
    // below ONLY fans the heartbeat out to active gMeshPeers entries, so without
    // this the heartbeat is "sent" to an empty list (sent=0) and the mesh never
    // comes up after a reboot until the peers are manually re-paired. Bootstrapping
    // the slot here lets the first heartbeat reach the paired peer, whose reply then
    // populates the slot properly via noteMeshPeerRxActivity. Idempotent: returns the
    // existing slot if already present. peerIdentityWantsEvent() defaults to true for
    // peers with no explicit subscription, so the bootstrapped peer is sent, not skipped.
    uint8_t pairedDeviceCount = 0;
    if (gEspNow && gEspNow->deviceCount > 0) {
      for (int i = 0; i < gEspNow->deviceCount; i++) {
        if (!isSelfMac(gEspNow->devices[i].mac)) {
          pairedDeviceCount++;
          getMeshPeerHealth(gEspNow->devices[i].mac, true);  // bootstrap slot so the heartbeat reaches it
        }
      }
    }
    // Send heartbeat if we have either active peers OR paired devices
    if (activePeerCount > 0 || pairedDeviceCount > 0) {
      V4PayloadHeartbeat hb = {};
      hb.role = gSettings.meshRole;
      hb.peerCount = activePeerCount > 0 ? activePeerCount : pairedDeviceCount;
      wifi_ap_record_t ap = {};
      hb.rssi      = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : (int8_t)-127;
      hb.uptimeSec = now / 1000;
      hb.freeHeap  = (uint32_t)ESP.getFreeHeap();
      strncpy(hb.deviceName, gSettings.espnowDeviceName.c_str(), sizeof(hb.deviceName) - 1);
      // Phase 5: gate on per-peer HEARTBEAT subscription. Peers default to
      // subscribed-to-all, so this is invisible until a peer opts out.
      v4_broadcast_category(ESPNOW_V4_TYPE_HEARTBEAT, ESPNOW_V4_FLAG_ACK_REQ,
                            generateMessageId(),
                            (const uint8_t*)&hb, (uint16_t)sizeof(hb), 1,
                            ESPNOW_EVT_HEARTBEAT);
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
        V4PayloadHeartbeat hb = {};
        hb.role      = MESH_ROLE_MASTER;
        hb.uptimeSec = now / 1000;
        hb.freeHeap  = (uint32_t)ESP.getFreeHeap();
        wifi_ap_record_t ap = {};
        hb.rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : (int8_t)-127;
        strncpy(hb.deviceName, gSettings.espnowDeviceName.c_str(), sizeof(hb.deviceName) - 1);
        // 2026-05-19: route through smart-send so master→backup liveness rides
        // SESSION_FRAME once KEY_EX has run. The receiver's backup-master
        // tracking now requires ctx.isAuthenticated, so a plaintext fallback
        // here is harmless (won't drive false failover state) but stays
        // available for the pre-KEY_EX window.
        v4_send_payload_smart(backupMac, ESPNOW_V4_TYPE_HEARTBEAT, ESPNOW_V4_FLAG_ACK_REQ,
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
    setMeshRole(MESH_ROLE_MASTER, "backup.promoted");  // Runtime only — not persisted, reboot restores backup role
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
        V4PayloadBondHeartbeat phb = {};
        phb.role      = gSettings.bondRole;
        phb.uptimeSec = now / 1000;
        phb.freeHeap  = (uint32_t)ESP.getFreeHeap();
        phb.seqNum    = ++gBondHeartbeatSeqNum;
        extern uint32_t gBootCounter;
        phb.bootCounter = gBootCounter;
        phb.settingsHash = gEspNow ? gEspNow->bondLocalSettingsHash : 0;
        wifi_ap_record_t ap2 = {};
        phb.rssi = (esp_wifi_sta_get_ap_info(&ap2) == ESP_OK) ? ap2.rssi : (int8_t)-127;
        // Encrypted + initiator-only: bondSendEncrypted kicks the session on the
        // initiator (higher-MAC) side; the responder skips until the session is up.
        // The session handshake is now what bootstraps the bond (see
        // bondNotifySessionEstablished), so the heartbeat no longer needs to be
        // the plaintext discovery beacon.
        bool sent = bondSendEncryptedAsync(bondMac, ESPNOW_V4_TYPE_BOND_HEARTBEAT, 0,
                      generateMessageId(), (const uint8_t*)&phb, (uint16_t)sizeof(phb));
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
    resetBondSync();
    // All-encrypted bond: tear down the (now-stale) session so the next send
    // re-establishes a fresh one. Critical for peer-reboot recovery — if the
    // peer rebooted it has no session, would silently drop our encrypted frames,
    // and (since our slot is still ACTIVE) bondSendEncrypted would never re-kick
    // a SESSION_OPEN. Clearing it makes the next initiator send rebuild the session.
    uint8_t offMac[6];
    if (parseMacAddress(gSettings.bondPeerMac, offMac)) {
      const PeerIdentity* offPid = peerIdentityFindByMac(offMac);
      SessionState* offS = offPid ? sessionFindByPeer(offMac, offPid->meshId) : nullptr;
      if (offS) {
        INFO_ESPNOWF("[BOND] peer offline — clearing stale session for re-establishment");
        sessionClear(offS);
      }
    }
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

  // 4. Check topology collection window
  checkTopologyCollectionWindow();
  
  // 6. Check broadcast tracker timeouts
  static uint32_t sLastBroadcastCheck = 0;
  if (now - sLastBroadcastCheck >= 500) {  // Check every 500ms
    sLastBroadcastCheck = now;
    broadcast_tracker_check_timeouts();
  }
  
  // 6b. Abandoned file-transfer cleanup is now driven by step 1d above
  // (fileSlotsTimeoutSweep) which handles all kFileSlots slots, not just
  // a single global pointer. This block is intentionally removed.
  
  // 7. Process deferred CMD (remote command received from another device)
  if (gEspNow->deferredCmdPending) {
    gEspNow->deferredCmdPending = false;
    v4_handle_cmd(gEspNow->deferredCmdSrcMac, gEspNow->deferredCmdDeviceName,
                  gEspNow->deferredCmdMsgId, gEspNow->deferredCmdPayload,
                  gEspNow->deferredCmdWasEncrypted);
  }
  
  // 7a. Saturation sampler — 1 Hz snapshot of derived link-pressure signals
  // (frames/s, queue depths, drops, ACK RTT). Cheap; only commits a sample
  // when ≥1s has elapsed since the last commit.
  espnowSaturationTick();

  // 7b. Drain stream queue (remote command output received via V3 STREAM frames)
  if (gEspNow->streamQueue) {
    int tail = gEspNow->streamQueueTail;
    int processed = 0;
    while (tail != gEspNow->streamQueueHead && processed < gEspNow->streamDrainMax) {
      auto& entry = gEspNow->streamQueue[tail];
      if (entry.used) {
        String devName = String(entry.deviceName);
        if (devName.length() == 0) devName = formatMacAddress(entry.srcMac);
        // reqId = the originating command's msgId so remote command output
        // correlates to its command (streaming preserved — this only tags it).
        // Legacy startstream sends msgId 0, so its telemetry stays "unsolicited".
        storeMessageInPeerHistory(entry.srcMac,
                                  devName.c_str(),
                                  entry.content,
                                  true,
                                  MSG_CMD_RESULT,   // remote-command stream output, not chat
                                  entry.cmdMsgId);
        BROADCAST_PRINTF("[STREAM:%s] %s", devName.c_str(), entry.content);
        entry.used = false;
      }
      tail = (tail + 1) & gEspNow->streamQueueMask;
      processed++;
    }
    gEspNow->streamQueueTail = tail;
  }
  
  // 8. Process deferred CMD_RESP (response to our remote command)
  if (gEspNow->deferredCmdRespPending) {
    gEspNow->deferredCmdRespPending = false;
    String deviceName = String(gEspNow->deferredCmdRespDeviceName);
    if (deviceName.length() == 0) deviceName = formatMacAddress(gEspNow->deferredCmdRespSrcMac);
    // Chunk the result: a command's output can exceed the 256 B record slot
    // (e.g. `files json` for a non-trivial dir). Storing it whole truncated it
    // mid-JSON; chunking lets the client reassemble by reqId.
    storeReceivedMessageChunked(gEspNow->deferredCmdRespSrcMac,
                                deviceName.c_str(),
                                gEspNow->deferredCmdRespResult,
                                true,
                                MSG_CMD_RESULT,   // remote-command result, not chat — preserve the CMD_RESP class
                                gEspNow->deferredCmdRespReqId);
    
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
  if (isBondMaster() && gEspNow->bondPeerOnline && gSettings.bondModeEnabled) {
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
    
    // Cooldown after retry exhaustion — don't re-request immediately.
    // FIXED (2026-05-21, all-encrypted bond): CAP_REQ/MANIFEST_REQ now ride a
    // SESSION_FRAME and need the session ACTIVE. Previously the master fired the
    // first CAP_REQ as soon as bondPeerOnline was set, but its own session could
    // still be ESTABLISHING — so the request was parked in the single-frame queue,
    // got overwritten/swept before the session opened, and burned all 3 retries.
    // Then this cooldown (formerly 15 s) delayed the next attempt, making the FIRST
    // bond sync take ~25 s instead of ~5 s (HW-observed). Two changes fix it:
    //   1) Gate the sync START on bondSessionActiveWith(peerMac) below, so the
    //      first request only fires once OUR session is genuinely ACTIVE (by which
    //      point the responding worker has been ACTIVE since slightly earlier).
    //   2) Shorten the cooldown to 3 s. The retry interval is already 3 s and the
    //      whole tick is gated on bondPeerOnline + an active session, so 3 s is
    //      ample to avoid hammering a genuinely stuck peer without the long stall.
    static const uint32_t BOND_SYNC_COOLDOWN_MS = 3000;
    bool inCooldown = (gEspNow->bondSyncLastAttemptMs > 0 &&
                       (now - gEspNow->bondSyncLastAttemptMs) < BOND_SYNC_COOLDOWN_MS);

    if (macOk && gEspNow->bondSyncInFlight == BOND_SYNC_NONE && !inCooldown &&
        bondSessionActiveWith(peerMac)) {
      // Decide what's missing and request it (priority order: CAP > MANIFEST > SETTINGS)
      bool haveCap = gEspNow->lastRemoteCapValid;
      bool haveManifest = gEspNow->bondManifestReceived;
      bool haveSettings = gEspNow->bondSettingsReceived;
      
      if (!haveCap) {
        // Need capabilities
        uint32_t reqId = generateMessageId();
        bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_BOND_CAP_REQ, ESPNOW_V4_FLAG_ACK_REQ, reqId, nullptr, 0);
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
          bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_MANIFEST_REQ, ESPNOW_V4_FLAG_ACK_REQ, msgId, nullptr, 0);
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
        (now - gEspNow->bondSyncLastAttemptMs >= BOND_SYNC_RETRY_MS) &&
        // Don't re-request while the response is already streaming in. The
        // manifest (and settings) arrive as a multi-second chunked file; firing
        // a fresh MANIFEST_REQ mid-transfer makes the peer regenerate and resend
        // the whole thing, doubling airtime and starving heartbeat ACKs. Push
        // the retry window forward and re-check next tick instead.
        !espnowInboundFileActiveFrom(peerMac)) {
      if (gEspNow->bondSyncRetryCount < 3) {
        gEspNow->bondSyncLastAttemptMs = now;
        gEspNow->bondSyncRetryCount++;
        if (gEspNow->bondSyncInFlight == BOND_SYNC_CAP) {
          bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_BOND_CAP_REQ, ESPNOW_V4_FLAG_ACK_REQ, generateMessageId(), nullptr, 0);
          BROADCAST_PRINTF("[BOND_SYNC] Retry CAP_REQ (%d/3)", (int)gEspNow->bondSyncRetryCount);
        } else if (gEspNow->bondSyncInFlight == BOND_SYNC_MANIFEST) {
          bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_MANIFEST_REQ, ESPNOW_V4_FLAG_ACK_REQ, generateMessageId(), nullptr, 0);
          BROADCAST_PRINTF("[BOND_SYNC] Retry MANIFEST_REQ (%d/3)", (int)gEspNow->bondSyncRetryCount);
        } else if (gEspNow->bondSyncInFlight == BOND_SYNC_SETTINGS) {
          requestBondSettings(peerMac);
          BROADCAST_PRINTF("[BOND_SYNC] Retry SETTINGS_REQ (%d/3)", (int)gEspNow->bondSyncRetryCount);
        }
      } else {
        // Max retries exhausted — reset in-flight but enforce cooldown before re-requesting.
        // Leave bondSyncLastAttemptMs set so the cooldown check below prevents instant retry.
        BROADCAST_PRINTF("[BOND_SYNC] %d exhausted %d retries, cooldown 3s before re-request",
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

  // POST-SYNC FALLBACK GUARD (both roles): if we've landed in isBondSynced()
  // == true without having fired the post-sync side effects yet, fire them.
  // Belt + suspenders for the cmd_bond_role direct call, which can fail if
  // the encrypted session wasn't yet ACTIVE at the moment of role swap. This
  // also generalizes — any future code path that lands in synced state via
  // a shortcut (peer reconnect with cached state, manual flag manipulation,
  // etc.) gets the STATUS_REQ kick for free. Idempotent via the
  // bondStatusReqSentOnce gate inside firePostSyncSideEffects.
  if (gSettings.bondModeEnabled && gEspNow->bondPeerOnline &&
      !gEspNow->bondStatusReqSentOnce && isBondSynced()) {
    uint8_t pMac[6];
    if (gSettings.bondPeerMac.length() > 0 &&
        parseMacAddress(gSettings.bondPeerMac, pMac) &&
        bondSessionActiveWith(pMac)) {
      firePostSyncSideEffects(pMac);
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
    bool sent = bondSendEncryptedAsync(gEspNow->bondPendingResponseMac, ESPNOW_V4_TYPE_BOND_CAP_RESP,
                  ESPNOW_V4_FLAG_ACK_REQ, respId,
                  (const uint8_t*)&cap, (uint16_t)sizeof(cap));
    gEspNow->bondCapSent = true;
    BROADCAST_PRINTF("[BOND] 9b: CAP_RESP sent=%d featureMask=0x%08lX", (int)sent, (unsigned long)cap.featureMask);
  }

  // 9d. Bond: peer requested our manifest — defer build+write+sendFileToMac to
  // cmd_exec. Gate on an encrypted session (event-driven, non-blocking): if it
  // isn't up yet, bondSendReadyOrDeferred() kicks the handshake and returns
  // false, the flag stays set, and we retry on a later tick once SESSION_ACTIVE.
  // On submit failure we re-set the flag so the next super-loop tick retries
  // instead of dropping the peer's request silently.
  if (gEspNow->bondNeedsManifestResponse &&
      bondSendReadyOrDeferred(gEspNow->bondPendingResponseMac)) {
    gEspNow->bondNeedsManifestResponse = false;
    BROADCAST_PRINTF("[BOND] 9d: bondNeedsManifestResponse consumed | role=%d dest=%s",
                     (int)gSettings.bondRole,
                     MAC_STR(gEspNow->bondPendingResponseMac));
    if (!submitBondFileSend(gEspNow->bondPendingResponseMac, BOND_FILE_KIND_MANIFEST)) {
      gEspNow->bondNeedsManifestResponse = true;  // retry next tick
      BROADCAST_PRINTF("[BOND] 9d: deferral failed — will retry next tick");
    }
  }

  // 9e. Bond: peer requested our settings — defer to cmd_exec, same shape as 9d.
  // The post-submit bookkeeping (bondSettingsSent + SYNC-COMPLETE log) stays
  // here because it reflects "we scheduled the response," matching the prior
  // inline semantics where the flag/log fired immediately after sendBondSettings
  // returned regardless of on-air success. Identity scope installs on cmd_exec
  // around sendBondSettings — same systemAuth coverage the inline version had.
  if (gEspNow->bondNeedsSettingsResponse &&
      bondSendReadyOrDeferred(gEspNow->bondPendingResponseMac)) {
    gEspNow->bondNeedsSettingsResponse = false;
    BROADCAST_PRINTF("[BOND] 9e: bondNeedsSettingsResponse consumed | role=%d dest=%s",
                     (int)gSettings.bondRole,
                     MAC_STR(gEspNow->bondPendingResponseMac));
    if (!submitBondFileSend(gEspNow->bondPendingResponseMac, BOND_FILE_KIND_SETTINGS)) {
      gEspNow->bondNeedsSettingsResponse = true;  // retry next tick
      BROADCAST_PRINTF("[BOND] 9e: deferral failed — will retry next tick");
    } else {
      gEspNow->bondSettingsSent = true;
      if (isBondWorker() && isBondSynced()) {
        BROADCAST_PRINTF("[BOND_SYNC] *** SYNC COMPLETE *** role=0 (worker)");
      }
    }
  }

  // 9e2. Bond: peer requested our settings schema — defer to cmd_exec, same
  // shape as 9d/9e. Receiver picks the file up in v4h_file_end → processBondSchema.
  if (gEspNow->bondNeedsSchemaResponse &&
      bondSendReadyOrDeferred(gEspNow->bondPendingResponseMac)) {
    gEspNow->bondNeedsSchemaResponse = false;
    BROADCAST_PRINTF("[BOND] 9e2: bondNeedsSchemaResponse consumed | role=%d dest=%s",
                     (int)gSettings.bondRole,
                     MAC_STR(gEspNow->bondPendingResponseMac));
    if (!submitBondFileSend(gEspNow->bondPendingResponseMac, BOND_FILE_KIND_SCHEMA)) {
      gEspNow->bondNeedsSchemaResponse = true;  // retry next tick
      BROADCAST_PRINTF("[BOND] 9e2: deferral failed — will retry next tick");
    }
  }

  // 9f. Bond: streaming setup — master pushes saved streaming prefs to worker after handshake
  if (gEspNow->bondNeedsStreamingSetup) {
    gEspNow->bondNeedsStreamingSetup = false;
    BROADCAST_PRINTF("[BOND] 9f: bondNeedsStreamingSetup consumed | role=%d",
                     (int)gSettings.bondRole);
    
    // Only master pushes streaming prefs to worker (after full sync)
    if (isBondMaster() && isBondSynced()) {
      extern bool sendBondStreamCtrl(RemoteSensorType sensorType, bool enable);
      struct { const char* name; bool enabled; RemoteSensorType type; } streams[] = {
        { "thermal",  gSettings.bondStreamThermal,  REMOTE_SENSOR_THERMAL },
        { "tof",      gSettings.bondStreamTof,      REMOTE_SENSOR_TOF },
        { "imu",      gSettings.bondStreamImu,      REMOTE_SENSOR_IMU },
        { "gps",      gSettings.bondStreamGps,      REMOTE_SENSOR_GPS },
        { "input",    gSettings.bondStreamInput,    REMOTE_SENSOR_INPUT },
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
      BROADCAST_PRINTF("[BOND] REJECTED %lu bond msg(s) from UNPAIRED %02X:%02X:%02X:%02X:%02X:%02X (type=%d, total=%lu) — run 'bondconnect' or 'espnowpair'!",
                       (unsigned long)newRejects,
                       gEspNow->bondUnpairedRejectMac[0], gEspNow->bondUnpairedRejectMac[1],
                       gEspNow->bondUnpairedRejectMac[2], gEspNow->bondUnpairedRejectMac[3],
                       gEspNow->bondUnpairedRejectMac[4], gEspNow->bondUnpairedRejectMac[5],
                       (int)gEspNow->bondUnpairedRejectType,
                       (unsigned long)gEspNow->bondUnpairedRejectCount);
      // Also surface in failed_login.log: an unpaired peer trying to drive the
      // bond channel is a failed attempt to gain command execution. Deferred +
      // throttled inside logBondAuthFailure (we are on espnow_task here).
      logBondAuthFailure(gEspNow->bondUnpairedRejectMac, "Bond traffic from unpaired peer");
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
    bondSendEncryptedAsync(gEspNow->bondPendingResponseMac, ESPNOW_V4_TYPE_BOND_STATUS_RESP,
                  0, respId, (const uint8_t*)&localStatus, sizeof(localStatus));
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
        bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_BOND_STATUS_RESP,
                      0, respId, (const uint8_t*)&localStatus, sizeof(localStatus));
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
        bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_BOND_STATUS_REQ, 0, reqId, nullptr, 0);
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
    processMetadata(gEspNow->deferredMetadataSrcMac, (const V4PayloadMetadata*)gEspNow->deferredMetadataPayload);
    DEBUG_ESPNOW_METADATAF("[METADATA] Task: processMetadata complete");
    
    // Log metadata to message history for web UI. PSRAM-backed scratch:
    // processMeshHeartbeats is the espnow_task super-loop body, so this
    // function runs only on that task — a single shared static buffer is
    // race-free. Saves 384 B of espnow_task stack on every metadata-drain
    // tick (helpful when running alongside an active SESSION_FRAME RX
    // unwrap that's already burning stack via plainBuf and v4_send_frame).
    const V4PayloadMetadata* meta = (const V4PayloadMetadata*)gEspNow->deferredMetadataPayload;
    EXT_RAM_BSS_ATTR static char metaMsg[384];
    snprintf(metaMsg, sizeof(metaMsg),
             "Metadata: name=%s friendly=%s room=%s zone=%s tags=%s stationary=%d",
             meta->deviceName, meta->friendlyName, meta->room, meta->zone,
             meta->tags, (int)meta->stationary);
    
    String devName = String(meta->deviceName);
    if (devName.length() == 0) devName = formatMacAddress(gEspNow->deferredMetadataSrcMac);
    storeMessageInPeerHistory(gEspNow->deferredMetadataSrcMac, devName.c_str(), 
                              metaMsg, false, MSG_SYSTEM_EVENT);  // peer-metadata snapshot, not chat
  }
  
  // 11. Drain text message queue
  {
    int tail = gEspNow->textQueueTail;
    int processed = 0;
    while (tail != gEspNow->textQueueHead && processed < 8) {
      auto& entry = gEspNow->textQueue[tail];
      if (entry.used) {
        String devName = String(entry.deviceName);
        if (devName.length() == 0) devName = formatMacAddress(entry.srcMac);
        // Store this piece as its own record. Group = msgId; piece = fragIndex+1
        // of fragCount. Single-frame text is just piece 1/1. The client groups
        // by reqId and stitches — the device never holds the whole message.
        bool stored = storeMessageInPeerHistory(entry.srcMac, devName.c_str(),
                                                entry.content, entry.encrypted,
                                                entry.msgType, entry.msgId,
                                                (uint8_t)(entry.fragIndex + 1), entry.fragCount);
        if (entry.fragCount > 1) {
          BROADCAST_PRINTF("[%s%s] (part %u/%u) %s", devName.c_str(),
                           entry.encrypted ? " [enc]" : "",
                           entry.fragIndex + 1, entry.fragCount, entry.content);
        } else {
          BROADCAST_PRINTF("[%s%s] %s", devName.c_str(),
                           entry.encrypted ? " [enc]" : "", entry.content);
        }
        // ESP-NOW App page push-kick: if the user is currently viewing the
        // inbox (merged or per-peer), enqueue a Redraw so the new entry
        // appears within one applier-tick. No-op when the page isn't
        // active or the user is on a non-message sub-mode; safe to call
        // from this task (the lens applier queue is thread-safe). When
        // BT/G2 are compiled out the header provides an inline no-op.
        if (stored) {
          g2ESPNowAppOnRxText(entry.srcMac);
        }
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
    TASK_PRIORITY_HIGH,
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

// =============================================================================
// ESP-NOW App ping — single-slot HEARTBEAT-with-ACK probe used by the on-glasses
// ESP-NOW App page (see G2_Page_ESPNow). One round-trip in flight at a time.
// =============================================================================
// The ACK RX path in onEspNowDataReceived (V3_TYPE_ACK branch, around line
// 2061) calls espnowAppPingNoteAck() unconditionally — that function early-
// outs if the ping slot is Idle, so the cost when nobody's pinging is one
// load + one compare.
//
// State machine: Idle → Pending (after Start) → Ok (rttMs set) | Timeout.
// "Timeout" is computed lazily inside espnowAppPingPoll() when the caller's
// timeoutMs has elapsed since startMs — no timer task.

struct EspNowAppPingSlot {
  uint8_t  peerMac[6];
  uint32_t msgId;
  uint32_t startMs;
  uint32_t rttMs;
  EspNowAppPingState state;
};

static EspNowAppPingSlot gEspNowAppPing = {
  /*peerMac*/ {0,0,0,0,0,0},
  /*msgId*/   0,
  /*startMs*/ 0,
  /*rttMs*/   0,
  /*state*/   EspNowAppPingState::Idle,
};

bool espnowAppPingStart(const uint8_t* mac) {
  if (!mac || !gEspNow || !gEspNow->initialized) return false;
  uint32_t msgId = generateMessageId();
  // Slot first — if the ACK lands in the gap between v4_send_frame returning
  // and the slot being populated, espnowAppPingNoteAck would see Pending=false
  // and drop the match.
  memcpy(gEspNowAppPing.peerMac, mac, 6);
  gEspNowAppPing.msgId   = msgId;
  gEspNowAppPing.startMs = (uint32_t)millis();
  gEspNowAppPing.rttMs   = 0;
  gEspNowAppPing.state   = EspNowAppPingState::Pending;

  // 2026-05-19: smart-send so the app-ping rides SESSION_FRAME when a session
  // is up. Zero-length payload is fine — v4_send_encrypted_or_queue and
  // v4_send_frame both accept (nullptr,0). Plaintext fallback for pre-KEY_EX
  // pings preserves the liveness-probe usefulness.
  bool ok = v4_send_payload_smart(mac, ESPNOW_V4_TYPE_HEARTBEAT, ESPNOW_V4_FLAG_ACK_REQ,
                                  msgId, nullptr, 0, /*ttl=*/1);
  if (!ok) {
    // Send refused (queue full / radio off / no peer). Don't leave the slot
    // Pending — that would let a stale ACK from an unrelated send win.
    gEspNowAppPing.state = EspNowAppPingState::Idle;
  }
  DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_APP_PING] start msgId=%lu peer=%02X:%02X:%02X:%02X:%02X:%02X ok=%d",
         (unsigned long)msgId, mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], (int)ok);
  return ok;
}

void espnowAppPingNoteAck(const uint8_t* src, uint32_t msgId) {
  if (gEspNowAppPing.state != EspNowAppPingState::Pending) return;
  if (gEspNowAppPing.msgId != msgId) return;
  if (memcmp(gEspNowAppPing.peerMac, src, 6) != 0) return;
  gEspNowAppPing.rttMs = (uint32_t)millis() - gEspNowAppPing.startMs;
  gEspNowAppPing.state = EspNowAppPingState::Ok;
  DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_APP_PING] ack matched: rtt=%lums",
         (unsigned long)gEspNowAppPing.rttMs);
}

EspNowAppPingState espnowAppPingPoll(uint32_t* outRttMs,
                                     uint8_t* outPeerMac,
                                     uint32_t timeoutMs) {
  if (outPeerMac) memcpy(outPeerMac, gEspNowAppPing.peerMac, 6);
  if (gEspNowAppPing.state == EspNowAppPingState::Pending) {
    if ((uint32_t)millis() - gEspNowAppPing.startMs > timeoutMs) {
      gEspNowAppPing.state = EspNowAppPingState::Timeout;
    }
  }
  if (outRttMs) {
    *outRttMs = (gEspNowAppPing.state == EspNowAppPingState::Ok)
                ? gEspNowAppPing.rttMs : 0;
  }
  return gEspNowAppPing.state;
}

void espnowAppPingClear() {
  gEspNowAppPing.state = EspNowAppPingState::Idle;
  gEspNowAppPing.msgId = 0;
  gEspNowAppPing.rttMs = 0;
}

// Last-failure reason from initEspNow(), readable by cmd_espnow_init so the
// CLI/UI caller sees the *actual* cause rather than a generic "failed"
// string. Static char buffer (not String) — keeps this off the heap and
// usable from any context. Cleared on each new init attempt.
static char gLastInitErrorReason[96] = {0};

// Helper: Initialize ESP-NOW subsystem (static - internal use only)
static bool initEspNow() {
  gLastInitErrorReason[0] = '\0';
  // Capture heap before initialization
  size_t heapBefore = ESP.getFreeHeap();
  
  // Peer slot count from settings (capped to compile-time ceiling). Computed into
  // a LOCAL and committed to gMeshPeerSlots ONLY after both arrays are allocated
  // below. Otherwise a CLI/BLE read command (e.g. `espnowdevices`) running before
  // init — or after a partial-alloc failure — would walk a null gMeshPeerMeta
  // (every loop is `for (i < gMeshPeerSlots)`), faulting on isActive at offset
  // 0xE0. gMeshPeerSlots stays 0 until the arrays actually exist.
  int slots = gSettings.meshPeerMax;
  if (slots < 1) slots = 1;
  if (slots > MESH_PEER_MAX) slots = MESH_PEER_MAX;

  // Allocate mesh peer arrays (dynamic based on meshPeerMax setting)
  if (!gMeshPeers) {
    size_t healthSize = sizeof(MeshPeerHealth) * slots;
    gMeshPeers = (MeshPeerHealth*)ps_alloc(healthSize, AllocPref::PreferPSRAM, "mesh.peers");
    if (gMeshPeers) {
      memset(gMeshPeers, 0, healthSize);
    } else {
      snprintf(gLastInitErrorReason, sizeof(gLastInitErrorReason),
               "out of PSRAM (peer health array)");
      broadcastOutput("[ESP-NOW] ERROR: Failed to allocate mesh peer health");
      return false;
    }
  }
  if (!gMeshPeerMeta) {
    size_t metaSize = sizeof(MeshPeerMeta) * slots;
    gMeshPeerMeta = (MeshPeerMeta*)ps_alloc(metaSize, AllocPref::PreferPSRAM, "mesh.meta");
    if (gMeshPeerMeta) {
      for (int i = 0; i < slots; i++) gMeshPeerMeta[i].clear();
    } else {
      snprintf(gLastInitErrorReason, sizeof(gLastInitErrorReason),
               "out of PSRAM (peer meta array)");
      broadcastOutput("[ESP-NOW] ERROR: Failed to allocate mesh peer meta");
      return false;
    }
  }
  // Both arrays exist — NOW it's safe to advertise the slot count to readers.
  gMeshPeerSlots = slots;

  // Allocate ESP-NOW state on first use
  // Initialize primary mesh metadata
  initPrimaryMeshFromLegacySettings();
  recomputeAllMeshFingerprints();

  if (!gEspNow) {
    gEspNow = (EspNowState*)ps_alloc(sizeof(EspNowState), AllocPref::PreferPSRAM, "espnow.state");
    if (!gEspNow) {
      snprintf(gLastInitErrorReason, sizeof(gLastInitErrorReason),
               "out of PSRAM (state structure)");
      broadcastOutput("[ESP-NOW] ERROR: Failed to allocate state structure");
      return false;
    }
    // EspNowState embeds C++ objects with non-trivial constructors — String
    // members at struct level (passphrase, deviceName, ...) and 5 Strings in
    // every devices[16] slot (name/friendlyName/room/zone/tags). ps_alloc() is
    // raw malloc and does NOT run those constructors; a zeroed String reads as
    // {isSSO=0, ptr.buff=NULL}, so String::c_str() returns NULL. Any field that
    // is read before being assigned (e.g. a freshly-paired peer whose
    // friendlyName/room/zone/tags metadata hasn't arrived yet) then feeds NULL
    // into printf("%s", ...) / ArduinoJson and crashes in strlen(NULL)
    // (LoadProhibited, EXCVADDR=0). Placement-new runs every constructor so all
    // Strings are valid empty SSO strings. Value-initialization also zero-inits
    // the POD members (replacing the old blanket memset) and honors any in-class
    // member initializers.
    memset(gEspNow, 0, sizeof(EspNowState));
    new (gEspNow) EspNowState();

    // Allocate small PSRAM buffers pulled out of the EspNowState struct
    gEspNow->listBuffer = (char*)ps_alloc(1024, AllocPref::PreferPSRAM, "espnow.listBuf");
    if (gEspNow->listBuffer) {
      memset(gEspNow->listBuffer, 0, 1024);
    } else {
      BROADCAST_PRINTF("[ESP-NOW] WARNING: Failed to allocate listBuffer");
    }
    gEspNow->deferredCmdRespResult = (char*)ps_alloc(6144, AllocPref::PreferPSRAM, "espnow.cmdResp");
    if (gEspNow->deferredCmdRespResult) {
      memset(gEspNow->deferredCmdRespResult, 0, 6144);
    } else {
      BROADCAST_PRINTF("[ESP-NOW] WARNING: Failed to allocate deferredCmdRespResult");
    }

    // STREAM ring for bonded/remote command output. A large burst (e.g. `help`,
    // ~24 frames in ~150ms) overflows a small ring and silently truncates. Use
    // 64 slots STRICTLY in PSRAM; if PSRAM is unavailable or the PSRAM alloc
    // fails, fall back to the historical 16-slot ring (drain cap 8) in internal
    // RAM so a PSRAM-less board pays no extra DRAM. (~295 B/slot.)
    gEspNow->streamQueue = nullptr;
    if (psramAvailableRuntime()) {
      size_t sqBytes = sizeof(EspNowState::StreamQueueEntry) * EspNowState::STREAM_QUEUE_SIZE_PSRAM;
      gEspNow->streamQueue = (EspNowState::StreamQueueEntry*)heap_caps_malloc(sqBytes, MALLOC_CAP_SPIRAM);
      if (gEspNow->streamQueue) {
        memset(gEspNow->streamQueue, 0, sqBytes);
        gEspNow->streamQueueCap = EspNowState::STREAM_QUEUE_SIZE_PSRAM;
        gEspNow->streamDrainMax = EspNowState::STREAM_QUEUE_SIZE_PSRAM;
      }
    }
    if (!gEspNow->streamQueue) {
      size_t sqBytes = sizeof(EspNowState::StreamQueueEntry) * EspNowState::STREAM_QUEUE_SIZE_FALLBACK;
      gEspNow->streamQueue = (EspNowState::StreamQueueEntry*)ps_alloc(sqBytes, AllocPref::PreferInternal, "espnow.streamQueue");
      if (gEspNow->streamQueue) memset(gEspNow->streamQueue, 0, sqBytes);
      gEspNow->streamQueueCap = EspNowState::STREAM_QUEUE_SIZE_FALLBACK;
      gEspNow->streamDrainMax = 8;
    }
    gEspNow->streamQueueMask = gEspNow->streamQueueCap - 1;
    if (!gEspNow->streamQueue) {
      // Both attempts failed — disable the ring (RX path + drain both no-op).
      BROADCAST_PRINTF("[ESP-NOW] WARNING: Failed to allocate streamQueue");
      gEspNow->streamQueueCap = 0;
      gEspNow->streamQueueMask = 0;
      gEspNow->streamDrainMax = 0;
    } else {
      DEBUG_ESPNOWF("[ESP-NOW] streamQueue: %d slots (%s), drainMax=%d",
                    gEspNow->streamQueueCap,
                    (gEspNow->streamQueueCap == EspNowState::STREAM_QUEUE_SIZE_PSRAM) ? "PSRAM" : "internal",
                    gEspNow->streamDrainMax);
    }

    // Allocate per-device message history with dynamic growth
    // Start with small initial capacity (5 slots) and grow as peers are discovered
    int initialCapacity = PEER_HISTORY_INITIAL_CAPACITY;
    if (initialCapacity > gMeshPeerSlots) {
      initialCapacity = gMeshPeerSlots;  // Cap at configured max
    }
    
    size_t histSize = sizeof(PeerMessageHistory) * initialCapacity;
    gEspNow->peerMessageHistories = (PeerMessageHistory*)ps_alloc(histSize, AllocPref::PreferPSRAM, "espnow.msgHist");
    if (gEspNow->peerMessageHistories) {
      // Placement-init each entry (has constructor)
      for (int i = 0; i < initialCapacity; i++) {
        new (&gEspNow->peerMessageHistories[i]) PeerMessageHistory();
      }
      gEspNow->peerHistoryCapacity = initialCapacity;
      gEspNow->peerHistoryCount = 0;
      BROADCAST_PRINTF("[ESP-NOW] Message history allocated: %d slots (~%u KB, grows to %d max)",
                       initialCapacity, (unsigned)(histSize / 1024), gMeshPeerSlots);
    } else {
      BROADCAST_PRINTF("[ESP-NOW] WARNING: Failed to allocate message history (%u bytes)", (unsigned)histSize);
      gEspNow->peerHistoryCapacity = 0;
      gEspNow->peerHistoryCount = 0;
      // Non-fatal — messaging will be limited but ESP-NOW still works
    }

    size_t totalBytes = sizeof(EspNowState) + histSize +
                        sizeof(MeshPeerHealth) * gMeshPeerSlots +
                        sizeof(MeshPeerMeta) * gMeshPeerSlots;
    BROADCAST_PRINTF("[ESP-NOW] Allocated state (%u bytes, %d peer slots)", (unsigned)totalBytes, gMeshPeerSlots);
  }

  // Allocate V3 reassembly buffers in PSRAM (saves ~6.4KB of internal BSS)
  if (!gV4Reasm) {
    size_t reasмSize = sizeof(V4ReasmEntry) * V4_REASM_MAX;
    gV4Reasm = (V4ReasmEntry*)ps_alloc(reasмSize, AllocPref::PreferPSRAM, "espnow.reasm");
    if (gV4Reasm) {
      memset(gV4Reasm, 0, reasмSize);
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
    snprintf(gLastInitErrorReason, sizeof(gLastInitErrorReason),
             "%s", setupError);
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

  // Initialize ESP-NOW. Translate the esp_err_t into a plain-English
  // explanation of what's actually wrong so the user doesn't have to look
  // up an error code — most of the time it's one of three things (WiFi
  // driver wasn't brought up, DRAM is too low, or the radio is in a bad
  // coex state) and we can say so directly.
  esp_err_t initErr = esp_now_init();
  if (initErr != ESP_OK) {
    size_t heapNow = ESP.getFreeHeap();
    wifi_mode_t wMode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&wMode);

    const char* reason = "unknown driver error";
    switch (initErr) {
      case ESP_ERR_WIFI_NOT_INIT:
        reason = "WiFi driver not started — radio must be on first";
        break;
      case ESP_ERR_ESPNOW_INTERNAL:
        reason = "radio busy or in bad state (BLE coex glitch — try again)";
        break;
      case ESP_ERR_NO_MEM:
        reason = "out of memory — free up DRAM and retry";
        break;
      case ESP_ERR_INVALID_ARG:
        reason = "invalid arg (firmware bug, please report)";
        break;
      default:
        reason = esp_err_to_name(initErr);  // fallback to IDF name string
        break;
    }
    snprintf(gLastInitErrorReason, sizeof(gLastInitErrorReason),
             "%s", reason);
    BROADCAST_PRINTF("[ESP-NOW] Cannot start: %s. (heap=%uB, was %uB, wifi_mode=%d)",
                     reason,
                     (unsigned)heapNow,
                     (unsigned)heapBefore,
                     (int)wMode);
    return false;
  }

  // Register callbacks (direct handler)
  esp_now_register_recv_cb(onEspNowDataReceived);
  esp_now_register_send_cb(onEspNowDataSent);

  gEspNow->initialized = true;

  // Spin up the single-sender TX dispatcher (idempotent). MUST live here, not
  // in the boot path, so it comes up for BOTH boot auto-init AND manual
  // `openespnow` — otherwise espnowtx::submit() finds a null queue, drops
  // every bonded sensor frame, and streaming silently breaks. See
  // System_ESPNow_Tx.h.
  if (!espnowtx::init()) {
    DEBUGF(DEBUG_ESPNOW_CORE, "[ESPNOW_TX] WARN: dispatcher init failed — sends fall back to producer tasks");
  }

  // Initialize the master-side remote-sensor cache (the cache of OTHER devices'
  // data we've received). Worker-side wire cache no longer exists — sensors are
  // read on demand by the broadcaster from their native caches.
  initRemoteSensorSystem();

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

  // Restore encryption passphrase from the primary mesh
  String bootPw = gSettings.meshes[0].passphrase;
  for (uint8_t i = 1; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].enabled && gSettings.meshes[i].isDefault &&
        gSettings.meshes[i].passphrase.length() > 0) {
      bootPw = gSettings.meshes[i].passphrase;
      break;
    }
  }
  if (bootPw.length() > 0) {
    gEspNow->passphrase = bootPw;
    deriveKeyFromPassphrase(bootPw, gEspNow->derivedKey);
    DEBUGF(DEBUG_ESPNOW_STREAM, "[ESP-NOW] Restored encryption passphrase from settings");
  } else {
    BROADCAST_PRINTF("[ESP-NOW] WARNING: No passphrase configured — encrypted "
                     "frames from peers will fail to decrypt. "
                     "Run 'espnowsetpassphrase primary <pw>'.");
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
  meshSendBootToPeers(bootMsg);
  BROADCAST_PRINTF("[ESP-NOW] Boot notification sent (counter=%lu)", (unsigned long)gBootCounter);

  notifyEspNowStarted(true);
  return true;
}

// ESP-NOW init command
// Deferred ESP-NOW init for web callers. initEspNow() is heavy (~360 KB alloc +
// task spin-up) and perturbs the WiFi radio — and a web "Initialize" request
// rides that same WiFi link. Running it synchronously while the browser's fetch
// is held open races the HTTPS connection against the radio reconfiguration,
// which surfaces intermittently as "TypeError: Load failed". So for SOURCE_WEB
// we ACK immediately and run the real init here, a beat later, on cmd_exec_task.
static void deferredEspNowInitFn(void* /*arg*/) {
  // Re-check: a second queued request (double-click) must not re-init.
  if (gEspNow && gEspNow->initialized) return;
  // The one place a short delay legitimately helps: AFTER the ACK's HTTP 200 has
  // flushed, BEFORE the radio-perturbing work begins.
  vTaskDelay(pdMS_TO_TICKS(300));
  if (gEspNow && gEspNow->initialized) return;
  (void)initEspNow();  // outcome surfaced via broadcastOutput + page status poll
}

const char* cmd_espnow_init(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (gEspNow && gEspNow->initialized) {
    return "ESP-NOW already initialized";
  }

  // Check memory before initializing ESP-NOW (task stack + state struct)
  if (!checkMemoryAvailable("espnow", nullptr)) {
    return "Error: Insufficient memory for ESP-NOW (need ~40KB DRAM + ~320KB PSRAM)";
  }

  // Web callers: ACK now, init async (see deferredEspNowInitFn) so the HTTPS
  // response is sent before the radio reconfigures. The memory check above
  // already ran synchronously, so an immediate OOM is still reported here.
  // Other transports (serial/OLED/BLE) keep synchronous init — their reply does
  // not ride the WiFi STA link, and callers like OLED expect ESP-NOW ready on
  // return.
  if (currentAuthContext().transport == SOURCE_WEB) {
    if (submitDeferredToCmdExec(deferredEspNowInitFn, nullptr)) {
      return "ESP-NOW initializing — poll 'espnowstatus' for completion";
    }
    // Couldn't queue the deferred work — fall through to synchronous init.
  }

  if (initEspNow()) {
    return "ESP-NOW initialized successfully";
  }
  // Surface the actual reason captured by initEspNow() instead of a generic
  // "failed" string. cmd_exec returns this via the result field; the lens
  // ESP-NOW App page logs it in onMainRedrawDone so the user can see what
  // went wrong without digging through serial broadcasts.
  static char fullMsg[128];
  if (gLastInitErrorReason[0]) {
    snprintf(fullMsg, sizeof(fullMsg), "Error: Cannot start ESP-NOW: %s",
             gLastInitErrorReason);
  } else {
    snprintf(fullMsg, sizeof(fullMsg),
             "Error: Cannot start ESP-NOW (no reason captured)");
  }
  return fullMsg;
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

  // 3. Cleanup active file transfers (Phase 4 multi-slot: walk + release all
  // slots; equivalent to the old single-slot delete).
  {
    uint8_t active = fileSlotsActiveCount();
    if (active > 0) {
      for (uint8_t i = 0; i < fileSlotsSlotCount(); i++) {
        FileTransferSlotInfo info;
        if (!fileSlotsSnapshot(i, &info)) continue;
        // Re-find the slot by (mac, msgId) and release. The handle-by-index
        // option would be cleaner but the public API is intentionally
        // opaque; the find-by-msg lookup is fine for shutdown cost.
        FileTransferSlot* slot = fileSlotsFindByMsg(info.peerMac, info.msgId);
        if (slot) fileSlotsRelease(slot);
      }
      char msg[80];
      snprintf(msg, sizeof(msg), "[ESP-NOW] %u active file transfer%s cleaned up",
               (unsigned)active, active == 1 ? "" : "s");
      broadcastOutput(msg);
    }
  }

  // 4. Clear retry queue entries
  {
    MeshRetryGuard guard("stopESPNow");
    if (guard.held) {
      memset(gMeshRetryQueue, 0, sizeof(gMeshRetryQueue));
    }
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
const char* cmd_espnow_deinit(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow || !gEspNow->initialized) {
    return "Error: ESP-NOW is not initialized";
  }

  if (deinitEspNow()) {
    return "ESP-NOW deinitialized successfully";
  } else {
    return "Error: Failed to deinitialize ESP-NOW";
  }
}

// ESP-NOW status command
const char* cmd_espnow_status(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // Read-only status: report "idle" instead of erroring when not initialized
  // (dashboard polls this; classifier sees no "Error:" prefix -> logs OK).
  if (argWantsJson(argsInput)) {
    if (!ensureDebugBuffer()) return "{\"schema\":1,\"ok\":false,\"error\":\"buffer\"}";
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    if (!gEspNow) { doc["ok"] = false; doc["error"] = "ESP-NOW not initialized"; }
    else {
      doc["initialized"] = gEspNow->initialized;
      doc["channel"]     = gEspNow->channel;
      if (gEspNow->initialized) {
        uint8_t mac[6]; WiFi.macAddress(mac);
        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        doc["mac"]           = String(macStr);
        doc["pairedDevices"] = (gEspNow->deviceCount > 0) ? (gEspNow->deviceCount - 1) : 0;
      }
    }
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }
  if (!gEspNow) return "Error: ESP-NOW not initialized (run 'openespnow' first)";
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
const char* cmd_espnow_stats(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // Read-only status: see comment in cmd_espnow_status above.
  if (argWantsJson(argsInput)) {
    if (!gEspNow) return "{\"schema\":1,\"ok\":false,\"error\":\"ESP-NOW not initialized\"}";
    if (!ensureDebugBuffer()) return "{\"schema\":1,\"ok\":false,\"error\":\"buffer\"}";
    PSRAM_JSON_DOC(doc);
    doc["schema"]           = 1;
    doc["messagesSent"]     = (unsigned long)gEspNow->routerMetrics.messagesSent;
    doc["messagesReceived"] = (unsigned long)gEspNow->routerMetrics.messagesReceived;
    doc["messagesFailed"]   = (unsigned long)gEspNow->routerMetrics.messagesFailed;
    doc["streamSent"]       = (unsigned long)gEspNow->streamSentCount;
    doc["streamReceived"]   = (unsigned long)gEspNow->streamReceivedCount;
    doc["streamDropped"]    = (unsigned long)gEspNow->streamDroppedCount;
    if (meshEnabled()) {
      doc["heartbeatsSent"]     = (unsigned long)gEspNow->heartbeatsSent;
      doc["heartbeatsReceived"] = (unsigned long)gEspNow->heartbeatsReceived;
    }
    doc["filesSent"]     = (unsigned long)gEspNow->fileTransfersSent;
    doc["filesReceived"] = (unsigned long)gEspNow->fileTransfersReceived;
    doc["uptimeSec"]     = (unsigned long)(gEspNow->lastResetTime > 0
                            ? (millis() - gEspNow->lastResetTime) / 1000 : millis() / 1000);
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }
  if (!gEspNow) return "Error: ESP-NOW not initialized (run 'openespnow' first)";

  // Output each line separately to avoid DEBUG_MSG_SIZE (256 byte) truncation.
  // Audit (2026-06): the orphan receiveErrors / meshForwards counters (displayed
  // but never incremented) were removed from EspNowState. If those metrics are
  // wanted again, add the fields + wire increments at the canonical paths first.
  BROADCAST_PRINTF(
    "ESP-NOW Statistics:\n"
    "  Messages Sent: %lu\n"
    "  Messages Received: %lu\n"
    "  Send Failures: %lu\n"
    "  Stream Sent: %lu\n"
    "  Stream Received: %lu\n"
    "  Stream Dropped: %lu",
    (unsigned long)gEspNow->routerMetrics.messagesSent,
    (unsigned long)gEspNow->routerMetrics.messagesReceived,
    (unsigned long)gEspNow->routerMetrics.messagesFailed,
    (unsigned long)gEspNow->streamSentCount,
    (unsigned long)gEspNow->streamReceivedCount,
    (unsigned long)gEspNow->streamDroppedCount);

  if (meshEnabled()) {
    BROADCAST_PRINTF("  Heartbeats Sent: %lu\n  Heartbeats Received: %lu",
                     (unsigned long)gEspNow->heartbeatsSent,
                     (unsigned long)gEspNow->heartbeatsReceived);
  }
  
  BROADCAST_PRINTF("  Files Sent: %lu\n  Files Received: %lu",
                   (unsigned long)gEspNow->fileTransfersSent,
                   (unsigned long)gEspNow->fileTransfersReceived);
  
  if (gEspNow->lastResetTime > 0) {
    unsigned long uptime = (millis() - gEspNow->lastResetTime) / 1000;
    BROADCAST_PRINTF("  Uptime: %lus", uptime);
  } else {
    BROADCAST_PRINTF("  Uptime: %lus (since boot)", millis() / 1000);
  }
  
  return "OK";
}

// ESP-NOW broadcast tracking statistics command
const char* cmd_espnow_broadcaststats(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // Read-only status: see comment in cmd_espnow_status above.
  if (!gEspNow) return "Error: ESP-NOW not initialized (run 'openespnow' first)";
  
  BROADCAST_PRINTF(
    "Broadcast ACK Tracking Statistics:\n"
    "  Broadcasts Tracked: %lu\n"
    "  Broadcasts Completed (100%%): %lu\n"
    "  Broadcasts Timed Out: %lu",
    (unsigned long)gBroadcastsTracked,
    (unsigned long)gBroadcastsCompleted,
    (unsigned long)gBroadcastsTimedOut);
  
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

// ESP-NOW link saturation command — see System_ESPNow_Saturation.h.
// Prints a rolling-window report of derived link-pressure signals (frames/sec,
// stream-queue depth, dropped-frame deltas, pending-frame backlog, ACK RTT)
// built on top of the existing cumulative counters. Intended for stress-test
// observability — e.g. enable multiple sensor streams and watch the gauges
// move. Honest about ceilings: queue depths are real percentages; frames/sec
// is an absolute rate (ESP-NOW has no fixed bandwidth ceiling to compare to).
const char* cmd_espnow_saturation(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized (run 'openespnow' first)";
  espnowSaturationReport();
  return "OK";
}

const char* cmd_espnow_saturationreset(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  espnowSaturationReset();
  return "Saturation window cleared — peaks/avgs will reflect post-reset traffic only";
}

// ESP-NOW router statistics command
const char* cmd_espnow_routerstats(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  // Read-only status: see comment in cmd_espnow_status above.
  if (argWantsJson(argsInput)) {
    if (!gEspNow) return "{\"schema\":1,\"ok\":false,\"error\":\"ESP-NOW not initialized\"}";
    snprintf(getDebugBuffer(), 1024,
      "{\"schema\":1,\"messagesSent\":%lu,\"messagesReceived\":%lu,\"messagesFailed\":%lu,"
      "\"v4FragTx\":%lu,\"v4FragRx\":%lu,\"reassembled\":%lu,\"reassemblyGc\":%lu,"
      "\"reassemblyTimeouts\":%lu,\"nextMessageId\":%lu}",
      (unsigned long)gEspNow->routerMetrics.messagesSent,
      (unsigned long)gEspNow->routerMetrics.messagesReceived,
      (unsigned long)gEspNow->routerMetrics.messagesFailed,
      (unsigned long)gEspNow->routerMetrics.v4FragTx,
      (unsigned long)gEspNow->routerMetrics.v4FragRx,
      (unsigned long)gEspNow->routerMetrics.v4FragRxCompleted,
      (unsigned long)gEspNow->routerMetrics.v4FragRxGc,
      (unsigned long)gEspNow->routerMetrics.chunksTimedOut,
      (unsigned long)gEspNow->nextMessageId);
    return getDebugBuffer();
  }
  if (!gEspNow) return "Error: ESP-NOW not initialized (run 'openespnow' first)";

  // Audit (2026-05) — this command USED to print ~18 lines of counters that
  // had no live increment site anywhere in the codebase (the V3 "Routing /
  // Queue/Retry / Chunking / Performance" sections all read fields whose bump
  // sites were removed in past V3→V4 / streaming refactors). They lied at 0
  // forever. Pruned to ONLY counters that are actually live today (router
  // top-line plus the V4 fragmentation family). If a removed section is wanted
  // back, wire the increment at its canonical path first.
  BROADCAST_PRINTF(
    "=== ESP-NOW Router Statistics ===\n"
    "Messages Sent: %lu\n"
    "Messages Received: %lu\n"
    "Messages Failed: %lu",
    (unsigned long)gEspNow->routerMetrics.messagesSent,
    (unsigned long)gEspNow->routerMetrics.messagesReceived,
    (unsigned long)gEspNow->routerMetrics.messagesFailed);

  BROADCAST_PRINTF(
    "\nV4 Fragmentation:\n"
    "  Fragments TX:        %lu\n"
    "  Fragments RX:        %lu\n"
    "  Reassembled:         %lu\n"
    "  Reassembly GC:       %lu\n"
    "  Reassembly Timeouts: %lu",
    (unsigned long)gEspNow->routerMetrics.v4FragTx,
    (unsigned long)gEspNow->routerMetrics.v4FragRx,
    (unsigned long)gEspNow->routerMetrics.v4FragRxCompleted,
    (unsigned long)gEspNow->routerMetrics.v4FragRxGc,
    (unsigned long)gEspNow->routerMetrics.chunksTimedOut);

  BROADCAST_PRINTF("\nMessage IDs:\n  Next Message ID: %lu", (unsigned long)gEspNow->nextMessageId);

  return "OK";
}

// ESP-NOW reset statistics command
const char* cmd_espnow_resetstats(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  
  gEspNow->heartbeatsSent = 0;
  gEspNow->heartbeatsReceived = 0;
  gEspNow->streamSentCount = 0;
  gEspNow->streamReceivedCount = 0;
  gEspNow->streamDroppedCount = 0;
  gEspNow->fileTransfersSent = 0;
  gEspNow->fileTransfersReceived = 0;
  gEspNow->lastResetTime = millis();
  
  gEspNow->routerMetrics = RouterMetrics();

  return "ESP-NOW statistics reset (including router metrics)";
}

// ============================================================================
// Phase 3.0 — long-term identity CLI
// ============================================================================
// `espnowidentity` is the read-only inspection command — every operator should
// know how to display the device's public key for OOB pairing verification
// once 3.3 (KEY_EX) lands. `espnowregenidentity` is destructive; the explicit
// flag is required so it can't be triggered by autocomplete or a typo. The
// flag also reads as forward-looking: from 3.2 onward, regenerating identity
// invalidates every paired peer's trust record, hence "wipe all bonds".

const char* cmd_espnow_identity(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  const EspNowIdentity& id = espnowIdentityGet();

  if (argWantsJson(argsInput)) {
    if (!id.valid) return "{\"schema\":1,\"valid\":false}";
    uint8_t jm[6]; WiFi.macAddress(jm);
    char jpub[65]; espnowIdentityFormatPubHex(id.pub, jpub, sizeof(jpub));
    snprintf(getDebugBuffer(), 1024,
      "{\"schema\":1,\"valid\":true,\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
      "\"pub\":\"%s\",\"createdAtSec\":%u,\"regenCount\":%u}",
      jm[0],jm[1],jm[2],jm[3],jm[4],jm[5], jpub,
      (unsigned)id.createdAtSec, (unsigned)id.regenCount);
    return getDebugBuffer();
  }

  if (!id.valid) {
    return "Error: ESP-NOW identity not initialized (crypto/identity boot path failed)";
  }

  uint8_t mac[6];
  WiFi.macAddress(mac);

  char pubHex[65];
  espnowIdentityFormatPubHex(id.pub, pubHex, sizeof(pubHex));

  char* p = getDebugBuffer();
  snprintf(p, 1024,
           "ESP-NOW Long-Term Identity:\n"
           "  MAC:          %02X:%02X:%02X:%02X:%02X:%02X\n"
           "  Ed25519 pub:  %s\n"
           "  createdAtSec: %u\n"
           "  regenCount:   %u",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           pubHex,
           (unsigned)id.createdAtSec,
           (unsigned)id.regenCount);
  return getDebugBuffer();
}

// Phase 3.3: initiate KEY_EX handshake with a paired-or-unpaired peer MAC.
// Additive command — does not interact with the existing espnowpair /
// espnowpairsecure LMK flows. Once the REPLY + CONFIRM round-trip completes,
// /system/espnow/peers/<mac>/identity.json appears on both ends. Subsequent
// phases (3.4 SESSION_OPEN, 3.5 SESSION_FRAME) consume that identity.
const char* cmd_espnow_keyex(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow || !gEspNow->initialized) {
    return "Error: ESP-NOW not initialized. Run 'openespnow' first.";
  }
  CommandArgs a(argsInput);
  if (a.count() < 1) {
    return "Error: invalid arguments — Usage: espnowkeyex <name_or_mac> [<mesh>]\n"
           "  <name_or_mac>  paired device name OR target peer MAC (AA:BB:CC:DD:EE:FF)\n"
           "  <mesh>         mesh label (defaults to current default mesh)";
  }
  String macStr = a.arg(0);
  String meshLabel = (a.count() >= 2) ? a.arg(1) : String("");

  uint8_t mac[6];
  // Permissive: accept a paired device name OR a raw MAC. Raw-MAC path lets
  // users run KEY_EX with brand-new peers that aren't in the legacy paired
  // device registry yet (KEY_EX is typically the *first* contact).
  if (!resolveDeviceNameOrMac(macStr, mac) && !parseMacAddress(macStr, mac)) {
    return "Error: not a paired device name and not a valid MAC.";
  }

  bool ok = espnowKeyExInitiate(mac, meshLabel.length() > 0 ? meshLabel.c_str() : nullptr);
  if (!ok) {
    return "Error: KEY_EX initiate failed (see [ERROR][ESP-NOW] log).";
  }

  cliHint("the handshake completes asynchronously - check progress with 'espnowsessions'");
  if (!ensureDebugBuffer()) return "KEY_EX_HELLO sent. Watch logs for REPLY/CONFIRM.";
  snprintf(getDebugBuffer(), 1024,
           "KEY_EX_HELLO sent to %02X:%02X:%02X:%02X:%02X:%02X (mesh '%s').\n"
           "Handshake completes asynchronously — watch for "
           "'KEY_EX with ... complete' log line. Peer identity will appear at "
           "/system/espnow/peers/%02X%02X%02X%02X%02X%02X/identity.json on success.",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           meshLabel.length() > 0 ? meshLabel.c_str() : "default",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return getDebugBuffer();
}

// Phase 3.4: initiate a SESSION_OPEN handshake. Requires the peer to have a
// prior KEY_EX identity record on disk (else there's no Ed25519 pubkey to
// verify the responder's CONFIRM signature against). Result lands in the
// in-RAM gSessions table; survives until reboot. Phase 3.5 will start
// requiring an active session for app-level unicast traffic.
const char* cmd_espnow_sessionopen(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow || !gEspNow->initialized) {
    return "Error: ESP-NOW not initialized. Run 'openespnow' first.";
  }
  CommandArgs a(argsInput);
  if (a.count() < 1) {
    return "Error: invalid arguments — Usage: espnowsessionopen <name_or_mac> [<mesh>]\n"
           "  Requires prior 'espnowkeyex' completion with the same peer.";
  }
  uint8_t mac[6];
  if (!resolveDeviceNameOrMac(a.arg(0), mac) && !parseMacAddress(a.arg(0), mac)) {
    return "Error: not a paired device name and not a valid MAC.";
  }
  String meshLabel = (a.count() >= 2) ? a.arg(1) : String("");
  bool ok = espnowSessionOpenInitiate(mac, meshLabel.length() > 0 ? meshLabel.c_str() : nullptr);
  if (!ok) {
    return "Error: SESSION_OPEN initiate failed (see [ERROR][ESP-NOW] log).";
  }
  if (!ensureDebugBuffer()) return "SESSION_OPEN sent.";
  snprintf(getDebugBuffer(), 1024,
           "SESSION_OPEN sent to %02X:%02X:%02X:%02X:%02X:%02X. Handshake completes "
           "asynchronously — watch for 'SESSION established (initiator)' log line. "
           "Run 'espnowsessions' to inspect.",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return getDebugBuffer();
}

// Reachability probe via KEY_EX. Kicks a single KEY_EX_HELLO and polls the
// in-flight slot until it resolves (REPLY arrived → identity present) or the
// caller's timeout expires (no response). Information-rich vs a plaintext PING:
// success tells you the peer is (1) alive, (2) on a mesh you share, and
// (3) running compatible firmware — all in one shot. KEY_EX is HMAC-authed by
// the mesh bootstrap key, so off-mesh sniffers learn nothing useful.
//
// Outcomes:
//   "alive on mesh '<label>' (<rtt>ms)"      — REPLY received within timeout
//   "no response within <N>ms"               — silent timeout (out of range,
//                                              powered off, OR different mesh —
//                                              indistinguishable from sender's
//                                              side, since wrong-mesh peers
//                                              silently drop our HELLO at the
//                                              HMAC check)
//   "rejected: <reason>"                     — in-flight cleared without
//                                              creating identity (e.g., peer
//                                              sent CONFIRM with status=2)
const char* cmd_espnow_probe(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow || !gEspNow->initialized) {
    return "Error: ESP-NOW not initialized. Run 'openespnow' first.";
  }
  CommandArgs a(argsInput);
  if (a.count() < 1) {
    return "Error: invalid arguments — Usage: espnowprobe <name_or_mac> [<timeoutMs (50-5000, default 500)>] [<mesh>]\n"
           "  Probes a peer via a single KEY_EX handshake. Reports whether the\n"
           "  peer is alive, on a mesh we share, and running compatible firmware.\n"
           "  No plaintext on the wire — mesh-gated by construction.";
  }

  uint8_t mac[6];
  if (!resolveDeviceNameOrMac(a.arg(0), mac) && !parseMacAddress(a.arg(0), mac)) {
    return "Error: not a paired device name and not a valid MAC.";
  }

  uint32_t timeoutMs = 500;
  if (a.count() >= 2) {
    long parsed = a.arg(1).toInt();
    if (parsed >= 50 && parsed <= 5000) {
      timeoutMs = (uint32_t)parsed;
    } else {
      return "Error: timeoutMs out of range (50-5000).";
    }
  }
  String meshLabel = (a.count() >= 3) ? a.arg(2) : String("");

  // Snapshot pre-probe state so we can distinguish "newly resolved" from
  // "was already paired before this probe."
  const PeerIdentity* preProbe = peerIdentityFindByMac(mac);
  bool hadIdentityBefore = (preProbe != nullptr);

  uint32_t kickMs = (uint32_t)millis();
  bool ok = espnowKeyExInitiate(mac, meshLabel.length() > 0 ? meshLabel.c_str() : nullptr);
  if (!ok) {
    return "Error: KEY_EX initiate failed (no enabled mesh? see [ERROR][ESP-NOW] log).";
  }

  // Poll until the in-flight slot clears (REPLY landed OR rejected) or timeout.
  // 10ms tick is fine — KEY_EX RTT in a healthy mesh is typically 50-200ms.
  uint32_t elapsedMs = 0;
  while (elapsedMs < timeoutMs && keyExIsInFlight(mac)) {
    vTaskDelay(pdMS_TO_TICKS(10));
    elapsedMs = (uint32_t)millis() - kickMs;
  }

  bool stillInFlight = keyExIsInFlight(mac);
  const PeerIdentity* postProbe = peerIdentityFindByMac(mac);

  if (!ensureDebugBuffer()) {
    if (stillInFlight) return "No response (timeout)";
    if (postProbe) return "Peer is alive";
    return "Peer rejected the probe";
  }
  char* buf = getDebugBuffer();

  if (stillInFlight) {
    snprintf(buf, 1024,
             "no response within %ums from %02X:%02X:%02X:%02X:%02X:%02X — "
             "peer offline, out of range, or on a different mesh "
             "(indistinguishable from this side). KEY_EX will keep retrying in "
             "background; run 'espnowprobe' again later or check the log.",
             (unsigned)timeoutMs,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return buf;
  }

  if (postProbe) {
    // Look up the mesh label from the resolved identity.
    const char* mlabel = "?";
    if (postProbe->meshId < Settings::N_MESHES) {
      mlabel = gSettings.meshes[postProbe->meshId].label.c_str();
      if (!mlabel || !mlabel[0]) mlabel = "(unnamed)";
    }
    snprintf(buf, 1024,
             "alive on mesh '%s' (%ums RTT) — %02X:%02X:%02X:%02X:%02X:%02X "
             "[%s]. Compatible firmware confirmed (KEY_EX REPLY parsed cleanly).",
             mlabel, (unsigned)elapsedMs,
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
             hadIdentityBefore ? "identity refreshed" : "new identity persisted");
    return buf;
  }

  // In-flight cleared but no identity → CONFIRM came back with status != 0
  // (e.g., pubkey conflict on responder side). Caller likely needs espnowforget
  // or espnowpairsecure to break the deadlock.
  snprintf(buf, 1024,
           "rejected by %02X:%02X:%02X:%02X:%02X:%02X — peer refused our pubkey "
           "(typically a stale identity from a prior pairing). Try 'espnowforget' "
           "on both sides + re-pair.",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return buf;
}

// Phase 3.6: force an immediate SESSION_REKEY for a peer. Useful for manual
// testing without waiting for the periodic threshold trigger. Requires an
// ACTIVE session.
const char* cmd_espnow_rekey(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow || !gEspNow->initialized) {
    return "Error: ESP-NOW not initialized. Run 'openespnow' first.";
  }
  CommandArgs a(argsInput);
  if (a.count() < 1) {
    return "Error: invalid arguments — Usage: espnowrekey <name_or_mac>\n"
           "  Forces immediate SESSION_REKEY; requires ACTIVE session.";
  }
  uint8_t mac[6];
  // Permissive: try a paired device name first (covers KEY_EX-paired peers
  // that are ALSO in the legacy device registry), fall back to raw MAC so
  // peers that completed KEY_EX without being registered still resolve.
  if (!resolveDeviceNameOrMac(a.arg(0), mac) && !parseMacAddress(a.arg(0), mac)) {
    return "Error: not a paired device name and not a valid MAC.";
  }
  bool ok = espnowRekeyInitiate(mac);
  if (!ok) {
    return "Error: REKEY initiate failed (no session, or just rekeyed recently — see log).";
  }
  cliHint("the rekey completes asynchronously - confirm the new keys with 'espnowsessions'");
  if (!ensureDebugBuffer()) return "REKEY sent.";
  snprintf(getDebugBuffer(), 1024,
           "REKEY sent to %02X:%02X:%02X:%02X:%02X:%02X. Symmetric exchange completes "
           "asynchronously — watch for 'SESSION rekeyed' log line.",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return getDebugBuffer();
}

// Phase 5 — list every paired peer and the subscription bitmap they've told
// us they want. Values reflect *what events this peer wants from us*, not
// what we want from them. Read-only.
const char* cmd_espnow_subs(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    JsonArray arr = doc["peers"].to<JsonArray>();
    uint8_t slots = peerIdentitySlotCount();
    for (uint8_t i = 0; i < slots; i++) {
      const PeerIdentity* pid = peerIdentityAt(i);
      if (!pid) continue;
      JsonObject o = arr.add<JsonObject>();
      o["mac"]    = String(MAC_STR(pid->mac));
      o["meshId"] = (unsigned)pid->meshId;
      o["subs"]   = (unsigned long)pid->subscribedEvents;  // bitmask; see help for bit meanings
    }
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

  char* p   = getDebugBuffer();
  size_t cap = 1024;
  int written = snprintf(p, cap,
                         "Peer Subscriptions (bitmap = what each peer wants FROM US):\n"
                         "  Each peer's row shows the mask THEY asked us to send THEM.\n"
                         "  Default 0xFFFFFFFF = all events. Narrowed via inbound SUBSCRIBE_UPDATE.\n"
                         "  To change a peer's row, run 'espnowrequestevents' on that peer (not here).\n"
                         "  bits: HB=0x01 SENSOR=0x02 TOPO=0x04 BOND_HB=0x08 WORKER=0x10 META=0x20 TIME=0x40\n");
  if (written < 0) return p;
  uint8_t slots = peerIdentitySlotCount();
  bool any = false;
  for (uint8_t i = 0; i < slots; i++) {
    const PeerIdentity* pid = peerIdentityAt(i);
    if (!pid) continue;
    any = true;
    int n = snprintf(p + written, cap - written,
                     "  %02X:%02X:%02X:%02X:%02X:%02X meshId=%u subs=0x%08lX\n",
                     pid->mac[0], pid->mac[1], pid->mac[2],
                     pid->mac[3], pid->mac[4], pid->mac[5],
                     (unsigned)pid->meshId,
                     (unsigned long)pid->subscribedEvents);
    if (n < 0 || (size_t)n >= cap - written) break;
    written += n;
  }
  if (!any) snprintf(p + written, cap - written, "  (no paired peers)\n");
  return p;
}

// Phase 5 — request that a peer send us only events matching <mask>. Sends
// a SUBSCRIBE_UPDATE through the encrypted dispatcher (session if available,
// plaintext otherwise). Bitmap is parsed as a 32-bit value (decimal or 0x-hex).
//
// Asymmetric semantic: this command mutates the PEER's outbound state, not
// ours. The skip count appears in the peer's [V4_BROADCAST_GATED] logs, not
// in ours. Our local PeerIdentity for that peer is unchanged; running
// `espnowsubs` here will not show the new mask — that's correct.
const char* cmd_espnow_requestevents(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow || !gEspNow->initialized) {
    return "Error: ESP-NOW not initialized. Run 'openespnow' first.";
  }
  CommandArgs a(argsInput);
  if (a.count() < 2) {
    return "Error: invalid arguments — Usage: espnowrequestevents <name_or_mac> <bitmask>\n"
           "  Request that <peer> send US only events matching <bitmask>.\n"
           "  Direction: this command updates state ON THE PEER, not locally.\n"
           "  bits: HB=0x01 SENSOR=0x02 TOPO=0x04 BOND_HB=0x08 WORKER=0x10 META=0x20 TIME=0x40\n"
           "  Examples:\n"
           "    espnowrequestevents deviceA 0x01       # only heartbeats from deviceA\n"
           "    espnowrequestevents deviceA 0xFFFFFFFF # everything (default)\n"
           "    espnowrequestevents deviceA 0          # nothing — deviceA goes quiet to us\n"
           "  Verify: run 'espnowsubs' on the PEER; its row for our MAC should\n"
           "  show the new mask. Skips appear in the PEER's heartbeat broadcast log.";
  }
  uint8_t mac[6];
  if (!resolveDeviceNameOrMac(a.arg(0), mac) && !parseMacAddress(a.arg(0), mac)) {
    return "Error: not a paired device name and not a valid MAC.";
  }
  uint32_t mask = 0;
  String maskStr = a.arg(1);
  maskStr.trim();
  if (maskStr.startsWith("0x") || maskStr.startsWith("0X")) {
    mask = (uint32_t)strtoul(maskStr.c_str() + 2, nullptr, 16);
  } else {
    mask = (uint32_t)strtoul(maskStr.c_str(), nullptr, 10);
  }

  V4PayloadSubscribe p{};
  p.requestedEvents = mask;
  // reserved[] zeroed by aggregate init

  uint32_t msgId = generateMessageId();
  if (!v4_send_payload_smart(mac, ESPNOW_V4_TYPE_SUBSCRIBE_UPDATE,
                              ESPNOW_V4_FLAG_ACK_REQ, msgId,
                              reinterpret_cast<const uint8_t*>(&p), sizeof(p), 1)) {
    if (!ensureDebugBuffer()) return "Error: SUBSCRIBE_UPDATE send failed.";
    snprintf(getDebugBuffer(), 1024,
             "Error: SUBSCRIBE_UPDATE send failed for %02X:%02X:%02X:%02X:%02X:%02X "
             "(peer may not be paired or session not ready).",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return getDebugBuffer();
  }
  if (!ensureDebugBuffer()) return "OK: SUBSCRIBE_UPDATE sent.";
  snprintf(getDebugBuffer(), 1024,
           "Asked %02X:%02X:%02X:%02X:%02X:%02X to send us only mask=0x%08lX "
           "(msgId=%lu). State updates ON THE PEER, not here — run 'espnowsubs' on "
           "the peer to verify (its row for our MAC should show the new mask). "
           "Skip counts will appear in the PEER's [V4_BROADCAST_GATED] logs, "
           "NOT in ours.",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           (unsigned long)mask, (unsigned long)msgId);
  return getDebugBuffer();
}

// Phase 3.5a: send a TEXT message AEAD-wrapped through the active session
// to the peer. Demonstrates SESSION_FRAME end-to-end without touching the
// existing espnowsend / v4_send_frame paths. Requires an ACTIVE session
// (run espnowsessionopen first).
const char* cmd_espnow_sessionsend(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow || !gEspNow->initialized) return "Error: ESP-NOW not initialized.";

  CommandArgs a(argsInput);
  if (a.count() < 2) {
    return "Error: invalid arguments — Usage: espnowsessionsend <name_or_mac> <message...>\n"
           "       Delivers an encrypted CHAT message (lands in the peer's espnowmessages). NOT command execution.\n"
           "       To run a command on the peer: espnowremote <target> <target-user> <target-pass> <command>";
  }
  uint8_t mac[6];
  if (!resolveDeviceNameOrMac(a.arg(0), mac) && !parseMacAddress(a.arg(0), mac)) {
    return "Error: not a paired device name and not a valid MAC.";
  }
  // Reassemble the message from args 1..N so spaces survive.
  String message = a.arg(1);
  for (uint8_t i = 2; i < a.count(); i++) {
    message += " ";
    message += a.arg(i);
  }
  if (message.length() == 0) return "Error: empty message.";
  // Plaintext + 16-byte tag must fit in ESPNOW_V4_MAX_PAYLOAD (218 B).
  if (message.length() > (size_t)(ESPNOW_V4_MAX_PAYLOAD - 16)) {
    return "Error: message too long for SESSION_FRAME (max 202 plaintext bytes).";
  }

  uint32_t msgId = generateMessageId();
  uint16_t plaintextLen = (uint16_t)message.length();
  char err[96] = {0};
  if (!v4_send_session_wrapped(mac, ESPNOW_V4_TYPE_TEXT, ESPNOW_V4_FLAG_ACK_REQ,
                               msgId,
                               reinterpret_cast<const uint8_t*>(message.c_str()),
                               plaintextLen, 1, err, sizeof(err))) {
    if (!ensureDebugBuffer()) return "Error: session send failed.";
    snprintf(getDebugBuffer(), 1024, "Error: %s.", err[0] ? err : "session send failed");
    return getDebugBuffer();
  }

  // Re-look up the session just for the success-message details (cheap).
  const PeerIdentity* pid = peerIdentityFindByMac(mac);
  SessionState* s = pid ? sessionFindByPeer(mac, pid->meshId) : nullptr;
  cliHint("this delivers a chat message to the peer's inbox, not a command - to run a command on the peer, use 'espnowremote'");
  if (!ensureDebugBuffer()) return "Session-encrypted TEXT sent.";
  snprintf(getDebugBuffer(), 1024,
           "Session-encrypted TEXT sent to %02X:%02X:%02X:%02X:%02X:%02X "
           "(sessionId=%u seq=%lu cipherLen=%u tagLen=16)",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           s ? (unsigned)s->sessionId : 0,
           s ? (unsigned long)(s->txSeqNext - 1) : 0,
           (unsigned)plaintextLen);
  return getDebugBuffer();
}

// Phase 3.4: dump all in-RAM SessionState slots — peer, sessionId, age, dir,
// counters. RAM-only; reboots wipe everything.
const char* cmd_espnow_sessions(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    JsonArray arr = doc["sessions"].to<JsonArray>();
    uint8_t slots = sessionSlotCount();
    uint32_t nowMs = (uint32_t)millis();
    static const char* kStates[] = { "FREE", "ESTAB", "ACTIVE", "REKEY", "CLOSED" };
    for (uint8_t i = 0; i < slots; i++) {
      const SessionState* s = sessionAt(i);
      if (!s) continue;
      JsonObject o = arr.add<JsonObject>();
      o["slot"]      = (int)i;
      o["mac"]       = String(MAC_STR(s->peerMac));
      o["meshId"]    = (unsigned)s->meshId;
      o["sessionId"] = (unsigned)s->sessionId;
      o["dir"]       = s->myDirection == 0 ? "A" : "B";
      o["state"]     = (s->state < 5) ? kStates[s->state] : "?";
      o["ageMs"]     = (unsigned long)(nowMs - s->establishedAtMs);
      o["txSeq"]     = (unsigned long)s->txSeqNext;
      o["rxHwm"]     = (unsigned long)s->rxSeqHighWater;
    }
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

  char* p   = getDebugBuffer();
  size_t cap = 1024;
  int written = snprintf(p, cap, "ESP-NOW Sessions:\n");
  if (written < 0) return p;
  uint8_t slots = sessionSlotCount();
  uint32_t nowMs = (uint32_t)millis();
  bool any = false;
  for (uint8_t i = 0; i < slots; i++) {
    const SessionState* s = sessionAt(i);
    if (!s) continue;
    any = true;
    static const char* kStates[] = { "FREE", "ESTAB", "ACTIVE", "REKEY", "CLOSED" };
    const char* stateStr = (s->state < 5) ? kStates[s->state] : "?";
    int n = snprintf(p + written, cap - written,
                     "  [%u] peer=%02X:%02X:%02X:%02X:%02X:%02X meshId=%u sessionId=%u "
                     "dir=%c state=%s ageMs=%lu txSeq=%lu rxHWM=%lu\n",
                     (unsigned)i,
                     s->peerMac[0], s->peerMac[1], s->peerMac[2],
                     s->peerMac[3], s->peerMac[4], s->peerMac[5],
                     (unsigned)s->meshId, (unsigned)s->sessionId,
                     s->myDirection == 0 ? 'A' : 'B',
                     stateStr,
                     (unsigned long)(nowMs - s->establishedAtMs),
                     (unsigned long)s->txSeqNext,
                     (unsigned long)s->rxSeqHighWater);
    if (n < 0 || (size_t)n >= cap - written) break;
    written += n;
  }
  if (!any) snprintf(p + written, cap - written, "  (no active sessions)\n");
  return p;
}

const char* cmd_espnow_regenidentity(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsInput;
  args.trim();
  if (args != "--confirm-wipe-all-bonds") {
    return "Error: destructive command — re-run with: "
           "espnowregenidentity --confirm-wipe-all-bonds";
  }

  EspNowIdentity fresh = {};
  if (!espnowIdentityRegenerate(fresh)) {
    return "Error: identity regeneration failed (see [ERROR][ESP-NOW] log lines)";
  }

  if (!ensureDebugBuffer()) return "OK (regenerated, but debug buffer unavailable for echo)";
  char pubHex[65];
  espnowIdentityFormatPubHex(fresh.pub, pubHex, sizeof(pubHex));
  char* p = getDebugBuffer();
  snprintf(p, 1024,
           "ESP-NOW identity regenerated.\n"
           "  Ed25519 pub: %s\n"
           "  regenCount:  %u\n"
           "NOTE: all previously paired peers must be re-paired (re-run KEY_EX / espnowpairsecure on both ends).",
           pubHex,
           (unsigned)fresh.regenCount);
  return getDebugBuffer();
}

// ESP-NOW pair device command
// Phase 2.5: parse an optional mesh argument from a CLI command.
// Accepts either a numeric index (0..N_MESHES-1) or a configured mesh label.
// Returns N_MESHES on parse error, or 0xFE if the arg is missing entirely
// (which the caller should interpret as "default mesh" — meshId=0 today).
// Caller is responsible for validating the resulting meshId is enabled.
static uint8_t parseMeshArgOrDefault(const String& arg) {
  if (arg.length() == 0) return 0xFE;  // missing
  // Try numeric first
  if (arg.length() <= 2) {
    bool isNum = true;
    for (size_t i = 0; i < arg.length(); i++) {
      if (arg[i] < '0' || arg[i] > '9') { isNum = false; break; }
    }
    if (isNum) {
      int n = arg.toInt();
      if (n < 0 || n >= (int)Settings::N_MESHES) return Settings::N_MESHES;  // out of range
      return (uint8_t)n;
    }
  }
  // Treat as label
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].label == arg) return i;
  }
  return Settings::N_MESHES;  // not found
}

const char* cmd_espnow_pair(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "Error: ESP-NOW not initialized. Run 'openespnow' first.";
  }

  CommandArgs a(argsInput);
  if (!a.hasMinArgs(2)) return "Error: invalid arguments — Usage: espnow pair <mac> <name> [mesh]";

  uint8_t mac[6];
  if (!a.argMac(0, mac)) {
    return "Error: Invalid MAC address format. Use AA:BB:CC:DD:EE:FF";
  }
  // Phase 2.5: name is now a single token (was: a.remaining(0)). The
  // optional 3rd arg is the mesh (label or 0..N_MESHES-1). Drops support
  // for spaces in peer names via this command — use underscores instead.
  String name = a.arg(1);
  uint8_t meshId = parseMeshArgOrDefault(a.arg(2));
  if (meshId == Settings::N_MESHES) {
    return "Error: Invalid mesh. Use a configured label or numeric index 0..3.";
  }
  if (meshId == 0xFE) meshId = 0;  // default to mesh 0 (primary)
  String macStr = a.arg(0);
  // Prevent pairing with self MAC (STA or AP interface)
  {
    uint8_t selfSta[6];
    uint8_t selfAp[6];
    esp_wifi_get_mac(WIFI_IF_STA, selfSta);
    esp_wifi_get_mac(WIFI_IF_AP, selfAp);
    if (memcmp(mac, selfSta, 6) == 0 || memcmp(mac, selfAp, 6) == 0) {
      return "Error: Cannot pair with self MAC address.";
    }
  }

  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (memcmp(gEspNow->devices[i].mac, mac, 6) == 0) {
      if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
      snprintf(getDebugBuffer(), 1024, "Device already paired. Use 'espnowunpair %s' first.", macStr.c_str());
      return getDebugBuffer();
    }
  }

  if (gEspNow->deviceCount >= 16) {
    return "Error: Maximum number of devices (16) already paired.";
  }

  if (!addEspNowPeerWithEncryption(mac, false, nullptr)) {
    return "Error: Failed to add unencrypted peer to ESP-NOW.";
  }

  memcpy(gEspNow->devices[gEspNow->deviceCount].mac, mac, 6);
  gEspNow->devices[gEspNow->deviceCount].name = name;
  gEspNow->devices[gEspNow->deviceCount].encrypted = false;
  memset(gEspNow->devices[gEspNow->deviceCount].key, 0, 16);
  gEspNow->devices[gEspNow->deviceCount].meshId = meshId;  // Phase 2.5
  // Initialize the optional metadata Strings to "" (matching addEspNowDevice and
  // the devices.json load path). gEspNow is ps_alloc'd without a constructor, so
  // these String members start zeroed — c_str() returns NULL until assigned, and
  // any %s consumer (e.g. /api/bond/paired-devices) would strlen(NULL)->crash.
  // This is why a freshly-paired device crashed the bond page but a reboot (which
  // reloads from devices.json, assigning "") did not.
  gEspNow->devices[gEspNow->deviceCount].friendlyName = "";
  gEspNow->devices[gEspNow->deviceCount].room = "";
  gEspNow->devices[gEspNow->deviceCount].zone = "";
  gEspNow->devices[gEspNow->deviceCount].tags = "";
  gEspNow->deviceCount++;

  removeFromUnpairedList(mac);

  // Seed gMeshPeers immediately so v4_broadcast reaches this peer on the next
  // heartbeat tick (without this, two freshly-paired devices never exchange
  // heartbeats until one reboots, so mesh topology stays empty)
  if (meshEnabled()) {
    noteMeshPeerRxActivity(mac, EspNowMeshRxKind::BootstrapLiveness);
    V4PayloadHeartbeat hb = {};
    hb.role = gSettings.meshRole;
    hb.uptimeSec = (uint32_t)(millis() / 1000);
    hb.freeHeap  = (uint32_t)ESP.getFreeHeap();
    strncpy(hb.deviceName, gSettings.espnowDeviceName.c_str(), sizeof(hb.deviceName) - 1);
    wifi_ap_record_t ap = {};
    hb.rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : (int8_t)-127;
    // 2026-05-19: smart-send so the post-pair seed heartbeat rides
    // SESSION_FRAME if a session is already up (rare — usually KEY_EX hasn't
    // run yet at this point, so this falls through to plaintext, which is
    // what we want for bootstrap liveness).
    v4_send_payload_smart(mac, ESPNOW_V4_TYPE_HEARTBEAT, ESPNOW_V4_FLAG_ACK_REQ, generateMessageId(),
                          (const uint8_t*)&hb, (uint16_t)sizeof(hb), 1);
  }
  bool peersOk   = saveMeshPeers();
  bool devicesOk = saveEspNowDevices();

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  if (!peersOk || !devicesOk) {
    snprintf(getDebugBuffer(), 1024,
      "Paired %s (%s) but failed to encrypt and save peer data — device encryption key unavailable. "
      "Peer will not persist across reboot.", name.c_str(), macStr.c_str());
  } else {
    snprintf(getDebugBuffer(), 1024, "Unencrypted device paired successfully: %s (%s)", name.c_str(), macStr.c_str());
  }
  return getDebugBuffer();
}

// Mesh TTL command
const char* cmd_espnow_meshttl(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);

  if (a.count() == 0) {
    if (!ensureDebugBuffer()) return "Error";
    int peerCount = (gEspNow ? (int)gEspNow->deviceCount : 0);
    snprintf(getDebugBuffer(), 1024, "Mesh TTL: %d\nAdaptive mode: %s\nActive peers: %d",
             gSettings.meshTTL, gSettings.meshAdaptiveTTL ? "enabled" : "disabled", peerCount);
    return getDebugBuffer();
  }

  // Check for 'adaptive' command
  String ttlArg = a.arg(0);
  ttlArg.toLowerCase();
  if (ttlArg == "adaptive") {
    setSetting(gSettings.meshAdaptiveTTL, !gSettings.meshAdaptiveTTL);
    
    snprintf(getDebugBuffer(), 1024, "Adaptive TTL %s (TTL now %d)", 
             gSettings.meshAdaptiveTTL ? "enabled" : "disabled", gSettings.meshTTL);
    return getDebugBuffer();
  }
  
  int ttl = a.argInt(0, 0);
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
const char* cmd_espnow_meshmetrics(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // Read-only status: see comment in cmd_espnow_status above.
  if (!gEspNow) return "Error: ESP-NOW not initialized (run 'openespnow' first)";
  if (!ensureDebugBuffer()) return "Error: Buffer allocation failed";

  if (argWantsJson(argsInput)) {
    // Counters are uninstrumented (see note below); expose the live config only.
    snprintf(getDebugBuffer(), 1024,
      "{\"schema\":1,\"mode\":\"%s\",\"activePeers\":%d,\"ttl\":%d,\"adaptiveTtl\":%s}",
      (gEspNow->mode == ESPNOW_MODE_MESH) ? "mesh" : "direct",
      (int)gEspNow->deviceCount, gSettings.meshTTL,
      gSettings.meshAdaptiveTTL ? "true" : "false");
    return getDebugBuffer();
  }

  // Audit (2026-06) — the mesh-routing counters this command used to print
  // (meshRoutes / directRoutes / meshForwards / path-length / TTL / loop stats)
  // had zero increment sites (multi-hop forwarding was never instrumented), so
  // they were removed from RouterMetrics. Only the live mesh configuration is
  // shown below. Re-add the fields + real instrumentation together if wanted.

  int peerCount = (int)gEspNow->deviceCount;
  int pos = 0;
  char* buf = getDebugBuffer();
  pos += snprintf(buf + pos, 1024 - pos, "=== Mesh Routing Configuration ===\n\n");
  pos += snprintf(buf + pos, 1024 - pos, "Mode:           %s\n",
                  (gEspNow->mode == ESPNOW_MODE_MESH) ? "mesh" : "direct");
  pos += snprintf(buf + pos, 1024 - pos, "Active peers:   %d\n", peerCount);
  pos += snprintf(buf + pos, 1024 - pos, "Current TTL:    %d\n", gSettings.meshTTL);
  pos += snprintf(buf + pos, 1024 - pos, "Adaptive TTL:   %s\n",
                  gSettings.meshAdaptiveTTL ? "enabled" : "disabled");
  pos += snprintf(buf + pos, 1024 - pos,
                  "\n(Note: per-forward / per-path / drop counters are not currently\n"
                  " instrumented — multi-hop mesh forwarding has no live bump sites.\n"
                  " See espnowsaturation / espnowstats for traffic-level metrics.)\n");

  return getDebugBuffer();
}

// ESP-NOW mode command
const char* cmd_espnow_mode(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"]  = 1;
    doc["enabled"] = gSettings.espnowenabled;
    doc["mode"]    = getEspNowModeString();
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }
  if (a.count() == 0) {
    snprintf(getDebugBuffer(), 1024, "ESP-NOW mode: %s", getEspNowModeString());
    return getDebugBuffer();
  }
  String mode = a.arg(0);
  mode.toLowerCase();
  if (mode == "direct") {
    setSetting(gSettings.espnowmesh, false);
    if (gEspNow) {
      gEspNow->mode = ESPNOW_MODE_DIRECT;  // Update runtime state immediately
    }
    saveMeshPeers();
    BROADCAST_PRINTF("[ESP-NOW] mode set to %s", getEspNowModeString());
    return "ESP-NOW mode set to direct";
  } else if (mode == "mesh") {
    setSetting(gSettings.espnowmesh, true);
    if (gEspNow) {
      gEspNow->mode = ESPNOW_MODE_MESH;  // Update runtime state immediately
    }
    saveMeshPeers();
    BROADCAST_PRINTF("[ESP-NOW] mode set to %s", getEspNowModeString());
    return "ESP-NOW mode set to mesh";
  }
  return "Error: invalid arguments — Usage: espnow mode [direct|mesh]";
}

// ESP-NOW setname command
const char* cmd_espnow_setname(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);

  if (a.count() == 0) {
    if (gSettings.espnowDeviceName.length() > 0) {
      snprintf(getDebugBuffer(), 1024, "Device name: %s", gSettings.espnowDeviceName.c_str());
    } else {
      snprintf(getDebugBuffer(), 1024, "Device name: (not set)");
    }
    return getDebugBuffer();
  }

  String name = a.arg(0);
  if (name.length() > 20) {
    return "Error: Device name must be 20 characters or less";
  }

  for (size_t i = 0; i < name.length(); i++) {
    char c = name.charAt(i);
    if (!isalnum(c) && c != '-' && c != '_') {
      return "Error: Device name can only contain letters, numbers, hyphens, and underscores";
    }
  }

  setSetting(gSettings.espnowDeviceName, name);
  setSetting(gSettings.espnowFirstTimeSetup, true);
  
  if (gEspNow && gEspNow->initialized) {
    uint8_t myMac[6];
    esp_wifi_get_mac(WIFI_IF_STA, myMac);
    
    bool found = false;
    for (int i = 0; i < gEspNow->deviceCount; i++) {
      if (memcmp(gEspNow->devices[i].mac, myMac, 6) == 0) {
        gEspNow->devices[i].name = name;
        found = true;
        break;
      }
    }

    if (!found) {
      addEspNowDevice(myMac, name, false, nullptr);
    }

    saveMeshPeers();
  }

  snprintf(getDebugBuffer(), 1024, "Device name set to: %s", name.c_str());
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
    setSetting(field, String(""));  // Persist to flash
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
  setSetting(field, trimmed);  // Persist to flash
  gMetadataChanged = true;  // Mark metadata as dirty
  snprintf(getDebugBuffer(), 1024, "%s set to: %s", fieldName, field.c_str());
  return getDebugBuffer();
}

const char* cmd_espnow_room(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return metaGetSet(argsInput, gSettings.espnowRoom, "Room", 31);
}

const char* cmd_espnow_zone(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return metaGetSet(argsInput, gSettings.espnowZone, "Zone", 31);
}

const char* cmd_espnow_tags(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return metaGetSet(argsInput, gSettings.espnowTags, "Tags", 63);
}

const char* cmd_espnow_friendlyname(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  return metaGetSet(argsInput, gSettings.espnowFriendlyName, "Friendly name", 47);
}

const char* cmd_espnow_stationary(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  if (a.count() == 0) {
    snprintf(getDebugBuffer(), 1024, "Stationary: %s", gSettings.espnowStationary ? "true" : "false");
    return getDebugBuffer();
  }
  bool val = a.argBool(0, false);
  setSetting(gSettings.espnowStationary, val);
  snprintf(getDebugBuffer(), 1024, "Stationary set to: %s", val ? "true" : "false");
  return getDebugBuffer();
}

const char* cmd_espnow_deviceinfo(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"]       = 1;
    doc["name"]         = gSettings.espnowDeviceName;
    doc["friendlyName"] = gSettings.espnowFriendlyName;
    doc["room"]         = gSettings.espnowRoom;
    doc["zone"]         = gSettings.espnowZone;
    doc["tags"]         = gSettings.espnowTags;
    doc["stationary"]   = gSettings.espnowStationary;
    const char* roleStr = "worker";
    if (gSettings.meshRole == MESH_ROLE_MASTER) roleStr = "master";
    else if (gSettings.meshRole == MESH_ROLE_BACKUP_MASTER) roleStr = "backup";
    doc["meshRole"] = roleStr;
    uint8_t myMac[6]; esp_wifi_get_mac(WIFI_IF_STA, myMac);
    doc["mac"] = String(MAC_STR(myMac));
    doc["hint"] = "this is THIS device's metadata - to list mesh peers use 'espnowdevices', or refresh one peer with 'espnowrequestmeta <peer>'";
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }
  cliHint("this is THIS device's metadata - to list mesh peers use 'espnowdevices', or refresh one peer with 'espnowrequestmeta <peer>'");
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

// Shared by the espnowdevices json CLI branch and the web metadata handler so
// the per-peer metadata shape lives in exactly one place.
void espnowSerializeMeshPeerMeta(JsonObject o, const MeshPeerMeta& m) {
  MeshPeerHealth* health = getMeshPeerHealth(m.mac, false);
  bool alive = health ? isMeshPeerAlive(health) : false;
  uint32_t lastContact = 0;
  if (health) {
    lastContact = health->lastMeshHeartbeatMs;
    if (health->lastRxActivityMs > lastContact) lastContact = health->lastRxActivityMs;
  }
  uint32_t ageSec = lastContact ? ((millis() - lastContact) / 1000) : 0;
  o["mac"]          = String(MAC_STR(m.mac));
  o["deviceName"]   = m.name;          // matches web's "deviceName"
  o["friendlyName"] = m.friendlyName;
  o["room"]         = m.room;
  o["zone"]         = m.zone;
  o["tags"]         = m.tags;
  o["stationary"]   = m.stationary;
  o["online"]       = alive;
  o["lastSeenSec"]  = ageSec;
  o["source"]       = "mesh";
}

const char* cmd_espnow_devices(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  // JSON: all-peers dump of the synced metadata cache (gMeshPeerMeta) in one
  // call — the BLE/CLI parity for the web's per-peer /api/espnow/metadata, but
  // for the whole mesh at once. Same per-peer shape (espnowSerializeMeshPeerMeta)
  // the web handler now uses. This is the on-device source; no per-peer
  // round-trip or creds, available whether or not the web is compiled in.
  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    JsonArray devs = doc["devices"].to<JsonArray>();
    int count = 0;
    for (int i = 0; i < gMeshPeerSlots; i++) {
      if (!gMeshPeerMeta[i].isActive) continue;
      espnowSerializeMeshPeerMeta(devs.add<JsonObject>(), gMeshPeerMeta[i]);
      count++;
    }
    doc["count"] = count;
    doc["hint"] = "a peer's value: run it remotely with 'espnowremote <peer> <target-user> <target-pass> <cmd>' then read the reply with 'espnowmessages json'; a peer's name/room/tags: 'espnowrequestmeta <peer>'";
    static String out;
    out = "";
    serializeJson(doc, out);
    return out.c_str();
  }

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
    uint32_t lastContact = 0;
    if (health) {
      lastContact = health->lastMeshHeartbeatMs;
      if (health->lastRxActivityMs > lastContact) lastContact = health->lastRxActivityMs;
    }
    uint32_t ageSec = lastContact ? ((millis() - lastContact) / 1000) : 0;

    const char* displayName = gMeshPeerMeta[i].friendlyName[0]
      ? gMeshPeerMeta[i].friendlyName : gMeshPeerMeta[i].name;
    if (!displayName[0]) displayName = MAC_STR(gMeshPeerMeta[i].mac);

    char zoneBuf[34] = "";
    if (gMeshPeerMeta[i].zone[0]) snprintf(zoneBuf, sizeof(zoneBuf), "/%s", gMeshPeerMeta[i].zone);
    pos += snprintf(buf + pos, 1024 - pos, "  %s%s [%s%s] %s",
                    displayName,
                    gMeshPeerMeta[i].room[0] ? "" : "",
                    gMeshPeerMeta[i].room[0] ? gMeshPeerMeta[i].room : "unassigned",
                    zoneBuf,
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
  emitListingTrailer("discovered/paired mesh peers",
                     "a peer's value: run it remotely with 'espnowremote <peer> <target-user> <target-pass> <cmd>' then read the reply with 'espnowmessages json'; a peer's name/room/tags: 'espnowrequestmeta <peer>'");
  return buf;
}

const char* cmd_espnow_rooms(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    JsonArray roomsArr = doc["rooms"].to<JsonArray>();
    const char* seen[MESH_PEER_MAX];
    int seenCount = 0;
    for (int i = 0; i < gMeshPeerSlots; i++) {
      if (!gMeshPeerMeta[i].isActive) continue;
      const char* room = gMeshPeerMeta[i].room[0] ? gMeshPeerMeta[i].room : "Unassigned";
      bool dup = false;
      for (int r = 0; r < seenCount; r++) if (strcmp(seen[r], room) == 0) { dup = true; break; }
      if (dup || seenCount >= MESH_PEER_MAX) continue;
      seen[seenCount++] = room;
      JsonObject ro = roomsArr.add<JsonObject>();
      ro["room"] = room;
      JsonArray devs = ro["devices"].to<JsonArray>();
      for (int j = 0; j < gMeshPeerSlots; j++) {
        if (!gMeshPeerMeta[j].isActive) continue;
        const char* r2 = gMeshPeerMeta[j].room[0] ? gMeshPeerMeta[j].room : "Unassigned";
        if (strcmp(r2, room) != 0) continue;
        MeshPeerHealth* h = getMeshPeerHealth(gMeshPeerMeta[j].mac, false);
        JsonObject d = devs.add<JsonObject>();
        const char* dn = gMeshPeerMeta[j].friendlyName[0] ? gMeshPeerMeta[j].friendlyName : gMeshPeerMeta[j].name;
        d["name"]   = String(dn[0] ? dn : MAC_STR(gMeshPeerMeta[j].mac));
        d["tags"]   = gMeshPeerMeta[j].tags;
        d["online"] = h ? isMeshPeerAlive(h) : false;
      }
    }
    doc["count"] = seenCount;
    doc["hint"] = "a peer's value: run it remotely with 'espnowremote <peer> <target-user> <target-pass> <cmd>' then read the reply with 'espnowmessages json'; a peer's name/room/tags: 'espnowrequestmeta <peer>'";
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

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
  emitListingTrailer("discovered/paired mesh peers",
                     "a peer's value: run it remotely with 'espnowremote <peer> <target-user> <target-pass> <cmd>' then read the reply with 'espnowmessages json'; a peer's name/room/tags: 'espnowrequestmeta <peer>'");
  return buf;
}

const char* cmd_espnow_find(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String query = argsInput;
  query.trim();
  if (query.length() == 0) return "Error: invalid arguments — Usage: espnow find <query> — search by name, room, or tag";

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
    snprintf(buf, 1024, "No devices matching '%s'", argsInput.c_str());
  } else {
    emitListingTrailer("discovered/paired mesh peers",
                       "a peer's value: run it remotely with 'espnowremote <peer> <target-user> <target-pass> <cmd>' then read the reply with 'espnowmessages json'; a peer's name/room/tags: 'espnowrequestmeta <peer>'");
  }
  return buf;
}

const char* cmd_espnow_roomcmd(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(4)) return "Error: invalid arguments — Usage: espnow roomcmd <room> <user> <pass> <command>";

  String targetRoom = a.arg(0);
  String user = a.arg(1);
  String pass = a.arg(2);
  String command = a.remaining(2);

  char* buf = getDebugBuffer();
  int pos = 0;
  int sent = 0;

  for (int i = 0; i < gMeshPeerSlots; i++) {
    if (!gMeshPeerMeta[i].isActive) continue;
    if (!gMeshPeerMeta[i].room[0]) continue;

    String room = gMeshPeerMeta[i].room;
    if (!room.equalsIgnoreCase(targetRoom)) continue;

    // Send remote command directly — no String concat, no re-parse
    char macStrBuf[18];
    formatMacAddressBuf(gMeshPeerMeta[i].mac, macStrBuf, sizeof(macStrBuf));
    char roomCmdPayload[ESPNOW_V4_MAX_PLAINTEXT];
    snprintf(roomCmdPayload, sizeof(roomCmdPayload), "%s:%s:%s", user.c_str(), pass.c_str(), command.c_str());
    // Phase 3.5 task #6 — encrypted-or-queue. Payload carries credentials;
    // peers without KEY_EX-derived identity get silently skipped (return false)
    // rather than leaking the password over plaintext.
    v4_send_encrypted_or_queue(gMeshPeerMeta[i].mac, ESPNOW_V4_TYPE_CMD, ESPNOW_V4_FLAG_ACK_REQ,
                                generateMessageId(),
                                (const uint8_t*)roomCmdPayload, (uint16_t)strlen(roomCmdPayload), 1,
                                nullptr, 0);
    sent++;

    const char* displayName = gMeshPeerMeta[i].friendlyName[0]
      ? gMeshPeerMeta[i].friendlyName : gMeshPeerMeta[i].name;
    pos += snprintf(buf + pos, 1024 - pos, "  -> %s (%s)\n", displayName, macStrBuf);
    if (pos >= 900) break;
  }

  if (sent == 0) {
    cliHint("no device matched - list rooms and members with 'espnowdevices'");
    snprintf(buf, 1024, "No devices found in room '%s'", targetRoom.c_str());
  } else {
    cliHint("each device's reply returns asynchronously - read them with 'espnowmessages json'");
    pos += snprintf(buf + pos, 1024 - pos, "Sent '%s' to %d device(s) in %s",
                    command.c_str(), sent, targetRoom.c_str());
  }
  return buf;
}

const char* cmd_espnow_tagcmd(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(4)) return "Error: invalid arguments — Usage: espnow tagcmd <tag> <user> <pass> <command>";

  String targetTag = a.arg(0);
  String user = a.arg(1);
  String pass = a.arg(2);
  String command = a.remaining(2);

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

    // Send remote command directly — no String concat, no re-parse
    char tagMacStrBuf[18];
    formatMacAddressBuf(gMeshPeerMeta[i].mac, tagMacStrBuf, sizeof(tagMacStrBuf));
    char tagCmdPayload[ESPNOW_V4_MAX_PLAINTEXT];
    snprintf(tagCmdPayload, sizeof(tagCmdPayload), "%s:%s:%s", user.c_str(), pass.c_str(), command.c_str());
    // Phase 3.5 task #6 — encrypted-or-queue (credentials protection).
    v4_send_encrypted_or_queue(gMeshPeerMeta[i].mac, ESPNOW_V4_TYPE_CMD, ESPNOW_V4_FLAG_ACK_REQ,
                                generateMessageId(),
                                (const uint8_t*)tagCmdPayload, (uint16_t)strlen(tagCmdPayload), 1,
                                nullptr, 0);
    sent++;

    const char* displayName = gMeshPeerMeta[i].friendlyName[0]
      ? gMeshPeerMeta[i].friendlyName : gMeshPeerMeta[i].name;
    pos += snprintf(buf + pos, 1024 - pos, "  -> %s (%s)\n", displayName, tagMacStrBuf);
    if (pos >= 900) break;
  }

  if (sent == 0) {
    cliHint("no device matched - list devices and their tags with 'espnowdevices'");
    snprintf(buf, 1024, "No devices found with tag '%s'", targetTag.c_str());
  } else {
    cliHint("each device's reply returns asynchronously - read them with 'espnowmessages json'");
    pos += snprintf(buf + pos, 1024 - pos, "Sent '%s' to %d device(s) with tag '%s'",
                    command.c_str(), sent, targetTag.c_str());
  }
  return buf;
}

// ESP-NOW heartbeat mode command
const char* cmd_espnow_hbmode(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);

  if (a.count() == 0) {
    const char* mode = gSettings.meshHeartbeatBroadcast ? "public" : "private";
    const char* desc = gSettings.meshHeartbeatBroadcast
      ? "Heartbeats broadcast to all devices (discovery enabled)"
      : "Heartbeats sent only to paired devices (discovery disabled)";
    snprintf(getDebugBuffer(), 1024, "Heartbeat mode: %s\n%s", mode, desc);
    return getDebugBuffer();
  }

  String hbArg = a.arg(0);
  hbArg.toLowerCase();
  if (hbArg == "public" || hbArg == "broadcast") {
    setSetting(gSettings.meshHeartbeatBroadcast, true);
    return "Heartbeat mode set to public (broadcast). Unpaired devices can now be discovered.";
  } else if (hbArg == "private" || hbArg == "unicast") {
    setSetting(gSettings.meshHeartbeatBroadcast, false);
    return "Heartbeat mode set to private (unicast). Only paired devices will receive heartbeats.";
  }
  
  return "Error: invalid arguments — Usage: espnow hbmode [public|private]";
}

// Mesh role command
const char* cmd_espnow_meshrole(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"]        = 1;
    doc["role"]          = getMeshRoleString(gSettings.meshRole);
    doc["masterMac"]     = gSettings.meshMasterMAC;
    doc["backupEnabled"] = gSettings.meshBackupEnabled;
    doc["backupMac"]     = gSettings.meshBackupMAC;
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

  if (a.count() == 0) {
    int pos = 0;
    char* buf = getDebugBuffer();
    pos += snprintf(buf + pos, 1024 - pos, "Mesh role: %s", getMeshRoleString(gSettings.meshRole));
    if (gSettings.meshMasterMAC.length() > 0) {
      pos += snprintf(buf + pos, 1024 - pos, "\nMaster MAC: %s", gSettings.meshMasterMAC.c_str());
    }
    pos += snprintf(buf + pos, 1024 - pos, "\nBackup enabled: %s", gSettings.meshBackupEnabled ? "yes" : "no");
    if (gSettings.meshBackupEnabled && gSettings.meshBackupMAC.length() > 0) {
      pos += snprintf(buf + pos, 1024 - pos, "\nBackup MAC: %s", gSettings.meshBackupMAC.c_str());
    }
    return buf;
  }
  
  String role = a.arg(0);
  role.toLowerCase();
  if (role == "worker") {
    setSetting(gSettings.meshRole, (uint8_t)MESH_ROLE_WORKER);
    BROADCAST_PRINTF("[MESH] Role set to worker");
    return "Role set to worker";
  } else if (role == "master") {
    setSetting(gSettings.meshRole, (uint8_t)MESH_ROLE_MASTER);
    setSetting(gSettings.meshMasterMAC, String(""));
    BROADCAST_PRINTF("[MESH] Role set to master");
    return "Role set to master";
  } else if (role == "backup") {
    setSetting(gSettings.meshRole, (uint8_t)MESH_ROLE_BACKUP_MASTER);
    BROADCAST_PRINTF("[MESH] Role set to backup master");
    return "Role set to backup master";
  }

  return "Error: invalid arguments — Usage: espnow meshrole [worker|master|backup]";
}


// Worker status configuration
const char* cmd_espnow_worker(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";

  CommandArgs a(argsInput);
  String subcmd = a.arg(0);
  subcmd.toLowerCase();

  // Show current configuration
  if (a.count() == 0 || subcmd == "show" || subcmd == "json") {
    if (argWantsJson(argsInput)) {
      snprintf(getDebugBuffer(), 1024,
               "{\"schema\":1,\"enabled\":%s,\"intervalMs\":%u,"
               "\"fields\":{\"heap\":%s,\"rssi\":%s,\"thermal\":%s,\"imu\":%s}}",
               gWorkerStatusConfig.enabled ? "true" : "false",
               gWorkerStatusConfig.intervalMs,
               gWorkerStatusConfig.includeHeap ? "true" : "false",
               gWorkerStatusConfig.includeRssi ? "true" : "false",
               gWorkerStatusConfig.includeThermal ? "true" : "false",
               gWorkerStatusConfig.includeImu ? "true" : "false");
      return getDebugBuffer();
    }
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
  if (subcmd == "on" || subcmd == "enable") {
    gWorkerStatusConfig.enabled = true;
    return "Worker status reporting enabled";
  }
  if (subcmd == "off" || subcmd == "disable") {
    gWorkerStatusConfig.enabled = false;
    return "Worker status reporting disabled";
  }

  // Set interval
  if (subcmd == "interval") {
    long interval = a.argInt(1, 0);
    if (interval < 1000) return "Error: interval must be >= 1000 ms";
    if (interval > 300000) return "Error: interval must be <= 300000 ms (5 min)";
    gWorkerStatusConfig.intervalMs = (uint16_t)interval;
    snprintf(getDebugBuffer(), 1024, "Worker status interval set to %u ms", gWorkerStatusConfig.intervalMs);
    return getDebugBuffer();
  }
  
  // Configure fields
  if (subcmd == "fields") {
    String fields = a.remaining(0);
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
  
  return "Error: invalid arguments — Usage: espnow worker [show|on|off|interval <ms>|fields <heap,rssi,thermal,imu>]";
}

// V2 fragmentation is now mandatory - command removed

// Mesh master command
const char* cmd_espnow_meshmaster(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);

  if (a.count() == 0) {
    if (gSettings.meshMasterMAC.length() > 0) {
      BROADCAST_PRINTF("Master MAC: %s", gSettings.meshMasterMAC.c_str());
    } else {
      BROADCAST_PRINTF("No master assigned");
    }
    return "OK";
  }

  String mac = a.arg(0);
  if (mac.length() != 17) {
    return "Error: Invalid MAC address format. Use: XX:XX:XX:XX:XX:XX";
  }

  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String myMacStr = macToHexString(myMac);
  if (mac.equalsIgnoreCase(myMacStr)) {
    return "Error: Cannot set your own MAC as master MAC";
  }

  mac.toUpperCase();
  setSetting(gSettings.meshMasterMAC, mac);
  BROADCAST_PRINTF("[MESH] Master MAC set to %s", gSettings.meshMasterMAC.c_str());
  return "OK";
}

// Mesh backup command
const char* cmd_espnow_meshbackup(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);

  if (a.count() == 0) {
    if (gSettings.meshBackupMAC.length() > 0) {
      BROADCAST_PRINTF("Backup MAC: %s", gSettings.meshBackupMAC.c_str());
    } else {
      BROADCAST_PRINTF("No backup assigned");
    }
    return "OK";
  }

  String mac = a.arg(0);
  if (mac.length() != 17) {
    return "Error: Invalid MAC address format. Use: XX:XX:XX:XX:XX:XX";
  }

  uint8_t myMac[6];
  esp_wifi_get_mac(WIFI_IF_STA, myMac);
  String myMacStr = macToHexString(myMac);
  if (mac.equalsIgnoreCase(myMacStr)) {
    return "Error: Cannot set your own MAC as backup MAC";
  }

  mac.toUpperCase();
  setSetting(gSettings.meshBackupMAC, mac);
  BROADCAST_PRINTF("[MESH] Backup MAC set to %s", gSettings.meshBackupMAC.c_str());
  return "OK";
}

// Backup master enable/disable command
const char* cmd_espnow_backupenable(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  String args = argsInput;
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
    return "Error: invalid arguments — Usage: espnow backupenable [on|off]";
  }

  setSetting(gSettings.meshBackupEnabled, enable);
  return enable ? "Backup master enabled" : "Backup master disabled";
}

// Mesh topology discovery command
const char* cmd_espnow_meshtopo(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!meshEnabled()) {
    return "Error: Mesh mode not enabled. Use 'espnowmode mesh' first.";
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
      broadcastOutput("Pair devices using 'espnowpair' or 'espnowpairsecure' first.");
    }
    return "ERROR";
  }
  
  BROADCAST_PRINTF("[TOPO] Initiating topology discovery for %d peer(s)...", peerCount);
  requestTopologyDiscovery();
  return "Topology discovery initiated. Use 'espnowtoporesults' to view responses.";
}

// Time sync command
const char* cmd_espnow_timesync(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!meshEnabled()) {
    return "Error: Mesh mode not enabled. Use 'espnowmode mesh' first.";
  }
  
  uint32_t epoch = (uint32_t)time(nullptr);
  if (epoch < 100000) {
    return "Error: No valid NTP time available. Ensure WiFi is connected and NTP is synced.";
  }
  
  DEBUG_ESPNOWF("[TIME_SYNC] Broadcasting time sync: epoch=%lu", (unsigned long)epoch);
  
  // Use V4 binary protocol instead of V2 JSON
  uint32_t millisAtEpoch = millis();
  bool sent = v4_broadcast_time_sync(epoch, millisAtEpoch);
  
  if (sent) {
    BROADCAST_PRINTF("Time sync broadcast sent (epoch: %lu)", (unsigned long)epoch);
    return "OK";
  } else {
    return "Error: Failed to send time sync";
  }
}

// Time status command
const char* cmd_espnow_timestatus(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "ERROR";
  
  if (gTimeIsSynced) {
    uint32_t epoch = (uint32_t)time(nullptr);
    uint32_t secondsSinceSync = (millis() - gLastTimeSyncMs) / 1000;
    snprintf(getDebugBuffer(), 1024, 
             "Time Status:\n  Synced: Yes\n  Epoch: %lu\n  Last sync: %lu seconds ago",
             (unsigned long)epoch, (unsigned long)secondsSinceSync);
  } else {
    snprintf(getDebugBuffer(), 1024, "Time Status:\n  Synced: No\n  Use 'espnowtimesync' on master to sync");
  }
  
  return getDebugBuffer();
}

// Mesh save command
const char* cmd_espnow_meshsave(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!meshEnabled()) {
    return "Error: Mesh mode not enabled.";
  }
  
  saveMeshPeers();
  return "Mesh peer topology saved to filesystem.";
}

// Topology results command
const char* cmd_espnow_toporesults(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  uint32_t now = millis();
  bool collectionActive = (gTopoRequestId != 0 && now < gTopoRequestTimeout);
  bool withinCollectionWindow = (gTopoLastResponseTime > 0 && 
                                  (now - gTopoLastResponseTime) < TOPO_COLLECTION_WINDOW_MS);
  
  if (collectionActive && (withinCollectionWindow || gTopoLastResponseTime == 0)) {
    return "Still collecting topology responses; run espnowtoporesults again shortly.";
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
const char* cmd_test_streams(const String& argsInput) {
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
const char* cmd_test_concurrent(const String& argsInput) {
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
  broadcastOutput("\nRun 'espnowtoporesults' to view the simulated results");
  
  return "OK";
}

// Test cleanup
const char* cmd_test_cleanup(const String& argsInput) {
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

// Phase 4: was cmd_test_filelock (exercised the old single-slot lock).
// Repurposed to dump the multi-slot table for diagnostics. Kept the same
// command name so anyone with muscle memory still gets useful output.
const char* cmd_test_filelock(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  broadcastOutput("\n=== File Transfer Slot Table ===");
  uint8_t total  = fileSlotsSlotCount();
  uint8_t active = fileSlotsActiveCount();
  BROADCAST_PRINTF("Slots: %u/%u in use", (unsigned)active, (unsigned)total);
  if (active == 0) {
    broadcastOutput("  (all slots idle)");
  } else {
    uint32_t nowMs = (uint32_t)millis();
    for (uint8_t i = 0; i < total; i++) {
      FileTransferSlotInfo info;
      if (!fileSlotsSnapshot(i, &info)) continue;
      static const char* kStates[] = { "FREE", "RECEIVING", "COMPLETING", "FAILED" };
      const char* stateStr = (info.state < 4) ? kStates[info.state] : "?";
      BROADCAST_PRINTF("  [%u] %s '%s' from %02X:%02X:%02X:%02X:%02X:%02X "
                       "msgId=%lu %lu/%lu bytes %u/%u chunks ageMs=%lu",
                       (unsigned)i, stateStr, info.filename,
                       info.peerMac[0], info.peerMac[1], info.peerMac[2],
                       info.peerMac[3], info.peerMac[4], info.peerMac[5],
                       (unsigned long)info.msgId,
                       (unsigned long)info.receivedBytes,
                       (unsigned long)info.totalSize,
                       (unsigned)info.receivedChunks,
                       (unsigned)info.totalChunks,
                       (unsigned long)(nowMs - info.startedMs));
    }
  }
  broadcastOutput("=== End slot table ===");
  return "OK";
}

// ESP-NOW Device Management Commands
// ============================================================================

// List paired devices
const char* cmd_espnow_list(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "{\"schema\":1,\"ok\":false,\"error\":\"ESP-NOW not initialized\"}";
  if (!gEspNow->initialized) {
    return "{\"schema\":1,\"ok\":false,\"error\":\"ESP-NOW not initialized\"}";
  }

  // Build JSON from gEspNow->devices[] — the authoritative source of truth.
  // esp_now_fetch_peer() only reflects the hardware peer table which may lag
  // behind after a reboot until loadMeshPeers() re-registers all peers.
  PSRAM_JSON_DOC(doc);
  doc["schema"] = 1;
  JsonArray devices = doc["devices"].to<JsonArray>();

  int listedCount = 0;
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (isSelfMac(gEspNow->devices[i].mac)) continue;  // Don't list self
    JsonObject d = devices.add<JsonObject>();
    char listMacBuf[18];
    formatMacAddressBuf(gEspNow->devices[i].mac, listMacBuf, sizeof(listMacBuf));
    // Wrap in String() — same dangling-stack-buffer fix as meshesCmd_listjson.
    d["mac"]       = String(listMacBuf);
    d["name"]      = gEspNow->devices[i].name;
    d["encrypted"] = gEspNow->devices[i].encrypted;
    // Phase 2.8: surface which mesh slot this peer was paired into so
    // the web UI can show a "mesh: <label>" badge per row and grey out
    // peers whose mesh is currently disabled.
    d["meshId"]    = (int)gEspNow->devices[i].meshId;
    listedCount++;
  }

  doc["count"] = listedCount;
  doc["hint"] = "a peer's value: run it remotely with 'espnowremote <peer> <target-user> <target-pass> <cmd>' then read the reply with 'espnowmessages json'; a peer's name/room/tags: 'espnowrequestmeta <peer>'";

  size_t needed = measureJson(doc) + 1;
  static const size_t bufSize = 1024;
  if (needed > bufSize) needed = bufSize;
  if (!gEspNow->listBuffer) return "{}";
  serializeJson(doc, gEspNow->listBuffer, needed);

  DEBUGF(DEBUG_HTTP, "[ESP-NOW] list: %d devices", gEspNow->deviceCount);
  return gEspNow->listBuffer;
}

// Mesh status command
const char* cmd_espnow_meshstatus(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "{\"schema\":1,\"ok\":false,\"error\":\"ESP-NOW not initialized\"}";
  if (!gEspNow->initialized) {
    return "{\"schema\":1,\"ok\":false,\"error\":\"ESP-NOW not initialized\"}";
  }

  if (!meshEnabled()) {
    return "{\"schema\":1,\"ok\":false,\"error\":\"Mesh mode not enabled\"}";
  }

  // Use ArduinoJson to avoid String concatenation heap fragmentation
  // Use PSRAM allocator to avoid internal heap fragmentation in cmd_exec task
  PSRAM_JSON_DOC(doc);
  doc["schema"] = 1;
  JsonArray peers = doc["peers"].to<JsonArray>();
  uint32_t now = millis();
  int activePeers = 0;
  buildMeshStatusPeersJson(peers, now, &activePeers);
  doc["totalPeers"] = activePeers;

  JsonArray unpaired = doc["unpaired"].to<JsonArray>();
  int unpairedCount = 0;
  
  for (int i = 0; i < gEspNow->unpairedDeviceCount; i++) {
    if (isPairedDevice(gEspNow->unpairedDevices[i].mac)) continue;
    
    JsonObject dev = unpaired.add<JsonObject>();
    uint32_t elapsed = now - gEspNow->unpairedDevices[i].lastSeenMs;
    char unpairedMacBuf[18];
    snprintf(unpairedMacBuf, sizeof(unpairedMacBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
             gEspNow->unpairedDevices[i].mac[0], gEspNow->unpairedDevices[i].mac[1],
             gEspNow->unpairedDevices[i].mac[2], gEspNow->unpairedDevices[i].mac[3],
             gEspNow->unpairedDevices[i].mac[4], gEspNow->unpairedDevices[i].mac[5]);
    // String() wrap forces deep-copy into the doc pool (task #14 pattern).
    dev["mac"] = String(unpairedMacBuf);
    dev["name"] = gEspNow->unpairedDevices[i].name.length() > 0 ? gEspNow->unpairedDevices[i].name : "Unknown";
    dev["rssi"] = gEspNow->unpairedDevices[i].rssi;
    dev["heartbeatCount"] = gEspNow->unpairedDevices[i].heartbeatCount;
    dev["secondsSinceLastSeen"] = elapsed / 1000;
    
    unpairedCount++;
  }
  
  doc["totalUnpaired"] = unpairedCount;

  JsonArray retryQueue = doc["retryQueue"].to<JsonArray>();
  int activeRetries = 0;
  
  {
    MeshRetryGuard retryGuard("espnowDebugJson");
    if (retryGuard.held) {
      for (int i = 0; i < MESH_RETRY_QUEUE_SIZE; i++) {
        if (!gMeshRetryQueue[i].active) continue;
        
        JsonObject retry = retryQueue.add<JsonObject>();
        uint32_t elapsed = now - gMeshRetryQueue[i].sentMs;
        
        retry["msgId"] = gMeshRetryQueue[i].msgId;
        char retryMacBuf[18];
        snprintf(retryMacBuf, sizeof(retryMacBuf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 gMeshRetryQueue[i].dstMac[0], gMeshRetryQueue[i].dstMac[1],
                 gMeshRetryQueue[i].dstMac[2], gMeshRetryQueue[i].dstMac[3],
                 gMeshRetryQueue[i].dstMac[4], gMeshRetryQueue[i].dstMac[5]);
        retry["dst"] = String(retryMacBuf);
        retry["retryCount"] = gMeshRetryQueue[i].retryCount;
        retry["secondsWaiting"] = elapsed / 1000;
        
        activeRetries++;
      }
    }
  }
  
  doc["activeRetries"] = activeRetries;

  // Serialize to gDebugBuffer
  if (!ensureDebugBuffer()) return "{\"error\":\"Buffer unavailable\"}";

  size_t len = serializeJson(doc, getDebugBuffer(), 1024);
  if (len >= 1024) return "{\"error\":\"Response too large\"}";

  return getDebugBuffer();
}

// Unpair device command
// ============================================================================
// Phase 2.8 — espnowmeshes CLI: manage the multi-mesh data model
// ============================================================================
//
// Subcommands:
//   espnowmeshes                                       — list all configured meshes
//   espnowmeshes add <label> <passphrase>              — add a new mesh
//   espnowmeshes remove <label>                        — disable a mesh (does NOT unpair peers)
//   espnowmeshes setdefault <label>                    — set the default mesh
//   espnowmeshes setpassphrase <label> <passphrase>    — change a mesh's passphrase
//   espnowmeshes rename <oldlabel> <newlabel>          — rename (changes fingerprint!)
//
// Mesh slot 0 ("primary") is special: it's bootstrapped at boot and holds
// the primary mesh passphrase. Removing or renaming it is allowed but breaks
// comms with existing peers until they too rename.

static bool isValidMeshLabel(const String& label) {
  if (label.length() == 0 || label.length() > 16) return false;
  // ASCII printable, no whitespace, no special-meaning characters
  for (size_t i = 0; i < label.length(); i++) {
    char c = label[i];
    if (c < 0x21 || c > 0x7E) return false;  // printable, no space
    if (c == '"' || c == '\\' || c == '/' || c == '=') return false;
  }
  // Reserved labels — avoid colliding with system/audit log conventions
  if (label == "system" || label == "internal" || label == "all" ||
      label == "default" || label == "none") return false;
  return true;
}

static int findFreeMeshSlot() {
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (!gSettings.meshes[i].enabled && gSettings.meshes[i].label.length() == 0) {
      return (int)i;
    }
  }
  return -1;
}

static const char* meshesCmd_list() {
  // Emit one broadcastOutput per line — a single returned blob is capped at
  // DEBUG_MSG_SIZE (256 bytes) by the debug task and would truncate mid-line
  // once 3+ meshes are configured. See System_Debug.cpp's cmd_debugqueue pattern.
  BROADCAST_PRINTF("=== Configured meshes (N_MESHES=%d) ===", (int)Settings::N_MESHES);
  int configured = 0;
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    const Settings::MeshIdentity& m = gSettings.meshes[i];
    if (m.label.length() == 0 && !m.enabled) continue;
    configured++;
    BROADCAST_PRINTF("  [%u] %-16s  %s  fp=0x%04X%s%s",
                     (unsigned)i,
                     m.label.length() ? m.label.c_str() : "(empty)",
                     m.enabled ? "enabled" : "disabled",
                     (unsigned)m.fingerprint,
                     m.isDefault ? "  (default)" : "",
                     m.passphrase.length() ? "  passphrase set" : "");
  }
  if (configured == 0) {
    broadcastOutput("  (none - run 'espnowmeshes add <label>')");
  }
  return "OK";
}

// JSON variant of meshesCmd_list — feeds the web UI multi-mesh table.
// Returned shape:
//   {
//     "nMeshes": 4,
//     "configuredCount": 3,
//     "defaultSlot": 0,
//     "meshes": [
//       { "slot": 0, "label": "primary", "enabled": true,
//         "fingerprint": 18265, "fingerprintHex": "0x4759",
//         "isDefault": true, "hasPassphrase": true },
//       ...
//     ]
//   }
// Only configured slots (label != "" or enabled) are emitted, but the
// caller can compute free slots = nMeshes - configuredCount.
static const char* meshesCmd_listjson() {
  PSRAM_JSON_DOC(doc);
  doc["nMeshes"] = (int)Settings::N_MESHES;
  int configured = 0;
  int defaultSlot = -1;
  JsonArray arr = doc["meshes"].to<JsonArray>();
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    const Settings::MeshIdentity& m = gSettings.meshes[i];
    if (m.label.length() == 0 && !m.enabled) continue;
    JsonObject o = arr.add<JsonObject>();
    o["slot"]          = (int)i;
    o["label"]         = m.label;
    o["enabled"]       = m.enabled;
    o["fingerprint"]   = (int)m.fingerprint;
    char fpHex[8];
    snprintf(fpHex, sizeof(fpHex), "0x%04X", (unsigned)m.fingerprint);
    // Wrap in String() — passing a raw stack-local char* causes ArduinoJson
    // to store by reference (zero-copy). The buffer's lifetime is just this
    // loop iteration, so subsequent iterations reuse the stack slot and the
    // earlier entry's pointer dangles. Same pattern as the task #14 fix.
    o["fingerprintHex"] = String(fpHex);
    o["isDefault"]     = m.isDefault;
    o["hasPassphrase"] = (m.passphrase.length() > 0);
    if (m.isDefault && m.enabled) defaultSlot = (int)i;
    configured++;
  }
  doc["configuredCount"] = configured;
  doc["defaultSlot"]     = defaultSlot;

  // Use a static buffer so this command works pre-init (meshes are settings,
  // not runtime state — the user may want to view/configure them before
  // running 'openespnow'). gEspNow->listBuffer would be null pre-init.
  // 1 KB is plenty for 4 meshes (~500 bytes serialized).
  EXT_RAM_BSS_ATTR static char meshesJsonBuf[1024];
  size_t needed = measureJson(doc) + 1;
  if (needed > sizeof(meshesJsonBuf)) needed = sizeof(meshesJsonBuf);
  serializeJson(doc, meshesJsonBuf, needed);
  return meshesJsonBuf;
}

static const char* meshesCmd_add(const String& label) {
  if (!isValidMeshLabel(label)) {
    return "Error: Invalid label. Use 1-16 printable ASCII chars, no whitespace/quotes/slash. "
           "Reserved labels: system, internal, all, default, none.";
  }
  // Already exists?
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].label == label) {
      if (gSettings.meshes[i].enabled) {
        return "Error: Mesh with that label already exists. Use 'espnowsetpassphrase' to change its passphrase.";
      } else {
        return "Error: Mesh with that label exists but is disabled. Use 'espnowmeshes enable <label>' to re-enable.";
      }
    }
  }
  // Fingerprint collision check (16-bit hash; vanishingly unlikely but cheap to verify)
  uint16_t fp = meshFingerprintForLabel(label);
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].enabled && gSettings.meshes[i].fingerprint == fp) {
      return "Error: Label fingerprint collides with an existing mesh. Pick a different label.";
    }
  }
  int slot = findFreeMeshSlot();
  if (slot < 0) {
    return "Error: All mesh slots full. Remove an existing mesh first.";
  }
  setSetting(gSettings.meshes[slot].label, label);
  setSetting(gSettings.meshes[slot].passphrase, String(""));
  gSettings.meshes[slot].fingerprint = fp;
  setSetting(gSettings.meshes[slot].enabled, true);
  // First mesh added becomes default if no other is flagged
  bool anyDefault = false;
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].enabled && gSettings.meshes[i].isDefault) { anyDefault = true; break; }
  }
  if (!anyDefault) setSetting(gSettings.meshes[slot].isDefault, true);
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  snprintf(getDebugBuffer(), 1024,
           "Added mesh '%s' in slot %d (fp=0x%04X)%s. "
           "No passphrase set yet — run 'espnowsetpassphrase %s <passphrase>' to enable encryption.",
           label.c_str(), slot, (unsigned)fp,
           gSettings.meshes[slot].isDefault ? " — set as default" : "",
           label.c_str());
  return getDebugBuffer();
}

static const char* meshesCmd_remove(const String& label) {
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].label == label) {
      if (gSettings.meshes[i].isDefault) {
        return "Error: Cannot remove the default mesh. Use 'espnowmeshes setdefault <other>' first.";
      }
      setSetting(gSettings.meshes[i].enabled, false);
      // Don't clear label — preserves it for future re-enable. To fully
      // delete, use rename to "" or wipe via 'espnowmeshes wipe' (future cmd).
      if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
      snprintf(getDebugBuffer(), 1024,
               "Disabled mesh '%s'. Paired peers in this mesh remain in their slots "
               "but their frames will be silently dropped. Re-enable with "
               "'espnowmeshes enable %s' or fully unpair with 'espnowunpair'.",
               label.c_str(), label.c_str());
      return getDebugBuffer();
    }
  }
  return "Error: Mesh not found. Run 'espnowmeshes' to list.";
}

static const char* meshesCmd_enable(const String& label) {
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].label == label) {
      if (gSettings.meshes[i].enabled) {
        return "Mesh is already enabled.";
      }
      // Phase 3.x: passphrase is no longer required to enable a mesh.
      // Reason: the UI exposes "Set passphrase" only on ENABLED meshes, so
      // requiring a passphrase before enable creates a chicken-and-egg
      // (user can't reach the passphrase field). A mesh without a passphrase
      // can still do per-peer encrypted sessions (KEY_EX + Ed25519 + X25519);
      // it just can't do BROADCAST_AUTH (no group HMAC tag on heartbeats) or
      // bootstrap KEY_EX-via-shared-secret. Set a passphrase later when ready.
      setSetting(gSettings.meshes[i].enabled, true);
      // Re-stamp fingerprint defensively in case the label was mutated
      // while disabled (rename doesn't refuse renaming disabled slots).
      gSettings.meshes[i].fingerprint = meshFingerprintForLabel(label);
      if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
      snprintf(getDebugBuffer(), 1024,
               "Enabled mesh '%s' (fp=0x%04X). Paired peers in this mesh "
               "will start exchanging frames again.",
               label.c_str(), (unsigned)gSettings.meshes[i].fingerprint);
      return getDebugBuffer();
    }
  }
  return "Error: Mesh not found. Run 'espnowmeshes' to list.";
}

static const char* meshesCmd_setdefault(const String& label) {
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].label == label) {
      if (!gSettings.meshes[i].enabled) {
        return "Error: Cannot set a disabled mesh as default. Enable it first.";
      }
      // Clear isDefault on all, then set on target
      for (uint8_t j = 0; j < Settings::N_MESHES; j++) {
        setSetting(gSettings.meshes[j].isDefault, j == i);
      }
      if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
      snprintf(getDebugBuffer(), 1024, "Set '%s' as default mesh.", label.c_str());
      return getDebugBuffer();
    }
  }
  return "Error: Mesh not found.";
}

// Set/clear the passphrase on a mesh by label. Pass an empty passphrase
// to clear. For slot 0 (the legacy primary mesh) this routes through
// setEspNowPassphrase() so gEspNow->derivedKey is re-derived immediately —
// otherwise the new passphrase wouldn't take effect for encrypted pairings
// until reboot.
static const char* meshesCmd_setpassphrase(const String& label, const String& passphrase) {
  // Length check — but allow empty as a "clear" sentinel.
  if (passphrase.length() > 0 && passphrase.length() < 8) {
    return "Error: Passphrase must be at least 8 characters (or empty to clear).";
  }
  if (passphrase.length() > 128) {
    return "Error: Passphrase must be 128 characters or less.";
  }
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].label == label) {
      if (i == 0) {
        // Route slot 0 through the canonical funnel: updates the legacy
        // field, re-derives gEspNow->derivedKey, and re-stamps mesh
        // metadata. Without this, encrypted pairings would keep using
        // the OLD derivedKey until reboot.
        setEspNowPassphrase(passphrase);
      } else {
        setSetting(gSettings.meshes[i].passphrase, passphrase);
        gSettings.meshes[i].fingerprint = meshFingerprintForLabel(label);
        // Non-zero slots don't have their own derived key yet — that lands
        // in Phase 3 (Signed Ephemeral DH). Until then the stored passphrase
        // is just persisted metadata, not used for runtime encryption.
      }
      // Phase 3.1: passphrase changed → cached stretched hash is stale.
      // Invalidate first (so callers can't accidentally use a stale subkey),
      // then re-stretch and re-derive synchronously. PBKDF2 stretching is
      // ~12 s with HW SHA accel — submitSync's 60 s timeout covers it.
      meshKeysInvalidate(i);
      gSettings.meshes[i].passphraseStretchedKeyValid = false;
      if (passphrase.length() > 0) {
        meshKeysStretchPassphrase(i);
        meshKeysDerive(i);
      }
      if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
      if (passphrase.length() == 0) {
        snprintf(getDebugBuffer(), 1024,
                 "Cleared passphrase for mesh '%s'.%s", label.c_str(),
                 i == 0 ? " ESP-NOW encryption disabled for primary mesh." : "");
      } else {
        snprintf(getDebugBuffer(), 1024,
                 "Updated passphrase for mesh '%s'.%s "
                 "Existing paired peers will need to re-pair if they were "
                 "using the old passphrase for encryption.",
                 label.c_str(),
                 i == 0 ? " Key re-derived." : " (Not used for current encryption.)");
      }
      return getDebugBuffer();
    }
  }
  return "Error: Mesh not found. Run 'espnowmeshes add <label>' first.";
}

static const char* meshesCmd_rename(const String& oldLabel, const String& newLabel) {
  if (!isValidMeshLabel(newLabel)) {
    return "Error: Invalid new label. See 'espnowmeshes add' for label rules.";
  }
  // New label must not collide
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].label == newLabel) {
      return "Error: Another mesh already has that label.";
    }
  }
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].label == oldLabel) {
      setSetting(gSettings.meshes[i].label, newLabel);
      gSettings.meshes[i].fingerprint = meshFingerprintForLabel(newLabel);
      // Phase 3.1: salt = SHA256("...salt:" || label). Rename → new salt →
      // cached hash is wrong for the new label. Recompute now if we have a
      // passphrase to stretch from.
      meshKeysInvalidate(i);
      gSettings.meshes[i].passphraseStretchedKeyValid = false;
      if (gSettings.meshes[i].passphrase.length() > 0) {
        meshKeysStretchPassphrase(i);
        meshKeysDerive(i);
      }
      if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
      snprintf(getDebugBuffer(), 1024,
               "Renamed mesh '%s' -> '%s' (new fp=0x%04X). WARNING: peers in this mesh "
               "must also rename or comms will break — the fingerprint changed.",
               oldLabel.c_str(), newLabel.c_str(),
               (unsigned)gSettings.meshes[i].fingerprint);
      return getDebugBuffer();
    }
  }
  return "Error: Mesh not found.";
}

const char* cmd_espnow_meshes(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);

  if (a.count() == 0) return meshesCmd_list();

  String sub = a.arg(0);
  sub.toLowerCase();

  if (sub == "list")     return meshesCmd_list();
  if (sub == "listjson") return meshesCmd_listjson();
  if (sub == "add") {
    if (!a.hasMinArgs(2)) return "Error: invalid arguments — Usage: espnowmeshes add <label>  (run 'espnowsetpassphrase <label> <pw>' after to enable encryption)";
    return meshesCmd_add(a.arg(1));
  }
  if (sub == "remove" || sub == "disable") {
    if (!a.hasMinArgs(2)) return "Error: invalid arguments — Usage: espnowmeshes remove <label>";
    return meshesCmd_remove(a.arg(1));
  }
  if (sub == "enable") {
    if (!a.hasMinArgs(2)) return "Error: invalid arguments — Usage: espnowmeshes enable <label>";
    return meshesCmd_enable(a.arg(1));
  }
  if (sub == "setdefault") {
    if (!a.hasMinArgs(2)) return "Error: invalid arguments — Usage: espnowmeshes setdefault <label>";
    return meshesCmd_setdefault(a.arg(1));
  }
  if (sub == "setpassphrase") {
    if (!a.hasMinArgs(3)) return "Error: invalid arguments — Usage: espnowmeshes setpassphrase <label> <passphrase>";
    return meshesCmd_setpassphrase(a.arg(1), a.arg(2));
  }
  if (sub == "rename") {
    if (!a.hasMinArgs(3)) return "Error: invalid arguments — Usage: espnowmeshes rename <oldlabel> <newlabel>";
    return meshesCmd_rename(a.arg(1), a.arg(2));
  }
  return "Error: invalid arguments — Usage: espnowmeshes [list|add|remove|enable|setdefault|setpassphrase|rename] ...";
}

// Tear down the cryptographic relationship for a peer: close any live AEAD
// session and delete the stored KEY_EX (Ed25519) identity at
// /system/espnow/peers/<MAC>/identity.json. Returns true if a stored identity
// was present (vs. just sweeping a stale file). Safe to call for an unknown
// peer. Shared by espnowunpair (full teardown) and espnowforget.
static bool espnowForgetPeerCrypto(const uint8_t mac[6]) {
  const PeerIdentity* pid = peerIdentityFindByMac(mac);
  bool had = (pid != nullptr);
  if (pid) {
    SessionState* s = sessionFindByPeer(mac, pid->meshId);
    if (s) sessionClear(s);  // drop ephemeral AEAD keys for this peer
  }
  extern bool peerIdentityForget(const uint8_t mac[6]);
  peerIdentityForget(mac);
  return had;
}

const char* cmd_espnow_unpair(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "Error: ESP-NOW not initialized. Run 'openespnow' first.";
  }

  CommandArgs a(argsInput);
  if (!a.hasMinArgs(1)) return "Error: invalid arguments — Usage: espnowunpair <name_or_mac>";
  String target = a.arg(0);

  uint8_t mac[6];
  if (!resolveDeviceNameOrMac(target, mac)) {
    EXT_RAM_BSS_ATTR static char errBuf[256];
    snprintf(errBuf, sizeof(errBuf), 
             "Error: Device '%s' not found. Use 'espnowdevices' to see paired devices.", 
             target.c_str());
    return errBuf;
  }

  String deviceName = getEspNowDeviceName(mac);

  esp_err_t result = esp_now_del_peer(mac);
  if (result != ESP_OK) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024, "Error: Failed to unpair device: %d", result);
    return getDebugBuffer();
  }

  removeEspNowDevice(mac);

  // Full teardown: also drop the crypto identity + any live session so a later
  // re-pair starts clean (no stale-key pub-conflict). De-bond story:
  // bonddisconnect (bond) → espnowunpair (pairing + crypto identity).
  espnowForgetPeerCrypto(mac);

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

  bool peersOk   = saveMeshPeers();
  bool devicesOk = saveEspNowDevices();

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  if (!peersOk || !devicesOk) {
    snprintf(getDebugBuffer(), 1024,
      "Unpaired device but failed to encrypt and save peer data — device encryption key unavailable. "
      "Peer list may not persist correctly across reboot.");
    return getDebugBuffer();
  }
  if (deviceName.length() > 0) {
    snprintf(getDebugBuffer(), 1024, "Unpaired device: %s (%s)",
             deviceName.c_str(), formatMacAddress(mac).c_str());
  } else {
    snprintf(getDebugBuffer(), 1024, "Unpaired device: %s",
             formatMacAddress(mac).c_str());
  }
  return getDebugBuffer();
}

// Forget a peer's crypto identity (KEY_EX Ed25519 pubkey) and close any live
// session, WITHOUT touching the device-registry pairing. This is the command
// the KEY_EX conflict warnings tell the operator to run ("presented new pubkey
// — run 'espnowforget'"). Accepts a raw MAC even for an already-unpaired peer
// (identities persist independently of the device registry).
const char* cmd_espnow_forget(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);
  if (!a.hasMinArgs(1)) return "Error: invalid arguments — Usage: espnowforget <name_or_mac>";
  String target = a.arg(0);

  uint8_t mac[6];
  // Try a raw MAC first (works even if the peer is no longer in the registry),
  // then fall back to name resolution.
  if (!parseMacAddress(target, mac) && !resolveDeviceNameOrMac(target, mac)) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024,
             "Could not resolve '%s'. Pass a MAC (AA:BB:CC:DD:EE:FF) or a paired device name.",
             target.c_str());
    return getDebugBuffer();
  }

  bool had = espnowForgetPeerCrypto(mac);

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  snprintf(getDebugBuffer(), 1024,
           "%s %s. Re-pair with 'espnowpairsecure %s <name>' on BOTH devices.",
           had ? "Forgot crypto identity + closed session for"
               : "No stored identity (swept any stale file) for",
           formatMacAddress(mac).c_str(), formatMacAddress(mac).c_str());
  return getDebugBuffer();
}

// ============================================================================
// ESP-NOW Messaging Commands
// ============================================================================

// Broadcast command
const char* cmd_espnow_broadcast(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "Error: ESP-NOW not initialized. Run 'openespnow' first.";
  }

  CommandArgs a(argsInput);
  if (!a.hasMinArgs(1)) return "Error: invalid arguments — Usage: espnow broadcast <message>";
  String message = a.raw();

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
  bool result = v4_broadcast_text(payload.c_str(), payload.length());
  int sent = result ? 1 : 0;
  int failed = result ? 0 : 1;

  if (sent == 0 && failed == 0) {
    return "Error: No paired devices to broadcast to";
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
// RAII flag set around an outbound file send so the rekey scheduler can see it
// (the slot table only tracks INBOUND transfers). Peer + start-time are published
// BEFORE the flag so a concurrent reader that sees the flag sees a valid peer.
// Consumed by espnowFileTransferActiveWithPeer (defined up by the rekey trigger).
struct FileSendActiveGuard {
  explicit FileSendActiveGuard(const uint8_t* mac) {
    if (gEspNow && mac) {
      memcpy(gEspNow->fileSendPeer, mac, 6);
      gEspNow->fileSendStartedMs = (uint32_t)millis();
      gEspNow->fileSendInProgress = true;
    }
  }
  ~FileSendActiveGuard() { if (gEspNow) gEspNow->fileSendInProgress = false; }
  FileSendActiveGuard(const FileSendActiveGuard&) = delete;
  FileSendActiveGuard& operator=(const FileSendActiveGuard&) = delete;
};

bool sendFileToMac(const uint8_t* mac, const String& localPath) {
  if (!gEspNow || !gEspNow->initialized) {
    return false;
  }

  // ── Per-chunk routing through smart ───────────────────────────────────────
  // 2026-05 (encrypt-or-wait foundation): each FILE_DATA chunk is small enough
  // for a single SESSION_FRAME (200 data + 2 chunkIdx + 16 AEAD tag = 218 =
  // MAX_PAYLOAD). Smart's single-frame path goes through v4_send_encrypted_or_queue
  // which now auto-handles every "no session / no identity" case (queue-and-kick
  // KEY_EX/SESSION_OPEN, drain via SESSION_CONFIRM). So we just call smart per
  // chunk and let it figure out encryption + session state per frame. Replaces
  // the old "fileSessionReady snapshot at start of transfer" wart, which got
  // the choice wrong if the session came up partway through.
  //
  // Hint: kick session establishment up-front (no-op if already in flight) so
  // the first FILE_START doesn't pay the full handshake RTT in its own queue
  // wait. Cheap.
  {
    const PeerIdentity* pid = peerIdentityFindByMac(mac);
    if (pid) {
      SessionState* s = sessionFindByPeer(mac, pid->meshId);
      if (!s || s->state != SESSION_ACTIVE) {
        espnowSessionOpenInitiate(mac, nullptr);
      }
    }
    // If !pid, smart's _or_queue will auto-kick KEY_EX on the first frame.
  }

  auto fileSendFrame = [&](uint8_t type, uint16_t flags, uint32_t id,
                           const uint8_t* p, uint16_t len) -> bool {
    return v4_send_payload_smart(mac, type, flags, id, p, len, 1);
  };

  // Security: Block sending sensitive files (credentials, passwords, keys).
  // Uses currentAuthContext() so CLI/web callers see the same allow/deny
  // they would on a direct read; internal bond/USER_SYNC callers run
  // through the same gate using whatever identity their task installed.
  if (!canRead(localPath, currentAuthContext())) {
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FILE_TX] SECURITY: Blocked sending sensitive file: %s", localPath.c_str());
    broadcastOutput("[ESP-NOW] SECURITY: Cannot send file containing credentials: " + localPath);
    return false;
  }

  {
    FsLockGuard guard("espnow.send_file.exists");
    if (!VFS::existsGuarded(localPath, currentAuthContext())) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FILE_TX] File not found: %s", localPath.c_str());
      return false;
    }
  }

  // Open + size under the FS lock, then RELEASE it. The send loop below runs for
  // many seconds (paced 15-50 ms/chunk); holding the FS lock across the whole
  // transfer (as this used to) starves every other FS user — web, OLED, a concurrent
  // inbound streaming write — for its entire duration. Re-take the lock only briefly
  // around each chunk read instead. The File handle stays valid across the gaps (it's
  // a private local; nothing else touches it).
  File file;
  uint32_t fileSize = 0;
  {
    FsLockGuard guard("espnow.send_file.open");
    file = VFS::openGuarded(localPath, "r", currentAuthContext());
    if (!file) {
      DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FILE_TX] Cannot open file: %s", localPath.c_str());
      return false;
    }
    fileSize = file.size();
  }
  
  // Phase 3.5: chunk size aligned to ESPNOW_V4_MAX_PLAINTEXT so that the
  // V4PayloadFileData.data[] declared size (200) matches what we put on the
  // wire. Old code used MAX_PAYLOAD - 2 = 216, which only "worked" because
  // the surrounding stack buffer was MAX_PAYLOAD-sized and we wrote past
  // the struct's declared bounds (UB in C++). The 200 + 2 (chunkIndex) =
  // 202 bytes plaintext fits ESPNOW_V4_MAX_PLAINTEXT, so once wrapped in
  // SESSION_FRAME each chunk's 202 + 16 (AEAD tag) = 218 hits the wire
  // payload budget exactly. 2026-05-19: FILE_START/DATA/END now go through
  // v4_send_payload_smart (encrypted single-frame when a session is up,
  // plaintext fallback otherwise) — the task #51-era "ready to flip" plan.
  const uint16_t v4ChunkSize = ESPNOW_V4_MAX_PLAINTEXT - 2;  // 200 bytes
  uint32_t maxFileSize = 65535 * v4ChunkSize;  // 16-bit chunk count max
  if (fileSize > maxFileSize) {
    { FsLockGuard guard("espnow.send_file.close"); file.close(); }
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FILE_TX] File too large: %lu bytes (max %lu)",
           (unsigned long)fileSize, (unsigned long)maxFileSize);
    return false;
  }
  
  String filename = localPath;
  int lastSlash = localPath.lastIndexOf('/');
  if (lastSlash >= 0) {
    filename = localPath.substring(lastSlash + 1);
  }
  
  uint16_t totalChunks = (fileSize > 0) ? (uint16_t)((fileSize + v4ChunkSize - 1) / v4ChunkSize) : 0;
  
  uint32_t transferId = generateMessageId();

  // Mark this peer as "send in progress" for the whole START→DATA→END window so
  // the rekey scheduler defers a key rotation until we're done (a big send is what
  // crosses the 10k tx threshold). Cleared on every exit path below via the dtor.
  FileSendActiveGuard sendGuard(mac);

  // Build and send FILE_START
  V4PayloadFileStart startPayload = {};
  startPayload.fileSize = fileSize;
  startPayload.chunkCount = totalChunks;
  startPayload.chunkSize = v4ChunkSize;
  strncpy(startPayload.filename, filename.c_str(), sizeof(startPayload.filename) - 1);
  
  // Session was ensured up-front (see fileSessionReady); fileSendFrame picks
  // encrypted fast-path vs plaintext chunked accordingly — no queue/evict.
  if (!fileSendFrame(ESPNOW_V4_TYPE_FILE_START, ESPNOW_V4_FLAG_ACK_REQ, transferId,
                     (const uint8_t*)&startPayload, sizeof(startPayload))) {
    { FsLockGuard guard("espnow.send_file.close"); file.close(); }
    DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FILE_TX] Failed to send FILE_START");
    return false;
  }
  
  DEBUG_ESPNOWF("[V4_FILE_TX] START: %s (%lu bytes, %u chunks, chunkSize=%u) to %s, transferId=%lu",
         filename.c_str(), (unsigned long)fileSize, totalChunks, v4ChunkSize, formatMacAddress(mac).c_str(), (unsigned long)transferId);
  DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FILE_TX] Transfer initiated: %s -> %s", filename.c_str(), formatMacAddress(mac).c_str());
  
  vTaskDelay(pdMS_TO_TICKS(100));  // Give receiver more time to set up
  
  // Send chunks - use stack buffer sized for v3 payload
  uint8_t chunkPayload[ESPNOW_V4_MAX_PAYLOAD];
  uint16_t chunkIdx = 0;
  while (chunkIdx < totalChunks) {
    V4PayloadFileData* fd = (V4PayloadFileData*)chunkPayload;
    fd->chunkIndex = chunkIdx;

    // Re-take the FS lock only for the read itself (not across the send/pacing
    // delays below), so other tasks can use the filesystem between chunks.
    int bytesRead;
    {
      FsLockGuard guard("espnow.send_file.read");
      bytesRead = file.read(fd->data, v4ChunkSize);
    }
    if (bytesRead <= 0) break;
    
    uint16_t payloadLen = 2 + bytesRead;  // chunkIndex (2) + data
    
    bool sent = false;
    for (int attempt = 0; attempt < 3 && !sent; attempt++) {
      // Each chunk: 200 data + 2 chunkIdx = 202 plaintext, fits SESSION_FRAME
      // single-frame (202 + 16 tag = 218 = MAX_PAYLOAD) when encrypted.
      sent = fileSendFrame(ESPNOW_V4_TYPE_FILE_DATA, 0, transferId,
                           chunkPayload, payloadLen);
      if (!sent) {
        DEBUGF(DEBUG_ESPNOW_ROUTER, "[V4_FILE_TX] Chunk %u send failed, retry %d", chunkIdx, attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(20));
      } else {
        DEBUG_ESPNOWF("[V4_FILE_TX] Chunk %u sent: %u bytes (attempt %d)", chunkIdx, payloadLen, attempt + 1);
      }
    }
    
    if (!sent) {
      ERROR_ESPNOWF("[V4_FILE_TX] Chunk %u failed after 3 retries", chunkIdx);
    }
    
    chunkIdx++;
    
    // Pace chunks - SLOWER for reliability (ESP-NOW can drop packets if sent too fast)
    vTaskDelay(pdMS_TO_TICKS(15));
    
    // Yield every 10 chunks with longer delay for receiver to process
    if ((chunkIdx % 10) == 0) {
      DEBUG_ESPNOWF("[V4_FILE_TX] Progress: %u/%u chunks sent", chunkIdx, totalChunks);
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
  { FsLockGuard guard("espnow.send_file.close"); file.close(); }

  // Small delay before FILE_END to ensure last chunks are processed
  vTaskDelay(pdMS_TO_TICKS(100));
  
  // Send FILE_END with retries for reliability
  V4PayloadFileEnd endPayload = {};
  endPayload.crc32 = 0;  // CRC not implemented yet
  endPayload.success = 1;
  
  bool endSent = false;
  for (int attempt = 0; attempt < 3 && !endSent; attempt++) {
    endSent = fileSendFrame(ESPNOW_V4_TYPE_FILE_END, ESPNOW_V4_FLAG_ACK_REQ, transferId,
                            (const uint8_t*)&endPayload, sizeof(endPayload));
    if (!endSent) {
      WARN_ESPNOWF("[V4_FILE_TX] FILE_END send failed, retry %d", attempt + 1);
      vTaskDelay(pdMS_TO_TICKS(50));
    }
  }
  
  DEBUG_ESPNOWF("[V4_FILE_TX] COMPLETE: %s (%u chunks) to %s, END_sent=%d", 
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
  if (isBondMaster()) {
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
  if (isBondMaster()) return false;
  
  // Get peer MAC
  uint8_t peerMac[6];
  if (!parseMacAddress(gSettings.bondPeerMac, peerMac)) return false;
  
  // Build payload: header + data
  // V4PayloadSensorData is 8 bytes header, max payload is 226, so max data is 218 bytes
  const uint16_t maxDataLen = ESPNOW_V4_MAX_PAYLOAD - sizeof(V4PayloadSensorData);
  if (dataLen > maxDataLen) {
    DEBUGF(DEBUG_ESPNOW_MESH, "[V4_SENSOR_TX] Data too large: %u > %u", dataLen, maxDataLen);
    return false;
  }
  
  // Allocate payload on stack
  uint8_t payloadBuf[ESPNOW_V4_MAX_PAYLOAD];
  V4PayloadSensorData* sd = (V4PayloadSensorData*)payloadBuf;
  sd->sensorType = sensorType;
  sd->flags = 0x01;  // Valid flag
  sd->dataLen = dataLen;
  sd->seqNum = ++gBondSensorSeqNum;
  
  if (data && dataLen > 0) {
    memcpy(sd->data, data, dataLen);
  }
  
  uint16_t totalLen = sizeof(V4PayloadSensorData) + dataLen;
  uint32_t msgId = generateMessageId();

  // Async send: enqueue to espnow_tx. The AEAD seal + frame buffer + capture
  // + esp_now_send all happen on espnow_tx's stack, not sensor_bcast's.
  // `sent` here means "queued OK," not "made it to the air" — the V4 ACK
  // mechanism is the delivery confirmation (same as before).
  bool sent = bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_SENSOR_DATA, 0, msgId, payloadBuf, totalLen);

  // Single concise debug line only on success/failure
  DEBUGF(DEBUG_ESPNOW_MESH, "[BOND] Sensor TX type=%u len=%u seq=%lu %s",
         sensorType, dataLen, (unsigned long)sd->seqNum, sent ? "QUEUED" : "DROP");

  return sent;
}

// Send stream control message to bonded peer (master -> worker)
bool sendBondStreamCtrl(RemoteSensorType sensorType, bool enable) {
  if (!isBondSynced()) return false;
  
  // Only master sends stream control to worker
  if (!isBondMaster()) return false;
  
  uint8_t peerMac[6];
  if (!parseMacAddress(gSettings.bondPeerMac, peerMac)) return false;
  
  V4PayloadStreamCtrl ctrl = {};
  ctrl.sensorType = (uint8_t)sensorType;
  ctrl.enable = enable ? 1 : 0;
  
  uint32_t msgId = generateMessageId();
  // Step 2 migration: route through espnow_tx so the AEAD seal + esp_now_send
  // don't run on espnow_task (called from the super-loop "9f" streaming-setup
  // block AND from startSensorDataStreaming on cmd_exec). Master is always the
  // initiator here (gated by isBondMaster + isBondSynced), so async returns
  // true on queue; behavior matches the prior sync return in the happy path.
  bool sent = bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_STREAM_CTRL, ESPNOW_V4_FLAG_ACK_REQ,
                            msgId, (const uint8_t*)&ctrl, sizeof(ctrl));

  DEBUGF(DEBUG_ESPNOW_MESH, "[BOND_STREAM_CTRL] TX %s %s -> %s",
         sensorTypeToString(sensorType), enable ? "ON" : "OFF", sent ? "QUEUED" : "FAIL");
  return sent;
}

#endif // ENABLE_BONDED_MODE

// Sendfile command
const char* cmd_espnow_sendfile(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "Error: ESP-NOW not initialized. Run 'openespnow' first.";
  }

  CommandArgs a(argsInput);
  if (!a.hasMinArgs(2)) return "Error: invalid arguments — Usage: espnowsendfile <name_or_mac> \"<filepath>\"";

  String target = a.arg(0);
  String filepath;
  const char* qerr = requireQuotedToken(a, 1, filepath);
  if (qerr) return qerr;

  uint8_t mac[6];
  if (!resolveDeviceNameOrMac(target, mac)) {
    EXT_RAM_BSS_ATTR static char errBuf[256];
    snprintf(errBuf, sizeof(errBuf), 
             "Error: Device '%s' not found. Use 'espnowdevices' to see paired devices.", 
             target.c_str());
    return errBuf;
  }

  String deviceName = getEspNowDeviceName(mac);
  if (deviceName.length() == 0) {
    deviceName = formatMacAddress(mac);
  }

  EXT_RAM_BSS_ATTR static char sendfileBuffer[512];

  if (isMeshMode()) {
    if (!espnowPeerExists(mac)) {
      BROADCAST_PRINTF("[ESP-NOW][mesh] file send rejected: no peer entry MAC=%s", formatMacAddress(mac).c_str());
      snprintf(sendfileBuffer, sizeof(sendfileBuffer), "Error: Rejected (mesh): destination not in ESP-NOW peer table.");
      return sendfileBuffer;
    }
    BROADCAST_PRINTF("[ESP-NOW][mesh] file send accepted MAC=%s", formatMacAddress(mac).c_str());
  }

  if (!VFS::existsGuarded(filepath, currentAuthContext())) {
    snprintf(sendfileBuffer, sizeof(sendfileBuffer), "Error: File not found: %s", filepath.c_str());
    return sendfileBuffer;
  }

  // Get file info for reporting
  File file = VFS::openGuarded(filepath, "r", currentAuthContext());
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

  // Pre-check against the receiver's hard ceiling. Files <= 128 KB are buffered
  // whole in PSRAM on the receiver; larger files stream chunk-by-chunk straight
  // to flash (up to kFileSlotMaxStreamSize). Past that the receiver rejects at
  // FILE_START, and that rejection is NOT signaled back to the sender — so
  // without this check we'd stream the whole file and then falsely report
  // success. Fail fast here with the real reason so the command result (and the
  // relayed espnowfetch result the user actually sees) says WHY.
  if (fileSize > kFileSlotMaxStreamSize) {
    snprintf(sendfileBuffer, sizeof(sendfileBuffer),
             "Error: '%s' is %lu bytes — exceeds the %lu MB ESP-NOW file-transfer limit; not sent",
             filename.c_str(), (unsigned long)fileSize,
             (unsigned long)(kFileSlotMaxStreamSize / (1024UL * 1024UL)));
    return sendfileBuffer;
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
// Resolve a mesh label to its slot index. Returns -1 if not found.
static int meshSlotByLabel(const String& label) {
  for (uint8_t i = 0; i < Settings::N_MESHES; i++) {
    if (gSettings.meshes[i].label == label) return (int)i;
  }
  return -1;
}

const char* cmd_espnow_setpassphrase(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "Error: ESP-NOW not initialized. Run 'openespnow' first.";
  }

  // Required form: espnowsetpassphrase <mesh> <passphrase>
  //                espnowsetpassphrase <mesh> clear
  // No backward-compat for the single-arg form — the mesh name is
  // mandatory so the operator is always explicit about which slot
  // they're modifying.
  CommandArgs a(argsInput);
  if (a.count() < 2) {
    return "Error: invalid arguments — Usage: espnowsetpassphrase <mesh> <passphrase>\n"
           "       espnowsetpassphrase <mesh> clear\n"
           "Run 'espnowmeshes list' to see available mesh labels.";
  }

  String meshLabel = a.arg(0);
  String passphrase = a.arg(1);
  // Allow extra tokens to flow in (e.g. unquoted passphrases with spaces)
  for (uint8_t i = 2; i < a.count(); i++) {
    passphrase += " ";
    passphrase += a.arg(i);
  }

  bool clearing = (passphrase == "clear");
  if (clearing) passphrase = "";

  // Strip surrounding quotes if present
  if (passphrase.length() >= 2 &&
      passphrase.startsWith("\"") && passphrase.endsWith("\"")) {
    passphrase = passphrase.substring(1, passphrase.length() - 1);
  }

  // Resolve mesh
  int slot = meshSlotByLabel(meshLabel);
  if (slot < 0) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024,
             "Mesh '%s' not found. Run 'espnowmeshes add %s' first, "
             "or 'espnowmeshes list' to see configured meshes.",
             meshLabel.c_str(), meshLabel.c_str());
    return getDebugBuffer();
  }

  // Validate passphrase length (clearing bypasses)
  if (!clearing) {
    if (passphrase.length() < 8) {
      return "Error: Passphrase must be at least 8 characters long.";
    }
    if (passphrase.length() > 128) {
      return "Error: Passphrase must be 128 characters or less.";
    }
  }

  // Delegate to the canonical mesh-aware path (handles slot-0 key
  // re-derivation, non-zero slots get persisted metadata only).
  return meshesCmd_setpassphrase(meshLabel, passphrase);
}

// Encryption status command
const char* cmd_espnow_encstatus(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (argWantsJson(argsInput)) {
    if (!ensureDebugBuffer()) return "{\"schema\":1,\"ok\":false,\"error\":\"buffer\"}";
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    if (!gEspNow || !gEspNow->initialized) {
      doc["ok"]    = false;
      doc["error"] = "ESP-NOW not initialized";
    } else {
      doc["running"]   = true;
      doc["encrypted"] = gEspNow->encryptionEnabled;
      if (gEspNow->encryptionEnabled) {
        doc["passphraseSet"]    = gEspNow->passphrase.length() > 0;
        doc["passphraseLength"] = (int)gEspNow->passphrase.length();
        char fp[9];
        snprintf(fp, sizeof(fp), "%02X%02X%02X%02X",
                 gEspNow->derivedKey[0], gEspNow->derivedKey[1],
                 gEspNow->derivedKey[2], gEspNow->derivedKey[3]);
        doc["keyFingerprint"] = String(fp);
      }
    }
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }
  // Read-only status: see comment in cmd_espnow_status above.
  if (!gEspNow) return "Error: ESP-NOW not initialized (run 'openespnow' first)";
  if (!gEspNow->initialized) {
    return "Error: ESP-NOW not initialized. Run 'openespnow' first.";
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
      n = snprintf(p, remaining, "  Passphrase Length: %d\n", (int)gEspNow->passphrase.length());
      p += n;
      remaining -= n;
    }

    n = snprintf(p, remaining, "  Passphrase Fingerprint: ");
    p += n;
    remaining -= n;

    for (int i = 0; i < 4; i++) {
      n = snprintf(p, remaining, "%02X", gEspNow->derivedKey[i]);
      p += n;
      remaining -= n;
    }

    n = snprintf(p, remaining, "...\n"
                 "    (derived from the passphrase — an IDENTICAL value on both devices proves the\n"
                 "     same passphrase. Compare with 'espnowencstatus' on the peer. This is the\n"
                 "     passphrase check, not the mesh label.)\n");
    p += n;
    remaining -= n;
  }

  return getDebugBuffer();
}

// ── Pairing window (re-key authorization) ───────────────────────────────────
// A deliberate, local `espnowpairsecure` opens a short window during which the
// KEY_EX conflict guard MAY replace an existing identity for one specific peer
// MAC — this is how a peer whose key rotated (re-flash / re-pair) gets accepted
// without manually running `espnowforget`. It is intentionally narrow:
//   • one MAC at a time,           • ~15 s lifetime,
//   • opened only by a LOCAL operator command,
//   • single-use (consumed on the first matching KEY_EX),
//   • the KEY_EX itself STILL must pass the mesh-passphrase HMAC.
// Outside the window, an over-the-air KEY_EX presenting a *different* pubkey for
// a known peer is refused (anti-key-substitution / trust-on-first-use). This
// replaces the older unconditional peerIdentityForget(), which raced with and
// destroyed valid identities the peer's own proactive KEY_EX had just stored.
static uint8_t  gPairWindowMac[6]   = {0};
static uint32_t gPairWindowExpiryMs = 0;

void espnowOpenPairingWindow(const uint8_t mac[6]) {
  if (!mac) return;
  memcpy(gPairWindowMac, mac, 6);
  gPairWindowExpiryMs = (uint32_t)millis() + 15000;  // 15 s
}

// True (single-use) iff a re-key is currently authorized for this peer.
bool espnowConsumePairingWindow(const uint8_t mac[6]) {
  if (gPairWindowExpiryMs == 0 || !mac) return false;
  if ((int32_t)(gPairWindowExpiryMs - (uint32_t)millis()) <= 0) {
    gPairWindowExpiryMs = 0;  // expired
    return false;
  }
  if (memcmp(gPairWindowMac, mac, 6) != 0) return false;
  gPairWindowExpiryMs = 0;  // consume — one re-key per pairsecure
  return true;
}

// Secure pairing command
const char* cmd_espnow_pairsecure(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "Error: ESP-NOW not initialized. Run 'openespnow' first.";
  }

  if (!gEspNow->encryptionEnabled) {
    return "Error: Encryption not enabled. Run 'espnowsetpassphrase <mesh> <passphrase>' first.";
  }

  CommandArgs a(argsInput);
  if (!a.hasMinArgs(2)) {
    return "Error: invalid arguments — Usage: espnowpairsecure <mac_address> <device_name> [mesh]";
  }

  String macStr = a.arg(0);
  // Phase 2.5: name is now a single token (was: a.remaining(0)). Optional
  // 3rd arg is mesh label or numeric index. Use underscores in names if
  // you previously relied on multi-word peer names.
  String deviceName = a.arg(1);
  uint8_t meshId = parseMeshArgOrDefault(a.arg(2));
  if (meshId == Settings::N_MESHES) {
    return "Error: Invalid mesh. Use a configured label or numeric index 0..3.";
  }
  if (meshId == 0xFE) meshId = 0;

  uint8_t mac[6];
  if (!parseMacAddress(macStr, mac)) {
    return "Error: Invalid MAC address format. Use AA:BB:CC:DD:EE:FF";
  }
  // Prevent pairing with self MAC (STA or AP interface)
  {
    uint8_t selfSta[6];
    uint8_t selfAp[6];
    esp_wifi_get_mac(WIFI_IF_STA, selfSta);
    esp_wifi_get_mac(WIFI_IF_AP, selfAp);
    if (memcmp(mac, selfSta, 6) == 0 || memcmp(mac, selfAp, 6) == 0) {
      return "Error: Cannot pair with self MAC address.";
    }
  }

  // Is this peer already in the device registry? (e.g. it auto-appeared as an
  // "unknown" device because the *other* end paired first and its proactive
  // KEY_EX/heartbeats reached us.) If so, don't error with "unpair first" —
  // just re-attach the name/mesh in place. Otherwise append a new entry.
  int existingIdx = -1;
  for (int i = 0; i < gEspNow->deviceCount; i++) {
    if (memcmp(gEspNow->devices[i].mac, mac, 6) == 0) { existingIdx = i; break; }
  }
  const bool wasUpdate = (existingIdx >= 0);

  if (wasUpdate) {
    // Rename / refresh in place. The radio peer already exists from the prior
    // add, so we don't re-add it (would just return ESPNOW_EXIST).
    gEspNow->devices[existingIdx].name = deviceName;
    gEspNow->devices[existingIdx].encrypted = true;
    memcpy(gEspNow->devices[existingIdx].key, gEspNow->derivedKey, 16);
    gEspNow->devices[existingIdx].meshId = meshId;
  } else {
    if (gEspNow->deviceCount >= 16) {
      return "Error: Maximum number of devices (16) already paired.";
    }
    if (!addEspNowPeerWithEncryption(mac, true, gEspNow->derivedKey)) {
      return "Error: Failed to add encrypted peer to ESP-NOW.";
    }
    memcpy(gEspNow->devices[gEspNow->deviceCount].mac, mac, 6);
    gEspNow->devices[gEspNow->deviceCount].name = deviceName;
    gEspNow->devices[gEspNow->deviceCount].encrypted = true;
    memcpy(gEspNow->devices[gEspNow->deviceCount].key, gEspNow->derivedKey, 16);
    gEspNow->devices[gEspNow->deviceCount].meshId = meshId;  // Phase 2.5
    gEspNow->deviceCount++;
  }

  removeFromUnpairedList(mac);

  // Seed gMeshPeers immediately so v4_broadcast reaches this peer on the next
  // heartbeat tick (without this, two freshly-paired devices never exchange
  // heartbeats until one reboots, so mesh topology stays empty)
  if (meshEnabled()) {
    noteMeshPeerRxActivity(mac, EspNowMeshRxKind::BootstrapLiveness);
    V4PayloadHeartbeat hb = {};
    hb.role = gSettings.meshRole;
    hb.uptimeSec = (uint32_t)(millis() / 1000);
    hb.freeHeap  = (uint32_t)ESP.getFreeHeap();
    strncpy(hb.deviceName, gSettings.espnowDeviceName.c_str(), sizeof(hb.deviceName) - 1);
    wifi_ap_record_t ap = {};
    hb.rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : (int8_t)-127;
    // 2026-05-19: smart-send so the post-pair seed heartbeat rides
    // SESSION_FRAME if a session is already up (rare — usually KEY_EX hasn't
    // run yet at this point, so this falls through to plaintext, which is
    // what we want for bootstrap liveness).
    v4_send_payload_smart(mac, ESPNOW_V4_TYPE_HEARTBEAT, ESPNOW_V4_FLAG_ACK_REQ, generateMessageId(),
                          (const uint8_t*)&hb, (uint16_t)sizeof(hb), 1);
  }
  bool peersOk   = saveMeshPeers();
  bool devicesOk = saveEspNowDevices();

  // 2026-05-19 — auto-trigger KEY_EX so "pairsecure" actually delivers a
  // working end-to-end secure pair in one command. Without this the legacy
  // LMK (now unused post-Phase-3.5) is the only "security" installed, and
  // any default-encrypted unicast (espnowsend, espnowsessionsend, …) fails
  // with "no peer identity — run espnowkeyex first" because the Phase 3.3
  // KEY_EX never ran. KEY_EX is async (~100ms over the air) — by the time
  // the user's next command lands, the peer identity will be cached and
  // the next encrypted send will auto-kick SESSION_OPEN.
  // Re-pair is an explicit, local operator action — open a short re-key window
  // for this peer (instead of blindly deleting its identity). If the peer's key
  // rotated (re-flash / re-pair), the incoming KEY_EX may replace the stored
  // identity ONCE within the window; otherwise the anti-key-substitution guard
  // still refuses a changed key. This avoids racing/destroying a valid identity
  // that the peer's own proactive KEY_EX may have just established (the cause of
  // the half-open-session bug). For a clean re-pair after a key rotation, run
  // this on BOTH devices (the secure-pair flow already does). The KEY_EX still
  // requires the mesh-passphrase HMAC, so this opens nothing to outsiders.
  espnowOpenPairingWindow(mac);

  const char* meshLabelArg = (meshId < Settings::N_MESHES &&
                              gSettings.meshes[meshId].label.length() > 0)
                                 ? gSettings.meshes[meshId].label.c_str()
                                 : nullptr;
  bool keyExKicked = espnowKeyExInitiate(mac, meshLabelArg);
  if (!keyExKicked) {
    WARN_ESPNOWF("[pairsecure] KEY_EX initiate failed for %s — encrypted "
                 "unicast will fail until the user runs espnowkeyex manually",
                 macStr.c_str());
  }

  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  if (!peersOk || !devicesOk) {
    snprintf(getDebugBuffer(), 1024,
      "Paired %s (%s) but failed to encrypt and save peer data — device encryption key unavailable. "
      "Peer will not persist across reboot.", deviceName.c_str(), macStr.c_str());
  } else {
    snprintf(getDebugBuffer(), 1024,
             "Encrypted device %s successfully: %s (%s)\n"
             "Key fingerprint: %02X%02X%02X%02X...\n"
             "%s",
             wasUpdate ? "updated" : "paired",
             deviceName.c_str(), macStr.c_str(),
             gEspNow->derivedKey[0], gEspNow->derivedKey[1], gEspNow->derivedKey[2], gEspNow->derivedKey[3],
             keyExKicked
                 ? "KEY_EX_HELLO sent; peer identity will land in ~100ms — "
                   "encrypted unicast (espnowsend, espnowsessionsend) is now usable."
                 : "WARN: KEY_EX kick failed; run 'espnowkeyex <mac>' manually before encrypted unicast.");
  }
  return getDebugBuffer();
}

// ============================================================================
// ESP-NOW Metadata Request Command
// ============================================================================

extern void requestMetadata(const uint8_t* peerMac, bool force);

const char* cmd_espnow_requestmeta(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow || !gEspNow->initialized) return "Error: ESP-NOW not initialized";
  
  String args = argsInput;
  args.trim();
  if (args.length() == 0) return "Error: invalid arguments — Usage: espnow requestmeta <name_or_mac>";
  
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
  cliHint("the peer's name/room/zone/tags arrive asynchronously - view them with 'espnowdevices'");
  snprintf(getDebugBuffer(), 1024, "Metadata request sent to %s", args.c_str());
  return getDebugBuffer();
}

// ============================================================================
// ESP-NOW Remote Execution & Streaming Commands
// ============================================================================

// Remote file browse
// reqId-correlation ack helper (Phase 3): in `json` mode, swap a human error
// string for a json error so a `... json` caller never has to parse free text.
// On success these senders return {"schema":1,"ok":true,"reqId":<msgId>} — the
// caller polls `espnowmessages json` and matches that reqId to the result.
static inline const char* espnowAckErr(bool wantJson, const char* text, const char* jsonErr) {
  return wantJson ? jsonErr : text;
}

const char* cmd_espnow_browse(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const bool wantJson = argWantsJson(argsInput);
  if (!gEspNow) return espnowAckErr(wantJson, "Error: ESP-NOW not initialized", "{\"schema\":1,\"ok\":false,\"error\":\"not initialized\"}");
  if (!gEspNow->initialized) {
    return espnowAckErr(wantJson, "ESP-NOW not initialized. Run 'openespnow' first.", "{\"schema\":1,\"ok\":false,\"error\":\"not initialized\"}");
  }

  if (!gEspNow->encryptionEnabled) {
    return espnowAckErr(wantJson, "ESP-NOW encryption required. Set a passphrase with 'espnowsetpassphrase <mesh> <passphrase>' and pair securely.", "{\"schema\":1,\"ok\":false,\"error\":\"encryption required\"}");
  }

  CommandArgs a(argsInput);
  if (!a.hasMinArgs(3)) return espnowAckErr(wantJson, "Usage: espnow browse <target> <username> <password> [\"path\"]", "{\"schema\":1,\"ok\":false,\"error\":\"usage\"}");

  String target = a.arg(0);
  String username = a.arg(1);
  String password = a.arg(2);
  String path = "/";
  if (a.has(3)) {
    const char* qerr = requireQuotedToken(a, 3, path);
    if (qerr) return espnowAckErr(wantJson, qerr, "{\"schema\":1,\"ok\":false,\"error\":\"bad path arg\"}");
  }
  if (path.length() == 0) path = "/";

  uint8_t targetMac[6];
  if (!resolveDeviceNameOrMac(target, targetMac)) {
    if (wantJson) return "{\"schema\":1,\"ok\":false,\"error\":\"not paired\"}";
    EXT_RAM_BSS_ATTR static char browseBuffer[256];
    snprintf(browseBuffer, sizeof(browseBuffer),
             "Target device '%s' not found or not paired. Pair the device first (prefer 'espnowpairsecure').",
             target.c_str());
    return browseBuffer;
  }

  // Build V3 CMD payload: "user:pass:files \"/path\"" — the remote parses the
  // path as a quoted token (uniform quoted-path rule), so it survives spaces.
  char cmdPayload[ESPNOW_V4_MAX_PAYLOAD];
  int payloadLen = snprintf(cmdPayload, sizeof(cmdPayload), "%s:%s:files \"%s\"",
                            username.c_str(), password.c_str(), path.c_str());
  if (payloadLen >= (int)sizeof(cmdPayload)) payloadLen = sizeof(cmdPayload) - 1;

  if (isMeshMode()) {
    if (!isPairedDevice(targetMac)) {
      BROADCAST_PRINTF("[ESP-NOW][mesh] browse send rejected: not paired MAC=%s", formatMacAddress(targetMac).c_str());
      return espnowAckErr(wantJson, "Rejected (mesh): device not paired. Use 'espnowpair' first.", "{\"schema\":1,\"ok\":false,\"error\":\"not paired (mesh)\"}");
    }
    if (!espnowPeerExists(targetMac)) {
      BROADCAST_PRINTF("[ESP-NOW][mesh] browse send rejected: no peer entry MAC=%s", formatMacAddress(targetMac).c_str());
      return espnowAckErr(wantJson, "Error: Rejected (mesh): destination not in ESP-NOW peer table.", "{\"schema\":1,\"ok\":false,\"error\":\"no peer entry\"}");
    }
  }

  // Send via V3 CMD (receiver parses user:pass:cmd format)
  uint32_t msgId = generateMessageId();
  // Step 3b: route through clerk (JOB_AEAD_SMART → v4_send_payload_smart →
  // v4_send_encrypted_or_queue). Status-string distinction (immediate vs
  // queued-for-SESSION_OPEN) is no longer surfaced to the user.
  bool sent = espnowtx::sendAeadSync(targetMac, ESPNOW_V4_TYPE_CMD, ESPNOW_V4_FLAG_ACK_REQ, msgId,
                                (const uint8_t*)cmdPayload, (uint16_t)payloadLen, 1, 2000);

  EXT_RAM_BSS_ATTR static char browseBuffer[256];
  if (!sent) {
    return espnowAckErr(wantJson, "Failed to send V3 browse request", "{\"schema\":1,\"ok\":false,\"error\":\"send failed\"}");
  }

  if (wantJson) {
    snprintf(browseBuffer, sizeof(browseBuffer),
             "{\"schema\":1,\"ok\":true,\"reqId\":%lu,"
             "\"hint\":\"the listing returns asynchronously - read it with 'espnowmessages json' (match this reqId)\"}",
             (unsigned long)msgId);
    return browseBuffer;
  }
  cliHintf("the listing returns asynchronously - read it with 'espnowmessages json' (match reqId %lu)", (unsigned long)msgId);
  snprintf(browseBuffer, sizeof(browseBuffer), "File browse request sent to %s for path: %s",
           target.c_str(), path.c_str());
  return browseBuffer;
}

// Remote file fetch (pull a file from remote device)
const char* cmd_espnow_fetch(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const bool wantJson = argWantsJson(argsInput);
  if (!gEspNow) return espnowAckErr(wantJson, "Error: ESP-NOW not initialized", "{\"schema\":1,\"ok\":false,\"error\":\"not initialized\"}");
  if (!gEspNow->initialized) {
    return espnowAckErr(wantJson, "ESP-NOW not initialized. Run 'openespnow' first.", "{\"schema\":1,\"ok\":false,\"error\":\"not initialized\"}");
  }

  if (!gEspNow->encryptionEnabled) {
    return espnowAckErr(wantJson, "ESP-NOW encryption required. Set a passphrase with 'espnowsetpassphrase <mesh> <passphrase>' and pair securely.", "{\"schema\":1,\"ok\":false,\"error\":\"encryption required\"}");
  }

  CommandArgs a(argsInput);
  if (!a.hasMinArgs(4)) return espnowAckErr(wantJson, "Usage: espnow fetch <target> <username> <password> \"<path>\"", "{\"schema\":1,\"ok\":false,\"error\":\"usage\"}");

  String target = a.arg(0);
  String username = a.arg(1);
  String password = a.arg(2);
  String path;
  const char* qerr = requireQuotedToken(a, 3, path);
  if (qerr) return espnowAckErr(wantJson, qerr, "{\"schema\":1,\"ok\":false,\"error\":\"bad path arg\"}");

  uint8_t targetMac[6];
  if (!resolveDeviceNameOrMac(target, targetMac)) {
    if (wantJson) return "{\"schema\":1,\"ok\":false,\"error\":\"not paired\"}";
    EXT_RAM_BSS_ATTR static char fetchBuffer[256];
    snprintf(fetchBuffer, sizeof(fetchBuffer),
             "Target device '%s' not found or not paired. Pair the device first (prefer 'espnowpairsecure').",
             target.c_str());
    return fetchBuffer;
  }

  // Build V3 CMD payload: "user:pass:espnowsendfile <our_name> <path>"
  // This tells the remote device to send the file back to us via V3 binary file transfer.
  // NOTE: the command token is "espnowsendfile" (one word) — it is registered that way in
  // the command table. Emitting "espnow sendfile" (two words) makes the remote reject it as
  // "Unknown command", which is why fetch silently failed while direct send worked.
  char cmdPayload[ESPNOW_V4_MAX_PAYLOAD];
  int payloadLen = snprintf(cmdPayload, sizeof(cmdPayload), "%s:%s:espnowsendfile %s \"%s\"",
                            username.c_str(), password.c_str(),
                            gSettings.espnowDeviceName.c_str(), path.c_str());
  if (payloadLen >= (int)sizeof(cmdPayload)) payloadLen = sizeof(cmdPayload) - 1;

  if (isMeshMode()) {
    if (!isPairedDevice(targetMac)) {
      BROADCAST_PRINTF("[ESP-NOW][mesh] fetch send rejected: not paired MAC=%s", formatMacAddress(targetMac).c_str());
      return espnowAckErr(wantJson, "Rejected (mesh): device not paired. Use 'espnowpair' first.", "{\"schema\":1,\"ok\":false,\"error\":\"not paired (mesh)\"}");
    }
    if (!espnowPeerExists(targetMac)) {
      BROADCAST_PRINTF("[ESP-NOW][mesh] fetch send rejected: no peer entry MAC=%s", formatMacAddress(targetMac).c_str());
      return espnowAckErr(wantJson, "Error: Rejected (mesh): destination not in ESP-NOW peer table.", "{\"schema\":1,\"ok\":false,\"error\":\"no peer entry\"}");
    }
  }

  // Step 3b: route through clerk (JOB_AEAD_SMART). Same semantics as the
  // browse path above.
  uint32_t msgId = generateMessageId();
  bool sent = espnowtx::sendAeadSync(targetMac, ESPNOW_V4_TYPE_CMD, ESPNOW_V4_FLAG_ACK_REQ, msgId,
                                (const uint8_t*)cmdPayload, (uint16_t)payloadLen, 1, 2000);

  EXT_RAM_BSS_ATTR static char fetchBuffer[256];
  if (!sent) {
    return espnowAckErr(wantJson, "Failed to send V3 fetch request", "{\"schema\":1,\"ok\":false,\"error\":\"send failed\"}");
  }

  if (wantJson) {
    snprintf(fetchBuffer, sizeof(fetchBuffer),
             "{\"schema\":1,\"ok\":true,\"reqId\":%lu,"
             "\"hint\":\"transfer status returns asynchronously - read it with 'espnowmessages json'; the file is written to this device's filesystem\"}",
             (unsigned long)msgId);
    return fetchBuffer;
  }
  cliHint("transfer status returns asynchronously - read it with 'espnowmessages json'; the file is saved to this device's filesystem");
  snprintf(fetchBuffer, sizeof(fetchBuffer), "File fetch request sent to %s for: %s",
           target.c_str(), path.c_str());
  return fetchBuffer;
}

// Remote command execution
const char* cmd_espnow_remote(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  const bool wantJson = argWantsJson(argsInput);
  if (!gEspNow) return espnowAckErr(wantJson, "Error: ESP-NOW not initialized", "{\"schema\":1,\"ok\":false,\"error\":\"not initialized\"}");
  if (!gEspNow->initialized) {
    return espnowAckErr(wantJson, "ESP-NOW not initialized. Run 'openespnow' first.", "{\"schema\":1,\"ok\":false,\"error\":\"not initialized\"}");
  }

  if (!gEspNow->encryptionEnabled) {
    return espnowAckErr(wantJson, "ESP-NOW encryption required. Set a passphrase with "
           "'espnowsetpassphrase <mesh> <passphrase>' and pair securely.", "{\"schema\":1,\"ok\":false,\"error\":\"encryption required\"}");
  }

  CommandArgs a(argsInput);
  if (!a.hasMinArgs(4)) return espnowAckErr(wantJson, "Usage: espnowremote <target> <target-user> <target-pass> <command>\n       <target-user>/<target-pass> are an account ON THE TARGET device (verified there), not this device's login.\n       Async: returns a reqId; read the output with 'espnowmessages json 0 <target-mac>'.", "{\"schema\":1,\"ok\":false,\"error\":\"usage\"}");

  String target = a.arg(0);
  String username = a.arg(1);
  String password = a.arg(2);
  String command = a.remaining(2);

  uint8_t targetMac[6];
  if (!resolveDeviceNameOrMac(target, targetMac)) {
    if (wantJson) return "{\"schema\":1,\"ok\":false,\"error\":\"not paired\"}";
    EXT_RAM_BSS_ATTR static char remoteBuffer[256];
    snprintf(remoteBuffer, sizeof(remoteBuffer),
             "Target device '%s' not found or not paired. Pair the device first (prefer 'espnowpairsecure').",
             target.c_str());
    return remoteBuffer;
  }

  // Check for self-targeting
  {
    uint8_t selfSta[6], selfAp[6];
    esp_wifi_get_mac(WIFI_IF_STA, selfSta);
    esp_wifi_get_mac(WIFI_IF_AP, selfAp);
    if (memcmp(targetMac, selfSta, 6) == 0 || memcmp(targetMac, selfAp, 6) == 0) {
      return espnowAckErr(wantJson, "Error: Cannot send remote command to self. This device is paired with its own MAC. Unpair and pair with the correct remote device.", "{\"schema\":1,\"ok\":false,\"error\":\"self target\"}");
    }
  }

  // V3 binary CMD message (credentials checked on receiver)
  // Build command payload: "user:pass:cmd"
  char cmdPayload[ESPNOW_V4_MAX_PAYLOAD];
  int payloadLen = snprintf(cmdPayload, sizeof(cmdPayload), "%s:%s:%s", 
                            username.c_str(), password.c_str(), command.c_str());
  if (payloadLen >= (int)sizeof(cmdPayload)) payloadLen = sizeof(cmdPayload) - 1;

  uint32_t msgId = generateMessageId();

  // Step 3b: route through clerk (JOB_AEAD_SMART). The 2-attempt retry loop
  // is dropped — it was for transient sync WiFi-TX backpressure, which the
  // clerk's queue handles differently. Retrying submitSync on failure (queue
  // full / timeout / dispatcher returned false) just doubles latency without
  // changing the outcome.
  bool success = espnowtx::sendAeadSync(targetMac, ESPNOW_V4_TYPE_CMD, ESPNOW_V4_FLAG_ACK_REQ, msgId,
                                   (const uint8_t*)cmdPayload, (uint16_t)payloadLen, 1, 2000);

  EXT_RAM_BSS_ATTR static char remoteBuffer[256];
  if (!success) {
    return espnowAckErr(wantJson, "Failed to send remote command", "{\"schema\":1,\"ok\":false,\"error\":\"send failed\"}");
  }

  // Show "Running" notification so the user sees immediate feedback
  // The CMD_RESP will update this in-place to OK/FAIL when the result arrives
  notifyRemoteCommandReceived(target.c_str(), command.c_str());

  if (wantJson) {
    snprintf(remoteBuffer, sizeof(remoteBuffer), "{\"schema\":1,\"ok\":true,\"reqId\":%lu}", (unsigned long)msgId);
    return remoteBuffer;
  }
  snprintf(remoteBuffer, sizeof(remoteBuffer),
           "Sent '%s' to %s. Output is ASYNC - read it with:  espnowmessages json 0 %02X:%02X:%02X:%02X:%02X:%02X  (match reqId %lu)",
           command.c_str(), target.c_str(),
           targetMac[0], targetMac[1], targetMac[2], targetMac[3], targetMac[4], targetMac[5],
           (unsigned long)msgId);
  return remoteBuffer;
}

// Start stream command
const char* cmd_espnow_startstream(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";

  if (!currentAuthContext().ip.startsWith("espnow:")) {
    return "Error: 'startstream' only works via ESP-NOW remote execution.\n"
           "Usage from Device A: espnow remote DeviceB admin pass startstream";
  }

  const uint8_t* senderMac = (const uint8_t*)currentAuthContext().opaque;

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

  EXT_RAM_BSS_ATTR static char streamBuffer[512];
  snprintf(streamBuffer, sizeof(streamBuffer),
           "Stream started - all output will be sent to %s\n"
           "Rate limited to 10 messages/second.\n"
           "Large messages (>200 bytes) use chunked transmission for complete delivery.\n"
           "Use 'espnowremote %s admin pass stopstream' to stop.",
           senderName.c_str(), senderName.c_str());
  return streamBuffer;
}

// Stop stream command
const char* cmd_espnow_stopstream(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow) return "Error: ESP-NOW not initialized";

  if (!gEspNow->streamActive) {
    return "Error: No active stream to stop.";
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
  EXT_RAM_BSS_ATTR static char streamBuffer[512];
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
const char* cmd_espnow_send(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gEspNow) return "Error: ESP-NOW not initialized";
  if (!gEspNow->initialized) {
    return "Error: ESP-NOW not initialized. Run 'openespnow' first.";
  }

  // Parity with browse/fetch/remote: refuse if system-wide encryption is off.
  // Pre-2026 supported a --plaintext opt-out (and a --encrypt no-op for muscle
  // memory); both were removed when the codebase committed to encrypted-only
  // user sends. Rationale: resolveDeviceNameOrMac already requires the target
  // to be paired, paired peers always have an AEAD session establishable via
  // the smart path's encrypt-or-queue, so plaintext was never strictly needed
  // here. Eliminates the only user-facing unencrypted ESP-NOW send mechanism.
  if (!gEspNow->encryptionEnabled) {
    return "Error: ESP-NOW encryption required. Set a passphrase with "
           "'espnowsetpassphrase <mesh> <passphrase>' and pair securely.";
  }

  CommandArgs a(argsInput);
  // Optional leading "json" flag — on-device UIs (OLED) use it to retrieve the
  // msgId so they can poll sendStatus for a post-send delivery indicator. It is
  // LEADING (never trailing) so it can never be mistaken for free-form message
  // text. json acks mirror the rest of the contract: {schema,ok,msgId|error}.
  bool jsonMode = a.arg(0).equalsIgnoreCase("json");
  int base = jsonMode ? 1 : 0;
  if (!a.hasMinArgs(base + 2)) {
    return jsonMode ? "{\"schema\":1,\"ok\":false,\"error\":\"usage\"}"
                    : "Usage: espnow send [json] <name_or_mac> <message>";
  }

  String target = a.arg(base);
  String message = a.remaining(base);

  DEBUGF(DEBUG_ESPNOW_STREAM, "[cmd_espnow_send] message.length()=%d",
         message.length());

  // Resolve device name or MAC address
  uint8_t mac[6];
  if (!resolveDeviceNameOrMac(target, mac)) {
    if (jsonMode) return "{\"schema\":1,\"ok\":false,\"error\":\"not found\"}";
    EXT_RAM_BSS_ATTR static char errBuf[256];
    snprintf(errBuf, sizeof(errBuf),
             "Error: Device '%s' not found. Use 'espnowdevices' to see paired devices.",
             target.c_str());
    return errBuf;
  }

  // Check if trying to send to self
  uint8_t selfSta[6];
  uint8_t selfAp[6];
  esp_wifi_get_mac(WIFI_IF_STA, selfSta);
  esp_wifi_get_mac(WIFI_IF_AP, selfAp);
  if (memcmp(mac, selfSta, 6) == 0 || memcmp(mac, selfAp, 6) == 0) {
    return jsonMode ? "{\"schema\":1,\"ok\":false,\"error\":\"self target\"}"
                    : "Error: Cannot send message to self. Use a different device MAC address.";
  }

  size_t msgLen = message.length();
  uint32_t msgId = generateMessageId();

  // Messages up to ESPNOW_V4_MAX_PLAINTEXT (~202 B) ride a single SESSION_FRAME;
  // longer ones (up to ESPNOW_TEXT_MAX_LEN) are fragmented by the smart path's
  // v4_send_encrypted_chunked — the same generic chunk/reassemble transport
  // command-results use — and rebuilt on the receiver (v4h_text accepts the
  // reassembled payload up to the same cap).
  if (msgLen > (size_t)ESPNOW_TEXT_MAX_LEN) {
    return jsonMode ? "{\"schema\":1,\"ok\":false,\"error\":\"too long\"}"
                    : "Error: message too long (max 1024 bytes). Send larger content as a file.";
  }

  // Multi-frame (chunked) sends require an ACTIVE session — the chunked transport
  // can't queue across the handshake the way single-frame sends do, so a long
  // message fired before the session is up would be dropped. The heartbeat path
  // pre-warms sessions, but cover the brief boot/discovery window too: if this is
  // a multi-frame send to a known peer with no active session yet, kick the
  // handshake and wait briefly for it. Safe to block here — this runs on the
  // cmd_exec task, NOT the espnow RX task, so the RX drainer keeps processing the
  // inbound SESSION_CONFIRM and the handshake can complete. Best-effort: if it
  // doesn't come up in time, the send proceeds and fails cleanly (marked below).
  if (msgLen > (size_t)ESPNOW_V4_MAX_PLAINTEXT) {
    const PeerIdentity* pid = peerIdentityFindByMac(mac);
    if (pid) {
      SessionState* s = sessionFindByPeer(mac, pid->meshId);
      if (!s || s->state != SESSION_ACTIVE) {
        espnowSessionOpenInitiate(mac, nullptr);  // kick (no-op if already in flight)
        uint32_t deadline = (uint32_t)millis() + 3000;
        while ((int32_t)(deadline - (uint32_t)millis()) > 0) {
          vTaskDelay(pdMS_TO_TICKS(50));
          s = sessionFindByPeer(mac, pid->meshId);
          if (s && s->state == SESSION_ACTIVE) break;
        }
      }
    }
  }
  // Step 3b: route through clerk (JOB_AEAD_SMART → v4_send_payload_smart →
  // v4_send_encrypted_or_queue). The clerk's notification is OK/FAIL only, so
  // the "sent" vs "queued for SESSION_OPEN" status-string distinction is dropped.
  // Smart path queues to the pending ring if session not yet ACTIVE; drain on
  // SESSION_CONFIRM reuses the same msgId so the bubble-flip still works.
  // Phase 3.5 task #49 — register the msgId so the web UI's polling loop can
  // flip the bubble to ✓✓ Delivered. Register BEFORE sending: a long (chunked)
  // message is sent SYNCHRONOUSLY, and v4_send_encrypted_chunked marks the msgId
  // delivered from INSIDE sendAeadSync once all fragments ACK. If we registered
  // afterward, that mark would hit an unregistered msgId (no-op) and the bubble
  // would wrongly time out. Single-frame sends ACK asynchronously, so early
  // registration is harmless for them.
  sendStatusRegister(msgId, mac);
  if (!espnowtx::sendAeadSync(mac, ESPNOW_V4_TYPE_TEXT, ESPNOW_V4_FLAG_ACK_REQ, msgId,
                         (const uint8_t*)message.c_str(), (uint16_t)msgLen, 1, 2000)) {
    sendStatusMarkFailed(msgId, mac);  // don't leave an orphan PENDING entry
    if (jsonMode) return "{\"schema\":1,\"ok\":false,\"error\":\"send failed\"}";
    if (!ensureDebugBuffer()) return "Error: encrypted send failed.";
    snprintf(getDebugBuffer(), 1024, "Error: encrypted send failed (clerk timeout or queue full).");
    return getDebugBuffer();
  }
  // Record into this peer's shared sent[] history so EVERY interface (web/OLED/
  // BLE/G2) shows the same outgoing message — not just the UI that sent it. msgId
  // lets the web de-dupe its own optimistic bubble and lets on-device UIs poll
  // delivery status. Recorded on accept (queued sends reuse this msgId on drain).
  storeSentMessageInPeerHistory(mac, message.c_str(), msgId);
  if (jsonMode) {
    if (!ensureDebugBuffer()) return "{\"schema\":1,\"ok\":true}";
    snprintf(getDebugBuffer(), 1024, "{\"schema\":1,\"ok\":true,\"msgId\":%lu}", (unsigned long)msgId);
    return getDebugBuffer();
  }
  if (!ensureDebugBuffer()) return "Encrypted message sent";
  snprintf(getDebugBuffer(), 1024,
           "Encrypted message sent via SESSION_FRAME (ID: %lu, %u plaintext bytes)",
           (unsigned long)msgId, (unsigned)msgLen);
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
  gEspNow->bondSchemaReceived = false;
  gEspNow->bondSettingsSent = false;
  gEspNow->lastRemoteCapValid = false;
  // Clear all pending deferred flags — stale messages from the previous
  // session must not be processed after a role swap or sync reset.
  gEspNow->bondNeedsCapabilityRequest = false;
  gEspNow->bondNeedsCapabilityResponse = false;
  gEspNow->bondReceivedCapability = false;
  gEspNow->bondNeedsManifestResponse = false;
  gEspNow->bondNeedsSettingsResponse = false;
  gEspNow->bondNeedsSchemaResponse = false;
  gEspNow->bondPeerStatusValid = false;
  gEspNow->bondNeedsProactiveStatus = false;
  gEspNow->bondStatusReqSentOnce = false;
  // NOTE: do NOT touch the bond auth token here. It is a property of the
  // encrypted session (SessionState::bondToken), not of this sync bookkeeping.
  // Wiping it on every resync (peer reboot, HB timeout, role swap, re-bondconnect)
  // is exactly the bug that left bonded-but-tokenless devices unable to swap
  // roles. The token lives and dies with its session; leave it alone.
}

// ============================================================================
// Bond Mode CLI Commands
// ============================================================================

/**
 * Request capability summary from bonded peer
 */
const char* cmd_bond_requestcap(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gSettings.bondModeEnabled || gSettings.bondPeerMac.length() == 0) {
    return "Error: Not connected in bond mode. Use 'bondconnect <device>' first.";
  }
  
  uint8_t peerMac[6];
  if (!parseMacAddress(gSettings.bondPeerMac, peerMac)) {
    return "Error: Invalid peer MAC address in settings.";
  }

  uint32_t reqId = generateMessageId();
  bool sent = false;
  for (int attempt = 0; attempt < 2; attempt++) {
    sent = bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_BOND_CAP_REQ, ESPNOW_V4_FLAG_ACK_REQ, reqId, nullptr, 0);
    if (sent) break;
  }
  if (sent) cliHint("the peer's capabilities arrive asynchronously - read them from GET /api/bond/status (note 'bondshowcap' shows THIS device's capabilities)");
  return sent ? "Capability request sent. Check output for response." : "Error: Failed to send capability request.";
}

/**
 * Show local capability summary
 */
const char* cmd_bond_showcap(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    CapabilitySummary jc;
    buildCapabilitySummary(jc);
    if (!ensureDebugBuffer()) return "{\"schema\":1,\"ok\":false,\"error\":\"buffer\"}";
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    doc["device"] = jc.deviceName;
    char macStr[18];
    snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
             jc.mac[0], jc.mac[1], jc.mac[2], jc.mac[3], jc.mac[4], jc.mac[5]);
    doc["mac"]  = String(macStr);
    doc["role"] = jc.role == 1 ? "master" : "worker";
    char fwStr[9];
    snprintf(fwStr, sizeof(fwStr), "%02X%02X%02X%02X",
             jc.fwHash[0], jc.fwHash[1], jc.fwHash[2], jc.fwHash[3]);
    doc["fwHash"]      = String(fwStr);
    doc["featureMask"] = (unsigned long)jc.featureMask;   // decode with CAP_FEATURE_* bits
    doc["serviceMask"] = (unsigned long)jc.serviceMask;   // decode with CAP_SERVICE_* bits
    doc["sensorMask"]  = (unsigned long)jc.sensorMask;    // decode with CAP_SENSOR_*  bits
    doc["flashMB"]     = (unsigned long)jc.flashSizeMB;
    doc["psramMB"]     = (unsigned long)jc.psramSizeMB;
    doc["wifiChannel"] = jc.wifiChannel;
    doc["uptimeSec"]   = (unsigned long)jc.uptimeSeconds;
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

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
const char* cmd_bond_requestmanifest(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gSettings.bondModeEnabled || gSettings.bondPeerMac.length() == 0) {
    return "Error: Not connected in bond mode. Use 'bondconnect <device>' first.";
  }
  
  uint8_t peerMac[6];
  if (!parseMacAddress(gSettings.bondPeerMac, peerMac)) {
    return "Error: Invalid peer MAC address in settings.";
  }
  
  // Send v3 manifest request
  uint32_t msgId = generateMessageId();
  if (bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_MANIFEST_REQ, ESPNOW_V4_FLAG_ACK_REQ, msgId, nullptr, 0)) {
    cliHint("the manifest arrives asynchronously via file transfer - view it with 'bondshowremotemanifest'");
    return "Manifest request sent (v3). Response will arrive via file transfer.";
  } else {
    return "Error: Failed to send manifest request.";
  }
}

/**
 * Request settings file from bonded peer (response arrives as _settings_out.json
 * via the file-transfer chain). Sibling to bondrequestcap / bondrequestmanifest;
 * added 2026-05 to round out the per-stage bond request primitives that the new
 * `bondresync` wrapper composes from.
 */
const char* cmd_bond_requestsettings(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!gSettings.bondModeEnabled || gSettings.bondPeerMac.length() == 0) {
    return "Error: Not connected in bond mode. Use 'bondconnect <device>' first.";
  }

  uint8_t peerMac[6];
  if (!parseMacAddress(gSettings.bondPeerMac, peerMac)) {
    return "Error: Invalid peer MAC address in settings.";
  }

  uint32_t msgId = generateMessageId();
  bool sent = false;
  for (int attempt = 0; attempt < 2; attempt++) {
    sent = bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_SETTINGS_REQ, ESPNOW_V4_FLAG_ACK_REQ, msgId, nullptr, 0);
    if (sent) break;
  }
  if (sent) cliHint("the peer's settings arrive asynchronously via file transfer - read them from GET /api/bond/settings");
  return sent ? "Settings request sent. Response will arrive via file transfer."
              : "Error: Failed to send settings request.";
}

/**
 * Request settings schema from bonded peer (response arrives as _schema_out.json
 * via the file-transfer chain). Added 2026-05 alongside bondrequestsettings.
 */
const char* cmd_bond_requestschema(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!gSettings.bondModeEnabled || gSettings.bondPeerMac.length() == 0) {
    return "Error: Not connected in bond mode. Use 'bondconnect <device>' first.";
  }

  uint8_t peerMac[6];
  if (!parseMacAddress(gSettings.bondPeerMac, peerMac)) {
    return "Error: Invalid peer MAC address in settings.";
  }

  uint32_t msgId = generateMessageId();
  bool sent = false;
  for (int attempt = 0; attempt < 2; attempt++) {
    sent = bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_SCHEMA_REQ, ESPNOW_V4_FLAG_ACK_REQ, msgId, nullptr, 0);
    if (sent) break;
  }
  if (sent) cliHint("the peer's settings schema arrives asynchronously via file transfer - read it from GET /api/bond/settings/schema");
  return sent ? "Schema request sent. Response will arrive via file transfer."
              : "Error: Failed to send schema request.";
}

/**
 * Force a re-sync of bond state with the peer (capabilities + manifest +
 * settings + schema). User-facing safety hatch for stuck-state recovery.
 *
 * USE CASES:
 *  - Worker's UI stuck on "Establishing Bond" (Cap/Status flags didn't flip after a transient race)
 *  - Peer enabled/disabled a sensor at runtime — capabilities went stale (no heartbeat
 *    auto-detection for cap changes; only bootCounter + settingsHash are heartbeat-tracked)
 *  - Suspected mid-sync packet loss left one side with stale data
 *  - General "this looks weird, force refresh" reflex (analogous to wpa_cli reconnect
 *    or DHCP renew — every robust protocol has a force-refresh primitive)
 *
 * BEHAVIOR by role:
 *  - MASTER: resetBondSync() + flag bondNeedsCapabilityRequest=true. The super-loop
 *    sync tick consumes the flag and kicks the existing CAP_REQ → MANIFEST_REQ →
 *    SETTINGS_REQ → SCHEMA_REQ chain.
 *  - WORKER: resetBondSync() + send CAP_REQ, MANIFEST_REQ, SETTINGS_REQ, SCHEMA_REQ
 *    directly to the master. The master responds to each (CAP_RESP + reciprocal CAP_RESP
 *    on the receive side, plus the three file transfers). The worker's *Received flags
 *    re-flip to true as the responses arrive.
 *
 * NEVER:
 *  - Tears down the AEAD session — sync-state refresh, not connection refresh
 *  - Clears the bond auth token — preserved per existing resetBondSync semantics
 *  - Clears peer identity (espnowforget territory)
 *  - Unbonds (bonddisconnect territory)
 *  - Touches sensor streaming state — orthogonal
 *
 * Idempotent — running twice converges; the responses just re-flip the flags again.
 */
const char* cmd_bond_resync(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (!gEspNow || !gEspNow->initialized) {
    return "Error: ESP-NOW not initialized.";
  }
  if (!gSettings.bondModeEnabled || gSettings.bondPeerMac.length() == 0) {
    return "Error: Not in bond mode. Use 'bondconnect <peer>' first.";
  }

  // Parse optional stage filter — default is --all.
  enum { STAGE_ALL, STAGE_CAP, STAGE_MANIFEST, STAGE_SETTINGS, STAGE_SCHEMA } stage = STAGE_ALL;
  String args = argsInput;
  args.trim();
  if (args.length() > 0) {
    if      (args == "--all")        stage = STAGE_ALL;
    else if (args == "--cap")        stage = STAGE_CAP;
    else if (args == "--manifest")   stage = STAGE_MANIFEST;
    else if (args == "--settings")   stage = STAGE_SETTINGS;
    else if (args == "--schema")     stage = STAGE_SCHEMA;
    else {
      return "Error: invalid arguments — Usage: bondresync [--cap|--manifest|--settings|--schema|--all]";
    }
  }

  // Per-stage shortcut — just delegate to the existing request verb.
  // String("") is what each verb expects when no positional args follow.
  if (stage == STAGE_CAP)      return cmd_bond_requestcap(String(""));
  if (stage == STAGE_MANIFEST) return cmd_bond_requestmanifest(String(""));
  if (stage == STAGE_SETTINGS) return cmd_bond_requestsettings(String(""));
  if (stage == STAGE_SCHEMA)   return cmd_bond_requestschema(String(""));

  // --all path: clear bookkeeping flags and kick the role-appropriate chain.
  uint8_t peerMac[6];
  if (!parseMacAddress(gSettings.bondPeerMac, peerMac)) {
    return "Error: Invalid peer MAC address in settings.";
  }

  // Reset the local sync state. We deliberately accept the wipe of
  // lastRemoteCapValid / *Received flags here — unlike the bondrole-change site
  // (where the peer's state hasn't actually changed), here the whole POINT is
  // to declare the local view stale and re-fetch. The auth token and session
  // state are preserved by resetBondSync (see comment in that function).
  resetBondSync();

  const bool master = isBondMaster();
  const bool peerOnline = gEspNow->bondPeerOnline;

  if (master) {
    // Master drives sync via the tick. Setting bondNeedsCapabilityRequest=true
    // kicks the existing chain on the next super-loop iteration.
    gEspNow->bondNeedsCapabilityRequest = true;
    if (!ensureDebugBuffer()) return peerOnline
                                ? "Bond resync initiated (master): full sync chain queued."
                                : "Bond resync queued (master); peer offline, will fire when peer returns.";
    snprintf(getDebugBuffer(), 1024,
             "Bond resync initiated (master role) — full chain queued "
             "(CAP → MANIFEST → SETTINGS → SCHEMA). Peer %s.",
             peerOnline ? "online; sync will start on next tick"
                        : "offline; sync will start when peer returns");
    return getDebugBuffer();
  }

  // Worker side: fire each request directly. Master will respond to each.
  // We do CAP_REQ first so the reciprocal CAP_RESP path on the master's
  // receiver re-populates our lastRemoteCapValid; the other three are file-
  // transfer responses that re-flip the matching *Received flags.
  uint32_t msgIdCap   = generateMessageId();
  uint32_t msgIdMan   = generateMessageId();
  uint32_t msgIdSet   = generateMessageId();
  uint32_t msgIdSch   = generateMessageId();
  bool capOk  = bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_BOND_CAP_REQ,  ESPNOW_V4_FLAG_ACK_REQ, msgIdCap, nullptr, 0);
  bool manOk  = bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_MANIFEST_REQ,  ESPNOW_V4_FLAG_ACK_REQ, msgIdMan, nullptr, 0);
  bool setOk  = bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_SETTINGS_REQ,  ESPNOW_V4_FLAG_ACK_REQ, msgIdSet, nullptr, 0);
  bool schOk  = bondSendEncryptedAsync(peerMac, ESPNOW_V4_TYPE_SCHEMA_REQ,    ESPNOW_V4_FLAG_ACK_REQ, msgIdSch, nullptr, 0);

  if (!ensureDebugBuffer()) {
    return (capOk && manOk && setOk && schOk)
        ? "Bond resync initiated (worker): CAP+MANIFEST+SETTINGS+SCHEMA requests sent."
        : "Bond resync (worker): some requests failed to send — see log.";
  }
  snprintf(getDebugBuffer(), 1024,
           "Bond resync initiated (worker role) — requests sent: "
           "cap=%s, manifest=%s, settings=%s, schema=%s. "
           "Master will respond to each; bond flags will re-populate as data arrives.%s",
           capOk ? "ok" : "FAIL",
           manOk ? "ok" : "FAIL",
           setOk ? "ok" : "FAIL",
           schOk ? "ok" : "FAIL",
           peerOnline ? "" : " (peer offline — queued)");
  return getDebugBuffer();
}

/**
 * Show cached remote manifests: bond showremotemanifest [fwHash]
 * Without args: list all cached manifests
 * With fwHash arg: show specific manifest
 */
const char* cmd_bond_showremotemanifest(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!filesystemReady) {
    return "Error: Filesystem not ready.";
  }
  
  String args = argsInput;
  args.trim();
  
  const char* manifestDir = "/system/manifests";
  
  // If no argument, list all cached manifests
  if (args.length() == 0) {
    if (!VFS::existsGuarded(manifestDir, currentAuthContext())) {
      return "No cached manifests. Use 'bondrequestmanifest' to fetch from peer.";
    }

    File dir = VFS::openGuarded(manifestDir, "r", currentAuthContext());
    if (!dir || !dir.isDirectory()) {
      return "Error: Cannot open manifests directory.";
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
    return "Use 'bondshowremotemanifest <fwHash>' to view details.";
  }
  
  // Show specific manifest by fwHash
  char path[96];
  snprintf(path, sizeof(path), "%s/%s.json", manifestDir, args.c_str());
  if (!VFS::existsGuarded(path, currentAuthContext())) {
    return "Error: Manifest not found. Use 'bondshowremotemanifest' to list available.";
  }

  FsLockGuard guard("bond.manifest.read");
  File f = VFS::openGuarded(path, "r", currentAuthContext());
  if (!f) {
    return "Error: Failed to open manifest file.";
  }
  
  String manifest = f.readString();
  f.close();
  
  BROADCAST_PRINTF("=== Remote Manifest: %s ===", args.c_str());
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
const char* cmd_bond_showmanifest(const String& argsInput) {
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
const char* cmd_bond_connect(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!gEspNow || !gEspNow->initialized) {
    return "Error: ESP-NOW not initialized. Run 'openespnow' first.";
  }
  
  String args = argsInput;
  args.trim();
  
  if (args.length() == 0) {
    return "Error: invalid arguments — Usage: bondconnect <mac_or_name>";
  }
  
  // Resolve device name or MAC to MAC bytes
  uint8_t peerMac[6];
  if (!resolveDeviceNameOrMac(args, peerMac)) {
    return "Error: Device not found. Use 'espnowlist' to see paired devices.";
  }
  
  // Check if already connected
  if (gSettings.bondModeEnabled && 
      gSettings.bondPeerMac == formatMacAddress(peerMac)) {
    return "Already connected to this device in bond mode.";
  }
  
  // Enable bond mode and set peer MAC
  // Role is determined by MAC address comparison (higher MAC = master)
  // This ensures deterministic role assignment when both devices run 'bondconnect'
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
                isBondMaster() ? "MASTER" : "WORKER",
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
           isBondMaster() ? "master" : "worker");
  return getDebugBuffer();
}

/**
 * Disconnect from bonded peer: bond disconnect
 */
const char* cmd_bond_disconnect(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gSettings.bondModeEnabled) {
    return "Error: Not currently in bond mode.";
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
const char* cmd_bond_status(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();

  if (argWantsJson(argsInput)) {
    if (!ensureDebugBuffer()) return "{\"schema\":1,\"ok\":false,\"error\":\"buffer\"}";
    PSRAM_JSON_DOC(doc);
    doc["schema"]  = 1;
    doc["enabled"] = gSettings.bondModeEnabled;
    doc["role"]    = isBondMaster() ? "master" : "worker";
    if (gSettings.bondModeEnabled) {
      uint8_t pm[6];
      String pname = "";
      if (parseMacAddress(gSettings.bondPeerMac, pm)) pname = getEspNowDeviceName(pm);
      doc["peer"]     = gSettings.bondPeerMac;
      doc["peerName"] = pname;
      bool online = gEspNow && gEspNow->bondPeerOnline;
      doc["online"]    = online;
      doc["syncState"] = (!online) ? "offline" : (isBondSynced() ? "synced" : "syncing");
      JsonObject sync = doc["sync"].to<JsonObject>();
      sync["cap"]        = gEspNow ? gEspNow->lastRemoteCapValid : false;
      sync["manifest"]   = gEspNow ? gEspNow->bondManifestReceived : false;
      sync["settingsRx"] = gEspNow ? gEspNow->bondSettingsReceived : false;
      sync["settingsTx"] = gEspNow ? gEspNow->bondSettingsSent : false;
      doc["heartbeatsSent"]     = (unsigned long)(gEspNow ? gEspNow->bondHeartbeatsSent : 0UL);
      doc["heartbeatsReceived"] = (unsigned long)(gEspNow ? gEspNow->bondHeartbeatsReceived : 0UL);
      uint32_t lastAgo = (gEspNow && gEspNow->lastBondHeartbeatReceivedMs > 0)
                         ? (millis() - gEspNow->lastBondHeartbeatReceivedMs) / 1000 : 0;
      doc["lastHeartbeatAgo"] = lastAgo;
    }
    serializeJson(doc, getDebugBuffer(), 1024);
    return getDebugBuffer();
  }

  if (!gSettings.bondModeEnabled) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    char* buf = getDebugBuffer();
    snprintf(buf, 512, "Bond mode: DISABLED\nRole: %s", isBondMaster() ? "master" : "worker");
    return buf;
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
  BROADCAST_PRINTF("Role: %s", isBondMaster() ? "master (display/gamepad)" : "worker (compute/network)");
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
const char* cmd_bond_role(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  String args = argsInput;
  args.trim();
  args.toLowerCase();
  
  if (args.length() == 0) {
    if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
    snprintf(getDebugBuffer(), 1024, "Current role: %s",
             isBondMaster() ? "master" : "worker");
    return getDebugBuffer();
  }
  
  uint8_t newRole = 255;
  if (args == "master" || args == "1") {
    newRole = 1;
  } else if (args == "worker" || args == "0") {
    newRole = 0;
  } else {
    return "Error: invalid arguments — Usage: bondrole <master|worker>";
  }
  
  bool changed = (newRole != gSettings.bondRole);
  setSetting(gSettings.bondRole, newRole);
  
  // Reset handshake when role changes — sequencing is role-dependent
  // (worker=initiator, master=responder). Without reset, both sides
  // could end up waiting or both initiating simultaneously.
  if (changed && gEspNow && gEspNow->initialized) {
    // Snapshot the "what we know about the peer / what we've sent the peer"
    // fields BEFORE resetBondSync. None of these change just because OUR
    // local role flipped — the peer's cap data is hardware-derived (role-
    // independent), the manifest is the peer's firmware hash, settings are
    // the peer's user config, schema is the peer's command schema. Likewise
    // bondCapSent/bondSettingsSent ("I've already sent my cap/settings to
    // peer") remain semantically true because cap+settings are role-independent
    // and the peer still has them cached from the pre-swap exchange.
    //
    // resetBondSync correctly wipes the role-dependent state machine flags
    // (bondSyncInFlight, bondNeedsCapability*, etc.) which DO need to reset
    // because sync sequencing is role-dependent — but it also clobbers all
    // the peer-state and obligation flags. Previously this left the new
    // worker's UI stuck on "Establishing Bond" because:
    //   - master: *Received flags went false → sync tick re-pulled → OK
    //   - worker: bondCapSent/bondSettingsSent went false →
    //             isBondSynced() returned false on worker → UI stuck
    //   - both: side effects of "becoming synced" (STATUS_REQ, streaming
    //             setup) live INSIDE processBondSettings which only runs
    //             on settings file arrival, so the role-swap-restored-to-
    //             synced path never fired them → bondPeerStatusValid stayed
    //             false → UI step 4 of 4 unchecked forever
    //
    // 2026-05 fix: snapshot + restore all six peer-state/obligation flags,
    // then fire the post-sync side effects directly via the shared helper
    // (firePostSyncSideEffects) below.
    bool wasCapValid   = gEspNow->lastRemoteCapValid;
    bool hadManifest   = gEspNow->bondManifestReceived;
    bool hadSettings   = gEspNow->bondSettingsReceived;
    bool hadSchema     = gEspNow->bondSchemaReceived;
    bool hadCapSent    = gEspNow->bondCapSent;
    bool hadSettSent   = gEspNow->bondSettingsSent;

    resetBondSync();

    // Restore the still-valid peer state. The peer hasn't changed, so the
    // UI doesn't need to re-show "Establishing Bond" — just relabel master/worker.
    gEspNow->lastRemoteCapValid     = wasCapValid;
    gEspNow->bondManifestReceived   = hadManifest;
    gEspNow->bondSettingsReceived   = hadSettings;
    gEspNow->bondSchemaReceived     = hadSchema;
    gEspNow->bondCapSent            = hadCapSent;
    gEspNow->bondSettingsSent       = hadSettSent;

    // bondPeerStatusValid is left cleared (resetBondSync wiped it) — status
    // payload may legitimately differ now that roles flipped (e.g. master
    // exposes different fields than worker), so let the next BOND_STATUS_RESP
    // re-populate it cleanly. This is the one peer-state field we DO want to
    // re-fetch after a role swap. bondStatusReqSentOnce is also left cleared
    // by resetBondSync, which is required so firePostSyncSideEffects below
    // will actually fire the STATUS_REQ that re-populates bondPeerStatusValid.
    gEspNow->bondPeerStatusValid = false;

    // Disable all sensor streaming — don't carry old master's prefs into new role
    setSetting(gSettings.bondStreamThermal, false);
    setSetting(gSettings.bondStreamTof, false);
    setSetting(gSettings.bondStreamImu, false);
    setSetting(gSettings.bondStreamGps, false);
    setSetting(gSettings.bondStreamInput, false);
    setSetting(gSettings.bondStreamFmradio, false);
    setSetting(gSettings.bondStreamRtc, false);
    setSetting(gSettings.bondStreamPresence, false);
    // Clear runtime streaming flags too
    for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
      stopSensorDataStreaming((RemoteSensorType)i);
    }

    // If peer is already online and the restored flags say we're synced, fire
    // the post-sync side effects directly (STATUS_REQ + streaming setup). This
    // is the hybrid Option A/B fix: cmd_bond_role uses the same helper as
    // processBondSettings does on cold sync. If the encrypted send fails
    // here because the session isn't yet ACTIVE, the helper leaves
    // bondStatusReqSentOnce=false and the bond tick guard retries.
    //
    // If we're NOT fully synced post-restore (e.g. a role swap that happened
    // mid-handshake before all the peer flags got set), fall back to the old
    // "let the sync tick drive it" path via bondNeedsCapabilityRequest.
    if (gEspNow->bondPeerOnline) {
      uint8_t peerMac[6];
      bool macOk = (gSettings.bondPeerMac.length() > 0 &&
                    parseMacAddress(gSettings.bondPeerMac, peerMac));
      if (macOk && isBondSynced()) {
        firePostSyncSideEffects(peerMac);
        BROADCAST_PRINTF("[BOND] Role changed to %s — post-sync side effects fired",
                         newRole == 1 ? "master" : "worker");
      } else if (newRole == 1) {
        gEspNow->bondNeedsCapabilityRequest = true;  // Consumed by sync tick
        BROADCAST_PRINTF("[BOND] Role changed to master — sync tick will drive handshake");
      } else {
        BROADCAST_PRINTF("[BOND] Role changed to worker — awaiting master CAP_REQ");
      }
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
const char* cmd_bond_stream(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gSettings.bondModeEnabled) {
    return "Error: Not in bond mode. Use 'bondconnect <device>' first.";
  }
  
  CommandArgs a(argsInput);

  if (a.count() == 0) {
    // Show current streaming status with detailed diagnostics
    broadcastOutput("[BOND] Sensor streaming diagnostics (bidirectional):");
    BROADCAST_PRINTF("  Bond mode enabled: %s", gSettings.bondModeEnabled ? "YES" : "NO");
    BROADCAST_PRINTF("  Our role: %s", isBondMaster() ? "MASTER" : "WORKER");
    BROADCAST_PRINTF("  Peer MAC: %s", gSettings.bondPeerMac.length() > 0 ? gSettings.bondPeerMac.c_str() : "(none)");
    BROADCAST_PRINTF("  ESP-NOW init: %s", (gEspNow && gEspNow->initialized) ? "YES" : "NO");
    BROADCAST_PRINTF("  Peer online: %s", gEspNow ? (gEspNow->bondPeerOnline ? "YES" : "NO") : "N/A");
    BROADCAST_PRINTF("  isBondModeOnline(): %s", isBondModeOnline() ? "YES" : "NO");
    BROADCAST_PRINTF("  V4PayloadSensorData size: %u bytes", (unsigned)sizeof(V4PayloadSensorData));
    BROADCAST_PRINTF("  Max sensor data: %u bytes", (unsigned)(ESPNOW_V4_MAX_PAYLOAD - sizeof(V4PayloadSensorData)));
    broadcastOutput("");
    broadcastOutput("  Sensor streaming (runtime / saved):");
    bool savedFlags[] = {gSettings.bondStreamThermal, gSettings.bondStreamTof, gSettings.bondStreamImu, 
                         gSettings.bondStreamGps, gSettings.bondStreamInput, gSettings.bondStreamFmradio,
                         gSettings.bondStreamRtc, gSettings.bondStreamPresence};
    const char* sensors[] = {"thermal", "tof", "imu", "gps", "input", "fmradio", "rtc", "presence"};
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
  if (!a.hasMinArgs(2)) {
    return "Error: invalid arguments — Usage: bondstream <sensor> <on|off>\n       bondstream (show status)";
  }

  String sensorName = a.arg(0);
  sensorName.toLowerCase();

  String action = a.arg(1);
  action.toLowerCase();
  
  // Validate sensor name
  RemoteSensorType sensorType = stringToSensorType(sensorName.c_str());
  if (strcmp(sensorTypeToString(sensorType), sensorName.c_str()) != 0) {
    return "Error: Unknown sensor. Valid: thermal, tof, imu, gps, input, fmradio, rtc, presence";
  }
  
  // Parse action
  bool enable = false;
  if (action == "on" || action == "1" || action == "start") {
    enable = true;
  } else if (action == "off" || action == "0" || action == "stop") {
    enable = false;
  } else {
    return "Error: invalid arguments — Usage: bondstream <sensor> <on|off>";
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
    case REMOTE_SENSOR_INPUT:  setSetting(gSettings.bondStreamInput, enable); break;
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
const char* cmd_bond_testsensor(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!gSettings.bondModeEnabled) {
    return "Error: Not in bond mode. Use 'bondconnect <device>' first.";
  }
  
  String args = argsInput;
  args.trim();

  // Default to thermal sensor for testing
  RemoteSensorType testType = REMOTE_SENSOR_THERMAL;
  if (args.length() > 0) {
    testType = stringToSensorType(args.c_str());
  }
  
  broadcastOutput("[BOND_TEST] Testing v3 sensor data transmission...");
  BROADCAST_PRINTF("  Sensor type: %s (%d)", sensorTypeToString(testType), (int)testType);
  BROADCAST_PRINTF("  Role: %s", isBondMaster() ? "MASTER" : "WORKER");
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
    return "Error: FAILED: Could not send test packet (check debug output)";
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
const char* cmd_espnow_buffers(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  CommandArgs a(argsInput);

  // No args: show current buffer settings
  if (a.count() == 0) {
    char* buf = getDebugBuffer();
    int pos = 0;
    pos += snprintf(buf + pos, 1024 - pos, "=== ESP-NOW Buffer Settings ===\n");
    pos += snprintf(buf + pos, 1024 - pos, "TX Queue Size:     %u (1-16, default: 8)\n", gSettings.espnowTxQueueSize);
    pos += snprintf(buf + pos, 1024 - pos, "RX Buffer Size:    %u (64-512, default: 256)\n", gSettings.espnowRxBufferSize);
    pos += snprintf(buf + pos, 1024 - pos, "Chunk Size:        %u (100-212, default: 200)\n", gSettings.espnowChunkSize);
    pos += snprintf(buf + pos, 1024 - pos, "File Chunk Size:   %u (100-216, default: 216)\n", gSettings.espnowFileChunkSize);
    pos += snprintf(buf + pos, 1024 - pos, "\nV4 Protocol Constants:\n");
    pos += snprintf(buf + pos, 1024 - pos, "  Max Payload:     %d bytes\n", ESPNOW_V4_MAX_PAYLOAD);
    pos += snprintf(buf + pos, 1024 - pos, "  Dedup Buffer:    %d entries\n", V4_DEDUP_SIZE);
    pos += snprintf(buf + pos, 1024 - pos, "\nNote: Changes take effect after ESP-NOW reinit or reboot.");
    return buf;
  }
  
  // Parse: espnow buffers <type> [value]
  String bufType = a.arg(0);
  bufType.toLowerCase();

  // If no value provided, show just that setting
  if (!a.has(1)) {
    if (bufType == "tx") {
      snprintf(getDebugBuffer(), 1024, "TX Queue Size: %u (range: 1-16)", gSettings.espnowTxQueueSize);
    } else if (bufType == "rx") {
      snprintf(getDebugBuffer(), 1024, "RX Buffer Size: %u (range: 64-512)", gSettings.espnowRxBufferSize);
    } else if (bufType == "chunk") {
      snprintf(getDebugBuffer(), 1024, "Chunk Size: %u (range: 100-212)", gSettings.espnowChunkSize);
    } else if (bufType == "filechunk") {
      snprintf(getDebugBuffer(), 1024, "File Chunk Size: %u (range: 100-216)", gSettings.espnowFileChunkSize);
    } else {
      return "Error: invalid arguments — Usage: espnow buffers [tx|rx|chunk|filechunk] [value]";
    }
    return getDebugBuffer();
  }
  
  // Parse value
  int value = a.argInt(1, 0);
  
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
    if (value < 100 || value > 212) return "Error: Chunk size must be 100-212";
    setSetting(gSettings.espnowChunkSize, (uint16_t)value);
    snprintf(getDebugBuffer(), 1024, "Chunk Size set to %d (takes effect after reinit)", value);
  } else if (bufType == "filechunk") {
    if (value < 100 || value > 216) return "Error: File chunk size must be 100-216";
    setSetting(gSettings.espnowFileChunkSize, (uint16_t)value);
    snprintf(getDebugBuffer(), 1024, "File Chunk Size set to %d (takes effect after reinit)", value);
  } else {
    return "Error: Unknown buffer type. Use: tx, rx, chunk, filechunk";
  }
  
  return getDebugBuffer();
}

// ============================================================================
// ESP-NOW Command Registry
// ============================================================================

extern const char* cmd_espnow_sensorstream(const String& argsInput);
extern const char* cmd_espnow_sensorstatus(const String& argsInput);
extern const char* cmd_espnow_sensorbroadcast(const String& argsInput);
extern const char* cmd_espnow_usersync(const String& argsInput);
// Device metadata commands
extern const char* cmd_espnow_room(const String& argsInput);
extern const char* cmd_espnow_zone(const String& argsInput);
extern const char* cmd_espnow_tags(const String& argsInput);
extern const char* cmd_espnow_friendlyname(const String& argsInput);
extern const char* cmd_espnow_stationary(const String& argsInput);
extern const char* cmd_espnow_deviceinfo(const String& argsInput);
// Master aggregation commands
extern const char* cmd_espnow_devices(const String& argsInput);
extern const char* cmd_espnow_rooms(const String& argsInput);
extern const char* cmd_espnow_find(const String& argsInput);
extern const char* cmd_espnow_roomcmd(const String& argsInput);
extern const char* cmd_espnow_tagcmd(const String& argsInput);

// Forward declarations for schema-driven setting commands (defined below via ESPNOW_SETTING_CMD macro)
const char* cmd_espnow_firsttimesetup(const String&);
const char* cmd_espnow_heartbeatinterval(const String&);
const char* cmd_espnow_failovertimeout(const String&);
const char* cmd_espnow_workerstatusinterval(const String&);
const char* cmd_espnow_topodiscoveryinterval(const String&);
const char* cmd_espnow_topoautorefresh(const String&);
const char* cmd_espnow_heartbeatbroadcast(const String&);
const char* cmd_espnow_meshadaptivettl(const String&);
const char* cmd_espnow_meshpeermax(const String&);
const char* cmd_espnow_sensorbroadcastinterval(const String&);
const char* cmd_espnow_txqueuesize(const String&);
const char* cmd_espnow_rxbuffersize(const String&);
const char* cmd_espnow_chunksize(const String&);
const char* cmd_espnow_filechunksize(const String&);
#if ENABLE_BONDED_MODE
const char* cmd_espnow_bondmodeenabled(const String&);
const char* cmd_espnow_bondpeermac(const String&);
const char* cmd_espnow_bondstreamthermal(const String&);
const char* cmd_espnow_bondstreamtof(const String&);
const char* cmd_espnow_bondstreamimu(const String&);
const char* cmd_espnow_bondstreamgps(const String&);
const char* cmd_espnow_bondstreaminput(const String&);
const char* cmd_espnow_bondstreamfmradio(const String&);
const char* cmd_espnow_bondstreamrtc(const String&);
const char* cmd_espnow_bondstreampresence(const String&);
#endif

// Buffered ESP-NOW message history as JSON. This is what lets a BLE/CLI client
// retrieve the ASYNC results of relayed remote ops (espnowremote/browse/fetch) —
// otherwise those only reach the web's /api/espnow/messages poll, which a BLE
// app cannot hit. Mirrors that endpoint's per-message shape.
//   Usage: espnowmessages json [sinceSeq] [mac]
// sinceSeq pages incrementally (return only messages newer than that seq); mac
// filters to one peer. Same access level as espnowremote (the producer).
const char* cmd_espnow_messages(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  CommandArgs a(argsInput);

  uint32_t since = 0;
  uint8_t  mac[6];
  bool     hasMac = false;
  for (int i = 0; i < a.count(); i++) {
    const String& t = a.arg(i);
    if (t.equalsIgnoreCase("json")) continue;
    if (parseMacAddress(t, mac)) hasMac = true;          // looks like a MAC
    else since = (uint32_t)strtoul(t.c_str(), nullptr, 10);  // else a seq number
  }

  // Page size capped at 8: keeps the serialized JSON comfortably under the 4096-byte async
  // command-result buffer (ExecReq.out) so a page is never truncated into invalid JSON, and
  // keeps the BLE fragment count per page low so the paced send stays quick and reliable.
  // memreport (the largest/burstiest producer) is the worst case and fits within this.
  const int MAXM = 8;
  static ReceivedTextMessage* msgs = nullptr;
  if (!msgs) msgs = (ReceivedTextMessage*)ps_alloc(sizeof(ReceivedTextMessage) * MAXM,
                                                   AllocPref::PreferPSRAM, "espnow.msgs.cli");
  if (!msgs) return argWantsJson(argsInput) ? "{\"schema\":1,\"ok\":false,\"error\":\"oom\"}" : "OOM";
  int n = hasMac ? getPeerMessages(mac, msgs, MAXM, since)
                 : getAllMessages(msgs, MAXM, since);

  if (argWantsJson(argsInput)) {
    PSRAM_JSON_DOC(doc);
    doc["schema"] = 1;
    JsonArray arr = doc["messages"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
      JsonObject o = arr.add<JsonObject>();
      char macStr[18];
      formatMacAddressBuf(msgs[i].senderMac, macStr, sizeof(macStr));
      o["seq"]   = (unsigned long)msgs[i].seqNum;
      o["reqId"] = (unsigned long)msgs[i].reqId;
      o["piece"] = msgs[i].piece;
      o["of"]    = msgs[i].pieceTotal;
      o["mac"]  = String(macStr);
      o["name"] = msgs[i].senderName;
      o["msg"]  = msgs[i].message;
      o["enc"]  = msgs[i].encrypted;
      o["ts"]   = (unsigned long)msgs[i].timestamp;
      o["type"] = (int)msgs[i].msgType;
      o["sent"] = msgs[i].isSent;          // direction: true = we sent it, false = received
      o["sendState"] = msgs[i].sendState;  // sent rows: 0 pending,1 delivered,2 timeout,3 failed
    }
    static char* jbuf = nullptr;
    if (!jbuf) jbuf = (char*)ps_alloc(32768, AllocPref::PreferPSRAM, "espnow.messages.json");
    if (!jbuf) return "{\"schema\":1,\"ok\":false,\"error\":\"oom\"}";
    serializeJson(doc, jbuf, 32768);
    return jbuf;
  }

  // Human text fallback (serial console).
  if (n == 0) return "No ESP-NOW messages";
  for (int i = 0; i < n; i++) {
    char macStr[18];
    formatMacAddressBuf(msgs[i].senderMac, macStr, sizeof(macStr));
    BROADCAST_PRINTF("[#%lu] %s%s: %s",
                     (unsigned long)msgs[i].seqNum,
                     msgs[i].senderName[0] ? msgs[i].senderName : macStr,
                     msgs[i].encrypted ? " [enc]" : "",
                     msgs[i].message);
  }
  return "OK";
}

// Columns: name, help, requiresAdmin, handler, usage, voiceCategory, [voiceSubCategory,] voiceTarget
extern const CommandEntry espNowCommands[] = {
  // ---- ESP-NOW Status & Statistics ----
  { "espnowread", "Read ESP-NOW status and configuration.", false, cmd_espnow_status },
  { "espnowstatus", "Show ESP-NOW status and configuration.", false, cmd_espnow_status },
  { "espnowstats", "Show ESP-NOW statistics (messages, errors, etc.).", false, cmd_espnow_stats },
  { "espnowrouterstats", "Show message router statistics and metrics.", false, cmd_espnow_routerstats },
  { "espnowbroadcaststats", "Show broadcast ACK tracking statistics.", false, cmd_espnow_broadcaststats },
  { "espnowresetstats", "Reset ESP-NOW statistics counters.", true, cmd_espnow_resetstats },
  { "espnowsaturation", "Show ESP-NOW link saturation: frames/sec, stream-queue depth, drops, ACK RTT (rolling 30s).", false, cmd_espnow_saturation },
  { "espnowsaturationreset", "Clear the saturation rolling window (use before a stress test).", false, cmd_espnow_saturationreset },

  // ---- ESP-NOW Cryptographic Identity (Phase 3.0/3.3) ----
  { "espnowidentity", "Show long-term Ed25519 identity (MAC, pub key, createdAtSec, regenCount).", false, cmd_espnow_identity },
  { "espnowregenidentity", "Regenerate Ed25519 identity. Requires '--confirm-wipe-all-bonds'.", true, cmd_espnow_regenidentity, "Usage: espnowregenidentity --confirm-wipe-all-bonds" },
  { "espnowkeyex", "Initiate KEY_EX handshake with a peer (runs alongside legacy pairing). (async - handshake completes later; check espnowsessions)", true, cmd_espnow_keyex, "Usage: espnowkeyex <name_or_mac> [<mesh>]\n       Returns OK when KEY_EX_HELLO is sent; the handshake completes asynchronously - inspect with 'espnowsessions' / 'espnowencstatus'." },
  { "espnowprobe", "Reachability probe via KEY_EX. Synchronous, bounded timeout. Reports alive+mesh+firmware in one shot (no plaintext on the wire).", true, cmd_espnow_probe, "Usage: espnowprobe <name_or_mac> [<timeoutMs (50-5000, default 500)>] [<mesh>]" },
  { "espnowsessionopen", "Initiate SESSION handshake (requires prior espnowkeyex). (async - session goes ACTIVE later; check espnowsessions)", true, cmd_espnow_sessionopen, "Usage: espnowsessionopen <name_or_mac> [<mesh>]\n       Returns OK when SESSION_OPEN is sent; the session becomes ACTIVE when CONFIRM arrives - run 'espnowsessions'." },
  { "espnowsessions", "Show in-RAM session state (peer, sessionId, dir, age, counters).", false, cmd_espnow_sessions },
  { "espnowsessionsend", "DIAGNOSTIC: send an AEAD-encrypted CHAT message over an active session (exercises the session-crypto path). NOT executed on the peer and returns no reply - to RUN a command use 'espnowremote'.", true, cmd_espnow_sessionsend, "Usage: espnowsessionsend <name_or_mac> <message>\n       Delivers an encrypted CHAT message (lands in the peer's espnowmessages). It is NOT command execution and no reply comes back.\n       To run a command on the peer: espnowremote <target> <target-user> <target-pass> <command>." },
  { "espnowrekey", "Force immediate SESSION_REKEY for a peer (manual trigger). (async - completes later; check espnowsessions)", true, cmd_espnow_rekey, "Usage: espnowrekey <name_or_mac>\n       Returns OK when REKEY is sent; new keys derive when the peer's REKEY arrives - verify with 'espnowsessions'." },
  { "espnowsubs", "List peers + their event-subscription bitmaps (what they want from us).", false, cmd_espnow_subs },
  { "espnowrequestevents", "Ask a peer to send US only events in <bitmask>. Updates state ON THE PEER. (async - changes peer state, no reply; verify with espnowsubs on the peer)", true, cmd_espnow_requestevents, "Usage: espnowrequestevents <name_or_mac> <bitmask>\n       Returns OK on delivery; this updates the PEER's subscription (no confirmation returns) - run 'espnowsubs' on that peer to verify." },

  // ---- ESP-NOW Initialization & Pairing ----
  { "openespnow", "Initialize ESP-NOW communication.", true, cmd_espnow_init },
  { "closeespnow", "Deinitialize ESP-NOW and free resources.", true, cmd_espnow_deinit },
  { "espnowpair", "Pair ESP-NOW device: 'espnowpair <mac> <name> [mesh]'. (synchronous; local registry add, no remote handshake)", true, cmd_espnow_pair, "Usage: espnowpair <mac> <name> [mesh]" },
  { "espnowunpair", "Unpair ESP-NOW device (also clears its crypto identity): 'espnowunpair <name_or_mac>'.", true, cmd_espnow_unpair, "Usage: espnowunpair <name_or_mac>" },
  { "espnowforget", "Forget a peer's crypto identity + close its session: 'espnowforget <name_or_mac>'.", true, cmd_espnow_forget, "Usage: espnowforget <name_or_mac>" },
  { "espnowlist", "List all paired ESP-NOW devices.", false, cmd_espnow_list },
  { "espnowmessages", "Buffered message history as JSON: 'espnowmessages json [sinceSeq] [mac]' — async results of espnowremote/browse/fetch.", false, cmd_espnow_messages, "Usage: espnowmessages [json] [<sinceSeq>] [<AA:BB:CC:DD:EE:FF>]" },
  
  // ---- ESP-NOW Mesh Configuration ----
  { "espnowmeshstatus", "Show mesh peer health (heartbeats & ACKs).", false, cmd_espnow_meshstatus },
  { "espnowmeshmetrics", "Show mesh routing metrics (forwards, path stats, drops).", false, cmd_espnow_meshmetrics },
  { "espnowmeshes", "Manage multi-mesh slots: 'espnowmeshes [list|add|remove|enable|setdefault|rename|setpassphrase] ...'.", true, cmd_espnow_meshes, "Usage: espnowmeshes list\n       espnowmeshes add <label>          (then set passphrase via 'espnowsetpassphrase <label> <pw>')\n       espnowmeshes remove <label>       (alias: disable)\n       espnowmeshes enable <label>\n       espnowmeshes setdefault <label>\n       espnowmeshes setpassphrase <label> <passphrase>\n       espnowmeshes rename <oldLabel> <newLabel>" },
  { "espnowmode", "Get/set ESP-NOW mode: 'espnowmode [direct|mesh]'.", true, cmd_espnow_mode, "Usage: espnowmode [direct|mesh]" },
  { "espnowmeshttl", "Get/set mesh TTL: 'espnowmeshttl [1-10|adaptive]'.", false, cmd_espnow_meshttl, "Usage: espnowmeshttl [<1..10>|adaptive]" },
  { "espnowsetname", "Get/set device name: 'espnowsetname [name]'.", true, cmd_espnow_setname, "Usage: espnowsetname [<name>]   (<=20 chars; letters, numbers, - and _ only)" },
  { "espnowhbmode", "Get/set heartbeat mode: 'espnowhbmode [public|private]'.", false, cmd_espnow_hbmode, "Usage: espnowhbmode [public|private]" },
  { "espnowmeshrole", "Get/set mesh role: 'espnowmeshrole [worker|master|backup]'.", true, cmd_espnow_meshrole, "Usage: espnowmeshrole [worker|master|backup]" },
  { "espnowmeshmaster", "Get/set master MAC: 'espnowmeshmaster [MAC]'.", true, cmd_espnow_meshmaster, "Usage: espnowmeshmaster [<AA:BB:CC:DD:EE:FF>]" },
  { "espnowmeshbackup", "Get/set backup MAC: 'espnowmeshbackup [MAC]'.", true, cmd_espnow_meshbackup, "Usage: espnowmeshbackup [<AA:BB:CC:DD:EE:FF>]" },
  { "espnowbackupenable", "Enable/disable backup master feature: 'espnowbackupenable [on|off]'.", true, cmd_espnow_backupenable, "Usage: espnowbackupenable [on|off]" },
  { "espnowmeshtopo", "Discover mesh topology (run on the master; role not enforced). (async - read results with espnowtoporesults)", false, cmd_espnow_meshtopo },
  { "espnowtoporesults", "Get topology discovery results.", false, cmd_espnow_toporesults },
  { "espnowtimesync", "Broadcast NTP time to mesh (intended for the master; role not enforced). (async broadcast; delivery only, no reply)", false, cmd_espnow_timesync },
  { "espnowtimestatus", "Show time synchronization status.", false, cmd_espnow_timestatus },
  { "espnowmeshsave", "Manually save mesh peer topology to filesystem.", false, cmd_espnow_meshsave },
  
  // ---- Device Metadata ----
  { "espnowroom", "Get/set device room: 'espnowroom [name]'.", false, cmd_espnow_room, "Usage: espnowroom [Kitchen|Bedroom|...]\n       espnowroom clear" },
  { "espnowzone", "Get/set device zone: 'espnowzone [name]'.", false, cmd_espnow_zone, "Usage: espnowzone [Counter|Door|Ceiling|...]\n       espnowzone clear" },
  { "espnowtags", "Get/set device tags: 'espnowtags [tag1,tag2,...]'.", false, cmd_espnow_tags, "Usage: espnowtags stationary,thermal\n       espnowtags clear" },
  { "espnowfriendlyname", "Get/set friendly display name: 'espnowfriendlyname [name]'.", false, cmd_espnow_friendlyname, "Usage: espnowfriendlyname [<name>]   (<=47 chars)\n       espnowfriendlyname clear" },
  { "espnowstationary", "Get/set stationary flag: 'espnowstationary [0|1]'.", false, cmd_espnow_stationary, "Usage: espnowstationary [on|off|0|1]" },
  { "espnowdeviceinfo", "Show all local device metadata.", false, cmd_espnow_deviceinfo },
  
  // ---- Master Aggregation ----
  { "espnowdevices", "List all mesh devices with room/zone/tags/status: espnowdevices [json].", false, cmd_espnow_devices },
  { "espnowrooms", "List rooms and their devices (aggregated from this node's cached peer metadata).", false, cmd_espnow_rooms },
  { "espnowfind", "Find devices by name, room, or tag: 'espnowfind <query>'.", false, cmd_espnow_find, "Usage: espnowfind <query>" },
  { "espnowroomcmd", "Run command on all devices in a room; user/pass must be valid on EACH target device. (async - replies via espnowmessages json)", true, cmd_espnow_roomcmd, "Usage: espnowroomcmd <room> <target-user> <target-pass> <command>\n       Credentials are checked ON EACH target device, not this one.\n       Returns OK on dispatch; each device's reply arrives later in 'espnowmessages json'." },
  { "espnowtagcmd", "Run command on all devices with a tag; user/pass must be valid on EACH target device. (async - replies via espnowmessages json)", true, cmd_espnow_tagcmd, "Usage: espnowtagcmd <tag> <target-user> <target-pass> <command>\n       Credentials are checked ON EACH target device, not this one.\n       Returns OK on dispatch; each device's reply arrives later in 'espnowmessages json'." },
  
  // ---- ESP-NOW Communication ----
  { "espnowsend", "Send message (auto-routes via mesh if enabled): 'espnowsend [json] <name_or_mac> <message>'. Requires ESP-NOW encryption enabled. (async send; delivery only, no reply)", false, cmd_espnow_send, "Usage: espnowsend [json] <name_or_mac> <message>\n       Requires ESP-NOW encryption (set a mesh passphrase first); plaintext send was removed.\n       Leading 'json' flag returns {schema,ok,msgId} for delivery-status polling.\n       Returns OK on delivery; one-way message, no result comes back." },
  { "espnowbroadcast", "Broadcast message: 'espnowbroadcast <message>'. (async send; delivery only, no reply)", false, cmd_espnow_broadcast, "Usage: espnowbroadcast <message>   (single frame, <= 218 bytes; longer text is NOT fragmented and fails silently)\n       Returns whether the single broadcast frame was transmitted to all peers, NOT a per-device delivery count; no per-device reply." },
  { "espnowsendfile", "Send file: 'espnowsendfile <name_or_mac> \"<filepath>\"'. (synchronous local send; does not confirm peer accepted)", false, cmd_espnow_sendfile, "Usage: espnowsendfile <name_or_mac> \"<filepath>\"\n       Blocks until the file is sent; 'success' means locally transmitted, not that the receiver stored it." },
  { "espnowbrowse", "Browse a peer's files; user/pass are an account ON THE TARGET: 'espnowbrowse <target> <target-user> <target-pass> [\"path\"]'. (async - result via espnowmessages json)", false, cmd_espnow_browse, "Usage: espnowbrowse <target> <target-user> <target-pass> [\"path\"]\n       Credentials are verified ON THE TARGET device, not this one.\n       Returns OK on delivery; the remote listing arrives later - read with 'espnowmessages json' (match the reqId)." },
  { "espnowfetch", "Fetch a file from a peer; user/pass are an account ON THE TARGET: 'espnowfetch <target> <target-user> <target-pass> \"<path>\"'. (async - status via espnowmessages json; file saved on this device)", false, cmd_espnow_fetch, "Usage: espnowfetch <target> <target-user> <target-pass> \"<path>\"\n       Credentials are verified ON THE TARGET device, not this one.\n       Returns OK on delivery; status lands in 'espnowmessages json'; the fetched file is written to this device's filesystem." },
  { "espnowremote", "Execute a command on a peer: 'espnowremote <target> <target-user> <target-pass> <cmd>'. user/pass are an account ON THE TARGET (verified there), not this device. (async - result via espnowmessages json)", false, cmd_espnow_remote, "Usage: espnowremote <target> <target-user> <target-pass> <command>\n       <target-user>/<target-pass> are credentials ON THE TARGET device, not this one.\n       Async: returns a reqId on delivery; read the output later with 'espnowmessages json 0 <target-mac>' (match the reqId)." },
  { "openstream", "Start streaming all output to ESP-NOW caller (admin, remote only).", true, cmd_espnow_startstream },
  { "closestream", "Stop streaming output to ESP-NOW device (admin).", true, cmd_espnow_stopstream },
  { "espnowworker", "Configure worker status reporting: 'espnowworker [show|on|off|interval <ms>|fields <list>]'.", false, cmd_espnow_worker, "Usage: espnowworker [show|on|off|interval <ms>|fields <heap,rssi,thermal,imu>]" },
  { "espnowsensorstream", "Enable/disable sensor data streaming to master (worker only): 'espnowsensorstream <sensor> <on|off>'. (local toggle; streamed data lands on the master's espnowsensorstatus)", false, cmd_espnow_sensorstream, "Usage: espnowsensorstream <thermal|tof|imu|gps|input|fmradio|camera|microphone|rtc|presence|apds> <on|off>\n       Local on/off toggle; the worker then streams to the master, viewable there via 'espnowsensorstatus' / GET /api/sensors/remote." },
  { "espnowsensorstatus", "Show remote sensor cache (master) or worker streaming status (worker).", false, cmd_espnow_sensorstatus },
  { "espnowsensorbroadcast", "Enable/disable all sensor ESP-NOW communication: 'espnowsensorbroadcast <on|off>'.", false, cmd_espnow_sensorbroadcast, "Usage: espnowsensorbroadcast [on|off]" },
  { "espnowusersync", "Enable/disable user credential sync: 'espnowusersync [on|off]'.", true, cmd_espnow_usersync, "Usage: espnowusersync [on|off]" },
  { "espnowrequestmeta", "Request metadata from peer: 'espnowrequestmeta <name_or_mac>'. (async - updates cache; view with espnowdevices)", false, cmd_espnow_requestmeta, "Usage: espnowrequestmeta <name_or_mac>\n       Returns OK on delivery; the peer's name/room/zone/tags arrive later and update the local peer cache shown by 'espnowdevices' / 'espnowrooms' / 'espnowfind'." },
  
#if ENABLE_BONDED_MODE
  // ---- Bond Mode Commands (1:1 handshake relationship) ----
  { "bondconnect", "Connect to bonded peer device: 'bondconnect <mac_or_name>'. (async - bond establishes when peer is seen; watch bondstatus)", false, cmd_bond_connect, "Usage: bondconnect <mac_or_name>\n       Returns immediately; the bond completes when the peer appears via heartbeat - watch 'bondstatus'." },
  { "bonddisconnect", "Disconnect from bonded peer device.", false, cmd_bond_disconnect },
  { "bondstatus", "Show bond mode status and configuration.", false, cmd_bond_status },
  { "bondrole", "Get/set bond mode role: 'bondrole [master|worker]' (no arg shows current role).", false, cmd_bond_role, "Usage: bondrole [master|worker]   (no arg shows current role)" },
  { "bondshowcap", "Show local device capability summary.", false, cmd_bond_showcap },
  { "bondrequestcap", "Request capability summary from bonded peer. (async - remote cap via GET /api/bond/status; note bondshowcap shows LOCAL cap)", false, cmd_bond_requestcap },
  { "bondshowmanifest", "Show local device manifest (UI apps + CLI commands).", false, cmd_bond_showmanifest },
  { "bondrequestmanifest", "Request full manifest from bonded peer. (async - view with bondshowremotemanifest)", false, cmd_bond_requestmanifest },
  { "bondrequestsettings", "Request settings file from bonded peer. (async - cached; read via GET /api/bond/settings)", false, cmd_bond_requestsettings },
  { "bondrequestschema", "Request settings schema from bonded peer. (async - cached; read via GET /api/bond/settings/schema)", false, cmd_bond_requestschema },
  { "bondresync", "Force re-sync of bond state (cap+manifest+settings+schema). Use when UI is stuck on 'Establishing Bond' or peer state looks stale. (async - results populate as they arrive)", false, cmd_bond_resync, "Usage: bondresync [--cap|--manifest|--settings|--schema|--all]\n       Returns OK on dispatch; results arrive over time - view via 'bondshowremotemanifest' and GET /api/bond/status, /api/bond/settings, /api/bond/settings/schema." },
  { "bondshowremotemanifest", "Show cached remote manifest(s): 'bondshowremotemanifest [fwHash]'.", false, cmd_bond_showremotemanifest, "Usage: bondshowremotemanifest [<fwHash>]" },
  { "bondstream", "Toggle bond sensor streaming (works on both roles): 'bondstream <sensor> <on|off>'. WORKER streams its sensor to the bonded master; MASTER commands the bonded worker to start/stop. (local toggle; data lands on the master's espnowsensorstatus)", false, cmd_bond_stream, "Usage: bondstream <sensor> <on|off>\n       bondstream (show status)\n       On a WORKER: streams this device's sensor to the bonded master. On a MASTER: tells the bonded worker to start/stop that sensor.\n       Streamed data is viewable on the master via 'espnowsensorstatus' / GET /api/sensors/remote." },
  { "bondtestsensor", "Test v3 sensor data transmission (worker only - a master cannot send sensor data): 'bondtestsensor [sensor_type]'. (async - frame appears on the master via espnowsensorstatus)", false, cmd_bond_testsensor, "Usage: bondtestsensor [thermal|tof|imu|gps|input|fmradio|rtc|presence]   (worker only)\n       Returns OK on send; the test frame appears on the bonded master's remote-sensor cache ('espnowsensorstatus' / GET /api/sensors/remote)." },
#endif
  
  // ---- ESP-NOW Encryption ----
  { "espnowsetpassphrase", "Set encryption passphrase on a mesh: 'espnowsetpassphrase <mesh> <phrase>'.", true, cmd_espnow_setpassphrase, "Usage: espnowsetpassphrase <mesh> <passphrase>\n       espnowsetpassphrase <mesh> clear" },
  { "espnowencstatus", "Show ESP-NOW encryption status and key fingerprint.", true, cmd_espnow_encstatus },
  { "espnowpairsecure", "Pair device with encryption: 'espnowpairsecure <mac> <name> [mesh]'. (local pair is synchronous; secure channel completes async - see espnowsessions)", true, cmd_espnow_pairsecure, "Usage: espnowpairsecure <mac_address> <device_name> [mesh]\n       Requires a mesh passphrase first - run 'espnowsetpassphrase <mesh> <passphrase>'.\n       The device is added synchronously; KEY_EX then runs asynchronously (~100ms) so the encrypted channel becomes usable shortly after - inspect with 'espnowsessions' / 'espnowencstatus'." },
  
  // ---- ESP-NOW Testing Commands ----
  { "teststreams", "Test topology stream management functions.", false, cmd_test_streams },
  { "testconcurrent", "Test concurrent topology streams (simulated).", false, cmd_test_concurrent },
  { "testcleanup", "Test cleanup of stale topology streams.", false, cmd_test_cleanup },
  { "testfilelock", "Test file transfer lock acquire/release.", false, cmd_test_filelock },
  
  // ---- ESP-NOW Settings ----
  { "espnowenabled", "Enable/disable ESP-NOW (0|1, takes effect after reboot).", true, cmd_espnowenabled, "Usage: espnowenabled <0|1>" },
  { "espnowbuffers", "Show/adjust ESP-NOW buffer sizes: 'espnowbuffers [tx|rx|chunk|filechunk] [value]'.", false, cmd_espnow_buffers, "Usage: espnowbuffers [tx|rx|chunk|filechunk] [<value>]   (tx 1..16, rx 64..512, chunk 100..212, filechunk 100..216)" },
  // ---- ESP-NOW Settings (schema-driven, for web UI save) ----
  { "espnowfirsttimesetup",          "Set first time setup flag: <0|1>", true, cmd_espnow_firsttimesetup, "Usage: espnowfirsttimesetup <0|1>" },
  { "espnowheartbeatinterval",       "Set master heartbeat interval: <1000-60000 ms>", true, cmd_espnow_heartbeatinterval, "Usage: espnowheartbeatinterval <1000..60000>" },
  { "espnowfailovertimeout",         "Set failover timeout: <5000-120000 ms>", true, cmd_espnow_failovertimeout, "Usage: espnowfailovertimeout <5000..120000>" },
  { "espnowworkerstatusinterval",    "Set worker status interval: <5000-120000 ms>", true, cmd_espnow_workerstatusinterval, "Usage: espnowworkerstatusinterval <5000..120000>" },
  { "espnowtopodiscoveryinterval",   "Set topology discovery interval: <0-300000 ms>", true, cmd_espnow_topodiscoveryinterval, "Usage: espnowtopodiscoveryinterval <0..300000>" },
  { "espnowtopoautorefresh",         "Set auto refresh topology: <0|1>", true, cmd_espnow_topoautorefresh, "Usage: espnowtopoautorefresh <0|1>" },
  { "espnowheartbeatbroadcast",      "Set heartbeat broadcast: <0|1>", true, cmd_espnow_heartbeatbroadcast, "Usage: espnowheartbeatbroadcast <0|1>" },
  { "espnowmeshadaptivettl",         "Set adaptive TTL: <0|1>", true, cmd_espnow_meshadaptivettl, "Usage: espnowmeshadaptivettl <0|1>" },
  { "espnowmeshpeermax",             "Set max peer slots: <1-16> (reboot required)", true, cmd_espnow_meshpeermax, "Usage: espnowmeshpeermax <1..16>" },
  { "espnowsensorbroadcastinterval", "Set sensor broadcast interval: <100-10000 ms>", true, cmd_espnow_sensorbroadcastinterval, "Usage: espnowsensorbroadcastinterval <100..10000>" },
  { "espnowtxqueuesize",             "Set TX queue size: <1-16>", true, cmd_espnow_txqueuesize, "Usage: espnowtxqueuesize <1..16>" },
  { "espnowrxbuffersize",            "Set RX buffer size: <64-512>", true, cmd_espnow_rxbuffersize, "Usage: espnowrxbuffersize <64..512>" },
  { "espnowchunksize",               "Set chunk size: <100-212>", true, cmd_espnow_chunksize, "Usage: espnowchunksize <100..212>" },
  { "espnowfilechunksize",           "Set file chunk size: <100-216>", true, cmd_espnow_filechunksize, "Usage: espnowfilechunksize <100..216>" },
#if ENABLE_BONDED_MODE
  { "espnowbondmodeenabled",         "Enable/disable bond mode: <0|1>", true, cmd_espnow_bondmodeenabled, "Usage: espnowbondmodeenabled <0|1>" },
  { "espnowbondpeermac",             "Set bond peer MAC address", true, cmd_espnow_bondpeermac, "Usage: espnowbondpeermac <AA:BB:CC:DD:EE:FF>" },
  { "bondstreamthermal",             "Set auto-stream thermal: <0|1>", true, cmd_espnow_bondstreamthermal, "Usage: bondstreamthermal <0|1>" },
  { "bondstreamtof",                 "Set auto-stream ToF: <0|1>", true, cmd_espnow_bondstreamtof, "Usage: bondstreamtof <0|1>" },
  { "bondstreamimu",                 "Set auto-stream IMU: <0|1>", true, cmd_espnow_bondstreamimu, "Usage: bondstreamimu <0|1>" },
  { "bondstreamgps",                 "Set auto-stream GPS: <0|1>", true, cmd_espnow_bondstreamgps, "Usage: bondstreamgps <0|1>" },
  { "bondstreaminput",             "Set auto-stream input device: <0|1>", true, cmd_espnow_bondstreaminput, "Usage: bondstreaminput <0|1>" },
  { "bondstreamfmradio",             "Set auto-stream FM radio: <0|1>", true, cmd_espnow_bondstreamfmradio, "Usage: bondstreamfmradio <0|1>" },
  { "bondstreamrtc",                 "Set auto-stream RTC: <0|1>", true, cmd_espnow_bondstreamrtc, "Usage: bondstreamrtc <0|1>" },
  { "bondstreampresence",            "Set auto-stream presence: <0|1>", true, cmd_espnow_bondstreampresence, "Usage: bondstreampresence <0|1>" },
#endif
};

extern const size_t espNowCommandsCount = sizeof(espNowCommands) / sizeof(espNowCommands[0]);

// Registration handled by gCommandModules[] in System_Utils.cpp

// ============================================================================
// ESP-NOW Settings Module (for modular settings registry)
// ============================================================================

// Columns: jsonKey, type, valuePtr, intDefault, floatDefault, stringDefault, minVal, maxVal, label, options[, isSecret[, group, cmdKey]]
static const SettingEntry espnowSettingEntries[] = {
  { "enabled",                    SETTING_BOOL,   &gSettings.espnowenabled,              false, 0, nullptr, 0, 1, "ESP-NOW Enabled", nullptr, false, nullptr, "espnowenabled" },
  { "mesh", SETTING_BOOL, &gSettings.espnowmesh, false, 0, nullptr, 0, 1, "Mesh Mode", nullptr, false, "mesh", "espnowmode" },
  { "userSyncEnabled",            SETTING_BOOL,   &gSettings.espnowUserSyncEnabled,      false, 0, nullptr, 0, 1, "User Sync Enabled", nullptr, false, nullptr, "espnowusersync" },
  { "captureToSd", SETTING_BOOL, &gSettings.espnowCaptureToSd, false, 0, nullptr, 0, 1, "Capture ESP-NOW traffic to SD card", nullptr, false, "capture", "espnowcapturetosd" },
  { "captureSkipHeartbeats", SETTING_BOOL, &gSettings.espnowCaptureSkipHeartbeats, true, 0, nullptr, 0, 1, "Skip heartbeat frames in capture", nullptr, false, "capture", "espnowcaptureskipheartbeats" },
  { "deviceName", SETTING_STRING, &gSettings.espnowDeviceName, 0, 0, "", 0, 0, "Device Name", nullptr, false, "identity", "espnowsetname" },
  { "room", SETTING_STRING, &gSettings.espnowRoom, 0, 0, "", 0, 0, "Room", nullptr, false, "identity", "espnowroom" },
  { "zone", SETTING_STRING, &gSettings.espnowZone, 0, 0, "", 0, 0, "Zone", nullptr, false, "identity", "espnowzone" },
  { "tags", SETTING_STRING, &gSettings.espnowTags, 0, 0, "", 0, 0, "Tags", nullptr, false, "identity", "espnowtags" },
  { "friendlyName", SETTING_STRING, &gSettings.espnowFriendlyName, 0, 0, "", 0, 0, "Friendly Name", nullptr, false, "identity", "espnowfriendlyname" },
  { "stationary", SETTING_BOOL, &gSettings.espnowStationary, false, 0, nullptr, 0, 1, "Stationary", nullptr, false, "identity", "espnowstationary" },
  { "firstTimeSetup",             SETTING_BOOL,   &gSettings.espnowFirstTimeSetup,       false, 0, nullptr, 0, 1, "First Time Setup", nullptr, false, nullptr, "espnowfirsttimesetup" },
  // meshRole is uint8_t — must be SETTING_U8. Same for meshTTL/meshPeerMax/
  // bondRole and the uint16_t buffer-size fields below. Untyped SETTING_INT
  // would write 4 bytes through a uint8/uint16 pointer, corrupting the
  // adjacent struct members (root cause of the 2026-05-18 crash).
  { "meshRole", SETTING_U8, &gSettings.meshRole, 0, 0, nullptr, 0, 2, "Mesh Role", "0:Worker,1:Master,2:Backup Master", false, "mesh", "espnowmeshrole" },
  { "masterMAC", SETTING_STRING, &gSettings.meshMasterMAC, 0, 0, "", 0, 0, "Master MAC", nullptr, false, "mesh", "espnowmeshmaster" },
  { "backupMAC", SETTING_STRING, &gSettings.meshBackupMAC, 0, 0, "", 0, 0, "Backup MAC", nullptr, false, "mesh", "espnowmeshbackup" },
  { "backupEnabled", SETTING_BOOL, &gSettings.meshBackupEnabled, false, 0, nullptr, 0, 1, "Backup Master Enabled", nullptr, false, "mesh", "espnowbackupenable" },
  { "masterHeartbeatInterval", SETTING_INT, &gSettings.meshMasterHeartbeatInterval, 10000, 0, nullptr, 1000, 60000, "Heartbeat Interval (ms)", nullptr, false, "mesh", "espnowheartbeatinterval" },
  { "failoverTimeout", SETTING_INT, &gSettings.meshFailoverTimeout, 20000, 0, nullptr, 5000, 120000, "Failover Timeout (ms)", nullptr, false, "mesh", "espnowfailovertimeout" },
  { "workerStatusInterval", SETTING_INT, &gSettings.meshWorkerStatusInterval, 30000, 0, nullptr, 5000, 120000, "Worker Status Interval (ms)", nullptr, false, "mesh", "espnowworkerstatusinterval" },
  { "topoDiscoveryInterval", SETTING_INT, &gSettings.meshTopoDiscoveryInterval, 0, 0, nullptr, 0, 300000, "Topo Discovery Interval (ms)", nullptr, false, "mesh", "espnowtopodiscoveryinterval" },
  { "topoAutoRefresh", SETTING_BOOL, &gSettings.meshTopoAutoRefresh, false, 0, nullptr, 0, 1, "Auto Refresh Topology", nullptr, false, "mesh", "espnowtopoautorefresh" },
  { "heartbeatBroadcast", SETTING_BOOL, &gSettings.meshHeartbeatBroadcast, true, 0, nullptr, 0, 1, "Heartbeat Broadcast", nullptr, false, "mesh", "espnowheartbeatbroadcast" },
  { "meshTTL", SETTING_U8, &gSettings.meshTTL, 3, 0, nullptr, 1, 10, "TTL", nullptr, false, "mesh", "espnowmeshttl" },
  { "meshAdaptiveTTL", SETTING_BOOL, &gSettings.meshAdaptiveTTL, false, 0, nullptr, 0, 1, "Adaptive TTL", nullptr, false, "mesh", "espnowmeshadaptivettl" },
  { "meshPeerMax", SETTING_U8, &gSettings.meshPeerMax, 8, 0, nullptr, 1, 16, "Max Peer Slots (reboot)", nullptr, false, "mesh", "espnowmeshpeermax" },
  { "sensorBroadcastIntervalMs", SETTING_U16, &gSettings.sensorBroadcastIntervalMs, 1000, 0, nullptr, 100, 10000, "Sensor Broadcast Interval (ms)", nullptr, false, "mesh", "espnowsensorbroadcastinterval" },
#if ENABLE_BONDED_MODE
  { "bondModeEnabled", SETTING_BOOL, &gSettings.bondModeEnabled, false, 0, nullptr, 0, 1, "Bond Mode Enabled", nullptr, false, "bond", "espnowbondmodeenabled" },
  { "bondRole", SETTING_U8, &gSettings.bondRole, 0, 0, nullptr, 0, 1, "Bond Role", "0:Worker (compute/network),1:Master (display/gamepad)", false, "bond", "bondrole" },
  { "bondPeerMac", SETTING_STRING, &gSettings.bondPeerMac, 0, 0, "", 0, 0, "Bond Peer MAC", nullptr, false, "bond", "espnowbondpeermac" },
  { "bondStreamThermal", SETTING_BOOL, &gSettings.bondStreamThermal, false, 0, nullptr, 0, 1, "Auto-stream Thermal", nullptr, false, "bond", "bondstreamthermal" },
  { "bondStreamTof", SETTING_BOOL, &gSettings.bondStreamTof, false, 0, nullptr, 0, 1, "Auto-stream ToF", nullptr, false, "bond", "bondstreamtof" },
  { "bondStreamImu", SETTING_BOOL, &gSettings.bondStreamImu, false, 0, nullptr, 0, 1, "Auto-stream IMU", nullptr, false, "bond", "bondstreamimu" },
  { "bondStreamGps", SETTING_BOOL, &gSettings.bondStreamGps, false, 0, nullptr, 0, 1, "Auto-stream GPS", nullptr, false, "bond", "bondstreamgps" },
  { "bondStreamInput", SETTING_BOOL, &gSettings.bondStreamInput, false, 0, nullptr, 0, 1, "Auto-stream Input Device", nullptr, false, "bond", "bondstreaminput" },
  { "bondStreamFmradio", SETTING_BOOL, &gSettings.bondStreamFmradio, false, 0, nullptr, 0, 1, "Auto-stream FM Radio", nullptr, false, "bond", "bondstreamfmradio" },
  { "bondStreamRtc", SETTING_BOOL, &gSettings.bondStreamRtc, false, 0, nullptr, 0, 1, "Auto-stream RTC", nullptr, false, "bond", "bondstreamrtc" },
  { "bondStreamPresence", SETTING_BOOL, &gSettings.bondStreamPresence, false, 0, nullptr, 0, 1, "Auto-stream Presence", nullptr, false, "bond", "bondstreampresence" },
#endif
  // Buffer size settings (requires reinit to take effect). All four fields
  // are uint16_t — drop the explicit (int*) casts and use SETTING_U16 so the
  // dispatch writes the correct 2 bytes instead of overflowing into the
  // adjacent uint16.
  { "txQueueSize", SETTING_U16, &gSettings.espnowTxQueueSize, 8, 0, nullptr, 1, 16, "TX Queue Size", nullptr, false, "buffers", "espnowtxqueuesize" },
  { "rxBufferSize", SETTING_U16, &gSettings.espnowRxBufferSize, 256, 0, nullptr, 64, 512, "RX Buffer Size", nullptr, false, "buffers", "espnowrxbuffersize" },
  { "chunkSize", SETTING_U16, &gSettings.espnowChunkSize, 200, 0, nullptr, 100, 212, "Chunk Size", nullptr, false, "buffers", "espnowchunksize" },
  { "fileChunkSize", SETTING_U16, &gSettings.espnowFileChunkSize, 216, 0, nullptr, 100, 216, "File Chunk Size", nullptr, false, "buffers", "espnowfilechunksize" }
};

// Helper: find an ESP-NOW setting entry by jsonKey
static const SettingEntry* findEspnowEntry(const char* key) {
  for (size_t i = 0; i < sizeof(espnowSettingEntries)/sizeof(espnowSettingEntries[0]); i++) {
    if (strcmp(espnowSettingEntries[i].jsonKey, key) == 0) return &espnowSettingEntries[i];
  }
  return nullptr;
}

// Macro to generate CLI command handlers that delegate to handleSettingCommand
#define ESPNOW_SETTING_CMD(func, jsonKey) \
  const char* func(const String& a) { \
    RETURN_VALID_IF_VALIDATE_CSTR(); \
    return handleSettingCommand(findEspnowEntry(jsonKey), a); \
  }

ESPNOW_SETTING_CMD(cmd_espnow_firsttimesetup, "firstTimeSetup")
ESPNOW_SETTING_CMD(cmd_espnow_heartbeatinterval, "masterHeartbeatInterval")
ESPNOW_SETTING_CMD(cmd_espnow_failovertimeout, "failoverTimeout")
ESPNOW_SETTING_CMD(cmd_espnow_workerstatusinterval, "workerStatusInterval")
ESPNOW_SETTING_CMD(cmd_espnow_topodiscoveryinterval, "topoDiscoveryInterval")
ESPNOW_SETTING_CMD(cmd_espnow_topoautorefresh, "topoAutoRefresh")
ESPNOW_SETTING_CMD(cmd_espnow_heartbeatbroadcast, "heartbeatBroadcast")
ESPNOW_SETTING_CMD(cmd_espnow_meshadaptivettl, "meshAdaptiveTTL")
ESPNOW_SETTING_CMD(cmd_espnow_meshpeermax, "meshPeerMax")
ESPNOW_SETTING_CMD(cmd_espnow_sensorbroadcastinterval, "sensorBroadcastIntervalMs")
ESPNOW_SETTING_CMD(cmd_espnow_txqueuesize, "txQueueSize")
ESPNOW_SETTING_CMD(cmd_espnow_rxbuffersize, "rxBufferSize")
ESPNOW_SETTING_CMD(cmd_espnow_chunksize, "chunkSize")
ESPNOW_SETTING_CMD(cmd_espnow_filechunksize, "fileChunkSize")
#if ENABLE_BONDED_MODE
ESPNOW_SETTING_CMD(cmd_espnow_bondmodeenabled, "bondModeEnabled")
ESPNOW_SETTING_CMD(cmd_espnow_bondpeermac, "bondPeerMac")
ESPNOW_SETTING_CMD(cmd_espnow_bondstreamthermal, "bondStreamThermal")
ESPNOW_SETTING_CMD(cmd_espnow_bondstreamtof, "bondStreamTof")
ESPNOW_SETTING_CMD(cmd_espnow_bondstreamimu, "bondStreamImu")
ESPNOW_SETTING_CMD(cmd_espnow_bondstreamgps, "bondStreamGps")
ESPNOW_SETTING_CMD(cmd_espnow_bondstreaminput, "bondStreamInput")
ESPNOW_SETTING_CMD(cmd_espnow_bondstreamfmradio, "bondStreamFmradio")
ESPNOW_SETTING_CMD(cmd_espnow_bondstreamrtc, "bondStreamRtc")
ESPNOW_SETTING_CMD(cmd_espnow_bondstreampresence, "bondStreamPresence")
#endif

bool isEspNowInitialized() {
  return gEspNow && gEspNow->initialized;
}

// Columns: name, jsonSection, entries, count, isConnected, description
extern const SettingsModule espnowSettingsModule = {
  "espnow", "network.espnow", espnowSettingEntries,
  sizeof(espnowSettingEntries) / sizeof(espnowSettingEntries[0]),
  isEspNowInitialized,
  "ESP-NOW mesh networking"
};

// Module registered explicitly by registerAllSettingsModules() in System_Settings.cpp

// ============================================================================
// ESP-NOW User Sync Toggle Command (merged from System_ESPNow_UserSync.cpp)
// ============================================================================

/**
 * Toggle user sync feature: espnow usersync [on|off]
 */
const char* cmd_espnow_usersync(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  
  if (!ensureDebugBuffer()) return "Error: Debug buffer unavailable";
  
  String args = argsInput;
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
    return "Error: invalid arguments — Usage: espnowusersync [on|off]";
  }
}

// ============================================================================
// Per-Device Message Buffer Management (merged from espnow_message_buffer.cpp)
// ============================================================================

// Helper: Grow peer history array when capacity is reached
static bool growPeerHistoryArray() {
  if (!gEspNow || !gEspNow->peerMessageHistories) return false;
  
  int oldCapacity = gEspNow->peerHistoryCapacity;
  int newCapacity = oldCapacity + PEER_HISTORY_GROWTH_INCREMENT;
  
  // Cap at gMeshPeerSlots (max configured peer limit)
  if (newCapacity > gMeshPeerSlots) {
    newCapacity = gMeshPeerSlots;
  }
  
  // Already at max capacity
  if (newCapacity <= oldCapacity) {
    return false;
  }
  
  DEBUG_ESPNOWF("[ESP-NOW] Growing peer history: %d -> %d slots", oldCapacity, newCapacity);
  
  // Allocate new larger array
  size_t newSize = sizeof(PeerMessageHistory) * newCapacity;
  PeerMessageHistory* newArray = (PeerMessageHistory*)ps_alloc(newSize, AllocPref::PreferPSRAM, "espnow.msgHist.grow");
  
  if (!newArray) {
    ERROR_ESPNOWF("[ESP-NOW] Failed to grow peer history array (%u bytes)", (unsigned)newSize);
    return false;
  }
  
  // Initialize new array with placement new
  for (int i = 0; i < newCapacity; i++) {
    new (&newArray[i]) PeerMessageHistory();
  }
  
  // Copy existing entries
  for (int i = 0; i < oldCapacity; i++) {
    if (gEspNow->peerMessageHistories[i].active) {
      // Copy the entire structure (includes MAC, messages, counters)
      memcpy(&newArray[i], &gEspNow->peerMessageHistories[i], sizeof(PeerMessageHistory));
    }
  }
  
  // Free old array and switch to new one
  free(gEspNow->peerMessageHistories);
  gEspNow->peerMessageHistories = newArray;
  gEspNow->peerHistoryCapacity = newCapacity;
  
  BROADCAST_PRINTF("[ESP-NOW] Peer history expanded to %d slots (~%u KB)",
                   newCapacity, (unsigned)(newSize / 1024));
  
  return true;
}

// Helper: Find or create peer message history for a given MAC address
PeerMessageHistory* findOrCreatePeerHistory(uint8_t* peerMac) {
  if (!gEspNow || !gEspNow->peerMessageHistories) return nullptr;
  
  // First, try to find existing history for this peer
  for (int i = 0; i < gEspNow->peerHistoryCapacity; i++) {
    PeerMessageHistory& history = gEspNow->peerMessageHistories[i];
    if (history.active && memcmp(history.peerMac, peerMac, 6) == 0) {
      return &history;
    }
  }
  
  // Not found, try to create new entry in existing capacity
  for (int i = 0; i < gEspNow->peerHistoryCapacity; i++) {
    PeerMessageHistory& history = gEspNow->peerMessageHistories[i];
    if (!history.active) {
      memcpy(history.peerMac, peerMac, 6);
      history.head = 0;
      history.tail = 0;
      history.count = 0;
      history.active = true;
      gEspNow->peerHistoryCount++;
      DEBUG_ESPNOWF("[ESP-NOW] Created peer history slot %d/%d for %02X:%02X:%02X:%02X:%02X:%02X",
                    gEspNow->peerHistoryCount, gEspNow->peerHistoryCapacity,
                    peerMac[0], peerMac[1], peerMac[2], peerMac[3], peerMac[4], peerMac[5]);
      return &history;
    }
  }
  
  // No free slots - try to grow the array
  if (growPeerHistoryArray()) {
    // Retry allocation in the newly grown array
    for (int i = 0; i < gEspNow->peerHistoryCapacity; i++) {
      PeerMessageHistory& history = gEspNow->peerMessageHistories[i];
      if (!history.active) {
        memcpy(history.peerMac, peerMac, 6);
        history.head = 0;
        history.tail = 0;
        history.count = 0;
        history.active = true;
        gEspNow->peerHistoryCount++;
        return &history;
      }
    }
  }
  
  // Still no free slots (growth failed or at max capacity)
  return nullptr;
}

// Store a message in the per-device buffer
// Fill one ring slot and advance head/tail/count. Shared by the received-store
// and sent-store paths so the two rings behave identically (only the ring +
// direction differ). `keyMac` becomes the record's senderMac — the conversation
// key reported as the JSON `mac` (the peer on both directions); `fragment`/
// `fragLen` is ONE fragment's text.
static void espnowStoreInRing(
  ReceivedTextMessage* ring, uint8_t& head, uint8_t& tail, uint8_t& count,
  const uint8_t* keyMac, const char* name,
  const char* fragment, size_t fragLen,
  bool encrypted, LogMessageType msgType,
  uint32_t reqId, uint8_t piece, uint8_t pieceTotal, bool isSent
) {
  ReceivedTextMessage& slot = ring[head];

  memcpy(slot.senderMac, keyMac, 6);
  strncpy(slot.senderName, name ? name : "", 31);
  slot.senderName[31] = '\0';

  size_t copyLen = fragLen < sizeof(slot.message) - 1 ? fragLen : sizeof(slot.message) - 1;
  memcpy(slot.message, fragment, copyLen);
  slot.message[copyLen] = '\0';

  slot.timestamp = millis();
  slot.encrypted = encrypted;
  slot.seqNum = ++gEspNow->globalMessageSeqNum;
  slot.reqId = reqId;
  slot.piece = piece;
  slot.pieceTotal = pieceTotal;
  slot.msgType = msgType;
  slot.active = true;
  slot.isSent = isSent;
  slot.sendState = 0;  // SEND_STATUS_PENDING; terminal state stamped later for sent rows

  head = (head + 1) % MESSAGES_PER_DEVICE;
  if (count < MESSAGES_PER_DEVICE) count++;       // grow until full
  else tail = (tail + 1) % MESSAGES_PER_DEVICE;   // then drop oldest
}

// Split `message` into <=200 B pieces sharing `reqId` and store each into `ring`
// with piece/of set. The slot caps at 256 B, so a single store of long content
// (a command result, a long send) would truncate; chunking lets the client
// reassemble by reqId. Shared by the sent and received chunked-store wrappers.
// A short message is a single 1-of-1 piece.
static void espnowStoreChunked(
  ReceivedTextMessage* ring, uint8_t& head, uint8_t& tail, uint8_t& count,
  const uint8_t* keyMac, const char* name, const char* message,
  bool encrypted, LogMessageType msgType, uint32_t reqId, bool isSent
) {
  const size_t FRAG = 200;  // slot holds 256; keep a margin
  size_t total = message ? strlen(message) : 0;
  uint8_t pieceTotal = (total <= FRAG) ? 1 : (uint8_t)((total + FRAG - 1) / FRAG);
  if (pieceTotal == 0) pieceTotal = 1;
  for (uint8_t p = 0; p < pieceTotal; p++) {
    size_t off = (size_t)p * FRAG;
    size_t len = (total - off) < FRAG ? (total - off) : FRAG;
    espnowStoreInRing(ring, head, tail, count, keyMac, name, message + off, len,
                      encrypted, msgType, reqId, (uint8_t)(p + 1), pieceTotal, isSent);
  }
}

bool storeMessageInPeerHistory(
  uint8_t* peerMac,
  const char* peerName,
  const char* message,
  bool encrypted,
  LogMessageType msgType,
  uint32_t reqId,
  uint8_t piece,
  uint8_t pieceTotal
) {
  if (!gEspNow) return false;

  PeerMessageHistory* history = findOrCreatePeerHistory(peerMac);
  if (!history) {
    broadcastOutput("[ESP-NOW] ERROR: No free peer history slots");
    return false;
  }

  espnowStoreInRing(history->messages, history->head, history->tail, history->count,
                    peerMac, peerName, message, strlen(message),
                    encrypted, msgType, reqId, piece, pieceTotal, /*isSent=*/false);
  return true;
}

bool storeSentMessageInPeerHistory(uint8_t* peerMac, const char* message, uint32_t msgId) {
  if (!gEspNow || !message) return false;

  PeerMessageHistory* history = findOrCreatePeerHistory(peerMac);
  if (!history) {
    broadcastOutput("[ESP-NOW] ERROR: No free peer history slots");
    return false;
  }

  // Split into ≤200 B pieces sharing msgId (shared chunker) so the collapse view
  // reassembles sent the same way and long sends aren't truncated. name empty
  // (sent rows show delivery status, not a sender name); sends are always
  // encrypted post-2026.
  espnowStoreChunked(history->sent, history->sentHead, history->sentTail, history->sentCount,
                     peerMac, "", message, /*encrypted=*/true, MSG_TEXT, msgId, /*isSent=*/true);
  return true;
}

bool storeReceivedMessageChunked(uint8_t* peerMac, const char* peerName, const char* message,
                                 bool encrypted, LogMessageType msgType, uint32_t reqId) {
  if (!gEspNow || !message) return false;

  PeerMessageHistory* history = findOrCreatePeerHistory(peerMac);
  if (!history) {
    broadcastOutput("[ESP-NOW] ERROR: No free peer history slots");
    return false;
  }

  // Received long content (e.g. a remote command's result) chunked the same way
  // as sent text, so a multi-hundred-byte result isn't truncated at the 256 B
  // slot. The client reassembles by reqId.
  espnowStoreChunked(history->messages, history->head, history->tail, history->count,
                     peerMac, peerName, message, encrypted, msgType, reqId, /*isSent=*/false);
  return true;
}

void espnowUpdateSentDeliveryState(const uint8_t* peerMac, uint32_t msgId, uint8_t state) {
  if (!gEspNow || !peerMac || msgId == 0) return;
  // Don't create a peer slot just to update — only stamp if history exists.
  PeerMessageHistory* history = nullptr;
  for (int i = 0; i < gEspNow->peerHistoryCapacity; i++) {
    if (gEspNow->peerMessageHistories[i].active &&
        memcmp(gEspNow->peerMessageHistories[i].peerMac, peerMac, 6) == 0) {
      history = &gEspNow->peerMessageHistories[i];
      break;
    }
  }
  if (!history) return;
  // Stamp every fragment of this message (all share msgId in reqId).
  for (int i = 0; i < MESSAGES_PER_DEVICE; i++) {
    ReceivedTextMessage& slot = history->sent[i];
    if (slot.active && slot.isSent && slot.reqId == msgId) slot.sendState = state;
  }
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

// Get all messages for a specific peer (for web UI API). Merges the received and
// sent rings into one seqNum-ordered timeline; each record's isSent gives direction.
// Two-pointer merge of one peer's received + sent rings by seqNum, appending to
// out[] from index `copied` up to maxMessages, keeping only seq > sinceSeq.
// Both rings are stored oldest→newest (tail→head) in strictly ascending global
// seq, so a linear merge yields ascending output. CRITICAL: the page cap is
// applied AFTER interleaving — never by draining the received ring first — so a
// sent row never gets starved by a received row of higher seq. (The old code
// filled the cap from messages[] first, advancing the poller's cursor past the
// sent rows below that high-water; those sent messages — your own 2/4/6 sends —
// then sat permanently below `sinceSeq` and never came back.) Returns new count.
static int mergePeerRingsBySeq(const PeerMessageHistory& h, ReceivedTextMessage* out,
                               int copied, int maxMessages, uint32_t sinceSeq) {
  int ri = 0, si = 0;
  while (copied < maxMessages) {
    // Advance each cursor to its next live, newer-than-sinceSeq record. Stale /
    // already-seen rows are the low-seq ones at the front, so this is O(1) after
    // the first call.
    while (ri < h.count) {
      uint8_t idx = (h.tail + ri) % MESSAGES_PER_DEVICE;
      if (h.messages[idx].active && h.messages[idx].seqNum > sinceSeq) break;
      ri++;
    }
    while (si < h.sentCount) {
      uint8_t idx = (h.sentTail + si) % MESSAGES_PER_DEVICE;
      if (h.sent[idx].active && h.sent[idx].seqNum > sinceSeq) break;
      si++;
    }
    bool haveR = ri < h.count, haveS = si < h.sentCount;
    if (!haveR && !haveS) break;

    const ReceivedTextMessage* pick;
    if (haveR && haveS) {
      uint8_t ridx = (h.tail + ri) % MESSAGES_PER_DEVICE;
      uint8_t sidx = (h.sentTail + si) % MESSAGES_PER_DEVICE;
      if (h.messages[ridx].seqNum <= h.sent[sidx].seqNum) { pick = &h.messages[ridx]; ri++; }
      else                                                { pick = &h.sent[sidx];     si++; }
    } else if (haveR) {
      uint8_t ridx = (h.tail + ri) % MESSAGES_PER_DEVICE; pick = &h.messages[ridx]; ri++;
    } else {
      uint8_t sidx = (h.sentTail + si) % MESSAGES_PER_DEVICE; pick = &h.sent[sidx]; si++;
    }
    memcpy(&out[copied++], pick, sizeof(ReceivedTextMessage));
  }
  return copied;
}

int getPeerMessages(uint8_t* peerMac, ReceivedTextMessage* outMessages, int maxMessages, uint32_t sinceSeq) {
  if (!gEspNow || !outMessages) return 0;

  PeerMessageHistory* history = findOrCreatePeerHistory(peerMac);
  if (!history) return 0;

  // One peer = one interleaved timeline; output is already ascending by seqNum.
  return mergePeerRingsBySeq(*history, outMessages, 0, maxMessages, sinceSeq);
}

// Get all messages from all peers (for global view)
int getAllMessages(ReceivedTextMessage* outMessages, int maxMessages, uint32_t sinceSeq) {
  if (!gEspNow || !outMessages || !gEspNow->peerMessageHistories) return 0;
  
  int copied = 0;

  // Per peer, interleave its received + sent rings by seq via the shared merge
  // (so a peer's sent rows aren't starved by its received rows — same fix as
  // getPeerMessages). The cross-peer cap still fills peer-by-peer, so a very
  // large multi-peer history can still miss the true global-newest — documented
  // limitation, fine for small meshes.
  for (int p = 0; p < gEspNow->peerHistoryCapacity && copied < maxMessages; p++) {
    PeerMessageHistory& history = gEspNow->peerMessageHistories[p];
    if (!history.active) continue;
    copied = mergePeerRingsBySeq(history, outMessages, copied, maxMessages, sinceSeq);
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

// Collapse a peer's per-fragment records into one zero-copy reference per logical
// message (see CollapsedMsgRef in the header for the contract + rationale). Walks
// the ring chronologically; fragments of one message share a reqId and are stored
// consecutively, so a linear group-merge is correct. No text is copied — each
// output entry points at the message's first present fragment in the live ring.
// Collapse one ring (received or sent) into logical-message refs, appending to
// out[] starting at index `startW`. Group-merge only searches refs added by THIS
// call (>= startW) and also gates on isSent, so a received and a sent fragment
// that happen to share a reqId never merge. Returns the new write count.
static int espnowCollapseRing(const ReceivedTextMessage* ring, uint8_t tail, uint8_t count,
                              CollapsedMsgRef* out, int startW, int maxN) {
  int w = startW;
  for (int i = 0; i < count; i++) {
    uint8_t idx = (tail + i) % MESSAGES_PER_DEVICE;
    const ReceivedTextMessage& msg = ring[idx];
    if (!msg.active) continue;

    // Multi-fragment text: merge into an existing group with the same reqId.
    // reqId == 0 (unsolicited / file events) and single-frame text never merge —
    // each is its own logical message.
    if (msg.reqId != 0 && msg.pieceTotal > 1) {
      int g = -1;
      for (int k = startW; k < w; k++) {
        if (out[k].reqId == msg.reqId && out[k].isSent == msg.isSent &&
            memcmp(out[k].head->senderMac, msg.senderMac, 6) == 0) { g = k; break; }
      }
      if (g >= 0) {
        if (out[g].partsPresent < 255) out[g].partsPresent++;
        // Prefer the lowest-numbered present fragment as the preview head, so the
        // truncated list line shows the true start of the message.
        if (msg.piece < out[g].head->piece) out[g].head = &msg;
        if (msg.pieceTotal > out[g].partsTotal) out[g].partsTotal = msg.pieceTotal;
        continue;
      }
    }

    if (w >= maxN) break;  // caller buffer full; remaining (older) messages dropped
    out[w].head = &msg;
    out[w].reqId = msg.reqId;
    out[w].partsPresent = 1;
    out[w].partsTotal = (msg.pieceTotal > 1) ? msg.pieceTotal : 1;
    out[w].isSent = msg.isSent;
    w++;
  }
  return w;
}

int espnowCollapsedPeerMessages(uint8_t* peerMac, CollapsedMsgRef* out, int maxN) {
  if (!gEspNow || !out || maxN <= 0) return 0;

  PeerMessageHistory* history = findOrCreatePeerHistory(peerMac);
  if (!history || history->count == 0) return 0;

  return espnowCollapseRing(history->messages, history->tail, history->count, out, 0, maxN);
}

int espnowGetConversation(uint8_t* peerMac, CollapsedMsgRef* out, int maxN) {
  if (!gEspNow || !out || maxN <= 0) return 0;

  PeerMessageHistory* history = findOrCreatePeerHistory(peerMac);
  if (!history) return 0;

  // Collapse both rings into one buffer, then order the combined set oldest→newest
  // by timestamp so callers see a single interleaved sent/received conversation.
  int w = espnowCollapseRing(history->messages, history->tail, history->count, out, 0, maxN);
  w = espnowCollapseRing(history->sent, history->sentTail, history->sentCount, out, w, maxN);

  for (int i = 0; i < w - 1; i++) {
    for (int j = 0; j < w - i - 1; j++) {
      if (out[j].head->timestamp > out[j + 1].head->timestamp) {
        CollapsedMsgRef temp = out[j];
        out[j] = out[j + 1];
        out[j + 1] = temp;
      }
    }
  }

  return w;
}

// All-peers collapsed read: the global-inbox analogue of espnowGetConversation.
// Collapses every active peer's received + sent rings into logical-message refs,
// then orders the combined set oldest→newest by timestamp (callers show the tail
// for "most recent"). reqId+isSent+senderMac gating in espnowCollapseRing means
// fragments from different peers never false-merge. Caps at maxN during
// collection — fine for small meshes; logs if it truncates.
int espnowCollapsedAllMessages(CollapsedMsgRef* out, int maxN) {
  if (!gEspNow || !out || maxN <= 0 || !gEspNow->peerMessageHistories) return 0;

  int w = 0;
  for (int p = 0; p < gEspNow->peerHistoryCapacity && w < maxN; p++) {
    PeerMessageHistory& history = gEspNow->peerMessageHistories[p];
    if (!history.active) continue;
    w = espnowCollapseRing(history.messages, history.tail, history.count, out, w, maxN);
    w = espnowCollapseRing(history.sent, history.sentTail, history.sentCount, out, w, maxN);
  }

  for (int i = 0; i < w - 1; i++) {
    for (int j = 0; j < w - i - 1; j++) {
      if (out[j].head->timestamp > out[j + 1].head->timestamp) {
        CollapsedMsgRef temp = out[j];
        out[j] = out[j + 1];
        out[j + 1] = temp;
      }
    }
  }

  if (w >= maxN) {
    DEBUG_ESPNOWF("[ESPNOW] collapsedAll hit cap maxN=%d; older logical messages dropped", maxN);
  }
  return w;
}

// Reassemble the full text of a (possibly multi-fragment) message into `out`.
// Walks the peer's received (isSent=false) or sent (isSent=true) ring, gathers
// the records sharing `reqId`, and concatenates their fragments in piece order —
// the reader-side equivalent of the reqId-stitching the web/BLE UIs do, so
// on-device displays (G2/OLED) can show the whole message, not just fragment 1.
// `reqId == 0` (unsolicited / single-frame) returns 0 — the caller should use
// the record's own text then. `complete` (if non-null) is set true only when all
// pieceTotal fragments were found. Returns bytes written (excluding NUL).
int espnowReassembleByReqId(const uint8_t* peerMac, uint32_t reqId, bool isSent,
                            char* out, size_t cap, bool* complete) {
  if (complete) *complete = false;
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!gEspNow || !peerMac || reqId == 0 || !gEspNow->peerMessageHistories) return 0;

  PeerMessageHistory* h = nullptr;
  for (int i = 0; i < gEspNow->peerHistoryCapacity; i++) {
    PeerMessageHistory& c = gEspNow->peerMessageHistories[i];
    if (c.active && memcmp(c.peerMac, peerMac, 6) == 0) { h = &c; break; }
  }
  if (!h) return 0;

  const ReceivedTextMessage* ring = isSent ? h->sent : h->messages;
  uint8_t tail = isSent ? h->sentTail : h->tail;
  uint8_t cnt  = isSent ? h->sentCount : h->count;

  // Determine the fragment count from any matching record.
  uint8_t total = 0;
  for (int i = 0; i < cnt; i++) {
    const ReceivedTextMessage& m = ring[(tail + i) % MESSAGES_PER_DEVICE];
    if (m.active && m.reqId == reqId) { total = m.pieceTotal ? m.pieceTotal : 1; break; }
  }
  if (total == 0) return 0;

  // Concatenate fragments in piece order (1..total).
  size_t pos = 0;
  uint8_t found = 0;
  for (uint8_t p = 1; p <= total; p++) {
    for (int i = 0; i < cnt; i++) {
      const ReceivedTextMessage& m = ring[(tail + i) % MESSAGES_PER_DEVICE];
      if (!m.active || m.reqId != reqId || m.piece != p) continue;
      size_t l = strlen(m.message);
      if (pos + l >= cap) l = (cap > pos + 1) ? (cap - 1 - pos) : 0;
      if (l) { memcpy(out + pos, m.message, l); pos += l; }
      found++;
      break;
    }
    if (pos >= cap - 1) break;
  }
  out[pos] = '\0';
  if (complete) *complete = (found >= total);
  return (int)pos;
}

#endif // ENABLE_ESPNOW
