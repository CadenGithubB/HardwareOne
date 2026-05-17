#ifndef SYSTEM_ESPNOW_WIRE_H
#define SYSTEM_ESPNOW_WIRE_H

// ============================================================================
// ESPNOW V4 wire schema — opcode enum, flags, 32-byte header, payload structs.
//
// Phase 1 of docs/ESPNOW_V4_PLAN.md cutover. Wire format is incompatible
// with V3 — magic is the same (0x3148) but version byte is 4 and the header
// grows from 24 → 32 bytes to make room for fields Phases 2/3 fill in
// (meshFingerprint, sessionId, frameSeq). Opcodes are renumbered into
// category-based ranges with reserved slots for future phases.
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

#define ESPNOW_V4_MAGIC        0x3148u       // 'H1' little-endian (unchanged from V3)
#define ESPNOW_V4_VERSION      4             // Protocol version
#define ESPNOW_V4_HEADER_LEN   32            // Header size in bytes (was 24 in V3)
#define ESPNOW_V4_MAX_PAYLOAD  (250 - 32)    // 218 bytes max payload (was 226 in V3)

// Phase 3.5 — when a frame is wrapped in SESSION_FRAME, 16 of the 218 payload
// bytes are consumed by the Poly1305 AEAD tag (cipher || tag layout on wire).
// Structs that may be encrypted MUST fit in this lower bound, not the raw
// MAX_PAYLOAD. static_asserts below enforce this for the encrypt-eligible
// payload types; new structs in the 30–49 (app unicast) / 60–69 (files) /
// 80–89 (sensors) ranges should pick this constant for their size cap.
#define ESPNOW_V4_AEAD_TAG_LEN 16
#define ESPNOW_V4_MAX_PLAINTEXT (ESPNOW_V4_MAX_PAYLOAD - ESPNOW_V4_AEAD_TAG_LEN)  // 202 bytes

// ---- Opcode enum -----------------------------------------------------------
// Category-based numbering with reserved slots for future phases.
// Ranges:
//   1–9    Transport     (ACK now; NACK/FRAG_REQ/FRAG_REPLY reserved)
//   10–19  Crypto        (Phase 3: KEY_EX_*, SESSION_*)
//   20–29  Discovery     (HEARTBEAT, TOPO_*, TIME_SYNC)
//   30–49  App unicast   (CMD, CMD_RESP, TEXT, METADATA_*, USER_SYNC)
//   50–59  Streaming     (STREAM, STREAM_CTRL)
//   60–69  Files         (FILE_START/CHUNK/END now; ACK/PROGRESS/CANCEL Phase 4)
//   70–79  Events        (Phase 5: SUBSCRIBE/UNSUBSCRIBE/EVENT/SUB_LIST_*)
//   80–89  Sensors       (SENSOR_BROADCAST/DATA/STATUS, WORKER_STATUS)
//   90–99  Bond          (BOND_HEARTBEAT, BOND_CAP_*, MANIFEST_*, SETTINGS_*, BOND_STATUS_*)
//   200+   User-defined  (reserved for future plugin-style extensions)
//
// Dead V3 opcodes removed: MANIFEST_RESP, SETTINGS_RESP, SETTINGS_PUSH
// (none were ever sent or received in V3 — manifests/settings travel as
//  files via FILE_END for specially-named payloads).

enum EspNowV4Type : uint8_t {
  // --- Transport (1–9) ---
  ESPNOW_V4_TYPE_ACK             = 1,
  // 2–4 reserved (NACK, FRAG_REQ, FRAG_REPLY for future negotiated retransmit)

  // --- Crypto / pairing (10–19) — Phase 3 ---
  ESPNOW_V4_TYPE_KEY_EX_HELLO    = 10,  // Phase 3 reserved
  ESPNOW_V4_TYPE_KEY_EX_REPLY    = 11,
  ESPNOW_V4_TYPE_KEY_EX_CONFIRM  = 12,
  ESPNOW_V4_TYPE_SESSION_OPEN    = 13,
  ESPNOW_V4_TYPE_SESSION_CONFIRM = 14,
  ESPNOW_V4_TYPE_SESSION_CLOSE   = 15,
  ESPNOW_V4_TYPE_SESSION_REKEY   = 16,

  // --- Discovery / timing (20–29) ---
  ESPNOW_V4_TYPE_HEARTBEAT       = 20,
  ESPNOW_V4_TYPE_TOPO_REQ        = 22,
  ESPNOW_V4_TYPE_TOPO_START      = 23,
  ESPNOW_V4_TYPE_TOPO_PEER       = 24,
  ESPNOW_V4_TYPE_TIME_SYNC       = 25,

  // --- Application unicast (30–49) ---
  ESPNOW_V4_TYPE_CMD             = 30,
  ESPNOW_V4_TYPE_CMD_RESP        = 31,  // (renamed CMD_RESP_STATUS in Phase 1 streaming refactor)
  ESPNOW_V4_TYPE_TEXT            = 32,
  ESPNOW_V4_TYPE_METADATA_REQ    = 33,
  ESPNOW_V4_TYPE_METADATA_RESP   = 34,
  ESPNOW_V4_TYPE_METADATA_PUSH   = 35,
  ESPNOW_V4_TYPE_USER_SYNC       = 36,

  // --- Streaming (50–59) ---
  ESPNOW_V4_TYPE_STREAM          = 50,
  ESPNOW_V4_TYPE_STREAM_CTRL     = 51,

  // --- Files (60–69) ---
  ESPNOW_V4_TYPE_FILE_START      = 60,
  ESPNOW_V4_TYPE_FILE_DATA       = 61,
  ESPNOW_V4_TYPE_FILE_END        = 62,
  // 63–65 reserved (FILE_ACK, FILE_PROGRESS, FILE_CANCEL — Phase 4)

  // --- Events (70–79) — Phase 5 reserved ---
  // 70=SUBSCRIBE, 71=UNSUBSCRIBE, 72=EVENT, 73=SUB_LIST_REQ, 74=SUB_LIST_REPLY

  // --- Sensors (80–89) ---
  ESPNOW_V4_TYPE_SENSOR_BROADCAST= 80,
  ESPNOW_V4_TYPE_SENSOR_DATA     = 81,  // Binary sensor data (bond mode streaming)
  ESPNOW_V4_TYPE_SENSOR_STATUS   = 82,
  ESPNOW_V4_TYPE_WORKER_STATUS   = 83,  // Detailed worker status report to master

  // --- Bond (90–99) ---
  ESPNOW_V4_TYPE_BOND_HEARTBEAT  = 90,
  ESPNOW_V4_TYPE_BOND_CAP_REQ    = 91,
  ESPNOW_V4_TYPE_BOND_CAP_RESP   = 92,
  ESPNOW_V4_TYPE_MANIFEST_REQ    = 93,
  ESPNOW_V4_TYPE_SETTINGS_REQ    = 94,
  ESPNOW_V4_TYPE_BOND_STATUS_REQ = 95,
  ESPNOW_V4_TYPE_BOND_STATUS_RESP= 96,
};

// ---- Flag bits (16-bit in V4; was 8-bit in V3) -----------------------------

enum EspNowV4Flags : uint16_t {
  ESPNOW_V4_FLAG_ACK_REQ        = 0x0001,  // Request ACK from receiver
  ESPNOW_V4_FLAG_ENCRYPTED      = 0x0002,  // Payload is encrypted (radio-layer LMK in Phase 1; AEAD in Phase 3)
  ESPNOW_V4_FLAG_COMPRESS       = 0x0004,  // Payload is compressed (future)
  ESPNOW_V4_FLAG_STREAM_BEGIN   = 0x0010,  // First chunk of stream
  ESPNOW_V4_FLAG_STREAM_END     = 0x0020,  // Last chunk of stream
  // Phase 3+ reserved:
  ESPNOW_V4_FLAG_BROADCAST_AUTH = 0x0100,  // Phase 3: HMAC'd with mesh group key
  ESPNOW_V4_FLAG_SESSION_FRAME  = 0x0200,  // Phase 3: AEAD-wrapped with session key
  ESPNOW_V4_FLAG_HANDSHAKE      = 0x0400,  // Phase 3: key-exchange / session message
  ESPNOW_V4_FLAG_PRIORITY_HIGH  = 0x0800,  // Phase 5: bump above retry queue (events)
  // 0x1000–0x8000 reserved
};

// ---- Frame header (32 bytes) -----------------------------------------------
//
// Phase 1 lands the structure; the new fields (meshFingerprint, sessionId,
// frameSeq) are populated by zero and not consulted yet. Phase 2 fills in
// meshFingerprint from gSettings; Phase 3 wires up sessionId + frameSeq for
// per-pair AEAD with replay protection.

struct __attribute__((packed)) EspNowV4Header {
  uint16_t magic;            // 0   — 0x3148 ('H1' little-endian)
  uint8_t  ver;              // 2   — ESPNOW_V4_VERSION (4)
  uint8_t  type;             // 3   — EspNowV4Type
  uint16_t flags;            // 4–5 — EspNowV4Flags (16-bit)
  uint8_t  headerLen;        // 6   — ESPNOW_V4_HEADER_LEN (32)
  uint8_t  reserved1;        // 7   — alignment padding, must be 0
  uint32_t msgId;            // 8–11
  uint8_t  origin[6];        // 12–17 — original sender MAC (for mesh forwarding)
  uint8_t  ttl;              // 18  — time-to-live (hops remaining)
  uint8_t  fragIndex;        // 19  — fragment index (0-based)
  uint8_t  fragCount;        // 20  — total fragment count (1 = not fragmented)
  uint8_t  reserved2;        // 21  — alignment padding, must be 0
  uint16_t meshFingerprint;  // 22–23 — Phase 2 (zero in Phase 1)
  uint16_t sessionId;        // 24–25 — Phase 3 (zero in Phase 1)
  uint32_t frameSeq;         // 26–29 — Phase 3 (zero in Phase 1)
  uint16_t crc16;            // 30–31 — CRC16-CCITT of payload
  // Note: V3 had a `payloadLen` field. In V4 it's derived from the ESPNOW
  // recv_info's `data_len` minus headerLen — saves 2 bytes on the wire.
};
static_assert(sizeof(EspNowV4Header) == 32, "EspNowV4Header must be 32 bytes");

// ---- Payload structs -------------------------------------------------------

struct __attribute__((packed)) V4PayloadHeartbeat {
  uint8_t  role;
  uint8_t  peerCount;
  int8_t   rssi;
  uint8_t  reserved;
  uint32_t uptimeSec;
  uint32_t freeHeap;
  char     deviceName[20];
};
static_assert(sizeof(V4PayloadHeartbeat) == 32, "V4PayloadHeartbeat must be 32 bytes");

// Time sync payload
struct __attribute__((packed)) V4PayloadTimeSync {
  uint32_t epochTime;     // Unix epoch time
  int64_t  timeOffset;    // Time offset in milliseconds
  uint32_t senderUptime;  // Sender uptime in seconds
};

// Topology request payload
struct __attribute__((packed)) V4PayloadTopoReq {
  uint32_t reqId;       // Request ID for correlation
  uint8_t  reserved[4]; // Padding for alignment
};

// Topology start payload (first message in topology response)
struct __attribute__((packed)) V4PayloadTopoStart {
  uint32_t reqId;       // Matches request ID
  uint8_t  peerCount;   // Number of TOPO_PEER messages to follow
  uint8_t  reserved[3];
};

// Topology peer entry (one per peer)
struct __attribute__((packed)) V4PayloadTopoPeer {
  uint32_t reqId;       // Matches request ID
  uint8_t  peerIndex;   // Which peer (0-based)
  uint8_t  isLast;      // 1 if this is the last peer
  uint8_t  mac[6];      // Peer MAC
  int8_t   rssi;        // Last RSSI
  uint8_t  encrypted;   // 1 if encrypted
  char     name[32];    // Peer name
};

// Worker status payload (detailed, for master consumption)
struct __attribute__((packed)) V4PayloadWorkerStatus {
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
struct __attribute__((packed)) V4PayloadBondHeartbeat {
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
// V4: header is 8 bytes, max V4 payload 218 bytes, leaving 210 bytes for sensor data.
struct __attribute__((packed)) V4PayloadSensorData {
  uint8_t  sensorType;     // RemoteSensorType enum value
  uint8_t  flags;          // Bit 0: valid, Bit 1: streaming enabled
  uint16_t dataLen;        // Length of data[] that follows
  uint32_t seqNum;         // Sequence number for ordering
  uint8_t  data[];         // Variable-length sensor data (flexible array member)
};

// Stream control payload for bond mode (master -> worker)
struct __attribute__((packed)) V4PayloadStreamCtrl {
  uint8_t sensorType;   // RemoteSensorType enum value
  uint8_t enable;       // 1 = start streaming, 0 = stop streaming
  uint8_t reserved[2];  // Padding
};
#endif // ENABLE_BONDED_MODE

// Sensor status payload for mesh broadcast
struct __attribute__((packed)) V4PayloadSensorStatus {
  uint8_t sensorType;   // RemoteSensorType enum value
  uint8_t enabled;      // 1 if enabled, 0 if disabled
  uint8_t reserved[2];  // Padding for alignment
};

// Sensor broadcast payload (sensor data to mesh)
// V4: header is 4 bytes, max V4 payload 218 bytes, leaving 214 bytes for JSON data.
struct __attribute__((packed)) V4PayloadSensorBroadcast {
  uint8_t  sensorType;  // RemoteSensorType enum value
  uint16_t dataLen;     // Length of JSON data that follows
  uint8_t  reserved;    // Padding for alignment
  uint8_t  data[];      // Variable-length JSON data (flexible array member)
};

// Metadata payload for metadata exchange (REQ/RESP/PUSH)
// Phase 3.5: trimmed tags 64→54 so the struct fits ESPNOW_V4_MAX_PLAINTEXT
// (202 B) for future SESSION_FRAME wrapping. Total now exactly 202.
struct __attribute__((packed)) V4PayloadMetadata {
  char    deviceName[32];
  char    friendlyName[48];
  char    room[32];
  char    zone[32];
  char    tags[54];       // was 64 — trimmed to fit encrypted-frame budget
  uint8_t stationary;
  uint8_t reserved[3];    // Padding for future fields
};
static_assert(sizeof(V4PayloadMetadata) <= ESPNOW_V4_MAX_PLAINTEXT,
              "V4PayloadMetadata must fit a SESSION_FRAME plaintext budget "
              "(MAX_PAYLOAD - AEAD_TAG = 202 B). If you need more space, "
              "either trim a field, fragment, or wait for encrypted "
              "fragmentation (Phase 3.5 task #51).");

// Command response payload — V4 keeps the V3 semantics for Phase 1
// (single buffer, truncated). Phase 1.5 / Phase 2 may refactor to streaming
// for unlimited output; for now, the cap is whatever ESPNOW_V4_MAX_PLAINTEXT
// allows minus the success byte (so the response fits even when wrapped in
// SESSION_FRAME — single-frame encrypted CMD_RESP). Larger responses use the
// fragmented path (v4_send_chunked); see Phase 3.5 task #51 for tag-aware
// fragmentation that'll let encrypted fragmented CMD_RESP work.
struct __attribute__((packed)) V4PayloadCmdResp {
  uint8_t success;                            // 1=success, 0=failure
  char    result[ESPNOW_V4_MAX_PLAINTEXT - 1]; // Null-terminated, 201 bytes — fits a SESSION_FRAME
};
static_assert(sizeof(V4PayloadCmdResp) <= ESPNOW_V4_MAX_PLAINTEXT,
              "V4PayloadCmdResp must fit a SESSION_FRAME plaintext budget");

// ---- Phase 3.3 — KEY_EX handshake payloads ---------------------------------
//
// Three-way handshake between two peers in the same mesh. Authenticates via
// HMAC-SHA256 over (meshFingerprint || senderMac || senderPubEd25519) keyed by
// the mesh bootstrap key (Blake2b subkey of the PBKDF2-stretched passphrase —
// see System_ESPNow_MeshKeys). Only peers that know the passphrase can produce
// a valid HMAC.
//
// On wire, each frame carries:
//   ESPNOW_V4_FLAG_HANDSHAKE  (so RX path knows it's plaintext-but-authenticated)
//   header.type               (KEY_EX_HELLO / KEY_EX_REPLY / KEY_EX_CONFIRM)
//   header.meshFingerprint    (RX dispatches mesh lookup off this)
//
// Flow:
//   A → B:  KEY_EX_HELLO   (A's pubkey + HMAC)
//   B → A:  KEY_EX_REPLY   (B's pubkey + HMAC) — only if A's HMAC verified
//   A → B:  KEY_EX_CONFIRM (status byte + pubkey fingerprint for OOB display)
//
// On success both sides persist each other's pubkey to
//   /system/espnow/peers/<MAC>/identity.json
// SESSION establishment (X25519 ephemeral DH, AEAD keys) happens in Phase 3.4
// — KEY_EX in 3.3 only proves "this peer knows the mesh passphrase + owns
// this Ed25519 pubkey".

struct __attribute__((packed)) V4PayloadKeyExHello {
  uint16_t meshFingerprint;     // also redundant with header.meshFingerprint
  uint8_t  senderMac[6];        // redundant with header.origin (HMAC input clarity)
  uint8_t  senderPubEd25519[32];
  uint8_t  hmac[32];            // HMAC-SHA256 over (meshFingerprint || senderMac || pub)
                                //   keyed by mesh bootstrap key
};
static_assert(sizeof(V4PayloadKeyExHello) == 72, "V4PayloadKeyExHello layout");

struct __attribute__((packed)) V4PayloadKeyExReply {
  uint16_t meshFingerprint;
  uint8_t  responderMac[6];
  uint8_t  responderPubEd25519[32];
  uint8_t  hmac[32];            // same construction as HELLO, with responder fields
};
static_assert(sizeof(V4PayloadKeyExReply) == 72, "V4PayloadKeyExReply layout");

struct __attribute__((packed)) V4PayloadKeyExConfirm {
  uint16_t meshFingerprint;
  uint8_t  confirmerMac[6];
  uint8_t  status;              // 0 = paired OK, 1 = HMAC fail, 2 = already-paired-different-key
  uint8_t  _pad;
  uint8_t  pubFingerprint[8];   // first 8 bytes of own pub key (for OOB display verification)
};
static_assert(sizeof(V4PayloadKeyExConfirm) == 18, "V4PayloadKeyExConfirm layout");

// ---- Phase 3.4 — SESSION establishment payloads -----------------------------
//
// Signed Ephemeral Diffie-Hellman (SIGMA-I pattern). Both sides have already
// exchanged long-term Ed25519 pubkeys via KEY_EX (3.3) and have those keys
// persisted at /system/espnow/peers/<MAC>/identity.json. SESSION_OPEN/CONFIRM
// negotiates fresh ephemeral X25519 keys, signs them under the long-term
// identity, and derives forward-secret AEAD session keys.
//
// Transcript signed:
//   OPEN:    "v4-sopen:" || sessionId(2) || initMac(6) || respMac(6) ||
//                          ephX25519Pub(32) || nonceA(16)
//   CONFIRM: "v4-sconf:" || sessionId(2) || respMac(6) || initMac(6) ||
//                          ephX25519Pub(32) || nonceA(16) || nonceB(16)
//
// nonceA is sent in OPEN and echoed back inside the CONFIRM signature input
// — that's the freshness binding from A to B. nonceB is added on the B side.
// Total signed length: OPEN = 9 + 2 + 6 + 6 + 32 + 16 = 71 bytes; CONFIRM = 88.
//
// Flow:
//   A → B: SESSION_OPEN    (sessionId, ephPub_A, nonceA, sig over OPEN transcript)
//   B → A: SESSION_CONFIRM (sessionId, ephPub_B, nonceB, sig over CONFIRM transcript)
// On success both sides hold matching aeadKeyAtoB / aeadKeyBtoA derived via
// Blake2b KDF (contexts "esp-AtoB" / "esp-BtoA") from the X25519 shared secret.

struct __attribute__((packed)) V4PayloadSessionOpen {
  uint16_t sessionId;            // initiator-chosen, 16-bit random non-zero
  uint8_t  initiatorMac[6];
  uint8_t  responderMac[6];
  uint8_t  ephX25519Pub[32];
  uint8_t  nonceA[16];
  uint8_t  signature[64];        // Ed25519 over "v4-sopen:"||sessionId||initMac||respMac||eph||nonceA
};
static_assert(sizeof(V4PayloadSessionOpen) == 126, "V4PayloadSessionOpen layout");

struct __attribute__((packed)) V4PayloadSessionConfirm {
  uint16_t sessionId;            // mirrors initiator's choice
  uint8_t  responderMac[6];
  uint8_t  initiatorMac[6];
  uint8_t  ephX25519Pub[32];
  uint8_t  nonceA[16];           // echoed from OPEN — freshness binding
  uint8_t  nonceB[16];
  uint8_t  signature[64];        // Ed25519 over "v4-sconf:"||sessionId||respMac||initMac||eph||nonceA||nonceB
};
static_assert(sizeof(V4PayloadSessionConfirm) == 142, "V4PayloadSessionConfirm layout");

// File transfer payloads
struct __attribute__((packed)) V4PayloadFileStart {
  uint32_t fileSize;      // Total file size in bytes
  uint16_t chunkCount;    // Total number of chunks
  uint16_t chunkSize;     // Size of each chunk (except last)
  char     filename[64];  // Destination filename
};

// Phase 3.5: chunk data sized for ESPNOW_V4_MAX_PLAINTEXT so encrypted
// file transfers (when task #6 flips FILE_DATA to encrypted) don't silently
// truncate every chunk by 16 bytes. Per-chunk payload drops 216→200; total
// file transfer takes ⌈N/200⌉ chunks instead of ⌈N/216⌉ — small bandwidth
// hit. Plaintext file transfers also use the new smaller size for a single
// code path; old firmware reading the V4PayloadFileStart.chunkSize field
// adapts automatically.
struct __attribute__((packed)) V4PayloadFileData {
  uint16_t chunkIndex;    // Chunk index (0-based)
  uint8_t  data[ESPNOW_V4_MAX_PLAINTEXT - 2];  // 200 bytes — fits a SESSION_FRAME
};
static_assert(sizeof(V4PayloadFileData) <= ESPNOW_V4_MAX_PLAINTEXT,
              "V4PayloadFileData must fit a SESSION_FRAME plaintext budget");

struct __attribute__((packed)) V4PayloadFileEnd {
  uint32_t crc32;         // CRC32 of entire file
  uint8_t  success;       // 1=transfer complete, 0=aborted
};

#endif // ENABLE_ESPNOW

#endif // SYSTEM_ESPNOW_WIRE_H
