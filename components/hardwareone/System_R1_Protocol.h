#ifndef SYSTEM_R1_PROTOCOL_H
#define SYSTEM_R1_PROTOCOL_H

#include "System_BuildConfig.h"
#include <Arduino.h>
#include <stdint.h>
#include <stddef.h>

// =============================================================================
// Even Realities R1 ring — wire protocol encoder/decoder
// =============================================================================
// Port of the FlutterApp's RingPacketEncoder
// (docs/FlutterApp-main/lib/src/protocol/r1_messages.dart). The ring talks
// a custom framed binary protocol over its write/notify chars
// (UUIDs G2RING_CHAR_WRITE_UUID / G2RING_CHAR_NOTIFY_UUID, see G2_Ring.h).
//
// Frame layout — outer envelope (one BLE write per frame):
//   [0]      transferType (always 0x00 for our use; 0x?? = file-transfer subset
//            we don't implement)
//   [1..4]   CRC32 of the model bytes — Castagnoli polynomial 0x1EDC6F41 with
//            init=0, no reflection, no final XOR. NOT zlib CRC32.
//   [5..N]   model (12-byte header + payload)
//
// Model header (12 bytes):
//   [ 0]     version 0x64
//   [ 1]     module: 0x01=system, 0x02=health, 0x03=sport
//   [ 2]     moduleVersion 0x64
//   [ 3]     serial low byte (LE)
//   [ 4]     serial high byte (LE)        ← excluded from CRC16
//   [ 5]     status byte (see encodeStatus below)
//   [ 6]     cmd
//   [ 7]     subCmd
//   [ 8]     modelLength low (LE) = 12 + payload.length
//   [ 9]     modelLength high (LE)
//   [10]     CRC16 low (LE)               ← excluded from CRC16
//   [11]     CRC16 high (LE)              ← excluded from CRC16
//   [12..]   payload
//
// CRC16 input = model[0..3] + model[5..9] + model[12..]
// CRC16 algo  = CCITT-XMODEM-like, init 0xFFFF — see r1Crc16().
//
// Setup primitives. The control owner is responsible for sequencing them and
// waiting for acknowledgements; this codec only builds/decodes frames:
//   1. Send pairAuth (subCmd=0x08 payload=[0x01], status=notify/get/ok)
//   2. Read deviceInfo and select an exact protocol profile
//   3. Send systemTime (subCmd=0x05 payload=tz_min(i16 LE)+epoch_s(u32 LE),
//      status=notify/SET/ok)
//   4. When the selected exact profile supports it (2.2.7.0005 and
//      2.2.9.0003), send advStart (subCmd=0x0A payload=reversed G2 right MAC
//      + reversed G2 left MAC, 12 B total, status=notify/get/ok)
//
// After step 1 the ring acks with status=ack/set/ok and begins emitting
// telemetry frames on its notify char (HR, HRV, temperature, activity,
// sleep, etc.). NO server-issued pkey is required — the literal byte 0x01
// is the entire auth payload.
// =============================================================================

#if ENABLE_BLUETOOTH && ENABLE_G2_GLASSES

// -----------------------------------------------------------------------------
// Constants — module / cmd / subCmd / status
// -----------------------------------------------------------------------------

// Module IDs (model[1])
#define R1_MODULE_SYSTEM    0x01
#define R1_MODULE_HEALTH    0x02
#define R1_MODULE_SPORT     0x03
#define R1_MODULE_TESTABLE  0x7F

// Cmd IDs (model[6]) — namespace is per-module. system module uses 0x00.
#define R1_CMD_SYSTEM       0x00
#define R1_CMD_HEARTRATE    0x01
#define R1_CMD_SPO2         0x02
#define R1_CMD_TEMPERATURE  0x03
#define R1_CMD_HRV          0x04
#define R1_CMD_ACTIVITY     0x05
#define R1_CMD_SLEEP        0x06
#define R1_CMD_HEALTHSET    0x07

// SubCmd IDs (model[7]) — for module=system, cmd=system.
#define R1_SUB_DEVICE_STATUS         0x01
#define R1_SUB_DEVICE_INFO           0x02
#define R1_SUB_WEAR_STATUS           0x03
#define R1_SUB_USER_INFO             0x04
#define R1_SUB_SYSTEM_TIME           0x05
#define R1_SUB_TOUCH_STATUS          0x06
#define R1_SUB_TOUCH_SWITCH          0x07
#define R1_SUB_PAIR_AUTH             0x08
#define R1_SUB_OTA_START             0x09
#define R1_SUB_ADV_START             0x0A
#define R1_SUB_GET_ALGO_KEY_STATUS   0x0B
#define R1_SUB_SET_ALGO_KEY          0x0C
#define R1_SUB_HEALTH_SETTINGS       0x0E
#define R1_SUB_SYSTEM_SETTINGS       0x0F
#define R1_SUB_DEVICE_SN             0x10
#define R1_SUB_NV_RECOVER            0x11
#define R1_SUB_POWER_CONTROL         0x12
#define R1_SUB_PACKET_ACK            0x7E
#define R1_SUB_HEARTBEAT             0x7F
#define R1_SUB_RGH_HEARTBEAT         0x80
#define R1_SUB_RGH_SHAKE_HANDS       0x81
#define R1_SUB_REMOVE_RING_NOTIFY    0x82

// SubCmd IDs for module=health, cmd=heartRate / spo2 / temperature / hrv / sleep
#define R1_SUB_DAILY                 0x01
#define R1_SUB_POINT                 0x02
#define R1_SUB_MEASURE               0x03

// Status byte bit layout (model[5]):
//   bit0     type:    0=notify, 1=ack
//   bit1     method:  0=get, 1=set
//   bits2-3  ack:     0=ok, 1=error, 2=refuse, 3=notSupport
#define R1_STATUS_TYPE_NOTIFY     0
#define R1_STATUS_TYPE_ACK        1
#define R1_STATUS_METHOD_GET      0
#define R1_STATUS_METHOD_SET      1
#define R1_STATUS_ACK_OK          0
#define R1_STATUS_ACK_ERROR       1
#define R1_STATUS_ACK_REFUSE      2
#define R1_STATUS_ACK_NOTSUPPORT  3

// Bound on payload size per frame. The ring rarely emits anything large; the
// biggest captured frame in the FlutterApp's test fixtures is ~80 B (nvRecover).
// 256 leaves headroom for unknown future opcodes without dynamic allocation.
#define R1_MAX_PAYLOAD             256
#define R1_MAX_FRAME              (1 + 4 + 12 + R1_MAX_PAYLOAD)

// Firmware-specific payloads are never selected by shape. Exact deviceInfo
// firmware strings select an identity, then each operation is admitted through
// the capability helpers below. Every other string remains diagnostic-only.
enum R1ProtocolProfile : uint8_t {
  R1_PROFILE_UNKNOWN = 0,
  R1_PROFILE_FW_2_2_7_0005 = 1,
  R1_PROFILE_FW_2_2_9_0003 = 2,
};

const char* r1ProtocolProfileName(R1ProtocolProfile profile);
R1ProtocolProfile r1ProfileForFirmware(const char* firmware);

// Per-operation policy. The 2.2.9 capture proves only a strict subset of the
// 2.2.7 traffic, so callers must not turn profile recognition into a blanket
// wire-layout alias. Generic pair-auth/device-info/time/device-status traffic
// remains profile-independent.
bool r1ProfileSupportsAdvStart(R1ProtocolProfile profile);
bool r1ProfileSupportsHealthCollectionSet(R1ProtocolProfile profile,
                                          bool enabled);
bool r1ProfileSupportsHealthQuery(R1ProtocolProfile profile, uint8_t cmd,
                                  uint8_t subCmd);
bool r1ProfileSupportsPointMeasureQuery(R1ProtocolProfile profile);
bool r1ProfileSupportsPointIngestion(R1ProtocolProfile profile);
// Exact-profile HardwareOne UI refresh contract, composed from the DAILY
// primitives observed in the 2.2.9 app/capture. This is deliberately separate
// from the legacy POINT/MEASURE experiment; no causal stock-page action is
// inferred from the capture.
bool r1ProfileSupportsHealthPageRefresh(R1ProtocolProfile profile);
bool r1ProfileSupportsSingleFrameDaily(R1ProtocolProfile profile, uint8_t cmd);
bool r1ProfileSupportsDailyPacketAck(R1ProtocolProfile profile, uint8_t cmd);
bool r1ProfileSupportsActivityReassembly(R1ProtocolProfile profile);
bool r1ProfileSupportsSleepDataIngestion(R1ProtocolProfile profile);
bool r1ProfileSupportsLowPower(R1ProtocolProfile profile);
bool r1ProfileSupportsUserInfo(R1ProtocolProfile profile);
// Ingestion of wear/battery from the capture-proven seven-byte deviceStatus
// response is independent from permission to transmit the dedicated,
// unproven wearStatus query.
bool r1ProfileSupportsDeviceStatusIngestion(R1ProtocolProfile profile);
bool r1ProfileSupportsWearStatus(R1ProtocolProfile profile);

// Structured parser result used by every typed decoder. These values are
// stable diagnostic/API identifiers; callers must not infer success from a
// partially populated output object.
enum R1ParseError : uint8_t {
  R1_PARSE_OK = 0,
  R1_PARSE_BAD_CRC = 1,
  R1_PARSE_LENGTH = 2,
  R1_PARSE_WRONG_PROFILE = 3,
  R1_PARSE_UNSUPPORTED_LAYOUT = 4,
  R1_PARSE_SLOT_RANGE = 5,
  R1_PARSE_VALUE_RANGE = 6,
  R1_PARSE_TOO_LARGE = 7,
  R1_PARSE_DUPLICATE_SLOT = 8,
};

const char* r1ParseErrorName(R1ParseError error);

// -----------------------------------------------------------------------------
// Encoded outbound frame (ready for writeChar->writeValue)
// -----------------------------------------------------------------------------
struct R1Frame {
  uint8_t  bytes[R1_MAX_FRAME];
  size_t   length;     // 0 if build failed (e.g. payload too large)
  uint16_t serial;     // serial number used for this frame
};

// -----------------------------------------------------------------------------
// Decoded inbound frame
// -----------------------------------------------------------------------------
struct R1Decoded {
  // crc32Valid is the real integrity check — must be true on every frame.
  // crc16Valid is reported separately because the R1 firmware emits a wrong
  // CRC16 on every outbound frame (verified against the dart test fixtures
  // in docs/FlutterApp-main/test/protocol/r1_messages_test.dart — every
  // ring→phone packet is mapped to crc16Ok=false there). So in practice
  // crc16Valid is `false` for any frame the ring sends us; that's normal,
  // not a transmission error.
  bool      crc16Valid;
  bool      crc32Valid;
  uint8_t   transferType;
  uint32_t  crc32Received;
  uint32_t  crc32Expected;
  uint16_t  crc16Received;
  uint16_t  crc16Expected;

  uint8_t   version;
  uint8_t   module;
  uint8_t   moduleVersion;
  uint16_t  serial;
  uint8_t   statusByte;
  uint8_t   statusType;     // R1_STATUS_TYPE_*
  uint8_t   statusMethod;   // R1_STATUS_METHOD_*
  uint8_t   statusAck;      // R1_STATUS_ACK_*
  uint8_t   cmd;
  uint8_t   subCmd;
  uint16_t  modelLength;    // as declared in the header
  bool      modelLengthValid;

  size_t    payloadLength;  // capped at R1_MAX_PAYLOAD
  uint8_t   payload[R1_MAX_PAYLOAD];
};

// A frame may reach typed ingestion only when both the outer CRC32 and the
// declared model length are valid and the complete payload fit our bounded
// decoder. Ring-originated CRC16 is intentionally not part of this gate; known
// firmware emits a bad CRC16 while retaining a valid CRC32.
R1ParseError r1ValidateDecoded(const R1Decoded& decoded);
bool r1DecodedIsTrusted(const R1Decoded& decoded);

// Compact capability for one packetAck. Only
// r1PacketAckDescriptorFromDecoded() can stamp a valid instance, and it does so
// after the full decoded-frame integrity, profile, status, and opcode whitelist
// gates pass. Keeping the echoed identifiers private prevents queue owners from
// manufacturing a "trusted" ACK request out of unvalidated wire fields; the
// read-only accessors exist for duplicate suppression and logs.
class R1PacketAckDescriptor {
public:
  R1PacketAckDescriptor() = default;

  bool valid() const;
  uint16_t receivedSerial() const { return serial_; }
  uint8_t module() const { return module_; }
  uint8_t cmd() const { return cmd_; }
  uint8_t subCmd() const { return subCmd_; }

private:
  friend bool r1PacketAckDescriptorFromDecoded(
      R1ProtocolProfile profile, const R1Decoded& decoded,
      R1PacketAckDescriptor& out);
  friend bool r1MakeDailyPacketAckDescriptor(R1ProtocolProfile profile,
                                             uint16_t serial, uint8_t cmd,
                                             R1PacketAckDescriptor& out);
  friend class R1Encoder;

  uint16_t serial_ = 0;
  uint8_t module_ = 0;
  uint8_t cmd_ = 0;
  uint8_t subCmd_ = 0;
  uint8_t trustMarker_ = 0;
};

static_assert(sizeof(R1PacketAckDescriptor) == 6,
              "packet ACK descriptor must stay compact");

bool r1PacketAckDescriptorFromDecoded(R1ProtocolProfile profile,
                                      const R1Decoded& decoded,
                                      R1PacketAckDescriptor& out);

struct R1DeviceInfo {
  char firmware[17];
  char hardware[17];
  R1ProtocolProfile profile;
};

// Exact 32-byte deviceInfo response: two NUL-padded 16-byte printable strings.
// Profile selection compares the decoded firmware string exactly.
R1ParseError r1ParseDeviceInfo(const R1Decoded& decoded, R1DeviceInfo& out);

// -----------------------------------------------------------------------------
// CRC helpers — exposed for unit testing / diagnostics. Not normally needed
// by callers (the encoder/decoder use them internally).
// -----------------------------------------------------------------------------
uint16_t r1Crc16(const uint8_t* data, size_t len);
uint32_t r1Crc32(const uint8_t* data, size_t len);

// CRC16 over the model excluding bytes [4] and [10..11], as the wire spec
// requires. modelLen must be >= 12.
uint16_t r1ModelCrc16(const uint8_t* model, size_t modelLen);

// One-shot self-test against sanitized golden vectors. It anchors both CRC
// algorithms, exact-profile builders, capability-gated settings/daily decoders,
// strict profile/integrity gates, and representative malformed pages. It contains no
// real device identifier or health value. Logs pass/fail via DEBUG_G2F; callers
// decide whether a failure should disable ring writes for the session.
bool r1ProtocolSelfTest();

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------
// Stateful — owns the per-session serial counter. The FlutterApp creates a
// fresh encoder per ring connect; we do the same. NOT thread-safe; call only
// from the BLE host task (or with external synchronisation).
class R1Encoder {
public:
  R1Encoder() : serial_(0) {}

  // Bumps `serial_` and returns the new value (1, 2, 3, ...).
  uint16_t nextSerial();

  // Reset the serial counter — useful when a new ring connection starts and
  // the peer expects to see serial 1 first.
  void resetSerial() { serial_ = 0; }

  // Build a generic frame. Returns length=0 without consuming a serial when
  // payload exceeds R1_MAX_PAYLOAD or a non-empty payload pointer is null.
  // Caller passes raw enums; the helper packs the status byte.
  R1Frame build(uint8_t module, uint8_t cmd, uint8_t subCmd,
                uint8_t statusType, uint8_t statusMethod, uint8_t statusAck,
                const uint8_t* payload, size_t payloadLen);

  // Convenience builders — cover the standard-setup sequence and a couple of
  // common probes. All bump the serial counter exactly once.

  // pairAuth: payload is the literal byte 0x01. Status notify/get/ok.
  R1Frame buildPairAuth();

  // systemTime: payload = tz_offset_minutes (i16 LE) + epoch_seconds (u32 LE).
  // Status notify/SET/ok (the only setup step that uses set, per FlutterApp).
  R1Frame buildSyncTime(int16_t tzOffsetMinutes, uint32_t epochSeconds);

  // Capability-gated advStart. Inputs are the natural six-byte temple MACs;
  // payload order is reversed-right followed by reversed-left. Unknown profile
  // or a missing MAC fails closed with length=0 and does not consume a serial.
  R1Frame buildAdvStart(R1ProtocolProfile profile,
                        const uint8_t* rightMac6,
                        const uint8_t* leftMac6);

  // Periodic keep-alive — used by the FlutterApp on a 30 s interval.
  // We don't call it ourselves yet but expose for future use.
  R1Frame buildHeartbeat();

  // Probe: ask the ring for its device info (firmware version etc).
  // Status notify/get/ok, empty payload.
  R1Frame buildDeviceInfoQuery();

  // Ring health-collection setting (system/system/0x0E). Only the timestamped
  // 12-byte SET and its empty ACK are capture-proven. There is intentionally
  // no GET/readback API. Unknown profiles fail closed without consuming a
  // serial.
  R1Frame buildHealthCollectionSet(R1ProtocolProfile profile,
                                   uint32_t epochSeconds, bool enabled);

  // Ring low-power setting (system/system/0x0F), switchType=0. Both GET and SET
  // fail closed unless the exact profile has capture-proven support.
  R1Frame buildLowPowerQuery(R1ProtocolProfile profile);
  R1Frame buildLowPowerSet(R1ProtocolProfile profile,
                           uint32_t epochSeconds, bool enabled);

  // Probe: ask the ring whether it currently detects skin contact.
  // Response payload should be a 1-byte BleRing1SystemWearStatus value
  // (0=unknown, 1=notWear, 2=wear) per the proto enum.
  R1Frame buildWearStatusQuery(R1ProtocolProfile profile);

  // Health-data requests. All three request flavours share an empty payload
  // and use status notify/get/ok. The ring may take a few seconds to respond
  // — `measure` in particular is a "start sampling now" command that returns
  // streaming notifies once the PPG algorithm has converged.
  //
  // `cmd` is one of R1_CMD_HEARTRATE / R1_CMD_SPO2 / R1_CMD_TEMPERATURE /
  // R1_CMD_HRV / R1_CMD_ACTIVITY / R1_CMD_SLEEP.
  // `subCmd` is R1_SUB_DAILY (aggregated history), R1_SUB_POINT (individual
  // measurement points), or R1_SUB_MEASURE (start a real-time session).
  //
  // Note: the ring rejects unsupported (cmd, subCmd) pairs with status=ack/
  // refuse — e.g. activity has no `measure` mode. That's fine, just
  // informative; we'll see the refusal in the decoded log.
  R1Frame buildHealthQuery(R1ProtocolProfile profile, uint8_t cmd,
                           uint8_t subCmd);

  // Acknowledge one trusted, capability-approved health daily data notify. The
  // payload echoes module/cmd/subCmd and received serial; byte 3 and bytes 6..9
  // are capture-proven zero but their semantics remain unknown. Invalid CRC,
  // length, status/opcode, or profile fails closed without consuming a serial.
  // The descriptor overload accepts only a capability produced by
  // r1PacketAckDescriptorFromDecoded(); it repeats the profile/opcode/stamp
  // checks before allocating the outbound serial.
  R1Frame buildPacketAck(R1ProtocolProfile profile,
                         const R1PacketAckDescriptor& received);
  R1Frame buildPacketAck(R1ProtocolProfile profile,
                         const R1Decoded& received);

  // Generic escape hatch for testing arbitrary (module, cmd, subCmd) tuples
  // with a notify/get/ok status and empty payload. Useful for poking at
  // unknown opcodes during RE.
  R1Frame buildGenericQuery(uint8_t module, uint8_t cmd, uint8_t subCmd);

private:
  uint16_t serial_;
};

// -----------------------------------------------------------------------------
// Decoder
// -----------------------------------------------------------------------------

// Decode a raw notify/read into out. Returns false if the frame is too short
// to contain the model header. Returns true for structurally decodable frames
// even when an integrity flag fails; inspect crc32Valid/modelLengthValid or use
// r1ValidateDecoded(). Oversized wire payloads are copied only up to the fixed
// buffer and force modelLengthValid=false, so typed ingestion fails closed.
bool r1Decode(const uint8_t* data, size_t len, R1Decoded& out);

// Friendly name lookups — return literal strings (no allocation), or "?"
// for unknown values. Useful for log lines.
const char* r1ModuleName(uint8_t module);
const char* r1CmdName(uint8_t module, uint8_t cmd);
const char* r1SubCmdName(uint8_t module, uint8_t cmd, uint8_t subCmd);
const char* r1StatusTypeName(uint8_t type);
const char* r1StatusMethodName(uint8_t method);
const char* r1StatusAckName(uint8_t ack);

// -----------------------------------------------------------------------------
// Diagnostic payload annotator
// -----------------------------------------------------------------------------
// Writes a short human-readable summary of `d.payload` into `out`. Daily
// summaries are emitted only through the exact selected profile and typed
// parsers. Returns the number of bytes written
// (excluding null terminator). Returns 0 if the opcode has no parser, in
// which case callers should just print the raw hex.
//
// Version-independent diagnostic annotations also cover these older confirmed
// payloads without feeding them into typed daily ingestion:
//   - system/system/wearStatus              → 1 B: 0=unknown 1=notWear 2=wear
//   - system/system/deviceInfo              → 32 B: 16-B ASCII fw + 16-B hw ver
//   - system/system/deviceSn                → redacted payload length only
//   - system/system/get/setAlgoKey          → status/length only, key redacted
//   - system/system/systemSettingsStatus    → 12 B low-power state
//   - system/system/nvRecover               → redacted payload length only
//   - system/system/deviceStatus            → 7 B: byte0=batt%?, wear, flag, zeros
//   - system/system/heartbeatPack           → same shape as deviceStatus
//   - health/heartRate/point                → value(i16) + ts + state + extra
//   - health/{hrv,spo2,temperature}/point   → same shape (extra=actual reading)
//   - health HR/SpO2/HRV/activity daily     → exact 2.2.7.0005 typed pages
//
// Anything else returns 0 and the caller falls back to raw hex.
size_t r1AnnotatePayload(R1ProtocolProfile profile, const R1Decoded& d,
                         char* out, size_t cap);

// -----------------------------------------------------------------------------
// Capability-gated settings/profile decoders
// -----------------------------------------------------------------------------
struct R1LowPowerStatus {
  uint32_t epochSeconds;
  uint8_t switchType;
  bool enabled;
};

struct R1UserInfo {
  uint8_t gender;
  uint8_t age;
  uint16_t heightCm;
  uint16_t weightKg;
  uint8_t reserved[6];
};

R1ParseError r1ParseLowPowerStatus(R1ProtocolProfile profile,
                                   const R1Decoded& decoded,
                                   R1LowPowerStatus& out);
// Decode-only policy boundary. There is intentionally no production user-info
// builder, UI, or CLI; changing that requires a separate explicit design
// decision and must never be inferred from a future capture alone.
R1ParseError r1ParseUserInfo(R1ProtocolProfile profile,
                             const R1Decoded& decoded,
                             R1UserInfo& out);

// -----------------------------------------------------------------------------
// Capability-gated daily page models
// -----------------------------------------------------------------------------
enum R1DailyMetric : uint8_t {
  R1_DAILY_METRIC_HEART_RATE = R1_CMD_HEARTRATE,
  R1_DAILY_METRIC_SPO2 = R1_CMD_SPO2,
  R1_DAILY_METRIC_HRV = R1_CMD_HRV,
  R1_DAILY_METRIC_ACTIVITY = R1_CMD_ACTIVITY,
};

// Daily pages expose two independent time fields. Captures prove that the
// ring can send a zero day base while the latest field is either an absolute
// epoch or seconds within a day. Keep the wire representation explicit so a
// caller cannot accidentally turn an unanchored slot into a 1970 timestamp.
enum R1DailyDayMode : uint8_t {
  R1_DAILY_DAY_ZERO_BASE = 0,
  R1_DAILY_DAY_EPOCH,
  R1_DAILY_DAY_UNKNOWN,
};

enum R1DailyTimestampMode : uint8_t {
  R1_DAILY_TIMESTAMP_NONE = 0,
  R1_DAILY_TIMESTAMP_EPOCH,
  R1_DAILY_TIMESTAMP_SECONDS_WITHIN_DAY,
  R1_DAILY_TIMESTAMP_UNKNOWN,
};

const char* r1DailyDayModeName(R1DailyDayMode mode);
const char* r1DailyTimestampModeName(R1DailyTimestampMode mode);

#define R1_COMMON_DAILY_MAX_RECORDS    24
#define R1_HRV_DAILY_MAX_RECORDS       24
// A full day has 144 ten-minute slots. A single BLE notification can only carry
// (R1_MAX_PAYLOAD-11)/7 = 35 records, so the ring fragments a larger activity
// day across several notifications (see the reassembly path in G2_Ring). The
// result type is sized for the full day; the single-frame decoder still self-
// limits to 35 because payloadLength must equal 11+count*7 within R1_MAX_PAYLOAD.
#define R1_ACTIVITY_DAILY_MAX_RECORDS  144
// Largest reassembled activity model: 12 B header + 11 B daily prefix + 144*7 B
// records = 1031 B. The reassembly buffer rounds up for headroom.
#define R1_ACTIVITY_REASSEMBLED_MODEL_MAX  (12U + 11U + 144U * 7U)
struct R1CommonDailyRecord {
  uint8_t hourSlot;
  uint8_t average;
  uint8_t maximum;
  uint8_t minimum;
  uint32_t bucketEpoch;
};

struct R1CommonDailyResult {
  R1ProtocolProfile profile;
  R1DailyMetric metric;
  uint16_t sourceSerial;
  uint32_t sourceCrc32;
  int16_t timezoneMinutes;
  R1DailyDayMode dayMode;
  uint32_t dayStart;
  R1DailyTimestampMode latestTimestampMode;
  uint32_t latestTimestampRaw;
  // Absolute epoch only. Zero means the raw value could not be anchored.
  uint32_t latestTimestamp;
  uint8_t latestValue;
  uint8_t count;
  uint32_t trailer;
  R1CommonDailyRecord records[R1_COMMON_DAILY_MAX_RECORDS];
};

struct R1HrvDailyRecord {
  uint8_t hourSlot;
  uint16_t average;
  uint16_t maximum;
  uint16_t minimum;
  uint32_t bucketEpoch;
};

struct R1HrvDailyResult {
  R1ProtocolProfile profile;
  R1DailyMetric metric;
  uint16_t sourceSerial;
  uint32_t sourceCrc32;
  int16_t timezoneMinutes;
  R1DailyDayMode dayMode;
  uint32_t dayStart;
  R1DailyTimestampMode latestTimestampMode;
  uint32_t latestTimestampRaw;
  // Absolute epoch only. Zero means the raw value could not be anchored.
  uint32_t latestTimestamp;
  uint16_t latestValue;
  uint8_t count;
  uint32_t trailer;
  R1HrvDailyRecord records[R1_HRV_DAILY_MAX_RECORDS];
};

struct R1ActivityDailyRecord {
  uint8_t tenMinuteSlot;
  uint16_t steps;
  uint16_t activeKcal;
  uint16_t restingKcal;
  uint16_t totalKcal;
  uint32_t bucketEpoch;
};

struct R1ActivityDailyResult {
  R1ProtocolProfile profile;
  R1DailyMetric metric;
  uint16_t sourceSerial;
  uint32_t sourceCrc32;
  int16_t timezoneMinutes;
  R1DailyDayMode dayMode;
  uint32_t dayStart;
  uint8_t count;
  uint32_t trailer;
  R1ActivityDailyRecord records[R1_ACTIVITY_DAILY_MAX_RECORDS];
};

R1ParseError r1ParseCommonDaily(R1ProtocolProfile profile,
                                const R1Decoded& decoded,
                                R1CommonDailyResult& out);
R1ParseError r1ParseHrvDaily(R1ProtocolProfile profile,
                             const R1Decoded& decoded,
                             R1HrvDailyResult& out);
R1ParseError r1ParseActivityDaily(R1ProtocolProfile profile,
                                  const R1Decoded& decoded,
                                  R1ActivityDailyResult& out);

// Parse an activity-daily frame that was reassembled from multiple BLE
// notifications. `model` is the concatenated model bytes (version…payload,
// i.e. the frame minus its 1-byte transferType and 4-byte CRC32), `modelLen`
// its length, and `crc32Whole` the CRC32 the fragments carried. Validates the
// CRC32 over the whole model and the daily header before parsing, so it fails
// closed exactly like the single-frame decoder. Shares the record-parsing core
// with r1ParseActivityDaily.
R1ParseError r1ParseReassembledActivityDaily(R1ProtocolProfile profile,
                                             const uint8_t* model,
                                             size_t modelLen,
                                             uint32_t crc32Whole,
                                             R1ActivityDailyResult& out);

// Stamp a trusted packetAck descriptor for a health daily NOTIFY that was
// validated outside the bounded R1Decoded path (i.e. after reassembly). Same
// trust-marker contract as r1PacketAckDescriptorFromDecoded — only this
// translation unit can mint a valid descriptor.
bool r1MakeDailyPacketAckDescriptor(R1ProtocolProfile profile, uint16_t serial,
                                    uint8_t cmd, R1PacketAckDescriptor& out);

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
#endif  // SYSTEM_R1_PROTOCOL_H
