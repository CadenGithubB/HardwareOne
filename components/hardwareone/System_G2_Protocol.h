#ifndef SYSTEM_G2_PROTOCOL_H
#define SYSTEM_G2_PROTOCOL_H

#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// =============================================================================
// Even Realities G2 — wire protocol primitives
// =============================================================================
// Low-level building blocks for the G2 BLE protocol as reverse-engineered by
// the g2-kit-unofficial project. High-level connection / session state is
// built on top of these in System_G2.{h,cpp}.
//
// Envelope (all command+notify traffic):
//
//   AA 21  LL LL  SS  FF  II  MM MM MM MM  <payload>  CC CC
//   │     │      │   │   │   │             │          └── CRC-16/CCITT-FALSE,
//   │     │      │   │   │   │             │              big-endian, over
//   │     │      │   │   │   │             │              bytes 0..end-2
//   │     │      │   │   │   │             └── protobuf (sid-dependent)
//   │     │      │   │   │   └── magic (u32-LE — firmware only checks low byte,
//   │     │      │   │   │       keep ≤0xFF or cycle as u8)
//   │     │      │   │   └── seq (group key — SAME on every fragment of one
//   │     │      │   │       logical message; NOT an incrementing counter)
//   │     │      │   └── flag: 0x00 request, 0x01 async / indication
//   │     │      └── sid (subsystem): 0x01 app-launch, 0x09 settings,
//   │     │          0x0d state events, 0x0e audio?, 0xe0 EvenHub (rendering),
//   │     │          0x80 dev_config (DO NOT USE — can brick glasses)
//   │     └── length = total envelope size in bytes (header + payload + CRC).
//   │         Different capture tools disagree — we write "total" because
//   │         g2-kit-unofficial's envelope builder does.
//   └── fixed preamble bytes (0xAA 0x21)
//
// Fragmentation: every BLE write is at most `MTU-3` bytes. When a logical
// envelope exceeds the ATT MTU, split at byte boundaries AND reuse the same
// `seq` byte on every fragment. The firmware reassembles by grouping on seq
// until it has `LL` bytes total.
//
// Serialization: writes to the command characteristic MUST be serialized.
// If a heartbeat lands mid-rebuild, the firmware's single reassembly buffer
// corrupts and the plugin task wedges (mic dies too).
//
// Authoritative reference: https://github.com/Commute773/g2-kit-unofficial
// =============================================================================

// ── Constants ─────────────────────────────────────────────────────────────────

#define G2_PREAMBLE_0       0xAA
#define G2_PREAMBLE_1       0x21
#define G2_ENVELOPE_HDR_LEN 11   // AA 21 LL*2 SS FF II MM*4
#define G2_ENVELOPE_CRC_LEN 2

// Subsystem IDs (sid). See evenhub-commands.md in the reference repo.
#define G2_SID_APP_LAUNCH   0x01
#define G2_SID_SETTINGS     0x09
#define G2_SID_STATE_EVENT  0x0D
#define G2_SID_EVEN_HUB     0xE0

// Flag byte.
#define G2_FLAG_REQUEST     0x00
#define G2_FLAG_ASYNC       0x01

// EvenHub Cmd enum (EvenHub_Cmd_List in the reference). Note that the wrapper
// field numbers are different — field indexes live in the pb serializer below.
#define G2_CMD_CREATE_STARTUP     0
#define G2_CMD_IMAGE_RAW_DATA     3
#define G2_CMD_UPDATE_TEXT        5
#define G2_CMD_REBUILD_PAGE       7
#define G2_CMD_SHUTDOWN_PAGE      9
#define G2_CMD_HEARTBEAT          12
#define G2_CMD_AUDIO_CTRL         15  // Cmd value = 15 (wrapper field is 18)
#define G2_CMD_IMU_CTRL           19

// ── CRC-16/CCITT-FALSE ───────────────────────────────────────────────────────
// Polynomial 0x1021, init 0xFFFF, no input reflect, no output reflect, no
// final XOR. Distinct from the more common CCITT-KERMIT (different init).
uint16_t g2CrcCcittFalse(const uint8_t* data, size_t len);

// ── Envelope build / parse ───────────────────────────────────────────────────

// Build a complete envelope: preamble, header, payload, CRC. Returns total
// bytes written, or 0 on buffer overflow. `payload` may be null when
// `payloadLen` is 0.
size_t g2BuildEnvelope(uint8_t sid,
                       uint8_t flag,
                       uint8_t seq,
                       uint32_t magic,
                       const uint8_t* payload,
                       size_t payloadLen,
                       uint8_t* out,
                       size_t outCap);

struct G2EnvelopeView {
  uint8_t  sid;
  uint8_t  flag;
  uint8_t  seq;
  uint32_t magic;
  const uint8_t* payload;
  size_t   payloadLen;
};

// Parse one envelope out of `in`. Returns true on success (CRC valid, length
// fields consistent). On failure `out` is left untouched.
bool g2ParseEnvelope(const uint8_t* in, size_t len, G2EnvelopeView* out);

// ── Protobuf (proto3) primitives ─────────────────────────────────────────────
// Hand-rolled. The G2 protocol only uses wire types 0 (varint) and 2
// (length-delimited message/string/bytes) for the messages we need. Wire
// types 1 (fixed64) and 5 (fixed32) are only seen in IMU data.
//
// All writers append to `buf` starting at `*pos`, advance `*pos`, and return
// true on success / false on overflow. Readers advance `*pos` past the field.

enum G2PbWire {
  G2_PB_WIRE_VARINT = 0,
  G2_PB_WIRE_FIXED64 = 1,
  G2_PB_WIRE_LEN_DELIM = 2,
  G2_PB_WIRE_FIXED32 = 5
};

// Varint + tag helpers.
bool g2PbWriteVarint(uint8_t* buf, size_t cap, size_t* pos, uint64_t v);
bool g2PbWriteTag(uint8_t* buf, size_t cap, size_t* pos, uint32_t field, uint8_t wire);

// Typed field writers.
bool g2PbWriteUint32(uint8_t* buf, size_t cap, size_t* pos, uint32_t field, uint32_t v);
bool g2PbWriteBool  (uint8_t* buf, size_t cap, size_t* pos, uint32_t field, bool v);
bool g2PbWriteString(uint8_t* buf, size_t cap, size_t* pos, uint32_t field, const char* s);
bool g2PbWriteBytes (uint8_t* buf, size_t cap, size_t* pos, uint32_t field,
                     const uint8_t* data, size_t len);

// Begin a nested (length-delimited) submessage. Reserves 2 bytes for its
// length prefix and returns the start of the nested payload via `innerStart`.
// Call g2PbEndNested once the inner message is written to patch the length.
bool g2PbBeginNested(uint8_t* buf, size_t cap, size_t* pos,
                     uint32_t field, size_t* innerStart);
bool g2PbEndNested(uint8_t* buf, size_t cap, size_t* pos, size_t innerStart);

// Readers. Return false on malformed input.
bool g2PbReadVarint(const uint8_t* buf, size_t len, size_t* pos, uint64_t* v);
bool g2PbReadTag(const uint8_t* buf, size_t len, size_t* pos,
                 uint32_t* field, uint8_t* wire);

// Skip one field of known wire type, advancing `pos` past its value. Useful
// for tolerating unknown fields in responses.
bool g2PbSkipField(const uint8_t* buf, size_t len, size_t* pos, uint8_t wire);

// ── High-level message builders (EvenHub wrapper) ────────────────────────────
// Each builder writes the complete envelope (preamble..CRC) into `out` and
// returns the total length. Fragmentation is the caller's responsibility
// (System_G2.cpp handles MTU splitting).
//
// `seq` and `magic` are assigned by the caller's request-tracking layer.

// EvenHub heartbeat (Cmd=12). Must be sent every ~5 s or the glasses'
// plugin task dies. `cnt` should monotonically increment.
size_t g2BuildHeartbeat(uint8_t seq, uint32_t magic, uint32_t cnt,
                        uint8_t* out, size_t outCap);

// EvenHub shutdown (Cmd=9). `exitMode` is typically 0 for a clean teardown.
size_t g2BuildShutdown(uint8_t seq, uint32_t magic, uint32_t exitMode,
                       uint8_t* out, size_t outCap);

// EvenHub CREATE: build a StartUpPage containing a single TextContainer.
// This is the session-primer — it must be sent once per connection before
// any other EvenHub traffic will be accepted. `containerName` ≤14 chars,
// case-sensitive; a stable name like "app" works fine.
size_t g2BuildCreateStartupText(uint8_t seq, uint32_t magic,
                                const char* containerName,
                                const char* initialContent,
                                uint8_t* out, size_t outCap);

// EvenHub REBUILD: replace the text content of an existing TextContainer.
// Use for "show text" after the initial create. Reuses the same container
// name; glasses will redraw.
size_t g2BuildRebuildText(uint8_t seq, uint32_t magic,
                          const char* containerName,
                          const char* content,
                          uint8_t* out, size_t outCap);

// EvenHub audio control (Cmd=15). `enable` maps to AudoFuncEn: 1 start, 0 stop.
size_t g2BuildAudioCtrl(uint8_t seq, uint32_t magic, bool enable,
                        uint8_t* out, size_t outCap);

// AppLaunch prelude (sid=0x01). MUST be sent once per fresh BLE session
// before any EvenHub traffic is accepted. The inner body is a fixed
// protobuf literal captured from the reference implementation — its nested
// fields are not semantically understood, just reproduced verbatim.
size_t g2BuildAppLaunch(uint8_t seq, uint8_t* out, size_t outCap);

// G2Setting basic-info request (sid=0x09). Triggers a response containing
// battery, charging status, and other fields. `magic` identifies the round
// trip in the response.
size_t g2BuildSettingBasicRequest(uint8_t seq, uint32_t magic,
                                  uint8_t* out, size_t outCap);

// Decode the battery percentage out of a G2SettingPackage response. Returns
// true and fills `*batteryPct` (0..100) if the response carries one.
bool g2ParseSettingBattery(const uint8_t* payload, size_t payloadLen,
                           uint8_t* batteryPct);

#endif // SYSTEM_G2_PROTOCOL_H
