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
// payload types; new structs in the app-unicast (50–69), remote-FS (70–89),
// files (110–129), sensors (150–169), and bond (170–189) ranges should pick
// this constant for their size cap.
#define ESPNOW_V4_AEAD_TAG_LEN 16
#define ESPNOW_V4_MAX_PLAINTEXT (ESPNOW_V4_MAX_PAYLOAD - ESPNOW_V4_AEAD_TAG_LEN)  // 202 bytes

// Phase 3.5 task #32 — BROADCAST_AUTH tag is HMAC-SHA256 (32 bytes) appended
// to the broadcast payload. Receivers verify against the mesh group key (the
// "esp-grup" subkey from Phase 3.1's mesh-key derivation). Auth-only — the
// payload stays plaintext; only forge-resistance, not confidentiality.
#define ESPNOW_V4_BROADCAST_AUTH_TAG_LEN 32
#define ESPNOW_V4_MAX_BROADCAST_AUTHED_PLAINTEXT \
            (ESPNOW_V4_MAX_PAYLOAD - ESPNOW_V4_BROADCAST_AUTH_TAG_LEN)  // 186 bytes

// ---- Opcode enum -----------------------------------------------------------
// Category-based numbering, 20 slots per category. Renumbered 2026-07 from the
// original 10-wide V4 map to create growth room; the wire is incompatible with
// the old numbering and no migration path is carried — the whole fleet is
// erased and reflashed together (version byte deliberately unchanged).
//
// Ranges:
//   0        invalid — never assign
//   1–9      Transport     (ACK live, RELAY_DATA=5 live; NACK/FRAG_REQ/FRAG_REPLY reserved 2–4)
//   10–29    Crypto        (KEY_EX_* 10–12, SESSION_* 13–15;
//                           16/17 earmarked SESSION_AUTH_REQ/GRANT)
//   30–49    Discovery     (HEARTBEAT, BOOT, TOPO_*, TIME_SYNC, PAIR_BEACON,
//                           PEER_LIST=39 live; 37/38 earmarked CAP_REQ/CAP_RESP)
//   50–69    App unicast   (CMD, CMD_RESP, TEXT, METADATA_*, USER_SYNC)
//   70–89    Remote FS     (FS_* read side live 70–75; 76–81 earmarked write
//                           side: DELETE_REQ/ACK, PUT_REQ/ACK, MKDIR, RENAME)
//   90–109   Streaming     (STREAM; 91–93 earmarked MEDIA_START/DATA/END)
//   110–129  Files         (FILE_START/DATA/END/CANCEL live; FILE_ACK=113,
//                           FILE_PROGRESS=114, FILE_NACK=116, FILE_RESUME_OK=117 reserved)
//   130–149  Events        (SUBSCRIBE_UPDATE live; UNSUBSCRIBE_ALL/EVENT_PUSH/
//                           SUB_LIST_REQ/SUB_LIST_REPLY reserved 131–134)
//   150–169  Sensors       (SENSOR_STATUS live; SENSOR_REQ=153/SENSOR_ENVELOPE=154 live —
//                           the secure fetcher's leased pull + encrypted reply;
//                           SENSOR_BROADCAST=150 TX retired (D2: plaintext path removed,
//                           encrypted ENVELOPE replaces it), RX kept vestigial;
//                           POWER_STATUS=152 reserved)
//   170–189  Bond          (BOND_*, MANIFEST_REQ, SETTINGS_REQ, SCHEMA_REQ, plus
//                           BOND_STREAM_CTRL/BOND_SENSOR_DATA — moved in from
//                           streaming/sensors because they were always bond-gated)
//   190–199  Unallocated buffer (future whole category, or spill for a full one)
//   200–255  User-defined  (plugin/experiment space — core NEVER allocates here)
//
// Allocation policy (this comment block is the source of truth for the map —
// update it in the same commit as any enum change):
//   * New opcodes take the lowest free slot in their category; a category that
//     fills spills into 190–199 before ever borrowing from a neighbor.
//   * An enum symbol is added only in the same change as its kV4HandlerTable
//     row and handler — a TX'd opcode with no RX row is a silent drop.
//   * Earmarks (named numbers in comments) are intentions, not allocations;
//     re-justify or release them when the category next allocates.
//
// Dead V3 opcodes removed: MANIFEST_RESP, SETTINGS_RESP, SETTINGS_PUSH
// (none were ever sent or received in V3 — manifests/settings travel as
//  files via FILE_END for specially-named payloads).

enum EspNowV4Type : uint8_t {
  // --- Transport (1–9) ---
  ESPNOW_V4_TYPE_ACK             = 1,
  // 2–4 reserved (NACK, FRAG_REQ, FRAG_REPLY for future negotiated retransmit)
  ESPNOW_V4_TYPE_RELAY_DATA      = 5,  // Routed-unicast envelope: BROADCAST_AUTH outer frame carrying
                                       //   {finalDst, hopsUsed} + a BYTE-IDENTICAL inner V4 frame.
                                       //   Relays are cryptographically blind to the inner frame
                                       //   (they hold the group key, not the endpoints' session keys).

  // --- Crypto / pairing (10–29) ---
  ESPNOW_V4_TYPE_KEY_EX_HELLO    = 10,
  ESPNOW_V4_TYPE_KEY_EX_REPLY    = 11,
  ESPNOW_V4_TYPE_KEY_EX_CONFIRM  = 12,
  ESPNOW_V4_TYPE_SESSION_OPEN    = 13,
  ESPNOW_V4_TYPE_SESSION_CONFIRM = 14,
  ESPNOW_V4_TYPE_SESSION_REKEY   = 15,
  // 16/17 earmarked SESSION_AUTH_REQ / SESSION_AUTH_GRANT (session-scoped
  // credential-free remote exec — mesh report §5.2 rank 11)

  // --- Discovery / timing (30–49) ---
  ESPNOW_V4_TYPE_HEARTBEAT       = 30,
  ESPNOW_V4_TYPE_BOOT            = 31,  // Device boot/online notice — its OWN type so the RX files it as a system event, not chat
  ESPNOW_V4_TYPE_TOPO_REQ        = 32,
  ESPNOW_V4_TYPE_TOPO_START      = 33,
  ESPNOW_V4_TYPE_TOPO_PEER       = 34,
  ESPNOW_V4_TYPE_TIME_SYNC       = 35,
  ESPNOW_V4_TYPE_PAIR_BEACON     = 36,  // Discovery beacon — BROADCAST_AUTH FF broadcast sent while a discovery window is open
  // 37/38 earmarked CAP_REQ / CAP_RESP (mesh capability discovery)
  ESPNOW_V4_TYPE_PEER_LIST       = 39,  // Distance-vector route advertisement. BROADCAST_AUTH, ttl=1
                                        //   ALWAYS (never relayed — it describes the sender's own view,
                                        //   so forwarding it would attribute someone else's routes to us).
  // Discover-then-confirm pairing control (BROADCAST_AUTH FF broadcast + V4PayloadPairCtrl.targetMac).
  ESPNOW_V4_TYPE_PAIR_REQUEST    = 40,  // initiator → target: "I want to pair" (payload carries requester name)
  ESPNOW_V4_TYPE_PAIR_ACCEPT     = 41,  // target → initiator: user accepted; both sides now run pairsecure
  ESPNOW_V4_TYPE_PAIR_REJECT     = 42,  // target → initiator: user rejected / not in discovery mode

  // --- Application unicast (50–69) ---
  ESPNOW_V4_TYPE_CMD             = 50,
  ESPNOW_V4_TYPE_CMD_RESP        = 51,
  ESPNOW_V4_TYPE_TEXT            = 52,
  ESPNOW_V4_TYPE_METADATA_REQ    = 53,
  ESPNOW_V4_TYPE_METADATA_RESP   = 54,
  ESPNOW_V4_TYPE_METADATA_PUSH   = 55,  // RX-plumbed but never TX'd today (no sendMetadata(isPush=true) caller)
  ESPNOW_V4_TYPE_USER_SYNC       = 56,

  // --- Remote filesystem (70–89) ---
  ESPNOW_V4_TYPE_FS_LIST_REQ     = 70,  // Browse remote VFS: "list directory <path>"
  ESPNOW_V4_TYPE_FS_LIST_REPLY   = 71,  // Reply: paginated batch of FileEntry-style records
  ESPNOW_V4_TYPE_FS_STAT_REQ     = 72,  // Storage stats for a VFS root (total/used/free)
  ESPNOW_V4_TYPE_FS_STAT_REPLY   = 73,  // Reply: 64-bit byte counters + percent used
  ESPNOW_V4_TYPE_FS_GET_REQ      = 74,  // Pull a file from the peer (peer responds via FILE_* opcodes)
  ESPNOW_V4_TYPE_FS_GET_ACK      = 75,  // Synchronous ack for FS_GET_REQ (status + fileSize)
  // 76–81 earmarked write side: FS_DELETE_REQ/ACK, FS_PUT_REQ/ACK, FS_MKDIR,
  // FS_RENAME (V4PayloadFsEntry.perms already carries WRITE/DELETE bits)

  // --- Streaming (90–109) ---
  ESPNOW_V4_TYPE_STREAM          = 90,
  // 91–93 earmarked MEDIA_START / MEDIA_DATA / MEDIA_END (camera/mic frames)

  // --- Files (110–129) ---
  ESPNOW_V4_TYPE_FILE_START      = 110,
  ESPNOW_V4_TYPE_FILE_DATA       = 111,
  ESPNOW_V4_TYPE_FILE_END        = 112,
  // 113=FILE_ACK, 114=FILE_PROGRESS reserved (transfer-feedback follow-ups)
  ESPNOW_V4_TYPE_FILE_CANCEL     = 115,  // receiver→sender: transfer won't complete (post-hoc notice)
  // 116=FILE_NACK (missing-chunk bitmap), 117=FILE_RESUME_OK reserved (resume work)

  // --- Events (130–149) ---
  ESPNOW_V4_TYPE_SUBSCRIBE_UPDATE = 130,  // sender → receiver: "send me only these event categories"
  // 131=UNSUBSCRIBE_ALL, 132=EVENT_PUSH, 133=SUB_LIST_REQ, 134=SUB_LIST_REPLY reserved
  // 135 earmarked EVENT_SUB v2 (semantic-kind mask wider than 32 bits)

  // --- Sensors (150–169) ---
  ESPNOW_V4_TYPE_SENSOR_BROADCAST= 150,  // TX retired (D2 secure fetcher): plaintext broadcast
                                         //   replaced by encrypted SENSOR_ENVELOPE. RX kept vestigial.
  ESPNOW_V4_TYPE_SENSOR_STATUS   = 151,
  // 152=POWER_STATUS reserved (voltage/charging/USB/heap beacon — mesh report rank 8)
  ESPNOW_V4_TYPE_SENSOR_REQ      = 153,  // master → worker: subscribe/oneshot/unsubscribe (leased pull)
  ESPNOW_V4_TYPE_SENSOR_ENVELOPE = 154,  // worker → master: session-encrypted unicast sensor reading

  // --- Bond (170–189) ---
  ESPNOW_V4_TYPE_BOND_HEARTBEAT  = 170,
  ESPNOW_V4_TYPE_BOND_CAP_REQ    = 171,
  ESPNOW_V4_TYPE_BOND_CAP_RESP   = 172,
  ESPNOW_V4_TYPE_MANIFEST_REQ    = 173,
  ESPNOW_V4_TYPE_SETTINGS_REQ    = 174,
  ESPNOW_V4_TYPE_BOND_STATUS_REQ = 175,
  ESPNOW_V4_TYPE_BOND_STATUS_RESP= 176,
  ESPNOW_V4_TYPE_SCHEMA_REQ      = 177,  // Master → worker: send your settings schema (response arrives as file _schema_out.json)
  ESPNOW_V4_TYPE_BOND_STREAM_CTRL= 178,  // was STREAM_CTRL (51) — always bond-gated, now numbered and named with its policy family
  ESPNOW_V4_TYPE_BOND_SENSOR_DATA= 179,  // was SENSOR_DATA (81) — bond binary sensor streaming
  // 180–182 earmarked BOND_POWER_STATUS / BOND_SETTINGS_SYNC_ACK / BOND_EVENT_PUSH
};

// ---- Flag bits (16-bit in V4; was 8-bit in V3) -----------------------------

enum EspNowV4Flags : uint16_t {
  ESPNOW_V4_FLAG_ACK_REQ        = 0x0001,  // Request ACK from receiver
  ESPNOW_V4_FLAG_ENCRYPTED      = 0x0002,  // DEPRECATED/VESTIGIAL — legacy LMK-era bit, never set since
                                           // the Phase 3.5 LMK rip (task #47). AEAD confidentiality is
                                           // signalled by ESPNOW_V4_FLAG_SESSION_FRAME on the wire and by
                                           // V4RxCtx::isSessionEncrypted on RX. DO NOT gate on this bit
                                           // (doing so silently broke USER_SYNC — it always read 0).
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
  uint8_t  origin[6];        // 12–17 — original sender MAC. NOT the radio sender once a frame has
                             //         been relayed: every forwarder preserves origin verbatim so
                             //         dedup, attribution and loop suppression stay end-to-end.
  uint8_t  ttl;              // 18  — hops remaining. Decremented (and the BROADCAST_AUTH tag
                             //         recomputed) by each forwarder; a frame arriving with ttl<=1
                             //         is delivered locally but never forwarded again. ttl=1 at TX
                             //         therefore means "single hop, never relayed" — that is the
                             //         correct value for anything link-local (HEARTBEAT, PEER_LIST,
                             //         ACK, PAIR_*). Relay-eligible today: BROADCAST_AUTH TEXT and
                             //         TIME_SYNC (flooded), and RELAY_DATA (routed). See
                             //         docs/ESPNOW_MESH_SYSTEM.md.
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

// ---- Multi-hop: routed unicast envelope (ESPNOW_V4_TYPE_RELAY_DATA) --------
//
// Wire layout of a routed frame, sent to the NEXT HOP (a direct ESP-NOW peer):
//
//   [ outer EspNowV4Header  32 B ]  type=RELAY_DATA, flags=BROADCAST_AUTH,
//                                   origin = the forwarder that emitted THIS hop,
//                                   ttl    = remaining hop budget,
//                                   msgId  = the inner frame's msgId (so the outer
//                                            frame dedups per end-to-end message)
//   [ V4PayloadRelayData     8 B ]  finalDst + hopsUsed
//   [ inner V4 frame     ≤ 178 B ]  BYTE-IDENTICAL — header and payload exactly as
//                                   the originator built them, including its own
//                                   origin, sessionId, frameSeq, crc16 and AEAD tag
//   [ outer HMAC tag        32 B ]  BROADCAST_AUTH over (outer hdr[0..29] || the two
//                                   blocks above), keyed by the mesh group key
//
// Why an envelope instead of new header fields: ttl and origin live INSIDE both
// integrity envelopes (the BROADCAST_AUTH HMAC and the SESSION_FRAME AEAD both
// cover header bytes 0..29). A relay holds the symmetric group key, so it may
// legally rewrite and re-tag the OUTER header; it can never touch the inner one,
// whose keys are pairwise. That is the whole security story of this design: the
// relay is cryptographically blind to what it carries, and the two endpoints run
// their normal KEY_EX / SESSION handshake straight through it.
//
// hopsUsed is advisory (diagnostics + a hard depth stop); ttl is the enforcement.
struct __attribute__((packed)) V4PayloadRelayData {
  uint8_t finalDst[6];   // ultimate destination MAC — the only node that unwraps
  uint8_t hopsUsed;      // forwards taken so far; 0 as emitted by the originator
  uint8_t reserved;      // pad, must be 0
};
static_assert(sizeof(V4PayloadRelayData) == 8, "V4PayloadRelayData must be 8 bytes");

// Byte budget for what an envelope can carry. 250 (radio MTU) minus the outer
// header, the envelope, and the outer HMAC tag.
#define ESPNOW_V4_RELAY_MAX_INNER_FRAME \
            (250 - ESPNOW_V4_HEADER_LEN - 8 - ESPNOW_V4_BROADCAST_AUTH_TAG_LEN)   // 178
#define ESPNOW_V4_RELAY_MAX_INNER_PAYLOAD \
            (ESPNOW_V4_RELAY_MAX_INNER_FRAME - ESPNOW_V4_HEADER_LEN)              // 146
// …and what fits once the inner frame is itself AEAD-sealed end-to-end.
#define ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT \
            (ESPNOW_V4_RELAY_MAX_INNER_PAYLOAD - ESPNOW_V4_AEAD_TAG_LEN)          // 130
static_assert(ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT == 130,
              "routed-unicast plaintext budget changed — re-check the handshake "
              "payloads below, which are sized to travel over a relay");

// ---- Multi-hop: distance-vector advertisement (ESPNOW_V4_TYPE_PEER_LIST) ---
//
// Each node periodically broadcasts its route table (BROADCAST_AUTH, ttl=1) so
// neighbours can learn paths beyond their own radio range. Classic split-horizon
// distance-vector: a route is never advertised back toward its own next hop, and
// `hops` is the ADVERTISER's cost, so a listener installs (hops + 1).
//
// `metric` is the worst measured link RSSI along the advertised path — the
// bottleneck link. Listeners compare min(metric, their own link to the
// advertiser) so a route is only as good as its weakest hop.
struct __attribute__((packed)) V4PayloadPeerListHeader {
  uint8_t entryCount;   // number of V4PayloadPeerListEntry records that follow
  uint8_t role;         // sender MESH_ROLE_* (informational)
  uint8_t seq;          // advertisement generation, wraps — diagnostics only
  uint8_t reserved;     // pad, must be 0
};
static_assert(sizeof(V4PayloadPeerListHeader) == 4, "V4PayloadPeerListHeader must be 4 bytes");

struct __attribute__((packed)) V4PayloadPeerListEntry {
  uint8_t mac[6];       // a destination the advertiser can reach
  uint8_t hops;         // advertiser's hop count to it (1 = advertiser's direct neighbour)
  int8_t  metric;       // worst link RSSI (dBm) along the advertiser's path; -128 = unknown
};
static_assert(sizeof(V4PayloadPeerListEntry) == 8, "V4PayloadPeerListEntry must be 8 bytes");

// How many entries fit one BROADCAST_AUTH frame (186 B of authed plaintext).
#define ESPNOW_V4_PEER_LIST_MAX_ENTRIES \
            ((ESPNOW_V4_MAX_BROADCAST_AUTHED_PLAINTEXT - 4) / 8)                  // 22
static_assert(sizeof(V4PayloadPeerListHeader)
                + ESPNOW_V4_PEER_LIST_MAX_ENTRIES * sizeof(V4PayloadPeerListEntry)
              <= ESPNOW_V4_MAX_BROADCAST_AUTHED_PLAINTEXT,
              "a full PEER_LIST must fit one broadcast-authed frame");

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

// WPS-style pairing beacon (ESPNOW_V4_TYPE_PAIR_BEACON). Sent PLAINTEXT to the
// FF broadcast address (flags=0) every ~1.5s while a device's pairing window is
// open. Same-mesh scoping is enforced for free by the header meshFingerprint
// gate (RX drops beacons whose fingerprint matches no enabled local mesh), so
// no fingerprint is carried here. All fields are attacker-controllable — the
// receiver must sanitize deviceName and never trust it beyond a display label;
// real trust comes from the subsequent KEY_EX (mesh-passphrase HMAC).
struct __attribute__((packed)) V4PayloadPairBeacon {
  uint8_t  role;            // sender MESH_ROLE_* (informational)
  uint8_t  flags;           // reserved (0)
  uint8_t  reserved[2];     // pad, 0
  char     deviceName[20];  // sender's espnowDeviceName (not guaranteed NUL-terminated)
};
static_assert(sizeof(V4PayloadPairBeacon) == 24, "V4PayloadPairBeacon must be 24 bytes");

// Discover-then-confirm control frame (PAIR_REQUEST / PAIR_ACCEPT / PAIR_REJECT).
// Sent as a BROADCAST_AUTH FF broadcast — same proven path as the beacon — because
// the two devices are NOT yet ESP-NOW peers (a unicast esp_now_send would fail,
// and fingerprintForPeer() of an unknown MAC yields no mesh key so the HMAC would
// be skipped). `targetMac` addresses the intended recipient; every other same-mesh
// device that hears the broadcast ignores it. All fields attacker-controllable
// except targetMac's match against our own MAC (sanitize deviceName).
struct __attribute__((packed)) V4PayloadPairCtrl {
  uint8_t targetMac[6];     // intended recipient — receivers drop if != own MAC
  uint8_t role;             // sender MESH_ROLE_* (informational)
  uint8_t reserved;         // pad, 0
  char    deviceName[20];   // sender's espnowDeviceName (display label; not NUL-guaranteed)
};
static_assert(sizeof(V4PayloadPairCtrl) == 28, "V4PayloadPairCtrl must be 28 bytes");

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
// NOTE (D2 secure fetcher): SENSOR_ENVELOPE=154 reuses THIS layout on the wire — same
// per-sensor {sensorType,dataLen,data[]} shape — but is sent as a session-encrypted
// UNICAST to the leasing controller instead of a plaintext broadcast (opcode 150).
struct __attribute__((packed)) V4PayloadSensorBroadcast {
  uint8_t  sensorType;  // RemoteSensorType enum value
  uint16_t dataLen;     // Length of JSON data that follows
  uint8_t  reserved;    // Padding for alignment
  uint8_t  data[];      // Variable-length JSON data (flexible array member)
};

// Sensor request payload (master -> worker): the secure fetcher's leased pull.
// Mesh-legal (NOT bond-gated) — registered REQ_PAIRED|REQ_SESSION_ENC and authorized
// in-handler by Ed25519 fingerprint against the worker's configured master/backup.
struct __attribute__((packed)) V4PayloadSensorReq {
  uint8_t  reqId;       // app-level correlation / renewal generation
  uint8_t  mode;        // 0=subscribe, 1=unsubscribe, 2=oneshot (see SensorReqMode)
  uint32_t sensorMask;  // bitmask over RemoteSensorType (bit i = 1u<<i)
  uint32_t intervalMs;  // desired per-sensor cadence hint (worker floors at spec.minIntervalMs)
  uint32_t leaseMs;     // lease duration (ms); must exceed the renewal cadence (e.g. 90000 for a 30s tick)
};
static_assert(sizeof(V4PayloadSensorReq) <= ESPNOW_V4_MAX_PLAINTEXT,
              "V4PayloadSensorReq must fit a SESSION_FRAME plaintext budget (202 B)");

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
              "(MAX_PAYLOAD - AEAD_TAG = 202 B). For larger payloads, route "
              "through v4_send_payload_smart or v4_send_encrypted_chunked "
              "(Phase 3.5 task #51 — shipped).");

// Command response payload — V4 keeps the V3 semantics for Phase 1
// (single buffer, truncated). Phase 1.5 / Phase 2 may refactor to streaming
// for unlimited output; for now, the cap is whatever ESPNOW_V4_MAX_PLAINTEXT
// allows minus the success byte (so the response fits even when wrapped in
// SESSION_FRAME — single-frame encrypted CMD_RESP). Larger responses go
// through v4_send_payload_smart which prefers encrypted-chunked (Phase 3.5
// task #51, shipped) and falls back to plaintext fragmented if no session.
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
// Total signed length: OPEN = 9 + 2 + 6 + 6 + 32 + 16 = 71 bytes;
//                      CONFIRM = 9 + 2 + 6 + 6 + 32 + 16 + 16 = 87 bytes.
// Sign exactly those byte counts — a transcript padded to any other length
// verifies against nothing and every handshake fails.
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

// ---- Phase 3.6 — SESSION_REKEY (forward-secrecy rotation) -------------------
//
// Refreshes the AEAD keys without a full handshake. Used when txSeqNext
// crosses a threshold (e.g. 10k frames) or sessionAge crosses a duration
// (e.g. 1h). The long-term Ed25519 identities established in 3.0 are reused
// to sign fresh ephemeral X25519 pubkeys; the resulting shared secret feeds
// the Blake2b KDF to produce a new aeadKeyTx/Rx pair.
//
// Two-message symmetric exchange (each side sends one REKEY containing its
// own fresh ephemeral pub):
//   A → B: SESSION_REKEY (newEphA, nonceA, sig_A)
//   B → A: SESSION_REKEY (newEphB, nonceB, sig_B)
// Either side can initiate. On receiving a REKEY without having sent one,
// the receiver picks its own fresh ephemeral and replies with REKEY. On
// receiving a REKEY while own REKEY is in flight (concurrent rekey race),
// the same protocol still converges — both ends derive shared from the same
// (newEphA, newEphB) pair, so they end up with identical keys regardless of
// who "started" it.
//
// Transcript signed:
//   "v4-rekey:" || sessionId(2) || senderMac(6) || receiverMac(6) ||
//                  newEphX25519Pub(32) || nonceRekey(16) || prevTxSeqAtRekey(4)
// prevTxSeqAtRekey binds the signature to a specific point in the existing
// session's transcript — prevents replay of a stale REKEY from a previous
// rekey round.
//
// Old keys stay valid in SessionState.aeadKey{Tx,Rx}Prev for ~5 seconds
// after the switch so in-flight frames sent under the old keys still decrypt.

struct __attribute__((packed)) V4PayloadSessionRekey {
  uint16_t sessionId;            // existing session this rekey applies to
  uint8_t  senderMac[6];
  uint8_t  receiverMac[6];
  uint8_t  newEphX25519Pub[32];
  uint8_t  nonceRekey[16];
  uint32_t prevTxSeqAtRekey;     // sender's txSeqNext at the moment of REKEY signing
  uint8_t  signature[64];        // Ed25519 over the transcript above
};
static_assert(sizeof(V4PayloadSessionRekey) == 130, "V4PayloadSessionRekey layout");
static_assert(sizeof(V4PayloadSessionRekey) <= ESPNOW_V4_MAX_PLAINTEXT,
              "REKEY must fit a SESSION_FRAME budget; rekey messages are sent inside the existing session");

// ---- Phase 5 — event subscription registry (opcode 130) ------------------
//
// Sender tells receiver "these are the event categories I want from you".
// Receiver stores the bitmap in its PeerIdentity-for-sender slot and gates
// outbound broadcasts on it. Default (no SUBSCRIBE_UPDATE yet received) is
// "send everything", so this opcode only ever narrows traffic — pre-Phase-5
// peers stay fully functional.
//
// Sent as a SESSION_FRAME if the session is available (preferred — narrows
// who can spoof subscription changes), otherwise plaintext. Receiver must
// confirm sender has a known PeerIdentity before applying.
struct __attribute__((packed)) V4PayloadSubscribe {
  uint32_t requestedEvents;  // bitmask of EspNowEventCategory bits
  uint8_t  reserved[12];     // reserved for future flags / scope hints; must be 0
};
static_assert(sizeof(V4PayloadSubscribe) == 16, "V4PayloadSubscribe layout");
static_assert(sizeof(V4PayloadSubscribe) <= ESPNOW_V4_MAX_PLAINTEXT,
              "SUBSCRIBE_UPDATE must fit a SESSION_FRAME budget");

// File transfer payloads
struct __attribute__((packed)) V4PayloadFileStart {
  uint32_t fileSize;      // Total file size in bytes
  uint16_t chunkCount;    // Total number of chunks
  uint16_t chunkSize;     // Size of each chunk (except last)
  char     filename[64];  // Destination filename
};

// Phase 3.5: chunk data sized for ESPNOW_V4_MAX_PLAINTEXT so encrypted
// file transfers don't silently truncate every chunk by 16 bytes. Per-chunk
// data is 200 B (was 216 in plaintext-only V3), and the struct also carries a
// 2 B chunkIndex: 202 B plaintext + 16 B AEAD tag = 218 = MAX_PAYLOAD EXACTLY,
// zero headroom. There is no room for another field — adding one trips the
// static_assert below rather than corrupting the wire, so shrink data[] (or
// widen the transport) instead of growing this struct.
// 2026-05-19: FILE_START/DATA/END now route through
// v4_send_payload_smart, so each chunk is session-encrypted when a session
// exists and plaintext otherwise. Old firmware reading
// V4PayloadFileStart.chunkSize adapts automatically.
struct __attribute__((packed)) V4PayloadFileData {
  uint16_t chunkIndex;    // Chunk index (0-based)
  uint8_t  data[ESPNOW_V4_MAX_PLAINTEXT - 2];  // 200 bytes — fits a SESSION_FRAME
};
static_assert(sizeof(V4PayloadFileData) <= ESPNOW_V4_MAX_PLAINTEXT,
              "V4PayloadFileData must fit a SESSION_FRAME plaintext budget");

struct __attribute__((packed)) V4PayloadFileEnd {
  uint32_t crc32;         // CRC32 of the entire file, computed with esp_crc32_le
                          // (seed 0) on both ends. Receiver REJECTS on mismatch
                          // (FILE_CANCEL_CRC_MISMATCH). Ignored when success=0.
                          // Flag day 2026-07: older firmware sent 0 here; both
                          // peers must run post-CRC firmware or every non-empty
                          // transfer fails.
  uint8_t  success;       // 1=transfer complete, 0=sender aborted mid-transfer
};

// Phase 4: FILE_CANCEL — the receiver tells the sender a transfer it started
// will not complete. The header's msgId correlates to the original FILE_START.
// Integrity pass: no longer just a post-hoc log notice — the sender matches
// (peer, msgId) against its in-flight send and ABORTS the chunk loop, so a
// cancel now stops the remaining airtime spend (see fileSendAbortMsgId).
enum EspNowFileCancelReason : uint8_t {
  FILE_CANCEL_TIMEOUT      = 1,  // no frame arrived within the slot timeout
  FILE_CANCEL_WRITE_FAILED = 2,  // staging write / rename to flash failed
  FILE_CANCEL_INCOMPLETE   = 3,  // FILE_END arrived but chunks were missing
  FILE_CANCEL_CRC_MISMATCH = 4,  // all chunks arrived but the whole-file CRC32
                                 // didn't match FILE_END's declared value
  FILE_CANCEL_REJECTED     = 5,  // FILE_START rejected (no slot / alloc / too
                                 // big), or a chunk arrived for an unknown
                                 // transfer — receiver never had/lost the slot
};
struct __attribute__((packed)) V4PayloadFileCancel {
  uint8_t reason;         // EspNowFileCancelReason
};

// ---- FS LIST request/reply (ESPNOW_V4_TYPE_FS_LIST_REQ / _REPLY) ----------
//
// Bonded-peer remote directory browser. Sender ("client") asks the peer
// to list entries at `path`, optionally paginated via startIndex/maxEntries.
// Receiver builds the listing under SYSTEM identity (device-trust model —
// bonded peers see whatever the device's bonded-peer ACL allows) and replies
// with a fragmented V4_TYPE_FS_LIST_REPLY containing a header + N entry
// records back-to-back in the same payload.
//
// Why a structured opcode (not CLI scrape):
//   - Deterministic parsing (no `ls` text format drift)
//   - Pagination for large directories
//   - Native permission bits per entry → caller's UI can grey out actions
//   - Identity propagation is well-defined (peer's bonded-trust scope)
//   - Reply size capped at fragment-max so single round-trip suffices for
//     typical OLED listings (140 B header + ≤ 32 entries × 76 B = 2572 B →
//     13 fragments, well under the 32-fragment / 6400 B message limit)
//
// Request fits a single SESSION_FRAME (144 B < 202 B plaintext budget) so
// it doesn't need fragmentation on the way out. Reply is sent via
// v4_send_payload_smart which negotiates encrypted-chunked under a session.

struct __attribute__((packed)) V4PayloadFsListReq {
  uint32_t reqId;          // Client-chosen correlation ID; echoed in reply.
                           // 0 is RESERVED (sentinel for "no request" in
                           // the pending-request table). Senders MUST pick
                           // a nonzero value.
  uint16_t startIndex;     // Pagination — first entry to return (0 = start)
  uint16_t maxEntries;     // Cap; receiver may return fewer. Hard cap: 32.
  char     path[128];      // Null-terminated VFS path on receiver
  uint8_t  reserved[8];    // Must be zero
};
static_assert(sizeof(V4PayloadFsListReq) == 144, "V4PayloadFsListReq layout");
static_assert(sizeof(V4PayloadFsListReq) <= ESPNOW_V4_MAX_PLAINTEXT,
              "V4PayloadFsListReq must fit single SESSION_FRAME");

// Reply layout (variable-length, on wire):
//   [V4PayloadFsListReplyHeader] [V4PayloadFsEntry × header.entryCount]
//
// status codes:
//   0 = OK
//   1 = NOT_FOUND      (path doesn't exist on receiver)
//   2 = NOT_A_DIR      (path exists but is a file)
//   3 = PERM_DENIED    (path exists but bonded-peer identity can't read)
//   4 = IO_ERROR       (FS error reading dir)
//   5 = TOO_BUSY       (receiver is in the middle of another listing;
//                       caller should retry with backoff)
//   6 = NOT_READY      (filesystem not mounted yet — early boot)
//
// hasMore=1 + nextStartIndex tells the client to issue a follow-up with
// startIndex=nextStartIndex to fetch the rest of a large directory.

enum FsListStatus : uint8_t {
  FS_LIST_STATUS_OK          = 0,
  FS_LIST_STATUS_NOT_FOUND   = 1,
  FS_LIST_STATUS_NOT_A_DIR   = 2,
  FS_LIST_STATUS_PERM_DENIED = 3,
  FS_LIST_STATUS_IO_ERROR    = 4,
  FS_LIST_STATUS_TOO_BUSY    = 5,
  FS_LIST_STATUS_NOT_READY   = 6,
};

struct __attribute__((packed)) V4PayloadFsListReplyHeader {
  uint32_t reqId;          // Echoes request
  uint8_t  status;         // FsListStatus
  uint8_t  entryCount;     // Number of V4PayloadFsEntry records that follow
  uint8_t  hasMore;        // 1 if more entries available beyond this batch
  uint8_t  reserved;       // Must be zero
  uint16_t totalEntries;   // Total entries in directory (for UI "of N")
  uint16_t nextStartIndex; // Pass to follow-up request when hasMore=1
  char     path[128];      // Echoes (normalized) request path
};
static_assert(sizeof(V4PayloadFsListReplyHeader) == 140,
              "V4PayloadFsListReplyHeader layout");

// One entry in a FS_LIST_REPLY. Layout intentionally matches the spirit of
// FileEntry in System_FileManager.h so the OLED render path can copy fields
// near-1:1 from the wire into its FileEntry cache.
struct __attribute__((packed)) V4PayloadFsEntry {
  char     name[64];       // Null-terminated filename (no path prefix)
  uint32_t size;           // Bytes (0 for folders)
  uint8_t  isFolder;       // 0 or 1
  uint8_t  perms;          // Bitmask matching FileEntry::permissions
                           //   bit 0 = READ, bit 1 = WRITE, bit 2 = DELETE
  uint8_t  reserved[6];    // Must be zero
};
static_assert(sizeof(V4PayloadFsEntry) == 76, "V4PayloadFsEntry layout");

// Hard cap on entries per reply — fits comfortably under the 6400 B
// fragmented message limit (140 + 32 × 76 = 2572 B).
#define FS_LIST_ENTRIES_PER_REPLY 32

// ---- FS STAT request/reply (ESPNOW_V4_TYPE_FS_STAT_REQ / _REPLY) ----------
//
// Storage stats for a VFS root on the bonded peer (total / used / free bytes
// and a percent-used reading). Replaces the prior `BondFs.exec('fsusage')`
// CLI-scrape path which broke on output collisions with concurrent commands.
// One request fits a single SESSION_FRAME (140 B). Reply is 164 B (pinned by
// the static_assert below) — also single-frame, no fragmentation needed.

struct __attribute__((packed)) V4PayloadFsStatReq {
  uint32_t reqId;          // Client-chosen correlation ID (nonzero)
  char     path[128];      // Root path on the peer (e.g. "/" or "/sd")
  uint8_t  reserved[8];    // Must be zero
};
static_assert(sizeof(V4PayloadFsStatReq) == 140, "V4PayloadFsStatReq layout");
static_assert(sizeof(V4PayloadFsStatReq) <= ESPNOW_V4_MAX_PLAINTEXT,
              "V4PayloadFsStatReq must fit single SESSION_FRAME");

struct __attribute__((packed)) V4PayloadFsStatReply {
  uint32_t reqId;          // Echoes request
  uint8_t  status;         // FsListStatus values (NOT_FOUND, PERM_DENIED, etc.)
  uint8_t  reserved1[3];   // Alignment for the 64-bit fields below
  uint64_t totalBytes;     // Capacity of the storage backing this path
  uint64_t usedBytes;
  uint64_t freeBytes;
  uint16_t percentUsedX10; // Tens-of-percent (e.g. 472 = 47.2%) so client gets
                           // one decimal without paying for a float on the wire
  uint16_t reserved2;
  char     path[128];      // Echoes (normalized) request path
};
static_assert(sizeof(V4PayloadFsStatReply) == 164, "V4PayloadFsStatReply layout");
static_assert(sizeof(V4PayloadFsStatReply) <= ESPNOW_V4_MAX_PLAINTEXT,
              "V4PayloadFsStatReply must fit single SESSION_FRAME");

// ---- FS GET request/ack (ESPNOW_V4_TYPE_FS_GET_REQ / _ACK) -----------------
//
// Pull a file from the peer. Two-stage protocol because the actual file
// transfer rides the existing FILE_START / FILE_DATA / FILE_END opcodes
// (which already handle chunking, CRC, and ACK retransmit):
//
//   client                                    peer
//     │── FS_GET_REQ {reqId, path} ─────────►│
//     │◄──── FS_GET_ACK {reqId, status, ────┤  (sync — fits one frame)
//     │      fileSize} if status=OK         │
//     │                                     │  …then peer sends the file
//     │◄── FILE_START / FILE_DATA × N / ────┤   via the existing transfer
//     │      FILE_END ──────────────────────┤   pipeline.
//
// On status != OK in the ACK, no FILE_* transfer follows. On status == OK,
// caller's existing FILE_* receive path takes over.
//
// This replaces the prior `BondFs.exec('espnowsendfile <localMac> <path>')`
// path: same outcome, deterministic structured trigger + ack instead of
// running a CLI on the peer and inferring success from text output.

struct __attribute__((packed)) V4PayloadFsGetReq {
  uint32_t reqId;
  char     path[128];      // File to fetch on the peer's VFS
  uint8_t  reserved[8];
};
static_assert(sizeof(V4PayloadFsGetReq) == 140, "V4PayloadFsGetReq layout");
static_assert(sizeof(V4PayloadFsGetReq) <= ESPNOW_V4_MAX_PLAINTEXT,
              "V4PayloadFsGetReq must fit single SESSION_FRAME");

struct __attribute__((packed)) V4PayloadFsGetAck {
  uint32_t reqId;
  uint8_t  status;         // FsListStatus values
  uint8_t  reserved[3];
  uint32_t fileSize;       // File size in bytes (informational — lets the
                           // client display a progress bar without waiting
                           // for FILE_END's CRC)
  char     path[128];      // Echoes (normalized) request path
};
static_assert(sizeof(V4PayloadFsGetAck) == 140, "V4PayloadFsGetAck layout");
static_assert(sizeof(V4PayloadFsGetAck) <= ESPNOW_V4_MAX_PLAINTEXT,
              "V4PayloadFsGetAck must fit single SESSION_FRAME");

// ---- Multi-hop reachability invariants -------------------------------------
//
// Two out-of-range peers become reachable only if they can complete KEY_EX and
// SESSION establishment THROUGH a relay — so every handshake frame must fit a
// RELAY_DATA envelope. These asserts are the guard rail: grow one of these
// payloads past the budget and far-peer pairing silently stops working (the
// handshake would fall back to a direct send that never lands). Break the build
// instead, and re-derive the budget rather than papering over it.
static_assert(sizeof(V4PayloadKeyExHello)   <= ESPNOW_V4_RELAY_MAX_INNER_PAYLOAD,
              "KEY_EX_HELLO must fit a RELAY_DATA envelope (far-peer pairing)");
static_assert(sizeof(V4PayloadKeyExReply)   <= ESPNOW_V4_RELAY_MAX_INNER_PAYLOAD,
              "KEY_EX_REPLY must fit a RELAY_DATA envelope (far-peer pairing)");
static_assert(sizeof(V4PayloadKeyExConfirm) <= ESPNOW_V4_RELAY_MAX_INNER_PAYLOAD,
              "KEY_EX_CONFIRM must fit a RELAY_DATA envelope (far-peer pairing)");
static_assert(sizeof(V4PayloadSessionOpen)    <= ESPNOW_V4_RELAY_MAX_INNER_PAYLOAD,
              "SESSION_OPEN must fit a RELAY_DATA envelope (far-peer sessions)");
static_assert(sizeof(V4PayloadSessionConfirm) <= ESPNOW_V4_RELAY_MAX_INNER_PAYLOAD,
              "SESSION_CONFIRM must fit a RELAY_DATA envelope (far-peer sessions)");
// REKEY travels SEALED, so it spends the AEAD tag too — an exact 130+16 fit.
static_assert(sizeof(V4PayloadSessionRekey) <= ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT,
              "SESSION_REKEY must fit a SEALED frame inside a RELAY_DATA envelope");

#endif // ENABLE_ESPNOW

#endif // SYSTEM_ESPNOW_WIRE_H
