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
// Auth flow (verified empirically against R1 firmware 2.0.8.0011 via captured
// frames in docs/FlutterApp-main/test/protocol/r1_messages_test.dart):
//   1. Send pairAuth (subCmd=0x08 payload=[0x01], status=notify/get/ok)
//   2. Send systemTime (subCmd=0x05 payload=tz_min(i16 LE)+epoch_s(u32 LE),
//      status=notify/SET/ok)
//   3. Send advStart (subCmd=0x0A payload=G2_right_mac_reversed(6 B),
//      status=notify/get/ok)
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

// -----------------------------------------------------------------------------
// CRC helpers — exposed for unit testing / diagnostics. Not normally needed
// by callers (the encoder/decoder use them internally).
// -----------------------------------------------------------------------------
uint16_t r1Crc16(const uint8_t* data, size_t len);
uint32_t r1Crc32(const uint8_t* data, size_t len);

// CRC16 over the model excluding bytes [4] and [10..11], as the wire spec
// requires. modelLen must be >= 12.
uint16_t r1ModelCrc16(const uint8_t* model, size_t modelLen);

// One-shot self-test against captured FlutterApp fixtures. Returns true if
// pairAuth (serial=1) AND systemTime (serial=3, tz=-4h, epoch=1773881395)
// reproduce their expected wire bytes verbatim. Cheap (~ a dozen XORs) and
// safe to call early in init. Logs detailed pass/fail via DEBUG_G2F so a
// silent regression in our CRC ports is caught the first time the ring
// module spins up after a flash.
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

  // Build a generic frame. Returns a R1Frame with length=0 if payload exceeds
  // R1_MAX_PAYLOAD. Caller passes raw enums; the helper packs the status byte.
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

  // advStart: payload = 6-byte MAC of the G2 right temple **in BLE address
  // order, NOT reversed** (i.e. for MAC c8:8d:65:00:97:69 send the bytes as
  // C8 8D 65 00 97 69). Confirmed against FlutterApp r1_manager.dart:262
  // which passes parseMacLikeBytes() output directly with no reversal, and
  // against docs/g2_proto/Ring_Bridge_Sequence.h Step 2.3 which says
  // "IN ORDER (NOT reversed)".
  // Pass nullptr to send all zeros (the FlutterApp comment notes the MAC
  // may not actually matter for some firmware — but reversing it definitely
  // breaks the bridge).
  R1Frame buildAdvStart(const uint8_t* mac6BleOrder);

  // Periodic keep-alive — used by the FlutterApp on a 30 s interval.
  // We don't call it ourselves yet but expose for future use.
  R1Frame buildHeartbeat();

  // Probe: ask the ring for its device info (firmware version etc).
  // Status notify/get/ok, empty payload.
  R1Frame buildDeviceInfoQuery();

  // Probe: request the active health-setting bitmap (which sensors are on).
  R1Frame buildHealthSettingsQuery();

  // Probe: ask the ring whether it currently detects skin contact.
  // Response payload should be a 1-byte BleRing1SystemWearStatus value
  // (0=unknown, 1=notWear, 2=wear) per the proto enum.
  R1Frame buildWearStatusQuery();

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
  R1Frame buildHealthQuery(uint8_t cmd, uint8_t subCmd);

  // Toggle the ring's continuous push behaviour. `enableMask` is sent as a
  // single byte payload; the bit layout isn't fully RE'd. Common values
  // worth trying: 0x00 (off), 0x01 (HR only?), 0xFF (everything). Status
  // notify/SET/ok.
  R1Frame buildHealthReportEnable(uint8_t enableMask);

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
// to even contain the model header (in which case `out` is left in an
// indeterminate state and callers should fall back to a hex dump). Returns
// true even if CRCs fail — inspect out.crcValid for that. Truncates payload
// at R1_MAX_PAYLOAD without flagging an error (such frames don't appear in
// any captured fixture but we don't want to crash on a future surprise).
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
// Speculative payload annotator
// -----------------------------------------------------------------------------
// Writes a short human-readable summary of `d.payload` into `out` based on
// best-guess parsers for known opcodes. Returns the number of bytes written
// (excluding null terminator). Returns 0 if the opcode has no parser, in
// which case callers should just print the raw hex.
//
// The parsers here are reverse-engineered from a small handful of captured
// payloads — none of them are protocol-confirmed. Each branch documents its
// confidence inline. Wrong guesses are LIES, not errors: the annotation is
// purely informational and never short-circuits the raw hex log.
//
// Confirmed via 2026-05-02 live captures + the python codec
// (docs/evenrealities_rev_share-main/.../ring1_packet_codec.py):
//   - system/system/wearStatus              → 1 B: 0=unknown 1=notWear 2=wear
//   - system/system/deviceInfo              → 32 B: 16-B ASCII fw + 16-B hw ver
//   - system/system/deviceSn                → ASCII serial (~15 B unprefixed)
//   - system/system/getAlgoKeyStatus        → status + ASCII hex device key
//   - system/system/healthSettingsStatus    → 12 B feature bitmap (byte 4)
//   - system/system/systemSettingsStatus    → 12 B feature bitmap (byte 5)
//   - system/system/nvRecover               → contains serial number ASCII
//   - system/system/deviceStatus            → 7 B: byte0=batt%?, wear, flag, zeros
//   - system/system/heartbeatPack           → same shape as deviceStatus
//   - health/heartRate/point                → value(i16) + ts + state + extra
//   - health/{hrv,spo2,temperature}/point   → same shape (extra=actual reading)
//   - health/heartRate/daily                → count + start/end ts + 4-B records
//   - health/activity/daily                 → page + ts + 7-B records (steps,kcal)
//
// Anything else returns 0 and the caller falls back to raw hex.
size_t r1AnnotatePayload(const R1Decoded& d, char* out, size_t cap);

#endif  // ENABLE_BLUETOOTH && ENABLE_G2_GLASSES
#endif  // SYSTEM_R1_PROTOCOL_H
