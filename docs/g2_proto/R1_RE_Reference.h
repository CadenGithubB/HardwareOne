#ifndef R1_RE_REFERENCE_H
#define R1_RE_REFERENCE_H

// =============================================================================
// R1 Ring Protocol — Reverse-Engineered Reference Header
// =============================================================================
//
// **Status (2026-05-02): Most of the wire format is now empirically verified
// against R1 firmware 2.2.0.24.** Earlier drafts of this file warned about a
// pkey requirement and an unverified envelope; both turned out to be wrong:
// pairAuth with the literal payload 0x01 is accepted, no server-issued key is
// involved, and the envelope shape now matches captured frames byte-for-byte.
//
// Sources cross-referenced:
//   1. docs/FlutterApp-main/lib/src/protocol/r1_messages.dart — encoder
//      (definitive for outbound shape; FlutterApp's own decoder doesn't
//      consume health data so its inbound parsers are sparse)
//   2. docs/evenrealities_rev_share-main/tools/btsnoop_parser/
//      ring1_packet_codec.py — independent codec from a different RE
//      effort (developed against firmware v2.1.0_beta_v3 — fields like
//      health-point and activity-record agree with our 2.2.0.24 captures;
//      common-daily layout DRIFTED between firmware versions, our parser
//      uses the empirical 2.2.0.24 shape rather than the codec's)
//   3. Live captures from this firmware (April–May 2026) via
//      `ringquery` CLI — see components/hardwareone/G2_Ring.cpp
//
// Wire codes verified by live captures are marked ✓
// Wire codes inferred from RE (FlutterApp / Python codec) are marked ?
// Wire codes that the firmware silently rejects (no ack, no refuse) are
// marked ✗ — the proto enum lists them but our firmware version doesn't
// implement them.
//
// All identifiers are prefixed `R1RE_` so they cannot collide with the
// firmware's `R1_*` macros in System_R1_Protocol.h.
// =============================================================================

// =============================================================================
// SECTION 1: BLE service / characteristic UUIDs (verified)
// =============================================================================
#define R1RE_SERVICE_UUID    "bae80001-4f05-4503-8e65-3af1f7329d1f" // ✓
#define R1RE_CHAR_WRITE_UUID "bae80012-4f05-4503-8e65-3af1f7329d1f" // ✓
#define R1RE_CHAR_NOTIFY_UUID "bae80013-4f05-4503-8e65-3af1f7329d1f" // ✓

// Advertising name format: "EVEN R1_XXXXXX" where XXXXXX = last 3 bytes of
// MAC in hex (e.g. mac F8:29:CA:BA:AC:1C → name "EVEN R1_BAAC1C"). ✓
//
// BLE address type: **Random Static** (top byte ≥ 0xC0). Direct connect via
// `BLEClient::connect(addr)` defaults to PUBLIC and silently times out at
// 30s; pass `BLE_ADDR_TYPE_RANDOM` explicitly. See firmware fix in
// G2_Ring.cpp:445-460.
//
// MTU: 64 negotiated by our firmware. FlutterApp negotiates 247 but our
// captured payloads never exceed 60 bytes anyway.

// =============================================================================
// SECTION 2: Wire envelope (✓ — verified against test fixtures and live frames)
// =============================================================================
//
// Each BLE write/notify is one envelope:
//
//   [0]      transferType    0x00 for normal frames (file-transfer subset
//                            uses non-zero values; we don't implement those)
//   [1..4]   CRC32 (LE)      computed over the entire model below
//   [5..]    model bytes:
//     [0]    version         0x64
//     [1]    module byte     0x01=system, 0x02=health, 0x03=sport, 0x7F=testable
//     [2]    moduleVersion   0x64
//     [3..4] serial (LE)     phone-side increments per outgoing message;
//                            ring echoes serial in ack frames
//     [5]    status byte     see SECTION 3
//     [6]    cmd byte        0x00 for module=system; 0x00..0x07 for module=health
//     [7]    subCmd byte     see SECTION 4 (system) / SECTION 5 (health)
//     [8..9] modelLen (LE)   total model length = 12 + payload.length
//     [10..11] CRC16 (LE)    over model[0..3] + model[5..9] + model[12..]
//                            — explicitly EXCLUDES the serial-high byte
//                            and the CRC16 field itself
//     [12..] payload
//
// **CRC algorithms**:
//   - Outer CRC32 = Castagnoli (polynomial 0x1EDC6F41), init=0, no reflection,
//     no final XOR. **NOT** standard zlib CRC32. esp_crc32_le() is wrong;
//     port the table-driven implementation from
//     docs/FlutterApp-main/lib/src/core/crc.dart `fileDataCrc32()`.
//   - Inner CRC16 = CCITT-XMODEM-like with the magic-shift form, init=0xFFFF.
//     Port from `crc16CcittLike()` in the same file.
//   - **Quirk**: the ring sends OUTBOUND frames with a wrong CRC16 — every
//     ring→phone notify in the FlutterApp test fixtures has crc16Ok=false.
//     This is firmware behaviour, not a transmission error. Our decoder
//     reports crc16Valid and crc32Valid separately and only flags `CRC32?!`
//     when CRC32 actually fails. The CRC32 is always correct.

#define R1RE_TRANSFER_TYPE_NORMAL  0x00 // ✓
#define R1RE_VERSION               0x64 // ✓
#define R1RE_MODVER                0x64 // ✓

// Module byte (model[1])
#define R1RE_MODULE_SYSTEM   0x01 // ✓
#define R1RE_MODULE_HEALTH   0x02 // ✓
#define R1RE_MODULE_SPORT    0x03 // ?
#define R1RE_MODULE_TESTABLE 0x7F // ?

// =============================================================================
// SECTION 3: Status byte — model[5]
// =============================================================================
// Bit layout:
//   bit 0     : type    0=notify, 1=ack
//   bit 1     : method  0=get, 1=set
//   bits 2..3 : ack     0=ok, 1=error, 2=refuse, 3=notSupport
//
// Phone usually sends notify/get/ok = 0x00 for queries and notify/set/ok =
// 0x02 for writes (e.g. systemTime, reportEnable). Ring replies with ack
// bit set and the ack bits filled per result.

#define R1RE_STATUS_TYPE_NOTIFY    0
#define R1RE_STATUS_TYPE_ACK       1
#define R1RE_STATUS_METHOD_GET     0
#define R1RE_STATUS_METHOD_SET     1
#define R1RE_STATUS_ACK_OK         0
#define R1RE_STATUS_ACK_ERROR      1
#define R1RE_STATUS_ACK_REFUSE     2
#define R1RE_STATUS_ACK_NOT_SUPPORT 3

#define R1RE_STATUS_BUILD(type, method, ack) \
    ((((type)  & 0x01) << 0) | \
     (((method) & 0x01) << 1) | \
     (((ack)   & 0x03) << 2))

#define R1RE_STATUS_TYPE(b)   (((b) >> 0) & 0x01)
#define R1RE_STATUS_METHOD(b) (((b) >> 1) & 0x01)
#define R1RE_STATUS_ACK(b)    (((b) >> 2) & 0x03)

// =============================================================================
// SECTION 4: System-module sub-commands
// =============================================================================
// model[1]=R1RE_MODULE_SYSTEM, model[6]=R1RE_SYS_CMD, model[7]=one of below.

#define R1RE_SYS_CMD              0x00 // ✓ (only cmd value seen for system module)

#define R1RE_SYS_SUB_DEVICE_STATUS              0x01 // ✓ returns 7 B (see SECTION 6).
                                                      //    byte[0] varies across reboots
                                                      //    (probably ring battery %)
#define R1RE_SYS_SUB_DEVICE_INFO                0x02 // ✓ returns 32 B = 16-B ASCII fw
                                                      //    version + 16-B ASCII hw
                                                      //    version, both null-padded.
                                                      //    Our ring: fw="2.2.0.0011"
                                                      //    hw="603MV1.9.3"
#define R1RE_SYS_SUB_WEAR_STATUS                0x03 // ✓ returns 1 B: 0/1/2 per
                                                      //    BleRing1SystemWearStatus
#define R1RE_SYS_SUB_USER_INFO                  0x04 // ✓ ring auto-emits empty notify
                                                      //    after pairAuth+systemTime
#define R1RE_SYS_SUB_SYSTEM_TIME                0x05 // ✓ payload = i16 LE tz_minutes +
                                                      //    u32 LE epoch_seconds
#define R1RE_SYS_SUB_TOUCH_STATUS               0x06 // ✗ silent on our firmware
#define R1RE_SYS_SUB_TOUCH_SWITCH               0x07 // ✗ silent on our firmware
#define R1RE_SYS_SUB_PAIR_AUTH                  0x08 // ✓ payload = literal byte 0x01.
                                                      //    NO server-issued pkey required.
                                                      //    Earlier docs claiming pkey
                                                      //    requirement were wrong.
#define R1RE_SYS_SUB_OTA_START                  0x09 // ?
#define R1RE_SYS_SUB_ADV_START                  0x0A // ✓ payload = G2 right-temple MAC
                                                      //    REVERSED (LSB first, 6 bytes)
#define R1RE_SYS_SUB_GET_ALGO_KEY_STATUS        0x0B // ✓ returns status byte + ASCII hex
                                                      //    string of device algo key.
                                                      //    Key starts with last 4 MAC
                                                      //    bytes in hex, then 8 bytes of
                                                      //    per-device unique seed.
#define R1RE_SYS_SUB_SET_ALGO_KEY               0x0C // ?
#define R1RE_SYS_SUB_HEALTH_SETTINGS_STATUS     0x0E // ✓ returns 12 B feature bitmap;
                                                      //    on our ring only byte[4]=0x01
                                                      //    is set
#define R1RE_SYS_SUB_SYSTEM_SETTINGS_STATUS     0x0F // ✓ returns 12 B feature bitmap;
                                                      //    same shape as health-settings
                                                      //    but different bit position
                                                      //    (byte[5]=0x01 on our ring)
#define R1RE_SYS_SUB_DEVICE_SN                  0x10 // ✓ returns ASCII serial number
                                                      //    string (~15 B, no length
                                                      //    prefix). Distinct from the
                                                      //    nvRecover serial — this is
                                                      //    the official user-visible SN.
                                                      //    Our ring: "B210DHACA200092"
#define R1RE_SYS_SUB_NV_RECOVER                 0x11 // ✓ ring auto-emits 44 B blob after
                                                      //    pairAuth. Payload[4..14]
                                                      //    contains ASCII serial number
                                                      //    (e.g. "ZYD5CZ1974") prefixed
                                                      //    by 0x5D delimiter. Different
                                                      //    serial from R1RE_SYS_SUB_DEVICE_SN.
#define R1RE_SYS_SUB_POWER_CONTROL              0x12 // ✗ silent on our firmware
#define R1RE_SYS_SUB_PACKET_ACK                 0x7E // ?
#define R1RE_SYS_SUB_HEARTBEAT_PACK             0x7F // ✓ ring sends spontaneously.
                                                      //    Same 7 B payload shape as
                                                      //    deviceStatus.

// --- Bridge-related sub-cmds (ring ↔ glasses) -------------------------------
// These three are how the ring tells the glasses "I'm here" and the glasses
// confirm. Required if you want ring gestures to drive glasses UI. Wire
// codes inferred from the python codec; not verified empirically.
#define R1RE_SYS_SUB_RING_GLASSES_HEARTBEAT_PACK 0x80 // ?
#define R1RE_SYS_SUB_RING_GLASSES_SHAKE_HANDS    0x81 // ?
#define R1RE_SYS_SUB_REMOVE_RING_NOTIFY          0x82 // ?

// =============================================================================
// SECTION 5: Health-module commands & sub-commands
// =============================================================================
// model[1]=R1RE_MODULE_HEALTH, model[6]=R1RE_HEALTH_CMD_*, model[7]=R1RE_HEALTH_SUB_*.

#define R1RE_HEALTH_CMD_HEART_RATE     0x01 // ✓
#define R1RE_HEALTH_CMD_SPO2           0x02 // ✓
#define R1RE_HEALTH_CMD_TEMPERATURE    0x03 // ✓ but ring rejects all subcmds for it
#define R1RE_HEALTH_CMD_HRV            0x04 // ✓
#define R1RE_HEALTH_CMD_ACTIVITY       0x05 // ✓
#define R1RE_HEALTH_CMD_SLEEP          0x06 // ✓ daily=0x01 returns ack only (no recorded
                                            //    sleep yet on test ring)
#define R1RE_HEALTH_CMD_HEALTH_SETTING 0x07 // ✓ used with subCmd reportEnable

// Per-metric sub-cmds (heart_rate, spo2, hrv, sleep)
#define R1RE_HEALTH_SUB_DAILY        0x01 // ✓ for hr/spo2/hrv/sleep/activity
#define R1RE_HEALTH_SUB_POINT        0x02 // ✓ for hr/hrv/spo2 — temperature returns
                                          //    no response on our firmware
#define R1RE_HEALTH_SUB_MEASURE      0x03 // ✗ silent for ALL metrics (hr/hrv/spo2/temp)
                                          //    on our firmware. Either needs a payload
                                          //    we haven't found or isn't implemented.

// Activity-specific
#define R1RE_HEALTH_SUB_ACTIVITY_DAILY            0x01 // ✓ multi-frame paginated response
#define R1RE_HEALTH_SUB_ACTIVITY_ALL_DAY_ACTIVITY 0x02 // ✗ silent on our firmware

// Health setting
#define R1RE_HEALTH_SUB_REPORT_ENABLE 0x01 // ✗ Tried module=health/cmd=healthSetting/
                                           //    subCmd=reportEnable with EVERY single
                                           //    -bit mask (0x00, 0x01, 0x02, 0x04, 0x08,
                                           //    0x10, 0x20, 0x40, 0x80) plus 0xFF — all
                                           //    silent on our firmware (2026-05-02
                                           //    bit-bash). Conclusion: opcode is either
                                           //    unimplemented in firmware 2.2.0.0011 or
                                           //    expects a multi-byte payload we haven't
                                           //    discovered. The official Even app would
                                           //    presumably know the right shape — sniff
                                           //    its traffic to find out.

// =============================================================================
// SECTION 6: Response payload shapes (empirical, 2026-05-02)
// =============================================================================
//
// ─── system/system/wearStatus (1 B) ────────────────────────────────────────
//   [0]    BleRing1SystemWearStatus: 0=unknown 1=notWear 2=wear
//
// ─── system/system/deviceInfo (32 B) ───────────────────────────────────────
//   [0..15]  ASCII fw version, NUL-padded (e.g. "2.2.0.0011")
//   [16..31] ASCII hw version, NUL-padded (e.g. "603MV1.9.3")
//
// ─── system/system/deviceSn (~15 B variable) ───────────────────────────────
//   [0..]   ASCII serial number string, no length prefix.
//           e.g. our ring: "B210DHACA200092" (15 chars)
//   This is the official user-visible SN. The nvRecover blob contains a
//   different identifier ("ZYD5CZ1974") that's likely an internal NV index.
//
// ─── system/system/getAlgoKeyStatus (~25 B) ────────────────────────────────
//   [0]     status byte (0x00 = OK in our captures)
//   [1..]   ASCII hex string of device key. First 8 chars = last 4 MAC
//           bytes in lowercase hex; remaining 16 chars = 8 bytes of
//           per-device unique seed.
//   e.g. our ring (MAC f8:29:ca:ba:ac:1c): "cabaac1c617823bd71160851"
//
// ─── system/system/healthSettingsStatus (12 B) ─────────────────────────────
//   [0..3]  reserved (00 00 00 00) — could be a u32 timestamp the python
//           codec calls "timestamp_seconds" but it's zero on our firmware
//   [4]     enabled_flag (per python codec). 0x01 on our ring. Possibly
//           a bitmap of which metrics auto-record.
//   [5..11] reserved (all zero in captures)
//
// ─── system/system/systemSettingsStatus (12 B) ─────────────────────────────
//   Same shape as healthSettingsStatus but encodes different settings.
//   On our ring: only byte[5]=0x01 set, rest zero. Compare to
//   healthSettings (byte[4]=0x01) — confirms these are independent feature
//   bitmaps even though the wire format is identical.
//
// ─── system/system/{deviceStatus, heartbeatPack} (7 B, shared shape) ───────
//   [0]     LIKELY ring battery percent. STABLE within a session (verified
//           constant 0x46=70 across 9 queries spanning 4 min on 2026-05-02);
//           drifts down across sessions/days (75 → 70 over hours of use).
//           Range 0x46..0x4D in our captures = 70..77 = plausible % values.
//           Not protocol-confirmed but high confidence.
//   [1]     wearStatus enum (consistently 0x02 = wear when worn)
//   [2]     UNKNOWN flag (0x01, stable)
//   [3..6]  reserved zeros
//
// ─── system/system/nvRecover (44 B in our captures) ────────────────────────
//   [0]     recover_type (0x02)
//   [1..2]  blob_length u16 LE (per python codec)
//   [3..4]  code_or_version u16 LE (per python codec)
//   [5..]   blob — contains ASCII serial number after a 0x5D delimiter,
//           e.g. our ring is "ZYD5CZ1974"
//
// ─── health/{heartRate,hrv,spo2,temperature}/point (8 B or 9 B) ────────────
//   [0..1]  value      i16 LE — primary reading slot. For HR this is 0
//                      (HR comes from extra_value); for temperature this
//                      probably carries the temp scaled ×10 or ×100, but
//                      we couldn't get a temperature response on our firmware
//                      to confirm.
//   [2..5]  timestamp  u32 LE epoch seconds — when the cached sample was
//                      recorded. Often 5-30 minutes stale (ring records on
//                      its own cadence; `point` returns the latest cached
//                      sample, NOT a fresh measurement).
//   [6]     state_code 0x01 = "have a sample"; haven't seen other values
//   [7..]   extra_value — single byte if total len==8, i16 LE if len==9.
//                      For HR: 1 byte = BPM. For HRV: 2 bytes = ms RMSSD.
//                      For SpO2: 1 byte = percent.
//
//   Verified live captures (2026-05-02 ~15:50 UTC):
//     hr:   value=0 state=1 extra=0x4C=76    → 76 BPM
//     hrv:  value=0 state=1 extra=0x0056=86  → 86 ms RMSSD
//     spo2: value=0 state=1 extra=0x63=99    → 99% SpO2
//
// ─── health/heartRate/daily (16 B for 1 record, 28 B for 4 records) ────────
//   Header (11 B):
//     [0]      record count
//     [1..2]   reserved (00 00)
//     [3..6]   startTs u32 LE — earliest sample (0 = no earlier data)
//     [7..10]  endTs   u32 LE — latest sample (matches the corresponding
//                              `point` response timestamp)
//   Records (4 B each):
//     [0]      HR in BPM (verified — matches `point` HR for record 0)
//     [1]      UNKNOWN. Values 8/9/14/15 looked like UTC hours but record
//              0's b1 stays constant while its HR changes between queries
//              minutes apart, so the simple "hour-of-day" model doesn't
//              hold. Possibly hour-of-day with record 0 special-cased to
//              "latest sample" overlay.
//     [2..3]   UNKNOWN. Drift slightly between queries. Not stable enough
//              to be min/max for the hour bucket.
//   Trailing: 1 byte. Possibly checksum, possibly padding.
//
//   The python codec
//   (docs/evenrealities_rev_share-main/.../ring1_packet_codec.py
//   _parse_common_daily) was developed against firmware v2.1.0_beta_v3
//   and falls through for our v2.2.0.24 payloads — the headers don't
//   match. Our parser is hand-rolled from captured bytes only.
//
// ─── health/activity/daily (multi-frame, 35 B + 44 B per frame) ────────────
//   Header (7 B per frame):
//     [0]      pageMarker — varies per frame in same response (0x04 for
//              the overview, 0x10 for the data frame in our captures).
//              NOT a record count.
//     [1..2]   reserved (00 00)
//     [3..6]   base_ts u32 LE — zero for the overview frame, today's
//              midnight UTC for the data frame
//   Records (7 B each, ✓ matches python codec _parse_activity_item):
//     [0]      slot_index — 10-minute bin since base_ts.
//              wall_time = base_ts + slot_index * 600 sec.
//     [1..2]   steps i16 LE
//     [3..4]   UNKNOWN — small values (00..08), maybe duration or zone
//     [5..6]   kcal i16 LE
//   The final record may be truncated mid-bytes if the response exceeds
//   one MTU; the next notify continues the data.
//
//   Verified live captures (slot 54 = 9:00 UTC bin = 5:00 AM EDT):
//     [36 61 00 0B 00 17 00] → slot=54, steps=97, kcal=23
//
// ─── health/sleep/daily (no payload observed) ──────────────────────────────
//   Returns ack-only on our test ring (no recorded sleep session yet). The
//   python codec _parse_sleep expects:
//     [0]      sleep_type_code
//     [1..7]   field_1..7 (per-byte, semantics unknown)
//     [8..29]  several u16/u32 fields
//     [30..31] stage_count u16 LE
//     [32..]   stage_count × {stage_type_code, duration_minutes_u16_LE}

// =============================================================================
// SECTION 7: Bridge sequence (ring ↔ glasses ↔ ESP)
// =============================================================================
//
// FULL SEQUENCE + IMPLEMENTATION GUIDE: see Ring_Bridge_Sequence.h in this
// directory.
//
// Big picture:
//   PHASE 1 — Glasses startup
//   PHASE 2 — Ring direct setup (pairAuth → systemTime → advStart). pairAuth
//             with payload [0x01] is sufficient — no pkey required.
//   PHASE 3 — Send glasses.RING_CONNECT_INFO (sid=128 cmd=6) with the ring's
//             MAC **REVERSED** and the ring name as raw UTF-8 bytes
//   PHASE 4 — Receive ring data on sid=144 / sid=145
//   PHASE 5 — Heartbeats: G2 base heartbeat (30s) and R1 heartbeatPack 0x7F
//
// **Time-zone encoding gotcha** (still applies):
//   - G2 TIME_SYNC uses QUARTER-HOURS (minutes / 15)
//   - R1 systemTime uses RAW MINUTES
//
// **MAC byte-reversal gotcha** (still applies):
//   - Phase 3 (RING_CONNECT_INFO to glasses): MAC IS REVERSED
//   - Phase 2 advStart to ring: MAC also REVERSED (LSB first)

#endif  // R1_RE_REFERENCE_H
