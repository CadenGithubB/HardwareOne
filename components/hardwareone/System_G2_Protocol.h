#ifndef SYSTEM_G2_PROTOCOL_H
#define SYSTEM_G2_PROTOCOL_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <stddef.h>
#include <stdint.h>

// =============================================================================
// Compile-time gate
// =============================================================================
// All G2 wire-protocol primitives below are gated behind ENABLE_BLUETOOTH +
// ENABLE_G2_GLASSES so they don't compile (or get linked) when the build
// has the G2 feature disabled. Header consumers outside the gate get an
// empty translation unit — they shouldn't reference any of these symbols
// since their own callers are also gated. Matches the gating pattern used
// by System_R1_Protocol.{h,cpp}, G2_Glasses.{h,cpp}, and G2_Ring.{h,cpp}.
#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// =============================================================================
// Even Realities G2 — wire protocol primitives
// =============================================================================
// Low-level building blocks for the G2 BLE protocol, matching the reference
// implementation at https://github.com/Commute773/g2-kit-unofficial .
//
// Envelope (single-fragment form — all our messages fit one fragment):
//
//   TX: AA 21 <seq> <len> <totFrags> <fragIdx> <sid> <flag> <pb...> <crcLE>
//   RX: AA 12 <seq> <len> <totFrags> <fragIdx> <sid> <flag> <pb...> <crcLE>
//       └──┬──┘ └─┬─┘ └─┬─┘ └───┬───┘ └──┬──┘ └─┬─┘ └──┬─┘ └──┬─┘ └──┬──┘
//          │     │     │       │        │     │      │      │      └── CRC-16/CCITT-FALSE
//          │     │     │       │        │     │      │      │          over JUST the pb bytes,
//          │     │     │       │        │     │      │      │          emitted little-endian.
//          │     │     │       │        │     │      │      └── protobuf payload
//          │     │     │       │        │     │      └── flag (0x20 request, 0x00 response,
//          │     │     │       │        │     │          0x01/0x06 async notify)
//          │     │     │       │        │     └── sid (0x01 app-launch, 0x09 settings,
//          │     │     │       │        │         0x0d state events, 0xe0 EvenCore — the
//          │     │     │       │        │         firmware's main rendering subsystem, named
//          │     │     │       │        │         `evenhub_main_msg_ctx` in Even's pb schema)
//          │     │     │       │        └── fragment index within this message (1-based)
//          │     │     │       └── total fragments for this message
//          │     │     └── length of THIS fragment's data (pb bytes on this frag + CRC
//          │     │         if last). One byte — max 0xFF.
//          │     └── transport seq byte (GROUP KEY: identical on every fragment of a
//          │         single logical message, so the firmware can reassemble them).
//          │         Incrementing per-fragment causes the firmware to silently drop
//          │         the whole message.
//          └── TX preamble 0xAA 0x21, RX preamble 0xAA 0x12.
//
// Fragmentation: every BLE write is at most (MTU-3) bytes, default cap 232
// in the reference. Reassembly is keyed on the seq byte. We currently only
// emit single-fragment messages (all of ours fit well under 250 B).
//
// Magic: there is NO "magic" field in the transport header. What the earlier
// docs called "magic" is actually the `MagicRandom` field inside the EvenCore
// protobuf wrapper, used for ACK correlation. Don't thread it through the
// envelope builder.
//
// Naming note: the firmware's internal name for the sid=0xE0 subsystem is
// "EvenHub" (pb message type `evenhub_main_msg_ctx`, enum `EvenHub_Cmd_List`).
// "EvenHub" is also the marketing name of Even's plugin/SDK platform — we
// don't use that SDK, we just speak the core rendering protocol directly. To
// avoid implying a plugin integration, this codebase calls the subsystem
// "EvenCore" throughout, while keeping the firmware identifiers visible in
// comments so anyone cross-referencing the source pb schema can still trace.
//
// Authoritative reference: ble/envelope.ts in the repo above.
// =============================================================================

// ── Constants ─────────────────────────────────────────────────────────────────

#define G2_PREAMBLE_0       0xAA
#define G2_PREAMBLE_TX      0x21
#define G2_PREAMBLE_RX      0x12

#define G2_ENVELOPE_HDR_LEN 8    // AA 21 seq len tot idx sid flag
#define G2_ENVELOPE_CRC_LEN 2
#define G2_MAX_FRAG_PB      253  // u8 len minus 2 CRC bytes

// Subsystem IDs (sid). Names from `service_id_def.proto::SID` in the
// reference (file `service_id_def_pb.ts`); the firmware enum is the
// authoritative list. Where our captures gave a different name, the
// firmware enum wins.
#define G2_SID_APP_LAUNCH   0x01  // UI_BACKGROUND_DASHBOARD_APP_ID — but used by us
                                  // as "app launch prelude". Carries dashboard
                                  // protobuf in normal flow.
#define G2_SID_TRANSLATE    0x05  // UI_TRANSLATE_APP_ID — front-pane translate
                                  // overlay. `translate_pb.ts`. Reachable; CTRL=1
                                  // with empty body returns errorCode=7.
#define G2_SID_TELEPROMPT   0x06  // UI_TELEPROMPT_APP_ID — full-page scrolling
                                  // text. `teleprompt_pb.ts`. Reachable; CTRL=1
                                  // with empty body returns errorCode=1.
#define G2_SID_EVEN_AI      0x07  // UI_FOREGROUND_EVEN_AI_ID — front-pane voice/AI
                                  // overlay. Payload is `EvenAIDataPackage` (see
                                  // `even_ai_pb.ts`). Carries CTRL/REPLY/ASK and
                                  // friends. Wake-word fires CTRL{status=WAKE_UP}.
#define G2_SID_TRANSCRIBE   0x0A  // UI_TRANSCRIBE_APP_ID — front-pane streaming
                                  // transcription. `transcribe_pb.ts`. Probed
                                  // silent on 2026-04-26 — subsystem may not be
                                  // initialized at runtime.
#define G2_SID_CONVERSATE   0x0B  // UI_CONVERSATE_APP_ID — front-pane transcribe
                                  // + tag bubbles. `conversate_pb.ts`. Reachable;
                                  // CTRL=1 opens but FSM parks waiting for a
                                  // PREP_NOTE_LIST response (cmd=3).
#define G2_SID_SETTINGS     0x09  // UI_SETTING_APP_ID — we use it for battery/version
                                  // reads on the side; the wider schema is broader.
#define G2_SID_STATE_EVENT  0x0D  // SERVICE_SYNC_INFO_APP_ID — non-rendering event
                                  // channel (gestures, foreground transitions).
#define G2_SID_WIDGET_XFORM 0x0E  // UI_HEALTH_APP_ID per the firmware enum, but we
                                  // empirically see widget-transform-shaped traffic
                                  // here. Disagreement worth a labelled capture.
#define G2_SID_HEARTBEAT    0x80  // UX_DEVICE_SETTINGS_APP_ID per firmware enum, but
                                  // observed traffic looks like heartbeat-style
                                  // status pings (`08 06 10 <varint> 2A 00`). Name
                                  // kept until we resolve the discrepancy.
#define G2_SID_FILE_RAW     0xC5  // UX_EVEN_FILE_SERVICE_RAW_SEND_DATA_ID — generic
                                  // file-service raw data channel (also used by OTA).
                                  // Phone notifications ride on this with file type
                                  // ANDROID_MSG_JSON_NOTIFICATION on the 0xC4 cmd
                                  // channel.
#define G2_SID_FILE_CMD     0xC4  // UX_EVEN_FILE_SERVICE_CMD_SEND_ID — companion to
                                  // 0xC5; carries the file-send start/data/result
                                  // metadata.
#define G2_SID_EVEN_CORE    0xE0  // UI_BACKGROUND_EVENHUB_APP_ID — back-pane render
                                  // surface ("EvenHub" in the firmware's pb schema).

// Flag byte — note request is 0x20, NOT 0x00. 0x00 is the response direction.
#define G2_FLAG_REQUEST     0x20
#define G2_FLAG_RESPONSE    0x00
#define G2_FLAG_NOTIFY      0x01
#define G2_FLAG_NOTIFY_ALT  0x06

// EvenCore Cmd enum values (field 1 of the wrapper protobuf). Taken from
// the firmware's `EvenHub_Cmd_List` in `EvenHub_pb.ts` — names kept as-is
// for grep-ability against the reference source.
#define G2_CMD_CREATE_STARTUP     0
#define G2_CMD_IMAGE_RAW_DATA     3
#define G2_CMD_UPDATE_TEXT        5
#define G2_CMD_REBUILD_PAGE       7
#define G2_CMD_SHUTDOWN_PAGE      9
#define G2_CMD_HEARTBEAT          12
#define G2_CMD_AUDIO_CTRL         15
#define G2_CMD_MENU_STARTUP       17  // OS_NOTIFY_MENU_STARTUP_PACKET (async notify from glasses)
#define G2_CMD_MENU_FAILED        18  // APP_RESPONSE_MENU_STARTUP_FAILED_PACKET (host → glasses)

// ── Default MagicRandom values per message type ──────────────────────────────
// Firmware only compares the low byte of MagicRandom when matching acks, so
// any u8 works. The reference uses fixed per-type constants rather than a
// rolling counter so the ack-matching layer can be stateless ("expecting
// REBUILD ack? look for magic=202"). We mirror that convention. These
// values are safe when only one message of each type is in flight at a
// time, which is our operating assumption for everything except image
// streaming — image fragments must each pass a unique magic.
#define G2_MAGIC_CREATE           201
#define G2_MAGIC_REBUILD          202
#define G2_MAGIC_REBUILD_LIST     203
#define G2_MAGIC_SHUTDOWN         204
#define G2_MAGIC_HEARTBEAT        205
#define G2_MAGIC_TEXT_UPGRADE     206
#define G2_MAGIC_AUDIO_CTRL       207
#define G2_MAGIC_SETTINGS         208
#define G2_MAGIC_IMAGE_BASE       210  // images += offset per fragment
#define G2_MAGIC_MENU_FAILED      211
#define G2_MAGIC_EVEN_AI_CTRL     212
#define G2_MAGIC_EVEN_AI_REPLY    213
#define G2_MAGIC_EVEN_AI_ASK      214
#define G2_MAGIC_EVEN_AI_ANALYSE  215

// ── Even-AI subsystem (sid=0x07, EvenAIDataPackage) ─────────────────────────
// Schema from `ble/gen/even_ai_pb.ts` in g2-kit-unofficial. Field numbers
// here match the .proto definitions verbatim. Defined-but-unexercised in
// the reference — host→glasses behaviour is unverified on hardware.
#define G2_AI_F_CMD          1   // commandId (eEvenAICommandId)
#define G2_AI_F_MAGIC        2   // magicRandom
#define G2_AI_F_CTRL         3   // EvenAIControl
#define G2_AI_F_ASK          5   // EvenAIAskInfo
#define G2_AI_F_ANALYSE      6   // EvenAIAnalyseInfo
#define G2_AI_F_REPLY        7   // EvenAIReplyInfo

#define G2_AI_CMD_CTRL       1
#define G2_AI_CMD_ASK        3
#define G2_AI_CMD_ANALYSE    4
#define G2_AI_CMD_REPLY      5
#define G2_AI_CMD_HEARTBEAT  9

// eEvenAIStatus
#define G2_AI_STATUS_WAKE_UP 1   // wake-word listening UI (firmware-initiated normally)
#define G2_AI_STATUS_ENTER   2   // open the front-pane card without listening
#define G2_AI_STATUS_EXIT    3

// EvenAIControl fields
#define G2_AI_CTRL_F_STATUS  1

// EvenAIReplyInfo fields
#define G2_AI_REPLY_F_CNT     1   // cmdCnt — increments per chunk for streamed replies
#define G2_AI_REPLY_F_STREAM  2   // streamEnable — 1 = streaming, 0 = single shot
#define G2_AI_REPLY_F_MODE    3   // textMode — meaning unverified
#define G2_AI_REPLY_F_TEXT    4   // text (bytes, UTF-8)
#define G2_AI_REPLY_F_END     6   // fTextEnd — 1 = last chunk, render-and-hold

// ── CRC-16/CCITT-FALSE ───────────────────────────────────────────────────────
// Polynomial 0x1021, init 0xFFFF, no input reflect, no output reflect, no
// final XOR.
uint16_t g2CrcCcittFalse(const uint8_t* data, size_t len);

// ── Envelope build / parse ───────────────────────────────────────────────────

// Build a complete single-fragment envelope. `seq` is the transport seq byte
// (caller picks — must be distinct from other in-flight messages). Returns
// total bytes written into `out`, or 0 on buffer overflow / payload too big.
size_t g2BuildEnvelope(uint8_t seq, uint8_t sid, uint8_t flag,
                       const uint8_t* payload, size_t payloadLen,
                       uint8_t* out, size_t outCap);

struct G2EnvelopeView {
  bool     isTx;         // true if preamble was AA 21, false for AA 12
  uint8_t  seq;
  uint8_t  totalFrags;
  uint8_t  fragIdx;
  uint8_t  sid;
  uint8_t  flag;
  const uint8_t* payload;
  size_t   payloadLen;   // pb bytes (CRC stripped)
};

// Parse a single-fragment frame. Verifies preamble + CRC. On success, fills
// `out->payload` to point into `in` (after the 8-byte header) and sets
// `payloadLen` to the pb byte count (excluding CRC).
bool g2ParseEnvelope(const uint8_t* in, size_t len, G2EnvelopeView* out);

// ── Protobuf (proto3) primitives ─────────────────────────────────────────────
enum G2PbWire {
  G2_PB_WIRE_VARINT = 0,
  G2_PB_WIRE_FIXED64 = 1,
  G2_PB_WIRE_LEN_DELIM = 2,
  G2_PB_WIRE_FIXED32 = 5
};

bool g2PbWriteVarint(uint8_t* buf, size_t cap, size_t* pos, uint64_t v);
bool g2PbWriteTag(uint8_t* buf, size_t cap, size_t* pos, uint32_t field, uint8_t wire);
bool g2PbWriteUint32(uint8_t* buf, size_t cap, size_t* pos, uint32_t field, uint32_t v);
bool g2PbWriteBool  (uint8_t* buf, size_t cap, size_t* pos, uint32_t field, bool v);
bool g2PbWriteString(uint8_t* buf, size_t cap, size_t* pos, uint32_t field, const char* s);
bool g2PbWriteBytes (uint8_t* buf, size_t cap, size_t* pos, uint32_t field,
                     const uint8_t* data, size_t len);
bool g2PbBeginNested(uint8_t* buf, size_t cap, size_t* pos,
                     uint32_t field, size_t* innerStart);
bool g2PbEndNested(uint8_t* buf, size_t cap, size_t* pos, size_t innerStart);

bool g2PbReadVarint(const uint8_t* buf, size_t len, size_t* pos, uint64_t* v);
bool g2PbReadTag(const uint8_t* buf, size_t len, size_t* pos,
                 uint32_t* field, uint8_t* wire);
bool g2PbSkipField(const uint8_t* buf, size_t len, size_t* pos, uint8_t wire);

// ── Container geometry presets ───────────────────────────────────────────────
// The list/text container's on-lens rectangle is a free parameter in the
// pb schema (X/Y/W/H, lens is 576×288). The reference's
// `buildCreateStartUpPageContainer` hardcodes 280×130 at origin (0,0), which
// is roughly half-lens and only fits ~3 rows of a list with the firmware's
// default font. We expose a small named-preset table so per-page callers
// can pick a size by intent ("LARGE" for menus, "SMALL" for compact info
// chips) without hand-tuning four magic numbers each time.
//
// Geometry is applied at CREATE time. Changing the geom requires
// SHUTDOWN+CREATE — REBUILD is in-place and reuses the existing rectangle.
// (g2ShowListPage already does SHUTDOWN+CREATE for every swap, so changing
// geom between pages is free.)
struct G2ContainerGeom {
  uint32_t x;
  uint32_t y;
  uint32_t w;
  uint32_t h;
};

// Full lens, no margin. Rarely needed — use LARGE in practice; this exists
// for future image work or any place where edge-to-edge is desired.
static constexpr G2ContainerGeom G2_GEOM_FULL   = {   0,   0, 576, 288 };

// Default for hijack menus and any "give me as many rows as possible" case.
// 8 px margin on every side keeps content clear of the curved lens edges.
static constexpr G2ContainerGeom G2_GEOM_LARGE  = {   8,   8, 560, 272 };

// Centered rectangle — about 80 % of the lens. Comfortable for medium-
// length lists where you want some surrounding dead space.
static constexpr G2ContainerGeom G2_GEOM_MEDIUM = {  48,  24, 480, 240 };

// Reference's original 280×130, re-centered. Matches the look of the
// firmware's own one-shot prompts. ~3 rows visible.
static constexpr G2ContainerGeom G2_GEOM_SMALL  = { 148,  79, 280, 130 };

// Half-height presets for split layouts (e.g. status header + scroll body).
// Currently unused by any page; provided for future composition.
static constexpr G2ContainerGeom G2_GEOM_TOP_HALF    = {   8,   8, 560, 130 };
static constexpr G2ContainerGeom G2_GEOM_BOTTOM_HALF = {   8, 150, 560, 130 };

// Half-width presets — useful when pairing two surfaces side-by-side
// (e.g. text on the left, icon/image on the right). 8 px margin on the
// outer edges; inner edge butts the lens midline so two LEFT/RIGHT
// containers tile cleanly.
static constexpr G2ContainerGeom G2_GEOM_LEFT_HALF   = {   8,   8, 280, 272 };
static constexpr G2ContainerGeom G2_GEOM_RIGHT_HALF  = { 288,   8, 280, 272 };

// Quadrant presets — quarter-lens rectangles. Together they tile the
// full lens with consistent 8 px edge margins and 0 px interior gutter.
// Useful for 2x2 layouts and combo (text + image) tests.
static constexpr G2ContainerGeom G2_GEOM_QUAD_TL = {   8,   8, 280, 136 };
static constexpr G2ContainerGeom G2_GEOM_QUAD_TR = { 288,   8, 280, 136 };
static constexpr G2ContainerGeom G2_GEOM_QUAD_BL = {   8, 144, 280, 136 };
static constexpr G2ContainerGeom G2_GEOM_QUAD_BR = { 288, 144, 280, 136 };

// Thin horizontal strips — status-bar (top) and footer (bottom). About
// one row tall on the firmware's default font. Pair with a TOP_HALF /
// BOTTOM_HALF body container for a header-plus-content layout.
static constexpr G2ContainerGeom G2_GEOM_STATUS_BAR = {   8,   8, 560,  40 };
static constexpr G2ContainerGeom G2_GEOM_FOOTER     = {   8, 240, 560,  40 };

// Edge-anchored geoms. Verified 2026-04-30 on firmware 2.2.0.24 via
// the Tests / Display / Selection Patterns / Edges + canaries bench
// (tests N / O / L). Because TextObject content anchors top-left of
// its bounding box, "bottom strip" geoms must hug y≈248 to actually
// paint at the bottom of vision — a geom anchored at y=144 paints
// in the vertical middle. STATUS_BAR above is the matching top
// preset (verified by test P).
//   BOTTOM_BAR    — 32 px strip across the full width
//   BOTTOM_BAR_L  — left half of BOTTOM_BAR (paired Yes button)
//   BOTTOM_BAR_R  — right half of BOTTOM_BAR (paired No button)
//   RIGHT_COL     — narrow column hugging the right edge, full height
static constexpr G2ContainerGeom G2_GEOM_BOTTOM_BAR    = {   8, 248, 560,  32 };
static constexpr G2ContainerGeom G2_GEOM_BOTTOM_BAR_L  = {   8, 248, 280,  32 };
static constexpr G2ContainerGeom G2_GEOM_BOTTOM_BAR_R  = { 288, 248, 280,  32 };
static constexpr G2ContainerGeom G2_GEOM_RIGHT_COL     = { 438,   8, 130, 272 };

// Stress shapes for geometry-edge testing — extreme aspect ratios that
// production pages don't use, but that exercise the firmware's container
// layout against unusual rectangles.
static constexpr G2ContainerGeom G2_GEOM_TALL_NARROW = {  16,   8,  96, 272 };
static constexpr G2ContainerGeom G2_GEOM_CENTER_DOT  = { 224, 104, 128,  80 };

// ── High-level message builders ──────────────────────────────────────────────
// Each builder writes the complete envelope (preamble..CRC) into `out` and
// returns the total byte count. `seq` is the transport seq byte; `magic` is
// the EvenCore `MagicRandom` ack-correlation field placed inside the pb.

size_t g2BuildHeartbeat(uint8_t seq, uint32_t magic, uint32_t cnt,
                        uint8_t* out, size_t outCap);

size_t g2BuildShutdown(uint8_t seq, uint32_t magic, uint32_t exitMode,
                       uint8_t* out, size_t outCap);

// EvenCore REBUILD with a single TextContainer. Works as both the initial
// "show something" call after the prelude AND subsequent updates — reusing
// the same container name edits it in place.
size_t g2BuildRebuildText(uint8_t seq, uint32_t magic,
                          const char* containerName,
                          const char* content,
                          uint8_t* out, size_t outCap);

// pb-only REBUILD-text — required when content exceeds the firmware's
// single-fragment envelope cap (~240 B). Caller ships via
// sendPbFragmented for multi-fragment wire framing. Geom-aware to
// match the CREATE's widget rectangle. See g2BuildCreateTextPagePb
// for the parallel CREATE shape.
size_t g2BuildRebuildTextPb(uint32_t magic,
                            const char* containerName,
                            const char* content,
                            const G2ContainerGeom& geom,
                            bool eventCapture,
                            uint8_t* pbOut, size_t pbCap);

// (Legacy g2BuildCreateStartupPage was removed 2026-04-30. It built a
// single-envelope CREATE_STARTUP_PAGE with a 256-byte internal payload
// buffer, which clipped content >256 B and emitted single-fragment
// envelopes the firmware refused past ~240 B. Use
// g2BuildCreateTextPagePb (geom-aware, pb-only) shipped via
// sendPbFragmented in G2_Glasses.cpp instead. See git history
// if you need the old wire shape.)

// EvenCore CREATE_STARTUP_PAGE (Cmd=0) carrying a ListContainerProperty with
// N selectable items instead of a TextContainer. Firmware draws a native
// selection box around the focused item and routes touchpad gestures into a
// List_ItemEvent sub-message on sid=0x0D. Reference: ListContainerProperty /
// List_ItemContainerProperty in ble/gen/EvenHub_pb.ts; `captureEvents=true`
// default in buildCreateStartUpPageContainer.
size_t g2BuildCreateListPage(uint8_t seq, uint32_t magic,
                             const char* containerName,
                             const char* const* items, size_t itemCount,
                             uint8_t* out, size_t outCap,
                             uint32_t widgetId = 10000,
                             const G2ContainerGeom& geom = G2_GEOM_LARGE);

// pb-body-only variant of g2BuildCreateListPage — emits the EvenCore
// CREATE_STARTUP_PAGE protobuf without wrapping it in an envelope. Caller
// is responsible for transport, including fragmentation if the body
// exceeds one envelope's u8 `len` byte (253 pb bytes per frag).
//
// The single-fragment envelope ceiling is what limits the existing
// g2BuildCreateListPage; lists with more than ~6–10 short rows can't fit.
// This entry point is used by the multi-fragment send path
// (G2_Glasses.cpp::sendPbFragmented) to support arbitrarily long
// lists without hitting that wall.
//
// Returns the pb body byte count, or 0 on buffer overflow.
size_t g2BuildCreateListPagePb(uint32_t magic,
                               const char* containerName,
                               const char* const* items, size_t itemCount,
                               uint32_t widgetId,
                               const G2ContainerGeom& geom,
                               uint8_t* pbOut, size_t pbCap);

// pb-body-only builder for CREATE_STARTUP_PAGE with a TextContainer
// (instead of ListContainer). Used to render free-flowing text on the
// lens — JSON dumps, debug spew, etc. — without the per-row selection
// borders that LIST widgets force on every line.
//
// `eventCapture=true` sets IsEventCapture=1 in the pb. The reference
// keeps it 0 ("we don't tap text areas") so this is a guess; if the
// firmware honors it we get TextEvent CLICK on tap and can route that
// to a back-handler. If it doesn't, callers should fall back to a
// state-event-driven exit (any sid=0x0D user-activity → return).
//
// Returns the pb body byte count, or 0 on buffer overflow.
size_t g2BuildCreateTextPagePb(uint32_t magic,
                               const char* containerName,
                               const char* content,
                               uint32_t widgetId,
                               const G2ContainerGeom& geom,
                               bool eventCapture,
                               uint8_t* pbOut, size_t pbCap);

// ── Multi-fragment transport (matches reference's framePb) ──────────────────
// Single-fragment envelopes can carry at most 253 pb bytes (u8 `len` field
// minus 2 CRC bytes). Anything larger must be split into multiple envelopes
// that share the same `seq` byte; the firmware reassembles by seq across
// fragments. CRC-16/CCITT-FALSE is computed over the concatenated pb body
// and appended LE only to the last fragment. Reference: ble/envelope.ts
// `framePb` in g2-kit-unofficial.
//
// Per-fragment pb chunk size, matching the reference's default. Chosen to
// stay well below typical BLE ATT MTU — even at MTU 23 the per-write
// payload is 20 B, and our sendEnvelope already does GATT-level chunking
// to bridge envelope→ATT-MTU; this constant is the protocol-level chunk
// size for the envelope `len` byte (which is u8, ceiling 255).
#define G2_FRAG_CHUNK_PB    232

// REBUILD_PAGE with a fresh ListContainerProperty — swap the items in an
// already-CREATEd list container. Used by stateful pages (Files, Settings)
// where each tap navigates / mutates the list contents in place.
//
// Note on geom: REBUILD reuses the rectangle the original CREATE established.
// The geom param here is included for symmetry with g2BuildCreateListPage,
// but the firmware ignores changes to it on the REBUILD path. To resize a
// container you must SHUTDOWN+CREATE (which is what g2ShowListPage does).
size_t g2BuildRebuildList(uint8_t seq, uint32_t magic,
                          const char* containerName,
                          const char* const* items, size_t itemCount,
                          uint8_t* out, size_t outCap,
                          const G2ContainerGeom& geom = G2_GEOM_LARGE);

// EvenCore UPDATE_TEXT (Cmd=5 = APP_UPDATE_TEXT_DATA_PACKET) for flicker-free
// in-place edits of an already-CREATEd text container. Reference:
// `buildTextUpgrade` in g2-kit-unofficial/ble/messages.ts.
size_t g2BuildUpdateText(uint8_t seq, uint32_t magic,
                         const char* containerName, uint32_t containerId,
                         const char* content,
                         uint8_t* out, size_t outCap);

// EvenCore MENU_STARTUP_FAILED (Cmd=18). Host → glasses response to a
// preceding OS_NOTIFY_MENU_STARTUP_PACKET (Cmd=17). Cancels a pending
// built-in mini-app launch so the firmware releases the widget slot and
// we can follow up with our own CREATE_STARTUP_PAGE for that widgetId.
// Fire-and-forget — the firmware does not ack this packet per the
// reference schema (ble/gen/EvenHub_pb.ts: MenuStartUpResPonse at wrapper
// field 21, only the FAILED variant exists in the Cmd enum).
//
// `errorCode` maps to EvenHub_ErrorCode_List. Any nonzero value cancels
// the launch; the specific code is advisory. `errorString` is optional
// diagnostic text — pass nullptr or "" to omit.
size_t g2BuildMenuStartupFailed(uint8_t seq, uint32_t magic,
                                uint32_t errorCode,
                                const char* errorString,
                                uint8_t* out, size_t outCap);

// EvenCore audio control. `enable` → AudoFuncEn 1/0.
size_t g2BuildAudioCtrl(uint8_t seq, uint32_t magic, bool enable,
                        uint8_t* out, size_t outCap);

// Even-AI builders (sid=0x07, EvenAIDataPackage). Both write a complete
// envelope ready for sendEnvelope. CTRL with status=ENTER opens the
// front-pane card; REPLY pushes text into it. Returns 0 on overflow.
//
// Caveat: the reference defines this schema but never exercises a
// host-driven REPLY flow. First-time use should be a one-shot — confirm
// the card actually appears before assuming streaming works.
size_t g2BuildEvenAICtrl(uint8_t seq, uint32_t magic, uint32_t status,
                         uint8_t* out, size_t outCap);
size_t g2BuildEvenAIAsk(uint8_t seq, uint32_t magic, uint32_t cmdCnt,
                        const char* text,
                        uint8_t* out, size_t outCap);
size_t g2BuildEvenAIAnalyse(uint8_t seq, uint32_t magic,
                            uint8_t* out, size_t outCap);
size_t g2BuildEvenAIReply(uint8_t seq, uint32_t magic, uint32_t cmdCnt,
                          const char* text, bool isLast,
                          uint8_t* out, size_t outCap);

// Even-AI CONFIG (Cmd=10). Schema fields not in the reference docs;
// empirically observed in g2-kit-unofficial as `voiceSwitch=field 1`,
// `streamSpeed=field 2`. Pass uint64_t max for "don't include this
// field" so the caller can probe individual fields. Returns 0 on
// overflow.
size_t g2BuildEvenAIConfig(uint8_t seq, uint32_t magic,
                           uint64_t voiceSwitch, uint64_t streamSpeed,
                           uint8_t* out, size_t outCap);

// EvenCore Cmd=3 (APP_UPDATE_IMAGE_RAW_DATA_PACKET) body. Carries one
// fragment of an image data stream inside ImgRawMsg (wrapper field 5).
// Schema verified 2026-04-26 against g2-kit-unofficial/ble/gen/EvenHub_pb.ts:
//   ImageRawDataUpdate {
//     uint32 ContainerID           = 1;  // matches CREATE'd container
//     string ContainerName         = 2;  // matches CREATE'd container
//     uint32 MapSessionId          = 3;  // bump per new image
//     uint32 MapTotalSize          = 4;  // total bytes of full pixel buffer
//     uint32 CompressMode          = 5;  // 0 = raw bytes
//     uint32 MapFragmentIndex      = 6;  // 0-based chunk in this session
//     uint32 MapFragmentPacketSize = 7;  // bytes in this chunk
//     bytes  MapRawData            = 8;  // pixel chunk
//   }
//
// The firmware reassembles by collecting all fragments of a session
// (matched by ContainerID/Name + MapSessionId) until MapTotalSize bytes
// arrive, then renders. This is image-LAYER fragmentation, separate from
// our envelope-level fragmentation (which kicks in when one of these
// per-chunk bodies happens to exceed ~250 pb bytes).
//
// Returns the pb body length written into `out`. The returned body
// needs `sendPbFragmented` to ship if it's larger than the single-frag
// ceiling, otherwise `g2BuildEnvelope` is fine.
size_t g2BuildImageRawBody(uint32_t magic,
                           uint32_t containerId,
                           const char* containerName,
                           uint32_t mapSessionId,
                           uint32_t mapTotalSize,
                           uint32_t mapFragmentIndex,
                           const uint8_t* data, size_t dataLen,
                           uint8_t* out, size_t outCap);

// EvenCore CREATE_STARTUP_PAGE (Cmd=0) carrying an
// ImageContainerProperty (wrapper field 4 of CreateStartUpPageContainer
// — same level as ListObject@f2 and TextObject@f3). Schema verified
// 2026-04-26 against g2-kit-unofficial/ble/gen/EvenHub_pb.ts:
//   ImageContainerProperty {
//     uint32 XPosition    = 1;
//     uint32 YPosition    = 2;
//     uint32 Width        = 3;
//     uint32 Height       = 4;
//     uint32 ContainerID  = 5;
//     string ContainerName = 6;
//   }
// No format/bpp/palette field exists — pixel encoding is implicit (4-bpp
// indexed per the doc's image-geometry section); the container is just
// a placement rectangle that subsequent Cmd=3 raw-data pushes target.
//
// Returns the envelope byte count, or 0 on overflow.
size_t g2BuildCreateImage(uint8_t seq, uint32_t magic,
                          const char* containerName, uint32_t containerId,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint32_t widgetId,
                          uint8_t* out, size_t outCap);

// pb-body-only variant — same as g2BuildCreateImage but writes only the
// EvenCore protobuf body (no envelope). Caller is responsible for
// transport (single-frag envelope or multi-frag). Returns pb body
// length, or 0 on overflow.
size_t g2BuildCreateImagePb(uint32_t magic,
                            const char* containerName, uint32_t containerId,
                            uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            uint32_t widgetId,
                            uint8_t* pbOut, size_t pbCap);

// Multi-tile CREATE_STARTUP_PAGE — one page containing N ImageObject
// children at distinct positions. Wraps the same CreateStartUpPageContainer
// shape as the single-tile builder but with f1=ContainerTotalNum=N and f4
// repeated N times. Used for full-display 576×288 = 2×2 tile layouts where
// the firmware expects all four tiles to be declared in a single CREATE
// (subsequent Cmd=3 pushes target each tile by its ContainerID/Name pair).
struct G2ImageTile {
  uint32_t x;
  uint32_t y;
  uint32_t w;
  uint32_t h;
  uint32_t containerId;
  const char* containerName;
};

size_t g2BuildCreateImageMultiPb(uint32_t magic,
                                 const G2ImageTile* tiles, size_t tileCount,
                                 uint32_t widgetId,
                                 uint8_t* pbOut, size_t pbCap);

size_t g2BuildCreateImageMulti(uint8_t seq, uint32_t magic,
                               const G2ImageTile* tiles, size_t tileCount,
                               uint32_t widgetId,
                               uint8_t* out, size_t outCap);

// Mixed-widget CREATE: one ListObject (f2) + one ImageObject (f4) in a
// single CreateStartUpPageContainer. Used by the Q16/Q17/Q18 probes to
// test whether the firmware accepts multi-type widget composition. The
// list and image can occupy any geometry (overlapping or not); render
// order / z-order is empirical territory — that's what the probes are
// for. Returns envelope length, or 0 on overflow.
size_t g2BuildCreateMixedListImagePb(uint32_t magic,
                                     const char* listName,
                                     const char* const* listItems,
                                     size_t listItemCount,
                                     const G2ContainerGeom& listGeom,
                                     const G2ImageTile& imageTile,
                                     uint32_t widgetId,
                                     uint8_t* pbOut, size_t pbCap);
size_t g2BuildCreateMixedListImage(uint8_t seq, uint32_t magic,
                                   const char* listName,
                                   const char* const* listItems,
                                   size_t listItemCount,
                                   const G2ContainerGeom& listGeom,
                                   const G2ImageTile& imageTile,
                                   uint32_t widgetId,
                                   uint8_t* out, size_t outCap);

// CreateStartUpPageContainer carrying N TextObject children (wrapper
// field 3 emitted N times, each with its own geometry + ContainerId +
// ContainerName + Content). Used to lay out side-by-side button
// affordances or any compound text layout that the single-TEXT widget
// can't express.
//
// SCHEMA RISK: the firmware has been verified to accept compound
// containers in the list+image shape (g2BuildCreateMixedListImagePb /
// Q16-Q18). Two TextObjects in the same wrapper is the same wire
// shape (repeated field 3) but not separately verified. Build it,
// ship it, watch for CreateResp res != 0 to detect rejection.
//
// Each child must have a UNIQUE containerId — the firmware uses that
// to disambiguate ListEvent / TextEvent reports if eventCapture were
// ever enabled (we leave it 0 here; these are display-only).
struct G2TextChildSpec {
  const char*       containerName;  // e.g. "btnYes" — short ASCII
  const char*       content;        // displayed text, may include \n
  uint32_t          containerId;    // 1..N, must be distinct per child
  G2ContainerGeom   geom;           // on-lens rectangle for THIS child
  bool              eventCapture;   // set TRUE to ask firmware to fire tap
                                    // events for this widget. The reference
                                    // says it's unused for text widgets but
                                    // the firmware empirically may emit
                                    // TextEvent CLICK with this CID — see
                                    // the Selection Patterns test bench's
                                    // canary for verification on 2.2.0.24.
};

size_t g2BuildCreateMultiTextPb(uint32_t magic,
                                const G2TextChildSpec* children,
                                size_t childCount,
                                uint32_t widgetId,
                                uint8_t* pbOut, size_t pbCap);
size_t g2BuildCreateMultiText(uint8_t seq, uint32_t magic,
                              const G2TextChildSpec* children,
                              size_t childCount,
                              uint32_t widgetId,
                              uint8_t* out, size_t outCap);

// REBUILD_PAGE counterpart for compound TextObject containers — sends
// all N children in a single REBUILD message. Required because per-
// child REBUILD-text on a compound blanks the other children on
// firmware 2.2.0.24. No WidgetId field; the widget binding is implicit
// on REBUILD against an existing container. See implementation comment
// for the schema-risk note.
size_t g2BuildRebuildMultiTextPb(uint32_t magic,
                                  const G2TextChildSpec* children,
                                  size_t childCount,
                                  uint8_t* pbOut, size_t pbCap);

// CREATE compound with 1 ListObject + N TextObject children. Used by
// Status' "<- Main Menu back row" + body/batt/meter compound. The list
// is tappable (eventCapture=1 inside writeListObjectWithItems); the
// text children are non-tappable panes. Each child needs a distinct
// ContainerId (list + texts share the cid space). REBUILD path MUST
// include the list child too — see g2BuildRebuildMixedListMultiTextPb
// — because multi-child REBUILD-text blanks any unmentioned siblings,
// regardless of widget type.
size_t g2BuildCreateMixedListMultiTextPb(uint32_t magic,
                                          const char* listName,
                                          const char* const* listItems,
                                          size_t listItemCount,
                                          const G2ContainerGeom& listGeom,
                                          const G2TextChildSpec* textChildren,
                                          size_t textChildCount,
                                          uint32_t widgetId,
                                          uint8_t* pbOut, size_t pbCap);

// REBUILD_PAGE counterpart — same shape as the CREATE above (cmd=7,
// G2_WRAP_F_REBUILD wrapper, no WidgetId). All children must be listed
// every REBUILD or unmentioned siblings go dark.
size_t g2BuildRebuildMixedListMultiTextPb(uint32_t magic,
                                           const char* listName,
                                           const char* const* listItems,
                                           size_t listItemCount,
                                           const G2ContainerGeom& listGeom,
                                           const G2TextChildSpec* textChildren,
                                           size_t textChildCount,
                                           uint8_t* pbOut, size_t pbCap);

// CreateStartUpPage with one ListObject + one TextObject child. The
// canonical "title + selectable list" layout: the TextObject acts as a
// non-tappable header (e.g. "Save changes?"), the ListObject carries N
// selectable rows that the firmware natively manages focus + scroll +
// CLICK events for. Title geom is typically G2_GEOM_STATUS_BAR or
// G2_GEOM_TOP_HALF; list geom typically G2_GEOM_BOTTOM_HALF or
// G2_GEOM_LARGE depending on how prominent you want the title to be.
//
// Schema: f1=ContainerTotalNum=2, f2=ListObject, f3=TextObject,
// f5=WidgetId. Same compound-widget pattern as
// g2BuildCreateMixedListImage but with TextObject (f3) instead of
// ImageObject (f4). Verified working on firmware 2.2.0.24 (see
// G2_PROTOCOL.md "Mixed-widget composition" section).
size_t g2BuildCreateMixedListTextPb(uint32_t magic,
                                    const char* listName,
                                    const char* const* listItems,
                                    size_t listItemCount,
                                    const G2ContainerGeom& listGeom,
                                    const G2TextChildSpec& textSpec,
                                    uint32_t widgetId,
                                    uint8_t* pbOut, size_t pbCap);
size_t g2BuildCreateMixedListText(uint8_t seq, uint32_t magic,
                                  const char* listName,
                                  const char* const* listItems,
                                  size_t listItemCount,
                                  const G2ContainerGeom& listGeom,
                                  const G2TextChildSpec& textSpec,
                                  uint32_t widgetId,
                                  uint8_t* out, size_t outCap);

// Decode the trailing field 15 sub-message of a HeartbeatAck pb body.
// Returns true if the sub-message was found AND both inner fields
// parsed; writes the seq counter (inner f1) to *seq and the cmd echo
// (inner f2) to *echo. Inputs may be null. Body shape on this firmware:
//   08 0C 10 CD 01 7A 04 08 NN 10 0C
//   ^cmd  ^magic    ^^^^len 4 sub-msg
bool g2DecodeHeartbeatAckTail(const uint8_t* pb, size_t pbLen,
                              uint64_t* seq, uint64_t* echo);

// The app-launch prelude, copied verbatim from the reference. Single fixed
// byte sequence — we do not rebuild the envelope for this one since the
// inner protobuf structure is not fully understood. Must be the first write
// after every fresh BLE connect.
size_t g2BuildAppLaunch(uint8_t* out, size_t outCap);

// Settings request — battery etc.
size_t g2BuildSettingBasicRequest(uint8_t seq, uint32_t magic,
                                  uint8_t* out, size_t outCap);

// ── Per-sid wire stats (silent counters — no log spam) ───────────────────
// Tracks every sid we've TX'd to or RX'd from this session. Used by the
// `g2protostats` CLI to show what protocol surface is live without us
// having to grep through giant ring-buffer dumps. Update hooks live in
// sendEnvelope (TX) and handleEnvelope (RX) so any new code path that
// goes through those gets tracked automatically.
struct G2SidStat {
  uint8_t  sid;
  uint32_t txCount;
  uint32_t rxCount;
  uint32_t lastRxMs;
  uint32_t lastTxMs;
  uint8_t  lastFlag;       // last RX flag byte
  uint16_t lastPbLen;      // last RX payload length
  uint8_t  lastSample[8];  // first 8 bytes of last RX payload
  uint8_t  lastSampleLen;
};

void   g2statsRecordTx(uint8_t sid, uint8_t flag, size_t pbLen);
void   g2statsRecordRx(uint8_t sid, uint8_t flag, const uint8_t* pb, size_t pbLen);
size_t g2statsCount();
const G2SidStat* g2statsAt(size_t idx);  // returns nullptr past the end
void   g2statsReset();

// Static reference: human-friendly name for each known sid. Returns
// "Unknown" for ids we haven't catalogued.
const char* g2sidName(uint8_t sid);
// Field numbers inside the DeviceReceiveRequestFromAPP body (field 4 of
// the wrapper, carried in every sid=0x09 push). Only the ones we've
// empirically identified are named; unknowns are numbered but not
// labelled. Exposed here so the verbose-dump callback in G2_Glasses
// can label known fields without hard-coding the numbers.
#define G2_SET_REQ_F_VER   5   // string — firmware version (e.g. "2.1.1.10")
#define G2_SET_REQ_F_BATT  12  // uint32 — battery percentage (0-100)

// Outer-wrapper field carrying device→app async settings telemetry
// (`G2SettingPackage.deviceSendInfoToApp`, used when commandId=3
// DeviceSendToAPP). Distinct from f4 deviceReceiveRequestFromApp where
// battery/version live.
#define G2_SET_F_SEND_INFO    5

// Inner field of `deviceSendInfoToApp` carrying the silent-mode flag
// (firmware → app push when the user toggles silent / DND via tap-and-
// hold both temples). 0 = silent off, 1 = silent on. Schema verified
// 2026-04-26 from a labelled capture; not in the local doc but the
// firmware reliably emits it on every silent-mode state change.
#define G2_SET_SEND_F_SILENT  2

bool g2ParseSettingBattery(const uint8_t* payload, size_t payloadLen,
                           uint8_t* batteryPct);

// Parse the silent-mode flag from a settings push. Returns true and
// writes 0 or 1 to *outFlag if the push carries a
// `deviceSendInfoToApp.silentMode` value (firmware→app at sid=0x09
// cmd=3). False if the push doesn't carry this field — most settings
// pushes (battery ticks etc.) don't, so callers should treat false as
// "no state change to report" rather than "silent mode is unknown".
bool g2ParseSettingSilentMode(const uint8_t* payload, size_t payloadLen,
                              uint8_t* outFlag);

// Pull the firmware version string from a sid=0x09 flag=0x01 push. Observed
// at field 5 of the DeviceReceiveRequestFromAPP body as ASCII (e.g.
// "2.1.1.10"). Returns false if no version string is present in this
// particular push — the firmware only includes it in some settings
// frames, so callers should treat a false return as "not in this frame"
// rather than "glasses have no version."
bool g2ParseSettingVersion(const uint8_t* payload, size_t payloadLen,
                           char* versionOut, size_t versionCap);

// Diagnostic sid=0x09 inner-field iterator. Calls `logFn` once per
// decoded field inside the DeviceReceiveRequestFromAPP body with
// (field_number, wire_type, varint_value, len_delim_bytes, len_delim_len).
// Use it to discover what unlabelled fields carry — varintVal is 0 for
// non-varint fields, bytes/byteLen populated for len-delim fields.
// Pass nullptr to no-op.
typedef void (*G2SettingsFieldLog)(uint32_t field, uint8_t wire,
                                   uint64_t varintVal,
                                   const uint8_t* bytes, size_t byteLen);

void g2DumpSettingFields(const uint8_t* payload, size_t payloadLen,
                         G2SettingsFieldLog logFn);

// =============================================================================
// DevConfig builders — sid=0x80 (UX_DEVICE_SETTINGS_APP_ID)
// =============================================================================
// Each builder constructs a full G2 envelope (preamble..CRC) carrying a
// well-formed `DevCfgDataPackage` protobuf with a HARDCODED commandId.
// Caller cannot inject an arbitrary cmd value — the only way to send the
// destructive ones (cmd=11 SET_DEVICE_INFO, cmd=13 RESTORE_TO_FACTORY_SETTINGS)
// is to write a new builder, which forces conscious code review.
//
// These bypass the `g2probe` block (which defends only the raw-hex-from-CLI
// path; see G2_Glasses.cpp:9141). The block intentionally stays in place.
//
// Schema reconstructed from the FlutterApp community RE project — see
// docs/g2_proto/dev_config_protocol.proto and Ring_Bridge_Sequence.h.
// **Test on a recoverable unit before shipping.** The brick incident that
// triggered the original blocklist was non-terminal (recoverable via case
// factory-reset) per G2_Glasses.cpp:9105-9106, but downtime is downtime.

#define G2_SID_DEV_CONFIG               0x80  // canonical SID for DevCfgDataPackage

// DevCfgDataPackage commandId values (matches eDevCfgCommandId in
// docs/g2_proto/dev_config_protocol.proto). Only the SAFE-tier opcodes
// have builders below. Soft-deny tier (UNPAIR=9, RESTART=15) and hard-deny
// tier (SET_DEVICE_INFO=11, RESTORE_TO_FACTORY_SETTINGS=13) intentionally
// have no builder — adding one requires an explicit code change.
#define G2_DEVCFG_CMD_AUTHENTICATION         4
#define G2_DEVCFG_CMD_PIPE_ROLE_CHANGE       5
#define G2_DEVCFG_CMD_RING_CONNECT_INFO      6
#define G2_DEVCFG_CMD_BASE_HEART_BEAT        14
#define G2_DEVCFG_CMD_TIME_SYNC              128

// PipeRoleChange.asCmdRole values (eGlassesLR enum)
#define G2_DEVCFG_ROLE_BOTH   0
#define G2_DEVCFG_ROLE_RIGHT  1
#define G2_DEVCFG_ROLE_LEFT   2

// AuthMgr.phoneType values (eDevice enum). We always send PHONE_ANDROID
// to mirror FlutterApp's setting.
#define G2_DEVCFG_PHONE_TYPE_ANDROID  4

// Magic-correlation constants for DevConfig messages. Chosen distinct from
// the EvenCore G2_MAGIC_* set above so DevConfig acks are unambiguous in
// the reply ring buffer.
#define G2_MAGIC_DEVCFG_HEARTBEAT      220
#define G2_MAGIC_DEVCFG_AUTH           221
#define G2_MAGIC_DEVCFG_TIME_SYNC      222
#define G2_MAGIC_DEVCFG_RING_CONNECT   223
#define G2_MAGIC_DEVCFG_PIPE_ROLE      224

// BASE_CONNECT_HEART_BEAT (cmd=14). Empty body. Safest possible message —
// no payload to corrupt. Use this first when validating that sid=0x80 TX
// is non-bricking on a given firmware revision.
size_t g2BuildDevCfgHeartbeat(uint8_t seq, uint32_t magic,
                              uint8_t* out, size_t outCap);

// AUTHENTICATION (cmd=4). Sends fixed payload: AuthMgr {
//   secAuth=true, phoneType=PHONE_ANDROID
// }. No caller-supplied data. Always safe within FlutterApp's known-good
// envelope shape.
size_t g2BuildDevCfgAuth(uint8_t seq, uint32_t magic,
                         uint8_t* out, size_t outCap);

// PIPE_ROLE_CHANGE (cmd=5). `role` must be one of
// G2_DEVCFG_ROLE_{BOTH,RIGHT,LEFT}; out-of-range values are rejected
// (returns 0). FlutterApp only ever sends RIGHT, to the right arm — we
// match that convention but expose all three values for experimentation.
size_t g2BuildDevCfgPipeRoleChange(uint8_t seq, uint32_t magic, uint8_t role,
                                   uint8_t* out, size_t outCap);

// TIME_SYNC (cmd=128). `timestamp` is unix seconds (s32). `tzQuarterHours`
// is the UTC offset in QUARTER-HOURS — i.e. minutes/15 — to match FlutterApp's
// g2_messages.dart:50 (`tzQuarterHours = inMinutes ~/ 15`). Range checked:
//   timestamp ∈ [1577836800, 4102444800]    (2020-01-01 .. 2099-12-31)
//   tzQuarterHours ∈ [-56, +56]             (±14 h, real-world max)
// Returns 0 on validation failure.
//
// **NB**: R1 systemTime uses RAW MINUTES (not quarter-hours) for the same
// concept. See R1 builders / docs/g2_proto/Ring_Bridge_Sequence.h Phase 2.2.
size_t g2BuildDevCfgTimeSync(uint8_t seq, uint32_t magic,
                             uint32_t timestamp, int32_t tzQuarterHours,
                             uint8_t* out, size_t outCap);

// RING_CONNECT_INFO (cmd=6). Establishes (or tears down) the ring → glasses
// bridge. After a `connect=true` send lands, ring telemetry
// (battery/HR/SpO2/HRV/temp/kcal/steps) starts arriving on sid=0x90
// (UX_RING_ROW_DATA_ID) and ring events on sid=0x91 (UX_RING_DATA_RELAY_ID),
// both as RingDataPackage protobuf — see docs/g2_proto/ring.proto.
//
//   `connect` — true asks the right temple to scan-and-bond with the named
//      ring (and start forwarding telemetry); false asks it to release the
//      ring so another central (e.g. us) can grab it. The MAC + name are
//      still required on release so the temple knows which bonded ring to
//      drop — empty MAC has been observed to be ignored.
//   `ringMacBleOrder` — 6 bytes in BLE address order (the order
//      esp_bd_addr_t / NimBLEAddress give you). The builder REVERSES them
//      internally to match FlutterApp's g2_messages.dart:109.
//   `ringName` — UTF-8 ring name (e.g. "EVEN R1_112233"). NOT
//      null-terminated on the wire — caller passes a normal C string and
//      the builder strips the terminator.
//
// Returns 0 if `ringMacBleOrder` is null, or `ringName` is null/empty,
// or `ringName` length > 32.
size_t g2BuildDevCfgRingConnect(uint8_t seq, uint32_t magic,
                                bool connect,
                                const uint8_t* ringMacBleOrder,
                                const char* ringName,
                                uint8_t* out, size_t outCap);

// =============================================================================
// Ring data push (sid=0x90 UX_RING_ROW_DATA_ID)
// =============================================================================
// **WARNING — direction matters.** sid=0x90 traffic in the official protocol
// flows GLASSES → HOST only: once the right temple has bridged the ring
// (via sid=0x80 cmd=6 RING_CONNECT_INFO; see `ringbridge` CLI), the temple
// itself emits RingDataPackage frames on sid=0x90 and forwards them to us.
// **Sending sid=0x90 in the OPPOSITE direction (host → temple) is silently
// ignored** — verified empirically 2026-05-02 with 51-byte well-formed
// RingDataPackage{rawData{...}} frames that produced zero ack, zero error,
// and no UI change on firmware 2.2.0.24.
//
// `g2BuildRingRawDataPush()` is preserved for **custom display experiments**
// (e.g. crafting frames the firmware might consume in some other context),
// not for driving the official health UI. To drive that UI, use the
// `ringbridge on` CLI which sends sid=0x80 cmd=6 RING_CONNECT_INFO and
// hands the ring off to the temple's bridge firmware.
//
// Schema: see docs/g2_proto/ring.proto. We pack a `RingDataPackage` with
// commandId=RAW_DATA(2), magicRandom set to a per-session correlation
// token, and rawData populated with whatever fields the caller has fresh.
//
// Field-presence semantics: pass `_valid=false` to OMIT a field from the
// proto entirely (proto3 default-suppression). The glasses' widget treats
// missing fields as "no fresh value" and keeps the previous reading on
// screen. So you can update HR every 30s and SpO2 every 5min by leaving
// SpO2 invalid on the HR-only ticks.
//
// Timestamps are unix seconds. Values that exceed int32_t range or come
// from "no data yet" cached state must be flagged invalid by the caller.
//
// SID and magic constants documented alongside the existing G2_SID_* /
// G2_MAGIC_DEVCFG_* pairs.
#define G2_SID_RING_RAW_DATA           0x90  // canonical SID for RingDataPackage rawData
#define G2_SID_RING_DATA_RELAY         0x91  // canonical SID for RingDataPackage event
#define G2_RING_CMD_NONE               0
#define G2_RING_CMD_EVENT              1
#define G2_RING_CMD_RAW_DATA           2
#define G2_MAGIC_RING_RAW_PUSH         225   // chosen distinct from G2_MAGIC_DEVCFG_*

struct G2RingPushFields {
  // Each field is sent only when its `_valid` flag is true.
  bool      battery_valid;       int32_t battery;
  bool      chargeStates_valid;  int32_t chargeStates;
  bool      hr_valid;            int32_t hr;            int32_t hrTs;
  bool      spo2_valid;          int32_t spo2;          int32_t spo2Ts;
  bool      hrv_valid;           int32_t hrv;           int32_t hrvTs;
  bool      temp_valid;          int32_t temp;          int32_t tempTs;
  bool      actKcal_valid;       int32_t actKcal;       int32_t actKcalTs;
  bool      allKcal_valid;       int32_t allKcal;       int32_t allKcalTs;
  bool      steps_valid;         int32_t steps;         int32_t stepsTs;
};

size_t g2BuildRingRawDataPush(uint8_t seq, uint32_t magic,
                              const G2RingPushFields& f,
                              uint8_t* out, size_t outCap);

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
#endif  // SYSTEM_G2_PROTOCOL_H
