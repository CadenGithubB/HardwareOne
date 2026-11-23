#ifndef SYSTEM_ESPNOW_H
#define SYSTEM_ESPNOW_H

#include <Arduino.h>
#include <esp_attr.h>   // EXT_RAM_BSS_ATTR (MAC_STR static)
#include <esp_err.h>

#include "System_BuildConfig.h"
#include "System_MacText.h"   // MAC_FMT, MAC_ARGS, macToDisplay, macEquals, macParse
#include "System_ESPNow_PeerStateCore.h"  // EspNowPeerState: online / connecting / offline

// ESP-NOW type definitions (always available for type-safe references)
enum MeshRole {
  MESH_ROLE_WORKER = 0,
  MESH_ROLE_MASTER = 1,
  MESH_ROLE_BACKUP_MASTER = 2
};

enum EspNowMode {
  ESPNOW_MODE_DIRECT = 0,
  ESPNOW_MODE_MESH = 1
};

// Compile-time ceiling for mesh peer slots; used in both real and stub builds.
#define MESH_PEER_MAX 16

// RX liveness classification (see noteMeshPeerRxActivity in System_ESPNow.cpp).
enum class EspNowMeshRxKind : uint8_t {
  MeshHeartbeat = 0,
  Ack = 1,
  RxActivity = 2,
  BootstrapLiveness = 3,
};

// Outcome of sendFileToMacResult.
enum class FileSendResult : uint8_t {
  Sent      = 0,  // every frame was handed to the radio (sender-side truth only)
  Failed    = 1,  // refused, aborted or cancelled by the receiver; the log names the cause
  NoSession = 2,  // no ACTIVE encrypted session with the peer: nothing was sent, the
                  // handshake was started — a retry a few seconds later can succeed
};

// The one reply for a send that needs an ACTIVE encrypted session with the peer
// (a file, a multi-frame message) when there is none yet. %s = the peer as the
// user named it. Starts with "Error:" and has no "sent"/"success" in it: the web
// device card decides success from those words.
#define ESPNOW_NO_SESSION_REPLY_FMT \
  "Error: no secure session with %s yet; one is being opened, try again in a few seconds"

// This radio instance's own STA / AP address. ESP-NOW uses its checked cache
// while running; a driver query can be a blocking RPC on a Hosted (P4 + C6)
// radio, so it is only the fallback before start or after stop.
// With ESP-NOW disabled, WiFi builds query the driver directly. With both
// disabled, non-null outputs are zeroed and ESP_ERR_NOT_SUPPORTED is returned.
// Every configuration rejects a null output with ESP_ERR_INVALID_ARG.
esp_err_t espnowSelfStaMac(uint8_t out[6]);
esp_err_t espnowSelfApMac(uint8_t out[6]);

// Parses "AA:BB:CC:DD:EE:FF" (System_ESPNow.cpp). Declared for every build
// because always-compiled code names it (CommandArgs::argMac); without ESP-NOW
// no command calls that code and the linker drops it.
bool parseMacAddress(const String& macStr, uint8_t mac[6]);

// The send primitives the transmit dispatcher (System_ESPNow_Tx.cpp) forwards
// to. Declared for every build because that file is always compiled; without
// ESP-NOW nothing submits a job and the linker drops the dispatcher.
// Flags are 16 bits wide so the high-byte bits (BROADCAST_AUTH, SESSION_FRAME,
// HANDSHAKE) survive.
bool v4_send_frame(const uint8_t* dst, uint8_t type, uint16_t flags, uint32_t msgId,
                   const uint8_t* payload, uint16_t payloadLen, uint8_t ttl);
bool v4_send_payload_smart(const uint8_t* dst, uint8_t type, uint16_t flags, uint32_t msgId,
                           const uint8_t* payload, uint16_t payloadLen, uint8_t ttl);
// Longest AEAD plaintext that still fits one frame to `dst` (a routed peer has
// a smaller budget than a direct one).
uint16_t v4_single_frame_cap(const uint8_t* dst);

#if ENABLE_ESPNOW

#include <ArduinoJson.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <type_traits>
#include <vector>
#include <WiFi.h>

#include "System_Debug.h"
#include "System_Settings.h"
#include "System_User.h"
#include "System_CommandRegistry.h"
#include "System_ESPNow_Sensors.h"  // RemoteSensorType
#include "System_ESPNow_HistoryCore.h"  // per-peer message-history byte rings (espnowhist::)
#include "System_ESPNow_Wire.h"  // CapabilitySummary, BondPeerStatus (wire structs)

// Settings persistence (broadcastOutput is provided by System_Debug.h).
bool writeSettingsJson();

// Message types
#define MSG_TYPE_BOOT "BOOT"  // the BOOT notice (opcode 31) JSON type; goes with BOOT

// Bond roles (stored as uint8_t in gSettings.bondRole when ENABLE_BONDED_MODE).
// When bonded mode is compiled out, helpers are false/"none" stubs so mesh-only
// builds can still include this header.
#if ENABLE_BONDED_MODE
enum BondRole {
  BOND_ROLE_WORKER = 0,   // compute/network device
  BOND_ROLE_MASTER = 1    // display/gamepad device
};
inline bool isBondMaster() { return gSettings.bondRole == BOND_ROLE_MASTER; }
inline bool isBondWorker() { return gSettings.bondRole == BOND_ROLE_WORKER; }
inline const char* bondRoleStr() { return isBondMaster() ? "master" : "worker"; }
#else
inline bool isBondMaster() { return false; }
inline bool isBondWorker() { return false; }
inline const char* bondRoleStr() { return "none"; }
#endif

// Set mesh role at runtime with logging. Does not persist — reboot restores saved role.
void setMeshRole(MeshRole role, const char* reason);

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

// Small, allocation-free String-like field for records that live inside the
// PSRAM-backed EspNowState. Capacity includes the trailing NUL. Keeping the
// familiar c_str()/length()/assignment surface limits churn at call sites while
// ensuring bounded registry metadata never falls back to the internal heap.
template <size_t Capacity>
class EspNowInlineText {
  static_assert(Capacity > 0, "EspNowInlineText needs room for a NUL");

 public:
  EspNowInlineText() : value_{} {}
  EspNowInlineText(const char* value) : value_{} { (void)assign(value); }
  EspNowInlineText(const String& value) : value_{} { (void)assign(value); }

  EspNowInlineText& operator=(const char* value) {
    (void)assign(value);
    return *this;
  }
  EspNowInlineText& operator=(const String& value) {
    (void)assign(value);
    return *this;
  }

  static constexpr size_t capacity() { return Capacity; }

  // Returns false rather than retaining a misleading prefix when the source
  // does not fit. Input boundaries that can receive persisted/user text should
  // check this result and report or migrate the value explicitly.
  bool assign(const char* value) {
    if (!value) value = "";
    size_t len = 0;
    while (len < Capacity && value[len] != '\0') ++len;
    if (len == Capacity) {
      clear();
      return false;
    }
    memcpy(value_, value, len + 1);
    return true;
  }
  bool assign(const String& value) { return assign(value.c_str()); }

  const char* c_str() const { return value_; }
  operator const char*() const { return value_; }

  size_t length() const {
    size_t len = 0;
    while (len < Capacity && value_[len] != '\0') ++len;
    return len;
  }
  bool isEmpty() const { return value_[0] == '\0'; }
  void clear() { value_[0] = '\0'; }

  bool equalsIgnoreCase(const String& other) const {
    return strcasecmp(value_, other.c_str()) == 0;
  }
  bool operator==(const char* other) const {
    return strcmp(value_, other ? other : "") == 0;
  }
  bool operator!=(const char* other) const { return !(*this == other); }
  bool operator==(const String& other) const {
    return strcmp(value_, other.c_str()) == 0;
  }
  bool operator!=(const String& other) const { return !(*this == other); }

 private:
  char value_[Capacity];
};

// ESP-NOW device name mapping. These fields are embedded in EspNowState, whose
// storage is allocated with PreferPsram, so none of this bounded metadata uses
// Arduino String's internal-heap realloc path.
struct EspNowDevice {
  uint8_t mac[6];
  EspNowInlineText<32> name;
  bool encrypted;        // Whether this device uses encryption
  uint8_t key[16];       // Per-device encryption key
  EspNowInlineText<48> friendlyName;  // Cached from mesh peer metadata
  EspNowInlineText<32> room;          // Cached from mesh peer metadata
  EspNowInlineText<32> zone;          // Cached from mesh peer metadata
  EspNowInlineText<64> tags;          // Cached metadata (comma-separated)
  // What this device's admin accepted of the room and tags above. Only these
  // make the peer a target of a room or tag command; what the peer announces
  // beyond them waits (System_ESPNow_GroupCore.h has the rules).
  EspNowInlineText<32> acceptedRoom;
  EspNowInlineText<64> acceptedTags;
  bool stationary;       // Cached from mesh peer metadata
  // Which of gSettings.meshes[] this peer belongs to.
  // The header's mesh id on frames to this peer comes from this slot.
  uint8_t meshId;
  // espnowpairsecure's first-key window (E31): millis() deadline until which
  // a key exchange may store this peer's FIRST key; 0 = none. Stored as
  // awaitingKeyUntilEpochMs while the clock is set
  // (System_ESPNow_RevokedCore.h, firstKeyFromStored).
  uint32_t awaitingKeyUntilMs;
};

// ==========================
// Mesh Topology Structures (from espnow_system.cpp)
// ==========================

// Topology collection is a fixed PSRAM-backed matrix: at most every configured
// mesh peer can respond, and every responder can report at most MESH_PEER_MAX
// peers. No formatted String storage is retained while collecting.
#define MAX_CONCURRENT_TOPO_STREAMS MESH_PEER_MAX
struct TopologyPeerRecord {
  uint8_t mac[6];
  char name[32];
  int8_t rssi;
  bool present;
};

struct TopologyStream {
  uint32_t reqId;              // Request ID to match responses
  uint8_t senderMac[6];        // MAC of device sending topology
  char senderName[32];         // Name of sender device
  uint16_t totalPeers;         // Total number of peers to expect
  uint16_t receivedPeers;      // Peers received so far
  uint16_t receivedPeerMask;   // One bit per validated peerIndex
  unsigned long startTime;     // Stream start time
  bool occupied;               // Slot belongs to the current/completed request
  bool active;                 // Stream in progress
  bool started;                // A validated TOPO_START was received
  bool complete;               // Finalized (complete or timed out)
  TopologyPeerRecord peers[MESH_PEER_MAX];
};
static_assert(MESH_PEER_MAX <= 16, "Topology receivedPeerMask needs widening");
static_assert(std::is_trivial<TopologyStream>::value &&
                  std::is_standard_layout<TopologyStream>::value,
              "TopologyStream must remain safe for bounded byte reset");

// Topology device name cache
#define MAX_TOPO_DEVICE_CACHE 16
struct TopoDeviceEntry {
  uint8_t mac[6];
  char name[32];
  bool active;
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
// MESH_PEER_MAX is defined above the #if ENABLE_ESPNOW guard (shared with stub build)
#define MESH_PEER_TIMEOUT_MS 30000

// Runtime peer slot count (set from gSettings.meshPeerMax at boot, capped to MESH_PEER_MAX)
extern int gMeshPeerSlots;

// Peer membership vs telemetry:
//   - Paired remotes live in gEspNow->devices[] (and esp_now peer table).
//   - MeshPeerHealth is RX telemetry keyed by MAC (mesh HB, ACK, TEXT, etc.).
//   - lastMeshHeartbeatMs = only V3 HEARTBEAT frames; lastRxActivityMs = any tracked RX
//     (see noteMeshPeerRxActivity). meshstatus JSON merges devices + health (buildMeshStatusPeersJson).

struct MeshPeerHealth {
  uint8_t mac[6];
  bool onlineLogged;             // Edge latch for the [EVENT][MESH] online/offline pair.
                                 // Keyed on heartbeat-based aliveness, NOT isActive (which
                                 // any RX frame re-arms) — so a powered-off / ACK-only peer
                                 // that never heartbeats never logs a spurious 'offline'.
  uint8_t _pad[1];               // Padding for alignment
  uint32_t lastMeshHeartbeatMs;  // Last V3 HEARTBEAT from this peer (mesh sync / isMeshPeerAlive)
  uint32_t lastRxActivityMs;     // Last ACK, HEARTBEAT, TEXT, or bootstrap liveness bump
  uint32_t lastAckMs;            // Last ACK from this peer
  uint32_t heartbeatCount;       // HEARTBEAT frames received (not pairing bootstrap)
  uint32_t ackCount;             // ACKs received
  uint32_t lastBootCounter;      // Last known boot counter from this peer
  int8_t rssi;                   // Last RSSI from heartbeat payload
  int16_t linkRssiEwma;          // EWMA (alpha 1/4) of OUR radio's per-frame RSSI for
                                 // frames from this peer — the true A<->B link quality.
                                 // `rssi` above is peer-REPORTED (its heartbeat payload,
                                 // AP-referenced); this one is measured here. QUARTER-dB
                                 // fixed point (value = dBm * 4): a whole-dB int8 EWMA
                                 // has a ±3 dB truncation deadband, which would eat the
                                 // planned route metric's entire hysteresis margin. 0 =
                                 // no sample yet. Readers report (v + (v>=0?2:-2)) / 4.
                                 // Later route metrics key on this
                                 // (docs/ESPNOW_RELAY_RESTORE_PLAN.md).
  bool isActive;                 // true if this slot is in use
  // True once a frame from this peer has actually been received over the radio.
  // Distinct from lastRxActivityMs != 0, because BootstrapLiveness SEEDS that
  // timestamp at pairing time to get the peer into broadcast fan-out — an
  // optimistic guess, not an observation. Multi-hop routing asks "can I reach
  // this peer directly?" and must not be fooled by the guess: a peer paired by
  // MAC while out of range would look audible for a full timeout window, and
  // every frame sent to it in that window (starting with the KEY_EX that
  // completes the pairing) would go out on a link that isn't there.
  // Never set for relayed frames — noteMeshPeerRxActivity returns early for
  // those, since they prove nothing about the direct link.
  bool everHeardDirect;
};

// hbRssi: pass RSSI from V4PayloadHeartbeat; use -128 to leave peer->rssi unchanged.
// linkRssi: OUR radio's RSSI for the frame that triggered this call (from
// recv_info->rx_ctrl); use -128 when there is no radio frame in hand (e.g.
// pairing bootstrap). Feeds MeshPeerHealth::linkRssiEwma.
// senderProven: the frame proved `mac` itself as its sender, not merely a key:
// it opened under the session bound to `mac`, or it carried a verified
// BROADCAST_AUTH tag over a header whose origin is `mac` (the tag does not
// cover the radio source, so a verified tag alone is not enough). A missing
// slot is created only then, or for a MAC in the paired registry; any other
// frame only refreshes a slot that already exists.
void noteMeshPeerRxActivity(const uint8_t* mac, EspNowMeshRxKind kind, int8_t hbRssi = -128,
                            int8_t linkRssi = -128, bool senderProven = false);

// Fills JsonArray for espnow meshstatus: union of active gMeshPeers + paired remotes not yet in slots.
// JSON keys: mac, name, alive (mesh HB), activityAlive, lastHeartbeat (mesh HB ms), lastRxActivity,
// lastAck, heartbeatCount, ackCount, secondsSinceHeartbeat, secondsSinceActivity,
// linkRssi (locally-measured per-frame EWMA dBm; 0 = no sample yet).
void buildMeshStatusPeersJson(JsonArray peers, uint32_t nowMillis, int* outTotalPeers);

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
  // Per-peer heartbeat backoff timing. Lives here (not in MeshPeerHealth)
  // because a dead peer's health slot is memset on every 5 s bootstrap, so it
  // can't hold state; this meta slot is durable. See meshHeartbeatMinInterval.
  uint32_t hbLastHeardMs;  // millis() of last received mesh HEARTBEAT, seeded to
                           // slot birth so a never-heard peer still ages. Any
                           // received heartbeat resets it -> full rate resumes.
  uint32_t hbLastSentMs;   // millis() we last sent a heartbeat to this peer (cadence gate)
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
    hbLastHeardMs = 0;
    hbLastSentMs = 0;
    isActive = false;
  }
};

extern MeshPeerMeta* gMeshPeerMeta;  // Dynamically allocated [gMeshPeerSlots] at init

// Peer metadata helpers
MeshPeerMeta* getMeshPeerMeta(const uint8_t mac[6], bool createIfMissing = false);
// Paired peers that are members of `room` for a room command: the room was
// accepted on this device, not merely announced by the peer.
int countMeshPeerMetaByRoom(const char* room);

// Room and tag membership of paired peers (System_ESPNow_GroupCore.h).
// True when `room` is the room this device accepted for the paired peer `mac`.
// A page that shows a peer's announced room marks it when this is false.
bool espnowGroupRoomAccepted(const uint8_t mac[6], const char* room);
// One paired peer's membership for a page or a JSON reply. `pendingRoom` and
// `pendingTags` are what the peer announces and this device has not accepted.
struct EspNowGroupView {
  char acceptedRoom[32];
  char acceptedTags[64];
  char pendingRoom[32];
  char pendingTags[64];
};
// `room` and `tags` are the announced values the caller shows. False when
// `mac` is not a paired peer: nothing can be accepted for it, so all of
// `room` and `tags` comes back as pending.
bool espnowGroupView(const uint8_t mac[6], const char* room, const char* tags, EspNowGroupView& out);
// The peer at `mac` may be another device from now on (its identity key was
// forgotten or replaced): nothing stays accepted for it, and frames waiting
// for a session with it are dropped. `why` goes into the event line.
void espnowGroupAcceptanceClear(const uint8_t mac[6], const char* why);

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
  EspNowInlineText<32> name;
  int rssi;
  uint32_t lastSeenMs;
  uint32_t heartbeatCount;
};

// ==========================
// Bond Mode Structures
// ==========================

// CapabilitySummary (BOND_CAP_RESP) and BondPeerStatus (BOND_STATUS_RESP) are
// wire structs: System_ESPNow_Wire.h.

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

// Sensor mask bits. One numbering for every mask that names sensors: the bit
// of a sensor is espnowsensor::sensorBit() of its RemoteSensorType
// (System_ESPNow_SensorCore.h), the same bit the SENSOR_REQ mask, the
// SENSOR_STATUS masks and the receiver's table use. No number is typed here.
#define CAP_SENSOR_THERMAL      espnowsensor::sensorBit(REMOTE_SENSOR_THERMAL)
#define CAP_SENSOR_TOF          espnowsensor::sensorBit(REMOTE_SENSOR_TOF)
#define CAP_SENSOR_IMU          espnowsensor::sensorBit(REMOTE_SENSOR_IMU)
#define CAP_SENSOR_INPUT        espnowsensor::sensorBit(REMOTE_SENSOR_INPUT)
#define CAP_SENSOR_APDS         espnowsensor::sensorBit(REMOTE_SENSOR_APDS)
#define CAP_SENSOR_GPS          espnowsensor::sensorBit(REMOTE_SENSOR_GPS)
#define CAP_SENSOR_RTC          espnowsensor::sensorBit(REMOTE_SENSOR_RTC)
#define CAP_SENSOR_PRESENCE     espnowsensor::sensorBit(REMOTE_SENSOR_PRESENCE)
#define CAP_SENSOR_FMRADIO      espnowsensor::sensorBit(REMOTE_SENSOR_FMRADIO)

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

// (The sensor names are in the one sensor table, System_ESPNow_SensorCore.h:
// sensorMaskNames() below lists the sensors of a mask.)

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

// The sensors of a mask by name, from the one sensor table: short names
// joined with "," (OLED) or long names joined with ", " (web/CLI); "None"
// for an empty mask.
inline String sensorMaskNames(uint32_t mask, bool longForm) {
  String result;
  for (int i = 0; i < REMOTE_SENSOR_MAX; i++) {
    if (!(mask & espnowsensor::sensorBit((uint8_t)i))) continue;
    if (result.length() > 0) result += longForm ? ", " : ",";
    result += longForm ? espnowsensor::kSensorTable[i].longName : espnowsensor::kSensorTable[i].shortName;
  }
  return result.length() > 0 ? result : String("None");
}


// Message history (message-buffer section of System_ESPNow.cpp; the ring itself
// is System_ESPNow_HistoryCore.h). Each peer that has a message gets ONE block
// holding a received and a sent byte ring of the same size, allocated by the
// peer's first store (never at ESP-NOW start) and never moved or freed. A ring
// keeps packed records (a 20-byte header plus exactly the text) and drops whole
// oldest records when a new one does not fit, so how many messages a ring keeps
// depends on their length: at the 16 KB PSRAM default about 270 forty-character
// messages per direction (about 250 records were kept before, whatever their
// length). Ring size: setting espnowhistorykb (0 = the default below), clamped
// to the board maximum and to a total cap over all slots; latched once per boot.
// PSRAM vs no PSRAM is decided at runtime (psramAvailableRuntime()): a
// CONFIG_SPIRAM build whose PSRAM was not found uses the _NOPSRAM values, and
// blocks then come from internal RAM only while ESPNOW_HISTORY_INTERNAL_FLOOR
// of 8-bit-capable internal RAM stays free (the classic ESP32's IRAM-only heap
// does not count). "PSRAM" at compile time is CONFIG_SPIRAM, the one option every
// target with PSRAM sets (the older CONFIG_*_SPIRAM_SUPPORT names are aliases
// only esp32 and esp32s3 keep).
//
// A remote command's streamed output (a memreport is ~88 records) must survive
// until it is paged over BLE: the 16 KB received ring holds about 1.8 such
// reports, so the app serialises remote commands (one at a time, paged to the
// end before the next); espnowhistorykb 32 doubles that.
#define ESPNOW_HISTORY_KB_DEFAULT_NOPSRAM 2
#define ESPNOW_HISTORY_KB_MAX_NOPSRAM     4
#define ESPNOW_HISTORY_TOTAL_KB_NOPSRAM   64           // ring bytes over all slots, no PSRAM
#define ESPNOW_HISTORY_INTERNAL_FLOOR     (48 * 1024)  // internal heap that must stay free after a block
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
  #define ESPNOW_HISTORY_KB_DEFAULT 16
  #define ESPNOW_HISTORY_KB_MAX     32
  #define ESPNOW_HISTORY_TOTAL_KB   1024    // and at most 1/4 of the PSRAM found at runtime
  #define ESPNOW_HISTORY_VIEW_MAX   512     // CollapsedMsgRef buffers of the on-device views
#else
  #define ESPNOW_HISTORY_KB_DEFAULT ESPNOW_HISTORY_KB_DEFAULT_NOPSRAM
  #define ESPNOW_HISTORY_KB_MAX     ESPNOW_HISTORY_KB_MAX_NOPSRAM
  #define ESPNOW_HISTORY_TOTAL_KB   ESPNOW_HISTORY_TOTAL_KB_NOPSRAM
  #define ESPNOW_HISTORY_VIEW_MAX   32
#endif
#define ESPNOW_HISTORY_SETTING_KB_MAX 32   // setting range 0..32 on every build
#define ESPNOW_HISTORY_NAME_CAP      32    // == espnowhist::kNameCap
// Longest wait for the message-store lock. A store that cannot take it in
// time is refused; a read returns nothing.
#define PEER_HISTORY_LOCK_TIMEOUT_MS 200
// History lines (no memory, full, damaged, numbers used up) are said at most
// once per this interval, with the count of what was folded in. The allocated
// line is said for every allocation (at most one per slot per boot).
#define PEER_HISTORY_SAY_INTERVAL_MS 60000
// After a failed block allocation, new attempts are held off this long.
#define PEER_HISTORY_ALLOC_RETRY_MS  5000
// The old fixed per-peer record depth and the eager initial block count no longer exist.
static_assert(2 * MESH_PEER_MAX <= espnowhist::kMaxSources, "merge scratch covers every ring");
static_assert(ESPNOW_HISTORY_KB_MAX * 1024 <= espnowhist::kMaxRingBytes, "board maximum fits the core");
static_assert(ESPNOW_HISTORY_SETTING_KB_MAX * 1024 <= espnowhist::kMaxRingBytes, "setting maximum fits the core");
static_assert(ESPNOW_HISTORY_NAME_CAP == espnowhist::kNameCap, "one name size");

// Max length of a user text message (espnowsend). Messages above
// ESPNOW_V4_MAX_PLAINTEXT (~202 B) are fragmented over the generic
// chunk/reassemble transport (same path command-results use); the receiver
// stores each fragment as its own history record — device-side reassembly
// deliberately SKIPS text, so the device never materializes a multi-KB lump —
// and clients rejoin the pieces by msgId/fragIndex. 1024 → 6 fragments max
// (1024 / 200).
//
// This is ONLY a length gate; it sizes no buffer. Both use sites are bare
// compares: TX rejects an oversized espnowsend, RX bounds an accepted payload
// (inert for text, which never arrives as more than one fragment). The text
// lands in TextQueueEntry::content (char[256], 255 B + NUL) and then in one
// history record, which holds at most espnowhist::kTextMax (255) bytes —
// already more than a 200 B fragment can fill. So changing this number costs
// no memory and frees none; it only moves where a long send is refused.
#define ESPNOW_TEXT_MAX_LEN 1024

// Message types for logging
enum LogMessageType {
  MSG_TEXT = 0,           // Regular text message (chat — sent or received)
  MSG_FILE_SEND_START,    // File transfer started
  MSG_FILE_SEND_SUCCESS,  // File transfer completed successfully
  MSG_FILE_SEND_FAILED,   // File transfer failed
  MSG_FILE_RECV_SUCCESS,  // File received successfully
  MSG_FILE_RECV_FAILED,   // File receive failed
  // Non-chat records that still live in the per-peer history so clients can
  // show/hide them durably by `type` (no session-bound reqId bookkeeping). The
  // command-response class EXISTS on the wire (ESPNOW_V4_TYPE_CMD_RESP); these
  // preserve it into storage instead of flattening everything to MSG_TEXT.
  MSG_CMD_RESULT,         // Output of a relayed remote op (espnowremote/browse/fetch) or streamed cmd output
  MSG_SYSTEM_EVENT        // Device/system notice (peer-metadata snapshot, etc.)
};
static_assert(MSG_SYSTEM_EVENT <= espnowhist::kTypeMax, "message types fit the record header");

// How a received record's sender is shown. FromName keeps the old guess: a null/empty name or the
// MAC text means unattributed. Callers that know pass Attributed / Unattributed.
enum class HistoryAttribution : uint8_t { FromName = 0, Attributed = 1, Unattributed = 2 };

// One history record COPIED OUT of the store. The store keeps packed records with no MAC or
// name; this view is filled under the store lock and is the reader's own memory.
// Removed versus the old struct: senderMac (use peerMac), senderName (use espnowHistoryDisplayName),
// active (every copied record is live).
struct ReceivedTextMessage {
  uint8_t  peerMac[6];      // the peer whose history holds it (JSON "mac"), both directions
  uint8_t  piece;           // 1-based fragment index
  uint8_t  pieceTotal;      // fragments in the message
  uint32_t seqNum;
  uint32_t reqId;
  uint32_t timestamp;       // millis() at store
  LogMessageType msgType;
  bool     encrypted;
  bool     isSent;
  bool     unattributed;    // received, sender not proven: shown under the MAC text
  uint8_t  sendState;       // sent records: 0 pending, 1 delivered, 2 timeout, 3 failed
  uint8_t  length;          // text bytes in message
  char     message[espnowhist::kTextMax + 1];   // NUL-terminated
};

// Block of one peer: this header, then ringBytes bytes (received), then ringBytes bytes (sent).
// Only the message-store functions in System_ESPNow.cpp touch it, under the store lock.
struct PeerMessageHistory {
  uint8_t  peerMac[6];
  bool     active;
  uint8_t  reserved;
  uint32_t ringBytes;       // bytes of EACH ring; written once at allocation, never changed
  espnowhist::RingState received;
  espnowhist::RingState sent;
  uint8_t*       receivedBuf()       { return reinterpret_cast<uint8_t*>(this + 1); }
  const uint8_t* receivedBuf() const { return reinterpret_cast<const uint8_t*>(this + 1); }
  uint8_t*       sentBuf()           { return receivedBuf() + ringBytes; }
  const uint8_t* sentBuf() const     { return receivedBuf() + ringBytes; }
};
static_assert(sizeof(PeerMessageHistory) == 92, "block header layout");
static_assert(sizeof(PeerMessageHistory) % 4 == 0, "rings start aligned");

// Router metrics
struct RouterMetrics {
  uint32_t messagesSent;
  uint32_t messagesReceived;
  uint32_t messagesFailed;
  // V4 fragmentation metrics (gV4Reasm path). Reassembly timeouts are counted by
  // v4FragRxGc — that is the one the GC sweep actually bumps.
  uint32_t v4FragTx;             // Total fragments transmitted
  uint32_t v4FragRx;             // Total fragments received
  uint32_t v4FragRxCompleted;    // Messages fully reassembled
  uint32_t v4FragRxGc;           // Reassembly contexts GC'ed due to timeout

  RouterMetrics() : messagesSent(0), messagesReceived(0), messagesFailed(0),
                    v4FragTx(0), v4FragRx(0), v4FragRxCompleted(0), v4FragRxGc(0) {}
};


// ==========================
// ESP-NOW State Structure (complete version matching .ino implementation)
// ==========================
struct EspNowState {
  // Core state
  bool initialized;
  uint8_t channel;
  EspNowMode mode;

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
  // Kept inline with the PSRAM-resident state. This avoids a tiny standalone
  // allocation for every stream session and gives teardown no pointer lifetime
  // to leak or race.
  uint8_t streamTarget[6];
  bool streamActive;
  bool streamingSuspended;
  uint32_t streamDroppedCount;
  uint32_t streamSentCount;
  uint32_t streamReceivedCount;
  unsigned long lastStreamSendTime;

  // List output buffer (PSRAM-allocated at init). Sized for the espnowlist
  // document with all 16 devices: a row is up to 100 bytes, the fixed part 260.
  static constexpr size_t kListBufferSize = 2048;
  char* listBuffer;
  
  // Message Router
  RouterMetrics routerMetrics;
  uint32_t nextMessageId;
  
  
  // Per-device message history buffers (for web UI and OLED)
  // Slot pointers; a block is allocated by the peer's first store and never moved or freed.
  // Only the message-store functions in System_ESPNow.cpp touch them, under the store lock.
  // The store is initialised iff peerHistoryCapacity > 0.
  PeerMessageHistory* peerMessageHistories[MESH_PEER_MAX];
  int peerHistoryCapacity;        // 0 until peerHistoryStoreInit; then min(gMeshPeerSlots, MESH_PEER_MAX)
  int peerHistoryCount;           // blocks allocated (= peers with a history)
  uint32_t peerHistoryRingBytes;  // bytes of each ring of a block (latched once per boot)
  uint32_t globalMessageSeqNum; // Global sequence number for all messages
  
  // Statistics (non-router specific)
  uint32_t heartbeatsSent;
  uint32_t heartbeatsReceived;
  uint32_t fileTransfersSent;
  uint32_t fileTransfersReceived;

  // Outbound file-send tracking — lets the rekey scheduler defer a key rotation
  // while WE are mid-send to a peer. The slot table only tracks INBOUND transfers,
  // yet a big OUTBOUND send is exactly what pushes our session txSeq past the rekey
  // threshold. Set by a RAII guard in sendFileToMac; fileSendStartedMs lets the
  // rekey trigger bound a flag left stuck by a task that died mid-send.
  volatile bool fileSendInProgress;
  uint8_t       fileSendPeer[6];
  uint32_t      fileSendStartedMs;
  // Abort channel for the in-flight send (integrity pass). fileSendMsgId is
  // the transferId published by FileSendActiveGuard; v4h_file_cancel (on
  // espnow_task) stores that id into fileSendAbortMsgId when a FILE_CANCEL
  // matches (inProgress && peer && msgId), and the sendFileToMac chunk loop
  // (cmd_exec) compares fileSendAbortMsgId against its OWN transferId.
  // Carrying the msgId instead of a bool makes the one theoretical stale-store
  // race harmless: a late store of transfer A's id can never equal transfer
  // B's id, so B ignores it. NOTE: this single-slot design (like
  // fileSendInProgress itself) assumes all sendFileToMac callers run
  // serialized on cmd_exec — re-audit if the planned Step 4 move of the
  // chunk loop onto espnow_tx ever lands.
  volatile uint32_t fileSendMsgId;
  volatile uint32_t fileSendAbortMsgId;

  unsigned long lastResetTime;

  // (The device's own name is gSettings.espnowDeviceName; there is no copy here.)

  // Last received remote capability (for OLED/web display)
  CapabilitySummary lastRemoteCap;
  bool lastRemoteCapValid;
  unsigned long lastRemoteCapTime;
  
#if ENABLE_BONDED_MODE
  // Bond heartbeat tracking
  uint32_t bondHeartbeatsSent;
  uint32_t bondHeartbeatsReceived;
  unsigned long lastBondHeartbeatReceivedMs;
  bool bondPeerOnline;              // true if heartbeat received within timeout
  uint32_t bondPeerBootCounter;     // Peer's boot counter (detect reboots)
  uint32_t bondPeerSettingsHash;    // Peer's settings hash (detect changes)
  uint32_t bondLocalSettingsHash;   // Our settings hash (sent in heartbeat)
  uint32_t bondCachedPeerSettingsHash; // Peer's hash at the moment we last cached their settings file. dirty = (bondPeerSettingsHash != this) when bondSettingsReceived.
  uint32_t bondPeerUptime;          // Peer's uptime in seconds (from heartbeat)
  
  // Master-driven sync tick state
  BondSyncRequestType bondSyncInFlight; // What request is currently pending
  uint8_t bondSyncRetryCount;           // Retry counter for current request
  uint32_t bondSyncLastAttemptMs;       // When last sync request was sent
  
  // Deferred flags (set in ISR-like callback, consumed in task context)
  bool bondNeedsCapabilityRequest;  // Peer came online — master starts sync
  bool bondNeedsStreamingSetup;     // Apply saved streaming prefs after sync
  // STREAM_CTRL received, per sensor, applied by the task after the receive
  // ring is drained (several in one loop all count). espnow_task only.
  espnowsensor::StreamCtrlQueue bondDeferredStreamCtrl;
  bool bondNeedsCapabilityResponse; // Peer requested our capabilities
  bool bondNeedsManifestResponse;   // Peer requested our manifest
  bool bondNeedsSettingsResponse;   // Peer requested our settings
  bool bondNeedsSchemaResponse;     // Peer requested our settings schema (mirrors settings)
  bool bondReceivedCapability;      // We received peer's CAP_RESP
  uint8_t bondPendingResponseMac[6]; // MAC of peer requesting cap/manifest/settings/schema

  // Sync completion tracking (replaces linear handshake state machine)
  bool bondCapSent;                 // We sent our capabilities this session
  bool bondManifestReceived;        // We received/cached peer's manifest (master)
  bool bondSettingsReceived;        // We received peer's settings (master)
  bool bondSchemaReceived;          // We received/cached peer's schema (master) — schema.json under peers/<MAC>/
  bool bondSettingsSent;            // We sent our settings to peer (worker)

  // NOTE: the bond auth token is NOT stored here. It lives in the encrypted
  // session (SessionState::bondToken), derived in sessionDeriveAeadKeys and
  // looked up via bondPeerActiveSession(). There is no global mirror.
  bool bondStatusReqSentOnce;       // One-shot: initial post-sync status exchange fired
  
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
  // Event-driven session gate for bond manifest/settings file sends: deadline
  // (millis) after which we stop waiting for an encrypted session and send
  // plaintext. 0 = no send currently waiting. See bondSendReadyOrDeferred().
  uint32_t bondSendWaitDeadlineMs;
#endif // ENABLE_BONDED_MODE
  
  // Secure sensor fetcher — deferred SENSOR_REQ apply (mesh-legal, NOT bond-gated).
  // v4h_sensor_req stashes here on the RX task; the super-loop (block 9f3) applies it
  // off the critical path because startSensorDataStreaming creates a task/mutex.
  // docs/ESPNOW_SENSOR_FETCHER_DESIGN.md
  bool     meshSensorReqPending;      // SENSOR_REQ received + authorized, awaiting apply
  uint32_t sensorReqRefusedBond;      // SENSOR_REQs refused because this device is a bond worker
  uint8_t  meshSensorReqMode;         // 0=subscribe, 1=unsubscribe, 2=oneshot
  uint32_t meshSensorReqMask;         // bitmask over RemoteSensorType
  uint32_t meshSensorReqIntervalMs;   // requested per-sensor cadence hint (ms)
  uint32_t meshSensorReqLeaseMs;      // lease duration (ms)
  uint8_t  meshSensorReqSrcMac[6];    // authorized controller MAC (encrypted-reply target)
  
  // Received metadata frames and requests for ours wait for the task in a
  // per-sender inbox of their own (System_ESPNow.cpp, metadataInboxFor).

  // Deferred message handling (ISR-safe pattern: callback sets flag, task processes)
  // TEXT message ring buffer. Deepened from 4 → 16: a long text now arrives as
  // N per-fragment entries (not one), so a multi-fragment burst needs headroom
  // or pieces get dropped. Cheap now that content is one fragment (256 B) not
  // a 1 KB lump. Must be a power of 2.
  static constexpr int TEXT_QUEUE_SIZE = 16;
  struct TextQueueEntry {
    uint8_t srcMac[6];
    char deviceName[32];
    char content[256];     // ONE fragment (≤200 B) or a single-frame message
    uint32_t msgId;        // group id — pieces of one chunked message share it
    uint8_t fragIndex;     // 0-based fragment index from the frame header
    uint8_t fragCount;     // total fragments in the message (1 when not chunked)
    bool encrypted;
    bool authenticated;    // frame proved key possession (SESSION_FRAME unwrap or BROADCAST_AUTH
                           // verify); false = plaintext, accepted only on a keyless device —
                           // filed under the MAC, never posts SYSEVT_TEXT_RX
    LogMessageType msgType; // MSG_TEXT (chat) or MSG_SYSTEM_EVENT (BOOT/system notice) — drain stores accordingly
    bool used;
  };
  TextQueueEntry textQueue[TEXT_QUEUE_SIZE];
  volatile int textQueueHead;  // ISR writes here (producer)
  volatile int textQueueTail;  // Task reads here (consumer)
  
  // CMD_RESP message
  bool deferredCmdRespPending;
  uint8_t deferredCmdRespSrcMac[6];
  char deferredCmdRespDeviceName[32];
  char* deferredCmdRespResult;  // PSRAM-allocated at init to V4_REASM_MAX_MSG + 1 (the fragmentation
                                // budget V4_FRAG_MAX × V4_MAX_FRAGMENT_PAYLOAD, plus the NUL). Alloc and
                                // clamp both derive from that one constant, so a maximal reassembled
                                // CMD_RESP now fits exactly. Sized from the WIRE, not from CMD_RESULT_MAX:
                                // the sender is a remote peer whose output limit is not ours to assume.
  bool deferredCmdRespSuccess;   // status == 0
  uint8_t deferredCmdRespStatus; // the answer's status (espnowanswer, rule F3)
  uint32_t deferredCmdRespReqId; // Correlation id — the request's msgId, carried in the CMD_RESP's reqId
  
  // STREAM message ring buffer (replaces single-buffer to prevent overwrite loss).
  // Allocated separately in initEspNow(): STREAM_QUEUE_SIZE_PSRAM slots when PSRAM
  // is available (the burst buffer a large bonded-command output like `help` —
  // ~24 frames in ~150ms — needs), else STREAM_QUEUE_SIZE_FALLBACK slots in
  // internal RAM (the historical size) so a PSRAM-less board pays no extra DRAM.
  // Both sizes MUST be powers of 2 (index wrap uses streamQueueMask).
  static constexpr int STREAM_QUEUE_SIZE_PSRAM    = 64;
  static constexpr int STREAM_QUEUE_SIZE_FALLBACK = 16;
  struct StreamQueueEntry {
    uint8_t srcMac[6];
    char deviceName[32];
    char content[256];
    uint32_t cmdMsgId;  // originating command's msgId (the STREAM answer's reqId). Stored
                        // as the record's reqId so remote command output correlates
                        // to its command. 0 = legacy startstream (unsolicited telemetry).
    bool used;          // Slot contains valid data
  };
  StreamQueueEntry* streamQueue;  // ps_alloc'd ring of streamQueueCap entries
  int streamQueueCap;             // slot count (power of 2): 16 or 64
  int streamQueueMask;            // streamQueueCap - 1 (index wrap mask)
  int streamDrainMax;             // max entries drained per task tick (8 or 64)
  volatile int streamQueueHead;  // producer (RX path)
  volatile int streamQueueTail;  // consumer (task tick)
  // CMD request (deferred to task for auth + execution)
  bool deferredCmdPending;
  uint8_t deferredCmdSrcMac[6];
  char deferredCmdDeviceName[32];
  char deferredCmdPayload[256];
  uint32_t deferredCmdMsgId;
  bool deferredCmdWasEncrypted;  // true if the CMD arrived AEAD-wrapped (SESSION_FRAME).
                                 // Bond (@BOND token) commands REQUIRE this — see v4_handle_cmd.

  // Constructor
  EspNowState() : 
    initialized(false),
    channel(0),
    mode(ESPNOW_MODE_DIRECT),
    passphrase(""),
    encryptionEnabled(false),
    devices{},
    deviceCount(0),
    unpairedDevices{},
    unpairedDeviceCount(0),
    streamTarget{},
    streamActive(false),
    streamingSuspended(false),
    streamDroppedCount(0),
    streamSentCount(0),
    streamReceivedCount(0),
    lastStreamSendTime(0),
    listBuffer(nullptr),
    nextMessageId(1),
    peerMessageHistories{},
    peerHistoryCapacity(0),
    peerHistoryCount(0),
    peerHistoryRingBytes(0),
    globalMessageSeqNum(0),
    heartbeatsSent(0),
    heartbeatsReceived(0),
    fileTransfersSent(0),
    fileTransfersReceived(0),
    fileSendInProgress(false),
    fileSendPeer{},
    fileSendStartedMs(0),
    fileSendMsgId(0),
    fileSendAbortMsgId(0),
    lastResetTime(0),
    lastRemoteCapValid(false),
    lastRemoteCapTime(0),
#if ENABLE_BONDED_MODE
    bondHeartbeatsSent(0),
    bondHeartbeatsReceived(0),
    lastBondHeartbeatReceivedMs(0),
    bondPeerOnline(false),
    bondPeerBootCounter(0),
    bondPeerSettingsHash(0),
    bondLocalSettingsHash(0),
    bondCachedPeerSettingsHash(0),
    bondPeerUptime(0),
    bondSyncInFlight(BOND_SYNC_NONE),
    bondSyncRetryCount(0),
    bondSyncLastAttemptMs(0),
    bondNeedsCapabilityRequest(false),
    bondNeedsStreamingSetup(false),
    bondDeferredStreamCtrl{0, 0},
    bondNeedsCapabilityResponse(false),
    bondNeedsManifestResponse(false),
    bondNeedsSettingsResponse(false),
    bondNeedsSchemaResponse(false),
    bondReceivedCapability(false),
    bondCapSent(false),
    bondManifestReceived(false),
    bondSettingsReceived(false),
    bondSchemaReceived(false),
    bondSettingsSent(false),
    bondStatusReqSentOnce(false),
    bondRssiLast(-100),
    bondRssiAvg(-100),
    bondLastRssiUpdateMs(0),
    bondPeerStatusValid(false),
    bondPeerStatusTimeMs(0),
    bondNeedsStatusResponse(false),
    bondNeedsProactiveStatus(false),
    bondLastStatusReqMs(0),
    bondSendWaitDeadlineMs(0),
#endif
    meshSensorReqPending(false),
    sensorReqRefusedBond(0),
    meshSensorReqMode(0),
    meshSensorReqMask(0),
    meshSensorReqIntervalMs(0),
    meshSensorReqLeaseMs(0),
    textQueueHead(0),
    textQueueTail(0),
    deferredCmdRespPending(false),
    deferredCmdRespResult(nullptr),
    deferredCmdRespSuccess(false),
    deferredCmdRespStatus(0),
    deferredCmdRespReqId(0),
    streamQueue(nullptr),
    streamQueueCap(0),
    streamQueueMask(0),
    streamDrainMax(0),
    streamQueueHead(0),
    streamQueueTail(0),
    deferredCmdPending(false),
    deferredCmdMsgId(0),
    deferredCmdWasEncrypted(false)
  {
    memset(derivedKey, 0, 16);
#if ENABLE_BONDED_MODE
    memset(bondPendingResponseMac, 0, 6);
    memset(&bondPeerStatus, 0, sizeof(bondPeerStatus));
#endif
    memset(meshSensorReqSrcMac, 0, 6);
    // Initialize deferred message buffers
    memset(textQueue, 0, sizeof(textQueue));
    memset(deferredCmdRespSrcMac, 0, 6);
    memset(deferredCmdRespDeviceName, 0, sizeof(deferredCmdRespDeviceName));
    // deferredCmdRespResult is a pointer — zeroed after ps_alloc in initEspNow
    // streamQueue is also a pointer — allocated + zeroed in initEspNow
    memset(deferredCmdSrcMac, 0, 6);
    memset(deferredCmdDeviceName, 0, sizeof(deferredCmdDeviceName));
    memset(deferredCmdPayload, 0, sizeof(deferredCmdPayload));
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

// Device management
String getEspNowDeviceName(const uint8_t* mac);
void removeEspNowDevice(const uint8_t* mac);
// True when the MAC has a device-registry entry (this device's own included).
// Any task (a scan of at most 16 records).
bool espnowIsRegistryPeer(const uint8_t mac[6]);
// Note: addEspNowDevice is now static (internal) in espnow_system.cpp

// Command functions
const char* cmd_espnow_status(const String& argsInput);
const char* cmd_espnow_stats(const String& argsInput);
const char* cmd_espnow_routerstats(const String& argsInput);
const char* cmd_espnow_resetstats(const String& argsInput);
const char* cmd_espnow_init(const String& argsInput);
const char* cmd_espnow_deinit(const String& argsInput);  // stop ESP-NOW (used by radiopower off)
const char* cmd_espnow_unpair(const String& argsInput);
const char* cmd_espnow_revoke(const String& argsInput);
const char* cmd_espnow_unrevoke(const String& argsInput);
const char* cmd_espnow_revoked(const String& argsInput);
const char* cmd_espnow_list(const String& argsInput);
const char* cmd_espnow_meshstatus(const String& argsInput);
const char* cmd_espnow_send(const String& argsInput);
const char* cmd_espnow_broadcast(const String& argsInput);
const char* cmd_espnow_sendfile(const String& argsInput);
const char* cmd_espnow_remote(const String& argsInput);
const char* cmd_espnow_startstream(const String& originalCmd);
const char* cmd_espnow_stopstream(const String& originalCmd);

// Test commands (defined in .ino)

// Configuration
const char* cmd_espnow_mode(const String& originalCmd);
// After meshKeysInitAll at boot: every keyed mesh's id from its key, and a
// clash between two meshes' ids keeps the lower slot's (the higher one is off
// the air for this boot, with a WARN). System_ESPNow_MeshIdCore.h.
void espnowMeshIdsAfterKeyInit();

// The device name's ESP-NOW side ('devicename', System_DeviceName.cpp, is the setter).
const char* espnowDeviceNameClearRefusal();
// The rule 'espnowfriendlyname', 'espnowroom' and 'espnowzone' apply (length
// and the metadata text rule), for the setup wizards' staging: true when
// `value` may be stored as that field; otherwise `why` gets the refusal.
enum class EspNowMetaField : uint8_t { FriendlyName, Room, Zone };
bool espnowMetadataFieldOk(EspNowMetaField field, const char* value, char* why, size_t whyCap);
void espnowDeviceNameApplied(const String& name);
// Another device this one knows by `name` (ignoring case), other than the one at
// `exceptMac`: a paired device's alias or a name a peer announces. `who` gets
// "<name> (<address>)".
bool espnowNameInUseBy(const char* name, const uint8_t* exceptMac, char* who, size_t whoCap);

// Streaming. broadcastOutput asks espnowStreamOutputWanted() first: true while
// cmd_exec runs an ESP-NOW command (its output goes to that request's session,
// keyed by the command context's id) or while the legacy startstream sink is
// on and a remote command is in flight.
bool espnowStreamOutputWanted();
void sendEspNowStreamMessage(const String& message);

// Mesh
const char* cmd_espnow_meshrole(const String& originalCmd);
const char* cmd_espnow_meshmaster(const String& originalCmd);
const char* cmd_espnow_meshbackup(const String& originalCmd);
const char* cmd_espnow_backupenable(const String& originalCmd);
const char* cmd_espnow_meshtopo(const String& originalCmd);
const char* cmd_espnow_toporesults(const String& argsInput);

// Encryption
const char* cmd_espnow_setpassphrase(const String& originalCmd);
const char* cmd_espnow_encstatus(const String& argsInput);
const char* cmd_espnow_pairsecure(const String& originalCmd);
void deriveKeyFromPassphrase(const String& passphrase, uint8_t* key);

// WPS-style pairing mode (global timed window; distinct from the per-peer
// espnowOpenPairingWindow KEY_EX re-key window). Broadcasts a discovery beacon
// and auto secure-pairs same-mesh devices whose window is also open.
const char* cmd_espnow_pairmode(const String& argsInput);
void     espnowPairModeOpen(uint32_t seconds);   // clamps to <=600; 0 -> default 120
void     espnowPairModeClose();
bool     espnowPairModeActive();
uint32_t espnowPairModeRemainingMs();

// (Mesh time: System_ESPNow_MeshTime.h.)
// True while a backup master carries the master role because the master is silent.
bool espnowBackupPromoted();
// An ACTIVE session with a paired peer; *establishedAtMs: when it went ACTIVE.
bool espnowSessionUp(const uint8_t* mac, uint32_t* establishedAtMs = nullptr);
// True on a mesh worker while its master session is ACTIVE and a sealed frame
// from the master came within 30 s (channel follow): the Wi-Fi rehunt waits.
// Any task (an atomic read).
bool espnowWorkerHoldsWifiRetry();
// The same, and at least 2 s old: usable for a sender's first frame.
bool espnowSessionSettled(const uint8_t* mac, uint32_t nowMs);
// The sessionId of the ACTIVE session with `mac` (0: none). A new one with
// every session open; a rekey keeps it. Any task (a read).
uint16_t espnowSessionIdOf(const uint8_t* mac);
// The capability word the peer `mac` sent in its session handshake
// (System_ESPNow_SessionCapsCore.h). False when no session with it is ACTIVE
// or REKEYING: unknown caps gate nothing. *confirmed: the word is the peer's
// current one (we opened the session, or a sealed frame of it has opened).
// Any task (a read).
bool espnowPeerSessionCaps(const uint8_t* mac, uint32_t* caps, bool* confirmed = nullptr);

// Helper functions
const char* getMeshRoleString(uint8_t role);
uint32_t generateMessageId();
bool shouldChunk(size_t size);
// Helper to resolve device name or MAC address to MAC bytes
bool resolveDeviceNameOrMac(const String& nameOrMac, uint8_t* outMac);

// Message router (internal - do not call directly)

// Callbacks
void onEspNowDataRecv(const esp_now_recv_info_t* recvInfo, const uint8_t* data, int len);
void onEspNowDataSent(const esp_now_send_info_t* tx_info, esp_now_send_status_t status);

// ESP-NOW command functions (cmd_espnow_stopstream implemented in espnow_system.cpp)
const char* cmd_espnow_send(const String& argsInput);

// Global state (defined in espnow_system.cpp)
extern EspNowState* gEspNow;


// Bond schema sync — sends ESPNOW_V4_TYPE_SCHEMA_REQ to the bonded peer. The
// worker responds with the schema as a file (_schema_out.json) which is
// cached at /system/espnow/peers/<MAC>/schema.json by v4h_file_end →
// processBondSchema. Returns true if the request was sent on-air. Callers
// poll gEspNow->bondSchemaReceived for completion. Master-only.
bool requestBondSchema(const uint8_t* peerMac);

// Copies the cached manifest of a bonded peer's firmware into one PSRAM buffer
// (fwHashHex = 32 lowercase hex characters; anything else is refused before the
// filesystem is touched). The read happens under the filesystem lock; the caller
// prints or sends the buffer after it is released and ps_free()s *out. Returns
// the length, or 0 with *err naming the reason.
size_t espnowReadCachedManifest(const char* fwHashHex, const AuthContext& auth, char** out, const char** err);

// Latest CMD_RESP drained for a command this device sent (any peer). `seq`
// increments on every result and is 0 until the first one; a sender snapshots
// espnowCmdResultSeq() before sending and watches for it to change. Written by
// the CMD_RESP drain on espnow_task; readable from any task (spinlock copy).
struct EspNowCmdResult {
  static constexpr size_t TEXT_MAX = 96;
  uint32_t seq;
  uint32_t reqId;          // msgId of the request this answers
  uint32_t receivedMs;
  bool     success;
  uint8_t  srcMac[6];
  char     text[TEXT_MAX]; // redacted result, first line, NUL-terminated, truncated
};
uint32_t espnowCmdResultSeq();
bool     espnowCmdResultGet(EspNowCmdResult& out);   // false until the first result

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

// Returns true if this device should stream sensor data to a remote receiver.
// A device should stream when it is:
//   • a mesh worker (not the master), OR
//   • a bond-mode worker
// Replaces the identical 7-line block copy-pasted into every sensor task loop.
inline bool shouldStreamSensorToRemote() {
  if (meshEnabled() && gSettings.meshRole != MESH_ROLE_MASTER) return true;
#if ENABLE_BONDED_MODE
  if (gSettings.bondModeEnabled && isBondWorker()) return true;
#endif
  return false;
}

// Mesh peer state (used by OLED and status views)
extern MeshPeerHealth* gMeshPeers;   // Dynamically allocated [gMeshPeerSlots] at init

// Mesh helper utilities (moved from .ino)
bool isMeshPeerAlive(const MeshPeerHealth* peer);
bool isMeshPeerRecentlyActive(const MeshPeerHealth* peer);
MeshPeerHealth* getMeshPeerHealth(const uint8_t mac[6], bool createIfMissing = false);
// Compare against the checked STA identity cached for this ESP-NOW instance.
// While stopped, a checked driver query preserves the existing helper behavior.
bool isSelfMac(const uint8_t* mac);
// A peer's state (System_ESPNow_PeerStateCore.h): its mesh heartbeat
// (isMeshPeerAlive) and its encrypted session (espnowSessionUp), as every
// surface reads them. Reads only, any task (the reader risk of gMeshPeers).
inline EspNowPeerState espnowPeerState(const uint8_t mac[6]) {
  const MeshPeerHealth* health = getMeshPeerHealth(mac, false);
  return espnowPeerStateOf(health && isMeshPeerAlive(health), espnowSessionUp(mac));
}
// The paired peers (gEspNow->devices[] without this device) by state; all
// zero while ESP-NOW is stopped.
inline EspNowPeerCounts espnowPeerCounts() {
  EspNowPeerCounts counts;
  if (!gEspNow || !gEspNow->initialized) return counts;
  const int cap = (int)(sizeof(gEspNow->devices) / sizeof(gEspNow->devices[0]));
  const int n = gEspNow->deviceCount < cap ? gEspNow->deviceCount : cap;
  for (int i = 0; i < n; i++) {
    const uint8_t* mac = gEspNow->devices[i].mac;
    if (isSelfMac(mac)) continue;
    counts.add(espnowPeerState(mac));
  }
  return counts;
}
void macFromHexString(const String& s, uint8_t out[6]);
// Zero-allocation MAC formatter for debug log call sites.
// Uses a GCC statement expression + a function-static char[18] — no heap, no
// String. Usage: DEBUG_ESPNOWF("... %s", MAC_STR(mac))  instead of  macToDisplayStr(mac).c_str()
//
// The static lives in PSRAM (EXT_RAM_BSS_ATTR). Each expansion site mints its
// own 18-byte static, so the ~37 sites in this build cost ~666 B of internal
// DRAM before this. Verified 2026-08-19: every expansion runs in task context
// (espnow_task ring drain, cmd_exec, httpd) — neither onEspNowDataReceived nor
// onEspNowDataSent uses it, no IRAM_ATTR caller, no critical section.
//
// BECAUSE this is a header macro, every FUTURE expansion moves to PSRAM too.
// Do NOT use MAC_STR from an ISR, an IRAM_ATTR function, or inside
// portENTER_CRITICAL/taskENTER_CRITICAL: PSRAM is unreachable with the flash
// cache off and slow under a spinlock. From the WiFi-task ESP-NOW callbacks it
// is technically reachable (cache on), but prefer a stack buffer there.
// (Pre-existing and PSRAM-independent: the static is shared per site, so two
// tasks expanding the same site concurrently can tear the string.)
#define MAC_STR(mac) \
  ({ EXT_RAM_BSS_ATTR static char _macbuf[18]; \
     snprintf(_macbuf, sizeof(_macbuf), MAC_FMT, MAC_ARGS(mac)); \
     (const char*)_macbuf; })

// Topology helpers (moved from .ino)
int requestTopologyDiscovery();  // peers that took the request; -1 when none was started
void sendTopologyResponse(uint32_t reqId, const uint8_t masterMac[6], JsonArray requestPath);
void checkTopologyCollectionWindow();  // Check collection window and finalize when timeout expires

void meshSendBootToPeers(const String& envelope);  // boot notice via ESPNOW_V4_TYPE_BOOT — RX stores it as MSG_SYSTEM_EVENT, not chat

// Topology state (for auto-discovery check in loop)
extern uint32_t gLastTopoRequest;

// Mesh message builders
String buildBootNotification(uint32_t msgId, const char* src, uint32_t bootCounter, uint32_t timestamp);

// V2 envelope builder (for user sync and other features)
void v2_init_envelope(JsonDocument& doc, const char* type, uint32_t msgId, const char* src, const char* dst, int ttl);
uint32_t generateMessageId();

// Custom JSON serializers for gSettings.meshes[] and
// the per-mesh bond arrays (the SettingsRegistry pattern can't handle
// arrays of structs). Called from System_Settings.cpp's save/load paths,
// mirroring the BLE_Peers writeJson/readJson pattern.
void espnowMeshesWriteJson(JsonDocument& doc, bool excludePasswords = false);
void espnowMeshesReadJson(JsonDocument& doc);

// Mesh heartbeat processing (FreeRTOS task)
void processMeshHeartbeats();  // Internal worker function (called by task)
// Flag an ESP-NOW channel re-sync after a WiFi association change. Called from
// the WiFi event handler (System_WiFi.cpp); the actual re-derive + re-pin runs
// on espnow_task in processMeshHeartbeats. Cheap/ISR-safe: sets a volatile flag.
void espnowNoteWifiChannelMayHaveChanged();

// Discover-then-confirm pairing — read accessors for the OLED/G2/web UI.
int  espnowGetPairCandidateCount();                                              // pruned live count
bool espnowGetPairCandidate(int idx, char* name, size_t nameCap, uint8_t mac[6], int* rssi);
// Pending accept/reject; optional `mac` (6 bytes) + `atMs` identify the request.
bool espnowGetIncomingPairRequest(char* name, size_t nameCap,
                                  uint8_t* mac = nullptr, uint32_t* atMs = nullptr);
// The last outgoing pair request: waiting, or its result for 30 s after it
// ends; `name` (optional) gets the peer's name. Mirrors espnowpair::Outcome.
enum EspNowPairOutcome : uint8_t {
  ESPNOW_PAIR_OUT_NONE = 0,
  ESPNOW_PAIR_OUT_WAITING,
  ESPNOW_PAIR_OUT_PAIRED,
  ESPNOW_PAIR_OUT_DECLINED,
  ESPNOW_PAIR_OUT_TIMED_OUT,
};
EspNowPairOutcome espnowGetPairRequestOutcome(char* name, size_t nameCap);
bool startEspNowTask();        // Start ESP-NOW heartbeat task
// Cooperatively stop and join the heartbeat/RX task. On timeout the task and
// its resources remain intact so closeespnow can retry safely.
bool stopEspNowTask(uint32_t timeoutMs);
// Safe diagnostic snapshot: holds the heartbeat lifecycle owner across task
// lookup + HWM query so closeespnow cannot delete the task between them. The
// returned handle is an opaque identity key only; do not query it later.
bool getEspNowTaskStackSnapshot(TaskHandle_t* outHandle,
                                UBaseType_t* outWatermark);

// True while a bounded close retained live resources for a later retry.
bool espnowShutdownPending();

// The radio-ownership answer for Wi-Fi decisions (mode, driver stop/deinit,
// power-down): ESP-NOW holds the radio while it is initialized and also while
// a timed-out close still retains espnow_task, espnow_tx and the driver
// (deinitEspNow clears `initialized` before its bounded waits). Use this, not
// isEspNowInitialized(), wherever the radio could be taken from under ESP-NOW.
bool espnowOwnsRadio();

// Espressif's ESP-NOW version (1 or 2, esp_now_get_version) as read at the last
// successful init; 0 before that. On a P4 it is the companion's, read through
// the bridge at that init.
uint32_t espnowStackVersion();

// Centralized driver-peer deletion boundary for UI/first-party callers. It
// rejects while ESP-NOW is stopped/closing and is included in close quiescence.
bool espnowDeletePeerRuntime(const uint8_t mac[6]);

// Message handling (for command execution)
void sendChunkedResponse(const uint8_t* targetMac, bool success, const String& result, const String& senderName);

// Per-device message buffer management (message buffer section of System_ESPNow.cpp).
// One lock serializes every store and read. A peer's history block (a received
// and a sent byte ring) is allocated by its first store, outside the lock; a
// store returns false when no slot is free, the block cannot be allocated, the
// message numbers are used up or the lock timed out (the reason is said at most
// once a minute). Reads never create a slot and return 0 when the peer has none.
// peerName is no longer stored: with HistoryAttribution::FromName a null/empty
// name or the MAC text marks the record unattributed (shown under the MAC text);
// otherwise readers show the peer's current name (espnowHistoryDisplayName).
bool storeMessageInPeerHistory(uint8_t* peerMac, const char* peerName, const char* message, bool encrypted,
                               LogMessageType msgType, uint32_t reqId = 0, uint8_t piece = 1,
                               uint8_t pieceTotal = 1,
                               HistoryAttribution attribution = HistoryAttribution::FromName);
// Record a message WE sent to peerMac into that peer's sent ring (the shared
// outgoing history every interface reads). Long text is split into ≤200 B
// fragments sharing msgId — same per-fragment model as received — so nothing is
// truncated (at most 255 pieces). Call once at the send chokepoint
// (cmd_espnow_send). msgId is the send's correlation id (used for
// delivery-status polling + web bubble de-dup).
bool storeSentMessageInPeerHistory(uint8_t* peerMac, const char* message, uint32_t msgId);

// Received-side chunked store: split a (possibly long) received message — e.g. a
// remote command's result — into <=200 B pieces sharing reqId (at most 255
// pieces), so the feed delivers reassemblable pieces instead of one record
// truncated at 255 B. Mirrors the sent path; the client stitches by reqId.
bool storeReceivedMessageChunked(uint8_t* peerMac, const char* peerName, const char* message,
                                 bool encrypted, LogMessageType msgType, uint32_t reqId,
                                 HistoryAttribution attribution = HistoryAttribution::FromName);
// Stamp the DURABLE delivery state onto the sent record(s) for msgId (all
// fragments share it). Called from the sendStatus terminal transitions
// (delivered / failed / timeout) so the conversation shows the resolved state
// long after the ephemeral sendStatus ring entry is swept. state mirrors
// SendStatusState; a state above 3 is ignored. No-op if the peer/msgId isn't in
// history.
void espnowUpdateSentDeliveryState(const uint8_t* peerMac, uint32_t msgId, uint8_t state);
void logFileTransferEvent(uint8_t* peerMac, const char* peerName, const char* filename, LogMessageType eventType);
// getPeerMessages / getAllMessages merge BOTH rings (received + sent) of one peer
// (getPeerMessages) or of every peer (getAllMessages, a k-way merge), ascending
// by seqNum: the lowest seqs above sinceSeq come first, so a poller paging on
// sinceSeq sees one unified, time-ordered stream and never skips another peer's
// lower seqs. Each record's isSent flag carries its direction; peerMac names the
// peer whose history holds it.
int getPeerMessages(uint8_t* peerMac, ReceivedTextMessage* outMessages, int maxMessages, uint32_t sinceSeq);
int getAllMessages(ReceivedTextMessage* outMessages, int maxMessages, uint32_t sinceSeq);

// Chunked-message collapse VIEW (shared by on-device displays: OLED and G2).
// Chunked text is stored as separate per-fragment records sharing a reqId (see the
// textQueue drain in System_ESPNow.cpp). Remote clients (web/BLE phone) reassemble
// those fragments themselves; on-device displays have no remote client, so this
// helper gives them ONE logical entry per message without copying text:
//   - a ref is a handle to the message's lowest present fragment: peer slot,
//     direction and that record's seqNum, plus an offset hint. It holds no
//     pointer and is seq-anchored: espnowCopyCollapsedHead copies the record
//     only while that seq is still live in the ring (never trusting the
//     offset) and refuses it once the record was dropped. Full text
//     (>1 fragment) is assembled by espnowReassembleByReqId.
//   - `partsPresent` / `partsTotal` let the caller flag multi-part / incomplete
//     messages (partsTotal == 1 for ordinary single-frame text).
//   - the view keeps the NEWEST maxN logical messages; maxN above
//     ESPNOW_HISTORY_VIEW_MAX is clamped to it, so size buffers at
//     ESPNOW_HISTORY_VIEW_MAX. A fragment joins its group when the group is
//     among the last 64 logical messages of the same peer and direction.
// espnowCollapsedPeerMessages (received ring only) is chronological (oldest→newest).
using CollapsedMsgRef = espnowhist::Ref;
int espnowCollapsedPeerMessages(uint8_t* peerMac, CollapsedMsgRef* out, int maxN);

// Collapsed, direction-tagged, time-ordered CONVERSATION with one peer — the
// shared read used by on-device UIs (OLED and G2) so each interface shows
// the same merged sent+received timeline without re-implementing the merge.
// Collapses both rings (received + sent), keeps the newest maxN and orders the
// result oldest→newest by timestamp; each ref's isSent says which side it is.
// Refs are handles (see CollapsedMsgRef). Returns the number written.
int espnowGetConversation(uint8_t* peerMac, CollapsedMsgRef* out, int maxN);

// Collapsed, direction-tagged, time-ordered GLOBAL inbox across ALL peers — the
// all-peers analogue of espnowGetConversation, for a "messages from everyone"
// view (G2 global inbox). Collapses every peer's received + sent rings, keeps
// the newest maxN logical messages and orders them oldest→newest by timestamp.
// Refs are handles (see CollapsedMsgRef). Returns the number written.
int espnowCollapsedAllMessages(CollapsedMsgRef* out, int maxN);

// Reassemble the full text of a multi-fragment message (gathered by reqId) into
// `out`, in piece order — the reader-side equivalent of the web/BLE reqId-stitch,
// so on-device UIs can show the whole message. reqId==0 returns 0 (use the
// record's own text). `complete` (optional) is true only if all fragments were
// present. Returns bytes written (excluding NUL).
int espnowReassembleByReqId(const uint8_t* peerMac, uint32_t reqId, bool isSent,
                            char* out, size_t cap, bool* complete = nullptr);

// Copy the head record of a collapsed message into caller-owned storage.
// Returns false when the record was dropped since the collapse (its seq is no
// longer live in that ring), the slot is out of range or empty, or the store
// lock timed out.
bool espnowCopyCollapsedHead(const CollapsedMsgRef& ref, ReceivedTextMessage* out);

// Received-ring record count for one peer (mac) or for every peer (mac == nullptr).
// 0 when the peer has no slot or the store lock timed out.
int espnowHistoryMessageCount(const uint8_t* mac);

// "espnowhistorykb" with no argument: saved setting, active latched ring size, peers, KB in use.
// Writes at most cap-1 chars + NUL; out null or cap 0 -> nothing. Takes no lock (int reads).
void espnowHistoryStatusLine(char* out, size_t cap);

// The name a reader shows for a copied record (contract):
// - "" for a sent record; the MAC text for an unattributed received record; else the peer's current
//   name (an active gMeshPeerMeta entry's name, then gEspNow->devices[].name: the order of
//   getEspNowDeviceName), else the MAC text. Never "" for a received record.
// - Writes at most cap-1 chars + NUL; out == nullptr or cap == 0 -> nothing written.
// - Safe when ESP-NOW is closed (gEspNow set, not initialised) and when gEspNow or gMeshPeerMeta is null.
// - Takes GroupLockGuard for the lookup. Never call it while holding gPeerHistoryLock or gGroupLock.
// - Reads only fixed char storage (MeshPeerMeta::name, EspNowInlineText<32>::c_str() of the inline
//   buffer) through espnowhist::boundedCopy, never an Arduino String. Loops are bounded by
//   min(gMeshPeerSlots, MESH_PEER_MAX) and min(deviceCount, 16), each read once.
// - Cost: at most MESH_PEER_MAX + 16 MAC compares and one 32-byte copy. Callers resolve once per
//   distinct peer per call (web, CLI, G2), not once per row.
void espnowHistoryDisplayName(const ReceivedTextMessage& m, char* out, size_t cap);

// Serialize one mesh peer's metadata + liveness into a JSON object. Single
// source of truth for the peer-metadata shape, shared by the `espnowdevices
// json` CLI branch and the web /api/espnow/metadata handler so the field set
// lives in exactly one place (and both get ArduinoJson string escaping).
// Adds online/lastSeenSec from MeshPeerHealth on top of the stored metadata.
void espnowSerializeMeshPeerMeta(JsonObject o, const MeshPeerMeta& m);

// File transfer to a specific MAC. Synchronous: pumps FILE_START/DATA/END on the
// calling task (cmd_exec for every caller). Refuses to start — NoSession, with
// the handshake kicked — unless the encrypted session with the peer is ACTIVE:
// a file cannot be carried across a handshake (one pending frame per peer).
FileSendResult sendFileToMacResult(const uint8_t* mac, const String& localPath);
// Same send; true only for FileSendResult::Sent.
bool sendFileToMac(const uint8_t* mac, const String& localPath);

#if ENABLE_BONDED_MODE
// Check if bond mode is active and peer is online
bool isBondModeOnline();
// Check if bond mode is active, peer is online, AND fully synced (cap + manifest + settings)
bool isBondSynced();

// What the bond partner said about its side of the bond (BOND_REQUEST /
// BOND_NOTICE, System_ESPNow_BondAgreeCore.h). The bond settings on this device
// are not part of it: a partner that declined is still the configured partner.
enum BondAgreementState : uint8_t {
  BOND_AGREEMENT_UNKNOWN  = 0,  // no answer yet (or a partner whose firmware does not answer)
  BOND_AGREEMENT_AGREED   = 1,  // the partner has this device as its partner
  BOND_AGREEMENT_DECLINED = 2,  // the partner has no bond with this device; `reason` says why
};
struct BondAgreementView {
  uint8_t state;        // BondAgreementState
  uint8_t reason;       // BondNoticeReason while declined, else 0
  bool    roleConflict; // agreed, but both devices have the same role
};
// Any task. All zero when bond mode is off or no partner is set.
BondAgreementView bondAgreementView();
// Counts the partner changes begun (bondWithFramesHeld): what was decided
// under another value may name a partner that changed.
uint32_t bondHoldEpochNow();
// True while a partner change runs (bond frames held off), or once one began
// after `epoch` (bondHoldEpochNow) was read: the bond settings and the
// agreement may be half old, half new. Any task.
bool bondHeldSince(uint32_t epoch);
// The agreement as every JSON carries it (bondstatus json, /api/bond/status,
// the system status): "unknown", "agreed" or "declined", and why:
// "not_bonded", "bonded_elsewhere" or "ended" while declined, "role_conflict"
// while agreed with both devices in the same role, otherwise "".
const char* bondAgreementKey(const BondAgreementView& view);
const char* bondAgreementReasonKey(const BondAgreementView& view);
// One line for a status page: the agreement in words, with the partner's name
// ("no partner is set" without a bond).
void bondAgreementDescribe(char* out, size_t outLen);
#else
// Stubs when bond mode is compiled out
inline bool isBondModeOnline() { return false; }
inline bool isBondSynced() { return false; }
#endif // ENABLE_BONDED_MODE

// =============================================================================
// ESP-NOW App ping (used by the on-glasses ESP-NOW App page).
// =============================================================================
// Sends a HEARTBEAT-with-ACK_REQ to one peer, records msgId + startMs, and
// classifies the response in the existing ACK RX path. There is exactly one
// in-flight ping slot — starting a new ping while one is pending clobbers
// the previous attempt's bookkeeping (the previous ACK, if it ever lands,
// is silently ignored).
//
// Lifecycle: Idle → Pending (after Start) → Ok (rttMs set) | Timeout.
//   espnowAppPingStart(mac) — fires the probe, transitions to Pending.
//   espnowAppPingState(...) — read current state; recomputes Timeout if
//                              startMs is older than timeoutMs.
//   espnowAppPingClear()   — force back to Idle.
//
// The state read is intentionally one-shot polled by the page's live-refresh
// tick rather than push-driven; the bookkeeping is one cacheline of static
// state, no task / no queue.
enum class EspNowAppPingState : uint8_t {
  Idle    = 0,
  Pending = 1,
  Ok      = 2,
  Timeout = 3,
};

bool espnowAppPingStart(const uint8_t* mac);
EspNowAppPingState espnowAppPingPoll(uint32_t* outRttMs,
                                     uint8_t* outPeerMac /* may be null */,
                                     uint32_t timeoutMs = 2000);
void espnowAppPingClear();
void espnowAppPingNoteAck(const uint8_t* src, uint32_t msgId);

#else // !ENABLE_ESPNOW

// ============================================================================
// ESP-NOW Stubs (when ENABLE_ESPNOW is disabled)
// EspNowMode and MeshRole enums are defined above the guard.
// Simplified stub structs are provided here for type compatibility.
//
// DESIGN NOTE — stub field divergence is intentional:
//   The real MeshPeerHealth/MeshTopoNode (above, under #if ENABLE_ESPNOW)
//   carry full telemetry fields (lastMeshHeartbeatMs, lastRxActivityMs, heartbeatCount, ackCount,
//   lastBootCounter, _pad, etc.).  The stubs below are deliberately minimal —
//   they only need to satisfy the compiler for *declarations* and for the few
//   inline no-op stubs that accept MeshPeerHealth* parameters.
//
//   CONTRACT: any C++ code that accesses fields beyond { mac, isActive, rssi }
//   MUST be inside a #if ENABLE_ESPNOW guard.  Do NOT add field accesses to
//   shared (unguarded) code without aligning both struct definitions.
// ============================================================================

#define MAX_MESH_PEERS MESH_PEER_MAX  // alias — canonical value defined above #if ENABLE_ESPNOW

struct MeshPeerHealth {
  uint8_t mac[6];
  String name;
  bool isOnline;
  bool isActive;
  uint32_t lastSeen;
  int8_t rssi;
  uint8_t role;
};

struct MeshTopoNode {
  uint8_t mac[6];
  String name;
  uint8_t role;
  bool isOnline;
};

struct EspNowState {
  bool initialized;
  EspNowMode mode;
  String passphrase;
  uint8_t channel;
  bool encryptionEnabled;
  void* reserved;
};

extern EspNowState* gEspNow;
extern MeshPeerHealth gMeshPeers[MAX_MESH_PEERS];
extern MeshTopoNode* gMeshTopology;

inline const char* checkEspNowFirstTimeSetup() { return "ESP-NOW disabled"; }
inline const char* cmd_espnow_init(const String& argsInput) { return "ESP-NOW disabled"; }
inline void sendEspNowStreamMessage(const char* topic, const char* payload) {}
inline bool isSelfMac(const uint8_t* mac) { return false; }
inline bool isMeshPeerAlive(const MeshPeerHealth* peer) { return false; }
inline bool isMeshPeerRecentlyActive(const MeshPeerHealth*) { return false; }
inline void noteMeshPeerRxActivity(const uint8_t*, EspNowMeshRxKind, int8_t = -128, int8_t = -128, bool = false) {}
inline bool meshEnabled() { return false; }
inline bool resolveDeviceNameOrMac(const String& nameOrMac, uint8_t* outMac) {
  (void)nameOrMac; (void)outMac;
  return false;
}
inline FileSendResult sendFileToMacResult(const uint8_t* mac, const String& localPath) {
  (void)mac; (void)localPath;
  return FileSendResult::Failed;
}
inline bool sendFileToMac(const uint8_t* mac, const String& localPath) {
  (void)mac; (void)localPath;
  return false;
}
inline bool isBondModeOnline() { return false; }
inline bool isBondSynced() { return false; }
// Compiled out, ESP-NOW never claims the radio.
inline bool espnowOwnsRadio() { return false; }

// Ping stubs — see real declarations above the ENABLE_ESPNOW else.
enum class EspNowAppPingState : uint8_t {
  Idle    = 0,
  Pending = 1,
  Ok      = 2,
  Timeout = 3,
};
inline bool espnowAppPingStart(const uint8_t*) { return false; }
inline EspNowAppPingState espnowAppPingPoll(uint32_t* outRttMs,
                                            uint8_t* outPeerMac,
                                            uint32_t /*timeoutMs*/ = 2000) {
  if (outRttMs)  *outRttMs  = 0;
  (void)outPeerMac;
  return EspNowAppPingState::Idle;
}
inline void espnowAppPingClear() {}
// WiFi-only profiles have no channel follower.
inline void espnowNoteWifiChannelMayHaveChanged() {}
inline bool espnowWorkerHoldsWifiRetry() { return false; }

#endif // ENABLE_ESPNOW

// Send paths, pairing and bond helpers used outside System_ESPNow.cpp.
#if ENABLE_ESPNOW
// Notifies bond logic the moment a session with
// `peerMac` reaches ACTIVE so the bonded relationship can use the encrypted
// session for discovery/sync (replaces plaintext-heartbeat discovery). No-op
// when bond mode is off or the peer isn't our bonded peer.
void bondNotifySessionEstablished(const uint8_t* peerMac);
String buildBondedCommandPayload(const String& command);
bool espnowConsumePairingWindow(const uint8_t mac[6]);
bool isBondSessionTokenValid();
// True when bond mode is on, a partner is configured and `mac` is that partner:
// the one owner of "is this our bond partner" (receive table, session hook,
// bond magic files, file RPC). False when bonded mode is compiled out.
bool isBondPeerMac(const uint8_t* mac);
// SENSOR_STATUS to the receiver of this device's readings (masks in the one sensor numbering).
bool v4_send_sensor_status(const uint8_t* dstMac, uint32_t presentMask, uint32_t streamingMask, uint16_t bootTag);
// One built part of a reading: SENSOR_ENVELOPE, or BOND_SENSOR_DATA to the bond partner.
bool v4_send_sensor_part(const uint8_t* dstMac, bool viaBond, const uint8_t* part, uint16_t len);
// A bond worker's sensor status to its bond partner as BOND_SENSOR_STATUS (185),
// the 151 payload (false in a build without bond mode).
bool v4_send_bond_sensor_status(const uint8_t* dstMac, uint32_t presentMask, uint32_t streamingMask, uint16_t bootTag);
// `mac` is paired in this device's primary mesh (the default slot of
// gSettings.meshes): a bond master forwards its partner's data only there.
bool espnowPeerInPrimaryMesh(const uint8_t* mac);
// A bond worker's battery state (180) and events (182) to its bond partner,
// with the 152/132 payloads (false in a build without bond mode).
bool v4_send_bond_power(const uint8_t* dstMac, const uint8_t* payload, uint16_t len);
bool v4_send_bond_event(const uint8_t* dstMac, const uint8_t* payload, uint16_t len);
// Longest plaintext one frame to `dst` carries (202 direct, 146 through a relay).
uint16_t v4_single_frame_cap(const uint8_t* dst);
bool v4_send_encrypted_or_queue(const uint8_t dst[6], uint8_t type, uint16_t baseFlags, uint32_t msgId, const uint8_t* plaintext, uint16_t plaintextLen, uint8_t ttl, char* outStatus = nullptr, size_t outStatusLen = 0);
bool v4_send_session_wrapped(const uint8_t dst[6], uint8_t type, uint16_t baseFlags, uint32_t msgId, const uint8_t* plaintext, uint16_t plaintextLen, uint8_t ttl, char* errOut, size_t errOutLen);
bool v4_send_user_sync(const uint8_t* dst, const char* jsonPayload, uint16_t jsonLen);

#if ENABLE_BONDED_MODE
void buildLocalBondStatus(BondPeerStatus& status);
String loadSettingsFromCache(const uint8_t* peerMac);
bool sendBondStreamCtrl(RemoteSensorType sensorType, bool enable);
#endif

#endif

#endif // SYSTEM_ESPNOW_H
