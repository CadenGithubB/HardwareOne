#include "System_G2_Protocol.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include <Arduino.h>     // millis()
#include <string.h>

// ── CRC-16/CCITT-FALSE ───────────────────────────────────────────────────────

uint16_t g2CrcCcittFalse(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= ((uint16_t)data[i]) << 8;
    for (int b = 0; b < 8; b++) {
      if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else              crc <<= 1;
    }
  }
  return crc;
}

// ── Envelope ─────────────────────────────────────────────────────────────────

size_t g2BuildEnvelope(uint8_t seq, uint8_t sid, uint8_t flag,
                       const uint8_t* payload, size_t payloadLen,
                       uint8_t* out, size_t outCap) {
  // Single-fragment only. pb bytes + 2 CRC bytes must fit in the u8 len field.
  if (payloadLen + G2_ENVELOPE_CRC_LEN > 0xFF) return 0;
  const size_t total = G2_ENVELOPE_HDR_LEN + payloadLen + G2_ENVELOPE_CRC_LEN;
  if (total > outCap) return 0;

  out[0] = G2_PREAMBLE_0;
  out[1] = G2_PREAMBLE_TX;
  out[2] = seq;
  out[3] = (uint8_t)(payloadLen + G2_ENVELOPE_CRC_LEN);
  out[4] = 1;              // totalFrags
  out[5] = 1;              // fragIdx (1-based)
  out[6] = sid;
  out[7] = flag;
  if (payload && payloadLen > 0) {
    memcpy(out + G2_ENVELOPE_HDR_LEN, payload, payloadLen);
  }
  // CRC over JUST the pb payload (not the header). Emitted little-endian
  // (low byte first).
  uint16_t crc = g2CrcCcittFalse(payload, payloadLen);
  out[G2_ENVELOPE_HDR_LEN + payloadLen]     = (uint8_t)(crc & 0xFF);
  out[G2_ENVELOPE_HDR_LEN + payloadLen + 1] = (uint8_t)((crc >> 8) & 0xFF);
  return total;
}

bool g2ParseEnvelope(const uint8_t* in, size_t len, G2EnvelopeView* out) {
  if (!in || !out) return false;
  if (len < G2_ENVELOPE_HDR_LEN + G2_ENVELOPE_CRC_LEN) return false;
  if (in[0] != G2_PREAMBLE_0) return false;
  if (in[1] != G2_PREAMBLE_TX && in[1] != G2_PREAMBLE_RX) return false;

  const uint8_t declared = in[3];
  // declared = fragment data length (pb bytes on this frag + CRC if last).
  // For single-fragment messages declared = payload + 2.
  if ((size_t)declared + G2_ENVELOPE_HDR_LEN > len) return false;
  const uint8_t totFrags = in[4];
  const uint8_t fragIdx  = in[5];
  if (totFrags == 0 || fragIdx == 0 || fragIdx > totFrags) return false;

  const bool isLast = (fragIdx == totFrags);
  const size_t pbLen = isLast ? (size_t)declared - G2_ENVELOPE_CRC_LEN
                              : (size_t)declared;
  const uint8_t* pb = in + G2_ENVELOPE_HDR_LEN;

  if (isLast) {
    // CRC check — LE, over just the pb bytes.
    const uint16_t rcv = (uint16_t)pb[pbLen] | ((uint16_t)pb[pbLen + 1] << 8);
    const uint16_t calc = g2CrcCcittFalse(pb, pbLen);
    if (rcv != calc) return false;
  }

  out->isTx       = (in[1] == G2_PREAMBLE_TX);
  out->seq        = in[2];
  out->totalFrags = totFrags;
  out->fragIdx    = fragIdx;
  out->sid        = in[6];
  out->flag       = in[7];
  out->payload    = pbLen ? pb : nullptr;
  out->payloadLen = pbLen;
  return true;
}

// ── Protobuf primitives ──────────────────────────────────────────────────────

bool g2PbWriteVarint(uint8_t* buf, size_t cap, size_t* pos, uint64_t v) {
  while (v >= 0x80) {
    if (*pos >= cap) return false;
    buf[(*pos)++] = (uint8_t)(v & 0x7F) | 0x80;
    v >>= 7;
  }
  if (*pos >= cap) return false;
  buf[(*pos)++] = (uint8_t)v;
  return true;
}

bool g2PbWriteTag(uint8_t* buf, size_t cap, size_t* pos, uint32_t field, uint8_t wire) {
  return g2PbWriteVarint(buf, cap, pos, ((uint64_t)field << 3) | (wire & 0x7));
}

bool g2PbWriteUint32(uint8_t* buf, size_t cap, size_t* pos, uint32_t field, uint32_t v) {
  if (!g2PbWriteTag(buf, cap, pos, field, G2_PB_WIRE_VARINT)) return false;
  return g2PbWriteVarint(buf, cap, pos, v);
}

bool g2PbWriteBool(uint8_t* buf, size_t cap, size_t* pos, uint32_t field, bool v) {
  return g2PbWriteUint32(buf, cap, pos, field, v ? 1u : 0u);
}

bool g2PbWriteString(uint8_t* buf, size_t cap, size_t* pos, uint32_t field, const char* s) {
  const size_t slen = s ? strlen(s) : 0;
  return g2PbWriteBytes(buf, cap, pos, field, (const uint8_t*)s, slen);
}

bool g2PbWriteBytes(uint8_t* buf, size_t cap, size_t* pos, uint32_t field,
                    const uint8_t* data, size_t len) {
  if (!g2PbWriteTag(buf, cap, pos, field, G2_PB_WIRE_LEN_DELIM)) return false;
  if (!g2PbWriteVarint(buf, cap, pos, (uint64_t)len)) return false;
  if (len > 0) {
    if (*pos + len > cap) return false;
    memcpy(buf + *pos, data, len);
    *pos += len;
  }
  return true;
}

static constexpr size_t NESTED_LEN_RESERVE = 2;

bool g2PbBeginNested(uint8_t* buf, size_t cap, size_t* pos,
                     uint32_t field, size_t* innerStart) {
  if (!g2PbWriteTag(buf, cap, pos, field, G2_PB_WIRE_LEN_DELIM)) return false;
  if (*pos + NESTED_LEN_RESERVE > cap) return false;
  *pos += NESTED_LEN_RESERVE;
  *innerStart = *pos;
  return true;
}

bool g2PbEndNested(uint8_t* buf, size_t cap, size_t* pos, size_t innerStart) {
  (void)cap;
  const size_t innerLen = *pos - innerStart;
  if (innerLen > 0x3FFF) return false;
  // Canonical varint: shift content back by 1 if a single byte suffices,
  // so strict decoders don't reject trailing 0-byte continuations.
  if (innerLen < 0x80) {
    buf[innerStart - 2] = (uint8_t)innerLen;
    if (innerLen > 0) {
      memmove(buf + innerStart - 1, buf + innerStart, innerLen);
    }
    *pos -= 1;
  } else {
    buf[innerStart - 2] = (uint8_t)((innerLen & 0x7F) | 0x80);
    buf[innerStart - 1] = (uint8_t)((innerLen >> 7) & 0x7F);
  }
  return true;
}

bool g2PbReadVarint(const uint8_t* buf, size_t len, size_t* pos, uint64_t* v) {
  uint64_t acc = 0;
  int shift = 0;
  while (*pos < len) {
    uint8_t b = buf[(*pos)++];
    acc |= ((uint64_t)(b & 0x7F)) << shift;
    if ((b & 0x80) == 0) {
      *v = acc;
      return true;
    }
    shift += 7;
    if (shift >= 64) return false;
  }
  return false;
}

bool g2PbReadTag(const uint8_t* buf, size_t len, size_t* pos,
                 uint32_t* field, uint8_t* wire) {
  uint64_t tag = 0;
  if (!g2PbReadVarint(buf, len, pos, &tag)) return false;
  *field = (uint32_t)(tag >> 3);
  *wire  = (uint8_t)(tag & 0x7);
  return true;
}

bool g2PbSkipField(const uint8_t* buf, size_t len, size_t* pos, uint8_t wire) {
  switch (wire) {
    case G2_PB_WIRE_VARINT: {
      uint64_t dummy;
      return g2PbReadVarint(buf, len, pos, &dummy);
    }
    case G2_PB_WIRE_FIXED64:
      if (*pos + 8 > len) return false;
      *pos += 8;
      return true;
    case G2_PB_WIRE_LEN_DELIM: {
      uint64_t sz;
      if (!g2PbReadVarint(buf, len, pos, &sz)) return false;
      if (*pos + sz > len) return false;
      *pos += (size_t)sz;
      return true;
    }
    case G2_PB_WIRE_FIXED32:
      if (*pos + 4 > len) return false;
      *pos += 4;
      return true;
    default:
      return false;
  }
}

// ── EvenCore wrapper + nested-message field numbers ─────────────────────────
// "EvenCore" is our name for the sid=0xE0 rendering subsystem; the firmware's
// pb schema calls it `evenhub_main_msg_ctx` (see ble/gen/EvenHub_pb.ts in the
// reference). The firmware identifiers in comments below are kept verbatim
// so they grep cleanly against the upstream source.
#define G2_WRAP_F_CMD            1
#define G2_WRAP_F_MAGIC          2
#define G2_WRAP_F_CREATE         3
#define G2_WRAP_F_IMAGE          5
#define G2_WRAP_F_REBUILD        7
#define G2_WRAP_F_TEXT_UPGRADE   9
#define G2_WRAP_F_SHUTDOWN       11
#define G2_WRAP_F_HEARTBEAT      14
#define G2_WRAP_F_AUDIO          18
#define G2_WRAP_F_MENU_RES       21   // MenuStartUpResPonse (Cmd=18 payload)

// MenuStartUpResPonse inner fields (ble/gen/EvenHub_pb.ts:186,191).
#define G2_MENURES_F_CODE        1    // uint32 errorCode
#define G2_MENURES_F_STR         2    // string  errorString

// TextContainerProperty
#define G2_TEXT_F_X         1
#define G2_TEXT_F_Y         2
#define G2_TEXT_F_W         3
#define G2_TEXT_F_H         4
#define G2_TEXT_F_CID       9
#define G2_TEXT_F_CNAME     10
#define G2_TEXT_F_EVCAP     11
#define G2_TEXT_F_CONTENT   12

// RebuildPageContainer / CreateStartUpPageContainer (share TextObject
// layout, differ in which bodies are repeated). Field numbers from
// `ble/gen/EvenHub_pb.ts` in the reference.
#define G2_PAGE_F_TOTAL     1
#define G2_PAGE_F_LIST_OBJ  2
#define G2_PAGE_F_TEXT_OBJ  3
#define G2_PAGE_F_WIDGET_ID 5  // only meaningful in CreateStartUpPageContainer

// ListContainerProperty (used by CREATE for launcher-style list pages).
#define G2_LIST_F_X         1
#define G2_LIST_F_Y         2
#define G2_LIST_F_W         3
#define G2_LIST_F_H         4
#define G2_LIST_F_CID       9
#define G2_LIST_F_CNAME    10
#define G2_LIST_F_ITEMS    11  // nested List_ItemContainerProperty
#define G2_LIST_F_EVCAP   12

// List_ItemContainerProperty (repeated ItemName = 4).
#define G2_ITEM_F_COUNT     1
#define G2_ITEM_F_SEL_BORDR 3
#define G2_ITEM_F_NAME      4

// TextContainerUpgrade (body for Cmd=5 APP_UPDATE_TEXT_DATA_PACKET).
#define G2_TXUPG_F_CID      1
#define G2_TXUPG_F_CNAME    2
#define G2_TXUPG_F_OFFSET   3
#define G2_TXUPG_F_LENGTH   4
#define G2_TXUPG_F_CONTENT  5

// HeartBeatPacket
#define G2_HB_F_CNT         1
// ShutDownContaniner
#define G2_SHUT_F_MODE      1
// AudioCtrCmd
#define G2_AUDIO_F_EN       1

// Default text container geometry (576×288 full lens, LVGL 50×10 grid).
static constexpr uint32_t TEXT_X = 0;
static constexpr uint32_t TEXT_Y = 0;
static constexpr uint32_t TEXT_W = 576;
static constexpr uint32_t TEXT_H = 288;
static constexpr uint32_t TEXT_CID = 1;

// ── High-level builders ──────────────────────────────────────────────────────

size_t g2BuildHeartbeat(uint8_t seq, uint32_t magic, uint32_t cnt,
                        uint8_t* out, size_t outCap) {
  uint8_t payload[32];
  size_t pos = 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_WRAP_F_CMD, G2_CMD_HEARTBEAT)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t inner;
  if (!g2PbBeginNested(payload, sizeof(payload), &pos, G2_WRAP_F_HEARTBEAT, &inner)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_HB_F_CNT, cnt)) return 0;
  if (!g2PbEndNested(payload, sizeof(payload), &pos, inner)) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_CORE, G2_FLAG_REQUEST,
                         payload, pos, out, outCap);
}

size_t g2BuildMenuStartupFailed(uint8_t seq, uint32_t magic,
                                uint32_t errorCode,
                                const char* errorString,
                                uint8_t* out, size_t outCap) {
  uint8_t payload[96];
  size_t pos = 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_WRAP_F_CMD, G2_CMD_MENU_FAILED)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t inner;
  if (!g2PbBeginNested(payload, sizeof(payload), &pos, G2_WRAP_F_MENU_RES, &inner)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_MENURES_F_CODE, errorCode)) return 0;
  if (errorString && errorString[0]) {
    if (!g2PbWriteString(payload, sizeof(payload), &pos, G2_MENURES_F_STR, errorString)) return 0;
  }
  if (!g2PbEndNested(payload, sizeof(payload), &pos, inner)) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_CORE, G2_FLAG_REQUEST,
                         payload, pos, out, outCap);
}

size_t g2BuildShutdown(uint8_t seq, uint32_t magic, uint32_t exitMode,
                       uint8_t* out, size_t outCap) {
  uint8_t payload[32];
  size_t pos = 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_WRAP_F_CMD, G2_CMD_SHUTDOWN_PAGE)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t inner;
  if (!g2PbBeginNested(payload, sizeof(payload), &pos, G2_WRAP_F_SHUTDOWN, &inner)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_SHUT_F_MODE, exitMode)) return 0;
  if (!g2PbEndNested(payload, sizeof(payload), &pos, inner)) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_CORE, G2_FLAG_REQUEST,
                         payload, pos, out, outCap);
}

// Default-geometry version: keeps the legacy callers (g2BuildRebuildText
// and g2BuildCreateStartupPage's single-text-line form) working without
// requiring them to know about G2ContainerGeom.
static bool writeTextProperty(uint8_t* buf, size_t cap, size_t* pos,
                              const char* containerName, const char* content) {
  size_t inner;
  if (!g2PbBeginNested(buf, cap, pos, G2_PAGE_F_TEXT_OBJ, &inner)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_X, TEXT_X)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_Y, TEXT_Y)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_W, TEXT_W)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_H, TEXT_H)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_CID, TEXT_CID)) return false;
  if (!g2PbWriteString(buf, cap, pos, G2_TEXT_F_CNAME, containerName)) return false;
  // Reference uses IsEventCapture=0 for text containers ("we don't tap text areas").
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_EVCAP, 0)) return false;
  if (!g2PbWriteString(buf, cap, pos, G2_TEXT_F_CONTENT, content ? content : "")) return false;
  return g2PbEndNested(buf, cap, pos, inner);
}

// Geom-aware variant. Used by the multi-page Settings JSON view and
// any future caller that wants the TEXT widget at a specific on-lens
// rectangle. `eventCapture=true` sets IsEventCapture=1, which the
// reference says is unused for text ("we don't tap text areas") — but
// we set it anyway in the hope that the firmware *does* fire
// TextEvent CLICK when the user taps a captured text container. If
// it doesn't, the caller falls back to user-activity-driven exit
// (see G2_Glasses.cpp's TEXT exit path).
static bool writeTextPropertyGeom(uint8_t* buf, size_t cap, size_t* pos,
                                   const char* containerName,
                                   const char* content,
                                   const G2ContainerGeom& geom,
                                   bool eventCapture) {
  size_t inner;
  if (!g2PbBeginNested(buf, cap, pos, G2_PAGE_F_TEXT_OBJ, &inner)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_X, geom.x)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_Y, geom.y)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_W, geom.w)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_H, geom.h)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_CID, TEXT_CID)) return false;
  if (!g2PbWriteString(buf, cap, pos, G2_TEXT_F_CNAME, containerName)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_EVCAP, eventCapture ? 1u : 0u)) return false;
  if (!g2PbWriteString(buf, cap, pos, G2_TEXT_F_CONTENT, content ? content : "")) return false;
  return g2PbEndNested(buf, cap, pos, inner);
}

size_t g2BuildCreateTextPagePb(uint32_t magic,
                               const char* containerName,
                               const char* content,
                               uint32_t widgetId,
                               const G2ContainerGeom& geom,
                               bool eventCapture,
                               uint8_t* pbOut, size_t pbCap) {
  size_t pos = 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_CMD, G2_CMD_CREATE_STARTUP)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t pageStart;
  if (!g2PbBeginNested(pbOut, pbCap, &pos, G2_WRAP_F_CREATE, &pageStart)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_TOTAL, 1)) return 0;
  // TextObject (wrapper field 3). Same widget the firmware uses for its
  // own text overlays; we just supply our own content + geom.
  if (!writeTextPropertyGeom(pbOut, pbCap, &pos,
                              containerName, content, geom, eventCapture)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_WIDGET_ID, widgetId)) return 0;
  if (!g2PbEndNested(pbOut, pbCap, &pos, pageStart)) return 0;
  return pos;
}

size_t g2BuildRebuildText(uint8_t seq, uint32_t magic,
                          const char* containerName,
                          const char* content,
                          uint8_t* out, size_t outCap) {
  uint8_t payload[256];
  size_t pos = 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_WRAP_F_CMD, G2_CMD_REBUILD_PAGE)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t pageStart;
  if (!g2PbBeginNested(payload, sizeof(payload), &pos, G2_WRAP_F_REBUILD, &pageStart)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_PAGE_F_TOTAL, 1)) return 0;
  if (!writeTextProperty(payload, sizeof(payload), &pos, containerName, content)) return 0;
  if (!g2PbEndNested(payload, sizeof(payload), &pos, pageStart)) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_CORE, G2_FLAG_REQUEST,
                         payload, pos, out, outCap);
}

// pb-only variant of g2BuildRebuildText — emits the RebuildPageContainer
// pb body without wrapping it in an envelope. The caller ships via
// sendPbFragmented, which does protocol-level multi-fragment envelope
// framing (totFrags > 1) — required when content exceeds the firmware's
// single-fragment envelope cap (~240 B). Geom-aware so the rebuild can
// match the CREATE's widget rectangle exactly. Mirrors
// g2BuildCreateTextPagePb's argument shape.
//
// Uses caller-provided pb buffer (ps_alloc'd, typically 8 KB) so Status-
// snapshot–sized content (~1-2 KB) doesn't overflow.
size_t g2BuildRebuildTextPb(uint32_t magic,
                            const char* containerName,
                            const char* content,
                            const G2ContainerGeom& geom,
                            bool eventCapture,
                            uint8_t* pbOut, size_t pbCap) {
  size_t pos = 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_CMD, G2_CMD_REBUILD_PAGE)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t pageStart;
  if (!g2PbBeginNested(pbOut, pbCap, &pos, G2_WRAP_F_REBUILD, &pageStart)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_TOTAL, 1)) return 0;
  if (!writeTextPropertyGeom(pbOut, pbCap, &pos,
                              containerName, content, geom, eventCapture)) return 0;
  if (!g2PbEndNested(pbOut, pbCap, &pos, pageStart)) return 0;
  return pos;
}

// ListContainerProperty container ID. Geometry now flows in from the
// caller via G2ContainerGeom — see G2_GEOM_* presets in the header.
static constexpr uint32_t G2_LIST_DEF_CID   = 1;
// widgetId for the StartUpPage's inner wrapper is supplied by the
// caller. Default 10000 (matching the reference) unless the caller is
// hijacking a menu-start.

// Emit a ListContainerProperty (wrapper field 2 of RebuildPage/CreateStartup)
// with N selectable items. Firmware draws a native selection highlight when
// IsItemSelectBorderEn=1, and routes touchpad gestures to a List_ItemEvent
// sub-message on sid=0x0D when IsEventCapture=1. This is the one widget the
// reference explicitly supports for scrollable menus.
static bool writeListObjectWithItems(uint8_t* buf, size_t cap, size_t* pos,
                                     const char* containerName,
                                     const char* const* items,
                                     size_t itemCount,
                                     const G2ContainerGeom& geom) {
  if (!items || itemCount == 0) return false;

  size_t listStart;
  if (!g2PbBeginNested(buf, cap, pos, G2_PAGE_F_LIST_OBJ, &listStart)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_LIST_F_X, geom.x)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_LIST_F_Y, geom.y)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_LIST_F_W, geom.w)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_LIST_F_H, geom.h)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_LIST_F_CID, G2_LIST_DEF_CID)) return false;
  if (!g2PbWriteString(buf, cap, pos, G2_LIST_F_CNAME, containerName)) return false;

  size_t itemStart;
  if (!g2PbBeginNested(buf, cap, pos, G2_LIST_F_ITEMS, &itemStart)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_ITEM_F_COUNT, (uint32_t)itemCount)) return false;
  // IsItemSelectBorderEn=1 → firmware draws the selection box natively,
  // no drawing code on our side.
  if (!g2PbWriteUint32(buf, cap, pos, G2_ITEM_F_SEL_BORDR, 1)) return false;
  for (size_t i = 0; i < itemCount; i++) {
    if (!g2PbWriteString(buf, cap, pos, G2_ITEM_F_NAME,
                         items[i] ? items[i] : "")) return false;
  }
  if (!g2PbEndNested(buf, cap, pos, itemStart)) return false;

  // IsEventCapture=1 → firmware routes touchpad gestures to this container
  // as List_ItemEvent sub-messages on sid=0x0D.
  if (!g2PbWriteUint32(buf, cap, pos, G2_LIST_F_EVCAP, 1)) return false;
  return g2PbEndNested(buf, cap, pos, listStart);
}

// (g2BuildCreateStartupPage was removed 2026-04-30. It built a single-
// envelope CREATE_STARTUP_PAGE with a 256-byte internal payload buffer
// — which clipped Status snapshots — and emitted single-fragment
// envelopes that the firmware rejected once content exceeded ~240 B.
// All callers migrated to g2BuildCreateTextPagePb (pb-only) +
// sendPbFragmented for proper multi-fragment wire framing. See git
// history if you need the legacy wire shape.)

// REBUILD_PAGE (Cmd=7) carrying a fresh ListContainerProperty. Used by
// stateful pages (file browser, settings editor) that need to swap the
// list contents in place — the widget stays the same widgetId, but the
// items inside change. Identical pb shape to g2BuildCreateListPage but
// with Cmd=7 + wrapper field G2_WRAP_F_REBUILD instead of CREATE.
//
// Safe against the UPDATE_TEXT-on-ListContainer crash because we're
// using REBUILD which is documented as full-replace; no partial-patch
// semantics like UPDATE_TEXT had.
size_t g2BuildRebuildList(uint8_t seq, uint32_t magic,
                          const char* containerName,
                          const char* const* items, size_t itemCount,
                          uint8_t* out, size_t outCap,
                          const G2ContainerGeom& geom) {
  uint8_t payload[512];
  size_t pos = 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_WRAP_F_CMD, G2_CMD_REBUILD_PAGE)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t pageStart;
  if (!g2PbBeginNested(payload, sizeof(payload), &pos, G2_WRAP_F_REBUILD, &pageStart)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_PAGE_F_TOTAL, 1)) return 0;
  if (!writeListObjectWithItems(payload, sizeof(payload), &pos,
                                containerName, items, itemCount, geom)) return 0;
  if (!g2PbEndNested(payload, sizeof(payload), &pos, pageStart)) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_CORE, G2_FLAG_REQUEST,
                         payload, pos, out, outCap);
}

size_t g2BuildCreateListPagePb(uint32_t magic,
                               const char* containerName,
                               const char* const* items, size_t itemCount,
                               uint32_t widgetId,
                               const G2ContainerGeom& geom,
                               uint8_t* pbOut, size_t pbCap) {
  size_t pos = 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_CMD, G2_CMD_CREATE_STARTUP)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t pageStart;
  if (!g2PbBeginNested(pbOut, pbCap, &pos, G2_WRAP_F_CREATE, &pageStart)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_TOTAL, 1)) return 0;
  // ListObject (wrapper field 2) — the widget the firmware natively
  // scrolls through on touchpad input. Never call UPDATE_TEXT against a
  // list container: that's the bug that froze the plugin task on
  // 2026-04-24. Use REBUILD with a fresh writeListObjectWithItems payload
  // to change the item set.
  if (!writeListObjectWithItems(pbOut, pbCap, &pos,
                                containerName, items, itemCount, geom)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_WIDGET_ID, widgetId)) return 0;
  if (!g2PbEndNested(pbOut, pbCap, &pos, pageStart)) return 0;
  return pos;
}

size_t g2BuildCreateListPage(uint8_t seq, uint32_t magic,
                             const char* containerName,
                             const char* const* items, size_t itemCount,
                             uint8_t* out, size_t outCap,
                             uint32_t widgetId,
                             const G2ContainerGeom& geom) {
  // Single-fragment build: builds pb into a stack buffer big enough for
  // the largest list that still fits one envelope (≤253 pb bytes), then
  // wraps. For larger lists, callers must use g2BuildCreateListPagePb +
  // a fragmenting transport (see G2_Glasses.cpp::sendPbFragmented).
  uint8_t payload[512];
  size_t pbLen = g2BuildCreateListPagePb(magic, containerName, items, itemCount,
                                          widgetId, geom, payload, sizeof(payload));
  if (pbLen == 0) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_CORE, G2_FLAG_REQUEST,
                         payload, pbLen, out, outCap);
}

size_t g2BuildUpdateText(uint8_t seq, uint32_t magic,
                         const char* containerName, uint32_t containerId,
                         const char* content,
                         uint8_t* out, size_t outCap) {
  uint8_t payload[256];
  size_t pos = 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_WRAP_F_CMD, G2_CMD_UPDATE_TEXT)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t bodyStart;
  if (!g2PbBeginNested(payload, sizeof(payload), &pos, G2_WRAP_F_TEXT_UPGRADE, &bodyStart)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_TXUPG_F_CID, containerId)) return 0;
  if (!g2PbWriteString(payload, sizeof(payload), &pos, G2_TXUPG_F_CNAME, containerName)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_TXUPG_F_OFFSET, 0)) return 0;
  const size_t clen = content ? strlen(content) : 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_TXUPG_F_LENGTH, (uint32_t)clen)) return 0;
  if (!g2PbWriteString(payload, sizeof(payload), &pos, G2_TXUPG_F_CONTENT, content ? content : "")) return 0;
  if (!g2PbEndNested(payload, sizeof(payload), &pos, bodyStart)) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_CORE, G2_FLAG_REQUEST,
                         payload, pos, out, outCap);
}

size_t g2BuildAudioCtrl(uint8_t seq, uint32_t magic, bool enable,
                        uint8_t* out, size_t outCap) {
  uint8_t payload[32];
  size_t pos = 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_WRAP_F_CMD, G2_CMD_AUDIO_CTRL)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t inner;
  if (!g2PbBeginNested(payload, sizeof(payload), &pos, G2_WRAP_F_AUDIO, &inner)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AUDIO_F_EN, enable ? 1u : 0u)) return 0;
  if (!g2PbEndNested(payload, sizeof(payload), &pos, inner)) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_CORE, G2_FLAG_REQUEST,
                         payload, pos, out, outCap);
}

size_t g2BuildEvenAICtrl(uint8_t seq, uint32_t magic, uint32_t status,
                         uint8_t* out, size_t outCap) {
  uint8_t payload[32];
  size_t pos = 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_F_CMD, G2_AI_CMD_CTRL)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_F_MAGIC, magic)) return 0;
  size_t inner;
  if (!g2PbBeginNested(payload, sizeof(payload), &pos, G2_AI_F_CTRL, &inner)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_CTRL_F_STATUS, status)) return 0;
  if (!g2PbEndNested(payload, sizeof(payload), &pos, inner)) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_AI, G2_FLAG_REQUEST,
                         payload, pos, out, outCap);
}

size_t g2BuildEvenAIAsk(uint8_t seq, uint32_t magic, uint32_t cmdCnt,
                        const char* text,
                        uint8_t* out, size_t outCap) {
  // EvenAIAskInfo shares wire shape with EvenAIReplyInfo (cmdCnt, streamEnable,
  // textMode, text, errorCode) but lives in field 5 of the wrapper and carries
  // no fTextEnd. The "question" panel — what the user supposedly asked.
  size_t textLen = text ? strnlen(text, 250) : 0;
  uint8_t payload[400];
  size_t pos = 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_F_CMD, G2_AI_CMD_ASK)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_F_MAGIC, magic)) return 0;
  size_t inner;
  if (!g2PbBeginNested(payload, sizeof(payload), &pos, G2_AI_F_ASK, &inner)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_REPLY_F_CNT, cmdCnt)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_REPLY_F_STREAM, 0)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_REPLY_F_MODE, 0)) return 0;
  if (textLen > 0) {
    if (!g2PbWriteBytes(payload, sizeof(payload), &pos, G2_AI_REPLY_F_TEXT,
                        (const uint8_t*)text, textLen)) return 0;
  }
  if (!g2PbEndNested(payload, sizeof(payload), &pos, inner)) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_AI, G2_FLAG_REQUEST,
                         payload, pos, out, outCap);
}

size_t g2BuildEvenAIAnalyse(uint8_t seq, uint32_t magic,
                            uint8_t* out, size_t outCap) {
  // EvenAIAnalyseInfo carries only an errorCode; we always send 0 (Success)
  // since this is the "I'm processing" transition and any non-zero would
  // tell the firmware something went wrong on our (synthetic) side. The
  // body is intentionally non-empty (errorCode=0 written explicitly) so the
  // wrapper field doesn't collapse into "field absent" on the wire.
  uint8_t payload[32];
  size_t pos = 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_F_CMD, G2_AI_CMD_ANALYSE)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_F_MAGIC, magic)) return 0;
  size_t inner;
  if (!g2PbBeginNested(payload, sizeof(payload), &pos, G2_AI_F_ANALYSE, &inner)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, /*errorCode field*/ 1, 0)) return 0;
  if (!g2PbEndNested(payload, sizeof(payload), &pos, inner)) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_AI, G2_FLAG_REQUEST,
                         payload, pos, out, outCap);
}

size_t g2BuildEvenAIReply(uint8_t seq, uint32_t magic, uint32_t cmdCnt,
                          const char* text, bool isLast,
                          uint8_t* out, size_t outCap) {
  // Cap text at ~250 B so the whole pb fits inside one fragment (max
  // single-fragment pb body is 253 B). Streaming long replies should
  // chunk above this caller and call us multiple times.
  size_t textLen = text ? strnlen(text, 250) : 0;
  uint8_t payload[400];
  size_t pos = 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_F_CMD, G2_AI_CMD_REPLY)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_F_MAGIC, magic)) return 0;
  size_t inner;
  if (!g2PbBeginNested(payload, sizeof(payload), &pos, G2_AI_F_REPLY, &inner)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_REPLY_F_CNT, cmdCnt)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_REPLY_F_STREAM, 0)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_REPLY_F_MODE, 0)) return 0;
  if (textLen > 0) {
    if (!g2PbWriteBytes(payload, sizeof(payload), &pos, G2_AI_REPLY_F_TEXT,
                        (const uint8_t*)text, textLen)) return 0;
  }
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_REPLY_F_END, isLast ? 1u : 0u)) return 0;
  if (!g2PbEndNested(payload, sizeof(payload), &pos, inner)) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_AI, G2_FLAG_REQUEST,
                         payload, pos, out, outCap);
}

size_t g2BuildEvenAIConfig(uint8_t seq, uint32_t magic,
                           uint64_t voiceSwitch, uint64_t streamSpeed,
                           uint8_t* out, size_t outCap) {
  // Wrapper: cmd=10 CONFIG + magic + (optional) inner config sub-msg.
  // The reference enum names CONFIG=10 but doesn't ship a host->glasses
  // example; field numbers below come from g2-kit-unofficial's inline
  // example string `08 01 10 A0 01` = {f1=1, f2=160} = voiceSwitch=on,
  // streamSpeed=160. Pass UINT64_MAX for either parameter to omit it
  // from the body — useful for probing one field at a time.
  uint8_t payload[64];
  size_t pos = 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_F_CMD, /*CONFIG*/10)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_AI_F_MAGIC, magic)) return 0;
  // Sub-message lives at field 10 (CONFIG slot in the reference).
  // Inner schema is not in our docs — guess: f1 = voiceSwitch (bool),
  // f2 = streamSpeed (uint). If the firmware rejects, the COMM_RSP
  // errorCode field will tell us what to adjust.
  if (voiceSwitch != UINT64_MAX || streamSpeed != UINT64_MAX) {
    size_t inner;
    if (!g2PbBeginNested(payload, sizeof(payload), &pos, /*F_CONFIG*/10, &inner)) return 0;
    if (voiceSwitch != UINT64_MAX) {
      if (!g2PbWriteUint32(payload, sizeof(payload), &pos, 1, (uint32_t)voiceSwitch)) return 0;
    }
    if (streamSpeed != UINT64_MAX) {
      if (!g2PbWriteUint32(payload, sizeof(payload), &pos, 2, (uint32_t)streamSpeed)) return 0;
    }
    if (!g2PbEndNested(payload, sizeof(payload), &pos, inner)) return 0;
  }
  return g2BuildEnvelope(seq, G2_SID_EVEN_AI, G2_FLAG_REQUEST,
                         payload, pos, out, outCap);
}

size_t g2BuildImageRawBody(uint32_t magic,
                           uint32_t containerId,
                           const char* containerName,
                           uint32_t mapSessionId,
                           uint32_t mapTotalSize,
                           uint32_t mapFragmentIndex,
                           const uint8_t* data, size_t dataLen,
                           uint8_t* out, size_t outCap) {
  if (!out || outCap == 0) return 0;
  if (!containerName)        return 0;
  if (dataLen > 0 && !data)  return 0;

  // Wrapper: f1=Cmd(3 UPDATE_IMAGE_RAW_DATA), f2=magic, f5=ImgRawMsg{...}.
  // Inner ImageRawDataUpdate fields verified against
  // g2-kit-unofficial/ble/gen/EvenHub_pb.ts.
  size_t pos = 0;
  if (!g2PbWriteUint32(out, outCap, &pos, /*F_CMD*/   1, /*UPDATE_IMAGE_RAW_DATA*/3)) return 0;
  if (!g2PbWriteUint32(out, outCap, &pos, /*F_MAGIC*/ 2, magic)) return 0;
  size_t inner;
  if (!g2PbBeginNested(out, outCap, &pos, /*F_IMG_RAW_MSG*/5, &inner)) return 0;
  if (!g2PbWriteUint32(out, outCap, &pos, 1, containerId)) return 0;
  if (!g2PbWriteString(out, outCap, &pos, 2, containerName)) return 0;
  if (!g2PbWriteUint32(out, outCap, &pos, 3, mapSessionId)) return 0;
  if (!g2PbWriteUint32(out, outCap, &pos, 4, mapTotalSize)) return 0;
  if (!g2PbWriteUint32(out, outCap, &pos, 5, /*CompressMode raw*/0)) return 0;
  if (!g2PbWriteUint32(out, outCap, &pos, 6, mapFragmentIndex)) return 0;
  if (!g2PbWriteUint32(out, outCap, &pos, 7, (uint32_t)dataLen)) return 0;
  if (dataLen > 0) {
    if (!g2PbWriteBytes(out, outCap, &pos, 8, data, dataLen)) return 0;
  }
  if (!g2PbEndNested(out, outCap, &pos, inner)) return 0;
  return pos;
}

size_t g2BuildCreateImagePb(uint32_t magic,
                            const char* containerName, uint32_t containerId,
                            uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                            uint32_t widgetId,
                            uint8_t* pbOut, size_t pbCap) {
  if (!pbOut || pbCap == 0 || !containerName) return 0;

  // Wrapper: Cmd=0 CREATE_STARTUP, magic, CreateMessage{...}.
  // Inner CreateStartUpPageContainer.ImageObject = field 4.
  size_t pos = 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_CMD, G2_CMD_CREATE_STARTUP)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t pageStart;
  if (!g2PbBeginNested(pbOut, pbCap, &pos, G2_WRAP_F_CREATE, &pageStart)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_TOTAL, 1)) return 0;

  // ImageContainerProperty at inner field 4.
  size_t imgStart;
  if (!g2PbBeginNested(pbOut, pbCap, &pos, /*F_IMAGE_OBJ*/4, &imgStart)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, 1, x)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, 2, y)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, 3, w)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, 4, h)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, 5, containerId)) return 0;
  if (!g2PbWriteString(pbOut, pbCap, &pos, 6, containerName)) return 0;
  if (!g2PbEndNested(pbOut, pbCap, &pos, imgStart)) return 0;

  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_WIDGET_ID, widgetId)) return 0;
  if (!g2PbEndNested(pbOut, pbCap, &pos, pageStart)) return 0;
  return pos;
}

size_t g2BuildCreateImage(uint8_t seq, uint32_t magic,
                          const char* containerName, uint32_t containerId,
                          uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                          uint32_t widgetId,
                          uint8_t* out, size_t outCap) {
  uint8_t payload[256];
  size_t pbLen = g2BuildCreateImagePb(magic, containerName, containerId,
                                      x, y, w, h, widgetId,
                                      payload, sizeof(payload));
  if (pbLen == 0) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_CORE, G2_FLAG_REQUEST,
                         payload, pbLen, out, outCap);
}

size_t g2BuildCreateImageMultiPb(uint32_t magic,
                                 const G2ImageTile* tiles, size_t tileCount,
                                 uint32_t widgetId,
                                 uint8_t* pbOut, size_t pbCap) {
  if (!pbOut || pbCap == 0 || !tiles || tileCount == 0) return 0;

  size_t pos = 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_CMD, G2_CMD_CREATE_STARTUP)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t pageStart;
  if (!g2PbBeginNested(pbOut, pbCap, &pos, G2_WRAP_F_CREATE, &pageStart)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_TOTAL, (uint32_t)tileCount)) return 0;

  for (size_t i = 0; i < tileCount; i++) {
    const G2ImageTile& t = tiles[i];
    if (!t.containerName) return 0;
    size_t imgStart;
    if (!g2PbBeginNested(pbOut, pbCap, &pos, /*F_IMAGE_OBJ*/4, &imgStart)) return 0;
    if (!g2PbWriteUint32(pbOut, pbCap, &pos, 1, t.x)) return 0;
    if (!g2PbWriteUint32(pbOut, pbCap, &pos, 2, t.y)) return 0;
    if (!g2PbWriteUint32(pbOut, pbCap, &pos, 3, t.w)) return 0;
    if (!g2PbWriteUint32(pbOut, pbCap, &pos, 4, t.h)) return 0;
    if (!g2PbWriteUint32(pbOut, pbCap, &pos, 5, t.containerId)) return 0;
    if (!g2PbWriteString(pbOut, pbCap, &pos, 6, t.containerName)) return 0;
    if (!g2PbEndNested(pbOut, pbCap, &pos, imgStart)) return 0;
  }

  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_WIDGET_ID, widgetId)) return 0;
  if (!g2PbEndNested(pbOut, pbCap, &pos, pageStart)) return 0;
  return pos;
}

size_t g2BuildCreateImageMulti(uint8_t seq, uint32_t magic,
                               const G2ImageTile* tiles, size_t tileCount,
                               uint32_t widgetId,
                               uint8_t* out, size_t outCap) {
  uint8_t payload[512];
  size_t pbLen = g2BuildCreateImageMultiPb(magic, tiles, tileCount, widgetId,
                                            payload, sizeof(payload));
  if (pbLen == 0) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_CORE, G2_FLAG_REQUEST,
                         payload, pbLen, out, outCap);
}

// Mixed CREATE — one ListObject (f2) + one ImageObject (f4) inside a
// single CreateStartUpPageContainer. Tests whether the firmware accepts
// multi-type widget composition in a single CREATE frame. Used by the
// Q16/Q17/Q18 probes that overlay an image on a list-shaped page.
//
// f1 (ContainerTotalNum) is set to 2 — one list + one image. The list
// is rendered first on the wire (f2 < f4); whether that means it's
// drawn first on the lens (z-order) is what we're testing.
size_t g2BuildCreateMixedListImagePb(uint32_t magic,
                                     const char* listName,
                                     const char* const* listItems,
                                     size_t listItemCount,
                                     const G2ContainerGeom& listGeom,
                                     const G2ImageTile& imageTile,
                                     uint32_t widgetId,
                                     uint8_t* pbOut, size_t pbCap) {
  if (!pbOut || pbCap == 0 || !listName || !listItems || listItemCount == 0
      || !imageTile.containerName) return 0;

  size_t pos = 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_CMD, G2_CMD_CREATE_STARTUP)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t pageStart;
  if (!g2PbBeginNested(pbOut, pbCap, &pos, G2_WRAP_F_CREATE, &pageStart)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_TOTAL, 2)) return 0;

  // ListObject (wrapper field 2) — share writeListObjectWithItems with
  // the existing list-only builders so the on-wire shape is identical.
  if (!writeListObjectWithItems(pbOut, pbCap, &pos,
                                listName, listItems, listItemCount,
                                listGeom)) return 0;

  // ImageObject (wrapper field 4) — same shape as g2BuildCreateImagePb.
  size_t imgStart;
  if (!g2PbBeginNested(pbOut, pbCap, &pos, /*F_IMAGE_OBJ*/4, &imgStart)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, 1, imageTile.x)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, 2, imageTile.y)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, 3, imageTile.w)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, 4, imageTile.h)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, 5, imageTile.containerId)) return 0;
  if (!g2PbWriteString(pbOut, pbCap, &pos, 6, imageTile.containerName)) return 0;
  if (!g2PbEndNested(pbOut, pbCap, &pos, imgStart)) return 0;

  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_WIDGET_ID, widgetId)) return 0;
  if (!g2PbEndNested(pbOut, pbCap, &pos, pageStart)) return 0;
  return pos;
}

size_t g2BuildCreateMixedListImage(uint8_t seq, uint32_t magic,
                                   const char* listName,
                                   const char* const* listItems,
                                   size_t listItemCount,
                                   const G2ContainerGeom& listGeom,
                                   const G2ImageTile& imageTile,
                                   uint32_t widgetId,
                                   uint8_t* out, size_t outCap) {
  // Larger payload buffer than other CREATE builders because we're
  // packing two widgets plus list items into one nested frame. 1 KB
  // is plenty for ~6 short list rows + a single image declaration.
  uint8_t payload[1024];
  size_t pbLen = g2BuildCreateMixedListImagePb(magic, listName,
                                                listItems, listItemCount,
                                                listGeom, imageTile,
                                                widgetId,
                                                payload, sizeof(payload));
  if (pbLen == 0) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_CORE, G2_FLAG_REQUEST,
                         payload, pbLen, out, outCap);
}

// Forward decl — writeTextChildSpec is defined further down alongside
// the multi-text builders. The image+text CREATE builder needs it now.
static bool writeTextChildSpec(uint8_t* buf, size_t cap, size_t* pos,
                               const G2TextChildSpec& spec);

// Image + Text compound CREATE. See g2BuildCreateMixedImageTextPb in the
// header for the design + independent-refresh contract. Body mirrors
// g2BuildCreateMixedListImagePb with the ListObject swapped for a
// TextObject (writeTextChildSpec — same helper used by the multi-text
// builders below). f1=ContainerTotalNum=2; f3 emitted before f4 so the
// text child sits before the image child on the wire.
size_t g2BuildCreateMixedImageTextPb(uint32_t magic,
                                     const G2TextChildSpec& textChild,
                                     const G2ImageTile& imageTile,
                                     uint32_t widgetId,
                                     uint8_t* pbOut, size_t pbCap) {
  if (!pbOut || pbCap == 0 || !imageTile.containerName) return 0;

  size_t pos = 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_CMD,   G2_CMD_CREATE_STARTUP)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t pageStart;
  if (!g2PbBeginNested(pbOut, pbCap, &pos, G2_WRAP_F_CREATE, &pageStart)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_TOTAL, 2)) return 0;

  // TextObject (wrapper field 3). writeTextChildSpec is local-static in
  // this .cpp; same helper used by the multi-text and list+text builders.
  if (!writeTextChildSpec(pbOut, pbCap, &pos, textChild)) return 0;

  // ImageObject (wrapper field 4) — identical to the list+image variant.
  size_t imgStart;
  if (!g2PbBeginNested(pbOut, pbCap, &pos, /*F_IMAGE_OBJ*/4, &imgStart)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, 1, imageTile.x)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, 2, imageTile.y)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, 3, imageTile.w)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, 4, imageTile.h)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, 5, imageTile.containerId)) return 0;
  if (!g2PbWriteString(pbOut, pbCap, &pos, 6, imageTile.containerName)) return 0;
  if (!g2PbEndNested(pbOut, pbCap, &pos, imgStart)) return 0;

  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_WIDGET_ID, widgetId)) return 0;
  if (!g2PbEndNested(pbOut, pbCap, &pos, pageStart)) return 0;
  return pos;
}

size_t g2BuildCreateMixedImageText(uint8_t seq, uint32_t magic,
                                   const G2TextChildSpec& textChild,
                                   const G2ImageTile& imageTile,
                                   uint32_t widgetId,
                                   uint8_t* out, size_t outCap) {
  // 1 KB payload — same headroom as the list+image variant. Image-decl
  // is fixed-size, text content is bounded by writeTextChildSpec internal
  // limits, so 1 KB is generous.
  uint8_t payload[1024];
  size_t pbLen = g2BuildCreateMixedImageTextPb(magic, textChild, imageTile,
                                                widgetId,
                                                payload, sizeof(payload));
  if (pbLen == 0) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_CORE, G2_FLAG_REQUEST,
                         payload, pbLen, out, outCap);
}

// Per-child TextObject emitter for the multi-text builder. Same shape
// as writeTextProperty/writeTextPropertyGeom but takes the CID + name
// + content + geom from the spec instead of the file-static defaults.
// EvCap=0 (consistent with single-text case — "we don't tap text
// areas"; tap dispatch in compound widgets goes through SysEvent).
static bool writeTextChildSpec(uint8_t* buf, size_t cap, size_t* pos,
                               const G2TextChildSpec& spec) {
  size_t inner;
  if (!g2PbBeginNested(buf, cap, pos, G2_PAGE_F_TEXT_OBJ, &inner)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_X, spec.geom.x)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_Y, spec.geom.y)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_W, spec.geom.w)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_H, spec.geom.h)) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_CID, spec.containerId)) return false;
  if (!g2PbWriteString(buf, cap, pos, G2_TEXT_F_CNAME,
                       spec.containerName ? spec.containerName : "")) return false;
  if (!g2PbWriteUint32(buf, cap, pos, G2_TEXT_F_EVCAP, spec.eventCapture ? 1u : 0u)) return false;
  if (!g2PbWriteString(buf, cap, pos, G2_TEXT_F_CONTENT,
                       spec.content ? spec.content : "")) return false;
  return g2PbEndNested(buf, cap, pos, inner);
}

size_t g2BuildCreateMultiTextPb(uint32_t magic,
                                const G2TextChildSpec* children,
                                size_t childCount,
                                uint32_t widgetId,
                                uint8_t* pbOut, size_t pbCap) {
  if (!pbOut || pbCap == 0 || !children || childCount == 0) return 0;

  size_t pos = 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_CMD,   G2_CMD_CREATE_STARTUP)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t pageStart;
  if (!g2PbBeginNested(pbOut, pbCap, &pos, G2_WRAP_F_CREATE, &pageStart)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_TOTAL, (uint32_t)childCount)) return 0;
  for (size_t i = 0; i < childCount; i++) {
    if (!writeTextChildSpec(pbOut, pbCap, &pos, children[i])) return 0;
  }
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_WIDGET_ID, widgetId)) return 0;
  if (!g2PbEndNested(pbOut, pbCap, &pos, pageStart)) return 0;
  return pos;
}

size_t g2BuildCreateMultiText(uint8_t seq, uint32_t magic,
                              const G2TextChildSpec* children,
                              size_t childCount,
                              uint32_t widgetId,
                              uint8_t* out, size_t outCap) {
  // 1 KB payload is enough for a handful of short-text children. The
  // worst single-child overhead is ~50 B (8 fixed fields + name +
  // content); 5 children with ~20-char content each is ~400 B, well
  // within the buffer.
  uint8_t payload[1024];
  size_t pbLen = g2BuildCreateMultiTextPb(magic, children, childCount,
                                          widgetId,
                                          payload, sizeof(payload));
  if (pbLen == 0) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_CORE, G2_FLAG_REQUEST,
                         payload, pbLen, out, outCap);
}

// REBUILD_PAGE counterpart to g2BuildCreateMultiTextPb — same wire shape
// (TextObject children repeated under wrapper field 3) but with
// Cmd=7 REBUILD_PAGE and the children packaged inside G2_WRAP_F_REBUILD
// (field 7) instead of G2_WRAP_F_CREATE (field 2). No WidgetId field —
// the existing widget binding is implicit on a REBUILD.
//
// Why this exists: per-child REBUILD-text on a compound CreateStartUpPage
// (sendRebuildTextNamedAndWait) renders ONLY the named child on this
// firmware (2.2.0.24) — the OTHER children blank. Verified empirically
// 2026-04-30 with the body+batt Status compound: rebuilding "body" alone
// blanked "batt", and vice versa. Sending all children in a single
// REBUILD message lets the firmware re-render the whole compound at
// once, keeping every child visible.
//
// SCHEMA RISK: this is the first time we ship a REBUILD-multitext shape
// (cmd=7 + N TextObject children). The probe at g2ProbeRebuildTextChild
// only exercised single-child REBUILD on a list+text compound. If the
// firmware rejects this with RebuildResp res != 0, the caller's worker
// bails and the user sees the failure in the log; we'd then need a
// different approach (full SHUTDOWN+CREATE every tick, or drop the
// compound entirely).
size_t g2BuildRebuildMultiTextPb(uint32_t magic,
                                  const G2TextChildSpec* children,
                                  size_t childCount,
                                  uint8_t* pbOut, size_t pbCap) {
  if (!pbOut || pbCap == 0 || !children || childCount == 0) return 0;

  size_t pos = 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_CMD,   G2_CMD_REBUILD_PAGE)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t pageStart;
  if (!g2PbBeginNested(pbOut, pbCap, &pos, G2_WRAP_F_REBUILD, &pageStart)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_TOTAL, (uint32_t)childCount)) return 0;
  for (size_t i = 0; i < childCount; i++) {
    if (!writeTextChildSpec(pbOut, pbCap, &pos, children[i])) return 0;
  }
  if (!g2PbEndNested(pbOut, pbCap, &pos, pageStart)) return 0;
  return pos;
}

// REBUILD_PAGE counterpart for compound with 1 ListObject + N
// TextObject children. Same shape as g2BuildCreateMixedListMultiTextPb
// but with the REBUILD wrapper (cmd=7, G2_WRAP_F_REBUILD) instead of
// CREATE (cmd=0, G2_WRAP_F_CREATE), and no WidgetId.
//
// SCHEMA RISK: empirically derived. We confirmed 2026-04-30 that
// REBUILD-multitext (N×TextObject only) blanks any unmentioned
// siblings on a list+text+text+text compound — Status' back-row list
// disappeared on the first REBUILD tick when only the 3 texts were
// sent. Including the list in every REBUILD keeps it visible by the
// same rule. CreateResp / RebuildResp will surface rejection.
size_t g2BuildRebuildMixedListMultiTextPb(uint32_t magic,
                                           const char* listName,
                                           const char* const* listItems,
                                           size_t listItemCount,
                                           const G2ContainerGeom& listGeom,
                                           const G2TextChildSpec* textChildren,
                                           size_t textChildCount,
                                           uint8_t* pbOut, size_t pbCap) {
  if (!pbOut || pbCap == 0) return 0;
  if (!listName || !listItems || listItemCount == 0) return 0;
  if (!textChildren || textChildCount == 0) return 0;

  size_t pos = 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_CMD,   G2_CMD_REBUILD_PAGE)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t pageStart;
  if (!g2PbBeginNested(pbOut, pbCap, &pos, G2_WRAP_F_REBUILD, &pageStart)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_TOTAL,
                       (uint32_t)(1 + textChildCount))) return 0;
  if (!writeListObjectWithItems(pbOut, pbCap, &pos,
                                listName, listItems, listItemCount,
                                listGeom)) return 0;
  for (size_t i = 0; i < textChildCount; i++) {
    if (!writeTextChildSpec(pbOut, pbCap, &pos, textChildren[i])) return 0;
  }
  if (!g2PbEndNested(pbOut, pbCap, &pos, pageStart)) return 0;
  return pos;
}

// CREATE compound with one ListObject + N TextObject children. Same
// wrapper shape as g2BuildCreateMixedListTextPb but with N text
// children instead of one — used by Status' back-row + body/batt/meter
// compound (1 list ≡ "<- Main Menu" tappable + 3 text panes).
//
// SCHEMA RISK: doc says firmware accepts mixed-type compound CREATEs
// in arbitrary combinations of f2/f3/f4 (see "Mixed-widget composition"
// in G2_PROTOCOL.md), but `1 list + 3 text` was not exercised before
// 2026-04-30. If the firmware rejects this shape, CreateResp returns
// res != 0 and the worker's CREATE branch logs the failure.
size_t g2BuildCreateMixedListMultiTextPb(uint32_t magic,
                                          const char* listName,
                                          const char* const* listItems,
                                          size_t listItemCount,
                                          const G2ContainerGeom& listGeom,
                                          const G2TextChildSpec* textChildren,
                                          size_t textChildCount,
                                          uint32_t widgetId,
                                          uint8_t* pbOut, size_t pbCap) {
  if (!pbOut || pbCap == 0) return 0;
  if (!listName || !listItems || listItemCount == 0) return 0;
  if (!textChildren || textChildCount == 0) return 0;

  size_t pos = 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_CMD,   G2_CMD_CREATE_STARTUP)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t pageStart;
  if (!g2PbBeginNested(pbOut, pbCap, &pos, G2_WRAP_F_CREATE, &pageStart)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_TOTAL,
                       (uint32_t)(1 + textChildCount))) return 0;

  // ListObject (wrapper field 2) — eventCapture=1 inside
  // writeListObjectWithItems, so the firmware fires ListEvent CLICK on
  // tap.
  if (!writeListObjectWithItems(pbOut, pbCap, &pos,
                                listName, listItems, listItemCount,
                                listGeom)) return 0;

  // TextObjects (wrapper field 3, repeated). Each child needs a
  // distinct ContainerId — caller is responsible for assigning unique
  // IDs across the whole compound (list + texts share the cid space).
  for (size_t i = 0; i < textChildCount; i++) {
    if (!writeTextChildSpec(pbOut, pbCap, &pos, textChildren[i])) return 0;
  }

  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_WIDGET_ID, widgetId)) return 0;
  if (!g2PbEndNested(pbOut, pbCap, &pos, pageStart)) return 0;
  return pos;
}

size_t g2BuildCreateMixedListTextPb(uint32_t magic,
                                    const char* listName,
                                    const char* const* listItems,
                                    size_t listItemCount,
                                    const G2ContainerGeom& listGeom,
                                    const G2TextChildSpec& textSpec,
                                    uint32_t widgetId,
                                    uint8_t* pbOut, size_t pbCap) {
  if (!pbOut || pbCap == 0 || !listName || !listItems || listItemCount == 0) return 0;

  size_t pos = 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_CMD,   G2_CMD_CREATE_STARTUP)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t pageStart;
  if (!g2PbBeginNested(pbOut, pbCap, &pos, G2_WRAP_F_CREATE, &pageStart)) return 0;
  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_TOTAL, 2)) return 0;

  // ListObject (wrapper field 2) — selection + scroll + CLICK managed
  // by firmware. This is what makes the page tappable.
  if (!writeListObjectWithItems(pbOut, pbCap, &pos,
                                listName, listItems, listItemCount,
                                listGeom)) return 0;

  // TextObject (wrapper field 3) — header / title; non-interactive
  // unless caller opts into eventCapture, which is normally false for
  // a header.
  if (!writeTextChildSpec(pbOut, pbCap, &pos, textSpec)) return 0;

  if (!g2PbWriteUint32(pbOut, pbCap, &pos, G2_PAGE_F_WIDGET_ID, widgetId)) return 0;
  if (!g2PbEndNested(pbOut, pbCap, &pos, pageStart)) return 0;
  return pos;
}

size_t g2BuildCreateMixedListText(uint8_t seq, uint32_t magic,
                                  const char* listName,
                                  const char* const* listItems,
                                  size_t listItemCount,
                                  const G2ContainerGeom& listGeom,
                                  const G2TextChildSpec& textSpec,
                                  uint32_t widgetId,
                                  uint8_t* out, size_t outCap) {
  uint8_t payload[1024];
  size_t pbLen = g2BuildCreateMixedListTextPb(magic, listName,
                                              listItems, listItemCount,
                                              listGeom, textSpec, widgetId,
                                              payload, sizeof(payload));
  if (pbLen == 0) return 0;
  return g2BuildEnvelope(seq, G2_SID_EVEN_CORE, G2_FLAG_REQUEST,
                         payload, pbLen, out, outCap);
}

bool g2DecodeHeartbeatAckTail(const uint8_t* pb, size_t pbLen,
                              uint64_t* seq, uint64_t* echo) {
  if (!pb || pbLen == 0) return false;
  size_t p = 0;
  while (p < pbLen) {
    uint32_t fld; uint8_t wire;
    if (!g2PbReadTag(pb, pbLen, &p, &fld, &wire)) return false;
    if (fld == 15 && wire == G2_PB_WIRE_LEN_DELIM) {
      uint64_t innerLen;
      if (!g2PbReadVarint(pb, pbLen, &p, &innerLen)) return false;
      if (p + innerLen > pbLen) return false;
      const uint8_t* sub = pb + p;
      const size_t   subN = (size_t)innerLen;
      p += subN;
      // Walk the sub-msg, picking off field 1 (seq) and field 2 (echo).
      bool gotSeq = false, gotEcho = false;
      size_t sp = 0;
      while (sp < subN) {
        uint32_t sf; uint8_t sw;
        if (!g2PbReadTag(sub, subN, &sp, &sf, &sw)) break;
        if (sw == G2_PB_WIRE_VARINT) {
          uint64_t v;
          if (!g2PbReadVarint(sub, subN, &sp, &v)) break;
          if (sf == 1) { if (seq)  *seq  = v; gotSeq  = true; }
          else if (sf == 2) { if (echo) *echo = v; gotEcho = true; }
        } else {
          if (!g2PbSkipField(sub, subN, &sp, sw)) break;
        }
      }
      return gotSeq && gotEcho;
    }
    // Skip non-target fields.
    if (!g2PbSkipField(pb, pbLen, &p, wire)) return false;
  }
  return false;
}

// ── Per-sid wire stats ───────────────────────────────────────────────────
// Linear-scan array; tiny enough that O(N) lookup beats hashing. Capacity
// is generous against the ~12 known sids in the firmware enum.
static constexpr size_t G2_SID_STAT_CAP = 16;
static G2SidStat gSidStats[G2_SID_STAT_CAP];
static size_t    gSidStatsCount = 0;

static G2SidStat* sidStatFind(uint8_t sid) {
  for (size_t i = 0; i < gSidStatsCount; i++) {
    if (gSidStats[i].sid == sid) return &gSidStats[i];
  }
  if (gSidStatsCount >= G2_SID_STAT_CAP) return nullptr;
  G2SidStat* s = &gSidStats[gSidStatsCount++];
  memset(s, 0, sizeof(*s));
  s->sid = sid;
  return s;
}

void g2statsRecordTx(uint8_t sid, uint8_t flag, size_t pbLen) {
  (void)flag; (void)pbLen;
  G2SidStat* s = sidStatFind(sid);
  if (!s) return;
  s->txCount++;
  s->lastTxMs = (uint32_t)millis();
}

void g2statsRecordRx(uint8_t sid, uint8_t flag, const uint8_t* pb, size_t pbLen) {
  G2SidStat* s = sidStatFind(sid);
  if (!s) return;
  s->rxCount++;
  s->lastRxMs   = (uint32_t)millis();
  s->lastFlag   = flag;
  s->lastPbLen  = (uint16_t)pbLen;
  const size_t cap   = sizeof(s->lastSample);
  const size_t take  = (pbLen < cap) ? pbLen : cap;
  if (pb && take) memcpy(s->lastSample, pb, take);
  s->lastSampleLen = (uint8_t)take;
}

size_t g2statsCount() { return gSidStatsCount; }
const G2SidStat* g2statsAt(size_t idx) {
  return (idx < gSidStatsCount) ? &gSidStats[idx] : nullptr;
}
void g2statsReset() {
  memset(gSidStats, 0, sizeof(gSidStats));
  gSidStatsCount = 0;
}

const char* g2sidName(uint8_t sid) {
  switch (sid) {
    case 0x01: return "AppLaunch / Dashboard";
    case 0x03: return "ForegroundMenu";
    case 0x04: return "ForegroundNotification";
    case 0x05: return "Translate";
    case 0x06: return "Teleprompt";
    case 0x07: return "EvenAI (front-pane)";
    case 0x08: return "Navigation";
    case 0x09: return "Settings";
    case 0x0A: return "Transcribe";
    case 0x0B: return "Conversate";
    case 0x0C: return "QuickList";
    case 0x0D: return "StateEvent (sync)";
    case 0x0E: return "Health / WidgetXform";
    case 0x10: return "Onboarding";
    case 0x21: return "ForegroundSystemAlert";
    case 0x22: return "ForegroundSystemClose";
    case 0x80: return "DeviceSettings (DANGEROUS)";
    case 0x81: return "GlassesCase";
    case 0x90: return "Ring data";
    case 0xC0: return "OTA cmd";
    case 0xC1: return "OTA raw";
    case 0xC4: return "FileService cmd";
    case 0xC5: return "FileService raw";
    case 0xE0: return "EvenHub (back-pane)";
    default:   return "Unknown";
  }
}

// App-launch prelude — verbatim from ble/messages.ts PRELUDE_F5872. The inner
// pb structure isn't fully understood; the reference treats it as an opaque
// blob, so we do the same. CRC 0xa1 0x42 at the end is part of the constant.
static const uint8_t PRELUDE_F5872[] = {
  0xaa, 0x21, 0x92, 0x13, 0x01, 0x01, 0x01, 0x20,
  0x08, 0x02, 0x10, 0x9c, 0x01, 0x22, 0x0a, 0x1a,
  0x08, 0x12, 0x06, 0x12, 0x04, 0x08, 0x00, 0x10,
  0x00, 0xa1, 0x42,
};

size_t g2BuildAppLaunch(uint8_t* out, size_t outCap) {
  if (outCap < sizeof(PRELUDE_F5872)) return 0;
  memcpy(out, PRELUDE_F5872, sizeof(PRELUDE_F5872));
  return sizeof(PRELUDE_F5872);
}

// ── Settings (sid=0x09) ──────────────────────────────────────────────────────
// G2SettingPackage wrapper fields, cross-referenced against
// `ble/gen/g2_setting_pb.ts` in the g2-kit-unofficial reference:
//   field 1 commandId           (enum g2_settingCommandId)
//   field 2 magicRandom
//   field 3 DeviceReceiveInfoFromAPP
//   field 4 DeviceReceiveRequestFromAPP  ← battery lives here (inner field 12)
//   field 5 DeviceSendInfoToAPP
//   field 6 Device_Respond_To_App
//   field 7 App_Respond_To_Device
//
// Observed traffic on real glasses shows the device echoes the full
// request back in outer field 4 with every setting populated, for both
// explicit responses (flag=0x00) and async pushes (flag=0x01). The
// earlier code looked at outer field 6 which the firmware never emits,
// so every battery reading silently dropped on the floor.
#define G2_SET_F_CMD        1
#define G2_SET_F_MAGIC      2
#define G2_SET_F_REQ        4   // DeviceReceiveRequestFromAPP echo
#define G2_SET_CMD_REQUEST  2
#define G2_SET_REQ_BASIC    1
#define G2_SET_REQ_F_TYPE   1
// G2_SET_REQ_F_VER (5) and G2_SET_REQ_F_BATT (12) are defined in the
// header so diagnostic callers (G2_Glasses.cpp verbose dumper) can
// label known fields without hard-coding the numbers.

size_t g2BuildSettingBasicRequest(uint8_t seq, uint32_t magic,
                                  uint8_t* out, size_t outCap) {
  uint8_t payload[32];
  size_t pos = 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_SET_F_CMD, G2_SET_CMD_REQUEST)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_SET_F_MAGIC, magic)) return 0;
  size_t inner;
  if (!g2PbBeginNested(payload, sizeof(payload), &pos, G2_SET_F_REQ, &inner)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_SET_REQ_F_TYPE, G2_SET_REQ_BASIC)) return 0;
  if (!g2PbEndNested(payload, sizeof(payload), &pos, inner)) return 0;
  return g2BuildEnvelope(seq, G2_SID_SETTINGS, G2_FLAG_REQUEST,
                         payload, pos, out, outCap);
}

// Shared inner-body scanner used by all the sid=0x09 settings parsers.
// Walks every len-delim outer field (3..7) looking for `targetField` inside,
// returning a pointer+length into the body where the target's value starts
// (AFTER its tag). Caller picks the decode based on wire type.
//
// Returns true if the target was found and *outPos was written; false if
// not found (caller should keep its default output).
static bool findInnerField(const uint8_t* payload, size_t payloadLen,
                           uint32_t targetField, uint8_t targetWire,
                           const uint8_t** outBody, size_t* outBodyLen,
                           size_t* outValueStart) {
  size_t pos = 0;
  while (pos < payloadLen) {
    uint32_t field; uint8_t wire;
    if (!g2PbReadTag(payload, payloadLen, &pos, &field, &wire)) return false;
    if (wire == G2_PB_WIRE_LEN_DELIM && field >= 3 && field <= 7) {
      uint64_t sublen;
      if (!g2PbReadVarint(payload, payloadLen, &pos, &sublen)) return false;
      if (pos + sublen > payloadLen) return false;
      const uint8_t* sub = payload + pos;
      size_t subpos = 0;
      while (subpos < (size_t)sublen) {
        uint32_t sfield; uint8_t swire;
        if (!g2PbReadTag(sub, (size_t)sublen, &subpos, &sfield, &swire)) break;
        if (sfield == targetField && swire == targetWire) {
          if (outBody)       *outBody       = sub;
          if (outBodyLen)    *outBodyLen    = (size_t)sublen;
          if (outValueStart) *outValueStart = subpos;
          return true;
        }
        if (!g2PbSkipField(sub, (size_t)sublen, &subpos, swire)) break;
      }
      pos += (size_t)sublen;
      continue;
    }
    if (!g2PbSkipField(payload, payloadLen, &pos, wire)) return false;
  }
  return false;
}

// Parse the silent-mode flag from a sid=0x09 cmd=3 DeviceSendToAPP
// settings push. Schema verified 2026-04-26:
//   wrapper { f1=commandId=3, f2=magic, f5=deviceSendInfoToApp { f2=silent } }
// On-wire example (silent ON): `08 03 10 18 2A 02 10 01`
//                                                ^^ ^^
//                                                f5 inner f2=1
// Scoped specifically to outer f5 so we don't pick up the inner f2 of
// outer f4 (deviceReceiveRequestFromApp.autoBrightnessLevel) which
// shares a field number but means something completely different.
bool g2ParseSettingSilentMode(const uint8_t* payload, size_t payloadLen,
                              uint8_t* outFlag) {
  if (!payload || !outFlag) return false;
  size_t pos = 0;
  while (pos < payloadLen) {
    uint32_t field; uint8_t wire;
    if (!g2PbReadTag(payload, payloadLen, &pos, &field, &wire)) return false;
    if (field == G2_SET_F_SEND_INFO && wire == G2_PB_WIRE_LEN_DELIM) {
      uint64_t sublen;
      if (!g2PbReadVarint(payload, payloadLen, &pos, &sublen)) return false;
      if (pos + sublen > payloadLen) return false;
      const uint8_t* sub = payload + pos;
      size_t subpos = 0;
      while (subpos < (size_t)sublen) {
        uint32_t sf; uint8_t sw;
        if (!g2PbReadTag(sub, (size_t)sublen, &subpos, &sf, &sw)) break;
        if (sf == G2_SET_SEND_F_SILENT && sw == G2_PB_WIRE_VARINT) {
          uint64_t v;
          if (!g2PbReadVarint(sub, (size_t)sublen, &subpos, &v)) break;
          *outFlag = (v != 0) ? 1 : 0;
          return true;
        }
        if (!g2PbSkipField(sub, (size_t)sublen, &subpos, sw)) break;
      }
      pos += (size_t)sublen;
      continue;
    }
    if (!g2PbSkipField(payload, payloadLen, &pos, wire)) return false;
  }
  return false;
}

bool g2ParseSettingBattery(const uint8_t* payload, size_t payloadLen,
                           uint8_t* batteryPct) {
  if (!payload || !batteryPct) return false;
  const uint8_t* sub; size_t sublen; size_t sp;
  if (!findInnerField(payload, payloadLen, G2_SET_REQ_F_BATT,
                      G2_PB_WIRE_VARINT, &sub, &sublen, &sp)) return false;
  uint64_t v;
  if (!g2PbReadVarint(sub, sublen, &sp, &v)) return false;
  if (v > 100) v = 100;
  *batteryPct = (uint8_t)v;
  return true;
}

// Pull the firmware version string out of a settings push. Observed at
// field 5 of the DeviceReceiveRequestFromAPP body as ASCII like "2.1.1.10".
// `versionOut` receives a null-terminated string. Returns false if no
// string at field 5 is present (not every settings push carries the
// version; the firmware sprinkles it in as a fresh marker when something
// changes).
bool g2ParseSettingVersion(const uint8_t* payload, size_t payloadLen,
                           char* versionOut, size_t versionCap) {
  if (!payload || !versionOut || versionCap == 0) return false;
  versionOut[0] = '\0';
  const uint8_t* sub; size_t sublen; size_t sp;
  if (!findInnerField(payload, payloadLen, G2_SET_REQ_F_VER,
                      G2_PB_WIRE_LEN_DELIM, &sub, &sublen, &sp)) return false;
  uint64_t slen;
  if (!g2PbReadVarint(sub, sublen, &sp, &slen)) return false;
  if (sp + slen > sublen) return false;
  const size_t copy = (slen < versionCap - 1) ? (size_t)slen : versionCap - 1;
  memcpy(versionOut, sub + sp, copy);
  versionOut[copy] = '\0';
  return true;
}

// Diagnostic dump: walk every inner field of the settings body and print a
// one-line summary per field. Used during development to discover what
// numeric fields mean beyond the ones we've identified (VER, BATT).
// Handler code can call this when a verbose settings-decode flag is set.
//
// `logFn` is a user-supplied callback so this file doesn't need to know
// about the debug-macro plumbing upstream; pass nullptr to no-op.
typedef void (*G2SettingsFieldLog)(uint32_t field, uint8_t wire,
                                   uint64_t varintVal,
                                   const uint8_t* bytes, size_t byteLen);

void g2DumpSettingFields(const uint8_t* payload, size_t payloadLen,
                         G2SettingsFieldLog logFn) {
  if (!payload || !logFn) return;
  size_t pos = 0;
  while (pos < payloadLen) {
    uint32_t field; uint8_t wire;
    if (!g2PbReadTag(payload, payloadLen, &pos, &field, &wire)) return;
    if (wire == G2_PB_WIRE_LEN_DELIM && field >= 3 && field <= 7) {
      uint64_t sublen;
      if (!g2PbReadVarint(payload, payloadLen, &pos, &sublen)) return;
      if (pos + sublen > payloadLen) return;
      const uint8_t* sub = payload + pos;
      size_t subpos = 0;
      while (subpos < (size_t)sublen) {
        uint32_t sf; uint8_t sw;
        if (!g2PbReadTag(sub, (size_t)sublen, &subpos, &sf, &sw)) break;
        if (sw == G2_PB_WIRE_VARINT) {
          uint64_t v;
          size_t vstart = subpos;
          if (!g2PbReadVarint(sub, (size_t)sublen, &subpos, &v)) break;
          logFn(sf, sw, v, sub + vstart, subpos - vstart);
        } else if (sw == G2_PB_WIRE_LEN_DELIM) {
          uint64_t sl;
          if (!g2PbReadVarint(sub, (size_t)sublen, &subpos, &sl)) break;
          if (subpos + sl > (size_t)sublen) break;
          logFn(sf, sw, 0, sub + subpos, (size_t)sl);
          subpos += (size_t)sl;
        } else {
          if (!g2PbSkipField(sub, (size_t)sublen, &subpos, sw)) break;
        }
      }
      pos += (size_t)sublen;
      continue;
    }
    if (!g2PbSkipField(payload, payloadLen, &pos, wire)) return;
  }
}

// =============================================================================
// DevConfig builders — sid=0x80 (UX_DEVICE_SETTINGS_APP_ID)
// =============================================================================
// See header for tier policy and provenance. The wrapper uses
// G2_WRAP_F_CMD (1) for commandId and G2_WRAP_F_MAGIC (2) for magicRandom,
// which are the same field tags as EvenCore's wrapper, so we reuse them
// instead of defining DevCfg-specific aliases. Inner sub-message field
// tags follow dev_pair_manager.proto / dev_settings.proto.

// AuthMgr fields (dev_pair_manager.proto)
#define G2_DEVCFG_AUTHMGR_F_SEC_AUTH    1
#define G2_DEVCFG_AUTHMGR_F_PHONE_TYPE  2

// PipeRoleChange fields (dev_pair_manager.proto)
#define G2_DEVCFG_ROLECHG_F_AS_CMD_ROLE  1

// RingInfo fields (dev_pair_manager.proto)
#define G2_DEVCFG_RINGINFO_F_CONNECT_RING  1
#define G2_DEVCFG_RINGINFO_F_RING_MAC      2
#define G2_DEVCFG_RINGINFO_F_RING_NAME     3

// TimeSync fields (dev_settings.proto)
#define G2_DEVCFG_TIMESYNC_F_TIMESTAMP  1
#define G2_DEVCFG_TIMESYNC_F_TIMEZONE   2

// DevCfgDataPackage wrapper field tags for the per-cmd nested sub-messages
// (dev_config_protocol.proto). Note: TimeSync sits at field 128, which
// requires a 2-byte varint tag — g2PbBeginNested handles this transparently.
#define G2_DEVCFG_WRAP_F_AUTH_MGR        3
#define G2_DEVCFG_WRAP_F_ROLE_CHANGE     4
#define G2_DEVCFG_WRAP_F_RING_INFO       5
#define G2_DEVCFG_WRAP_F_BASE_HEARTBEAT  13
#define G2_DEVCFG_WRAP_F_TIME_SYNC       128

// Helper: encode int64 as varint (proto3 int64 wire format = varint of the
// 64-bit two's-complement bit pattern). Negative values produce 10-byte
// varints. We need this for TimeSync.timezone, which is declared int64 in
// the schema even though real-world values fit in a byte.
static bool g2PbWriteInt64(uint8_t* buf, size_t cap, size_t* pos,
                           uint32_t field, int64_t v) {
  if (!g2PbWriteTag(buf, cap, pos, field, G2_PB_WIRE_VARINT)) return false;
  return g2PbWriteVarint(buf, cap, pos, (uint64_t)v);
}

size_t g2BuildDevCfgHeartbeat(uint8_t seq, uint32_t magic,
                              uint8_t* out, size_t outCap) {
  uint8_t pb[16];
  size_t pos = 0;
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_WRAP_F_CMD,
                       G2_DEVCFG_CMD_BASE_HEART_BEAT)) return 0;
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  // BaseConnHeartBeat sub-message present but empty (no fields set on TX).
  size_t inner;
  if (!g2PbBeginNested(pb, sizeof(pb), &pos,
                       G2_DEVCFG_WRAP_F_BASE_HEARTBEAT, &inner)) return 0;
  if (!g2PbEndNested(pb, sizeof(pb), &pos, inner)) return 0;
  return g2BuildEnvelope(seq, G2_SID_DEV_CONFIG, G2_FLAG_REQUEST,
                         pb, pos, out, outCap);
}

size_t g2BuildDevCfgAuth(uint8_t seq, uint32_t magic,
                         uint8_t* out, size_t outCap) {
  uint8_t pb[32];
  size_t pos = 0;
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_WRAP_F_CMD,
                       G2_DEVCFG_CMD_AUTHENTICATION)) return 0;
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t inner;
  if (!g2PbBeginNested(pb, sizeof(pb), &pos,
                       G2_DEVCFG_WRAP_F_AUTH_MGR, &inner)) return 0;
  if (!g2PbWriteBool(pb, sizeof(pb), &pos,
                     G2_DEVCFG_AUTHMGR_F_SEC_AUTH, true)) return 0;
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos,
                       G2_DEVCFG_AUTHMGR_F_PHONE_TYPE,
                       G2_DEVCFG_PHONE_TYPE_ANDROID)) return 0;
  if (!g2PbEndNested(pb, sizeof(pb), &pos, inner)) return 0;
  return g2BuildEnvelope(seq, G2_SID_DEV_CONFIG, G2_FLAG_REQUEST,
                         pb, pos, out, outCap);
}

size_t g2BuildDevCfgPipeRoleChange(uint8_t seq, uint32_t magic, uint8_t role,
                                   uint8_t* out, size_t outCap) {
  if (role != G2_DEVCFG_ROLE_BOTH &&
      role != G2_DEVCFG_ROLE_RIGHT &&
      role != G2_DEVCFG_ROLE_LEFT) {
    return 0;
  }
  uint8_t pb[24];
  size_t pos = 0;
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_WRAP_F_CMD,
                       G2_DEVCFG_CMD_PIPE_ROLE_CHANGE)) return 0;
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t inner;
  if (!g2PbBeginNested(pb, sizeof(pb), &pos,
                       G2_DEVCFG_WRAP_F_ROLE_CHANGE, &inner)) return 0;
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos,
                       G2_DEVCFG_ROLECHG_F_AS_CMD_ROLE, role)) return 0;
  if (!g2PbEndNested(pb, sizeof(pb), &pos, inner)) return 0;
  return g2BuildEnvelope(seq, G2_SID_DEV_CONFIG, G2_FLAG_REQUEST,
                         pb, pos, out, outCap);
}

size_t g2BuildDevCfgTimeSync(uint8_t seq, uint32_t magic,
                             uint32_t timestamp, int32_t tzQuarterHours,
                             uint8_t* out, size_t outCap) {
  // Validation: reject pre-2020 / post-2099 timestamps and out-of-real-world
  // timezone offsets. Caller should pass values from a synced RTC.
  static const uint32_t TS_MIN = 1577836800u;  // 2020-01-01T00:00:00Z
  static const uint32_t TS_MAX = 4102444800u;  // 2099-12-31T00:00:00Z (approx)
  if (timestamp < TS_MIN || timestamp > TS_MAX) return 0;
  if (tzQuarterHours < -56 || tzQuarterHours > 56) return 0;

  uint8_t pb[40];
  size_t pos = 0;
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_WRAP_F_CMD,
                       G2_DEVCFG_CMD_TIME_SYNC)) return 0;
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t inner;
  if (!g2PbBeginNested(pb, sizeof(pb), &pos,
                       G2_DEVCFG_WRAP_F_TIME_SYNC, &inner)) return 0;
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos,
                       G2_DEVCFG_TIMESYNC_F_TIMESTAMP, timestamp)) return 0;
  // timezone is int64 in the schema; sign-extend the int32 value.
  if (!g2PbWriteInt64(pb, sizeof(pb), &pos,
                      G2_DEVCFG_TIMESYNC_F_TIMEZONE,
                      (int64_t)tzQuarterHours)) return 0;
  if (!g2PbEndNested(pb, sizeof(pb), &pos, inner)) return 0;
  return g2BuildEnvelope(seq, G2_SID_DEV_CONFIG, G2_FLAG_REQUEST,
                         pb, pos, out, outCap);
}

size_t g2BuildDevCfgRingConnect(uint8_t seq, uint32_t magic,
                                bool connect,
                                const uint8_t* ringMacBleOrder,
                                const char* ringName,
                                uint8_t* out, size_t outCap) {
  if (!ringMacBleOrder || !ringName) return 0;
  size_t nameLen = 0;
  while (ringName[nameLen] && nameLen <= 32) nameLen++;
  if (nameLen == 0 || nameLen > 32) return 0;

  // Reverse the MAC: BLE address order → on-the-wire order per FlutterApp
  // g2_messages.dart:109 (Uint8List.fromList(ringMac.reversed.toList())).
  uint8_t macReversed[6];
  for (int i = 0; i < 6; i++) macReversed[i] = ringMacBleOrder[5 - i];

  uint8_t pb[80];
  size_t pos = 0;
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_WRAP_F_CMD,
                       G2_DEVCFG_CMD_RING_CONNECT_INFO)) return 0;
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_WRAP_F_MAGIC, magic)) return 0;
  size_t inner;
  if (!g2PbBeginNested(pb, sizeof(pb), &pos,
                       G2_DEVCFG_WRAP_F_RING_INFO, &inner)) return 0;
  if (!g2PbWriteBool(pb, sizeof(pb), &pos,
                     G2_DEVCFG_RINGINFO_F_CONNECT_RING, connect)) return 0;
  if (!g2PbWriteBytes(pb, sizeof(pb), &pos,
                      G2_DEVCFG_RINGINFO_F_RING_MAC,
                      macReversed, sizeof(macReversed))) return 0;
  if (!g2PbWriteBytes(pb, sizeof(pb), &pos,
                      G2_DEVCFG_RINGINFO_F_RING_NAME,
                      reinterpret_cast<const uint8_t*>(ringName),
                      nameLen)) return 0;
  if (!g2PbEndNested(pb, sizeof(pb), &pos, inner)) return 0;
  return g2BuildEnvelope(seq, G2_SID_DEV_CONFIG, G2_FLAG_REQUEST,
                         pb, pos, out, outCap);
}

// =============================================================================
// g2BuildRingRawDataPush — synthesise a sid=0x90 RingDataPackage frame
// =============================================================================
// Field tag numbers for RingRawData (encoded as field<<3 | wire-type=0 for
// varint = the byte value before the value bytes):
#define G2_RING_RAW_F_BATTERY            ((1u  << 3) | 0)
#define G2_RING_RAW_F_CHARGE_STATES      ((2u  << 3) | 0)
#define G2_RING_RAW_F_HR                 ((3u  << 3) | 0)
#define G2_RING_RAW_F_HR_TS              ((4u  << 3) | 0)
#define G2_RING_RAW_F_SPO2               ((5u  << 3) | 0)
#define G2_RING_RAW_F_SPO2_TS            ((6u  << 3) | 0)
#define G2_RING_RAW_F_HRV                ((7u  << 3) | 0)
#define G2_RING_RAW_F_HRV_TS             ((8u  << 3) | 0)
#define G2_RING_RAW_F_TEMP               ((9u  << 3) | 0)
#define G2_RING_RAW_F_TEMP_TS            ((10u << 3) | 0)
#define G2_RING_RAW_F_ACT_KCAL           ((11u << 3) | 0)
#define G2_RING_RAW_F_ACT_KCAL_TS        ((12u << 3) | 0)
#define G2_RING_RAW_F_ALL_KCAL           ((13u << 3) | 0)
#define G2_RING_RAW_F_ALL_KCAL_TS        ((14u << 3) | 0)
#define G2_RING_RAW_F_STEPS              ((15u << 3) | 0)
#define G2_RING_RAW_F_STEPS_TS           ((16u << 3) | 0)
// Field tag numbers for the outer RingDataPackage:
#define G2_RING_PKG_F_CMD                ((1u  << 3) | 0)
#define G2_RING_PKG_F_MAGIC              ((2u  << 3) | 0)
#define G2_RING_PKG_F_RAW_DATA           ((4u  << 3) | 2)  // wire-type 2 = length-delim

size_t g2BuildRingRawDataPush(uint8_t seq, uint32_t magic,
                              const G2RingPushFields& f,
                              uint8_t* out, size_t outCap) {
  uint8_t pb[160];
  size_t pos = 0;

  // Outer wrapper: commandId=2, magicRandom=N, rawData=<nested>
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_PKG_F_CMD,
                       G2_RING_CMD_RAW_DATA)) return 0;
  if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_PKG_F_MAGIC, magic)) return 0;

  // Nested RingRawData. Fields written only when *_valid is set.
  size_t inner;
  if (!g2PbBeginNested(pb, sizeof(pb), &pos,
                       G2_RING_PKG_F_RAW_DATA, &inner)) return 0;
  if (f.battery_valid &&
      !g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_RAW_F_BATTERY,
                       (uint32_t)f.battery)) return 0;
  if (f.chargeStates_valid &&
      !g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_RAW_F_CHARGE_STATES,
                       (uint32_t)f.chargeStates)) return 0;
  if (f.hr_valid) {
    if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_RAW_F_HR,
                         (uint32_t)f.hr)) return 0;
    if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_RAW_F_HR_TS,
                         (uint32_t)f.hrTs)) return 0;
  }
  if (f.spo2_valid) {
    if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_RAW_F_SPO2,
                         (uint32_t)f.spo2)) return 0;
    if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_RAW_F_SPO2_TS,
                         (uint32_t)f.spo2Ts)) return 0;
  }
  if (f.hrv_valid) {
    if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_RAW_F_HRV,
                         (uint32_t)f.hrv)) return 0;
    if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_RAW_F_HRV_TS,
                         (uint32_t)f.hrvTs)) return 0;
  }
  if (f.temp_valid) {
    if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_RAW_F_TEMP,
                         (uint32_t)f.temp)) return 0;
    if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_RAW_F_TEMP_TS,
                         (uint32_t)f.tempTs)) return 0;
  }
  if (f.actKcal_valid) {
    if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_RAW_F_ACT_KCAL,
                         (uint32_t)f.actKcal)) return 0;
    if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_RAW_F_ACT_KCAL_TS,
                         (uint32_t)f.actKcalTs)) return 0;
  }
  if (f.allKcal_valid) {
    if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_RAW_F_ALL_KCAL,
                         (uint32_t)f.allKcal)) return 0;
    if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_RAW_F_ALL_KCAL_TS,
                         (uint32_t)f.allKcalTs)) return 0;
  }
  if (f.steps_valid) {
    if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_RAW_F_STEPS,
                         (uint32_t)f.steps)) return 0;
    if (!g2PbWriteUint32(pb, sizeof(pb), &pos, G2_RING_RAW_F_STEPS_TS,
                         (uint32_t)f.stepsTs)) return 0;
  }
  if (!g2PbEndNested(pb, sizeof(pb), &pos, inner)) return 0;

  return g2BuildEnvelope(seq, G2_SID_RING_RAW_DATA, G2_FLAG_REQUEST,
                         pb, pos, out, outCap);
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
