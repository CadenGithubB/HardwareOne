#ifndef SYSTEM_ESPNOW_WIRE_H
#define SYSTEM_ESPNOW_WIRE_H

// ============================================================================
// ESPNOW V3 wire schema — opcode enum, flags, header layout, payload structs.
//
// Extracted from System_ESPNow.cpp as part of Phase 0 of the V4 plan
// (docs/ESPNOW_V4_PLAN.md). This is a pure relocation — values and layouts
// are unchanged.
//
// Restrictions on contents of this file:
//   - Value-typed declarations only (enums, packed structs, constants).
//   - No function declarations, no globals, no logging.
//   - Anything that would create a dependency back into the ESPNOW
//     implementation belongs in System_ESPNow.h / .cpp, not here.
// ============================================================================

#include <Arduino.h>
#include "System_BuildConfig.h"

#if ENABLE_ESPNOW

// ---- Protocol constants ----------------------------------------------------

#define ESPNOW_V3_MAGIC        0x3148u
#define ESPNOW_V3_MAX_PAYLOAD  (250 - 24)  // 226 bytes max payload (frame minus header)

// ---- Opcode enum -----------------------------------------------------------

enum EspNowV3Type : uint8_t {
  ESPNOW_V3_TYPE_ACK             = 1,
  ESPNOW_V3_TYPE_BOND_CAP_REQ    = 2,
  ESPNOW_V3_TYPE_BOND_CAP_RESP   = 3,
  ESPNOW_V3_TYPE_TEXT            = 4,
  ESPNOW_V3_TYPE_CMD             = 5,
  ESPNOW_V3_TYPE_CMD_RESP        = 6,
  ESPNOW_V3_TYPE_HEARTBEAT       = 7,
  ESPNOW_V3_TYPE_FILE_START      = 8,
  ESPNOW_V3_TYPE_FILE_DATA       = 9,
  ESPNOW_V3_TYPE_FILE_END        = 10,
  ESPNOW_V3_TYPE_MANIFEST_REQ    = 11,
  ESPNOW_V3_TYPE_MANIFEST_RESP   = 12,
  ESPNOW_V3_TYPE_STREAM          = 13,
  ESPNOW_V3_TYPE_BOND_HEARTBEAT  = 14,
  ESPNOW_V3_TYPE_SENSOR_DATA     = 15,  // Binary sensor data (bond mode)
  ESPNOW_V3_TYPE_SETTINGS_REQ    = 16,  // Request settings from bonded device
  ESPNOW_V3_TYPE_SETTINGS_RESP   = 17,  // Settings response (JSON payload)
  ESPNOW_V3_TYPE_SETTINGS_PUSH   = 18,  // RESERVED (push removed — settings changes use remote commands)
  ESPNOW_V3_TYPE_METADATA_REQ    = 19,  // Request peer's metadata
  ESPNOW_V3_TYPE_METADATA_RESP   = 20,  // Metadata response
  ESPNOW_V3_TYPE_METADATA_PUSH   = 21,  // Push metadata update (when changed)
  ESPNOW_V3_TYPE_TIME_SYNC       = 22,  // Time synchronization (epoch + millis)
  ESPNOW_V3_TYPE_TOPO_REQ        = 23,  // Topology discovery request
  ESPNOW_V3_TYPE_TOPO_START      = 24,  // Topology response start (peer count)
  ESPNOW_V3_TYPE_TOPO_PEER       = 25,  // Topology response peer entry
  ESPNOW_V3_TYPE_USER_SYNC       = 26,  // User data synchronization
  ESPNOW_V3_TYPE_WORKER_STATUS   = 27,  // Worker status report to master (detailed)
  ESPNOW_V3_TYPE_SENSOR_STATUS   = 28,  // Sensor status broadcast (enabled/disabled)
  ESPNOW_V3_TYPE_SENSOR_BROADCAST= 29,  // Sensor data broadcast to mesh
  ESPNOW_V3_TYPE_BOND_STATUS_REQ = 30,  // Request live status from bonded peer
  ESPNOW_V3_TYPE_BOND_STATUS_RESP= 31,  // Live status response (BondPeerStatus payload)
  ESPNOW_V3_TYPE_STREAM_CTRL     = 32,  // Stream control (master->worker: start/stop sensor streaming)
};

// ---- Flag bits -------------------------------------------------------------

enum EspNowV3Flags : uint8_t {
  ESPNOW_V3_FLAG_ACK_REQ      = 0x01,  // Request ACK from receiver
  ESPNOW_V3_FLAG_ENCRYPTED    = 0x02,  // Payload is encrypted
  ESPNOW_V3_FLAG_COMPRESS     = 0x04,  // Payload is compressed (future)
  ESPNOW_V3_FLAG_STREAM_BEGIN = 0x10,  // First chunk of stream
  ESPNOW_V3_FLAG_STREAM_END   = 0x20,  // Last chunk of stream
};

// ---- Frame header ----------------------------------------------------------

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

// ---- Payload structs -------------------------------------------------------

struct __attribute__((packed)) V3PayloadHeartbeat {
  uint8_t  role;
  uint8_t  peerCount;
  int8_t   rssi;
  uint8_t  reserved;
  uint32_t uptimeSec;
  uint32_t freeHeap;
  char     deviceName[20];
};
static_assert(sizeof(V3PayloadHeartbeat) == 32, "V3PayloadHeartbeat must be 32 bytes");

// Time sync payload
struct __attribute__((packed)) V3PayloadTimeSync {
  uint32_t epochTime;     // Unix epoch time
  int64_t  timeOffset;    // Time offset in milliseconds
  uint32_t senderUptime;  // Sender uptime in seconds
};

// Topology request payload
struct __attribute__((packed)) V3PayloadTopoReq {
  uint32_t reqId;       // Request ID for correlation
  uint8_t  reserved[4]; // Padding for alignment
};

// Topology start payload (first message in topology response)
struct __attribute__((packed)) V3PayloadTopoStart {
  uint32_t reqId;       // Matches request ID
  uint8_t  peerCount;   // Number of TOPO_PEER messages to follow
  uint8_t  reserved[3];
};

// Topology peer entry (one per peer)
struct __attribute__((packed)) V3PayloadTopoPeer {
  uint32_t reqId;       // Matches request ID
  uint8_t  peerIndex;   // Which peer (0-based)
  uint8_t  isLast;      // 1 if this is the last peer
  uint8_t  mac[6];      // Peer MAC
  int8_t   rssi;        // Last RSSI
  uint8_t  encrypted;   // 1 if encrypted
  char     name[32];    // Peer name
};

// Worker status payload (detailed, for master consumption)
struct __attribute__((packed)) V3PayloadWorkerStatus {
  uint32_t freeHeap;
  uint32_t totalHeap;
  int8_t   rssi;
  uint8_t  gThermalEnabled;
  uint8_t  gImuEnabled;
  uint8_t  reserved;
  char     name[20];
  // Metadata fields follow as variable JSON payload if needed
};

#if ENABLE_BONDED_MODE
// Bond mode heartbeat payload (lightweight)
struct __attribute__((packed)) V3PayloadBondHeartbeat {
  uint8_t  role;           // Bond role (master/worker)
  int8_t   rssi;           // WiFi RSSI
  uint8_t  reserved[2];    // Padding for alignment
  uint32_t uptimeSec;      // Uptime in seconds
  uint32_t freeHeap;       // Free heap bytes
  uint32_t seqNum;         // Sequence number for tracking
  uint32_t bootCounter;    // Persistent boot counter
  uint32_t settingsHash;   // Hash of local settings (exclude passwords)
};

// Binary sensor data payload for bond mode streaming (compact, no JSON overhead)
// Max payload is 226 bytes, header is 8 bytes, leaving 218 bytes for sensor data
struct __attribute__((packed)) V3PayloadSensorData {
  uint8_t  sensorType;     // RemoteSensorType enum value
  uint8_t  flags;          // Bit 0: valid, Bit 1: streaming enabled
  uint16_t dataLen;        // Length of data[] that follows
  uint32_t seqNum;         // Sequence number for ordering
  uint8_t  data[];         // Variable-length sensor data (flexible array member)
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
  uint8_t  sensorType;  // RemoteSensorType enum value
  uint16_t dataLen;     // Length of JSON data that follows
  uint8_t  reserved;    // Padding for alignment
  uint8_t  data[];      // Variable-length JSON data (flexible array member)
};

// Metadata payload for metadata exchange (REQ/RESP/PUSH)
// Total: 180 bytes (fits comfortably in 226 byte payload limit)
struct __attribute__((packed)) V3PayloadMetadata {
  char    deviceName[32];
  char    friendlyName[48];
  char    room[32];
  char    zone[32];
  char    tags[64];
  uint8_t stationary;
  uint8_t reserved[3];    // Padding for future fields
};

// Command response payload
struct __attribute__((packed)) V3PayloadCmdResp {
  uint8_t success;                          // 1=success, 0=failure
  char    result[ESPNOW_V3_MAX_PAYLOAD - 1]; // Null-terminated result (truncated if needed)
};

// File transfer payloads
struct __attribute__((packed)) V3PayloadFileStart {
  uint32_t fileSize;      // Total file size in bytes
  uint16_t chunkCount;    // Total number of chunks
  uint16_t chunkSize;     // Size of each chunk (except last)
  char     filename[64];  // Destination filename
};

struct __attribute__((packed)) V3PayloadFileData {
  uint16_t chunkIndex;    // Chunk index (0-based)
  uint8_t  data[ESPNOW_V3_MAX_PAYLOAD - 2];  // Chunk data (224 bytes max)
};

struct __attribute__((packed)) V3PayloadFileEnd {
  uint32_t crc32;         // CRC32 of entire file
  uint8_t  success;       // 1=transfer complete, 0=aborted
};

#endif // ENABLE_ESPNOW

#endif // SYSTEM_ESPNOW_WIRE_H
