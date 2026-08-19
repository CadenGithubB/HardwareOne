#include "System_R1_Protocol.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "System_Debug.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <array>

// =============================================================================
// CRC16 — CCITT-XMODEM-like with the magic-shift form used by the ring.
// =============================================================================
// Direct port of crc16CcittLike() in
// docs/FlutterApp-main/lib/src/core/crc.dart. Verified against the known
// frame `64 01 64 03 00 00 05 12 00 10 FF 33 48 BB 69` → 0x7780, captured
// from the FlutterApp test fixture for systemTime (serialId=3).
//
//   crc = 0xFFFF
//   for each byte b:
//     crc = ((crc >> 8) | ((crc << 8) & 0xFF00)) ^ b
//     crc ^= (crc & 0xFF) >> 4
//     crc ^= (crc << 12) & 0xFFFF
//     crc ^= ((crc & 0xFF) << 5) & 0xFFFF
uint16_t r1Crc16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc = (uint16_t)((crc >> 8) | ((crc << 8) & 0xFF00)) ^ data[i];
    crc = (uint16_t)(crc ^ ((crc & 0xFF) >> 4));
    crc = (uint16_t)(crc ^ ((crc << 12) & 0xFFFF));
    crc = (uint16_t)(crc ^ (((crc & 0xFF) << 5) & 0xFFFF));
  }
  return crc;
}

uint16_t r1ModelCrc16(const uint8_t* model, size_t modelLen) {
  if (modelLen < 12) return 0;
  // CRC input = model[0..3] + model[5..9] + model[12..]. We assemble inline
  // to avoid a dynamic buffer; uses a stack scratch sized to header (4+5)
  // plus payload length.
  uint8_t scratch[9 + R1_MAX_PAYLOAD];
  if (modelLen - 12 > R1_MAX_PAYLOAD) return 0;
  size_t off = 0;
  memcpy(scratch + off, model, 4);          off += 4;       // [0..3]
  memcpy(scratch + off, model + 5, 5);      off += 5;       // [5..9]
  if (modelLen > 12) {
    memcpy(scratch + off, model + 12, modelLen - 12);
    off += modelLen - 12;
  }
  return r1Crc16(scratch, off);
}

// =============================================================================
// CRC32 — Castagnoli polynomial 0x1EDC6F41, init=0, no reflection, no final XOR.
// =============================================================================
// Port of fileDataCrc32() in docs/FlutterApp-main/lib/src/core/crc.dart.
// The table is built once on first call (lazy init keeps it out of .bss
// until the ring code actually runs — DRAM is tight on this device).
//
// NOT zlib CRC32. Don't substitute esp_crc32_le; it uses a different
// polynomial AND reflection convention.

// Table built at COMPILE TIME (constexpr) so the 1 KB of lookup data lives in
// flash .rodata instead of a runtime-initialized internal-DRAM .bss buffer.
// Same algorithm as the previous runtime builder (poly 0x1EDC6F41, MSB-first,
// non-reflected) evaluated by the compiler → byte-identical output.
static constexpr std::array<uint32_t, 256> r1BuildCrc32Table() {
  std::array<uint32_t, 256> table{};
  const uint32_t poly = 0x1EDC6F41u;
  for (int v = 0; v < 256; v++) {
    uint32_t crc = (uint32_t)v << 24;
    for (int i = 0; i < 8; i++) {
      crc = (crc & 0x80000000u) ? (uint32_t)((crc << 1) ^ poly)
                                : (uint32_t)(crc << 1);
    }
    table[v] = crc;
  }
  return table;
}
static constexpr std::array<uint32_t, 256> kR1Crc32Table = r1BuildCrc32Table();

uint32_t r1Crc32(const uint8_t* data, size_t len) {
  uint32_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    uint8_t idx = (uint8_t)(data[i] ^ ((crc >> 24) & 0xFF));
    crc = (crc << 8) ^ kR1Crc32Table[idx];
  }
  return crc;
}

// =============================================================================
// Status byte
// =============================================================================
static inline uint8_t r1EncodeStatus(uint8_t type, uint8_t method, uint8_t ack) {
  return (uint8_t)((type & 0x1) | ((method & 0x1) << 1) | ((ack & 0x3) << 2));
}

static inline void r1DecodeStatus(uint8_t b, uint8_t& type, uint8_t& method, uint8_t& ack) {
  type   = (uint8_t)(b & 0x1);
  method = (uint8_t)((b >> 1) & 0x1);
  ack    = (uint8_t)((b >> 2) & 0x3);
}

static inline void writeU16LE(uint8_t* p, uint16_t value) {
  p[0] = (uint8_t)(value & 0xFF);
  p[1] = (uint8_t)((value >> 8) & 0xFF);
}

static inline void writeU32LE(uint8_t* p, uint32_t value) {
  p[0] = (uint8_t)(value & 0xFF);
  p[1] = (uint8_t)((value >> 8) & 0xFF);
  p[2] = (uint8_t)((value >> 16) & 0xFF);
  p[3] = (uint8_t)((value >> 24) & 0xFF);
}

const char* r1ProtocolProfileName(R1ProtocolProfile profile) {
  switch (profile) {
    case R1_PROFILE_FW_2_2_7_0005: return "2.2.7.0005";
    case R1_PROFILE_UNKNOWN:
    default:                       return "unknown";
  }
}

R1ProtocolProfile r1ProfileForFirmware(const char* firmware) {
  if (firmware && strcmp(firmware, "2.2.7.0005") == 0) {
    return R1_PROFILE_FW_2_2_7_0005;
  }
  return R1_PROFILE_UNKNOWN;
}

const char* r1ParseErrorName(R1ParseError error) {
  switch (error) {
    case R1_PARSE_OK:                 return "ok";
    case R1_PARSE_BAD_CRC:            return "badCrc";
    case R1_PARSE_LENGTH:             return "length";
    case R1_PARSE_WRONG_PROFILE:      return "wrongProfile";
    case R1_PARSE_UNSUPPORTED_LAYOUT: return "unsupportedLayout";
    case R1_PARSE_SLOT_RANGE:         return "slotRange";
    case R1_PARSE_VALUE_RANGE:        return "valueRange";
    case R1_PARSE_TOO_LARGE:          return "tooLarge";
    case R1_PARSE_DUPLICATE_SLOT:     return "duplicateSlot";
    default:                          return "unsupportedLayout";
  }
}

// =============================================================================
// R1Encoder
// =============================================================================

uint16_t R1Encoder::nextSerial() {
  serial_ = (uint16_t)((serial_ + 1) & 0xFFFF);
  return serial_;
}

R1Frame R1Encoder::build(uint8_t module, uint8_t cmd, uint8_t subCmd,
                         uint8_t statusType, uint8_t statusMethod, uint8_t statusAck,
                         const uint8_t* payload, size_t payloadLen) {
  R1Frame f = {};
  if (payloadLen > R1_MAX_PAYLOAD || (payloadLen != 0 && !payload)) {
    f.length = 0;
    return f;
  }
  uint16_t serial = nextSerial();
  f.serial = serial;

  // Build the model in-place inside the outbound buffer (5-byte envelope
  // prefix, then 12-byte header, then payload).
  const size_t envelopePrefix = 1 + 4;          // transferType + CRC32 placeholder
  const size_t modelLen = 12 + payloadLen;
  const size_t totalLen = envelopePrefix + modelLen;
  if (totalLen > R1_MAX_FRAME) {
    f.length = 0;
    return f;
  }

  uint8_t* out = f.bytes;
  out[0] = 0x00;                                // transferType — we never send
                                                // file-transfer (non-zero) frames

  uint8_t* model = out + envelopePrefix;
  model[ 0] = 0x64;                             // version
  model[ 1] = module;
  model[ 2] = 0x64;                             // moduleVersion
  model[ 3] = (uint8_t)(serial & 0xFF);
  model[ 4] = (uint8_t)((serial >> 8) & 0xFF);
  model[ 5] = r1EncodeStatus(statusType, statusMethod, statusAck);
  model[ 6] = cmd;
  model[ 7] = subCmd;
  model[ 8] = (uint8_t)(modelLen & 0xFF);
  model[ 9] = (uint8_t)((modelLen >> 8) & 0xFF);
  model[10] = 0x00;                             // CRC16 placeholder (excluded)
  model[11] = 0x00;
  if (payloadLen > 0) {
    memcpy(model + 12, payload, payloadLen);
  }

  // CRC16 over the model with bytes [4] and [10..11] excluded.
  uint16_t crc16 = r1ModelCrc16(model, modelLen);
  model[10] = (uint8_t)(crc16 & 0xFF);
  model[11] = (uint8_t)((crc16 >> 8) & 0xFF);

  // CRC32 over the full (now-finalised) model. Stored LE in envelope[1..4].
  uint32_t crc32 = r1Crc32(model, modelLen);
  out[1] = (uint8_t)(crc32 & 0xFF);
  out[2] = (uint8_t)((crc32 >> 8) & 0xFF);
  out[3] = (uint8_t)((crc32 >> 16) & 0xFF);
  out[4] = (uint8_t)((crc32 >> 24) & 0xFF);

  f.length = totalLen;
  return f;
}

R1Frame R1Encoder::buildPairAuth() {
  // Verified wire frame from FlutterApp test fixture (serialId=1):
  //   00 F9531997 64 01 64 01 00 00 00 08 0D 00 3F01 01
  // i.e. status=notify/get/ok, payload=[0x01].
  const uint8_t payload[1] = { 0x01 };
  return build(R1_MODULE_SYSTEM, R1_CMD_SYSTEM, R1_SUB_PAIR_AUTH,
               R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_GET, R1_STATUS_ACK_OK,
               payload, sizeof(payload));
}

R1Frame R1Encoder::buildSyncTime(int16_t tzOffsetMinutes, uint32_t epochSeconds) {
  // Verified wire frame from FlutterApp test fixture (serialId=3, tz=-4h, 2026-03-19):
  //   00 70BAC9D4 64 01 64 03 00 02 00 05 12 00 8077 10FF3348BB69
  // i.e. status=notify/SET/ok, payload = i16(tz) + u32(epoch) LE.
  if (tzOffsetMinutes < -720 || tzOffsetMinutes > 840) return R1Frame{};
  uint8_t payload[6];
  uint16_t tz = (uint16_t)tzOffsetMinutes;     // sign-extended via 2's complement cast
  payload[0] = (uint8_t)(tz & 0xFF);
  payload[1] = (uint8_t)((tz >> 8) & 0xFF);
  payload[2] = (uint8_t)(epochSeconds & 0xFF);
  payload[3] = (uint8_t)((epochSeconds >> 8) & 0xFF);
  payload[4] = (uint8_t)((epochSeconds >> 16) & 0xFF);
  payload[5] = (uint8_t)((epochSeconds >> 24) & 0xFF);
  return build(R1_MODULE_SYSTEM, R1_CMD_SYSTEM, R1_SUB_SYSTEM_TIME,
               R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_SET, R1_STATUS_ACK_OK,
               payload, sizeof(payload));
}

R1Frame R1Encoder::buildAdvStart(R1ProtocolProfile profile,
                                 const uint8_t* rightMac6,
                                 const uint8_t* leftMac6) {
  if (profile != R1_PROFILE_FW_2_2_7_0005 || !rightMac6 || !leftMac6) {
    return R1Frame{};
  }
  uint8_t payload[12];
  for (size_t i = 0; i < 6; ++i) {
    payload[i] = rightMac6[5 - i];
    payload[6 + i] = leftMac6[5 - i];
  }
  return build(R1_MODULE_SYSTEM, R1_CMD_SYSTEM, R1_SUB_ADV_START,
               R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_GET, R1_STATUS_ACK_OK,
               payload, sizeof(payload));
}

R1Frame R1Encoder::buildHeartbeat() {
  return build(R1_MODULE_SYSTEM, R1_CMD_SYSTEM, R1_SUB_HEARTBEAT,
               R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_GET, R1_STATUS_ACK_OK,
               nullptr, 0);
}

R1Frame R1Encoder::buildDeviceInfoQuery() {
  return build(R1_MODULE_SYSTEM, R1_CMD_SYSTEM, R1_SUB_DEVICE_INFO,
               R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_GET, R1_STATUS_ACK_OK,
               nullptr, 0);
}

R1Frame R1Encoder::buildHealthCollectionSet(R1ProtocolProfile profile,
                                            uint32_t epochSeconds,
                                            bool enabled) {
  if (profile != R1_PROFILE_FW_2_2_7_0005) return R1Frame{};
  uint8_t payload[12] = {};
  writeU32LE(payload, epochSeconds);
  payload[4] = enabled ? 1 : 0;
  return build(R1_MODULE_SYSTEM, R1_CMD_SYSTEM, R1_SUB_HEALTH_SETTINGS,
               R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_SET, R1_STATUS_ACK_OK,
               payload, sizeof(payload));
}

R1Frame R1Encoder::buildLowPowerQuery() {
  return build(R1_MODULE_SYSTEM, R1_CMD_SYSTEM, R1_SUB_SYSTEM_SETTINGS,
               R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_GET, R1_STATUS_ACK_OK,
               nullptr, 0);
}

R1Frame R1Encoder::buildLowPowerSet(R1ProtocolProfile profile,
                                    uint32_t epochSeconds,
                                    bool enabled) {
  if (profile != R1_PROFILE_FW_2_2_7_0005) return R1Frame{};
  uint8_t payload[12] = {};
  writeU32LE(payload, epochSeconds);
  payload[4] = 0;  // Captured switchType; no other value is supported.
  payload[5] = enabled ? 1 : 0;
  return build(R1_MODULE_SYSTEM, R1_CMD_SYSTEM, R1_SUB_SYSTEM_SETTINGS,
               R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_SET, R1_STATUS_ACK_OK,
               payload, sizeof(payload));
}

R1Frame R1Encoder::buildWearStatusQuery() {
  return build(R1_MODULE_SYSTEM, R1_CMD_SYSTEM, R1_SUB_WEAR_STATUS,
               R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_GET, R1_STATUS_ACK_OK,
               nullptr, 0);
}

R1Frame R1Encoder::buildHealthQuery(uint8_t cmd, uint8_t subCmd) {
  return build(R1_MODULE_HEALTH, cmd, subCmd,
               R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_GET, R1_STATUS_ACK_OK,
               nullptr, 0);
}

R1Frame R1Encoder::buildPacketAck(
    R1ProtocolProfile profile, const R1PacketAckDescriptor& received) {
  if (profile != R1_PROFILE_FW_2_2_7_0005 || !received.valid()) {
    return R1Frame{};
  }

  uint8_t payload[10] = {};
  payload[0] = received.module_;
  payload[1] = received.cmd_;
  payload[2] = received.subCmd_;
  // payload[3] and payload[6..9] are capture-proven zero. Their semantic
  // names are not known, so keep them reserved rather than inventing fields.
  writeU16LE(payload + 4, received.serial_);
  return build(R1_MODULE_SYSTEM, R1_CMD_SYSTEM, R1_SUB_PACKET_ACK,
               R1_STATUS_TYPE_ACK, R1_STATUS_METHOD_GET, R1_STATUS_ACK_OK,
               payload, sizeof(payload));
}

R1Frame R1Encoder::buildPacketAck(R1ProtocolProfile profile,
                                  const R1Decoded& received) {
  R1PacketAckDescriptor descriptor;
  if (!r1PacketAckDescriptorFromDecoded(profile, received, descriptor)) {
    return R1Frame{};
  }
  return buildPacketAck(profile, descriptor);
}

R1Frame R1Encoder::buildGenericQuery(uint8_t module, uint8_t cmd, uint8_t subCmd) {
  return build(module, cmd, subCmd,
               R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_GET, R1_STATUS_ACK_OK,
               nullptr, 0);
}

// =============================================================================
// Decoder
// =============================================================================

bool r1Decode(const uint8_t* data, size_t len, R1Decoded& out) {
  // 1 (transferType) + 4 (CRC32) + 12 (model header) = 17 bytes minimum.
  if (!data || len < 17) return false;

  out = R1Decoded{};
  out.transferType  = data[0];
  out.crc32Received = (uint32_t)data[1] |
                      ((uint32_t)data[2] << 8) |
                      ((uint32_t)data[3] << 16) |
                      ((uint32_t)data[4] << 24);

  const uint8_t* model = data + 5;
  const size_t modelLenWire = len - 5;

  out.version       = model[0];
  out.module        = model[1];
  out.moduleVersion = model[2];
  out.serial        = (uint16_t)model[3] | ((uint16_t)model[4] << 8);
  out.statusByte    = model[5];
  r1DecodeStatus(out.statusByte, out.statusType, out.statusMethod, out.statusAck);
  out.cmd           = model[6];
  out.subCmd        = model[7];
  out.modelLength   = (uint16_t)model[8] | ((uint16_t)model[9] << 8);
  out.crc16Received = (uint16_t)model[10] | ((uint16_t)model[11] << 8);

  // Use the wire-derived model length for CRC validation (the declared length
  // can be wrong on malformed frames; we still want to compute a useful
  // expected value over what we actually received).
  out.crc16Expected = r1ModelCrc16(model, modelLenWire);
  out.crc32Expected = r1Crc32(model, modelLenWire);

  const size_t payloadLenWire = (modelLenWire > 12) ? (modelLenWire - 12) : 0;
  out.modelLengthValid = (out.modelLength >= 12 &&
                          out.modelLength == modelLenWire &&
                          payloadLenWire <= R1_MAX_PAYLOAD);
  size_t payloadLen = payloadLenWire;
  if (payloadLen > R1_MAX_PAYLOAD) payloadLen = R1_MAX_PAYLOAD;
  out.payloadLength = payloadLen;
  if (payloadLen > 0) {
    memcpy(out.payload, model + 12, payloadLen);
  }

  out.crc16Valid = (out.crc16Received == out.crc16Expected);
  out.crc32Valid = (out.crc32Received == out.crc32Expected);
  return true;
}

R1ParseError r1ValidateDecoded(const R1Decoded& decoded) {
  if (!decoded.crc32Valid) return R1_PARSE_BAD_CRC;
  if (!decoded.modelLengthValid || decoded.modelLength < 12 ||
      decoded.payloadLength != (size_t)(decoded.modelLength - 12) ||
      decoded.payloadLength > R1_MAX_PAYLOAD) {
    return R1_PARSE_LENGTH;
  }
  if (decoded.transferType != 0 || decoded.version != 0x64 ||
      decoded.moduleVersion != 0x64) {
    return R1_PARSE_UNSUPPORTED_LAYOUT;
  }
  return R1_PARSE_OK;
}

bool r1DecodedIsTrusted(const R1Decoded& decoded) {
  return r1ValidateDecoded(decoded) == R1_PARSE_OK;
}

namespace {
constexpr uint8_t R1_PACKET_ACK_TRUST_MARKER = 0xA7;

bool r1PacketAckCmdAllowed(uint8_t cmd) {
  return cmd == R1_CMD_HEARTRATE || cmd == R1_CMD_HRV ||
         cmd == R1_CMD_SPO2 || cmd == R1_CMD_SLEEP ||
         cmd == R1_CMD_ACTIVITY;
}
}  // namespace

bool R1PacketAckDescriptor::valid() const {
  return trustMarker_ == R1_PACKET_ACK_TRUST_MARKER &&
         module_ == R1_MODULE_HEALTH && subCmd_ == R1_SUB_DAILY &&
         r1PacketAckCmdAllowed(cmd_);
}

bool r1PacketAckDescriptorFromDecoded(R1ProtocolProfile profile,
                                      const R1Decoded& decoded,
                                      R1PacketAckDescriptor& out) {
  out = R1PacketAckDescriptor{};
  if (profile != R1_PROFILE_FW_2_2_7_0005 ||
      !r1DecodedIsTrusted(decoded) ||
      decoded.module != R1_MODULE_HEALTH ||
      decoded.subCmd != R1_SUB_DAILY ||
      decoded.statusType != R1_STATUS_TYPE_NOTIFY ||
      decoded.statusMethod != R1_STATUS_METHOD_SET ||
      decoded.statusAck != R1_STATUS_ACK_OK ||
      !r1PacketAckCmdAllowed(decoded.cmd)) {
    return false;
  }

  out.serial_ = decoded.serial;
  out.module_ = decoded.module;
  out.cmd_ = decoded.cmd;
  out.subCmd_ = decoded.subCmd;
  out.trustMarker_ = R1_PACKET_ACK_TRUST_MARKER;
  return true;
}

static bool decodePaddedString16(const uint8_t* wire, char out[17]) {
  if (!wire || !out) return false;
  bool terminated = false;
  size_t outLen = 0;
  for (size_t i = 0; i < 16; ++i) {
    const uint8_t c = wire[i];
    if (c == 0) {
      terminated = true;
      continue;
    }
    // Exact NUL padding: printable bytes may not resume after padding begins.
    if (terminated || c < 0x20 || c > 0x7E) return false;
    out[outLen++] = (char)c;
  }
  out[outLen] = '\0';
  return outLen > 0;
}

R1ParseError r1ParseDeviceInfo(const R1Decoded& decoded, R1DeviceInfo& out) {
  out = R1DeviceInfo{};
  const R1ParseError integrity = r1ValidateDecoded(decoded);
  if (integrity != R1_PARSE_OK) return integrity;
  if (decoded.module != R1_MODULE_SYSTEM || decoded.cmd != R1_CMD_SYSTEM ||
      decoded.subCmd != R1_SUB_DEVICE_INFO ||
      decoded.statusType != R1_STATUS_TYPE_ACK ||
      decoded.statusMethod != R1_STATUS_METHOD_SET ||
      decoded.statusAck != R1_STATUS_ACK_OK) {
    return R1_PARSE_UNSUPPORTED_LAYOUT;
  }
  if (decoded.payloadLength != 32) return R1_PARSE_LENGTH;
  if (!decodePaddedString16(decoded.payload, out.firmware) ||
      !decodePaddedString16(decoded.payload + 16, out.hardware)) {
    out = R1DeviceInfo{};
    return R1_PARSE_UNSUPPORTED_LAYOUT;
  }
  out.profile = r1ProfileForFirmware(out.firmware);
  return R1_PARSE_OK;
}

// =============================================================================
// Name lookups
// =============================================================================

const char* r1ModuleName(uint8_t module) {
  switch (module) {
    case R1_MODULE_SYSTEM:   return "system";
    case R1_MODULE_HEALTH:   return "health";
    case R1_MODULE_SPORT:    return "sport";
    case R1_MODULE_TESTABLE: return "testable";
    default:                 return "?";
  }
}

const char* r1CmdName(uint8_t module, uint8_t cmd) {
  if (module == R1_MODULE_SYSTEM) {
    return (cmd == R1_CMD_SYSTEM) ? "system" : "?";
  }
  // health and sport modules share the same cmd namespace
  switch (cmd) {
    case R1_CMD_HEARTRATE:    return "heartRate";
    case R1_CMD_SPO2:         return "spo2";
    case R1_CMD_TEMPERATURE:  return "temperature";
    case R1_CMD_HRV:          return "hrv";
    case R1_CMD_ACTIVITY:     return "activity";
    case R1_CMD_SLEEP:        return "sleep";
    case R1_CMD_HEALTHSET:    return "healthSetting";
    default:                  return "?";
  }
}

const char* r1SubCmdName(uint8_t module, uint8_t cmd, uint8_t subCmd) {
  if (module == R1_MODULE_SYSTEM && cmd == R1_CMD_SYSTEM) {
    switch (subCmd) {
      case R1_SUB_DEVICE_STATUS:        return "deviceStatus";
      case R1_SUB_DEVICE_INFO:          return "deviceInfo";
      case R1_SUB_WEAR_STATUS:          return "wearStatus";
      case R1_SUB_USER_INFO:            return "userInfo";
      case R1_SUB_SYSTEM_TIME:          return "systemTime";
      case R1_SUB_TOUCH_STATUS:         return "touchStatus";
      case R1_SUB_TOUCH_SWITCH:         return "touchSwitch";
      case R1_SUB_PAIR_AUTH:            return "pairAuth";
      case R1_SUB_OTA_START:            return "otaStart";
      case R1_SUB_ADV_START:            return "advStart";
      case R1_SUB_GET_ALGO_KEY_STATUS:  return "getAlgoKeyStatus";
      case R1_SUB_SET_ALGO_KEY:         return "setAlgoKey";
      case R1_SUB_HEALTH_SETTINGS:      return "healthCollection";
      case R1_SUB_SYSTEM_SETTINGS:      return "systemSettingsStatus";
      case R1_SUB_DEVICE_SN:            return "deviceSn";
      case R1_SUB_NV_RECOVER:           return "nvRecover";
      case R1_SUB_POWER_CONTROL:        return "powerControl";
      case R1_SUB_PACKET_ACK:           return "packetAck";
      case R1_SUB_HEARTBEAT:            return "heartbeatPack";
      case R1_SUB_RGH_HEARTBEAT:        return "ringGlassesHeartbeatPack";
      case R1_SUB_RGH_SHAKE_HANDS:      return "ringGlassesShakeHands";
      case R1_SUB_REMOVE_RING_NOTIFY:   return "removeRingNotify";
      default:                          return "?";
    }
  }
  // health/sport telemetry subCmds share a small set
  switch (subCmd) {
    case R1_SUB_DAILY:    return "daily";
    case R1_SUB_POINT:    return "point";
    case R1_SUB_MEASURE:  return "measure";
    default:              return "?";
  }
}

const char* r1StatusTypeName(uint8_t type) {
  switch (type) {
    case R1_STATUS_TYPE_NOTIFY: return "notify";
    case R1_STATUS_TYPE_ACK:    return "ack";
    default:                    return "?";
  }
}

const char* r1StatusMethodName(uint8_t method) {
  switch (method) {
    case R1_STATUS_METHOD_GET: return "get";
    case R1_STATUS_METHOD_SET: return "set";
    default:                   return "?";
  }
}

const char* r1StatusAckName(uint8_t ack) {
  switch (ack) {
    case R1_STATUS_ACK_OK:         return "ok";
    case R1_STATUS_ACK_ERROR:      return "error";
    case R1_STATUS_ACK_REFUSE:     return "refuse";
    case R1_STATUS_ACK_NOTSUPPORT: return "notSupport";
    default:                       return "?";
  }
}

// =============================================================================
// Speculative payload annotator
// =============================================================================
//
// Tiny helpers shared across the parsers below.

static inline uint32_t readU32LE(const uint8_t* p) {
  return (uint32_t)p[0] |
         ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

static inline uint16_t readU16LE(const uint8_t* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline int16_t readI16LE(const uint8_t* p) {
  return (int16_t)readU16LE(p);
}

static bool bytesAreZero(const uint8_t* p, size_t len) {
  if (!p && len != 0) return false;
  for (size_t i = 0; i < len; ++i) {
    if (p[i] != 0) return false;
  }
  return true;
}

static R1ParseError validateKnownProfileAndFrame(R1ProtocolProfile profile,
                                                  const R1Decoded& decoded) {
  if (profile != R1_PROFILE_FW_2_2_7_0005) return R1_PARSE_WRONG_PROFILE;
  return r1ValidateDecoded(decoded);
}

static bool isAckSetOk(const R1Decoded& decoded) {
  return decoded.statusType == R1_STATUS_TYPE_ACK &&
         decoded.statusMethod == R1_STATUS_METHOD_SET &&
         decoded.statusAck == R1_STATUS_ACK_OK;
}

R1ParseError r1ParseLowPowerStatus(R1ProtocolProfile profile,
                                   const R1Decoded& decoded,
                                   R1LowPowerStatus& out) {
  out = R1LowPowerStatus{};
  const R1ParseError gate = validateKnownProfileAndFrame(profile, decoded);
  if (gate != R1_PARSE_OK) return gate;
  if (decoded.module != R1_MODULE_SYSTEM || decoded.cmd != R1_CMD_SYSTEM ||
      decoded.subCmd != R1_SUB_SYSTEM_SETTINGS || !isAckSetOk(decoded)) {
    return R1_PARSE_UNSUPPORTED_LAYOUT;
  }
  if (decoded.payloadLength != 12) return R1_PARSE_LENGTH;
  if (decoded.payload[4] != 0 || decoded.payload[5] > 1 ||
      !bytesAreZero(decoded.payload + 6, 6)) {
    return R1_PARSE_UNSUPPORTED_LAYOUT;
  }
  out.epochSeconds = readU32LE(decoded.payload);
  out.switchType = decoded.payload[4];
  out.enabled = decoded.payload[5] == 1;
  return R1_PARSE_OK;
}

R1ParseError r1ParseUserInfo(R1ProtocolProfile profile,
                             const R1Decoded& decoded,
                             R1UserInfo& out) {
  out = R1UserInfo{};
  const R1ParseError gate = validateKnownProfileAndFrame(profile, decoded);
  if (gate != R1_PARSE_OK) return gate;
  if (decoded.module != R1_MODULE_SYSTEM || decoded.cmd != R1_CMD_SYSTEM ||
      decoded.subCmd != R1_SUB_USER_INFO ||
      decoded.statusType != R1_STATUS_TYPE_NOTIFY ||
      decoded.statusMethod != R1_STATUS_METHOD_GET ||
      decoded.statusAck != R1_STATUS_ACK_OK) {
    return R1_PARSE_UNSUPPORTED_LAYOUT;
  }
  if (decoded.payloadLength != 12) return R1_PARSE_LENGTH;
  if (decoded.payload[0] > 2 || !bytesAreZero(decoded.payload + 6, 6)) {
    return R1_PARSE_UNSUPPORTED_LAYOUT;
  }
  out.gender = decoded.payload[0];
  out.age = decoded.payload[1];
  out.heightCm = readU16LE(decoded.payload + 2);
  out.weightKg = readU16LE(decoded.payload + 4);
  memcpy(out.reserved, decoded.payload + 6, sizeof(out.reserved));
  return R1_PARSE_OK;
}

// Format an epoch as ISO 8601 in UTC. Returns chars written. If the epoch is
// 0 (sentinel "no data"), writes "n/a" instead.
static size_t formatEpoch(uint32_t epochSec, char* out, size_t cap) {
  if (cap == 0) return 0;
  if (epochSec == 0) {
    return (size_t)snprintf(out, cap, "n/a");
  }
  time_t t = (time_t)epochSec;
  struct tm utc;
  gmtime_r(&t, &utc);
  return (size_t)strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

// system/system/{deviceStatus,heartbeatPack} — both share a 7-byte payload
// shape. Captures:
//   Session A: byte0=0x49 (73), 0x4B (75), 0x4D (77) across reboots
//   Session B: byte0=0x46 (70) — STAYED CONSTANT across 9 queries spanning
//              4 minutes within the session (verified 2026-05-02 12:22-12:26)
// byte[0] is **stable within a session** but drifts down across days
// (75 → 70). Best fit is **ring battery percent**: realistic 0..100 range,
// stable while connected (drains slowly), drops over time. Not 100%
// confirmed — could also be a session-id or other persisted counter.
// byte[1] is consistently 0x02 = WEAR per BleRing1SystemWearStatus enum.
// byte[2] is 0x01 — meaning unknown but stable, possibly version flag.
// bytes[3..6] are always zero in our captures.
static size_t annotateDeviceStatus(const uint8_t* p, size_t len, char* out, size_t cap) {
  if (len < 7) return 0;
  const char* wear =
      (p[1] == 0) ? "unknown" :
      (p[1] == 1) ? "notWear" :
      (p[1] == 2) ? "wear"    : "?";
  return (size_t)snprintf(out, cap,
      "deviceStatus byte0=%u(maybe-batt%%?) wear=%s flag=%u tail=%02X%02X%02X%02X",
      p[0], wear, p[2], p[3], p[4], p[5], p[6]);
}

// system/system/deviceSn — identifier material. Routine diagnostics expose
// only the payload length; raw bytes remain available only to explicit capture
// tooling with its own privacy controls.
static size_t annotateDeviceSn(const uint8_t* p, size_t len, char* out, size_t cap) {
  if (len < 1) return 0;
  (void)p;
  return (size_t)snprintf(out, cap, "deviceSn redacted bytes=%u",
                          (unsigned)len);
}

// system/system/getAlgoKeyStatus — byte[0] is a status flag (0=ok seen),
// bytes[1..] contain secret key material. Routine diagnostics expose only the
// status and key length.
static size_t annotateAlgoKey(const uint8_t* p, size_t len, char* out, size_t cap) {
  if (len < 1) return 0;
  return (size_t)snprintf(out, cap,
                          "algoKeyStatus=%u keyBytes=%u redacted",
                          p[0], (unsigned)(len - 1));
}

// system/system/wearStatus — 1 byte: 0=unknown 1=notWear 2=wear (per
// BleRing1SystemWearStatus in r1_ring_enums.proto).
static size_t annotateWearStatus(const uint8_t* p, size_t len, char* out, size_t cap) {
  if (len < 1) return 0;
  const char* state =
      (p[0] == 0) ? "unknown" :
      (p[0] == 1) ? "notWear" :
      (p[0] == 2) ? "wear"    : "?";
  return (size_t)snprintf(out, cap, "wearStatus=%u(%s)", p[0], state);
}

// system/system/nvRecover may contain identifier material. Keep routine
// diagnostics to a redacted payload length.
static size_t annotateNvRecover(const uint8_t* p, size_t len, char* out, size_t cap) {
  if (len < 1) return 0;
  (void)p;
  return (size_t)snprintf(out, cap, "nvRecover redacted bytes=%u",
                          (unsigned)len);
}

// health/{heartRate,hrv,spo2,temperature}/point
// Layout cross-referenced against
// docs/evenrealities_rev_share-main/tools/btsnoop_parser/ring1_packet_codec.py
// _parse_health_point():
//   [0..1] value      i16 LE — primary reading. For temperature this is the
//                              temp value (probably scaled ×10). For HR it
//                              has been observed as 0 — HR comes from
//                              extra_value instead. Likely opcode-dependent.
//   [2..5] timestamp  u32 LE epoch seconds
//   [6]    state_code 1 byte — meaning unknown (1 = "have a sample"?)
//   [7..]  extra_value — 1 byte if len==8, i16 LE if len>=9. For HR this is
//                        the actual BPM reading.
//
// For heartRate specifically we surface the extra_value as `hr=N bpm`. For
// other commands we report value + extra_value generically — caller can
// interpret based on cmd.
static size_t annotateHealthPoint(uint8_t cmd, const uint8_t* p, size_t len,
                                  char* out, size_t cap) {
  if (len < 7) return 0;
  int16_t value = (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
  uint32_t ts = readU32LE(p + 2);
  uint8_t state = p[6];
  char tsBuf[32];
  formatEpoch(ts, tsBuf, sizeof(tsBuf));

  // Decode extra_value: 1 byte if len==8, else i16 LE if len>=9.
  long extra = 0;
  bool hasExtra = false;
  if (len >= 9) {
    extra = (long)(int16_t)((uint16_t)p[7] | ((uint16_t)p[8] << 8));
    hasExtra = true;
  } else if (len >= 8) {
    extra = (long)p[7];
    hasExtra = true;
  }

  // For heartRate we know extra_value carries the BPM. Use a friendlier
  // label. For other commands surface both fields generically — until we
  // capture more we don't know which slot has the actual reading.
  if (cmd == R1_CMD_HEARTRATE && hasExtra) {
    return (size_t)snprintf(out, cap,
        "hrPoint ts=%s state=%u value=%d hr=%ld bpm",
        tsBuf, state, (int)value, extra);
  }
  if (hasExtra) {
    return (size_t)snprintf(out, cap,
        "healthPoint ts=%s state=%u value=%d extra=%ld",
        tsBuf, state, (int)value, extra);
  }
  return (size_t)snprintf(out, cap,
      "healthPoint ts=%s state=%u value=%d",
      tsBuf, state, (int)value);
}

// Firmware 2.2.7.0005 daily pages. These parsers intentionally accept a full
// decoded frame, not a naked payload: CRC32/model-length/status/profile gates
// cannot be accidentally bypassed by a telemetry caller.
static R1ParseError validateDailyFrame(R1ProtocolProfile profile,
                                       const R1Decoded& decoded,
                                       uint8_t expectedCmd) {
  const R1ParseError gate = validateKnownProfileAndFrame(profile, decoded);
  if (gate != R1_PARSE_OK) return gate;
  if (decoded.module != R1_MODULE_HEALTH || decoded.cmd != expectedCmd ||
      decoded.subCmd != R1_SUB_DAILY ||
      decoded.statusType != R1_STATUS_TYPE_NOTIFY ||
      decoded.statusMethod != R1_STATUS_METHOD_SET ||
      decoded.statusAck != R1_STATUS_ACK_OK) {
    return R1_PARSE_UNSUPPORTED_LAYOUT;
  }
  return R1_PARSE_OK;
}

static R1ParseError parseDailyPrefix(const uint8_t* payload, size_t payloadLength,
                                     uint8_t& count,
                                     int16_t& timezoneMinutes,
                                     uint32_t& dayStart) {
  if (!payload || payloadLength < 7) return R1_PARSE_LENGTH;
  count = payload[0];
  timezoneMinutes = readI16LE(payload + 1);
  // HardwareOne's configured fixed-offset range is UTC-12 through UTC+14.
  if (timezoneMinutes < -720 || timezoneMinutes > 840) {
    return R1_PARSE_VALUE_RANGE;
  }
  dayStart = readU32LE(payload + 3);
  return R1_PARSE_OK;
}

static constexpr uint32_t R1_DAILY_EPOCH_FLOOR = 1577836800UL;  // 2020-01-01
static constexpr uint32_t R1_DAILY_EPOCH_CEILING = 4102444800UL; // 2100-01-01

static bool dailyEpochPlausible(uint32_t value) {
  return value >= R1_DAILY_EPOCH_FLOOR && value < R1_DAILY_EPOCH_CEILING;
}

static R1DailyDayMode classifyDailyDay(uint32_t dayStart,
                                      int16_t timezoneMinutes) {
  if (dayStart == 0) return R1_DAILY_DAY_ZERO_BASE;
  if (!dailyEpochPlausible(dayStart)) return R1_DAILY_DAY_UNKNOWN;
  // The captured day key is UTC for tz=0 and local midnight expressed as UTC
  // for nonzero offsets. Do not promote a merely plausible random u32.
  const int64_t localStart = static_cast<int64_t>(dayStart) +
      static_cast<int64_t>(timezoneMinutes) * 60LL;
  if ((localStart % 86400LL) != 0) return R1_DAILY_DAY_UNKNOWN;
  return R1_DAILY_DAY_EPOCH;
}

const char* r1DailyDayModeName(R1DailyDayMode mode) {
  switch (mode) {
    case R1_DAILY_DAY_ZERO_BASE: return "zero-base";
    case R1_DAILY_DAY_EPOCH: return "epoch";
    case R1_DAILY_DAY_UNKNOWN: return "unknown";
    default: return "invalid";
  }
}

const char* r1DailyTimestampModeName(R1DailyTimestampMode mode) {
  switch (mode) {
    case R1_DAILY_TIMESTAMP_NONE: return "none";
    case R1_DAILY_TIMESTAMP_EPOCH: return "epoch";
    case R1_DAILY_TIMESTAMP_SECONDS_WITHIN_DAY: return "seconds-within-day";
    case R1_DAILY_TIMESTAMP_UNKNOWN: return "unknown";
    default: return "invalid";
  }
}

static R1ParseError classifyDailyTimestamp(uint32_t raw,
                                           R1DailyDayMode dayMode,
                                           uint32_t dayStart,
                                           R1DailyTimestampMode& mode,
                                           uint32_t& absoluteTimestamp) {
  absoluteTimestamp = 0;
  if (raw == 0) {
    mode = R1_DAILY_TIMESTAMP_NONE;
    return R1_PARSE_OK;
  }
  if (dailyEpochPlausible(raw)) {
    mode = R1_DAILY_TIMESTAMP_EPOCH;
    absoluteTimestamp = raw;
    return R1_PARSE_OK;
  }
  if (raw < 86400UL) {
    mode = R1_DAILY_TIMESTAMP_SECONDS_WITHIN_DAY;
    if (dayMode != R1_DAILY_DAY_EPOCH) return R1_PARSE_OK;
    const uint64_t normalized = static_cast<uint64_t>(dayStart) + raw;
    if (normalized >= R1_DAILY_EPOCH_CEILING || normalized > UINT32_MAX) {
      return R1_PARSE_VALUE_RANGE;
    }
    absoluteTimestamp = static_cast<uint32_t>(normalized);
    return R1_PARSE_OK;
  }
  mode = R1_DAILY_TIMESTAMP_UNKNOWN;
  return R1_PARSE_OK;
}

static R1ParseError checkedBucketEpoch(R1DailyDayMode dayMode,
                                       uint32_t dayStart, uint8_t slot,
                                       uint32_t secondsPerSlot,
                                       uint32_t& bucketEpoch) {
  if (dayMode != R1_DAILY_DAY_EPOCH) {
    bucketEpoch = 0;
    return R1_PARSE_OK;
  }
  const uint64_t value = (uint64_t)dayStart +
                         (uint64_t)slot * (uint64_t)secondsPerSlot;
  if (value > UINT32_MAX) return R1_PARSE_VALUE_RANGE;
  bucketEpoch = (uint32_t)value;
  return R1_PARSE_OK;
}

R1ParseError r1ParseCommonDaily(R1ProtocolProfile profile,
                                const R1Decoded& decoded,
                                R1CommonDailyResult& out) {
  out = R1CommonDailyResult{};
  const R1ParseError integrity = validateKnownProfileAndFrame(profile, decoded);
  if (integrity != R1_PARSE_OK) return integrity;
  if (decoded.cmd != R1_CMD_HEARTRATE && decoded.cmd != R1_CMD_SPO2) {
    return R1_PARSE_UNSUPPORTED_LAYOUT;
  }
  const R1ParseError gate = validateDailyFrame(profile, decoded, decoded.cmd);
  if (gate != R1_PARSE_OK) return gate;

  uint8_t count = 0;
  int16_t timezoneMinutes = 0;
  uint32_t dayStart = 0;
  R1ParseError error = parseDailyPrefix(decoded.payload, decoded.payloadLength,
                                        count, timezoneMinutes, dayStart);
  if (error != R1_PARSE_OK) return error;
  if (count > R1_COMMON_DAILY_MAX_RECORDS) return R1_PARSE_TOO_LARGE;
  const size_t expectedLength = 16U + (size_t)count * 4U;
  if (decoded.payloadLength != expectedLength) return R1_PARSE_LENGTH;

  const uint8_t* payload = decoded.payload;
  const uint32_t trailer = readU32LE(payload + expectedLength - 4);

  out.profile = profile;
  out.metric = (decoded.cmd == R1_CMD_HEARTRATE)
      ? R1_DAILY_METRIC_HEART_RATE : R1_DAILY_METRIC_SPO2;
  out.sourceSerial = decoded.serial;
  out.sourceCrc32 = decoded.crc32Received;
  out.timezoneMinutes = timezoneMinutes;
  out.dayMode = classifyDailyDay(dayStart, timezoneMinutes);
  out.dayStart = dayStart;
  out.latestTimestampRaw = readU32LE(payload + 7);
  error = classifyDailyTimestamp(out.latestTimestampRaw, out.dayMode,
                                 out.dayStart, out.latestTimestampMode,
                                 out.latestTimestamp);
  if (error != R1_PARSE_OK) {
    out = R1CommonDailyResult{};
    return error;
  }
  out.latestValue = payload[11];
  out.count = count;
  out.trailer = trailer;

  if (decoded.cmd == R1_CMD_SPO2 && out.latestValue > 100) {
    out = R1CommonDailyResult{};
    return R1_PARSE_VALUE_RANGE;
  }
  const uint8_t* record = payload + 12;
  uint32_t seenSlots = 0;
  for (uint8_t i = 0; i < count; ++i, record += 4) {
    R1CommonDailyRecord& dst = out.records[i];
    dst.hourSlot = record[0];
    dst.average = record[1];
    dst.maximum = record[2];
    dst.minimum = record[3];
    if (dst.hourSlot > 23) {
      out = R1CommonDailyResult{};
      return R1_PARSE_SLOT_RANGE;
    }
    const uint32_t slotBit = 1UL << dst.hourSlot;
    if ((seenSlots & slotBit) != 0) {
      out = R1CommonDailyResult{};
      return R1_PARSE_DUPLICATE_SLOT;
    }
    seenSlots |= slotBit;
    if (dst.minimum > dst.average || dst.average > dst.maximum ||
        (decoded.cmd == R1_CMD_SPO2 && dst.maximum > 100)) {
      out = R1CommonDailyResult{};
      return R1_PARSE_VALUE_RANGE;
    }
    error = checkedBucketEpoch(out.dayMode, dayStart, dst.hourSlot, 3600,
                               dst.bucketEpoch);
    if (error != R1_PARSE_OK) {
      out = R1CommonDailyResult{};
      return error;
    }
  }
  return R1_PARSE_OK;
}

R1ParseError r1ParseHrvDaily(R1ProtocolProfile profile,
                             const R1Decoded& decoded,
                             R1HrvDailyResult& out) {
  out = R1HrvDailyResult{};
  const R1ParseError gate = validateDailyFrame(profile, decoded, R1_CMD_HRV);
  if (gate != R1_PARSE_OK) return gate;

  uint8_t count = 0;
  int16_t timezoneMinutes = 0;
  uint32_t dayStart = 0;
  R1ParseError error = parseDailyPrefix(decoded.payload, decoded.payloadLength,
                                        count, timezoneMinutes, dayStart);
  if (error != R1_PARSE_OK) return error;
  if (count > R1_HRV_DAILY_MAX_RECORDS) return R1_PARSE_TOO_LARGE;
  const size_t expectedLength = 17U + (size_t)count * 7U;
  if (decoded.payloadLength != expectedLength) return R1_PARSE_LENGTH;

  const uint8_t* payload = decoded.payload;
  const uint32_t trailer = readU32LE(payload + expectedLength - 4);

  out.profile = profile;
  out.metric = R1_DAILY_METRIC_HRV;
  out.sourceSerial = decoded.serial;
  out.sourceCrc32 = decoded.crc32Received;
  out.timezoneMinutes = timezoneMinutes;
  out.dayMode = classifyDailyDay(dayStart, timezoneMinutes);
  out.dayStart = dayStart;
  out.latestTimestampRaw = readU32LE(payload + 7);
  error = classifyDailyTimestamp(out.latestTimestampRaw, out.dayMode,
                                 out.dayStart, out.latestTimestampMode,
                                 out.latestTimestamp);
  if (error != R1_PARSE_OK) {
    out = R1HrvDailyResult{};
    return error;
  }
  out.latestValue = readU16LE(payload + 11);
  out.count = count;
  out.trailer = trailer;

  const uint8_t* record = payload + 13;
  uint32_t seenSlots = 0;
  for (uint8_t i = 0; i < count; ++i, record += 7) {
    R1HrvDailyRecord& dst = out.records[i];
    dst.hourSlot = record[0];
    dst.average = readU16LE(record + 1);
    dst.maximum = readU16LE(record + 3);
    dst.minimum = readU16LE(record + 5);
    if (dst.hourSlot > 23) {
      out = R1HrvDailyResult{};
      return R1_PARSE_SLOT_RANGE;
    }
    const uint32_t slotBit = 1UL << dst.hourSlot;
    if ((seenSlots & slotBit) != 0) {
      out = R1HrvDailyResult{};
      return R1_PARSE_DUPLICATE_SLOT;
    }
    seenSlots |= slotBit;
    if (dst.minimum > dst.average || dst.average > dst.maximum) {
      out = R1HrvDailyResult{};
      return R1_PARSE_VALUE_RANGE;
    }
    error = checkedBucketEpoch(out.dayMode, dayStart, dst.hourSlot, 3600,
                               dst.bucketEpoch);
    if (error != R1_PARSE_OK) {
      out = R1HrvDailyResult{};
      return error;
    }
  }
  return R1_PARSE_OK;
}

// Shared activity-daily record-parsing core. Operates on a raw payload span so
// both the single-frame decoder (decoded.payload) and the multi-fragment
// reassembly path (a stitched buffer) run identical layout/range validation.
static R1ParseError parseActivityDailyPayload(R1ProtocolProfile profile,
                                              const uint8_t* payload,
                                              size_t payloadLength,
                                              uint16_t serial,
                                              uint32_t crc32Received,
                                              R1ActivityDailyResult& out) {
  out = R1ActivityDailyResult{};

  uint8_t count = 0;
  int16_t timezoneMinutes = 0;
  uint32_t dayStart = 0;
  R1ParseError error =
      parseDailyPrefix(payload, payloadLength, count, timezoneMinutes, dayStart);
  if (error != R1_PARSE_OK) return error;
  if ((size_t)count > (size_t)R1_ACTIVITY_DAILY_MAX_RECORDS) {
    return R1_PARSE_TOO_LARGE;
  }
  const size_t expectedLength = 11U + (size_t)count * 7U;
  if (payloadLength != expectedLength) return R1_PARSE_LENGTH;

  const uint32_t trailer = readU32LE(payload + expectedLength - 4);

  out.profile = profile;
  out.metric = R1_DAILY_METRIC_ACTIVITY;
  out.sourceSerial = serial;
  out.sourceCrc32 = crc32Received;
  out.timezoneMinutes = timezoneMinutes;
  out.dayMode = classifyDailyDay(dayStart, timezoneMinutes);
  out.dayStart = dayStart;
  out.count = count;
  out.trailer = trailer;

  const uint8_t* record = payload + 7;
  uint8_t seenSlots[18] = {};
  for (uint8_t i = 0; i < count; ++i, record += 7) {
    R1ActivityDailyRecord& dst = out.records[i];
    dst.tenMinuteSlot = record[0];
    dst.steps = readU16LE(record + 1);
    dst.activeKcal = readU16LE(record + 3);
    dst.totalKcal = readU16LE(record + 5);
    if (dst.tenMinuteSlot > 143) {
      out = R1ActivityDailyResult{};
      return R1_PARSE_SLOT_RANGE;
    }
    const uint8_t seenByte = (uint8_t)(dst.tenMinuteSlot >> 3);
    const uint8_t seenBit = (uint8_t)(1U << (dst.tenMinuteSlot & 0x07));
    if ((seenSlots[seenByte] & seenBit) != 0) {
      out = R1ActivityDailyResult{};
      return R1_PARSE_DUPLICATE_SLOT;
    }
    seenSlots[seenByte] |= seenBit;
    if (dst.totalKcal < dst.activeKcal) {
      out = R1ActivityDailyResult{};
      return R1_PARSE_VALUE_RANGE;
    }
    dst.restingKcal = (uint16_t)(dst.totalKcal - dst.activeKcal);
    error = checkedBucketEpoch(out.dayMode, dayStart, dst.tenMinuteSlot, 600,
                               dst.bucketEpoch);
    if (error != R1_PARSE_OK) {
      out = R1ActivityDailyResult{};
      return error;
    }
  }
  return R1_PARSE_OK;
}

R1ParseError r1ParseActivityDaily(R1ProtocolProfile profile,
                                  const R1Decoded& decoded,
                                  R1ActivityDailyResult& out) {
  out = R1ActivityDailyResult{};
  const R1ParseError gate = validateDailyFrame(profile, decoded, R1_CMD_ACTIVITY);
  if (gate != R1_PARSE_OK) return gate;
  return parseActivityDailyPayload(profile, decoded.payload,
                                   decoded.payloadLength, decoded.serial,
                                   decoded.crc32Received, out);
}

R1ParseError r1ParseReassembledActivityDaily(R1ProtocolProfile profile,
                                             const uint8_t* model,
                                             size_t modelLen,
                                             uint32_t crc32Whole,
                                             R1ActivityDailyResult& out) {
  out = R1ActivityDailyResult{};
  if (profile != R1_PROFILE_FW_2_2_7_0005) return R1_PARSE_WRONG_PROFILE;
  // 12-byte model header + at least the 7-byte daily prefix start.
  if (!model || modelLen < 12) return R1_PARSE_LENGTH;

  const uint8_t version = model[0];
  const uint8_t moduleId = model[1];
  const uint8_t moduleVersion = model[2];
  const uint16_t serial = (uint16_t)model[3] | ((uint16_t)model[4] << 8);
  uint8_t statusType = 0, statusMethod = 0, statusAck = 0;
  r1DecodeStatus(model[5], statusType, statusMethod, statusAck);
  const uint8_t cmd = model[6];
  const uint8_t subCmd = model[7];
  const size_t declaredModelLen = (size_t)model[8] | ((size_t)model[9] << 8);

  if (version != 0x64 || moduleVersion != 0x64) return R1_PARSE_UNSUPPORTED_LAYOUT;
  // The stitched buffer must contain at least the declared model. Some
  // transports pad the final fragment, so trailing bytes past declaredModelLen
  // are allowed (and excluded from the CRC/parse below).
  if (declaredModelLen < 12 || declaredModelLen > modelLen) return R1_PARSE_LENGTH;
  if (moduleId != R1_MODULE_HEALTH || cmd != R1_CMD_ACTIVITY ||
      subCmd != R1_SUB_DAILY || statusType != R1_STATUS_TYPE_NOTIFY ||
      statusMethod != R1_STATUS_METHOD_SET || statusAck != R1_STATUS_ACK_OK) {
    return R1_PARSE_UNSUPPORTED_LAYOUT;
  }
  // The fragments carried a whole-model CRC32; recompute over exactly the
  // declared model and reject any reassembly that does not reproduce it.
  if (r1Crc32(model, declaredModelLen) != crc32Whole) return R1_PARSE_BAD_CRC;

  return parseActivityDailyPayload(profile, model + 12, declaredModelLen - 12,
                                   serial, crc32Whole, out);
}

bool r1MakeDailyPacketAckDescriptor(R1ProtocolProfile profile, uint16_t serial,
                                    uint8_t cmd, R1PacketAckDescriptor& out) {
  out = R1PacketAckDescriptor{};
  if (profile != R1_PROFILE_FW_2_2_7_0005 || !r1PacketAckCmdAllowed(cmd)) {
    return false;
  }
  out.serial_ = serial;
  out.module_ = R1_MODULE_HEALTH;
  out.cmd_ = cmd;
  out.subCmd_ = R1_SUB_DAILY;
  out.trustMarker_ = R1_PACKET_ACK_TRUST_MARKER;
  return true;
}

static size_t annotateCommonDaily(R1ProtocolProfile profile,
                                  const R1Decoded& decoded,
                                  char* out, size_t cap) {
  R1CommonDailyResult parsed;
  const R1ParseError error = r1ParseCommonDaily(profile, decoded, parsed);
  if (error != R1_PARSE_OK) {
    return (size_t)snprintf(out, cap, "daily rejected=%s",
                            r1ParseErrorName(error));
  }
  const char* tag = parsed.metric == R1_DAILY_METRIC_HEART_RATE
      ? "hrDaily" : "spo2Daily";
  return (size_t)snprintf(
      out, cap,
      "%s count=%u tz=%d day={raw=%lu,mode=%s} latest={raw=%lu,mode=%s,epoch=%lu,value=%u} trailer=%08lX",
      tag, parsed.count, (int)parsed.timezoneMinutes,
      (unsigned long)parsed.dayStart,
      r1DailyDayModeName(parsed.dayMode),
      (unsigned long)parsed.latestTimestampRaw,
      r1DailyTimestampModeName(parsed.latestTimestampMode),
      (unsigned long)parsed.latestTimestamp, parsed.latestValue,
      (unsigned long)parsed.trailer);
}

static size_t annotateHrvDaily(R1ProtocolProfile profile,
                               const R1Decoded& decoded,
                               char* out, size_t cap) {
  R1HrvDailyResult parsed;
  const R1ParseError error = r1ParseHrvDaily(profile, decoded, parsed);
  if (error != R1_PARSE_OK) {
    return (size_t)snprintf(out, cap, "daily rejected=%s",
                            r1ParseErrorName(error));
  }
  return (size_t)snprintf(
      out, cap,
      "hrvDaily count=%u tz=%d day={raw=%lu,mode=%s} latest={raw=%lu,mode=%s,epoch=%lu,value=%u} trailer=%08lX",
      parsed.count, (int)parsed.timezoneMinutes,
      (unsigned long)parsed.dayStart,
      r1DailyDayModeName(parsed.dayMode),
      (unsigned long)parsed.latestTimestampRaw,
      r1DailyTimestampModeName(parsed.latestTimestampMode),
      (unsigned long)parsed.latestTimestamp, (unsigned)parsed.latestValue,
      (unsigned long)parsed.trailer);
}

static size_t annotateActivityDaily(R1ProtocolProfile profile,
                                    const R1Decoded& decoded,
                                    char* out, size_t cap) {
  // R1ActivityDailyResult is now sized for a full 144-slot day (~2.3 KB); this
  // diagnostic path runs serialized on the ring owner task, so a single static
  // instance keeps it off that task's modest stack.
  static R1ActivityDailyResult parsed;
  const R1ParseError error = r1ParseActivityDaily(profile, decoded, parsed);
  if (error != R1_PARSE_OK) {
    return (size_t)snprintf(out, cap, "daily rejected=%s",
                            r1ParseErrorName(error));
  }
  uint32_t steps = 0;
  uint32_t active = 0;
  uint32_t total = 0;
  for (uint8_t i = 0; i < parsed.count; ++i) {
    steps += parsed.records[i].steps;
    active += parsed.records[i].activeKcal;
    total += parsed.records[i].totalKcal;
  }
  return (size_t)snprintf(
      out, cap,
      "activityDaily count=%u tz=%d day={raw=%lu,mode=%s} steps=%lu activeKcal=%lu totalKcal=%lu trailer=%08lX",
      parsed.count, (int)parsed.timezoneMinutes,
      (unsigned long)parsed.dayStart, r1DailyDayModeName(parsed.dayMode),
      (unsigned long)steps,
      (unsigned long)active, (unsigned long)total,
      (unsigned long)parsed.trailer);
}

size_t r1AnnotatePayload(R1ProtocolProfile profile, const R1Decoded& d,
                         char* out, size_t cap) {
  if (cap == 0 || d.payloadLength == 0) return 0;
  const R1ParseError integrity = r1ValidateDecoded(d);
  if (integrity != R1_PARSE_OK) {
    return (size_t)snprintf(out, cap, "payload rejected=%s",
                            r1ParseErrorName(integrity));
  }
  // Dispatch by (module, cmd, subCmd). Health module uses the same daily/
  // point/measure subCmds across heartRate/spo2/temperature/hrv/sleep, but
  // only the heartRate parsers are hypothesised — the others would just
  // return 0 and fall through to raw hex.
  if (d.module == R1_MODULE_SYSTEM && d.cmd == R1_CMD_SYSTEM) {
    switch (d.subCmd) {
      case R1_SUB_DEVICE_STATUS:
      case R1_SUB_HEARTBEAT:
        return annotateDeviceStatus(d.payload, d.payloadLength, out, cap);
      case R1_SUB_DEVICE_INFO: {
        R1DeviceInfo parsed;
        const R1ParseError error = r1ParseDeviceInfo(d, parsed);
        if (error != R1_PARSE_OK) {
          return (size_t)snprintf(out, cap, "deviceInfo rejected=%s",
                                  r1ParseErrorName(error));
        }
        return (size_t)snprintf(out, cap,
                                "deviceInfo fw='%s' hw='%s' profile=%s",
                                parsed.firmware, parsed.hardware,
                                r1ProtocolProfileName(parsed.profile));
      }
      case R1_SUB_DEVICE_SN:
        return annotateDeviceSn(d.payload, d.payloadLength, out, cap);
      case R1_SUB_GET_ALGO_KEY_STATUS:
        return annotateAlgoKey(d.payload, d.payloadLength, out, cap);
      case R1_SUB_SET_ALGO_KEY:
        return (size_t)snprintf(out, cap,
                                "setAlgoKey redacted bytes=%u",
                                (unsigned)d.payloadLength);
      case R1_SUB_WEAR_STATUS:
        return annotateWearStatus(d.payload, d.payloadLength, out, cap);
      case R1_SUB_SYSTEM_SETTINGS: {
        R1LowPowerStatus parsed;
        const R1ParseError error = r1ParseLowPowerStatus(profile, d, parsed);
        if (error != R1_PARSE_OK) {
          return (size_t)snprintf(out, cap, "lowPower rejected=%s",
                                  r1ParseErrorName(error));
        }
        return (size_t)snprintf(out, cap,
                                "lowPower=%s switchType=%u epoch=%lu",
                                parsed.enabled ? "on" : "off",
                                parsed.switchType,
                                (unsigned long)parsed.epochSeconds);
      }
      case R1_SUB_USER_INFO: {
        R1UserInfo parsed;
        const R1ParseError error = r1ParseUserInfo(profile, d, parsed);
        return (size_t)snprintf(out, cap, "userInfo=%s (redacted)",
                                r1ParseErrorName(error));
      }
      case R1_SUB_NV_RECOVER:
        return annotateNvRecover(d.payload, d.payloadLength, out, cap);
      default:
        return 0;
    }
  }
  if (d.module == R1_MODULE_HEALTH) {
    // The point opcode shape is the same for heartRate / hrv / spo2 /
    // temperature; the parser branches on cmd internally to choose a
    // friendly label.
    if (d.subCmd == R1_SUB_POINT &&
        (d.cmd == R1_CMD_HEARTRATE || d.cmd == R1_CMD_HRV ||
         d.cmd == R1_CMD_SPO2      || d.cmd == R1_CMD_TEMPERATURE)) {
      return annotateHealthPoint(d.cmd, d.payload, d.payloadLength, out, cap);
    }
    if (d.subCmd == R1_SUB_DAILY &&
        (d.cmd == R1_CMD_HEARTRATE || d.cmd == R1_CMD_SPO2)) {
      return annotateCommonDaily(profile, d, out, cap);
    }
    if (d.subCmd == R1_SUB_DAILY && d.cmd == R1_CMD_HRV) {
      return annotateHrvDaily(profile, d, out, cap);
    }
    if (d.cmd == R1_CMD_ACTIVITY && d.subCmd == R1_SUB_DAILY) {
      return annotateActivityDaily(profile, d, out, cap);
    }
  }
  return 0;
}

// =============================================================================
// Sanitized boot self-test
// =============================================================================
// The wire vectors below preserve official-app schemas but contain only
// synthetic locally-administered MACs, synthetic epochs, and synthetic health
// values. No capture file, real device identifier, or user profile is embedded.

static bool selfTestCompare(const char* label,
                            const uint8_t* actual, size_t actualLen,
                            const uint8_t* expected, size_t expectedLen) {
  if (actualLen != expectedLen) {
    DEBUG_RING_SETUPF("[R1-selftest] FAIL %s: length=%u (expected %u)",
              label, (unsigned)actualLen, (unsigned)expectedLen);
    return false;
  }
  for (size_t i = 0; i < actualLen; i++) {
    if (actual[i] != expected[i]) {
      DEBUG_RING_SETUPF("[R1-selftest] FAIL %s: byte[%u]=%02X (expected %02X)",
                label, (unsigned)i, actual[i], expected[i]);
      return false;
    }
  }
  DEBUG_RING_SETUPF("[R1-selftest] PASS %s (%u B)", label, (unsigned)actualLen);
  return true;
}

static bool selfTestExpect(const char* label, bool condition) {
  if (!condition) {
    DEBUG_RING_SETUPF("[R1-selftest] FAIL %s", label);
    return false;
  }
  DEBUG_RING_SETUPF("[R1-selftest] PASS %s", label);
  return true;
}

// Keep the temporary encoded frame out of the caller's boot-task stack frame.
static bool __attribute__((noinline)) selfTestDecoded(
    uint16_t serial, uint8_t module, uint8_t cmd, uint8_t subCmd,
    uint8_t statusType, uint8_t statusMethod,
    const uint8_t* payload, size_t payloadLength,
    R1Decoded& decoded, R1Frame* wire = nullptr) {
  R1Encoder encoder;
  for (uint16_t i = 1; i < serial; ++i) (void)encoder.nextSerial();
  R1Frame frame = encoder.build(module, cmd, subCmd,
                                statusType, statusMethod, R1_STATUS_ACK_OK,
                                payload, payloadLength);
  if (wire) *wire = frame;
  return frame.length != 0 && r1Decode(frame.bytes, frame.length, decoded);
}

bool r1ProtocolSelfTest() {
  bool ok = true;
  // R1ActivityDailyResult is now full-day sized (~2.3 KB). This one-shot boot
  // self-test runs single-threaded, so its activity sub-tests share one static
  // instance (aliased below) rather than putting ~2.3 KB on the init stack.
  static R1ActivityDailyResult activityScratch;

  {
    // Existing public fixture: it independently anchors both CRC algorithms.
    R1Encoder legacyEncoder;
    static const uint8_t expectedAuth[] = {
      0x00, 0x97, 0x19, 0x53, 0xF9,
      0x64, 0x01, 0x64, 0x01, 0x00, 0x00, 0x00, 0x08, 0x0D, 0x00, 0x3F, 0x01,
      0x01,
    };
    R1Frame rejectedNullPayload = legacyEncoder.build(
        R1_MODULE_SYSTEM, R1_CMD_SYSTEM, R1_SUB_PAIR_AUTH,
        R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_GET, R1_STATUS_ACK_OK,
        nullptr, 1);
    R1Frame auth = legacyEncoder.buildPairAuth();
    ok &= selfTestExpect("null payload fails without serial",
                         rejectedNullPayload.length == 0 && auth.serial == 1);
    ok &= selfTestCompare("pairAuth golden", auth.bytes, auth.length,
                          expectedAuth, sizeof(expectedAuth));
    (void)legacyEncoder.nextSerial();
    static const uint8_t expectedTime[] = {
      0x00, 0xD4, 0xC9, 0xBA, 0x70,
      0x64, 0x01, 0x64, 0x03, 0x00, 0x02, 0x00, 0x05, 0x12, 0x00, 0x80, 0x77,
      0x10, 0xFF, 0x33, 0x48, 0xBB, 0x69,
    };
    R1Frame rejectedTimezone = legacyEncoder.buildSyncTime(
        static_cast<int16_t>(-721), static_cast<uint32_t>(1773881395));
    R1Frame timeFrame = legacyEncoder.buildSyncTime((int16_t)-240,
                                                     (uint32_t)1773881395);
    ok &= selfTestExpect("syncTime timezone range fails without serial",
                         rejectedTimezone.length == 0 && timeFrame.serial == 3);
    ok &= selfTestCompare("syncTime golden", timeFrame.bytes, timeFrame.length,
                          expectedTime, sizeof(expectedTime));
  }

  {
    // Synthetic locally-administered temple addresses. The golden payload
    // proves right/left order and independent reversal without a real MAC.
    static const uint8_t syntheticRight[6] = {0x02, 0, 0, 0, 0, 0xA1};
    static const uint8_t syntheticLeft[6]  = {0x02, 0, 0, 0, 0, 0xB2};
    static const uint8_t expectedAdvStart[] = {
      0x00,0xC5,0xDB,0xF8,0xC7, 0x64,0x01,0x64,0x01,0x00,0x00,0x00,
      0x0A,0x18,0x00,0xC5,0xDA,
      0xA1,0,0,0,0,0x02, 0xB2,0,0,0,0,0x02,
    };
    R1Encoder advEncoder;
    R1Frame rejectedAdv = advEncoder.buildAdvStart(
        R1_PROFILE_UNKNOWN, syntheticRight, syntheticLeft);
    R1Frame adv = advEncoder.buildAdvStart(
        R1_PROFILE_FW_2_2_7_0005, syntheticRight, syntheticLeft);
    ok &= selfTestExpect("advStart unknown fails without serial",
                         rejectedAdv.length == 0 && adv.serial == 1);
    ok &= selfTestCompare("advStart dual reversed golden",
                          adv.bytes, adv.length,
                          expectedAdvStart, sizeof(expectedAdvStart));
  }

  {
    static const uint8_t expectedHealthOn[] = {
      0x00,0xCC,0xBE,0x21,0x6F, 0x64,0x01,0x64,0x01,0x00,0x02,0x00,
      0x0E,0x18,0x00,0xC5,0x5C, 0x01,0x00,0x00,0x65,0x01,0,0,0,0,0,0,0,
    };
    static const uint8_t expectedHealthOff[] = {
      0x00,0x2F,0x94,0xD3,0xBC, 0x64,0x01,0x64,0x01,0x00,0x02,0x00,
      0x0E,0x18,0x00,0x16,0x1B, 0x01,0x00,0x00,0x65,0x00,0,0,0,0,0,0,0,
    };
    R1Encoder healthOnEncoder;
    R1Encoder healthOffEncoder;
    R1Frame healthOn = healthOnEncoder.buildHealthCollectionSet(
        R1_PROFILE_FW_2_2_7_0005, 0x65000001UL, true);
    R1Frame healthOff = healthOffEncoder.buildHealthCollectionSet(
        R1_PROFILE_FW_2_2_7_0005, 0x65000001UL, false);
    ok &= selfTestCompare("health collection on golden", healthOn.bytes,
                          healthOn.length, expectedHealthOn,
                          sizeof(expectedHealthOn));
    ok &= selfTestCompare("health collection off golden", healthOff.bytes,
                          healthOff.length, expectedHealthOff,
                          sizeof(expectedHealthOff));
  }

  {
    static const uint8_t expectedLowPowerOn[] = {
      0x00,0x46,0xB2,0xE0,0x22, 0x64,0x01,0x64,0x01,0x00,0x02,0x00,
      0x0F,0x18,0x00,0x8B,0x0D, 0x01,0x00,0x00,0x65,0x00,0x01,0,0,0,0,0,0,
    };
    static const uint8_t expectedLowPowerOff[] = {
      0x00,0xC1,0xCD,0x6B,0x2F, 0x64,0x01,0x64,0x01,0x00,0x02,0x00,
      0x0F,0x18,0x00,0xEA,0xB5, 0x01,0x00,0x00,0x65,0x00,0x00,0,0,0,0,0,0,
    };
    R1Encoder lowOnEncoder;
    R1Encoder lowOffEncoder;
    R1Frame lowOn = lowOnEncoder.buildLowPowerSet(
        R1_PROFILE_FW_2_2_7_0005, 0x65000001UL, true);
    R1Frame lowOff = lowOffEncoder.buildLowPowerSet(
        R1_PROFILE_FW_2_2_7_0005, 0x65000001UL, false);
    ok &= selfTestCompare("low power on golden", lowOn.bytes, lowOn.length,
                          expectedLowPowerOn, sizeof(expectedLowPowerOn));
    ok &= selfTestCompare("low power off golden", lowOff.bytes,
                          lowOff.length, expectedLowPowerOff,
                          sizeof(expectedLowPowerOff));
  }

  {
    // Exact identity/profile selection, with synthetic hardware text.
    uint8_t deviceInfoPayload[32] = {};
    // sizeof includes the NUL (11 bytes); the rest of the 16-byte field is
    // already zero from the {} init. A fixed 12 read one byte past the
    // literal (ASan-caught, 2026-08-11) — benign only while the next rodata
    // byte happened to be 0x00; a nonzero byte there would have failed
    // decodePaddedString16 and silently disabled the ring module at init.
    memcpy(deviceInfoPayload, "2.2.7.0005", sizeof("2.2.7.0005"));
    memcpy(deviceInfoPayload + 16, "SYNTH-HW", 8);
    R1Decoded deviceInfoDecoded;
    R1DeviceInfo deviceInfo;
    ok &= selfTestExpect(
        "deviceInfo fixture decode",
        selfTestDecoded(1, R1_MODULE_SYSTEM, R1_CMD_SYSTEM,
                        R1_SUB_DEVICE_INFO, R1_STATUS_TYPE_ACK,
                        R1_STATUS_METHOD_SET, deviceInfoPayload,
                        sizeof(deviceInfoPayload), deviceInfoDecoded) &&
        r1ParseDeviceInfo(deviceInfoDecoded, deviceInfo) == R1_PARSE_OK &&
        strcmp(deviceInfo.firmware, "2.2.7.0005") == 0 &&
        strcmp(deviceInfo.hardware, "SYNTH-HW") == 0 &&
        deviceInfo.profile == R1_PROFILE_FW_2_2_7_0005 &&
        r1ProfileForFirmware("2.2.7.0006") == R1_PROFILE_UNKNOWN);
  }

  {
    // Routine annotations must never echo identifier or key material.
    static const uint8_t deviceSnPayload[] = "SYNTH-ID";
    static const uint8_t algoKeyPayload[] = {0, 'A', 'B', 'C', 'D'};
    static const uint8_t nvRecoverPayload[] = "PRIVATE-ID";
    R1Decoded decoded{};
    char annotation[96] = {};
    bool redacted = selfTestDecoded(
        1, R1_MODULE_SYSTEM, R1_CMD_SYSTEM, R1_SUB_DEVICE_SN,
        R1_STATUS_TYPE_ACK, R1_STATUS_METHOD_SET,
        deviceSnPayload, sizeof(deviceSnPayload) - 1, decoded);
    if (redacted) {
      redacted = r1AnnotatePayload(R1_PROFILE_FW_2_2_7_0005, decoded,
                                   annotation, sizeof(annotation)) > 0 &&
                 strstr(annotation, "SYNTH-ID") == nullptr;
    }
    if (redacted) {
      redacted = selfTestDecoded(
          1, R1_MODULE_SYSTEM, R1_CMD_SYSTEM,
          R1_SUB_GET_ALGO_KEY_STATUS, R1_STATUS_TYPE_ACK,
          R1_STATUS_METHOD_SET, algoKeyPayload, sizeof(algoKeyPayload),
          decoded);
    }
    if (redacted) {
      redacted = r1AnnotatePayload(R1_PROFILE_FW_2_2_7_0005, decoded,
                                   annotation, sizeof(annotation)) > 0 &&
                 strstr(annotation, "ABCD") == nullptr;
    }
    if (redacted) {
      redacted = selfTestDecoded(
          1, R1_MODULE_SYSTEM, R1_CMD_SYSTEM, R1_SUB_NV_RECOVER,
          R1_STATUS_TYPE_ACK, R1_STATUS_METHOD_SET,
          nvRecoverPayload, sizeof(nvRecoverPayload) - 1, decoded);
    }
    if (redacted) {
      redacted = r1AnnotatePayload(R1_PROFILE_FW_2_2_7_0005, decoded,
                                   annotation, sizeof(annotation)) > 0 &&
                 strstr(annotation, "PRIVATE-ID") == nullptr;
    }
    ok &= selfTestExpect("sensitive diagnostics redacted", redacted);
  }

  // Low-power response and user-info decode-only fixtures. Health collection
  // has no capture-proven readback payload, so only its SET vectors above are
  // tested.
  static const uint8_t lowStatePayload[12] = {0,0,0,0, 0,1,0,0,0,0,0,0};
  static const uint8_t userInfoPayload[12] = {
    2,0, 0x34,0x12, 0x78,0x56, 0,0,0,0,0,0,
  };
  {
    R1Decoded lowStateDecoded;
    R1Decoded userInfoDecoded;
    R1LowPowerStatus lowState;
    R1UserInfo userInfo;
    ok &= selfTestExpect(
        "typed low-power/user-info decoders",
        selfTestDecoded(1, R1_MODULE_SYSTEM, R1_CMD_SYSTEM,
                        R1_SUB_SYSTEM_SETTINGS, R1_STATUS_TYPE_ACK,
                        R1_STATUS_METHOD_SET, lowStatePayload,
                        sizeof(lowStatePayload), lowStateDecoded) &&
        selfTestDecoded(1, R1_MODULE_SYSTEM, R1_CMD_SYSTEM,
                        R1_SUB_USER_INFO, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_GET, userInfoPayload,
                        sizeof(userInfoPayload), userInfoDecoded) &&
        r1ParseLowPowerStatus(R1_PROFILE_FW_2_2_7_0005,
                              lowStateDecoded, lowState) == R1_PARSE_OK &&
        lowState.switchType == 0 && lowState.enabled &&
        r1ParseUserInfo(R1_PROFILE_FW_2_2_7_0005,
                        userInfoDecoded, userInfo) == R1_PARSE_OK &&
        userInfo.gender == 2 && userInfo.age == 0 &&
        userInfo.heightCm == 0x1234 && userInfo.weightKg == 0x5678);
  }

  // Sanitized daily pages: same field widths/order as the official app, with
  // synthetic epochs and values. The final u32 is ring-owned opaque metadata,
  // not a sentinel: captures have shown it changing between sessions.
  static const uint8_t hrPayload[] = {
    2, 0xC4,0xFF, 0x10,0xC7,0x55,0x69,
    0x34,0x12,0x00,0x00, 70,
    0,60,70,50, 23,80,90,70, 0xD4,0x9A,0,0,
  };
  static const uint8_t spo2Payload[] = {
    1, 0x4A,0x01, 0xA8,0x6B,0x55,0x69,
    0x68,0x14,0x56,0x69, 98,
    12,97,99,95, 0x1F,0x28,0,0,
  };
  static const uint8_t hrvPayload[] = {
    1, 0xC4,0xFF, 0x10,0xC7,0x55,0x69,
    0x34,0x12,0x00,0x00, 0x2C,0x01,
    5, 0xFA,0x00, 0x90,0x01, 0xC8,0x00, 0xD4,0x9A,0,0,
  };
  static const uint8_t activityPayload[] = {
    2, 0xC4,0xFF, 0x10,0xC7,0x55,0x69,
    0,   100,0, 5,0, 10,0,
    143, 200,0, 20,0, 30,0,
    0x1F,0x28,0,0,
  };
  {
    R1Decoded decoded;
    R1CommonDailyResult parsed;
    ok &= selfTestExpect(
        "typed HR daily",
        selfTestDecoded(45, R1_MODULE_HEALTH, R1_CMD_HEARTRATE,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, hrPayload, sizeof(hrPayload),
                        decoded) &&
        r1ParseCommonDaily(R1_PROFILE_FW_2_2_7_0005,
                           decoded, parsed) == R1_PARSE_OK &&
        parsed.count == 2 && parsed.latestValue == 70 &&
        parsed.dayMode == R1_DAILY_DAY_EPOCH &&
        parsed.latestTimestampMode == R1_DAILY_TIMESTAMP_SECONDS_WITHIN_DAY &&
        parsed.latestTimestampRaw == 0x1234UL &&
        parsed.latestTimestamp == 0x6955D944UL &&
        parsed.records[0].bucketEpoch == 0x6955C710UL &&
        parsed.records[1].bucketEpoch == 0x69570A80UL &&
        parsed.records[1].average == 80 &&
        parsed.records[1].maximum == 90 &&
        parsed.records[1].minimum == 70 &&
        parsed.trailer == 0x00009AD4UL);
  }
  {
    R1Decoded decoded;
    R1CommonDailyResult parsed;
    ok &= selfTestExpect(
        "typed SpO2 daily",
        selfTestDecoded(46, R1_MODULE_HEALTH, R1_CMD_SPO2,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, spo2Payload, sizeof(spo2Payload),
                        decoded) &&
        r1ParseCommonDaily(R1_PROFILE_FW_2_2_7_0005,
                           decoded, parsed) == R1_PARSE_OK &&
        parsed.timezoneMinutes == 330 && parsed.latestValue == 98 &&
        parsed.dayMode == R1_DAILY_DAY_EPOCH &&
        parsed.latestTimestampMode == R1_DAILY_TIMESTAMP_EPOCH &&
        parsed.latestTimestampRaw == 0x69561468UL &&
        parsed.latestTimestamp == parsed.latestTimestampRaw &&
        parsed.records[0].hourSlot == 12 &&
        parsed.records[0].average == 97 &&
        parsed.trailer == 0x0000281FUL);
  }
  {
    R1Decoded decoded;
    R1HrvDailyResult parsed;
    ok &= selfTestExpect(
        "typed HRV daily",
        selfTestDecoded(47, R1_MODULE_HEALTH, R1_CMD_HRV,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, hrvPayload, sizeof(hrvPayload),
                        decoded) &&
        r1ParseHrvDaily(R1_PROFILE_FW_2_2_7_0005,
                        decoded, parsed) == R1_PARSE_OK &&
        parsed.latestTimestampMode == R1_DAILY_TIMESTAMP_SECONDS_WITHIN_DAY &&
        parsed.latestTimestampRaw == 0x1234UL &&
        parsed.latestTimestamp == 0x6955D944UL && parsed.latestValue == 300 &&
        parsed.records[0].average == 250 &&
        parsed.records[0].maximum == 400 && parsed.records[0].minimum == 200);
  }
  {
    R1Decoded decoded;
    R1ActivityDailyResult& parsed = activityScratch;
    ok &= selfTestExpect(
        "typed activity daily",
        selfTestDecoded(48, R1_MODULE_HEALTH, R1_CMD_ACTIVITY,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, activityPayload,
                        sizeof(activityPayload), decoded) &&
        r1ParseActivityDaily(R1_PROFILE_FW_2_2_7_0005,
                             decoded, parsed) == R1_PARSE_OK &&
        parsed.count == 2 && parsed.records[0].steps == 100 &&
        parsed.records[0].activeKcal == 5 &&
        parsed.records[0].restingKcal == 5 &&
        parsed.records[1].tenMinuteSlot == 143 &&
        parsed.dayMode == R1_DAILY_DAY_EPOCH &&
        parsed.records[1].bucketEpoch == 0x69571638UL);
  }

  // Sanitized capture regressions for the mixed timestamp modes observed in
  // one 2.2.7.0005 fetch. Zero-base pages remain structurally valid and retain
  // their slots/raw latest value, but no 1970 bucket or invented epoch escapes.
  static const uint8_t zeroBaseSecondsPayload[] = {
    2, 0,0, 0,0,0,0, 0x66,0x99,0,0, 70,
    7,60,70,50, 10,65,75,55, 0xD4,0x9A,0,0,
  };
  static const uint8_t anchoredSecondsPayload[] = {
    1, 0,0, 0x00,0xB9,0x55,0x69, 0x66,0x99,0,0, 70,
    10,65,75,55, 0xD4,0x9A,0,0,
  };
  static const uint8_t zeroBaseEpochPayload[] = {
    1, 0,0, 0,0,0,0, 0xC0,0x61,0x56,0x69, 98,
    12,97,99,95, 0xD4,0x9A,0,0,
  };
  static const uint8_t zeroBaseActivityPayload[] = {
    1, 0,0, 0,0,0,0,
    48, 3,0, 4,0, 9,0, 0xD4,0x9A,0,0,
  };
  static const uint8_t unknownDayPayload[] = {
    1, 0,0, 0x01,0xB9,0x55,0x69, 0x66,0x99,0,0, 70,
    10,65,75,55, 0xD4,0x9A,0,0,
  };
  static const uint8_t unknownLatestPayload[] = {
    1, 0,0, 0x00,0xB9,0x55,0x69, 0x90,0x5F,0x01,0x00, 70,
    10,65,75,55, 0xD4,0x9A,0,0,
  };
  {
    R1Decoded decoded;
    R1CommonDailyResult parsed;
    ok &= selfTestExpect(
        "daily zero-base seconds preserved unanchored",
        selfTestDecoded(49, R1_MODULE_HEALTH, R1_CMD_HEARTRATE,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, zeroBaseSecondsPayload,
                        sizeof(zeroBaseSecondsPayload), decoded) &&
        r1ParseCommonDaily(R1_PROFILE_FW_2_2_7_0005,
                           decoded, parsed) == R1_PARSE_OK &&
        parsed.dayMode == R1_DAILY_DAY_ZERO_BASE &&
        parsed.latestTimestampMode == R1_DAILY_TIMESTAMP_SECONDS_WITHIN_DAY &&
        parsed.latestTimestampRaw == 39270UL &&
        parsed.latestTimestamp == 0 && parsed.records[0].bucketEpoch == 0 &&
        parsed.records[1].bucketEpoch == 0);
  }
  {
    R1Decoded decoded;
    R1CommonDailyResult parsed;
    ok &= selfTestExpect(
        "daily anchored seconds normalized",
        selfTestDecoded(50, R1_MODULE_HEALTH, R1_CMD_HEARTRATE,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, anchoredSecondsPayload,
                        sizeof(anchoredSecondsPayload), decoded) &&
        r1ParseCommonDaily(R1_PROFILE_FW_2_2_7_0005,
                           decoded, parsed) == R1_PARSE_OK &&
        parsed.dayMode == R1_DAILY_DAY_EPOCH &&
        parsed.latestTimestampMode == R1_DAILY_TIMESTAMP_SECONDS_WITHIN_DAY &&
        parsed.latestTimestampRaw == 39270UL &&
        parsed.latestTimestamp == 0x69565266UL &&
        parsed.records[0].bucketEpoch == 0x695645A0UL);
  }
  {
    R1Decoded decoded;
    R1CommonDailyResult parsed;
    ok &= selfTestExpect(
        "daily zero-base absolute latest preserved",
        selfTestDecoded(51, R1_MODULE_HEALTH, R1_CMD_SPO2,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, zeroBaseEpochPayload,
                        sizeof(zeroBaseEpochPayload), decoded) &&
        r1ParseCommonDaily(R1_PROFILE_FW_2_2_7_0005,
                           decoded, parsed) == R1_PARSE_OK &&
        parsed.dayMode == R1_DAILY_DAY_ZERO_BASE &&
        parsed.latestTimestampMode == R1_DAILY_TIMESTAMP_EPOCH &&
        parsed.latestTimestampRaw == 0x695661C0UL &&
        parsed.latestTimestamp == parsed.latestTimestampRaw &&
        parsed.records[0].bucketEpoch == 0);
  }
  {
    R1Decoded decoded;
    R1ActivityDailyResult& parsed = activityScratch;
    ok &= selfTestExpect(
        "daily zero-base activity remains unanchored",
        selfTestDecoded(52, R1_MODULE_HEALTH, R1_CMD_ACTIVITY,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, zeroBaseActivityPayload,
                        sizeof(zeroBaseActivityPayload), decoded) &&
        r1ParseActivityDaily(R1_PROFILE_FW_2_2_7_0005,
                             decoded, parsed) == R1_PARSE_OK &&
        parsed.dayMode == R1_DAILY_DAY_ZERO_BASE &&
        parsed.records[0].tenMinuteSlot == 48 &&
        parsed.records[0].bucketEpoch == 0);
  }
  {
    R1Decoded decoded;
    R1CommonDailyResult parsed;
    ok &= selfTestExpect(
        "daily non-boundary day remains unknown",
        selfTestDecoded(53, R1_MODULE_HEALTH, R1_CMD_HEARTRATE,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, unknownDayPayload,
                        sizeof(unknownDayPayload), decoded) &&
        r1ParseCommonDaily(R1_PROFILE_FW_2_2_7_0005,
                           decoded, parsed) == R1_PARSE_OK &&
        parsed.dayMode == R1_DAILY_DAY_UNKNOWN &&
        parsed.latestTimestampMode == R1_DAILY_TIMESTAMP_SECONDS_WITHIN_DAY &&
        parsed.latestTimestamp == 0 && parsed.records[0].bucketEpoch == 0);
  }
  {
    R1Decoded decoded;
    R1CommonDailyResult parsed;
    ok &= selfTestExpect(
        "daily unknown latest remains raw only",
        selfTestDecoded(54, R1_MODULE_HEALTH, R1_CMD_HEARTRATE,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, unknownLatestPayload,
                        sizeof(unknownLatestPayload), decoded) &&
        r1ParseCommonDaily(R1_PROFILE_FW_2_2_7_0005,
                           decoded, parsed) == R1_PARSE_OK &&
        parsed.dayMode == R1_DAILY_DAY_EPOCH &&
        parsed.latestTimestampMode == R1_DAILY_TIMESTAMP_UNKNOWN &&
        parsed.latestTimestampRaw == 90000UL &&
        parsed.latestTimestamp == 0);
  }

  // packetAck uses a fresh outbound serial and echoes only the trusted received
  // daily frame identifiers/serial. Reserved fields stay zero.
  static const uint8_t expectedPacketAck[] = {
    0x00,0xF1,0x18,0xDB,0xCB, 0x64,0x01,0x64,0x01,0x00,0x01,0x00,
    0x7E,0x16,0x00,0xB5,0x7B, 0x02,0x01,0x01,0x00,0x2D,0x00,0,0,0,0,
  };
  {
    R1Decoded source{};
    const bool sourceOk = selfTestDecoded(
        45, R1_MODULE_HEALTH, R1_CMD_HEARTRATE, R1_SUB_DAILY,
        R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_SET,
        hrPayload, sizeof(hrPayload), source);
    R1Encoder packetAckEncoder;
    R1PacketAckDescriptor descriptor;
    const bool descriptorOk = r1PacketAckDescriptorFromDecoded(
        R1_PROFILE_FW_2_2_7_0005, source, descriptor);

    auto expectDescriptorReject = [&](const char* name,
                                      R1ProtocolProfile profile,
                                      const R1Decoded& candidate) {
      R1PacketAckDescriptor rejected = descriptor;
      const bool accepted = r1PacketAckDescriptorFromDecoded(
          profile, candidate, rejected);
      ok &= selfTestExpect(name, !accepted && !rejected.valid());
    };
    R1Decoded rejectedSource = source;
    expectDescriptorReject("packetAck unknown profile rejected",
                           R1_PROFILE_UNKNOWN, rejectedSource);
    rejectedSource = source;
    rejectedSource.crc32Valid = false;
    expectDescriptorReject("packetAck bad integrity rejected",
                           R1_PROFILE_FW_2_2_7_0005, rejectedSource);
    rejectedSource = source;
    rejectedSource.payloadLength = rejectedSource.payloadLength + 1;
    expectDescriptorReject("packetAck bad length rejected",
                           R1_PROFILE_FW_2_2_7_0005, rejectedSource);
    rejectedSource = source;
    rejectedSource.module = R1_MODULE_SPORT;
    expectDescriptorReject("packetAck wrong module rejected",
                           R1_PROFILE_FW_2_2_7_0005, rejectedSource);
    rejectedSource = source;
    rejectedSource.subCmd = R1_SUB_POINT;
    expectDescriptorReject("packetAck wrong subcommand rejected",
                           R1_PROFILE_FW_2_2_7_0005, rejectedSource);
    rejectedSource = source;
    rejectedSource.statusType = R1_STATUS_TYPE_ACK;
    expectDescriptorReject("packetAck wrong status type rejected",
                           R1_PROFILE_FW_2_2_7_0005, rejectedSource);
    rejectedSource = source;
    rejectedSource.statusMethod = R1_STATUS_METHOD_GET;
    expectDescriptorReject("packetAck wrong status method rejected",
                           R1_PROFILE_FW_2_2_7_0005, rejectedSource);
    rejectedSource = source;
    rejectedSource.statusAck = R1_STATUS_ACK_ERROR;
    expectDescriptorReject("packetAck error status rejected",
                           R1_PROFILE_FW_2_2_7_0005, rejectedSource);
    rejectedSource = source;
    rejectedSource.cmd = R1_CMD_TEMPERATURE;
    expectDescriptorReject("packetAck non-whitelist opcode rejected",
                           R1_PROFILE_FW_2_2_7_0005, rejectedSource);

    R1PacketAckDescriptor invalidDescriptor;
    R1Frame rejectedDefault = packetAckEncoder.buildPacketAck(
        R1_PROFILE_FW_2_2_7_0005, invalidDescriptor);
    R1Frame rejectedProfile = packetAckEncoder.buildPacketAck(
        R1_PROFILE_UNKNOWN, descriptor);
    rejectedSource = source;
    rejectedSource.crc32Valid = false;
    R1Frame rejectedIntegrity = packetAckEncoder.buildPacketAck(
        R1_PROFILE_FW_2_2_7_0005, rejectedSource);
    rejectedSource = source;
    rejectedSource.statusMethod = R1_STATUS_METHOD_GET;
    R1Frame rejectedStatus = packetAckEncoder.buildPacketAck(
        R1_PROFILE_FW_2_2_7_0005, rejectedSource);
    rejectedSource = source;
    rejectedSource.cmd = R1_CMD_TEMPERATURE;
    R1Frame rejectedOpcode = packetAckEncoder.buildPacketAck(
        R1_PROFILE_FW_2_2_7_0005, rejectedSource);
    R1Frame packetAck = packetAckEncoder.buildPacketAck(
        R1_PROFILE_FW_2_2_7_0005, descriptor);
    ok &= selfTestExpect("packetAck rejects without consuming serial",
                         sourceOk && descriptorOk && descriptor.valid() &&
                         rejectedDefault.length == 0 &&
                         rejectedProfile.length == 0 &&
                         rejectedIntegrity.length == 0 &&
                         rejectedStatus.length == 0 &&
                         rejectedOpcode.length == 0 &&
                         packetAck.serial == 1);
    ok &= selfTestCompare("packetAck golden", packetAck.bytes,
                          packetAck.length, expectedPacketAck,
                          sizeof(expectedPacketAck));
    R1Encoder decodedPacketAckEncoder;
    const R1Frame decodedPacketAck = decodedPacketAckEncoder.buildPacketAck(
        R1_PROFILE_FW_2_2_7_0005, source);
    ok &= selfTestCompare("packetAck decoded wrapper", decodedPacketAck.bytes,
                          decodedPacketAck.length, expectedPacketAck,
                          sizeof(expectedPacketAck));
  }

  // Integrity and malformed-layout cases. Each output object starts clean and
  // a rejected frame must never masquerade as a partial success.
  {
    R1Decoded source{};
    R1Frame wire{};
    const bool fixtureOk = selfTestDecoded(
        45, R1_MODULE_HEALTH, R1_CMD_HEARTRATE, R1_SUB_DAILY,
        R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_SET,
        hrPayload, sizeof(hrPayload), source, &wire);
    if (fixtureOk && wire.length > 17) wire.bytes[17] ^= 0x01;
    R1Decoded damaged{};
    R1CommonDailyResult rejected;
    ok &= selfTestExpect(
        "daily bad CRC rejected",
        fixtureOk && r1Decode(wire.bytes, wire.length, damaged) &&
        r1ParseCommonDaily(R1_PROFILE_FW_2_2_7_0005,
                           damaged, rejected) == R1_PARSE_BAD_CRC &&
        rejected.count == 0);
  }

  {
    R1Decoded source{};
    R1CommonDailyResult rejected;
    const bool fixtureOk = selfTestDecoded(
        45, R1_MODULE_HEALTH, R1_CMD_HEARTRATE, R1_SUB_DAILY,
        R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_SET,
        hrPayload, sizeof(hrPayload), source);
    R1Decoded wrongLength = source;
    wrongLength.modelLengthValid = false;
    ok &= selfTestExpect(
        "daily model length rejected",
        fixtureOk &&
        r1ParseCommonDaily(R1_PROFILE_FW_2_2_7_0005,
                           wrongLength, rejected) == R1_PARSE_LENGTH);
    ok &= selfTestExpect(
        "daily unknown profile rejected",
        fixtureOk &&
        r1ParseCommonDaily(R1_PROFILE_UNKNOWN,
                           source, rejected) == R1_PARSE_WRONG_PROFILE);
    R1Decoded corruptWrongOpcode = source;
    corruptWrongOpcode.cmd = R1_CMD_ACTIVITY;
    corruptWrongOpcode.crc32Valid = false;
    ok &= selfTestExpect(
        "daily integrity precedes opcode",
        fixtureOk &&
        r1ParseCommonDaily(R1_PROFILE_FW_2_2_7_0005,
                           corruptWrongOpcode, rejected) == R1_PARSE_BAD_CRC);
  }

  static const uint8_t zeroRecordPayload[] = {
    0, 0,0, 0x00,0x00,0x00,0x65,
    0,0,0,0, 0, 0xD4,0x9A,0,0,
  };
  {
    R1Decoded decoded;
    R1CommonDailyResult parsed;
    ok &= selfTestExpect(
        "daily zero records",
        selfTestDecoded(1, R1_MODULE_HEALTH, R1_CMD_HEARTRATE,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, zeroRecordPayload,
                        sizeof(zeroRecordPayload), decoded) &&
        r1ParseCommonDaily(R1_PROFILE_FW_2_2_7_0005,
                           decoded, parsed) == R1_PARSE_OK &&
        parsed.count == 0);
  }

  {
    uint8_t malformed[sizeof(hrPayload)];
    memcpy(malformed, hrPayload, sizeof(malformed));
    malformed[0] = R1_COMMON_DAILY_MAX_RECORDS + 1;
    R1Decoded decoded;
    R1CommonDailyResult rejected;
    ok &= selfTestExpect(
        "daily count overflow",
        selfTestDecoded(1, R1_MODULE_HEALTH, R1_CMD_HEARTRATE,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, malformed, sizeof(malformed),
                        decoded) &&
        r1ParseCommonDaily(R1_PROFILE_FW_2_2_7_0005,
                           decoded, rejected) == R1_PARSE_TOO_LARGE);
  }

  {
    uint8_t malformed[sizeof(hrPayload)];
    memcpy(malformed, hrPayload, sizeof(malformed));
    malformed[16] = malformed[12];  // Duplicate second hourly slot.
    R1Decoded decoded;
    R1CommonDailyResult rejected;
    ok &= selfTestExpect(
        "daily duplicate hour rejected",
        selfTestDecoded(1, R1_MODULE_HEALTH, R1_CMD_HEARTRATE,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, malformed, sizeof(malformed),
                        decoded) &&
        r1ParseCommonDaily(R1_PROFILE_FW_2_2_7_0005,
                           decoded, rejected) == R1_PARSE_DUPLICATE_SLOT);
  }

  {
    uint8_t malformed[sizeof(hrPayload)];
    memcpy(malformed, hrPayload, sizeof(malformed));
    malformed[sizeof(malformed) - 4] = 0x78;
    malformed[sizeof(malformed) - 3] = 0x56;
    malformed[sizeof(malformed) - 2] = 0x34;
    malformed[sizeof(malformed) - 1] = 0x12;
    R1Decoded decoded;
    R1CommonDailyResult parsed;
    ok &= selfTestExpect(
        "daily opaque trailer preserved",
        selfTestDecoded(1, R1_MODULE_HEALTH, R1_CMD_HEARTRATE,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, malformed, sizeof(malformed),
                        decoded) &&
        r1ParseCommonDaily(R1_PROFILE_FW_2_2_7_0005,
                           decoded, parsed) == R1_PARSE_OK &&
        parsed.trailer == 0x12345678UL);
  }

  {
    uint8_t malformed[sizeof(hrPayload)];
    memcpy(malformed, hrPayload, sizeof(malformed));
    malformed[1] = 0x2F;  // -721 minutes.
    malformed[2] = 0xFD;
    R1Decoded decoded;
    R1CommonDailyResult rejected;
    ok &= selfTestExpect(
        "daily timezone range",
        selfTestDecoded(1, R1_MODULE_HEALTH, R1_CMD_HEARTRATE,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, malformed, sizeof(malformed),
                        decoded) &&
        r1ParseCommonDaily(R1_PROFILE_FW_2_2_7_0005,
                           decoded, rejected) == R1_PARSE_VALUE_RANGE);
  }

  {
    uint8_t malformed[sizeof(activityPayload)];
    memcpy(malformed, activityPayload, sizeof(malformed));
    malformed[7] = 144;
    R1Decoded decoded;
    R1ActivityDailyResult& rejected = activityScratch;
    ok &= selfTestExpect(
        "activity slot range",
        selfTestDecoded(1, R1_MODULE_HEALTH, R1_CMD_ACTIVITY,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, malformed, sizeof(malformed),
                        decoded) &&
        r1ParseActivityDaily(R1_PROFILE_FW_2_2_7_0005,
                             decoded, rejected) == R1_PARSE_SLOT_RANGE);
  }

  {
    uint8_t malformed[sizeof(activityPayload)];
    memcpy(malformed, activityPayload, sizeof(malformed));
    malformed[14] = malformed[7];  // Duplicate second ten-minute slot.
    R1Decoded decoded;
    R1ActivityDailyResult& rejected = activityScratch;
    ok &= selfTestExpect(
        "activity duplicate slot rejected",
        selfTestDecoded(1, R1_MODULE_HEALTH, R1_CMD_ACTIVITY,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, malformed, sizeof(malformed),
                        decoded) &&
        r1ParseActivityDaily(R1_PROFILE_FW_2_2_7_0005,
                             decoded, rejected) == R1_PARSE_DUPLICATE_SLOT);
  }

  {
    uint8_t malformed[sizeof(activityPayload)];
    memcpy(malformed, activityPayload, sizeof(malformed));
    malformed[10] = 11;  // active kcal 11, total kcal 10.
    R1Decoded decoded;
    R1ActivityDailyResult& rejected = activityScratch;
    ok &= selfTestExpect(
        "activity kcal underflow",
        selfTestDecoded(1, R1_MODULE_HEALTH, R1_CMD_ACTIVITY,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_SET, malformed, sizeof(malformed),
                        decoded) &&
        r1ParseActivityDaily(R1_PROFILE_FW_2_2_7_0005,
                             decoded, rejected) == R1_PARSE_VALUE_RANGE);
  }

  {
    R1Decoded decoded;
    R1CommonDailyResult rejected;
    ok &= selfTestExpect(
        "daily unknown status",
        selfTestDecoded(1, R1_MODULE_HEALTH, R1_CMD_HEARTRATE,
                        R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                        R1_STATUS_METHOD_GET, hrPayload, sizeof(hrPayload),
                        decoded) &&
        r1ParseCommonDaily(R1_PROFILE_FW_2_2_7_0005,
                           decoded, rejected) == R1_PARSE_UNSUPPORTED_LAYOUT);
  }

  return ok;
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
