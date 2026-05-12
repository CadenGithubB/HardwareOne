#include "System_R1_Protocol.h"

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

#include "System_Debug.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

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

static uint32_t* sCrc32Table = nullptr;

static void r1Crc32EnsureTable() {
  if (sCrc32Table) return;
  static uint32_t table[256];
  const uint32_t poly = 0x1EDC6F41u;
  for (int v = 0; v < 256; v++) {
    uint32_t crc = (uint32_t)v << 24;
    for (int i = 0; i < 8; i++) {
      if (crc & 0x80000000u) {
        crc = (crc << 1) ^ poly;
      } else {
        crc <<= 1;
      }
    }
    table[v] = crc;
  }
  sCrc32Table = table;
}

uint32_t r1Crc32(const uint8_t* data, size_t len) {
  r1Crc32EnsureTable();
  uint32_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    uint8_t idx = (uint8_t)(data[i] ^ ((crc >> 24) & 0xFF));
    crc = (crc << 8) ^ sCrc32Table[idx];
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
  if (payloadLen > R1_MAX_PAYLOAD) {
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

R1Frame R1Encoder::buildAdvStart(const uint8_t* mac6BleOrder) {
  uint8_t payload[6] = {0};
  if (mac6BleOrder) {
    memcpy(payload, mac6BleOrder, 6);
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

R1Frame R1Encoder::buildHealthSettingsQuery() {
  return build(R1_MODULE_SYSTEM, R1_CMD_SYSTEM, R1_SUB_HEALTH_SETTINGS,
               R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_GET, R1_STATUS_ACK_OK,
               nullptr, 0);
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

R1Frame R1Encoder::buildHealthReportEnable(uint8_t enableMask) {
  // module=health (0x02), cmd=healthSetting (0x07), subCmd=reportEnable (0x01).
  //
  // Originally tried with module=system; the ring went silent (no ack, no
  // refusal — request just discarded). The Python codec
  // (docs/evenrealities_rev_share-main/.../ring1_packet_codec.py) puts
  // healthSetting as a CMD value alongside heartRate/spo2 etc., implying
  // it lives under module=health. Switched 2026-05-02.
  //
  // The subCmd byte 0x01 means "reportEnable" only inside the healthSetting
  // cmd namespace — the same byte means "daily" inside heartRate/spo2/etc.
  uint8_t payload[1] = { enableMask };
  return build(R1_MODULE_HEALTH, R1_CMD_HEALTHSET, /*reportEnable*/0x01,
               R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_SET, R1_STATUS_ACK_OK,
               payload, sizeof(payload));
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
  out.modelLengthValid = (out.modelLength == modelLenWire);
  out.crc16Received = (uint16_t)model[10] | ((uint16_t)model[11] << 8);

  // Use the wire-derived model length for CRC validation (the declared length
  // can be wrong on malformed frames; we still want to compute a useful
  // expected value over what we actually received).
  out.crc16Expected = r1ModelCrc16(model, modelLenWire);
  out.crc32Expected = r1Crc32(model, modelLenWire);

  size_t payloadLen = (modelLenWire > 12) ? (modelLenWire - 12) : 0;
  if (payloadLen > R1_MAX_PAYLOAD) payloadLen = R1_MAX_PAYLOAD;
  out.payloadLength = payloadLen;
  if (payloadLen > 0) {
    memcpy(out.payload, model + 12, payloadLen);
  }

  out.crc16Valid = (out.crc16Received == out.crc16Expected);
  out.crc32Valid = (out.crc32Received == out.crc32Expected);
  return true;
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
      case R1_SUB_HEALTH_SETTINGS:      return "healthSettingsStatus";
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

// system/system/deviceInfo — 32 B payload: two 16-byte ASCII strings,
// null-padded. First is firmware version (e.g. "2.2.0.0011"), second is
// hardware version (e.g. "603MV1.9.3"). Verified live 2026-05-02 12:26:30.
static size_t annotateDeviceInfo(const uint8_t* p, size_t len, char* out, size_t cap) {
  if (len < 32) return 0;
  // Copy each 16-byte half into a NUL-terminated buffer for safe printing.
  char fw[17], hw[17];
  memcpy(fw, p, 16); fw[16] = '\0';
  memcpy(hw, p + 16, 16); hw[16] = '\0';
  // Trim trailing nulls/garbage by null-terminating at the first non-printable.
  for (int i = 0; i < 16; i++) { if (fw[i] < 0x20 || fw[i] > 0x7E) { fw[i] = '\0'; break; } }
  for (int i = 0; i < 16; i++) { if (hw[i] < 0x20 || hw[i] > 0x7E) { hw[i] = '\0'; break; } }
  return (size_t)snprintf(out, cap, "deviceInfo fw='%s' hw='%s'", fw, hw);
}

// system/system/deviceSn — variable-length ASCII serial number, no length
// prefix. The python codec reads up to 30 bytes; ours observed 15 chars
// "B210DHACA200092". Verified 2026-05-02 12:26:55.
static size_t annotateDeviceSn(const uint8_t* p, size_t len, char* out, size_t cap) {
  if (len < 1) return 0;
  char sn[33];
  size_t copy = len < 32 ? len : 32;
  memcpy(sn, p, copy); sn[copy] = '\0';
  // Stop at first non-printable.
  for (size_t i = 0; i < copy; i++) { if (sn[i] < 0x20 || sn[i] > 0x7E) { sn[i] = '\0'; break; } }
  return (size_t)snprintf(out, cap, "deviceSn='%s'", sn);
}

// system/system/getAlgoKeyStatus — byte[0] is a status flag (0=ok seen),
// bytes[1..] are ASCII hex characters representing the device's algo key.
// On our ring the key starts with the last 4 MAC bytes in hex (cabaac1c…),
// followed by what appears to be a per-device unique tail. Verified
// 2026-05-02 12:26:44.
static size_t annotateAlgoKey(const uint8_t* p, size_t len, char* out, size_t cap) {
  if (len < 2) return 0;
  uint8_t status = p[0];
  char key[40];
  size_t copy = (len - 1) < 39 ? (len - 1) : 39;
  memcpy(key, p + 1, copy); key[copy] = '\0';
  for (size_t i = 0; i < copy; i++) { if (key[i] < 0x20 || key[i] > 0x7E) { key[i] = '\0'; break; } }
  return (size_t)snprintf(out, cap, "algoKeyStatus=%u key='%s'", status, key);
}

// system/system/{healthSettingsStatus,systemSettingsStatus} share a 12-byte
// shape but encode different bitmaps. healthSettingsStatus has byte[4]=0x01
// on our ring; systemSettingsStatus has byte[5]=0x01.
// Reused annotator: report which non-zero bytes are set so we can compare.
// (annotateHealthSettings already does this and works for both.)

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

// system/system/healthSettingsStatus — 12-byte feature bitmap. We don't know
// the bit-meanings yet; just print which byte offsets are non-zero so we can
// correlate with `ringquery report` experiments.
static size_t annotateHealthSettings(const uint8_t* p, size_t len, char* out, size_t cap) {
  if (len < 12) return 0;
  size_t off = 0;
  off += snprintf(out + off, cap - off, "settings={");
  bool first = true;
  for (size_t i = 0; i < len && off + 12 < cap; i++) {
    if (p[i] != 0) {
      off += snprintf(out + off, cap - off, "%s[%u]=%02X",
                      first ? "" : ",", (unsigned)i, p[i]);
      first = false;
    }
  }
  if (first) off += snprintf(out + off, cap - off, "all-zero");
  off += snprintf(out + off, cap - off, "}");
  return off;
}

// system/system/nvRecover — payload contains an ASCII serial number embedded
// in binary metadata. From the empirical capture
// `02 74 00 5D 5A 59 44 35 43 5A 31 39 37 34 00 00 ...` the chars at offset 4
// onward (`ZYD5CZ1974` after the leading 0x5D delimiter) look like a serial
// number. We extract the longest printable run from offset 4 as a best-effort
// hint.
static size_t annotateNvRecover(const uint8_t* p, size_t len, char* out, size_t cap) {
  if (len < 16) return 0;
  // Scan from offset 4 up to 32 bytes for printable ASCII run.
  char serial[32];
  size_t si = 0;
  size_t startScan = 4;
  // Skip a leading non-printable byte if present (the 0x5D in our capture).
  if (startScan < len && (p[startScan] < 0x20 || p[startScan] > 0x7E)) startScan++;
  for (size_t i = startScan; i < len && si + 1 < sizeof(serial); i++) {
    uint8_t c = p[i];
    if (c >= 0x20 && c <= 0x7E) {
      serial[si++] = (char)c;
    } else {
      if (si > 0) break;  // first run found, stop
    }
  }
  serial[si] = '\0';
  if (si == 0) return 0;
  return (size_t)snprintf(out, cap, "nvRecover serial='%s'", serial);
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

// health/heartRate/daily
// Format does NOT match the layout in the python codec
// (docs/evenrealities_rev_share-main/.../ring1_packet_codec.py
// _parse_common_daily) — that codec was developed against firmware
// v2.1.0_beta_v3 and falls through for our v2.2.0.24 payloads. So this
// parser is hand-rolled from empirical captures only.
//
// Header (11 B):
//   [0]    record count
//   [1..2] reserved (00 00)
//   [3..6] startTs   u32 LE — earliest sample (0 if no data window yet)
//   [7..10] endTs    u32 LE — latest sample
// Records (4 B each):
//   [0]    HR     BPM. HIGH confidence. The HR in record 0 always matches
//                 the corresponding `hr point` response, suggesting record 0
//                 is a "latest sample" overlay rather than a fixed historical
//                 record (we observed this byte change from 85 → 76 in the
//                 same b1=8 record across queries 30 min apart).
//   [1]    b1     UNKNOWN. Originally guessed "hour of day UTC" because the
//                 values 8/9/14/15 looked like hour markers and the latest
//                 record's b1 matched the current UTC hour. But: record 0's
//                 b1=8 stays constant while its HR changes, which doesn't
//                 fit a per-hour-bucket model. Could be a slot index, a
//                 sample sequence, or hour-of-day with record 0 special-cased.
//   [2..3] ?,?    UNKNOWN. Drift slightly between queries even when b0 (HR)
//                 stays constant — they aren't simple min/max for the bucket.
// Trailing: 1 byte, possibly a checksum or padding.
//
// Defensive: if the size doesn't match `count` records + 1 trailing, we
// emit the count + timestamps anyway and bail on records — the firmware
// has been observed to vary header layouts across versions, so don't
// assume our hypothesis holds for unfamiliar shapes.
// health/{heartRate,spo2,hrv,temperature}/daily — count-prefixed history
// with a fixed-size record stream. Hand-rolled against firmware 2.2.0.0011
// HR captures (3-record fixture verified live 2026-05-02); applied
// speculatively to the sibling metrics (spo2/hrv/temperature) under the
// hypothesis that they share the envelope and only differ in per-record
// interpretation.
//
// The Python codec (ring1_packet_codec.py _parse_common_daily) was developed
// against firmware v2.1.0_beta_v3 and has a multi-layout dispatcher we don't
// match — the codec's header is 5-6 bytes vs our 11-byte hr header. So we
// emit a "size mismatch" breadcrumb when the hypothesis fails, which is what
// lets a first-capture-of-spo2-daily debug itself rather than show raw hex.
//
// CONFIDENCE per metric:
//   * heartRate  ✓   verified live 2026-05-02 (3-record fixture, hr=69)
//   * spo2       ◯   no capture yet on our firmware — record[0] guess is %
//   * hrv        ◯   no capture yet — record[0] guess is RMSSD ms (low byte)
//   * temperature ✗  ring rejects all temperature opcodes on our firmware
static size_t annotateGenericDaily(uint8_t cmd, const uint8_t* p, size_t len,
                                   char* out, size_t cap) {
  if (len < 11) return 0;
  uint8_t count = p[0];
  uint32_t startTs = readU32LE(p + 3);
  uint32_t endTs   = readU32LE(p + 7);
  size_t recordsStart = 11;
  size_t expectedLen  = recordsStart + (size_t)count * 4 + 1;

  // Metric tag for log readability + per-metric record[0] label.
  // For HR the label is verified (BPM). For the others we use a guess
  // tag so the log makes clear we haven't confirmed.
  const char* tag      = "?Daily";
  const char* valLabel = "val";
  switch (cmd) {
    case R1_CMD_HEARTRATE:   tag = "hrDaily";   valLabel = "hr";        break;
    case R1_CMD_SPO2:        tag = "spo2Daily"; valLabel = "spo2?";     break;
    case R1_CMD_HRV:         tag = "hrvDaily";  valLabel = "hrv?";      break;
    case R1_CMD_TEMPERATURE: tag = "tempDaily"; valLabel = "temp?";     break;
    default: break;
  }

  char tsStartBuf[32], tsEndBuf[32];
  formatEpoch(startTs, tsStartBuf, sizeof(tsStartBuf));
  formatEpoch(endTs,   tsEndBuf,   sizeof(tsEndBuf));

  if (expectedLen != len) {
    // Header says count=N but body doesn't fit the hr template. Don't
    // pretend we parsed records; emit the header fields and a length
    // breadcrumb so we can spot a new firmware variant or a per-metric
    // shape difference in the log.
    return (size_t)snprintf(out, cap,
        "%s count=%u start=%s end=%s (size mismatch: got %u B, expected %u for hr-template — metric=%s may have different shape)",
        tag, count, tsStartBuf, tsEndBuf,
        (unsigned)len, (unsigned)expectedLen, valLabel);
  }

  size_t off = (size_t)snprintf(out, cap,
      "%s count=%u start=%s end=%s recs=[",
      tag, count, tsStartBuf, tsEndBuf);
  for (uint8_t i = 0; i < count && off + 32 < cap; i++) {
    const uint8_t* r = p + recordsStart + (size_t)i * 4;
    off += snprintf(out + off, cap - off,
                    "%s{%s=%u b1=%u b2=%02X b3=%02X}",
                    i ? "," : "", valLabel, r[0], r[1], r[2], r[3]);
  }
  off += snprintf(out + off, cap - off, "]");
  return off;
}

// health/sleep/daily — sleep session with a trailing stage array.
//
// Speculative parser ported from ring1_packet_codec.py _parse_sleep
// (firmware v2.1.0_beta_v3). NOT yet captured on our firmware
// 2.2.0.0011 — the ring's test wearer hasn't logged a sleep session
// yet, so all queries return ack-only. This parser is staged so the
// first real response is readable instead of a hex blob; expect
// adjustments after live capture.
//
// Wire layout (per python codec — bytes 1..29 are sparsely-named
// fields whose semantics weren't reverse-engineered; we just label
// the meaningful ones):
//   [0]      sleep_type_code (BleRing1SleepType: 0=long, 1=short)
//   [1..29]  unknown fields (sleep onset/wakeup times, totals, ...)
//   [30..31] stage_count u16 LE
//   [32..]   stage_count × { stage_type:u8, duration_minutes:u16 LE }
//
// Stage types per BleRing1SleepStageType:
//   0 = awake, 1 = rem, 2 = light, 3 = deep
static size_t annotateSleep(const uint8_t* p, size_t len, char* out, size_t cap) {
  if (len < 1) return 0;
  const char* sleepType =
      (p[0] == 0) ? "long" :
      (p[0] == 1) ? "short" : "?";
  size_t off = (size_t)snprintf(out, cap,
      "sleep type=%u(%s) totalLen=%u",
      p[0], sleepType, (unsigned)len);
  if (len < 32) {
    off += snprintf(out + off, cap - off, " (no stage block — payload too short)");
    return off;
  }
  uint16_t stageCount = readU16LE(p + 30);
  off += snprintf(out + off, cap - off, " stages=%u recs=[", stageCount);
  const size_t stageBase = 32;
  for (uint16_t i = 0; i < stageCount && off + 32 < cap; i++) {
    const size_t recOff = stageBase + (size_t)i * 3;
    if (recOff + 3 > len) {
      off += snprintf(out + off, cap - off, "%s(truncated)", i ? "," : "");
      break;
    }
    const uint8_t stageType = p[recOff];
    const uint16_t duration = readU16LE(p + recOff + 1);
    const char* stageName =
        (stageType == 0) ? "awake" :
        (stageType == 1) ? "rem"   :
        (stageType == 2) ? "light" :
        (stageType == 3) ? "deep"  : "?";
    off += snprintf(out + off, cap - off,
                    "%s{%s=%um}",
                    i ? "," : "", stageName, duration);
  }
  off += snprintf(out + off, cap - off, "]");
  return off;
}

// health/activity/daily — paginated multi-frame response.
//
// Record layout cross-referenced against the python codec
// (docs/evenrealities_rev_share-main/.../ring1_packet_codec.py
// _parse_activity_item):
//   [0]    slot_index — 10-minute bin since base_ts (slot 50 = +500 min)
//   [1..2] steps      — i16 LE
//   [3..4] ?,?        — unknown (always small in our captures, ~02-08)
//   [5..6] kcal       — i16 LE
//
// Verified against your 2026-05-02 captures: slot 54 showed steps=97 kcal=23
// during an active 10-minute window. Slots 50-53 (sedentary) showed steps=0-8
// and kcal=2-20.
//
// Header layout (NOT matching python codec — their 14-byte header doesn't
// fit our 7-byte-header firmware variant):
//   [0]    pageMarker / chunk-id (varies across frames in same response —
//          0x04 for the overview, 0x0B/0x10/etc. for data frames)
//   [1..2] reserved (always 00 00 in captures)
//   [3..6] base_ts u32 LE (zero for overview frame, today's midnight UTC
//          for data frames)
// Records start at offset 7, 7 bytes each. Final record may be truncated
// by frame-size limits (we've seen 1-2 trailing bytes of a partial record);
// the ring continues the data in subsequent notify frames.
static size_t annotateActivityDaily(const uint8_t* p, size_t len, char* out, size_t cap) {
  if (len < 7) return 0;
  uint8_t pageMarker = p[0];
  uint32_t ts        = readU32LE(p + 3);
  size_t recordsStart = 7;
  size_t recCount = (len - recordsStart) / 7;
  size_t partial  = (len - recordsStart) % 7;
  char tsBuf[32];
  formatEpoch(ts, tsBuf, sizeof(tsBuf));
  size_t off = (size_t)snprintf(out, cap,
      "activityDaily page=0x%02X base=%s recs(%u)=[",
      pageMarker, tsBuf, (unsigned)recCount);
  for (size_t i = 0; i < recCount && off + 48 < cap; i++) {
    const uint8_t* r = p + recordsStart + i * 7;
    uint8_t  slot  = r[0];
    int16_t  steps = (int16_t)((uint16_t)r[1] | ((uint16_t)r[2] << 8));
    int16_t  kcal  = (int16_t)((uint16_t)r[5] | ((uint16_t)r[6] << 8));
    // Slot is in 10-minute bins since base_ts. Compute minute-of-day for
    // a quick visual cue (slot 54 → +540 min → 09:00 from base midnight).
    unsigned minOfDay = (unsigned)slot * 10;
    off += snprintf(out + off, cap - off,
                    "%s{slot=%u(+%u min) steps=%d kcal=%d ?=%02X,%02X}",
                    i ? "," : "", slot, minOfDay, (int)steps, (int)kcal,
                    r[3], r[4]);
  }
  off += snprintf(out + off, cap - off, "]");
  if (partial > 0) {
    off += snprintf(out + off, cap - off, " +%u B partial(continuation?)", (unsigned)partial);
  }
  return off;
}

size_t r1AnnotatePayload(const R1Decoded& d, char* out, size_t cap) {
  if (cap == 0 || d.payloadLength == 0) return 0;
  // Dispatch by (module, cmd, subCmd). Health module uses the same daily/
  // point/measure subCmds across heartRate/spo2/temperature/hrv/sleep, but
  // only the heartRate parsers are hypothesised — the others would just
  // return 0 and fall through to raw hex.
  if (d.module == R1_MODULE_SYSTEM && d.cmd == R1_CMD_SYSTEM) {
    switch (d.subCmd) {
      case R1_SUB_DEVICE_STATUS:
      case R1_SUB_HEARTBEAT:
        return annotateDeviceStatus(d.payload, d.payloadLength, out, cap);
      case R1_SUB_DEVICE_INFO:
        return annotateDeviceInfo(d.payload, d.payloadLength, out, cap);
      case R1_SUB_DEVICE_SN:
        return annotateDeviceSn(d.payload, d.payloadLength, out, cap);
      case R1_SUB_GET_ALGO_KEY_STATUS:
        return annotateAlgoKey(d.payload, d.payloadLength, out, cap);
      case R1_SUB_WEAR_STATUS:
        return annotateWearStatus(d.payload, d.payloadLength, out, cap);
      case R1_SUB_HEALTH_SETTINGS:
      case R1_SUB_SYSTEM_SETTINGS:
        // Same 12-byte feature-bitmap shape, reuse the same dumper.
        return annotateHealthSettings(d.payload, d.payloadLength, out, cap);
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
    // Daily history — same hr-template envelope across all four metrics.
    // Confidence is high only for HR; the others use the same dispatcher
    // and emit a size-mismatch breadcrumb if the hypothesis fails on a
    // first real capture. See annotateGenericDaily for confidence notes.
    if (d.subCmd == R1_SUB_DAILY &&
        (d.cmd == R1_CMD_HEARTRATE || d.cmd == R1_CMD_SPO2 ||
         d.cmd == R1_CMD_HRV       || d.cmd == R1_CMD_TEMPERATURE)) {
      return annotateGenericDaily(d.cmd, d.payload, d.payloadLength, out, cap);
    }
    if (d.cmd == R1_CMD_ACTIVITY && d.subCmd == R1_SUB_DAILY) {
      return annotateActivityDaily(d.payload, d.payloadLength, out, cap);
    }
    // Sleep — speculative parser staged for the first real session capture.
    if (d.cmd == R1_CMD_SLEEP && d.subCmd == R1_SUB_DAILY) {
      return annotateSleep(d.payload, d.payloadLength, out, cap);
    }
  }
  return 0;
}

// =============================================================================
// Self-test against captured FlutterApp fixtures
// =============================================================================
//
// docs/FlutterApp-main/test/protocol/r1_messages_test.dart, the syncTime test:
//   encoder.auth();                     → serial=1, pairAuth   payload=[01]
//   encoder.healthSettingsStatus();     → serial=2 (used to advance the
//                                         counter; bytes are not asserted)
//   encoder.syncTime(2026-03-19T00:49:55Z, tz=-4h)
//                                       → serial=3, systemTime
//                                         payload=10 FF 33 48 BB 69
//
// Expected wire bytes (verbatim from the dart test fixture):
//   pairAuth:    00 97 19 53 F9 64 01 64 01 00 00 00 08 0D 00 3F 01 01
//   syncTime:    00 D4 C9 BA 70 64 01 64 03 00 02 00 05 12 00 80 77
//                10 FF 33 48 BB 69
//
// If our encoder reproduces these bytes exactly, the CRC16 + CRC32 + status
// byte + LE pack helpers are all wired up correctly. If it doesn't, the
// log line points at the first divergent byte so the bug is easy to find.

static bool selfTestCompare(const char* label,
                            const uint8_t* actual, size_t actualLen,
                            const uint8_t* expected, size_t expectedLen) {
  if (actualLen != expectedLen) {
    DEBUG_G2F("[R1-selftest] FAIL %s: length=%u (expected %u)",
              label, (unsigned)actualLen, (unsigned)expectedLen);
    return false;
  }
  for (size_t i = 0; i < actualLen; i++) {
    if (actual[i] != expected[i]) {
      DEBUG_G2F("[R1-selftest] FAIL %s: byte[%u]=%02X (expected %02X)",
                label, (unsigned)i, actual[i], expected[i]);
      return false;
    }
  }
  DEBUG_G2F("[R1-selftest] PASS %s (%u B)", label, (unsigned)actualLen);
  return true;
}

bool r1ProtocolSelfTest() {
  R1Encoder encoder;

  // Vector 1 — pairAuth at serial=1.
  static const uint8_t expectedAuth[] = {
    0x00, 0x97, 0x19, 0x53, 0xF9,
    0x64, 0x01, 0x64, 0x01, 0x00, 0x00, 0x00, 0x08, 0x0D, 0x00, 0x3F, 0x01,
    0x01,
  };
  R1Frame auth = encoder.buildPairAuth();
  bool ok1 = selfTestCompare("pairAuth(ser=1)", auth.bytes, auth.length,
                             expectedAuth, sizeof(expectedAuth));

  // Advance the encoder so the next build lands at serial=3, matching the
  // dart test setup (which calls healthSettingsStatus() between auth and
  // syncTime).
  (void)encoder.buildHealthSettingsQuery();

  // Vector 2 — syncTime at serial=3, tz=-4h (-240 min), epoch=1773881395
  // (= 2026-03-19T00:49:55Z, the exact moment the dart fixture parses).
  static const uint8_t expectedTime[] = {
    0x00, 0xD4, 0xC9, 0xBA, 0x70,
    0x64, 0x01, 0x64, 0x03, 0x00, 0x02, 0x00, 0x05, 0x12, 0x00, 0x80, 0x77,
    0x10, 0xFF, 0x33, 0x48, 0xBB, 0x69,
  };
  R1Frame timeFrame = encoder.buildSyncTime((int16_t)-240, (uint32_t)1773881395);
  bool ok2 = selfTestCompare("syncTime(ser=3,tz=-240,epoch=1773881395)",
                             timeFrame.bytes, timeFrame.length,
                             expectedTime, sizeof(expectedTime));

  // Vector 3-5 — payload annotator dry-runs against captured 2026-05-02 logs.
  // We just check the annotator produces *some* output (non-zero return) and
  // the output is null-terminated within bounds. Wrong field interpretation
  // would still return non-zero — the value here is catching a future
  // refactor that breaks the dispatcher entirely.

  auto runAnnotate = [](const char* label, uint8_t module, uint8_t cmd,
                        uint8_t subCmd, const uint8_t* payload, size_t len) -> bool {
    R1Decoded d = {};
    d.module = module;
    d.cmd    = cmd;
    d.subCmd = subCmd;
    d.payloadLength = len;
    if (len <= sizeof(d.payload)) memcpy(d.payload, payload, len);
    char buf[256];
    size_t n = r1AnnotatePayload(d, buf, sizeof(buf));
    if (n == 0) {
      DEBUG_G2F("[R1-selftest] FAIL %s: annotator returned 0", label);
      return false;
    }
    DEBUG_G2F("[R1-selftest] PASS %s → '%s'", label, buf);
    return true;
  };

  // Captured 2026-05-02 10:22:14 — payload[1]=[02] (wear).
  static const uint8_t pWear[]    = { 0x02 };
  // Captured 10:25:17 — payload[12]=[00 00 00 00 01 00 00 00 00 00 00 00].
  static const uint8_t pHealth[]  = { 0,0,0,0, 1, 0,0,0,0,0,0,0 };
  // Captured 10:22:44 — payload[8]=[00 00 61 08 F6 69 01 45]. HR=69.
  static const uint8_t pHrPoint[] = { 0x00,0x00, 0x61,0x08,0xF6,0x69, 0x01, 0x45 };
  // Captured 10:22:52 — 24-byte hr/daily with 3 records.
  static const uint8_t pHrDaily[] = {
    0x03, 0x00, 0x00, 0x80,0x3E,0xF5,0x69, 0x61,0x08,0xF6,0x69,
    0x45, 0x08, 0x4A, 0x4E, 0x48, 0x09, 0x57, 0x67, 0x45, 0x0E, 0x50, 0x64, 0x45,
  };
  // Captured 11:19:22 — 44-byte activity/daily with 5 records (one partial).
  // Slot 54 = `36 61 00 0B 00 17 00` → steps=97 kcal=23 — this is the
  // canary that proves steps + kcal extraction is working.
  static const uint8_t pActivity[] = {
    0x10, 0x00, 0x00, 0x80,0x3E,0xF5,0x69,
    0x32, 0x00,0x00, 0x00,0x00, 0x02,0x00,
    0x33, 0x07,0x00, 0x07,0x00, 0x14,0x00,
    0x34, 0x00,0x00, 0x02,0x00, 0x0E,0x00,
    0x35, 0x08,0x00, 0x04,0x00, 0x10,0x00,
    0x36, 0x61,0x00, 0x0B,0x00, 0x17,0x00,
    0x37, 0x2A,
  };
  // Captured 11:57:44 — `ringquery raw 1 0 1` → deviceStatus 7-byte payload.
  static const uint8_t pDevStatus[] = { 0x49, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00 };

  bool ok3 = runAnnotate("annotate wearStatus(02)", R1_MODULE_SYSTEM, R1_CMD_SYSTEM,
                         R1_SUB_WEAR_STATUS, pWear, sizeof(pWear));
  bool ok4 = runAnnotate("annotate healthSettings(bit4)", R1_MODULE_SYSTEM, R1_CMD_SYSTEM,
                         R1_SUB_HEALTH_SETTINGS, pHealth, sizeof(pHealth));
  bool ok5 = runAnnotate("annotate hrPoint(hr=69)", R1_MODULE_HEALTH, R1_CMD_HEARTRATE,
                         R1_SUB_POINT, pHrPoint, sizeof(pHrPoint));
  bool ok6 = runAnnotate("annotate hrDaily(3 recs)", R1_MODULE_HEALTH, R1_CMD_HEARTRATE,
                         R1_SUB_DAILY, pHrDaily, sizeof(pHrDaily));
  bool ok7 = runAnnotate("annotate activityDaily(5 recs steps+kcal)",
                         R1_MODULE_HEALTH, R1_CMD_ACTIVITY, R1_SUB_DAILY,
                         pActivity, sizeof(pActivity));
  bool ok8 = runAnnotate("annotate deviceStatus(byte0=73,wear)",
                         R1_MODULE_SYSTEM, R1_CMD_SYSTEM, R1_SUB_DEVICE_STATUS,
                         pDevStatus, sizeof(pDevStatus));

  return ok1 && ok2 && ok3 && ok4 && ok5 && ok6 && ok7 && ok8;
}

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
