#include "System_G2_Protocol.h"

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

size_t g2BuildEnvelope(uint8_t sid, uint8_t flag, uint8_t seq, uint32_t magic,
                       const uint8_t* payload, size_t payloadLen,
                       uint8_t* out, size_t outCap) {
  const size_t total = G2_ENVELOPE_HDR_LEN + payloadLen + G2_ENVELOPE_CRC_LEN;
  if (total > outCap) return 0;
  if (total > 0xFFFF) return 0;  // length field is u16

  out[0] = G2_PREAMBLE_0;
  out[1] = G2_PREAMBLE_1;
  out[2] = (uint8_t)(total & 0xFF);
  out[3] = (uint8_t)((total >> 8) & 0xFF);
  out[4] = sid;
  out[5] = flag;
  out[6] = seq;
  out[7]  = (uint8_t)(magic & 0xFF);
  out[8]  = (uint8_t)((magic >> 8) & 0xFF);
  out[9]  = (uint8_t)((magic >> 16) & 0xFF);
  out[10] = (uint8_t)((magic >> 24) & 0xFF);
  if (payload && payloadLen > 0) {
    memcpy(out + G2_ENVELOPE_HDR_LEN, payload, payloadLen);
  }
  // CRC covers everything from the preamble through the last payload byte.
  uint16_t crc = g2CrcCcittFalse(out, G2_ENVELOPE_HDR_LEN + payloadLen);
  out[G2_ENVELOPE_HDR_LEN + payloadLen]     = (uint8_t)((crc >> 8) & 0xFF);
  out[G2_ENVELOPE_HDR_LEN + payloadLen + 1] = (uint8_t)(crc & 0xFF);
  return total;
}

bool g2ParseEnvelope(const uint8_t* in, size_t len, G2EnvelopeView* out) {
  if (!in || !out) return false;
  if (len < G2_ENVELOPE_HDR_LEN + G2_ENVELOPE_CRC_LEN) return false;
  if (in[0] != G2_PREAMBLE_0 || in[1] != G2_PREAMBLE_1) return false;

  const uint16_t declared = (uint16_t)in[2] | ((uint16_t)in[3] << 8);
  if (declared != len) return false;

  const size_t payloadLen = len - G2_ENVELOPE_HDR_LEN - G2_ENVELOPE_CRC_LEN;
  const uint16_t rcvCrc =
      ((uint16_t)in[G2_ENVELOPE_HDR_LEN + payloadLen] << 8) |
      (uint16_t)in[G2_ENVELOPE_HDR_LEN + payloadLen + 1];
  const uint16_t calcCrc = g2CrcCcittFalse(in, G2_ENVELOPE_HDR_LEN + payloadLen);
  if (rcvCrc != calcCrc) return false;

  out->sid  = in[4];
  out->flag = in[5];
  out->seq  = in[6];
  out->magic = (uint32_t)in[7]
             | ((uint32_t)in[8]  << 8)
             | ((uint32_t)in[9]  << 16)
             | ((uint32_t)in[10] << 24);
  out->payload    = payloadLen ? (in + G2_ENVELOPE_HDR_LEN) : nullptr;
  out->payloadLen = payloadLen;
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

// Nested-message support. We always reserve 2 bytes for the length prefix
// and later patch them as a 2-byte varint. This limits nested payloads to
// 2^14 - 1 = 16383 bytes, which is plenty for G2 text/heartbeat/audio but
// NOT enough for raw image fragments — those hit 4 KB max anyway so still OK.
static constexpr size_t NESTED_LEN_RESERVE = 2;

bool g2PbBeginNested(uint8_t* buf, size_t cap, size_t* pos,
                     uint32_t field, size_t* innerStart) {
  if (!g2PbWriteTag(buf, cap, pos, field, G2_PB_WIRE_LEN_DELIM)) return false;
  if (*pos + NESTED_LEN_RESERVE > cap) return false;
  // Reserve 2 bytes; we'll patch after the inner body is known.
  *pos += NESTED_LEN_RESERVE;
  *innerStart = *pos;
  return true;
}

bool g2PbEndNested(uint8_t* buf, size_t cap, size_t* pos, size_t innerStart) {
  (void)cap;
  const size_t innerLen = *pos - innerStart;
  if (innerLen > 0x3FFF) return false;  // exceeds 2-byte varint capacity
  // Protobuf wants canonical (minimal) varints. If the inner payload fits in
  // one byte (<128), shift it back to reclaim the reserved second byte —
  // otherwise strict decoders reject `0x82 0x00`-style non-canonical encodings.
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
    if (shift >= 64) return false;  // malformed
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

// ── High-level message builders ──────────────────────────────────────────────

// EvenHub wrapper field numbers (see reference: ble/gen/EvenHub_pb.ts):
//   1  Cmd               (varint, enum)
//   2  MagicRandom       (varint)
//   3  CreateMessage     (len-delim, CreateStartUpPageContainer)
//   5  ImgRawMsg         (len-delim)
//   7  RebuildContainer  (len-delim, RebuildPageContainer)
//   9  TextUpgrade       (len-delim, TextContainerUpgrade)
//   11 ShutDownCmd       (len-delim, ShutDownContaniner)
//   14 HeartPacketCmd    (len-delim, HeartBeatPacket)
//   18 AudioCtrCommand   (len-delim, AudioCtrCmd)
#define G2_WRAP_F_CMD            1
#define G2_WRAP_F_MAGIC          2
#define G2_WRAP_F_CREATE         3
#define G2_WRAP_F_IMAGE          5
#define G2_WRAP_F_REBUILD        7
#define G2_WRAP_F_TEXT_UPGRADE   9
#define G2_WRAP_F_SHUTDOWN       11
#define G2_WRAP_F_HEARTBEAT      14
#define G2_WRAP_F_AUDIO          18

// TextContainerProperty field numbers (flat — no Rect/TextStyle submessages):
//   1..4   XPosition, YPosition, Width, Height
//   5..8   BorderWidth, BorderColor, BorderRadius, PaddingLength
//   9      ContainerID
//   10     ContainerName (string)
//   11     IsEventCapture
//   12     Content (string)
#define G2_TEXT_F_X         1
#define G2_TEXT_F_Y         2
#define G2_TEXT_F_W         3
#define G2_TEXT_F_H         4
#define G2_TEXT_F_CID       9
#define G2_TEXT_F_CNAME     10
#define G2_TEXT_F_EVCAP     11
#define G2_TEXT_F_CONTENT   12

// CreateStartUpPageContainer fields:
//   1 ContainerTotalNum, 3 repeated TextObject
// RebuildPageContainer: same shape.
#define G2_PAGE_F_TOTAL     1
#define G2_PAGE_F_TEXT_OBJ  3

// HeartBeatPacket: 1 Cnt
#define G2_HB_F_CNT         1
// ShutDownContaniner: 1 exitMode
#define G2_SHUT_F_MODE      1
// AudioCtrCmd: 1 AudoFuncEn
#define G2_AUDIO_F_EN       1

// Reasonable container default: full lens area with a single event-capturing
// text box. Values below are a conservative viewport — tuning is the caller's
// job and does not affect wire-level correctness.
static constexpr uint32_t TEXT_X = 0;
static constexpr uint32_t TEXT_Y = 0;
static constexpr uint32_t TEXT_W = 576;
static constexpr uint32_t TEXT_H = 288;
static constexpr uint32_t TEXT_CID = 1;

// Build a TextContainerProperty into a nested field of the wrapper.
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
  if (!g2PbWriteBool  (buf, cap, pos, G2_TEXT_F_EVCAP, true)) return false;
  if (!g2PbWriteString(buf, cap, pos, G2_TEXT_F_CONTENT, content ? content : "")) return false;
  return g2PbEndNested(buf, cap, pos, inner);
}

// Build an EvenHub envelope around a single-TextContainer page. Used by both
// CREATE (Cmd=0, wrapper field 3) and REBUILD (Cmd=7, wrapper field 7).
static size_t buildTextPage(uint8_t seq, uint32_t magic, uint32_t cmd,
                            uint32_t wrapperField,
                            const char* containerName, const char* content,
                            uint8_t* out, size_t outCap) {
  uint8_t payload[256];
  size_t pos = 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_WRAP_F_CMD, cmd)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_WRAP_F_MAGIC, magic)) return 0;

  size_t pageStart;
  if (!g2PbBeginNested(payload, sizeof(payload), &pos, wrapperField, &pageStart)) return 0;
  if (!g2PbWriteUint32(payload, sizeof(payload), &pos, G2_PAGE_F_TOTAL, 1)) return 0;
  if (!writeTextProperty(payload, sizeof(payload), &pos, containerName, content)) return 0;
  if (!g2PbEndNested(payload, sizeof(payload), &pos, pageStart)) return 0;

  return g2BuildEnvelope(G2_SID_EVEN_HUB, G2_FLAG_REQUEST, seq, magic,
                         payload, pos, out, outCap);
}

size_t g2BuildCreateStartupText(uint8_t seq, uint32_t magic,
                                const char* containerName,
                                const char* initialContent,
                                uint8_t* out, size_t outCap) {
  return buildTextPage(seq, magic, G2_CMD_CREATE_STARTUP, G2_WRAP_F_CREATE,
                       containerName, initialContent, out, outCap);
}

size_t g2BuildRebuildText(uint8_t seq, uint32_t magic,
                          const char* containerName,
                          const char* content,
                          uint8_t* out, size_t outCap) {
  return buildTextPage(seq, magic, G2_CMD_REBUILD_PAGE, G2_WRAP_F_REBUILD,
                       containerName, content, out, outCap);
}

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

  return g2BuildEnvelope(G2_SID_EVEN_HUB, G2_FLAG_REQUEST, seq, magic,
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

  return g2BuildEnvelope(G2_SID_EVEN_HUB, G2_FLAG_REQUEST, seq, magic,
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

  return g2BuildEnvelope(G2_SID_EVEN_HUB, G2_FLAG_REQUEST, seq, magic,
                         payload, pos, out, outCap);
}

// The app-launch prelude is a byte-literal in the reference repo
// (ble/messages.ts PRELUDE_F5872) — its nested payload has no recoverable
// field names, so we reproduce the inner protobuf verbatim and only parametrise
// the outer envelope seq. The reference uses seq=1 magic=0x9c (156) always;
// we match that because the firmware keys ACKs on magic's low byte and 0x9c
// is baked into its AppLaunch dispatch.
static const uint8_t G2_APPLAUNCH_INNER[] = {
  0x08, 0x02,                    // field 1 varint 2  (type)
  0x10, 0x9c, 0x01,              // field 2 varint 156 (magic)
  0x22, 0x0a,                    // field 4 len-delim len=10
    0x1a, 0x08,                  //   inner f3 len-delim len=8
      0x12, 0x06,                //     f2 len-delim len=6
        0x12, 0x04,              //       f2 len-delim len=4
          0x08, 0x00,            //         f1 varint 0
          0x10, 0x00             //         f2 varint 0
};

size_t g2BuildAppLaunch(uint8_t seq, uint8_t* out, size_t outCap) {
  // Magic must be the specific prelude magic (0x9c / 156) — firmware keys its
  // ACK lookup on that value.
  return g2BuildEnvelope(G2_SID_APP_LAUNCH, G2_FLAG_ASYNC, seq, 0x0000009Cu,
                         G2_APPLAUNCH_INNER, sizeof(G2_APPLAUNCH_INNER),
                         out, outCap);
}

// ── Settings (sid=0x09) ──────────────────────────────────────────────────────
// G2SettingPackage wrapper:
//   1 commandId (enum: 1=DeviceReceiveInfo, 2=DeviceReceiveRequest, ...)
//   2 magicRandom
//   4 deviceReceiveRequestFromApp (len-delim, DeviceReceiveRequestFromAPP)
//   6 deviceRespondToApp          (response direction)
//
// DeviceReceiveRequestFromAPP (used both on request and response):
//   1 settingInfoType (enum: 0 brightness, 1 basic)
//  12 battery (on response)
#define G2_SET_F_CMD        1
#define G2_SET_F_MAGIC      2
#define G2_SET_F_REQ        4
#define G2_SET_F_RESP       6
#define G2_SET_CMD_REQUEST  2
#define G2_SET_REQ_BASIC    1
#define G2_SET_REQ_F_TYPE   1
#define G2_SET_REQ_F_BATT   12

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

  return g2BuildEnvelope(G2_SID_SETTINGS, G2_FLAG_REQUEST, seq, magic,
                         payload, pos, out, outCap);
}

bool g2ParseSettingBattery(const uint8_t* payload, size_t payloadLen,
                           uint8_t* batteryPct) {
  if (!payload || !batteryPct) return false;
  size_t pos = 0;
  // Walk the outer G2SettingPackage looking for field 6 (deviceRespondToApp)
  // — the battery reply lives inside that submessage as field 12.
  while (pos < payloadLen) {
    uint32_t field;
    uint8_t wire;
    if (!g2PbReadTag(payload, payloadLen, &pos, &field, &wire)) return false;
    if (field == G2_SET_F_RESP && wire == G2_PB_WIRE_LEN_DELIM) {
      uint64_t sublen;
      if (!g2PbReadVarint(payload, payloadLen, &pos, &sublen)) return false;
      if (pos + sublen > payloadLen) return false;
      const uint8_t* sub = payload + pos;
      size_t subpos = 0;
      while (subpos < (size_t)sublen) {
        uint32_t sfield;
        uint8_t  swire;
        if (!g2PbReadTag(sub, (size_t)sublen, &subpos, &sfield, &swire)) return false;
        if (sfield == G2_SET_REQ_F_BATT && swire == G2_PB_WIRE_VARINT) {
          uint64_t v;
          if (!g2PbReadVarint(sub, (size_t)sublen, &subpos, &v)) return false;
          if (v > 100) v = 100;
          *batteryPct = (uint8_t)v;
          return true;
        }
        if (!g2PbSkipField(sub, (size_t)sublen, &subpos, swire)) return false;
      }
      pos += (size_t)sublen;
      continue;
    }
    if (!g2PbSkipField(payload, payloadLen, &pos, wire)) return false;
  }
  return false;
}
