#ifndef SYSTEM_ESPNOW_WIRE_H
#define SYSTEM_ESPNOW_WIRE_H

// ============================================================================
// ESPNOW V4 wire schema — opcode enum, flags, 32-byte header, payload structs.
//
// Wire format version byte 4 (magic 0x3148, 32-byte header). Opcodes are in
// category-based ranges with reserved slots.
//
// Frame rules (1.0). 1.0 is a full-erase install; after it a format only
// grows at its end.
//   - Every multi-byte field is little-endian (asserted below).
//   - Every payload is a packed struct in this file, with a static_assert on
//     its size and on every field offset.
//   - Each opcode has a minimum payload length, ESPNOW_V4_MINLEN_<OPCODE>
//     below, used by its kV4HandlerTable row. It is measured after the
//     session unwrap, the group-tag strip and reassembly. The dispatcher drops
//     a shorter frame and counts it (espnowstats rxLengthDrops); a longer one
//     reaches the handler, which reads the fields it knows and ignores the
//     rest. After 1.0 fields are only appended, and a minimum never rises:
//     its static_assert against sizeof() becomes ">=" for a grown struct and
//     the receiver zero-fills the fields an older sender left out.
//   - The six handshake frames (KEY_EX_HELLO/REPLY/CONFIRM, SESSION_OPEN/
//     CONFIRM/REKEY) are the exception: exact length (signed transcripts),
//     so they cannot grow after 1.0.
//   - Fixed-header frames cannot grow either. Their payload ends in data
//     that runs to the end of the frame or in entries at a fixed stride, so an
//     older receiver would read an appended field as data. Neither their
//     header nor an entry struct gains a field after 1.0; more needs a new
//     opcode. They are RELAY_DATA (V4PayloadRelayData, then the inner frame),
//     PEER_LIST (V4PayloadPeerListHeader, V4PayloadPeerListEntry),
//     SENSOR_ENVELOPE and BOND_SENSOR_DATA (V4PayloadSensorReading, then a
//     part of the reading), CMD_RESP (V4PayloadCmdRespHeader, then text),
//     STREAM (V4PayloadStreamHeader, then text), FILE_DATA
//     (V4PayloadFileData), FS_LIST_REPLY (V4PayloadFsListReplyHeader,
//     V4PayloadFsEntry), and the text-only TEXT, BOOT, CMD and USER_SYNC.
//     Each of those structs is marked "Fixed header: cannot grow".
//   - Reserved bytes are sent as 0 and ignored by the receiver.
//   - A receiver reads a payload with V4RxCtx::read(struct) (zero-fills what
//     the frame lacks), a tail with V4RxCtx::tail(offset), or a Core parse*()
//     helper whose offsets the asserts here pin. Never by a pointer cast.
//   - The group (BROADCAST_AUTH) tag is 16 bytes.
//   - Sensor masks are 32 bits (espnowsensor::SensorMask).
//   - Device names travel in ESPNOW_V4_NAME_LEN (32) byte fields.
//   - Header flags in 0xF000 are must-understand: a receiver drops a frame
//     with a bit there it does not know (rxMustUnderstandDrops); it ignores
//     unknown bits elsewhere (see the flag enum).
//   - SESSION_OPEN and SESSION_CONFIRM carry each side's u32 capability
//     word inside the signed transcript (System_ESPNow_SessionCapsCore.h).
//   - An ACK means "received", not "accepted": the ACK goes out before the
//     dispatcher, so a frame dropped for its length is visible only in
//     rxLengthDrops on the receiver.
//   - Answers (rule F3, System_ESPNow_AnswerCore.h): every answer frame
//     starts {u32 reqId, u8 status} (V4PayloadAnswerHeader). reqId is the
//     request's correlation number (its reqId field, or its header msgId when
//     it has none; 0 = sent unasked); status 0 is ok, 1-15 are the shared
//     reasons (busy, refused, unsupported, too_large, no_slot, bad_request,
//     not_ready, io_error, not_found, timeout, cancelled), 16 and up a reason
//     of that frame alone. The header msgId of every frame, answers included,
//     is the sender's own fresh number: no frame borrows the request's msgId.
//     The answers are TOPO_START, TOPO_PEER, CMD_RESP, FS_LIST_REPLY,
//     FS_STAT_REPLY, FS_GET_ACK, STREAM, FILE_CANCEL, CATCHUP_LIST_RESP and
//     CATCHUP_READ_RESP.
//   - METADATA is exactly the 202-byte sealed budget, so it cannot grow (to a
//     peer behind a relay it goes as sealed fragments: sendMetadata);
//     FS_STAT_REPLY (164) has 38 bytes left, the routed frames their margins
//     against ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT.
//
// Restrictions on contents of this file:
//   - Value-typed declarations only (enums, packed structs, constants).
//   - No function declarations, no globals, no logging.
//   - Anything that would create a dependency back into the ESPNOW
//     implementation belongs in System_ESPNow.h / .cpp, not here.
// ============================================================================

#include <Arduino.h>
#include "System_BuildConfig.h"
#include "System_ESPNow_SensorCore.h"  // the sensor numbering and the reading header's sizes
#include "System_ESPNow_PowerEventCore.h"  // the battery and event frames' sizes
#include "System_ESPNow_ForwardCore.h"     // the forward-link frame's size
#include "System_ESPNow_TimeCore.h"    // the TIME_SYNC frame's size and source numbers
#include "System_ESPNow_AnswerCore.h"  // the answer header {reqId, status} and the status table (rule F3)
#include "System_ESPNow_CatchupCore.h" // the CATCHUP_* frames' sizes, opcodes and status bytes
#include "System_DeviceName.h"        // the device-name rule and its 32-byte wire field

#if ENABLE_ESPNOW

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
              "the wire format is little-endian and its structs are copied as they are");

// ---- Protocol constants ----------------------------------------------------

// Width of every device-name field on the wire: up to 31 bytes of name and a
// zero (a full field may have no terminator; receivers bound their reads).
#define ESPNOW_V4_NAME_LEN     32

#define ESPNOW_V4_MAGIC        0x3148u       // 'H1' little-endian
#define ESPNOW_V4_VERSION      4             // Protocol version
#define ESPNOW_V4_HEADER_LEN   32            // Header size in bytes
#define ESPNOW_V4_MAX_PAYLOAD  (250 - 32)    // 218 bytes max payload

// The name of this format, as `versions` shows it: "custom ESP-NOW v1". It is
// HardwareOne's own mesh format carried in Espressif ESP-NOW frames; "v1" is
// the Espressif frame class it fits (header + payload within ESP-NOW v1's 250
// bytes, not v2's 1470), worked out from the two lengths above. Display only:
// peers still check ESPNOW_V4_VERSION, the byte on the wire, which this does
// not change. HW1_ESPNOW_RELEASE is the owner's own number for the format; it
// is not displayed, and it moves when the wire format does (the assert below).
#define HW1_ESPNOW_RELEASE     "1.0"
static_assert(ESPNOW_V4_VERSION == 4,
              "wire format changed: bump HW1_ESPNOW_RELEASE and docs/COMPATIBILITY.md");

// When a frame is wrapped in SESSION_FRAME, 16 of the 218 payload
// bytes are consumed by the Poly1305 AEAD tag (cipher || tag layout on wire).
// Structs that may be encrypted MUST fit in this lower bound, not the raw
// MAX_PAYLOAD. static_asserts below enforce this for the encrypt-eligible
// payload types; new structs in the app-unicast (50–69), remote-FS (70–89),
// files (110–129), sensors (150–169), and bond (170–189) ranges should pick
// this constant for their size cap.
#define ESPNOW_V4_AEAD_TAG_LEN 16
#define ESPNOW_V4_MAX_PLAINTEXT (ESPNOW_V4_MAX_PAYLOAD - ESPNOW_V4_AEAD_TAG_LEN)  // 202 bytes

// BROADCAST_AUTH (group-signed) tag: the first 16 bytes of
// HMAC-SHA256(groupKey, "hw1-espnow-1-group" || header[0..29] || payload)
// (System_ESPNow_HandshakeCore.h), appended to the payload and
// compared in constant time over those 16 bytes. The key is the mesh group key
// (the "esp-grup" subkey of the mesh-key derivation). Authentication only: the
// payload stays plaintext. 16 bytes, like the AEAD tag, since 1.0 (was 32).
#define ESPNOW_V4_BROADCAST_AUTH_TAG_LEN 16
#define ESPNOW_V4_MAX_BROADCAST_AUTHED_PLAINTEXT \
            (ESPNOW_V4_MAX_PAYLOAD - ESPNOW_V4_BROADCAST_AUTH_TAG_LEN)  // 202 bytes
static_assert(ESPNOW_V4_MAX_BROADCAST_AUTHED_PLAINTEXT == 202, "group-signed payload budget");

// ---- Opcode enum -----------------------------------------------------------
// Category-based numbering, 20 slots per category. Renumbered 2026-07 from the
// original 10-wide V4 map to create growth room; the wire is incompatible with
// the old numbering and no migration path is carried — the whole fleet is
// erased and reflashed together (version byte deliberately unchanged).
//
// Ranges:
//   0        invalid — never assign
//   1–9      Transport     (ACK live, RELAY_DATA=5 live; FRAG_REQ/FRAG_REPLY reserved 3–4;
//                           2 free (released 1.0: the ACK carries a status))
//   10–29    Crypto        (KEY_EX_* 10–12, SESSION_* 13–15;
//                           16/17 free (released 1.0);
//                           SESSION_UNKNOWN=18 live, 2026-11-18)
//   30–49    Discovery     (HEARTBEAT, BOOT, TOPO_*, TIME_SYNC, PAIR_BEACON,
//                           PEER_LIST=39 live; 37/38 free (released 1.0: the
//                           session caps word covers capability discovery))
//   50–69    App unicast   (CMD, CMD_RESP, TEXT, METADATA_*, USER_SYNC)
//   70–89    Remote FS     (FS_* read side live 70–75; 76–81 earmarked write
//                           side: DELETE_REQ/ACK, PUT_REQ/ACK, MKDIR, RENAME;
//                           82–85 live file catch-up (back-capture, 2026-11-19):
//                           CATCHUP_LIST_REQ=82, CATCHUP_LIST_RESP=83,
//                           CATCHUP_READ_REQ=84, CATCHUP_READ_RESP=85; 86–89 free)
//   90–109   Streaming     (STREAM; 91–93 earmarked MEDIA_START/DATA/END)
//   110–129  Files         (FILE_START/DATA/END/CANCEL live; FILE_ACK=113,
//                           FILE_PROGRESS=114, FILE_NACK=116, FILE_RESUME_OK=117 reserved)
//   130–149  Events        (EVENT_PUSH=132 live: one system event of the sender, a
//                           session unicast to the masters named on it; 130 retired
//                           (SUBSCRIBE_UPDATE, removed in 1.0 — do not reuse); 131 and
//                           133–149 free (131/133–135 earmarks released 1.0))
//   150–169  Sensors       (SENSOR_STATUS=151 live: which sensors the sender has and
//                           streams, a session unicast to the device that receives its
//                           readings; SENSOR_REQ=153/SENSOR_ENVELOPE=154 live —
//                           the leased pull and the reading, in 1 to 4 frames;
//                           150 retired (SENSOR_BROADCAST, removed — do not reuse);
//                           POWER_STATUS=152 live: the sender's battery state, a
//                           session unicast to the masters named on it;
//                           FORWARD_LINK=155 live: a bond master announces to the
//                           masters named on it that it forwards its bond partner's
//                           readings, battery state, events and sensor status, and
//                           they answer; a master's SENSOR_REQ with its link byte set
//                           asks the bond master for its partner's readings;
//                           156–158 earmarked device outbox (back-capture, 1.x):
//                           BACKLOG_REQ=156, BACKLOG_RESP=157, BACKLOG_DATA=158;
//                           159 free)
//   170–189  Bond          (BOND_*, MANIFEST_REQ, SETTINGS_REQ, SCHEMA_REQ, plus
//                           BOND_STREAM_CTRL/BOND_SENSOR_DATA — moved in from
//                           streaming/sensors because they were always bond-gated;
//                           BOND_REQUEST=183 / BOND_NOTICE=184 are accepted from any
//                           paired peer over its session, not only the bond partner;
//                           BOND_POWER_STATUS=180 / BOND_EVENT_PUSH=182: a bond
//                           worker's battery state and events, to its bond master
//                           only. This reverses the 2026-11 decision that they "need
//                           no bond": a bond worker never sends to a mesh master
//                           itself (owner rule), so they travel over the bond link
//                           with the 152/132 payloads unchanged;
//                           BOND_SENSOR_STATUS=185: a bond worker's sensor status,
//                           to its bond master only, the 151 payload unchanged; 181
//                           keeps its earmark)
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

enum EspNowV4Type : uint8_t {
  // --- Transport (1–9) ---
  ESPNOW_V4_TYPE_ACK             = 1,
  // 2 free (was the NACK earmark; released 1.0). 3–4 reserved FRAG_REQ / FRAG_REPLY
  // (selective retransmit, after 1.0)
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
  // 16/17 free (the SESSION_AUTH_REQ/GRANT earmark was released in 1.0: a remote
  // command carries its credentials, D2)
  ESPNOW_V4_TYPE_SESSION_UNKNOWN = 18,  // "I have no session with that id": the answer to a sealed frame for a
                                        //   session the receiver does not hold (it restarted). Group-signed
                                        //   (BROADCAST_AUTH), ttl 1, V4PayloadSessionUnknown. The sender of the
                                        //   frame only closes that session and opens a new one; see
                                        //   System_ESPNow_PeerRestartCore.h

  // --- Discovery / timing (30–49) ---
  ESPNOW_V4_TYPE_HEARTBEAT       = 30,
  ESPNOW_V4_TYPE_BOOT            = 31,  // Device boot/online notice — its OWN type so the RX files it as a system event, not chat.
                                        //   JSON envelope, pld.boot = the sender's boot counter. Since 2026-11-18
                                        //   sent group-signed (BROADCAST_AUTH, ttl 1) to each paired peer the
                                        //   sender holds no live session with, so it arrives without a session: a
                                        //   receiver that still holds a session with the sender closes it when the
                                        //   counter is newer than the last one seen and the tag is of the sender's
                                        //   mesh (System_ESPNow_PeerRestartCore.h); a copy (counter already seen or
                                        //   older) is not filed again. Sealed to a peer the sender still holds a
                                        //   live session with (ESP-NOW closed and opened again: sessions stay in
                                        //   RAM), which ends nothing and only records the counter, and when no
                                        //   group key is held for the peer's mesh. The row (REQ_AUTH_WHEN_KEYED)
                                        //   is unchanged.
  ESPNOW_V4_TYPE_TOPO_REQ        = 32,
  ESPNOW_V4_TYPE_TOPO_START      = 33,
  ESPNOW_V4_TYPE_TOPO_PEER       = 34,
  ESPNOW_V4_TYPE_TIME_SYNC       = 35,  // master → paired device, session unicast: the master's UTC clock
  ESPNOW_V4_TYPE_PAIR_BEACON     = 36,  // Discovery beacon — BROADCAST_AUTH FF broadcast sent while a discovery window is open
  // 37/38 free (the CAP_REQ/CAP_RESP earmark was released in 1.0)
  ESPNOW_V4_TYPE_PEER_LIST       = 39,  // Distance-vector route advertisement. BROADCAST_AUTH, ttl=1
                                        //   ALWAYS (never relayed — it describes the sender's own view,
                                        //   so forwarding it would attribute someone else's routes to us).
  // Discover-then-confirm pairing control (BROADCAST_AUTH FF broadcast + V4PayloadPairCtrl.targetMac).
  ESPNOW_V4_TYPE_PAIR_REQUEST    = 40,  // initiator → target: "I want to pair" (requester name and key)
  ESPNOW_V4_TYPE_PAIR_ACCEPT     = 41,  // target → initiator: user accepted; both sides now run pairsecure
  ESPNOW_V4_TYPE_PAIR_REJECT     = 42,  // target → initiator: user rejected / not in discovery mode

  // --- Application unicast (50–69) ---
  ESPNOW_V4_TYPE_CMD             = 50,
  ESPNOW_V4_TYPE_CMD_RESP        = 51,
  ESPNOW_V4_TYPE_TEXT            = 52,
  ESPNOW_V4_TYPE_METADATA_REQ    = 53,
  ESPNOW_V4_TYPE_METADATA_RESP   = 54,
  ESPNOW_V4_TYPE_METADATA_PUSH   = 55,  // unsolicited; metadataPushTick sends it when local metadata changes
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
  // File catch-up (back-capture, 2026-11-19; System_ESPNow_CatchupCore.h): a
  // master named on a worker lists the files it offers and reads byte ranges.
  ESPNOW_V4_TYPE_CATCHUP_LIST_REQ  = 82,  // collector → device: one LIST page (V4PayloadCatchupListReq)
  ESPNOW_V4_TYPE_CATCHUP_LIST_RESP = 83,  // device → collector: header + self-sized entries
  ESPNOW_V4_TYPE_CATCHUP_READ_REQ  = 84,  // collector → device: a window of one file (V4PayloadCatchupReadReq)
  ESPNOW_V4_TYPE_CATCHUP_READ_RESP = 85,  // device → collector: 1-8 frames, header + data
  // 86–89 free

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
  // 130 retired — SUBSCRIBE_UPDATE (per-peer event mask), removed in 1.0; it was on
  //   the wire, so the number is never reused. 131 free.
  ESPNOW_V4_TYPE_EVENT_PUSH      = 132,  // sender → the masters named on it: one system event (V4PayloadEventPush)
  // 133–149 free (the SUB_LIST and EVENT_SUB v2 earmarks were released in 1.0)

  // --- Sensors (150–169) ---
  // 150 retired — SENSOR_BROADCAST (plaintext sensor data). TX removed with the D2 secure
  //   fetcher, RX row removed afterwards; do not reuse the number.
  ESPNOW_V4_TYPE_SENSOR_STATUS   = 151,  // sender of readings → their receiver: sensors present / streaming (masks)
  ESPNOW_V4_TYPE_POWER_STATUS    = 152,  // sender → the masters named on it: its battery state (V4PayloadPowerStatus)
  ESPNOW_V4_TYPE_SENSOR_REQ      = 153,  // master → worker: subscribe/oneshot/unsubscribe (leased pull)
  ESPNOW_V4_TYPE_SENSOR_ENVELOPE = 154,  // worker → master: one part of a sensor reading (V4PayloadSensorReading)
  ESPNOW_V4_TYPE_FORWARD_LINK    = 155,  // bond master ⇄ the masters named on it: "I forward for my bond partner"
                                         //   and the answer (V4PayloadForwardLink, System_ESPNow_ForwardCore.h)
  // 156–158 earmarked device outbox: BACKLOG_REQ / BACKLOG_RESP / BACKLOG_DATA
  // (back-capture design 2026-11-19, built in 1.x; wire stream ids below); 159 free

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
  ESPNOW_V4_TYPE_BOND_SENSOR_DATA= 179,  // one part of a sensor reading to the bond partner (same layout as 154)
  ESPNOW_V4_TYPE_BOND_POWER_STATUS = 180,  // bond worker → its bond master: battery state (same layout as 152)
  // 181 earmarked BOND_SETTINGS_SYNC_ACK.
  ESPNOW_V4_TYPE_BOND_EVENT_PUSH = 182,  // bond worker → its bond master: one system event (same layout as 132)
  ESPNOW_V4_TYPE_BOND_REQUEST    = 183,  // "you are my bond partner": sent to the configured partner
  ESPNOW_V4_TYPE_BOND_NOTICE     = 184,  // how the sender's bond stands toward the receiver (BondNoticeReason)
  ESPNOW_V4_TYPE_BOND_SENSOR_STATUS = 185,  // bond worker → its bond master: sensors present / streaming (same layout as 151)
};

// ---- Wire stream ids (earmarked for back-capture, built in 1.x) -------------
// The stream byte of the back-capture frames (BACKLOG_*, 156–158) is a fixed
// wire number, independent of REMOTE_SENSOR_MAX, so a new sensor type never
// renumbers battery or events. The device maps it to its internal stream ids
// (espnowfwd::kStreamId*) at the edge:
//     0–127   the sensor type (RemoteSensorType, the reading header's byte 0)
//     0xF0    battery (the POWER_STATUS values)
//     0xF1    events (the EVENT_PUSH values)
//     0xFF    all streams (a status request or the "I hold backlog" notice)
// Every other value is unassigned. Comment only until those frames land: an
// enum symbol comes with its row.
//
// ---- Flag bits (16-bit) ---------------------------------------------------
//
// Two classes of bit (1.0 rule):
//   * 0xF000 is must-understand. A receiver drops a frame that carries a bit
//     there it does not know (ESPNOW_V4_FLAGS_MUST_UNDERSTAND minus
//     ESPNOW_V4_FLAGS_KNOWN_MUST_UNDERSTAND), before it is acknowledged,
//     remembered, authenticated or forwarded by flooding, and counts it
//     (espnowstats "RX Must-Understand Drops"). A relay forwarding a routed
//     envelope does not look inside it; the final receiver checks on delivery.
//   * Every other bit is ignored by a receiver that does not know it.
// A new bit that changes how the payload is read goes in 0xF000, and a sender
// sets it only on a sealed frame to a peer whose session capability word
// (System_ESPNow_SessionCapsCore.h) advertises the matching capability: never
// on handshake, group-signed or plaintext frames, which no session vouches for.
// BROADCAST_AUTH and SESSION_FRAME also change how the payload is read, but
// sit outside 0xF000 because every 1.0 receiver knows them; moving them would
// spend two of the four must-understand bits.
// Bits are only defined when something both sets and reads them
// (test_espnow_header_flags.py). Removed at 1.0, never set or never read:
// 0x0002 ENCRYPTED, 0x0004 COMPRESS, 0x0010 STREAM_BEGIN, 0x0020 STREAM_END,
// 0x0400 HANDSHAKE, 0x0800 PRIORITY_HIGH. Those values are free again.

enum EspNowV4Flags : uint16_t {
  ESPNOW_V4_FLAG_ACK_REQ        = 0x0001,  // Request ACK from receiver
  ESPNOW_V4_FLAG_BROADCAST_AUTH = 0x0100,  // payload ends in the mesh group key's tag
  ESPNOW_V4_FLAG_SESSION_FRAME  = 0x0200,  // payload is AEAD-sealed with the session key
  ESPNOW_V4_FLAGS_MUST_UNDERSTAND       = 0xF000,  // class mask, not a flag
  ESPNOW_V4_FLAGS_KNOWN_MUST_UNDERSTAND = 0x0000,  // must-understand bits this firmware knows
  // The must-understand bits a receiver of this firmware drops a frame for.
  ESPNOW_V4_FLAGS_UNKNOWN_MUST_UNDERSTAND =
      ESPNOW_V4_FLAGS_MUST_UNDERSTAND & ~ESPNOW_V4_FLAGS_KNOWN_MUST_UNDERSTAND,
};
static_assert(((ESPNOW_V4_FLAG_ACK_REQ | ESPNOW_V4_FLAG_BROADCAST_AUTH | ESPNOW_V4_FLAG_SESSION_FRAME) &
               ESPNOW_V4_FLAGS_MUST_UNDERSTAND) == 0,
              "the 1.0 flags are outside the must-understand class");
static_assert((ESPNOW_V4_FLAGS_KNOWN_MUST_UNDERSTAND & ~ESPNOW_V4_FLAGS_MUST_UNDERSTAND) == 0,
              "known must-understand bits lie inside the class");

// ---- Frame header (32 bytes) -----------------------------------------------
//
// The payload length is not a header field: it is the radio frame's length
// minus headerLen.

struct __attribute__((packed)) EspNowV4Header {
  uint16_t magic;            // 0   — 0x3148 ('H1' little-endian)
  uint8_t  ver;              // 2   — ESPNOW_V4_VERSION (4)
  uint8_t  type;             // 3   — EspNowV4Type
  uint16_t flags;            // 4–5 — EspNowV4Flags (16-bit)
  uint8_t  headerLen;        // 6   — ESPNOW_V4_HEADER_LEN (32)
  uint8_t  reserved1;        // 7   — sent 0, ignored
  uint32_t msgId;            // 8–11 — the sender's own number: random nonzero start each ESP-NOW
                             //         start, never 0 (0 skips the duplicate check), 0xFFFFFFFF -> 1
                             //         on wrap (System_ESPNow_MsgIdCore.h)
  uint8_t  origin[6];        // 12–17 — original sender MAC. NOT the radio sender once a frame has
                             //         been relayed: every forwarder preserves origin verbatim so
                             //         dedup, attribution and loop suppression stay end-to-end.
  uint8_t  ttl;              // 18  — hops remaining. Decremented (and the BROADCAST_AUTH tag
                             //         recomputed) by each forwarder; a frame arriving with ttl<=1
                             //         is delivered locally but never forwarded again. ttl=1 at TX
                             //         therefore means "single hop, never relayed" — that is the
                             //         correct value for anything link-local (HEARTBEAT, PEER_LIST,
                             //         ACK, PAIR_*). Relay-eligible today: BROADCAST_AUTH TEXT
                             //         (flooded) and RELAY_DATA (routed; TIME_SYNC and the
                             //         sensor frames travel in it, never flooded). See
                             //         docs/ESPNOW_MESH_SYSTEM.md.
  uint8_t  fragIndex;        // 19  — fragment index (0-based)
  uint8_t  fragCount;        // 20  — total fragment count (1 = not fragmented)
  uint8_t  reserved2;        // 21  — sent 0, ignored
  uint16_t meshFingerprint;  // 22–23 — the mesh id: derived from the mesh key (System_ESPNow_MeshIdCore.h:
                             //         KDF subkey 3, context "esp-mid1", first nonzero LE u16), never 0;
                             //         a receiver drops 0 and any id none of its enabled meshes has
  uint16_t sessionId;        // 24–25 — the pairwise session of a SESSION_FRAME (0 otherwise)
  uint32_t frameSeq;         // 26–29 — that session's frame number (replay check; 0 otherwise)
  uint16_t crc16;            // 30–31 — CRC16-CCITT of payload
};
static_assert(sizeof(EspNowV4Header) == 32, "EspNowV4Header must be 32 bytes");
static_assert(offsetof(EspNowV4Header, magic) == 0 && offsetof(EspNowV4Header, ver) == 2 &&
              offsetof(EspNowV4Header, type) == 3 && offsetof(EspNowV4Header, flags) == 4 &&
              offsetof(EspNowV4Header, headerLen) == 6 && offsetof(EspNowV4Header, reserved1) == 7 &&
              offsetof(EspNowV4Header, msgId) == 8 && offsetof(EspNowV4Header, origin) == 12 &&
              offsetof(EspNowV4Header, ttl) == 18 && offsetof(EspNowV4Header, fragIndex) == 19 &&
              offsetof(EspNowV4Header, fragCount) == 20 && offsetof(EspNowV4Header, reserved2) == 21 &&
              offsetof(EspNowV4Header, meshFingerprint) == 22 && offsetof(EspNowV4Header, sessionId) == 24 &&
              offsetof(EspNowV4Header, frameSeq) == 26 && offsetof(EspNowV4Header, crc16) == 30,
              "EspNowV4Header layout");

// ---- Payload structs -------------------------------------------------------

// ---- Answer header (rule F3) ------------------------------------------------
// The first five bytes of every answer frame (System_ESPNow_AnswerCore.h has
// the rules and the status table; espnowanswer::packHeader / parseHeader
// write and read it). Each answer struct below starts with these two fields
// at these offsets; the asserts after the structs pin it.
struct __attribute__((packed)) V4PayloadAnswerHeader {
  uint32_t reqId;   // 0  the request this answers (0: sent unasked)
  uint8_t  status;  // 4  0 ok; 1-15 shared (espnowanswer::k*); 16+ this frame's own
};
static_assert(sizeof(V4PayloadAnswerHeader) == 5 && sizeof(V4PayloadAnswerHeader) == espnowanswer::kHeaderLen,
              "V4PayloadAnswerHeader is 5 bytes on the wire");
static_assert(offsetof(V4PayloadAnswerHeader, reqId) == 0 && offsetof(V4PayloadAnswerHeader, status) == 4,
              "V4PayloadAnswerHeader layout (espnowanswer::packHeader writes these offsets)");

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
//   [ inner V4 frame     ≤ 194 B ]  BYTE-IDENTICAL — header and payload exactly as
//                                   the originator built them, including its own
//                                   origin, sessionId, frameSeq, crc16 and AEAD tag
//   [ outer group tag       16 B ]  BROADCAST_AUTH over (outer hdr[0..29] || the two
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
// Fixed header: cannot grow (see the frame rules at the top of this file).
struct __attribute__((packed)) V4PayloadRelayData {
  uint8_t finalDst[6];   // ultimate destination MAC — the only node that unwraps
  uint8_t hopsUsed;      // forwards taken so far; 0 as emitted by the originator
  uint8_t reserved;      // sent 0, ignored
};
static_assert(sizeof(V4PayloadRelayData) == 8, "V4PayloadRelayData must be 8 bytes");
static_assert(offsetof(V4PayloadRelayData, finalDst) == 0 && offsetof(V4PayloadRelayData, hopsUsed) == 6 &&
              offsetof(V4PayloadRelayData, reserved) == 7, "V4PayloadRelayData layout");

// Byte budget for what an envelope can carry. 250 (radio MTU) minus the outer
// header, the envelope, and the outer HMAC tag.
#define ESPNOW_V4_RELAY_MAX_INNER_FRAME \
            (250 - ESPNOW_V4_HEADER_LEN - 8 - ESPNOW_V4_BROADCAST_AUTH_TAG_LEN)   // 194
#define ESPNOW_V4_RELAY_MAX_INNER_PAYLOAD \
            (ESPNOW_V4_RELAY_MAX_INNER_FRAME - ESPNOW_V4_HEADER_LEN)              // 162
// …and what fits once the inner frame is itself AEAD-sealed end-to-end.
#define ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT \
            (ESPNOW_V4_RELAY_MAX_INNER_PAYLOAD - ESPNOW_V4_AEAD_TAG_LEN)          // 146
static_assert(ESPNOW_V4_RELAY_MAX_INNER_FRAME == 194 && ESPNOW_V4_RELAY_MAX_INNER_PAYLOAD == 162,
              "routed-unicast budgets changed — re-check the handshake payloads below, "
              "which travel plaintext over a relay");
static_assert(ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT == 146,
              "routed sealed budget changed — re-check the sensor, time, battery, event and "
              "forward-link frames below and the reading part size, which are sized to it");

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
// Fixed header: cannot grow (see the frame rules at the top of this file).
struct __attribute__((packed)) V4PayloadPeerListHeader {
  uint8_t entryCount;   // number of V4PayloadPeerListEntry records that follow
  uint8_t role;         // sender MESH_ROLE_* (informational)
  uint8_t seq;          // advertisement generation, wraps — diagnostics only
  uint8_t reserved;     // sent 0, ignored
};
static_assert(sizeof(V4PayloadPeerListHeader) == 4, "V4PayloadPeerListHeader must be 4 bytes");
static_assert(offsetof(V4PayloadPeerListHeader, entryCount) == 0 && offsetof(V4PayloadPeerListHeader, role) == 1 &&
              offsetof(V4PayloadPeerListHeader, seq) == 2 && offsetof(V4PayloadPeerListHeader, reserved) == 3,
              "V4PayloadPeerListHeader layout");

// Fixed header: cannot grow (see the frame rules at the top of this file).
struct __attribute__((packed)) V4PayloadPeerListEntry {
  uint8_t mac[6];       // a destination the advertiser can reach
  uint8_t hops;         // advertiser's hop count to it (1 = advertiser's direct neighbour)
  int8_t  metric;       // worst link RSSI (dBm) along the advertiser's path; -128 = unknown
};
static_assert(sizeof(V4PayloadPeerListEntry) == 8, "V4PayloadPeerListEntry must be 8 bytes");
static_assert(offsetof(V4PayloadPeerListEntry, mac) == 0 && offsetof(V4PayloadPeerListEntry, hops) == 6 &&
              offsetof(V4PayloadPeerListEntry, metric) == 7, "V4PayloadPeerListEntry layout");

// How many entries fit one BROADCAST_AUTH frame (202 B of authed plaintext).
#define ESPNOW_V4_PEER_LIST_MAX_ENTRIES \
            ((ESPNOW_V4_MAX_BROADCAST_AUTHED_PLAINTEXT - 4) / 8)                  // 24
static_assert(sizeof(V4PayloadPeerListHeader)
                + ESPNOW_V4_PEER_LIST_MAX_ENTRIES * sizeof(V4PayloadPeerListEntry)
              <= ESPNOW_V4_MAX_BROADCAST_AUTHED_PLAINTEXT,
              "a full PEER_LIST must fit one broadcast-authed frame");

// HEARTBEAT (30). Every sender builds it with v4FillHeartbeat (the app ping too).
// chanInfo (System_ESPNow_ChannelFollowCore.h): bits 0-3 the sender's radio
// channel (0 = not stated, older firmware), 0x10 probe (a hunting worker asks
// its master to answer), 0x20 answer; 0xC0 sent 0, ignored.
struct __attribute__((packed)) V4PayloadHeartbeat {
  uint8_t  role;          // 0
  uint8_t  peerCount;     // 1
  int8_t   rssi;          // 2
  uint8_t  chanInfo;      // 3   channel + probe/answer bits (older firmware: 0, ignores it)
  uint32_t uptimeSec;     // 4
  uint32_t freeHeap;      // 8
  char     deviceName[32];  // 12  the sender's device name (System_DeviceName.h); NUL always present, rest 0
};
static_assert(sizeof(V4PayloadHeartbeat) == 44, "V4PayloadHeartbeat must be 44 bytes");
static_assert(offsetof(V4PayloadHeartbeat, role) == 0 && offsetof(V4PayloadHeartbeat, peerCount) == 1 &&
              offsetof(V4PayloadHeartbeat, rssi) == 2 && offsetof(V4PayloadHeartbeat, chanInfo) == 3 &&
              offsetof(V4PayloadHeartbeat, uptimeSec) == 4 && offsetof(V4PayloadHeartbeat, freeHeap) == 8 &&
              offsetof(V4PayloadHeartbeat, deviceName) == 12, "V4PayloadHeartbeat layout");
static_assert(sizeof(V4PayloadHeartbeat::deviceName) == devicename::kFieldLen, "V4PayloadHeartbeat name field");

// WPS-style pairing beacon (ESPNOW_V4_TYPE_PAIR_BEACON). Sent group-signed
// (BROADCAST_AUTH) to the FF broadcast address every ~1.5s while a device's
// pairing window is open; the row requires the tag. Same-mesh scoping comes
// from the header meshFingerprint gate (RX drops beacons whose mesh id matches
// no enabled local mesh) and the tag, so no mesh id is carried here. The
// receiver reads deviceName with devicename::getField (an invalid name is
// absent and shown as "peer-B2C3") and never trusts it beyond a display
// label; real trust comes from the subsequent KEY_EX (mesh-passphrase HMAC).
struct __attribute__((packed)) V4PayloadPairBeacon {
  uint8_t  role;            // 0  sender MESH_ROLE_* (informational)
  uint8_t  flags;           // 1  sent 0, ignored
  uint8_t  reserved[2];     // 2  sent 0, ignored
  char     deviceName[32];  // 4  sender's device name (System_DeviceName.h); NUL always present, rest 0
};
static_assert(sizeof(V4PayloadPairBeacon) == 36, "V4PayloadPairBeacon must be 36 bytes");
static_assert(offsetof(V4PayloadPairBeacon, role) == 0 && offsetof(V4PayloadPairBeacon, flags) == 1 &&
              offsetof(V4PayloadPairBeacon, reserved) == 2 && offsetof(V4PayloadPairBeacon, deviceName) == 4,
              "V4PayloadPairBeacon layout");
static_assert(sizeof(V4PayloadPairBeacon::deviceName) == devicename::kFieldLen, "V4PayloadPairBeacon name field");

// Discover-then-confirm control frame (PAIR_REQUEST / PAIR_ACCEPT / PAIR_REJECT).
// Sent as a BROADCAST_AUTH FF broadcast — same proven path as the beacon — because
// the two devices are NOT yet ESP-NOW peers (a unicast esp_now_send would fail,
// and fingerprintForPeer() of an unknown MAC yields no mesh key so the HMAC would
// be skipped). `targetMac` addresses the intended recipient; every other same-mesh
// device that hears the broadcast ignores it. All fields attacker-controllable
// except targetMac's match against our own MAC (deviceName via devicename::getField).
struct __attribute__((packed)) V4PayloadPairCtrl {
  uint8_t targetMac[6];     // 0  intended recipient — receivers drop if != own MAC
  uint8_t role;             // 6  sender MESH_ROLE_* (informational)
  uint8_t reserved;         // 7  sent 0, ignored
  char    deviceName[32];   // 8  sender's device name (System_DeviceName.h); NUL always present, rest 0
};
static_assert(sizeof(V4PayloadPairCtrl) == 40, "V4PayloadPairCtrl must be 40 bytes");
static_assert(offsetof(V4PayloadPairCtrl, targetMac) == 0 && offsetof(V4PayloadPairCtrl, role) == 6 &&
              offsetof(V4PayloadPairCtrl, reserved) == 7 && offsetof(V4PayloadPairCtrl, deviceName) == 8,
              "V4PayloadPairCtrl layout");
static_assert(sizeof(V4PayloadPairCtrl::deviceName) == devicename::kFieldLen, "V4PayloadPairCtrl name field");

// PAIR_REQUEST (40): the control frame plus the requester's long-term Ed25519
// key (1.0), so a request from a revoked key is refused whatever MAC it comes
// from, and the accept prompt can show the keyId. Unauthenticated beyond the
// group tag: the key exchange that follows an accept proves it.
struct __attribute__((packed)) V4PayloadPairRequest {
  V4PayloadPairCtrl ctrl;   // 0
  uint8_t senderPub[32];    // 40 the requester's long-term key (keyId = its first 8 bytes)
};
static_assert(sizeof(V4PayloadPairRequest) == 72, "V4PayloadPairRequest must be 72 bytes");
static_assert(offsetof(V4PayloadPairRequest, ctrl) == 0 && offsetof(V4PayloadPairRequest, senderPub) == 40,
              "V4PayloadPairRequest layout");

// TIME_SYNC (35): the mesh master's clock, sent to each paired device over
// its session (System_ESPNow_TimeCore.h has the rules and the meaning of
// every field). UTC; the timezone is applied only where a time is shown.
// The code packs and parses with meshtime::packFrame / parseFrame; this
// struct states the layout.
struct __attribute__((packed)) V4PayloadTimeSync {
  int64_t  epochMs;       // 0   UTC milliseconds since 1970, the sender's clock when it built the frame
  uint8_t  source;        // 8   where the sender's clock came from (meshtime::kSrc*); never 0
  uint8_t  flags;         // 9   bit 0: the sender is a backup master acting as master
  uint16_t sinceSetMin;   // 10  minutes since that source set the sender's clock; 65535 = or longer
  uint8_t  reserved[4];   // 12  0
};
static_assert(sizeof(V4PayloadTimeSync) == 16 && sizeof(V4PayloadTimeSync) == meshtime::kFrameLen,
              "V4PayloadTimeSync is 16 bytes on the wire");
static_assert(offsetof(V4PayloadTimeSync, epochMs) == 0 && offsetof(V4PayloadTimeSync, source) == 8 && offsetof(V4PayloadTimeSync, flags) == 9 &&
              offsetof(V4PayloadTimeSync, sinceSetMin) == 10 && offsetof(V4PayloadTimeSync, reserved) == 12,
              "V4PayloadTimeSync layout (meshtime::packFrame writes these offsets)");

// Topology request payload
struct __attribute__((packed)) V4PayloadTopoReq {
  uint32_t reqId;       // 0  Request ID for correlation
  uint8_t  reserved[4]; // 4  sent 0, ignored
};
static_assert(sizeof(V4PayloadTopoReq) == 8, "V4PayloadTopoReq layout");
static_assert(offsetof(V4PayloadTopoReq, reqId) == 0 && offsetof(V4PayloadTopoReq, reserved) == 4,
              "V4PayloadTopoReq layout");

// Topology start payload (first message in topology response). An answer:
// {reqId, status} first (rule F3); status is always 0 today.
struct __attribute__((packed)) V4PayloadTopoStart {
  uint32_t reqId;       // 0  the TOPO_REQ's reqId
  uint8_t  status;      // 4  0 ok (espnowanswer)
  uint8_t  peerCount;   // 5  Number of TOPO_PEER messages to follow
  uint8_t  reserved[2]; // 6  sent 0, ignored
};
static_assert(sizeof(V4PayloadTopoStart) == 8, "V4PayloadTopoStart layout");
static_assert(offsetof(V4PayloadTopoStart, reqId) == 0 && offsetof(V4PayloadTopoStart, status) == 4 &&
              offsetof(V4PayloadTopoStart, peerCount) == 5 && offsetof(V4PayloadTopoStart, reserved) == 6,
              "V4PayloadTopoStart layout");

// Topology peer entry (one per peer). An answer: {reqId, status} first.
struct __attribute__((packed)) V4PayloadTopoPeer {
  uint32_t reqId;       // 0   the TOPO_REQ's reqId
  uint8_t  status;      // 4   0 ok (espnowanswer)
  uint8_t  peerIndex;   // 5   Which peer (0-based)
  uint8_t  isLast;      // 6   1 if this is the last peer
  uint8_t  mac[6];      // 7   Peer MAC
  int8_t   rssi;        // 13  Last RSSI
  uint8_t  encrypted;   // 14  1 if encrypted
  char     name[32];    // 15  Peer name (System_DeviceName.h rule; empty = no name, the receiver shows the MAC)
};
static_assert(sizeof(V4PayloadTopoPeer) == 47, "V4PayloadTopoPeer layout");
static_assert(offsetof(V4PayloadTopoPeer, reqId) == 0 && offsetof(V4PayloadTopoPeer, status) == 4 &&
              offsetof(V4PayloadTopoPeer, peerIndex) == 5 &&
              offsetof(V4PayloadTopoPeer, isLast) == 6 && offsetof(V4PayloadTopoPeer, mac) == 7 &&
              offsetof(V4PayloadTopoPeer, rssi) == 13 && offsetof(V4PayloadTopoPeer, encrypted) == 14 &&
              offsetof(V4PayloadTopoPeer, name) == 15, "V4PayloadTopoPeer layout");
static_assert(sizeof(V4PayloadTopoPeer::name) == devicename::kFieldLen, "TOPO_PEER name field");

#if ENABLE_BONDED_MODE
// Bond mode heartbeat payload (lightweight)
struct __attribute__((packed)) V4PayloadBondHeartbeat {
  uint8_t  role;           // 0   Bond role (master/worker)
  int8_t   rssi;           // 1   WiFi RSSI
  uint8_t  reserved[2];    // 2   sent 0, ignored
  uint32_t uptimeSec;      // 4   Uptime in seconds
  uint32_t freeHeap;       // 8   Free heap bytes
  uint32_t reserved2;      // 12  sent 0, ignored (was seqNum, which no receiver read)
  uint32_t bootCounter;    // 16  Persistent boot counter
  uint32_t settingsHash;   // 20  Hash of local settings (exclude passwords)
};
static_assert(sizeof(V4PayloadBondHeartbeat) == 24, "V4PayloadBondHeartbeat layout");
static_assert(offsetof(V4PayloadBondHeartbeat, role) == 0 && offsetof(V4PayloadBondHeartbeat, rssi) == 1 &&
              offsetof(V4PayloadBondHeartbeat, reserved) == 2 && offsetof(V4PayloadBondHeartbeat, uptimeSec) == 4 &&
              offsetof(V4PayloadBondHeartbeat, freeHeap) == 8 && offsetof(V4PayloadBondHeartbeat, reserved2) == 12 &&
              offsetof(V4PayloadBondHeartbeat, bootCounter) == 16 &&
              offsetof(V4PayloadBondHeartbeat, settingsHash) == 20, "V4PayloadBondHeartbeat layout");

// (BOND_SENSOR_DATA carries V4PayloadSensorReading, below the bond block: one
// reading layout for the mesh frame 154 and the bond frame 179.)

// Stream control payload for bond mode (master -> worker)
struct __attribute__((packed)) V4PayloadStreamCtrl {
  uint8_t sensorType;   // 0  RemoteSensorType enum value
  uint8_t enable;       // 1  1 = start streaming, 0 = stop streaming
  uint8_t reserved[2];  // 2  sent 0, ignored
};
static_assert(sizeof(V4PayloadStreamCtrl) == 4, "V4PayloadStreamCtrl layout");
static_assert(offsetof(V4PayloadStreamCtrl, sensorType) == 0 && offsetof(V4PayloadStreamCtrl, enable) == 1 &&
              offsetof(V4PayloadStreamCtrl, reserved) == 2, "V4PayloadStreamCtrl layout");

// ---- Bond agreement (BOND_REQUEST / BOND_NOTICE) ------------------------------
// A bond is a setting on each device; these two frames let the devices tell
// each other how it stands (System_ESPNow_BondAgreeCore.h has the rules).
// Neither changes the receiver's bond settings. Both rows are REQ_PAIRED +
// REQ_SESSION_ENC without REQ_BOND_PEER: the receiver may not have the sender
// as its partner, which is what the frames are there to say.

// BOND_REQUEST flags.
enum BondRequestFlags : uint8_t {
  BOND_REQUEST_FLAG_FRESH = 0x01,        // the bond was just set up or the session just came up
                                         // (clear on the once-a-minute repeats)
  BOND_REQUEST_FLAG_ROLE_CHOSEN = 0x02,  // the sender's user chose its role (bondrole): a partner
                                         // that did not choose takes the other role
  BOND_REQUEST_FLAG_ROLE_YIELDS = 0x04,  // the sender follows that rule itself (set on every request
                                         // of firmware with role choice; older firmware never yields)
};

// BOND_REQUEST: "I have you configured as my bond partner."
struct __attribute__((packed)) V4PayloadBondRequest {
  uint8_t  role;         // sender's bond role (0 worker, 1 master)
  uint8_t  flags;        // BondRequestFlags
  uint16_t seq;          // request number, never 0, counted up from 1 since boot (16 bit, goes
                         // round). The answering BOND_NOTICE carries it back; the sender's ENDED
                         // names the last one it gave out
  uint8_t  reserved[4];  // 4  sent 0, ignored
};
static_assert(sizeof(V4PayloadBondRequest) == 8, "V4PayloadBondRequest is 8 bytes on the wire");
static_assert(offsetof(V4PayloadBondRequest, role) == 0 && offsetof(V4PayloadBondRequest, flags) == 1 &&
              offsetof(V4PayloadBondRequest, seq) == 2 && offsetof(V4PayloadBondRequest, reserved) == 4,
              "V4PayloadBondRequest layout");

// BOND_NOTICE reasons. A receiver ignores a value it does not know, so new
// ones can be added; never reuse a number.
enum BondNoticeReason : uint8_t {
  BOND_NOTICE_ACCEPTED         = 1,  // you are my bond partner too
  BOND_NOTICE_NOT_BONDED       = 2,  // bond mode is off here, or no partner is set
  BOND_NOTICE_BONDED_ELSEWHERE = 3,  // my partner is another device (which one is not said)
  BOND_NOTICE_ENDED            = 4,  // I just ended our bond (disconnect, partner change, bond mode off, unpair)
  BOND_NOTICE_ROLE_CONFLICT    = 5,  // you are my partner too, but we both have the same role
};

// BOND_NOTICE: how the sender's bond stands toward the receiver.
struct __attribute__((packed)) V4PayloadBondNotice {
  uint8_t  reason;       // BondNoticeReason
  uint8_t  role;         // sender's bond role (0 worker, 1 master) with ACCEPTED, ROLE_CONFLICT
                         // and ENDED; 0 with NOT_BONDED and BONDED_ELSEWHERE, which go to
                         // peers that are not the partner and must not hint at who is
  uint16_t seq;          // the receiver's BOND_REQUEST this answers; 0 when nobody asked.
                         // With ENDED, which is never an answer: the sender's own last
                         // BOND_REQUEST number (0: it sent none). Its requests up to that
                         // number were made before it ended the bond; the receiver ignores
                         // one that arrives after the ENDED
  uint8_t  flags;        // BondNoticeFlags, with the role only (ACCEPTED, ROLE_CONFLICT,
                         // ENDED); 0 otherwise. Was reserved (0): firmware from before it
                         // reads "not chosen"
  uint8_t  reserved[3];  // 5  sent 0, ignored
};
static_assert(sizeof(V4PayloadBondNotice) == 8, "V4PayloadBondNotice is 8 bytes on the wire");
static_assert(offsetof(V4PayloadBondNotice, reason) == 0 && offsetof(V4PayloadBondNotice, role) == 1 &&
              offsetof(V4PayloadBondNotice, seq) == 2 && offsetof(V4PayloadBondNotice, flags) == 4 &&
              offsetof(V4PayloadBondNotice, reserved) == 5, "V4PayloadBondNotice layout");

// BOND_NOTICE flags.
enum BondNoticeFlags : uint8_t {
  BOND_NOTICE_FLAG_ROLE_CHOSEN = 0x01,  // the sender's user chose the role the notice carries
};
#endif // ENABLE_BONDED_MODE

// BOND_CAP_RESP (172): the capability summary a bond partner sends on bonding
// (buildCapabilitySummary). Declared in every build (a value type); only bond
// builds send or receive it.
struct __attribute__((packed)) CapabilitySummary {
  uint8_t  reserved;           // 0   sent 0, ignored (was protoVersion, sent 1 and never read)
  uint8_t  role;               // 1   0=worker, 1=master
  uint8_t  inputDeviceType;    // 2   INPUT_DEVICE_TYPE: 0 none, 1 Seesaw gamepad, 2 ANO encoder, 3 GPIO encoder, 4 GT911 touch, 5 IMU taps and tilt
  uint8_t  chipModel;          // 3   esp_chip_info model
  uint32_t featureMask;        // 4   compile-time features (CAP_FEATURE_*)
  uint32_t serviceMask;        // 8   runtime services (CAP_SERVICE_*)
  uint32_t sensorMask;         // 12  sensors (CAP_SENSOR_*), 32 bits
  uint8_t  mac[6];             // 16  device MAC address
  uint8_t  wifiChannel;        // 22  current Wi-Fi channel
  uint8_t  reserved0;          // 23  sent 0, ignored
  uint32_t flashSizeMb;        // 24
  uint32_t psramSizeMb;        // 28
  uint32_t uptimeSeconds;      // 32
  uint8_t  fwHash[16];         // 36  first 16 bytes of the firmware SHA-256
  char     deviceName[32];     // 52  device name (System_DeviceName.h): devicename::putField / getField
};
static_assert(sizeof(CapabilitySummary) == 84, "CapabilitySummary layout");
static_assert(offsetof(CapabilitySummary, reserved) == 0 && offsetof(CapabilitySummary, role) == 1 &&
              offsetof(CapabilitySummary, inputDeviceType) == 2 && offsetof(CapabilitySummary, chipModel) == 3 &&
              offsetof(CapabilitySummary, featureMask) == 4 && offsetof(CapabilitySummary, serviceMask) == 8 &&
              offsetof(CapabilitySummary, sensorMask) == 12 && offsetof(CapabilitySummary, mac) == 16 &&
              offsetof(CapabilitySummary, wifiChannel) == 22 && offsetof(CapabilitySummary, reserved0) == 23 &&
              offsetof(CapabilitySummary, flashSizeMb) == 24 && offsetof(CapabilitySummary, psramSizeMb) == 28 &&
              offsetof(CapabilitySummary, uptimeSeconds) == 32 && offsetof(CapabilitySummary, fwHash) == 36 &&
              offsetof(CapabilitySummary, deviceName) == 52, "CapabilitySummary layout");

// BOND_STATUS_RESP (176): the periodic bond status snapshot, the answer to
// BOND_STATUS_REQ (~30 s). The masks use the CAP_SENSOR_* bits of
// CapabilitySummary, 32 bits. Declared in every build (a value type).
struct __attribute__((packed)) BondPeerStatus {
  uint32_t uptimeSec;          // 0   device uptime in seconds
  uint32_t freeHeap;           // 4   free heap bytes
  uint32_t minFreeHeap;        // 8   minimum free heap since boot
  uint32_t sensorEnabledMask;  // 12  CAP_SENSOR_* bits for sensors currently running
  uint32_t sensorConnectedMask;// 16  CAP_SENSOR_* bits for sensors currently connected
  uint8_t  wifiConnected;      // 20  Wi-Fi STA connected (0/1)
  uint8_t  bluetoothActive;    // 21  Bluetooth active (0/1)
  uint8_t  httpActive;         // 22  HTTP server running (0/1)
  uint8_t  bondHandshakeState; // 23  current handshake state
  uint8_t  reserved[4];        // 24  sent 0, ignored
};
static_assert(sizeof(BondPeerStatus) == 28, "BondPeerStatus layout");
static_assert(offsetof(BondPeerStatus, uptimeSec) == 0 && offsetof(BondPeerStatus, freeHeap) == 4 &&
              offsetof(BondPeerStatus, minFreeHeap) == 8 && offsetof(BondPeerStatus, sensorEnabledMask) == 12 &&
              offsetof(BondPeerStatus, sensorConnectedMask) == 16 && offsetof(BondPeerStatus, wifiConnected) == 20 &&
              offsetof(BondPeerStatus, bluetoothActive) == 21 && offsetof(BondPeerStatus, httpActive) == 22 &&
              offsetof(BondPeerStatus, bondHandshakeState) == 23 && offsetof(BondPeerStatus, reserved) == 24,
              "BondPeerStatus layout");

// ---- Sensor frames -----------------------------------------------------------
// One numbering of the sensor types everywhere: RemoteSensorType
// (System_ESPNow_SensorCore.h). A mask bit is espnowsensor::sensorBit(type).
// Every frame below fits one frame through a relay (146 bytes of plaintext).

// SENSOR_STATUS (151): which sensors the sender has and which it is streaming.
// A session unicast to the device that receives the sender's readings (row:
// paired + session-encrypted), sent every 5 s while a stream runs and when a
// stream starts or stops. Bits of types the receiver does not know are ignored.
// A bond worker sends the same 16 bytes to its bond master only, as
// BOND_SENSOR_STATUS (185); its bond master may forward them to the master
// named on it with the link byte set (System_ESPNow_ForwardCore.h,
// packStatusFrame / parseStatusFrame).
struct __attribute__((packed)) V4PayloadSensorStatus {
  uint32_t presentMask;    // 0   bit i: sensor type i is connected on the sender
  uint32_t streamingMask;  // 4   bit i: the sender is sending readings of type i
  uint16_t bootTag;        // 8   as in the reading header
  uint8_t  fwdLink;        // 10  0, or the bond master's link number: the sender's bond partner's status
  uint8_t  reserved[5];    // 11  0
};
static_assert(sizeof(V4PayloadSensorStatus) == 16 && sizeof(V4PayloadSensorStatus) == espnowfwd::kStatusFrameLen,
              "V4PayloadSensorStatus is 16 bytes on the wire");
static_assert(offsetof(V4PayloadSensorStatus, presentMask) == 0 && offsetof(V4PayloadSensorStatus, streamingMask) == 4 && offsetof(V4PayloadSensorStatus, bootTag) == 8 &&
              offsetof(V4PayloadSensorStatus, fwdLink) == 10 && offsetof(V4PayloadSensorStatus, reserved) == 11,
              "V4PayloadSensorStatus layout (espnowfwd::packStatusFrame writes these offsets)");

// The header of one part of a sensor reading: SENSOR_ENVELOPE (154), and
// BOND_SENSOR_DATA (179) to a bond partner. The part's bytes of the reading's
// JSON text follow it, with no terminator; their length is the frame's payload
// length minus 12. System_ESPNow_SensorCore.h has the rules and the meaning of
// every field; the code packs and parses with espnowsensor::packReadingHeader
// / parseReadingHeader, and this struct states the layout.
// Fixed header: cannot grow (see the frame rules at the top of this file).
struct __attribute__((packed)) V4PayloadSensorReading {
  uint8_t  sensorType;  // 0   RemoteSensorType
  uint8_t  flags;       // 1   bit 0: ageMs is filled; bit 1: first reading of a stream to this target;
                        //     bit 2: forwarded (the sender's bond partner's reading)
  uint16_t bootTag;     // 2   the same for every frame of one boot of the sender
  uint16_t counter;     // 4   the sender's count of readings of this sensor (1 first, 0 after 65535)
  uint8_t  parts;       // 6   low nibble: this part's index; high nibble: number of parts (1 to 4)
  uint8_t  fwdLink;     // 7   0, or the bond master's link number (set if and only if bit 2 is)
  uint32_t ageMs;       // 8   how old the reading was when sent; 0 today
};
static_assert(sizeof(V4PayloadSensorReading) == 12 &&
              sizeof(V4PayloadSensorReading) == espnowsensor::kReadingHeaderLen,
              "V4PayloadSensorReading is 12 bytes on the wire");
static_assert(offsetof(V4PayloadSensorReading, sensorType) == 0 && offsetof(V4PayloadSensorReading, flags) == 1 &&
              offsetof(V4PayloadSensorReading, fwdLink) == 7 &&
              offsetof(V4PayloadSensorReading, bootTag) == 2 && offsetof(V4PayloadSensorReading, counter) == 4 &&
              offsetof(V4PayloadSensorReading, parts) == 6 && offsetof(V4PayloadSensorReading, ageMs) == 8,
              "V4PayloadSensorReading layout (espnowsensor::packReadingHeader writes these offsets)");

#define SENSOR_READING_HEADER_LEN       12
#define SENSOR_READING_MAX_PARTS        4
#define SENSOR_READING_PART_MAX_DIRECT  (ESPNOW_V4_MAX_PLAINTEXT - SENSOR_READING_HEADER_LEN)              // 190
#define SENSOR_READING_PART_MAX_ROUTED  (ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT - SENSOR_READING_HEADER_LEN)  // 134
// The largest reading, the same on a direct and a routed path.
#define SENSOR_READING_MAX_BYTES        (SENSOR_READING_MAX_PARTS * SENSOR_READING_PART_MAX_ROUTED)        // 536
static_assert(SENSOR_READING_PART_MAX_DIRECT == 190 && SENSOR_READING_PART_MAX_ROUTED == 134 &&
              SENSOR_READING_MAX_BYTES == 536, "sensor reading budgets (docs/APP_JSON_CONTRACT.md)");
static_assert(espnowsensor::kFrameCapDirect == ESPNOW_V4_MAX_PLAINTEXT &&
              espnowsensor::kFrameCapRouted == ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT &&
              espnowsensor::kReadingMaxBytes == SENSOR_READING_MAX_BYTES &&
              espnowsensor::kReadingMaxParts == SENSOR_READING_MAX_PARTS,
              "System_ESPNow_SensorCore.h plans parts with the frame budgets of this file");
static_assert(SENSOR_READING_HEADER_LEN + SENSOR_READING_PART_MAX_ROUTED == ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT,
              "a part of a reading fills one routed frame exactly");

// Sensor request payload (master -> worker): the secure fetcher's leased pull.
// Mesh-legal (NOT bond-gated) — registered REQ_PAIRED|REQ_SESSION_ENC and authorized
// in-handler by Ed25519 fingerprint against the worker's configured master/backup.
// With fwdLink set the request is for the receiver's bond partner: the
// receiver, its bond master, turns it into its own bond-link control of the
// partner's sensors (System_ESPNow_ForwardCore.h, sensorReqDecide); a device
// never applies such a request to its own sensors.
// reqId is 32 bits like every other request number (rule F3): an answer to a
// sensor request, when one is added, carries it back in its {reqId, status}.
// 1.0 has no such answer (the readings and SENSOR_STATUS follow instead).
struct __attribute__((packed)) V4PayloadSensorReq {
  uint32_t reqId;       // 0   the sender's request number (nonzero; generateMessageId)
  uint8_t  mode;        // 4   0=subscribe, 1=unsubscribe, 2=oneshot (see SensorReqMode)
  uint8_t  fwdLink;     // 5   0, or the receiver's link number: a request for the receiver's bond partner
  uint8_t  reserved[2]; // 6   0
  uint32_t sensorMask;  // 8   bitmask over RemoteSensorType (bit i = 1u<<i)
  uint32_t intervalMs;  // 12  desired per-sensor cadence hint (worker floors at spec.minIntervalMs)
  uint32_t leaseMs;     // 16  lease duration (ms); must exceed the renewal cadence (e.g. 90000 for a 30s tick)
};
static_assert(sizeof(V4PayloadSensorReq) == 20, "V4PayloadSensorReq is 20 bytes on the wire");
static_assert(offsetof(V4PayloadSensorReq, reqId) == 0 && offsetof(V4PayloadSensorReq, mode) == 4 &&
              offsetof(V4PayloadSensorReq, fwdLink) == 5 && offsetof(V4PayloadSensorReq, reserved) == 6 &&
              offsetof(V4PayloadSensorReq, sensorMask) == 8 && offsetof(V4PayloadSensorReq, intervalMs) == 12 &&
              offsetof(V4PayloadSensorReq, leaseMs) == 16,
              "V4PayloadSensorReq layout");

// POWER_STATUS (152): the sender's battery state. A session unicast to each
// master named on the sender (row: paired + session-encrypted, no bond gate),
// sent with an acknowledgement request: two that nobody acknowledges clear a
// session the master no longer has. System_ESPNow_PowerEventCore.h has the
// rules and the meaning of every field; the code packs and parses with
// espnowsensor::packPowerFrame / parsePowerFrame, and this struct states the
// layout.
struct __attribute__((packed)) V4PayloadPowerStatus {
  uint16_t counter;      // 0   the sender's count of battery frames (1 first, 0 after 65535)
  uint16_t bootTag;      // 2   as in the reading header
  uint8_t  status;       // 4   BatteryStatus
  uint8_t  backend;      // 5   0 none, 1 ADC, 2 fuel gauge
  uint16_t known;        // 6   which of the values below are valid, and the yes/no states
  uint16_t voltageMv;    // 8
  uint16_t percentX10;   // 10  0 to 1000
  int16_t  rateX10;      // 12  percent per hour x 10
  uint16_t sampleAgeMs;  // 14  age of the battery sample when sent; 65535: that or more, or none
  uint8_t  fwdLink;      // 16  0, or the bond master's link number: the sender's bond partner's frame
  uint8_t  reserved[7];  // 17  0
};
static_assert(sizeof(V4PayloadPowerStatus) == 24 && sizeof(V4PayloadPowerStatus) == espnowsensor::kPowerFrameLen,
              "V4PayloadPowerStatus is 24 bytes on the wire");
static_assert(offsetof(V4PayloadPowerStatus, counter) == 0 && offsetof(V4PayloadPowerStatus, bootTag) == 2 && offsetof(V4PayloadPowerStatus, status) == 4 &&
              offsetof(V4PayloadPowerStatus, backend) == 5 && offsetof(V4PayloadPowerStatus, known) == 6 &&
              offsetof(V4PayloadPowerStatus, voltageMv) == 8 && offsetof(V4PayloadPowerStatus, percentX10) == 10 &&
              offsetof(V4PayloadPowerStatus, rateX10) == 12 && offsetof(V4PayloadPowerStatus, sampleAgeMs) == 14 &&
              offsetof(V4PayloadPowerStatus, fwdLink) == 16 && offsetof(V4PayloadPowerStatus, reserved) == 17,
              "V4PayloadPowerStatus layout (espnowsensor::packPowerFrame writes these offsets)");

// EVENT_PUSH (132): one system event of the sender, with its text cut to fit
// one frame through a relay. A session unicast to each master named on the
// sender (row: paired + session-encrypted, no bond gate). The kind travels as
// its name. The text fields are zero padded and have no terminator when full;
// the receiver trusts none of them to be text.
struct __attribute__((packed)) V4PayloadEventPush {
  uint16_t counter;      // 0    the sender's count of event frames (1 first, 0 after 65535)
  uint16_t bootTag;      // 2    as in the reading header
  uint8_t  flags;        // 4    bit 0 subject cut, bit 1 detail cut, bit 2 who cut, bit 3 kind cut,
                         //      bit 4 forwarded (the sender's bond partner's event)
  uint8_t  tier;         // 5    0 verbose, 1 standard, 2 alert
  uint8_t  source;       // 6    which interface caused the event (0 to 7)
  uint8_t  fwdLink;      // 7    0, or the bond master's link number (set if and only if bit 4 is)
  uint16_t skipped;      // 8    events the sender left out since the last one it sent
  uint16_t reserved1;    // 10   0
  uint32_t eventSeq;     // 12   the event's number on the sender (#N of its events.log)
  uint32_t ageMs;        // 16   how old the event was when sent
  char     kind[32];     // 20
  char     subject[32];  // 52
  char     detail[32];   // 84
  char     who[12];      // 116
};
static_assert(sizeof(V4PayloadEventPush) == 128 && sizeof(V4PayloadEventPush) == espnowsensor::kEventFrameLen,
              "V4PayloadEventPush is 128 bytes on the wire");
static_assert(offsetof(V4PayloadEventPush, counter) == 0 && offsetof(V4PayloadEventPush, bootTag) == 2 &&
              offsetof(V4PayloadEventPush, fwdLink) == 7 && offsetof(V4PayloadEventPush, reserved1) == 10 &&
              offsetof(V4PayloadEventPush, flags) == 4 && offsetof(V4PayloadEventPush, tier) == 5 &&
              offsetof(V4PayloadEventPush, source) == 6 && offsetof(V4PayloadEventPush, skipped) == 8 &&
              offsetof(V4PayloadEventPush, eventSeq) == 12 && offsetof(V4PayloadEventPush, ageMs) == 16 &&
              offsetof(V4PayloadEventPush, kind) == 20 && offsetof(V4PayloadEventPush, subject) == 52 &&
              offsetof(V4PayloadEventPush, detail) == 84 && offsetof(V4PayloadEventPush, who) == 116,
              "V4PayloadEventPush layout (espnowsensor::packEventFrame writes these offsets)");

// FORWARD_LINK (155): a bond master tells a master named on it that it
// forwards its bond partner's readings, battery state and events (announce),
// that it no longer does (withdraw), and the master answers or asks again
// (query). A session unicast both ways (row: paired + session-encrypted, no
// bond gate: the receiver is not the sender's bond partner). Through a relay
// in one frame. System_ESPNow_ForwardCore.h has the rules and the meaning of
// every field; the code packs and parses with espnowfwd::packLinkFrame /
// parseLinkFrame, and this struct states the layout.
struct __attribute__((packed)) V4PayloadForwardLink {
  uint8_t  op;              // 0   1 announce, 2 withdraw, 3 query, 4 answer
  uint8_t  linkGen;         // 1   the bond master's link number (1..255)
  uint16_t viaBootTag;      // 2   the bond master's boot tag
  uint8_t  streams;         // 4   bit 0 readings, bit 1 battery, bit 2 events
  uint8_t  result;          // 5   answer: 1 accepted ... 9 name_taken
  uint8_t  answers;         // 6   answer: the op answered
  uint8_t  announceGen;     // 7   announce: the bond master's announcement number (1..255), echoed by the answer
  uint8_t  partnerMac[6];   // 8   the bond partner; all zero: "for nobody" (withdraw) or a query
  uint16_t reserved1;       // 14  sent 0, ignored
  uint32_t sensorMask;      // 16  the sensors forwarded (32 bits)
  uint8_t  partnerKey[32];  // 20  the partner's Ed25519 public key as the bond master holds it
  char     partnerName[32]; // 52  the partner's device name (System_DeviceName.h; empty = none, shown as its MAC)
  uint8_t  reserved2[12];   // 84  sent 0, ignored
};
static_assert(sizeof(V4PayloadForwardLink) == 96 && sizeof(V4PayloadForwardLink) == espnowfwd::kLinkFrameLen,
              "V4PayloadForwardLink is 96 bytes on the wire");
static_assert(offsetof(V4PayloadForwardLink, op) == 0 && offsetof(V4PayloadForwardLink, announceGen) == 7 &&
              offsetof(V4PayloadForwardLink, linkGen) == 1 && offsetof(V4PayloadForwardLink, viaBootTag) == 2 &&
              offsetof(V4PayloadForwardLink, streams) == 4 && offsetof(V4PayloadForwardLink, result) == 5 &&
              offsetof(V4PayloadForwardLink, answers) == 6 && offsetof(V4PayloadForwardLink, partnerMac) == 8 &&
              offsetof(V4PayloadForwardLink, reserved1) == 14 &&
              offsetof(V4PayloadForwardLink, sensorMask) == espnowfwd::kLinkOffMask &&
              offsetof(V4PayloadForwardLink, partnerKey) == espnowfwd::kLinkOffKey &&
              offsetof(V4PayloadForwardLink, partnerName) == espnowfwd::kLinkOffName &&
              offsetof(V4PayloadForwardLink, reserved2) == 84 &&
              espnowfwd::kLinkOffMask == 16 && espnowfwd::kLinkOffKey == 20 && espnowfwd::kLinkOffName == 52,
              "V4PayloadForwardLink layout (espnowfwd::packLinkFrame writes these offsets)");

// Every frame of this block and TIME_SYNC travel inside a relay envelope
// unchanged, in one frame each.
static_assert(sizeof(V4PayloadTimeSync) <= ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT &&
              sizeof(V4PayloadSensorStatus) <= ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT &&
              sizeof(V4PayloadSensorReq) <= ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT &&
              sizeof(V4PayloadPowerStatus) <= ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT &&
              sizeof(V4PayloadEventPush) <= ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT &&
              sizeof(V4PayloadForwardLink) <= ESPNOW_V4_RELAY_MAX_INNER_PLAINTEXT,
              "the time, sensor, battery, event and forward-link frames must fit one routed frame");

// Metadata payload for metadata exchange (REQ/RESP/PUSH)
// Trimmed tags 64→54 so the struct fits ESPNOW_V4_MAX_PLAINTEXT
// (202 B) for future SESSION_FRAME wrapping. Total now exactly 202.
// It is exactly the sealed single-frame budget (202 B), so it cannot grow.
struct __attribute__((packed)) V4PayloadMetadata {
  char    deviceName[32];    // 0   the sender's device name (System_DeviceName.h); NUL always present, rest 0
  char    friendlyName[48];  // 32
  char    room[32];          // 80
  char    zone[32];          // 112
  char    tags[54];          // 144
  uint8_t stationary;        // 198
  uint8_t reserved[3];       // 199  sent 0, ignored
};
static_assert(sizeof(V4PayloadMetadata) == ESPNOW_V4_MAX_PLAINTEXT && sizeof(V4PayloadMetadata) == 202,
              "V4PayloadMetadata is exactly one SESSION_FRAME plaintext budget (202 B)");
static_assert(offsetof(V4PayloadMetadata, deviceName) == 0 && offsetof(V4PayloadMetadata, friendlyName) == 32 &&
              offsetof(V4PayloadMetadata, room) == 80 && offsetof(V4PayloadMetadata, zone) == 112 &&
              offsetof(V4PayloadMetadata, tags) == 144 && offsetof(V4PayloadMetadata, stationary) == 198 &&
              offsetof(V4PayloadMetadata, reserved) == 199, "V4PayloadMetadata layout");
static_assert(sizeof(V4PayloadMetadata::deviceName) == devicename::kFieldLen &&
              sizeof(V4PayloadForwardLink::partnerName) == devicename::kFieldLen,
              "every frame that carries the device name has a 32-byte field");

// CMD_RESP (51): the answer to a CMD (and to USER_SYNC): {reqId, status},
// then the command's output text to the end of the payload, with no
// terminator (a receiver strips one trailing NUL that a sender may still
// add). reqId is the CMD's header msgId; the CMD_RESP has its own. status:
// 0 ok; busy, refused, unsupported, too_large, no_slot, bad_request,
// not_ready (shared);
// 16 failed: the command ran and reported a failure. Larger outputs go through
// v4_send_payload_smart, which seals and fragments them.
// Fixed header: cannot grow (see the frame rules at the top of this file).
struct __attribute__((packed)) V4PayloadCmdRespHeader {
  uint32_t reqId;            // 0  the CMD's header msgId
  uint8_t  status;           // 4  espnowanswer: 0 ok, 1-15 shared, 16 failed
};
static_assert(sizeof(V4PayloadCmdRespHeader) == 5, "V4PayloadCmdRespHeader layout");
static_assert(offsetof(V4PayloadCmdRespHeader, reqId) == 0 && offsetof(V4PayloadCmdRespHeader, status) == 4,
              "V4PayloadCmdRespHeader layout");

// STREAM (90): one line of a remote command's output while it runs (and the
// startstream live output): {reqId, status}, then the line to the end of the
// payload, no terminator. reqId is the CMD's header msgId, 0 for startstream
// output, which answers nothing; status is 0. Each STREAM frame has its own
// header msgId (they used to all carry the command's, which forced the
// duplicate check to skip them).
// Fixed header: cannot grow (see the frame rules at the top of this file).
struct __attribute__((packed)) V4PayloadStreamHeader {
  uint32_t reqId;            // 0  the CMD's header msgId; 0 = startstream output
  uint8_t  status;           // 4  0
};
static_assert(sizeof(V4PayloadStreamHeader) == 5, "V4PayloadStreamHeader layout");
static_assert(offsetof(V4PayloadStreamHeader, reqId) == 0 && offsetof(V4PayloadStreamHeader, status) == 4,
              "V4PayloadStreamHeader layout");

// ---- KEY_EX handshake payloads ---------------------------------
//
// Three-way handshake between two peers in the same mesh. Each frame carries a
// 16-byte HMAC-SHA256 tag keyed by the mesh bootstrap key (Blake2b subkey of
// the PBKDF2-stretched passphrase, System_ESPNow_MeshKeys), with a label per
// frame type, so only peers that know the passphrase can produce one and a
// HELLO tag never verifies as a REPLY. The exact bytes are in
// System_ESPNow_HandshakeCore.h (pinned by test_espnow_handshake_kat.py).
//
// Flow:
//   A -> B:  KEY_EX_HELLO   (A's key, a fresh nonce, tag)
//   B -> A:  KEY_EX_REPLY   (B's key, B's fresh nonce, tag that also covers A's
//                            MAC, key and HELLO nonce: a recorded REPLY is useless)
//   A -> B:  KEY_EX_CONFIRM (status, keyId, tag over both MACs, both nonces and
//                            both keys). A refusal of a HELLO (status 2 or 3) is a
//                            CONFIRM from B, tagged over A's HELLO nonce and key.
// A receiver takes a REPLY only for a key exchange of its own in flight, and a
// HELLO whose nonce it already answered is a copy (ignored).
//
// On success both sides persist each other's pubkey to
//   /system/espnow/peers/<MAC>/identity.json
// SESSION establishment (X25519 ephemeral DH, AEAD keys) comes after it
// — KEY_EX only proves "this peer knows the mesh passphrase + owns
// this Ed25519 pubkey".

struct __attribute__((packed)) V4PayloadKeyExHello {
  uint16_t meshFingerprint;     // the mesh id (also in the header)
  uint8_t  senderMac[6];        // = header.origin
  uint8_t  senderPubEd25519[32];
  uint8_t  nonce[16];           // fresh random for every HELLO sent (a retry included)
  uint8_t  hmac[16];            // HandshakeCore helloInput, keyed by the mesh bootstrap key
};
static_assert(sizeof(V4PayloadKeyExHello) == 72, "V4PayloadKeyExHello layout");
static_assert(offsetof(V4PayloadKeyExHello, meshFingerprint) == 0 && offsetof(V4PayloadKeyExHello, senderMac) == 2 &&
              offsetof(V4PayloadKeyExHello, senderPubEd25519) == 8 && offsetof(V4PayloadKeyExHello, nonce) == 40 &&
              offsetof(V4PayloadKeyExHello, hmac) == 56,
              "V4PayloadKeyExHello layout");

struct __attribute__((packed)) V4PayloadKeyExReply {
  uint16_t meshFingerprint;
  uint8_t  responderMac[6];
  uint8_t  responderPubEd25519[32];
  uint8_t  nonce[16];           // the responder's own fresh nonce
  uint8_t  hmac[16];            // HandshakeCore replyInput: also covers the HELLO answered
};
static_assert(sizeof(V4PayloadKeyExReply) == 72, "V4PayloadKeyExReply layout");
static_assert(offsetof(V4PayloadKeyExReply, meshFingerprint) == 0 && offsetof(V4PayloadKeyExReply, responderMac) == 2 &&
              offsetof(V4PayloadKeyExReply, responderPubEd25519) == 8 && offsetof(V4PayloadKeyExReply, nonce) == 40 &&
              offsetof(V4PayloadKeyExReply, hmac) == 56,
              "V4PayloadKeyExReply layout");

struct __attribute__((packed)) V4PayloadKeyExConfirm {
  uint16_t meshFingerprint;
  uint8_t  confirmerMac[6];
  uint8_t  status;              // 0 = paired OK, 1 = HMAC fail, 2 = already-paired-different-key,
                                //   3 = not accepted (not paired here, or revoked)
  uint8_t  reserved;            // sent 0, ignored (covered by the tag)
  uint8_t  keyId[8];            // first 8 bytes of the sender's key (for OOB display verification)
  uint8_t  hmac[16];            // HandshakeCore confirmInput over the 18 bytes above and the exchange
};
static_assert(sizeof(V4PayloadKeyExConfirm) == 34, "V4PayloadKeyExConfirm layout");
static_assert(offsetof(V4PayloadKeyExConfirm, meshFingerprint) == 0 && offsetof(V4PayloadKeyExConfirm, confirmerMac) == 2 &&
              offsetof(V4PayloadKeyExConfirm, status) == 8 && offsetof(V4PayloadKeyExConfirm, reserved) == 9 &&
              offsetof(V4PayloadKeyExConfirm, keyId) == 10 && offsetof(V4PayloadKeyExConfirm, hmac) == 18,
              "V4PayloadKeyExConfirm layout");

// ---- SESSION establishment payloads -----------------------------
//
// Signed Ephemeral Diffie-Hellman (SIGMA-I pattern). Both sides have already
// exchanged long-term Ed25519 pubkeys via KEY_EX and have those keys
// persisted at /system/espnow/peers/<MAC>/identity.json. SESSION_OPEN/CONFIRM
// negotiates fresh ephemeral X25519 keys, signs them under the long-term
// identity, and derives forward-secret AEAD session keys.
//
// Transcripts signed (System_ESPNow_HandshakeCore.h, integers little endian):
//   OPEN:    "hw1-espnow-1-sopen" || sessionId(2) || initMac(6) || respMac(6) ||
//            ephX25519Pub(32) || nonceA(16) || sessionCaps(4) || openEpochMs(8)       = 92 bytes
//   CONFIRM: "hw1-espnow-1-sconf" || sessionId(2) || respMac(6) || initMac(6) ||
//            ephX25519Pub(32) || nonceA(16) || nonceB(16) || sessionCaps(4)           = 100 bytes
// Sign exactly those byte counts — a transcript padded to any other length
// verifies against nothing and every handshake fails.
//
// nonceA is sent in OPEN and echoed back inside the CONFIRM signature input
// — that's the freshness binding from A to B. nonceB is added on the B side.
//
// sessionCaps (1.0): the sender's capability word, signed by its owner
// (System_ESPNow_SessionCapsCore.h has the bits and the rules). The
// initiator's word is not echoed in CONFIRM: it is already bound to nonceA,
// which CONFIRM signs. The responder takes the initiator's word from an OPEN,
// whose signature covers nothing fresh from the responder: a recorded OPEN
// replayed after the responder restarted carries its old word, so the
// responder counts that word as confirmed only after the first sealed frame of
// the session opens. REKEY carries no word: the session keeps the one from its
// handshake, since a word is fixed for the sender's boot.
//
// openEpochMs (1.0, E33): the initiator's clock (UTC ms) when it signed, 0 when
// its clock is not set. Signed, sent and not checked yet: the freshness check
// for a replayed OPEN (SEC-3) comes after 1.0 without a wire change.
//
// meshTag (1.0, revocation C1): the last 16 bytes of OPEN, CONFIRM and REKEY,
// HMAC-SHA256(bootstrap key of the mesh slot the peer's identity names,
// "<transcript label>-mk" || meshId(2) || transcript || signature)[0..16). It
// binds the session to the current mesh passphrase: after a passphrase change
// a device without the new passphrase cannot open or rekey a session, even
// with modified firmware. The receiver checks it before the signature, and a
// wrong one is logged as "mesh key mismatch". The session keys mix in the same
// bootstrap key too (C2, sessionDeriveAeadKeys).
//
// These six handshake frames (KEY_EX HELLO/REPLY/CONFIRM, SESSION
// OPEN/CONFIRM/REKEY) are the one exception to "longer frames are read up to
// the known fields": each is taken only at its exact length, because its
// signature or tag covers the whole frame.
//
// Flow:
//   A → B: SESSION_OPEN    (sessionId, ephPub_A, nonceA, caps_A, openEpochMs, sig, meshTag)
//   B → A: SESSION_CONFIRM (sessionId, ephPub_B, nonceB, caps_B, sig, meshTag)
// On success both sides hold matching aeadKeyAtoB / aeadKeyBtoA derived via
// Blake2b KDF (contexts "esp-AtoB" / "esp-BtoA") from the X25519 shared secret
// under the mesh bootstrap key (HandshakeCore "session keys").

struct __attribute__((packed)) V4PayloadSessionOpen {
  uint16_t sessionId;            // initiator-chosen, 16-bit random non-zero
  uint8_t  initiatorMac[6];
  uint8_t  responderMac[6];
  uint8_t  ephX25519Pub[32];
  uint8_t  nonceA[16];
  uint32_t sessionCaps;          // the initiator's capability word (SessionCapsCore.h)
  uint64_t openEpochMs;          // the initiator's clock when signing (UTC ms), 0 = not set
  uint8_t  signature[64];        // Ed25519 over the OPEN transcript above
  uint8_t  meshTag[16];          // C1 mesh tag over the transcript and the signature
};
static_assert(sizeof(V4PayloadSessionOpen) == 154, "V4PayloadSessionOpen layout");
static_assert(offsetof(V4PayloadSessionOpen, sessionId) == 0 &&
              offsetof(V4PayloadSessionOpen, initiatorMac) == 2 &&
              offsetof(V4PayloadSessionOpen, responderMac) == 8 &&
              offsetof(V4PayloadSessionOpen, ephX25519Pub) == 14 &&
              offsetof(V4PayloadSessionOpen, nonceA) == 46 &&
              offsetof(V4PayloadSessionOpen, sessionCaps) == 62 &&
              offsetof(V4PayloadSessionOpen, openEpochMs) == 66 &&
              offsetof(V4PayloadSessionOpen, signature) == 74 &&
              offsetof(V4PayloadSessionOpen, meshTag) == 138,
              "V4PayloadSessionOpen field offsets");

struct __attribute__((packed)) V4PayloadSessionConfirm {
  uint16_t sessionId;            // mirrors initiator's choice
  uint8_t  responderMac[6];
  uint8_t  initiatorMac[6];
  uint8_t  ephX25519Pub[32];
  uint8_t  nonceA[16];           // echoed from OPEN — freshness binding
  uint8_t  nonceB[16];
  uint32_t sessionCaps;          // the responder's capability word (SessionCapsCore.h)
  uint8_t  signature[64];        // Ed25519 over the CONFIRM transcript above
  uint8_t  meshTag[16];          // C1 mesh tag over the transcript and the signature
};
static_assert(sizeof(V4PayloadSessionConfirm) == 162, "V4PayloadSessionConfirm layout");
static_assert(offsetof(V4PayloadSessionConfirm, sessionId) == 0 &&
              offsetof(V4PayloadSessionConfirm, responderMac) == 2 &&
              offsetof(V4PayloadSessionConfirm, initiatorMac) == 8 &&
              offsetof(V4PayloadSessionConfirm, ephX25519Pub) == 14 &&
              offsetof(V4PayloadSessionConfirm, nonceA) == 46 &&
              offsetof(V4PayloadSessionConfirm, nonceB) == 62 &&
              offsetof(V4PayloadSessionConfirm, sessionCaps) == 78 &&
              offsetof(V4PayloadSessionConfirm, signature) == 82 &&
              offsetof(V4PayloadSessionConfirm, meshTag) == 146,
              "V4PayloadSessionConfirm field offsets");

// ---- SESSION_REKEY (forward-secrecy rotation) -------------------
//
// Refreshes the AEAD keys without a full handshake. Used when txSeqNext
// crosses a threshold (e.g. 10k frames) or sessionAge crosses a duration
// (e.g. 1h). The long-term Ed25519 identities are reused to sign fresh
// ephemeral X25519 pubkeys; the resulting shared secret, under the mesh
// bootstrap key (C2), feeds the Blake2b KDF to produce a new aeadKeyTx/Rx pair.
//
// Two-message symmetric exchange (each side sends one REKEY containing its
// own fresh ephemeral pub):
//   A → B: SESSION_REKEY (newEphA, nonceA, sig_A, meshTag)
//   B → A: SESSION_REKEY (newEphB, nonceB, sig_B, meshTag)
// Either side can initiate. On receiving a REKEY without having sent one,
// the receiver picks its own fresh ephemeral and replies with REKEY. On
// receiving a REKEY while own REKEY is in flight (concurrent rekey race),
// the same protocol still converges — both ends derive shared from the same
// (newEphA, newEphB) pair, so they end up with identical keys regardless of
// who "started" it.
//
// Transcript signed (HandshakeCore, 84 bytes):
//   "hw1-espnow-1-rekey" || sessionId(2) || senderMac(6) || receiverMac(6) ||
//                           newEphX25519Pub(32) || nonceRekey(16) || prevTxSeqAtRekey(4)
// prevTxSeqAtRekey binds the signature to a specific point in the existing
// session's transcript — prevents replay of a stale REKEY from a previous
// rekey round. The meshTag binds it to the current mesh key: a session opened
// under a passphrase that has since changed is closed, and its REKEY refused.
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
  uint8_t  meshTag[16];          // C1 mesh tag over the transcript and the signature
};
static_assert(sizeof(V4PayloadSessionRekey) == 146, "V4PayloadSessionRekey layout");
static_assert(offsetof(V4PayloadSessionRekey, sessionId) == 0 && offsetof(V4PayloadSessionRekey, senderMac) == 2 &&
              offsetof(V4PayloadSessionRekey, receiverMac) == 8 && offsetof(V4PayloadSessionRekey, newEphX25519Pub) == 14 &&
              offsetof(V4PayloadSessionRekey, nonceRekey) == 46 && offsetof(V4PayloadSessionRekey, prevTxSeqAtRekey) == 62 &&
              offsetof(V4PayloadSessionRekey, signature) == 66 && offsetof(V4PayloadSessionRekey, meshTag) == 130,
              "V4PayloadSessionRekey layout");
static_assert(sizeof(V4PayloadSessionRekey) <= ESPNOW_V4_MAX_PAYLOAD,
              "REKEY is sent plaintext, signed under the identity key: it must fit one frame");

// File transfer payloads
struct __attribute__((packed)) V4PayloadFileStart {
  uint32_t fileSize;      // 0  Total file size in bytes
  uint16_t chunkCount;    // 4  Total number of chunks
  uint16_t chunkSize;     // 6  Size of each chunk (except last)
  char     filename[64];  // 8  Destination filename
};
static_assert(sizeof(V4PayloadFileStart) == 72, "V4PayloadFileStart layout");
static_assert(offsetof(V4PayloadFileStart, fileSize) == 0 && offsetof(V4PayloadFileStart, chunkCount) == 4 &&
              offsetof(V4PayloadFileStart, chunkSize) == 6 && offsetof(V4PayloadFileStart, filename) == 8,
              "V4PayloadFileStart layout");

// Chunk data sized for ESPNOW_V4_MAX_PLAINTEXT so encrypted
// file transfers don't silently truncate every chunk by 16 bytes. Per-chunk
// data is 200 B, and the struct also carries a
// 2 B chunkIndex: 202 B plaintext + 16 B AEAD tag = 218 = MAX_PAYLOAD EXACTLY,
// zero headroom. There is no room for another field — adding one trips the
// static_assert below rather than corrupting the wire, so shrink data[] (or
// widen the transport) instead of growing this struct.
// 2026-05-19: FILE_START/DATA/END now route through
// v4_send_payload_smart, so each chunk is session-encrypted when a session
// exists and plaintext otherwise. Old firmware reading
// V4PayloadFileStart.chunkSize adapts automatically.
// Fixed header: cannot grow (see the frame rules at the top of this file).
struct __attribute__((packed)) V4PayloadFileData {
  uint16_t chunkIndex;    // Chunk index (0-based)
  uint8_t  data[ESPNOW_V4_MAX_PLAINTEXT - 2];  // 200 bytes — fits a SESSION_FRAME
};
static_assert(sizeof(V4PayloadFileData) == ESPNOW_V4_MAX_PLAINTEXT && sizeof(V4PayloadFileData) == 202,
              "V4PayloadFileData must fit a SESSION_FRAME plaintext budget");
static_assert(offsetof(V4PayloadFileData, chunkIndex) == 0 && offsetof(V4PayloadFileData, data) == 2,
              "V4PayloadFileData layout (a chunk's bytes follow the index; the last chunk is shorter)");

struct __attribute__((packed)) V4PayloadFileEnd {
  uint32_t crc32;         // CRC32 of the entire file, computed with esp_crc32_le
                          // (seed 0) on both ends. Receiver REJECTS on mismatch
                          // (FILE_CANCEL_CRC_MISMATCH). Ignored when status != 0.
                          // Flag day 2026-07: older firmware sent 0 here; both
                          // peers must run post-CRC firmware or every non-empty
                          // transfer fails.
  uint8_t  status;        // 0 = transfer complete; anything else = the sender stopped it
                          // mid-transfer (it sends espnowanswer::kCancelled). Was
                          // `success` with 1 = complete: 0 means ok in every status
                          // byte since 1.0 (rule F3)
};
static_assert(sizeof(V4PayloadFileEnd) == 5, "V4PayloadFileEnd layout");
static_assert(offsetof(V4PayloadFileEnd, crc32) == 0 && offsetof(V4PayloadFileEnd, status) == 4,
              "V4PayloadFileEnd layout");

// FILE_CANCEL — the receiver tells the sender a transfer it started
// will not complete. An answer (rule F3): reqId is the FILE_START's header
// msgId (the transfer's number until FILE_START carries its own id); the
// FILE_CANCEL's header msgId is the canceller's own. The sender matches
// (peer, reqId) against its in-flight send and ABORTS the chunk loop, so a
// cancel stops the remaining airtime spend (see fileSendAbortMsgId).
// The reasons are status values of the shared table (espnowanswer); two are
// this frame's own (16, 17).
// The numbers are those of espnowanswer (asserted after the answer structs).
enum EspNowFileCancelReason : uint8_t {
  FILE_CANCEL_TIMEOUT      = 10,  // timeout: no frame arrived within the slot timeout
  FILE_CANCEL_WRITE_FAILED = 8,   // io_error: staging write / rename to flash failed
  FILE_CANCEL_INCOMPLETE   = 16,  // incomplete (this frame's own): FILE_END arrived but chunks were missing
  FILE_CANCEL_CRC_MISMATCH = 17,  // crc_mismatch (this frame's own): all chunks arrived but the whole-file
                                  // CRC32 didn't match FILE_END's declared value
  FILE_CANCEL_REJECTED     = 2,   // refused: FILE_START rejected (no slot / alloc / too big), or a chunk
                                  // arrived for an unknown transfer — receiver never had/lost the slot
};
struct __attribute__((packed)) V4PayloadFileCancel {
  uint32_t reqId;         // 0  the FILE_START's header msgId
  uint8_t  status;        // 4  EspNowFileCancelReason (never 0)
};
static_assert(sizeof(V4PayloadFileCancel) == 5 && offsetof(V4PayloadFileCancel, reqId) == 0 &&
              offsetof(V4PayloadFileCancel, status) == 4, "V4PayloadFileCancel layout");

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
// Request fits a single SESSION_FRAME (144 B < 202 B plaintext budget), so it
// doesn't need fragmentation. FS_* frames are never relayed
// (v4_routed_type_allowed), so only the direct budget applies. Reply is sent via
// v4_send_payload_smart which negotiates encrypted-chunked under a session.

struct __attribute__((packed)) V4PayloadFsListReq {
  uint32_t reqId;          // Client-chosen correlation ID; echoed in reply.
                           // 0 is RESERVED (sentinel for "no request" in
                           // the pending-request table). Senders MUST pick
                           // a nonzero value.
  uint16_t startIndex;     // Pagination — first entry to return (0 = start)
  uint16_t maxEntries;     // Cap; receiver may return fewer. Hard cap: 32.
  char     path[128];      // Null-terminated VFS path on receiver
  uint8_t  reserved[8];    // sent 0, ignored
};
static_assert(sizeof(V4PayloadFsListReq) == 144, "V4PayloadFsListReq layout");
static_assert(offsetof(V4PayloadFsListReq, reqId) == 0 && offsetof(V4PayloadFsListReq, startIndex) == 4 &&
              offsetof(V4PayloadFsListReq, maxEntries) == 6 && offsetof(V4PayloadFsListReq, path) == 8 &&
              offsetof(V4PayloadFsListReq, reserved) == 136, "V4PayloadFsListReq layout");
static_assert(sizeof(V4PayloadFsListReq) <= ESPNOW_V4_MAX_PLAINTEXT,
              "V4PayloadFsListReq must fit single SESSION_FRAME");

// Reply layout (variable-length, on wire):
//   [V4PayloadFsListReplyHeader] [V4PayloadFsEntry × header.entryCount]
//
// status codes (FS_LIST_REPLY, FS_STAT_REPLY and FS_GET_ACK; the answer
// table of rule F3, System_ESPNow_AnswerCore.h):
//   0  = OK
//   9  = NOT_FOUND      (shared not_found: path doesn't exist on receiver)
//   16 = NOT_A_DIR      (this frame's own: path exists but is a file)
//   2  = PERM_DENIED    (shared refused: bonded-peer identity can't read)
//   8  = IO_ERROR       (shared io_error: FS error reading dir)
//   1  = TOO_BUSY       (shared busy: receiver is in the middle of another
//                        listing; caller should retry with backoff)
//   7  = NOT_READY      (shared not_ready: filesystem not mounted yet)
//
// hasMore=1 + nextStartIndex tells the client to issue a follow-up with
// startIndex=nextStartIndex to fetch the rest of a large directory.

// The numbers are those of espnowanswer (asserted after the answer structs).
enum FsListStatus : uint8_t {
  FS_LIST_STATUS_OK          = 0,   // ok
  FS_LIST_STATUS_NOT_FOUND   = 9,   // not_found
  FS_LIST_STATUS_NOT_A_DIR   = 16,  // not_a_dir (FS answers only)
  FS_LIST_STATUS_PERM_DENIED = 2,   // refused
  FS_LIST_STATUS_IO_ERROR    = 8,   // io_error
  FS_LIST_STATUS_TOO_BUSY    = 1,   // busy
  FS_LIST_STATUS_NOT_READY   = 7,   // not_ready
};

// Fixed header: cannot grow (see the frame rules at the top of this file).
struct __attribute__((packed)) V4PayloadFsListReplyHeader {
  uint32_t reqId;          // Echoes request (answer header, rule F3)
  uint8_t  status;         // FsListStatus (the answer status table)
  uint8_t  entryCount;     // Number of V4PayloadFsEntry records that follow
  uint8_t  hasMore;        // 1 if more entries available beyond this batch
  uint8_t  reserved;       // sent 0, ignored
  uint16_t totalEntries;   // Total entries in directory (for UI "of N")
  uint16_t nextStartIndex; // Pass to follow-up request when hasMore=1
  char     path[128];      // Echoes (normalized) request path
};
static_assert(sizeof(V4PayloadFsListReplyHeader) == 140,
              "V4PayloadFsListReplyHeader layout");
static_assert(offsetof(V4PayloadFsListReplyHeader, reqId) == 0 && offsetof(V4PayloadFsListReplyHeader, status) == 4 &&
              offsetof(V4PayloadFsListReplyHeader, entryCount) == 5 && offsetof(V4PayloadFsListReplyHeader, hasMore) == 6 &&
              offsetof(V4PayloadFsListReplyHeader, reserved) == 7 &&
              offsetof(V4PayloadFsListReplyHeader, totalEntries) == 8 &&
              offsetof(V4PayloadFsListReplyHeader, nextStartIndex) == 10 &&
              offsetof(V4PayloadFsListReplyHeader, path) == 12, "V4PayloadFsListReplyHeader layout");

// One entry in a FS_LIST_REPLY. Layout intentionally matches the spirit of
// FileEntry in System_FileManager.h so the OLED render path can copy fields
// near-1:1 from the wire into its FileEntry cache.
// Fixed header: cannot grow (see the frame rules at the top of this file).
struct __attribute__((packed)) V4PayloadFsEntry {
  char     name[64];       // Null-terminated filename (no path prefix)
  uint32_t size;           // Bytes (0 for folders)
  uint8_t  isFolder;       // 0 or 1
  uint8_t  perms;          // Bitmask matching FileEntry::permissions
                           //   bit 0 = READ, bit 1 = WRITE, bit 2 = DELETE
  uint8_t  reserved[6];    // sent 0, ignored
};
static_assert(sizeof(V4PayloadFsEntry) == 76, "V4PayloadFsEntry layout");
static_assert(offsetof(V4PayloadFsEntry, name) == 0 && offsetof(V4PayloadFsEntry, size) == 64 &&
              offsetof(V4PayloadFsEntry, isFolder) == 68 && offsetof(V4PayloadFsEntry, perms) == 69 &&
              offsetof(V4PayloadFsEntry, reserved) == 70, "V4PayloadFsEntry layout");

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
  uint8_t  reserved[8];    // sent 0, ignored
};
static_assert(sizeof(V4PayloadFsStatReq) == 140, "V4PayloadFsStatReq layout");
static_assert(offsetof(V4PayloadFsStatReq, reqId) == 0 && offsetof(V4PayloadFsStatReq, path) == 4 &&
              offsetof(V4PayloadFsStatReq, reserved) == 132, "V4PayloadFsStatReq layout");
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
static_assert(offsetof(V4PayloadFsStatReply, reqId) == 0 && offsetof(V4PayloadFsStatReply, status) == 4 &&
              offsetof(V4PayloadFsStatReply, reserved1) == 5 && offsetof(V4PayloadFsStatReply, totalBytes) == 8 &&
              offsetof(V4PayloadFsStatReply, usedBytes) == 16 && offsetof(V4PayloadFsStatReply, freeBytes) == 24 &&
              offsetof(V4PayloadFsStatReply, percentUsedX10) == 32 && offsetof(V4PayloadFsStatReply, reserved2) == 34 &&
              offsetof(V4PayloadFsStatReply, path) == 36, "V4PayloadFsStatReply layout");
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
static_assert(offsetof(V4PayloadFsGetReq, reqId) == 0 && offsetof(V4PayloadFsGetReq, path) == 4 &&
              offsetof(V4PayloadFsGetReq, reserved) == 132, "V4PayloadFsGetReq layout");
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
static_assert(offsetof(V4PayloadFsGetAck, reqId) == 0 && offsetof(V4PayloadFsGetAck, status) == 4 &&
              offsetof(V4PayloadFsGetAck, reserved) == 5 && offsetof(V4PayloadFsGetAck, fileSize) == 8 &&
              offsetof(V4PayloadFsGetAck, path) == 12, "V4PayloadFsGetAck layout");
static_assert(sizeof(V4PayloadFsGetAck) <= ESPNOW_V4_MAX_PLAINTEXT,
              "V4PayloadFsGetAck must fit single SESSION_FRAME");

// ---- SESSION_UNKNOWN (opcode 18, 2026-11-18) ----------------------------------
//
// A device that receives a SESSION_FRAME for a session id it does not hold
// ACTIVE (it restarted, or dropped the session) cannot open the frame and,
// before this, dropped it silently: the sender kept sealing into a session the
// receiver no longer had until two of its own sends timed out (~10 s). Now the
// receiver answers once, naming what it could not open:
//
//   B → A: SESSION_UNKNOWN { sessionId, frameSeq, msgId } of A's frame
//
// Sent group-signed (BROADCAST_AUTH: B has no session to seal it in) under the
// tag of A's mesh (A ignores a hint under another mesh's tag), ttl 1, never
// routed or flooded, at most once per peer per second, and only to a peer
// whose identity B holds. Not sent when the id is B's own SESSION_OPEN
// awaiting its CONFIRM (A's frames can overtake the CONFIRM) or a session B
// replaced (a late frame). Row: REQ_AUTHENTICATED | REQ_PAIRED.
//
// A acts only when sessionId is its live (ACTIVE/REKEYING) session with B and
// frameSeq is one it sealed in it (1..txSeqNext); then it closes that session,
// marks msgId failed at once and starts a SESSION_OPEN. At most one such close
// per peer per 10 s (shared with the BOOT notice rule). Nothing is accepted or
// sent on the strength of the hint: the new session is the normal signed
// SESSION_OPEN/CONFIRM. A forged hint (any mesh member holds the group key)
// costs one fresh handshake per 10 s at most; a re-sent copy names a session
// that is no longer live and does nothing.
struct __attribute__((packed)) V4PayloadSessionUnknown {
  uint16_t sessionId;    // 0  the header sessionId of the frame that could not be opened
  uint8_t  reserved[2];  // 2  sent 0, ignored
  uint32_t frameSeq;     // 4  its header frameSeq
  uint32_t msgId;        // 8  its header msgId (lets the sender mark that send failed at once)
};
static_assert(sizeof(V4PayloadSessionUnknown) == 12,
              "V4PayloadSessionUnknown layout (12 B + the 16 B group tag: one frame)");
static_assert(offsetof(V4PayloadSessionUnknown, sessionId) == 0 && offsetof(V4PayloadSessionUnknown, reserved) == 2 &&
              offsetof(V4PayloadSessionUnknown, frameSeq) == 4 && offsetof(V4PayloadSessionUnknown, msgId) == 8,
              "V4PayloadSessionUnknown layout");

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
static_assert(sizeof(V4PayloadSessionConfirm) == ESPNOW_V4_RELAY_MAX_INNER_PAYLOAD &&
              sizeof(V4PayloadSessionConfirm) == 162,
              "SESSION_CONFIRM is exactly the 162-byte relay budget (1.0 design 7.4 risk 3)");
// REKEY travels plaintext, signed under the identity key (like OPEN/CONFIRM):
// 146 of the 162 bytes. SESSION_CONFIRM uses all 162 (exactly the budget): it can
// never grow, and the relay budget can never shrink.
static_assert(sizeof(V4PayloadSessionRekey) <= ESPNOW_V4_RELAY_MAX_INNER_PAYLOAD,
              "SESSION_REKEY must fit a RELAY_DATA envelope (plaintext, signed under the identity key)");

// Every name field is ESPNOW_V4_NAME_LEN bytes (the structs spell 32 so the
// host tests that copy one struct need no macro).
static_assert(sizeof(V4PayloadHeartbeat::deviceName) == ESPNOW_V4_NAME_LEN &&
              sizeof(V4PayloadPairBeacon::deviceName) == ESPNOW_V4_NAME_LEN &&
              sizeof(V4PayloadPairCtrl::deviceName) == ESPNOW_V4_NAME_LEN &&
              sizeof(V4PayloadTopoPeer::name) == ESPNOW_V4_NAME_LEN &&
              sizeof(V4PayloadForwardLink::partnerName) == ESPNOW_V4_NAME_LEN &&
              sizeof(V4PayloadMetadata::deviceName) == ESPNOW_V4_NAME_LEN &&
              sizeof(CapabilitySummary::deviceName) == ESPNOW_V4_NAME_LEN,
              "device names travel in ESPNOW_V4_NAME_LEN-byte fields");
static_assert(ESPNOW_V4_NAME_LEN == devicename::kFieldLen,
              "the wire name width is the device-name field (System_DeviceName.h)");

// ---- File catch-up (CATCHUP_*, opcodes 82-85, 2026-11-19) --------------------
//
// A master or backup master named on a worker (by key) lists the files the
// worker offers and reads byte ranges of them (docs/BACKCAPTURE_FILES.md;
// System_ESPNow_CatchupCore.h packs and parses these bytes, never by a cast).
// Single frames, session unicast, no ACK, ttl 1; correlation by reqId. The two
// requests grow at their end (minLen never rises). The two answers carry
// headerLen and a reader skips to it, so they grow too; LIST entries are
// self-sized (entryLen) and the name sits at the header's entryFixed.
struct __attribute__((packed)) V4PayloadCatchupListReq {
  uint32_t reqId;          // nonzero
  uint32_t fromFileId;     // list ids >= this, ascending; 0 starts a full run
  uint16_t kindMask;       // bit (kind-1); 0 = all
  uint8_t  link;           // 0 = own files; other values answer 16 no_link
  uint8_t  maxEntries;     // 0 = as many as fit one frame
  uint8_t  maxFrame;       // largest answer payload; 0 = the path cap (floor 64)
  uint8_t  reserved[3];
};
static_assert(sizeof(V4PayloadCatchupListReq) == 16, "V4PayloadCatchupListReq layout");
static_assert(offsetof(V4PayloadCatchupListReq, reqId) == 0 && offsetof(V4PayloadCatchupListReq, fromFileId) == 4 &&
              offsetof(V4PayloadCatchupListReq, kindMask) == 8 && offsetof(V4PayloadCatchupListReq, link) == 10 &&
              offsetof(V4PayloadCatchupListReq, maxEntries) == 11 && offsetof(V4PayloadCatchupListReq, maxFrame) == 12 &&
              offsetof(V4PayloadCatchupListReq, reserved) == 13, "V4PayloadCatchupListReq layout");

struct __attribute__((packed)) V4PayloadCatchupListRespHeader {
  uint32_t reqId;
  uint8_t  status;         // 0 ok, 1 busy (u16 retryAfterS at headerLen), 2, 3, 7, 16 no_link
  uint8_t  link;           // echoed
  uint8_t  entryCount;
  uint8_t  headerLen;      // 24
  uint32_t nextFileId;     // 0 = end of the list
  uint32_t ledgerGen;      // stepped on add, rename, trim or remove
  uint32_t ledgerId;       // random, new when the ledger is created
  uint16_t bootTag;
  uint8_t  flags;          // 0 a file is sealed, 1 files withheld, 2 older files beyond the horizon
  uint8_t  entryFixed;     // 24: the name's offset in each entry
};
static_assert(sizeof(V4PayloadCatchupListRespHeader) == 24, "V4PayloadCatchupListRespHeader layout");
static_assert(offsetof(V4PayloadCatchupListRespHeader, reqId) == 0 &&
              offsetof(V4PayloadCatchupListRespHeader, status) == 4 &&
              offsetof(V4PayloadCatchupListRespHeader, link) == 5 &&
              offsetof(V4PayloadCatchupListRespHeader, entryCount) == 6 &&
              offsetof(V4PayloadCatchupListRespHeader, headerLen) == 7 &&
              offsetof(V4PayloadCatchupListRespHeader, nextFileId) == 8 &&
              offsetof(V4PayloadCatchupListRespHeader, ledgerGen) == 12 &&
              offsetof(V4PayloadCatchupListRespHeader, ledgerId) == 16 &&
              offsetof(V4PayloadCatchupListRespHeader, bootTag) == 20 &&
              offsetof(V4PayloadCatchupListRespHeader, flags) == 22 &&
              offsetof(V4PayloadCatchupListRespHeader, entryFixed) == 23, "V4PayloadCatchupListRespHeader layout");

struct __attribute__((packed)) V4PayloadCatchupReadReq {
  uint32_t reqId;          // nonzero
  uint32_t fileId;
  uint32_t offset;         // logical
  uint32_t maxBytes;       // 0 = to the end (capped at 8 frames, or 1 MiB for a digest)
  uint8_t  link;           // as LIST
  uint8_t  mode;           // 0 data, 1 digest
  uint8_t  maxFrame;       // as LIST
  uint8_t  flags;          // bit 0 storedBefore: every byte before offset is stored (Q8)
  uint32_t ledgerId;       // must equal the device's, else 16 gone
};
static_assert(sizeof(V4PayloadCatchupReadReq) == 24, "V4PayloadCatchupReadReq layout");
static_assert(offsetof(V4PayloadCatchupReadReq, reqId) == 0 && offsetof(V4PayloadCatchupReadReq, fileId) == 4 &&
              offsetof(V4PayloadCatchupReadReq, offset) == 8 && offsetof(V4PayloadCatchupReadReq, maxBytes) == 12 &&
              offsetof(V4PayloadCatchupReadReq, link) == 16 && offsetof(V4PayloadCatchupReadReq, mode) == 17 &&
              offsetof(V4PayloadCatchupReadReq, maxFrame) == 18 && offsetof(V4PayloadCatchupReadReq, flags) == 19 &&
              offsetof(V4PayloadCatchupReadReq, ledgerId) == 20, "V4PayloadCatchupReadReq layout");

struct __attribute__((packed)) V4PayloadCatchupReadRespHeader {
  uint32_t reqId;
  uint8_t  status;         // 0 ok, 1-8 shared, 16 gone, 17 trimmed, 18 past_end
  uint8_t  flags;          // 0 last, 1 atEnd, 2 digest, 3 growing, 4 sealed on send
  uint8_t  link;           // echoed
  uint8_t  headerLen;      // 20; data starts here
  uint32_t fileId;
  uint32_t offset;         // logical offset of the first plaintext byte
  uint32_t end;            // the file's current logical end
};
static_assert(sizeof(V4PayloadCatchupReadRespHeader) == 20, "V4PayloadCatchupReadRespHeader layout");
static_assert(offsetof(V4PayloadCatchupReadRespHeader, reqId) == 0 &&
              offsetof(V4PayloadCatchupReadRespHeader, status) == 4 &&
              offsetof(V4PayloadCatchupReadRespHeader, flags) == 5 &&
              offsetof(V4PayloadCatchupReadRespHeader, link) == 6 &&
              offsetof(V4PayloadCatchupReadRespHeader, headerLen) == 7 &&
              offsetof(V4PayloadCatchupReadRespHeader, fileId) == 8 &&
              offsetof(V4PayloadCatchupReadRespHeader, offset) == 12 &&
              offsetof(V4PayloadCatchupReadRespHeader, end) == 16, "V4PayloadCatchupReadRespHeader layout");
// The core reads and writes these offsets; its numbers are this file's.
static_assert(espnowcatchup::kOpListReq == ESPNOW_V4_TYPE_CATCHUP_LIST_REQ &&
              espnowcatchup::kOpListResp == ESPNOW_V4_TYPE_CATCHUP_LIST_RESP &&
              espnowcatchup::kOpReadReq == ESPNOW_V4_TYPE_CATCHUP_READ_REQ &&
              espnowcatchup::kOpReadResp == ESPNOW_V4_TYPE_CATCHUP_READ_RESP,
              "System_ESPNow_CatchupCore.h names the CATCHUP opcodes of this file");
static_assert(espnowcatchup::kListReqLen == sizeof(V4PayloadCatchupListReq) &&
              espnowcatchup::kListRespHeaderLen == sizeof(V4PayloadCatchupListRespHeader) &&
              espnowcatchup::kReadReqLen == sizeof(V4PayloadCatchupReadReq) &&
              espnowcatchup::kReadRespHeaderLen == sizeof(V4PayloadCatchupReadRespHeader) &&
              espnowcatchup::kEntryFixedLen == 24,
              "System_ESPNow_CatchupCore.h sizes are this file's structs");
static_assert(espnowcatchup::kStOk == espnowanswer::kOk && espnowcatchup::kStBusy == espnowanswer::kBusy &&
              espnowcatchup::kStRefused == espnowanswer::kRefused &&
              espnowcatchup::kStUnsupported == espnowanswer::kUnsupported &&
              espnowcatchup::kStTooLarge == espnowanswer::kTooLarge &&
              espnowcatchup::kStBadRequest == espnowanswer::kBadRequest &&
              espnowcatchup::kStNotReady == espnowanswer::kNotReady &&
              espnowcatchup::kStIoError == espnowanswer::kIoError &&
              espnowcatchup::kStNoLink == espnowanswer::kCatchupNoLink &&
              espnowcatchup::kStGone == espnowanswer::kCatchupGone &&
              espnowcatchup::kStTrimmed == espnowanswer::kCatchupTrimmed &&
              espnowcatchup::kStPastEnd == espnowanswer::kCatchupPastEnd,
              "the CATCHUP status bytes are the shared table's (rule F3)");
static_assert(espnowcatchup::kDirectPayload <= ESPNOW_V4_MAX_PLAINTEXT,
              "a CATCHUP frame fits one SESSION_FRAME");

// ---- Answer frames (rule F3) --------------------------------------------------
// The opcodes System_ESPNow_AnswerCore.h names are these, and every answer
// struct starts with the answer header {reqId u32 at 0, status u8 at 4}.
static_assert(espnowanswer::kOpTopoStart == ESPNOW_V4_TYPE_TOPO_START &&
              espnowanswer::kOpTopoPeer == ESPNOW_V4_TYPE_TOPO_PEER &&
              espnowanswer::kOpCmdResp == ESPNOW_V4_TYPE_CMD_RESP &&
              espnowanswer::kOpFsListReply == ESPNOW_V4_TYPE_FS_LIST_REPLY &&
              espnowanswer::kOpFsStatReply == ESPNOW_V4_TYPE_FS_STAT_REPLY &&
              espnowanswer::kOpFsGetAck == ESPNOW_V4_TYPE_FS_GET_ACK &&
              espnowanswer::kOpStream == ESPNOW_V4_TYPE_STREAM &&
              espnowanswer::kOpFileCancel == ESPNOW_V4_TYPE_FILE_CANCEL &&
              espnowanswer::kOpCatchupListResp == ESPNOW_V4_TYPE_CATCHUP_LIST_RESP &&
              espnowanswer::kOpCatchupReadResp == ESPNOW_V4_TYPE_CATCHUP_READ_RESP,
              "System_ESPNow_AnswerCore.h names the answer opcodes of this file");
#define ESPNOW_V4_ANSWER_HEADER_AT_START(T) \
  (offsetof(T, reqId) == offsetof(V4PayloadAnswerHeader, reqId) && \
   offsetof(T, status) == offsetof(V4PayloadAnswerHeader, status) && \
   sizeof(((T*)nullptr)->reqId) == 4 && sizeof(((T*)nullptr)->status) == 1)
static_assert(ESPNOW_V4_ANSWER_HEADER_AT_START(V4PayloadTopoStart) &&
              ESPNOW_V4_ANSWER_HEADER_AT_START(V4PayloadTopoPeer) &&
              ESPNOW_V4_ANSWER_HEADER_AT_START(V4PayloadCmdRespHeader) &&
              ESPNOW_V4_ANSWER_HEADER_AT_START(V4PayloadStreamHeader) &&
              ESPNOW_V4_ANSWER_HEADER_AT_START(V4PayloadFsListReplyHeader) &&
              ESPNOW_V4_ANSWER_HEADER_AT_START(V4PayloadFsStatReply) &&
              ESPNOW_V4_ANSWER_HEADER_AT_START(V4PayloadFsGetAck) &&
              ESPNOW_V4_ANSWER_HEADER_AT_START(V4PayloadFileCancel) &&
              ESPNOW_V4_ANSWER_HEADER_AT_START(V4PayloadCatchupListRespHeader) &&
              ESPNOW_V4_ANSWER_HEADER_AT_START(V4PayloadCatchupReadRespHeader),
              "every answer starts {u32 reqId, u8 status} (rule F3)");
#undef ESPNOW_V4_ANSWER_HEADER_AT_START
static_assert(FS_LIST_STATUS_OK == espnowanswer::kOk && FS_LIST_STATUS_NOT_FOUND == espnowanswer::kNotFound &&
              FS_LIST_STATUS_NOT_A_DIR == espnowanswer::kFsNotADir &&
              FS_LIST_STATUS_PERM_DENIED == espnowanswer::kRefused &&
              FS_LIST_STATUS_IO_ERROR == espnowanswer::kIoError && FS_LIST_STATUS_TOO_BUSY == espnowanswer::kBusy &&
              FS_LIST_STATUS_NOT_READY == espnowanswer::kNotReady,
              "the FS answers use the shared status table (rule F3)");
static_assert(FILE_CANCEL_TIMEOUT == espnowanswer::kTimeout && FILE_CANCEL_WRITE_FAILED == espnowanswer::kIoError &&
              FILE_CANCEL_INCOMPLETE == espnowanswer::kFileIncomplete &&
              FILE_CANCEL_CRC_MISMATCH == espnowanswer::kFileCrcMismatch &&
              FILE_CANCEL_REJECTED == espnowanswer::kRefused,
              "FILE_CANCEL's reasons are answer statuses (rule F3)");

// ---- Receive minimum lengths (kV4HandlerTable's minLen column) --------------
//
// The smallest payload a legitimate 1.0 sender sends for each opcode, after
// the session unwrap, the group-tag strip and reassembly. The dispatcher drops
// a shorter frame (counted in rxLengthDrops) and hands a longer one to the
// handler unchanged. These numbers are FROZEN at their 1.0 values: a struct
// that grows after 1.0 keeps its minimum (change its "==" assert below to
// ">="), and the receiver zero-fills the fields an older sender left out.
// The six handshake rows are exact (V4_OPC_LEN_EXACT): their transcripts are
// signed, so they never grow. Each row names the constant for its opcode.
#define ESPNOW_V4_MINLEN_KEY_EX_HELLO        72   // exact
#define ESPNOW_V4_MINLEN_KEY_EX_REPLY        72   // exact
#define ESPNOW_V4_MINLEN_KEY_EX_CONFIRM      34   // exact
#define ESPNOW_V4_MINLEN_SESSION_OPEN        154  // exact
#define ESPNOW_V4_MINLEN_SESSION_CONFIRM     162  // exact
#define ESPNOW_V4_MINLEN_SESSION_REKEY       146  // exact
#define ESPNOW_V4_MINLEN_SESSION_UNKNOWN     12
#define ESPNOW_V4_MINLEN_TIME_SYNC           16
#define ESPNOW_V4_MINLEN_TEXT                1    // text, no terminator
#define ESPNOW_V4_MINLEN_BOOT                1    // JSON envelope
#define ESPNOW_V4_MINLEN_RELAY_DATA          40   // envelope 8 + an inner header 32
#define ESPNOW_V4_MINLEN_PEER_LIST           4    // header; entries follow (count checked by the handler)
#define ESPNOW_V4_MINLEN_CMD                 1
#define ESPNOW_V4_MINLEN_CMD_RESP            5    // answer header {reqId, status}; text follows
#define ESPNOW_V4_MINLEN_HEARTBEAT           44
#define ESPNOW_V4_MINLEN_PAIR_BEACON         36
#define ESPNOW_V4_MINLEN_PAIR_REQUEST        72   // control 40 + the requester key 32
#define ESPNOW_V4_MINLEN_PAIR_ACCEPT         40
#define ESPNOW_V4_MINLEN_PAIR_REJECT         40
#define ESPNOW_V4_MINLEN_SENSOR_STATUS       16
#define ESPNOW_V4_MINLEN_SENSOR_REQ          20
#define ESPNOW_V4_MINLEN_SENSOR_ENVELOPE     13   // reading header 12 + at least one byte of the part
#define ESPNOW_V4_MINLEN_POWER_STATUS        24
#define ESPNOW_V4_MINLEN_EVENT_PUSH          128
#define ESPNOW_V4_MINLEN_FORWARD_LINK        96
#define ESPNOW_V4_MINLEN_TOPO_REQ            8
#define ESPNOW_V4_MINLEN_TOPO_START          8
#define ESPNOW_V4_MINLEN_TOPO_PEER           47
#define ESPNOW_V4_MINLEN_USER_SYNC           1
#define ESPNOW_V4_MINLEN_METADATA_REQ        0    // senders send no payload
#define ESPNOW_V4_MINLEN_METADATA_RESP       202
#define ESPNOW_V4_MINLEN_METADATA_PUSH       202
#define ESPNOW_V4_MINLEN_STREAM              6    // answer header {reqId, status} + at least one byte of the line
#define ESPNOW_V4_MINLEN_FILE_START          72
#define ESPNOW_V4_MINLEN_FILE_DATA           3    // chunk index 2 + at least one byte
#define ESPNOW_V4_MINLEN_FILE_END            5
#define ESPNOW_V4_MINLEN_FILE_CANCEL         5
#define ESPNOW_V4_MINLEN_FS_LIST_REQ         144
#define ESPNOW_V4_MINLEN_FS_LIST_REPLY       140  // header; entries follow (count checked by the handler)
#define ESPNOW_V4_MINLEN_FS_STAT_REQ         140
#define ESPNOW_V4_MINLEN_FS_STAT_REPLY       164
#define ESPNOW_V4_MINLEN_FS_GET_REQ          140
#define ESPNOW_V4_MINLEN_FS_GET_ACK          140
#define ESPNOW_V4_MINLEN_CATCHUP_LIST_REQ    16
#define ESPNOW_V4_MINLEN_CATCHUP_LIST_RESP   24   // header; entries follow (self-sized)
#define ESPNOW_V4_MINLEN_CATCHUP_READ_REQ    24
#define ESPNOW_V4_MINLEN_CATCHUP_READ_RESP   20   // header; data follows
#define ESPNOW_V4_MINLEN_BOND_HEARTBEAT      24
#define ESPNOW_V4_MINLEN_BOND_STREAM_CTRL    4
#define ESPNOW_V4_MINLEN_BOND_CAP_REQ        0    // senders send no payload
#define ESPNOW_V4_MINLEN_BOND_CAP_RESP       84
#define ESPNOW_V4_MINLEN_BOND_SENSOR_DATA    13   // as SENSOR_ENVELOPE
#define ESPNOW_V4_MINLEN_BOND_POWER_STATUS   24
#define ESPNOW_V4_MINLEN_BOND_EVENT_PUSH     128
#define ESPNOW_V4_MINLEN_BOND_SENSOR_STATUS  16
#define ESPNOW_V4_MINLEN_SETTINGS_REQ        0    // senders send no payload
#define ESPNOW_V4_MINLEN_SCHEMA_REQ          0    // senders send no payload
#define ESPNOW_V4_MINLEN_BOND_STATUS_REQ     0    // senders send no payload
#define ESPNOW_V4_MINLEN_BOND_STATUS_RESP    28
#define ESPNOW_V4_MINLEN_MANIFEST_REQ        0    // senders send no payload
#define ESPNOW_V4_MINLEN_BOND_REQUEST        8
#define ESPNOW_V4_MINLEN_BOND_NOTICE         8

// Until the 1.0 cut every fixed frame's minimum is its struct's size.
static_assert(sizeof(V4PayloadKeyExHello) == ESPNOW_V4_MINLEN_KEY_EX_HELLO &&
              sizeof(V4PayloadKeyExReply) == ESPNOW_V4_MINLEN_KEY_EX_REPLY &&
              sizeof(V4PayloadKeyExConfirm) == ESPNOW_V4_MINLEN_KEY_EX_CONFIRM &&
              sizeof(V4PayloadSessionOpen) == ESPNOW_V4_MINLEN_SESSION_OPEN &&
              sizeof(V4PayloadSessionConfirm) == ESPNOW_V4_MINLEN_SESSION_CONFIRM &&
              sizeof(V4PayloadSessionRekey) == ESPNOW_V4_MINLEN_SESSION_REKEY,
              "a handshake frame is exact-length: its minimum is its size, and it never grows");
static_assert(sizeof(V4PayloadSessionUnknown) == ESPNOW_V4_MINLEN_SESSION_UNKNOWN &&
              sizeof(V4PayloadTimeSync) == ESPNOW_V4_MINLEN_TIME_SYNC &&
              sizeof(V4PayloadRelayData) + ESPNOW_V4_HEADER_LEN == ESPNOW_V4_MINLEN_RELAY_DATA &&
              sizeof(V4PayloadPeerListHeader) == ESPNOW_V4_MINLEN_PEER_LIST &&
              sizeof(V4PayloadCmdRespHeader) == ESPNOW_V4_MINLEN_CMD_RESP &&
              sizeof(V4PayloadHeartbeat) == ESPNOW_V4_MINLEN_HEARTBEAT &&
              sizeof(V4PayloadPairBeacon) == ESPNOW_V4_MINLEN_PAIR_BEACON &&
              sizeof(V4PayloadPairRequest) == ESPNOW_V4_MINLEN_PAIR_REQUEST &&
              sizeof(V4PayloadPairCtrl) == ESPNOW_V4_MINLEN_PAIR_ACCEPT &&
              sizeof(V4PayloadPairCtrl) == ESPNOW_V4_MINLEN_PAIR_REJECT &&
              sizeof(V4PayloadSensorStatus) == ESPNOW_V4_MINLEN_SENSOR_STATUS &&
              sizeof(V4PayloadSensorReq) == ESPNOW_V4_MINLEN_SENSOR_REQ &&
              sizeof(V4PayloadSensorReading) + 1 == ESPNOW_V4_MINLEN_SENSOR_ENVELOPE &&
              sizeof(V4PayloadPowerStatus) == ESPNOW_V4_MINLEN_POWER_STATUS &&
              sizeof(V4PayloadEventPush) == ESPNOW_V4_MINLEN_EVENT_PUSH &&
              sizeof(V4PayloadForwardLink) == ESPNOW_V4_MINLEN_FORWARD_LINK &&
              sizeof(V4PayloadTopoReq) == ESPNOW_V4_MINLEN_TOPO_REQ &&
              sizeof(V4PayloadTopoStart) == ESPNOW_V4_MINLEN_TOPO_START &&
              sizeof(V4PayloadTopoPeer) == ESPNOW_V4_MINLEN_TOPO_PEER &&
              sizeof(V4PayloadMetadata) == ESPNOW_V4_MINLEN_METADATA_RESP &&
              sizeof(V4PayloadMetadata) == ESPNOW_V4_MINLEN_METADATA_PUSH &&
              sizeof(V4PayloadFileStart) == ESPNOW_V4_MINLEN_FILE_START &&
              offsetof(V4PayloadFileData, data) + 1 == ESPNOW_V4_MINLEN_FILE_DATA &&
              sizeof(V4PayloadFileEnd) == ESPNOW_V4_MINLEN_FILE_END &&
              sizeof(V4PayloadFileCancel) == ESPNOW_V4_MINLEN_FILE_CANCEL &&
              sizeof(V4PayloadStreamHeader) + 1 == ESPNOW_V4_MINLEN_STREAM &&
              sizeof(V4PayloadFsListReq) == ESPNOW_V4_MINLEN_FS_LIST_REQ &&
              sizeof(V4PayloadFsListReplyHeader) == ESPNOW_V4_MINLEN_FS_LIST_REPLY &&
              sizeof(V4PayloadFsStatReq) == ESPNOW_V4_MINLEN_FS_STAT_REQ &&
              sizeof(V4PayloadFsStatReply) == ESPNOW_V4_MINLEN_FS_STAT_REPLY &&
              sizeof(V4PayloadFsGetReq) == ESPNOW_V4_MINLEN_FS_GET_REQ &&
              sizeof(V4PayloadFsGetAck) == ESPNOW_V4_MINLEN_FS_GET_ACK &&
              sizeof(V4PayloadCatchupListReq) == ESPNOW_V4_MINLEN_CATCHUP_LIST_REQ &&
              sizeof(V4PayloadCatchupListRespHeader) == ESPNOW_V4_MINLEN_CATCHUP_LIST_RESP &&
              sizeof(V4PayloadCatchupReadReq) == ESPNOW_V4_MINLEN_CATCHUP_READ_REQ &&
              sizeof(V4PayloadCatchupReadRespHeader) == ESPNOW_V4_MINLEN_CATCHUP_READ_RESP &&
              sizeof(CapabilitySummary) == ESPNOW_V4_MINLEN_BOND_CAP_RESP &&
              sizeof(V4PayloadSensorReading) + 1 == ESPNOW_V4_MINLEN_BOND_SENSOR_DATA &&
              sizeof(V4PayloadPowerStatus) == ESPNOW_V4_MINLEN_BOND_POWER_STATUS &&
              sizeof(V4PayloadEventPush) == ESPNOW_V4_MINLEN_BOND_EVENT_PUSH &&
              sizeof(V4PayloadSensorStatus) == ESPNOW_V4_MINLEN_BOND_SENSOR_STATUS &&
              sizeof(BondPeerStatus) == ESPNOW_V4_MINLEN_BOND_STATUS_RESP,
              "minLen never rises: a fixed frame's 1.0 minimum is its struct size (after 1.0: >=)");
#if ENABLE_BONDED_MODE
static_assert(sizeof(V4PayloadBondHeartbeat) == ESPNOW_V4_MINLEN_BOND_HEARTBEAT &&
              sizeof(V4PayloadStreamCtrl) == ESPNOW_V4_MINLEN_BOND_STREAM_CTRL &&
              sizeof(V4PayloadBondRequest) == ESPNOW_V4_MINLEN_BOND_REQUEST &&
              sizeof(V4PayloadBondNotice) == ESPNOW_V4_MINLEN_BOND_NOTICE,
              "minLen never rises: a fixed frame's 1.0 minimum is its struct size (after 1.0: >=)");
#endif

#endif // ENABLE_ESPNOW

#endif // SYSTEM_ESPNOW_WIRE_H
