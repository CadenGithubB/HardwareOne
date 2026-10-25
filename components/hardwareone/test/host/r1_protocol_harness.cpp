#include "System_R1_Protocol.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#ifdef NDEBUG
#error "Host checks require assertions"
#endif

int main() {
  assert(r1ProtocolSelfTest()); // all existing codec golden/malformed fixtures

  // Exact identities only: every recognised string selects its own profile,
  // and its name maps back to it (a missed name case would break the round trip).
  struct Known { const char* firmware; R1ProtocolProfile profile; };
  const Known known[] = {{"2.2.7.0005", R1_PROFILE_FW_2_2_7_0005},
                         {"2.2.9.0003", R1_PROFILE_FW_2_2_9_0003},
                         {"2.3.2.0007", R1_PROFILE_FW_2_3_2_0007},
                         {"2.3.2.0008", R1_PROFILE_FW_2_3_2_0008}};
  for (const Known& k : known) {
    assert(r1ProfileForFirmware(k.firmware) == k.profile);
    assert(!strcmp(r1ProtocolProfileName(k.profile), k.firmware));
    assert(r1ProfileForFirmware(r1ProtocolProfileName(k.profile)) == k.profile);
  }
  assert(R1_PROFILE_FW_2_3_2_0008 != R1_PROFILE_FW_2_3_2_0007);
  for (const char* version : {"2.3.2.000", "2.3.2.00070", "2.3.2.0006", "2.3.2.0009",
                              "2.3.2.00080", "2.3.2.008", "2.3.2.0007 ", "2.3.2.0008 ",
                              "v2.3.2.0007", "v2.3.2.0008", ""})
    assert(r1ProfileForFirmware(version) == R1_PROFILE_UNKNOWN);
  assert(r1ProfileForFirmware(nullptr) == R1_PROFILE_UNKNOWN);

  // deviceInfo as a 2.3.2.0008 ring sends it (ack/set/ok, two NUL-padded
  // 16-byte strings) selects that profile; any other length is refused.
  {
    R1Encoder ring;
    uint8_t payload[33] = {};
    memcpy(payload, "2.3.2.0008", sizeof("2.3.2.0008"));
    memcpy(payload + 16, "603MV1.9.3", sizeof("603MV1.9.3"));
    const R1Frame frame = ring.build(R1_MODULE_SYSTEM, R1_CMD_SYSTEM, R1_SUB_DEVICE_INFO,
                                     R1_STATUS_TYPE_ACK, R1_STATUS_METHOD_SET, R1_STATUS_ACK_OK,
                                     payload, 32);
    R1Decoded decoded{};
    assert(frame.length && r1Decode(frame.bytes, frame.length, decoded) && decoded.crc32Valid);
    R1DeviceInfo info{};
    assert(r1ParseDeviceInfo(decoded, info) == R1_PARSE_OK);
    assert(info.profile == R1_PROFILE_FW_2_3_2_0008);
    assert(!strcmp(info.firmware, "2.3.2.0008") && !strcmp(info.hardware, "603MV1.9.3"));
    const R1Frame longer = ring.build(R1_MODULE_SYSTEM, R1_CMD_SYSTEM, R1_SUB_DEVICE_INFO,
                                      R1_STATUS_TYPE_ACK, R1_STATUS_METHOD_SET, R1_STATUS_ACK_OK,
                                      payload, 33);
    assert(longer.length && r1Decode(longer.bytes, longer.length, decoded));
    assert(r1ParseDeviceInfo(decoded, info) == R1_PARSE_LENGTH);
  }

  // Both 2.3.2 profiles admit connection setup with the same advStart bytes,
  // plus exactly the DAILY history subset their stock-app captures prove
  // (docs/R1_HEALTH_2_3_2_RESEARCH.md). Nothing else is inherited.
  const uint8_t right[6] = {2,0,0,0,0,0xA1}, left[6] = {2,0,0,0,0,0xB2};
  const uint8_t expected[] = {
    0,0xC5,0xDB,0xF8,0xC7,0x64,1,0x64,1,0,0,0,0x0A,0x18,0,0xC5,0xDA,
    0xA1,0,0,0,0,2,0xB2,0,0,0,0,2
  };
  const auto dataCmd = [](unsigned cmd) {
    return cmd == R1_CMD_HEARTRATE || cmd == R1_CMD_HRV || cmd == R1_CMD_SPO2 ||
           cmd == R1_CMD_ACTIVITY;
  };
  for (const R1ProtocolProfile profile : {R1_PROFILE_FW_2_3_2_0007, R1_PROFILE_FW_2_3_2_0008}) {
    R1Encoder encoder;
    assert(r1ProfileSupportsAdvStart(profile));
    assert(encoder.buildAdvStart(profile,nullptr,left).length == 0);
    const auto adv = encoder.buildAdvStart(profile,right,left);
    assert(adv.serial == 1 && adv.length == sizeof(expected));
    assert(!memcmp(adv.bytes,expected,sizeof(expected)));
    assert(r1ProfileSupportsHealthPageRefresh(profile));
    assert(r1ProfileSupportsActivityReassembly(profile));
    assert(r1ProfileSupportsDeviceStatusIngestion(profile));
    assert(r1ProfileSupportsHealthCollectionSet(profile,true));
    assert(!r1ProfileSupportsHealthCollectionSet(profile,false));
    assert(!r1ProfileSupportsDeviceStatusWear(profile));
    assert(!r1ProfileSupportsPointIngestion(profile));
    assert(!r1ProfileSupportsPointMeasureQuery(profile));
    assert(!r1ProfileSupportsLowPower(profile));
    assert(!r1ProfileSupportsUserInfo(profile));
    // Wear: the ring's own push, plus one empty GET after setup (build 243's
    // getWearStatus). Sleep: typed records, acknowledged once stored.
    assert(r1ProfileSupportsWearStatus(profile));
    assert(r1ProfileSupportsWearStatusIngestion(profile));
    assert(r1ProfileSupportsWearQueryOnConnect(profile));
    const R1Frame wearQuery = encoder.buildWearStatusQuery(profile);
    R1Decoded wq{};
    assert(wearQuery.length == 17 && r1Decode(wearQuery.bytes, wearQuery.length, wq) &&
           wq.subCmd == R1_SUB_WEAR_STATUS && wq.statusType == R1_STATUS_TYPE_NOTIFY &&
           wq.statusMethod == R1_STATUS_METHOD_GET && wq.payloadLength == 0);
    assert(!r1ProfileSupportsSleepDataIngestion(profile));
    assert(r1ProfileSupportsSleepRecords(profile));
    assert(r1ProfileSupportsChargeStatus(profile));
    assert(r1ProfileSupportsDeviceStatusPoll(profile));
    for (unsigned cmd=0;cmd<256;++cmd) {
      assert(r1ProfileSupportsSingleFrameDaily(profile,cmd) == dataCmd(cmd));
      assert(r1ProfileSupportsDailyPacketAck(profile,cmd) ==
             (dataCmd(cmd) || cmd == R1_CMD_SLEEP));
      for (unsigned sub=0;sub<256;++sub)
        assert(r1ProfileSupportsHealthQuery(profile,cmd,sub) ==
               (sub == R1_SUB_DAILY && (dataCmd(cmd) || cmd == R1_CMD_SLEEP)));
    }

    // DAILY request: empty notify/get/ok. On 2.3.x sub 0x02 is forceAwake
    // (sleep) and allDayActivity (activity), so POINT never builds.
    const R1Frame hr = encoder.buildHealthQuery(profile, R1_CMD_HEARTRATE, R1_SUB_DAILY);
    R1Decoded req{};
    assert(hr.length == 17 && r1Decode(hr.bytes, hr.length, req) && r1DecodedIsTrusted(req));
    assert(req.module == R1_MODULE_HEALTH && req.cmd == R1_CMD_HEARTRATE &&
           req.subCmd == R1_SUB_DAILY && req.statusType == R1_STATUS_TYPE_NOTIFY &&
           req.statusMethod == R1_STATUS_METHOD_GET && req.payloadLength == 0);
    for (const uint8_t cmd : {R1_CMD_SLEEP, R1_CMD_ACTIVITY, R1_CMD_HEARTRATE})
      for (const uint8_t sub : {R1_SUB_POINT, R1_SUB_MEASURE})
        assert(encoder.buildHealthQuery(profile, cmd, sub).length == 0);

    // Health collection ON: epoch, enable=1, seven reserved zeros (captured).
    const R1Frame on = encoder.buildHealthCollectionSet(profile, 0x6A000000u, true);
    R1Decoded set{};
    assert(on.length && r1Decode(on.bytes, on.length, set) && set.payloadLength == 12);
    assert(set.subCmd == R1_SUB_HEALTH_SETTINGS && set.statusMethod == R1_STATUS_METHOD_SET &&
           set.payload[4] == 1);
    assert(encoder.buildHealthCollectionSet(profile, 0x6A000000u, false).length == 0);
  }

  // Synthetic DAILY pages in the captured 2.3.2 shapes (no capture values).
  {
    R1Encoder ring;
    const uint32_t day = 1767240000u;  // 2026-01-01 local midnight at UTC-4
    const auto u16 = [](uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); };
    const auto u32 = [](uint8_t* p, uint32_t v) {
      for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
    };
    const auto page = [&](uint8_t cmd, const uint8_t* payload, size_t length, R1Decoded& out) {
      const R1Frame f = ring.build(R1_MODULE_HEALTH, cmd, R1_SUB_DAILY, R1_STATUS_TYPE_NOTIFY,
                                   R1_STATUS_METHOD_SET, R1_STATUS_ACK_OK, payload, length);
      assert(f.length && r1Decode(f.bytes, f.length, out) && r1DecodedIsTrusted(out));
    };
    for (const R1ProtocolProfile profile : {R1_PROFILE_FW_2_3_2_0007, R1_PROFILE_FW_2_3_2_0008}) {
      // HR: 0008 backlog page, dated day, latest is the ring-wide newest sample
      // on a later day, trailer is a small uptime-domain constant.
      uint8_t hrPage[16 + 2 * 4] = {2};
      u16(hrPage + 1, (uint16_t)(int16_t)-240);
      u32(hrPage + 3, day);
      u32(hrPage + 7, day + 3 * 86400u + 40000u);
      hrPage[11] = 71;
      const uint8_t records[] = {9, 70, 80, 60, 10, 72, 90, 65};
      memcpy(hrPage + 12, records, sizeof(records));
      u32(hrPage + 20, 9000u);
      R1Decoded d{};
      page(R1_CMD_HEARTRATE, hrPage, sizeof(hrPage), d);
      R1CommonDailyResult hr{};
      assert(r1ParseCommonDaily(profile, d, hr) == R1_PARSE_OK);
      assert(hr.dayMode == R1_DAILY_DAY_EPOCH && hr.count == 2 && hr.trailer == 9000u);
      assert(hr.latestTimestampMode == R1_DAILY_TIMESTAMP_EPOCH &&
             hr.latestTimestamp == day + 3 * 86400u + 40000u);
      assert(hr.records[1].bucketEpoch == day + 10 * 3600u);
      R1PacketAckDescriptor ack;
      assert(r1PacketAckDescriptorFromDecoded(profile, d, ack));
      const R1Frame ackFrame = ring.buildPacketAck(profile, ack);
      R1Decoded ackDecoded{};
      assert(ackFrame.length && r1Decode(ackFrame.bytes, ackFrame.length, ackDecoded));
      const uint8_t ackPayload[10] = {R1_MODULE_HEALTH, R1_CMD_HEARTRATE, R1_SUB_DAILY, 0,
                                      (uint8_t)d.serial, (uint8_t)(d.serial >> 8), 0, 0, 0, 0};
      assert(ackDecoded.subCmd == R1_SUB_PACKET_ACK && ackDecoded.statusType == R1_STATUS_TYPE_ACK &&
             ackDecoded.statusMethod == R1_STATUS_METHOD_GET && ackDecoded.payloadLength == 10 &&
             !memcmp(ackDecoded.payload, ackPayload, 10));

      // 0007 shape: the latest stamp predates the ring's time sync (1970
      // range), so it stays unanchored; the dated day still anchors buckets.
      u32(hrPage + 7, 812000u);
      page(R1_CMD_HEARTRATE, hrPage, sizeof(hrPage), d);
      assert(r1ParseCommonDaily(profile, d, hr) == R1_PARSE_OK);
      assert(hr.latestTimestampMode == R1_DAILY_TIMESTAMP_UNKNOWN && hr.latestTimestamp == 0 &&
             hr.records[0].bucketEpoch == day + 9 * 3600u);

      // Soon after a ring reset the uptime-domain latest is below one day.
      // On 2.3.2 it still stays unanchored (never dayStart + raw).
      for (const uint32_t raw : {40000u, 86399u}) {
        u32(hrPage + 7, raw);
        page(R1_CMD_HEARTRATE, hrPage, sizeof(hrPage), d);
        assert(r1ParseCommonDaily(profile, d, hr) == R1_PARSE_OK);
        assert(hr.latestTimestampMode == R1_DAILY_TIMESTAMP_UNKNOWN && hr.latestTimestamp == 0 &&
               hr.records[0].bucketEpoch == day + 9 * 3600u);
      }

      // 0007 zero-base page (recorded before the day was known).
      u16(hrPage + 1, 0);
      u32(hrPage + 3, 0);
      page(R1_CMD_HEARTRATE, hrPage, sizeof(hrPage), d);
      assert(r1ParseCommonDaily(profile, d, hr) == R1_PARSE_OK);
      assert(hr.dayMode == R1_DAILY_DAY_ZERO_BASE && hr.records[0].bucketEpoch == 0);

      // HRV: 17 + 7n with 16-bit latest and records.
      uint8_t hrvPage[17 + 7] = {1};
      u16(hrvPage + 1, (uint16_t)(int16_t)-240);
      u32(hrvPage + 3, day);
      u32(hrvPage + 7, day + 11 * 3600u);
      u16(hrvPage + 11, 300);
      hrvPage[13] = 11;
      u16(hrvPage + 14, 280);
      u16(hrvPage + 16, 320);
      u16(hrvPage + 18, 250);
      u32(hrvPage + 20, 9000u);
      page(R1_CMD_HRV, hrvPage, sizeof(hrvPage), d);
      R1HrvDailyResult hrv{};
      assert(r1ParseHrvDaily(profile, d, hrv) == R1_PARSE_OK && hrv.count == 1 &&
             hrv.latestValue == 300 && hrv.records[0].maximum == 320);

      // A sleep record may be acknowledged on 2.3.2 (once the history model
      // has stored it; that decision is the RX path's, see ring_daily_ack).
      const uint8_t sleepPage[36] = {2};
      page(R1_CMD_SLEEP, sleepPage, sizeof(sleepPage), d);
      assert(r1PacketAckDescriptorFromDecoded(profile, d, ack) &&
             ack.cmd() == R1_CMD_SLEEP && ack.subCmd() == R1_SUB_DAILY);

      // A full activity day arrives as a 3-part frame; the reassembled model
      // (92 ten-minute records, as captured) parses under both profiles.
      static uint8_t model[R1_ACTIVITY_REASSEMBLED_MODEL_MAX];
      const size_t count = 92, payloadLength = 11 + count * 7, modelLength = 12 + payloadLength;
      memset(model, 0, sizeof(model));
      model[0] = 0x64; model[1] = R1_MODULE_HEALTH; model[2] = 0x64;
      u16(model + 3, 24);
      model[5] = 0x02;  // notify/set/ok
      model[6] = R1_CMD_ACTIVITY; model[7] = R1_SUB_DAILY;
      u16(model + 8, (uint16_t)modelLength);
      uint8_t* p = model + 12;
      p[0] = (uint8_t)count;
      u16(p + 1, (uint16_t)(int16_t)-240);
      u32(p + 3, day);
      for (size_t i = 0; i < count; ++i) {
        uint8_t* r = p + 7 + i * 7;
        r[0] = (uint8_t)(30 + i);
        u16(r + 1, (uint16_t)(10 + i));
        u16(r + 3, 2);
        u16(r + 5, 9);
      }
      u32(p + 7 + count * 7, 9000u);
      static R1ActivityDailyResult activity;
      assert(r1ParseReassembledActivityDaily(profile, model, modelLength,
                                             r1Crc32(model, modelLength), activity) == R1_PARSE_OK);
      assert(activity.count == count && activity.dayMode == R1_DAILY_DAY_EPOCH &&
             activity.records[count - 1].bucketEpoch == day + (30 + count - 1) * 600u);
      assert(r1MakeDailyPacketAckDescriptor(profile, 24, R1_CMD_ACTIVITY, ack));
    }
  }

  // Sleep records in the captured 2.3.2 shape (synthetic values).
  {
    R1Encoder ring;
    const uint32_t day = 1767240000u;  // local midnight at UTC-4
    const auto u16 = [](uint8_t* p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); };
    const auto u32 = [](uint8_t* p, uint32_t v) {
      for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(v >> (8 * i));
    };
    struct Stage { uint8_t type; uint16_t epochs; };
    // Builds a record payload; returns its length.
    const auto record = [&](uint8_t* out, uint8_t type, int16_t tz, uint32_t start,
                            uint32_t end, const Stage* stages, uint16_t n) {
      memset(out, 0, 32);
      out[0] = type;
      out[1] = 90;  // unnamed summary bytes are carried, never interpreted
      u16(out + 10, (uint16_t)tz);
      u32(out + 12, start);
      u32(out + 16, end);
      u16(out + 30, n);
      for (uint16_t i = 0; i < n; ++i) {
        out[32 + i * 3] = stages[i].type;
        u16(out + 33 + i * 3, stages[i].epochs);
      }
      u32(out + 32 + n * 3, 9000u);
      return (size_t)(36 + n * 3);
    };
    const auto decode = [&](const uint8_t* payload, size_t length, R1Decoded& out) {
      const R1Frame f = ring.build(R1_MODULE_HEALTH, R1_CMD_SLEEP, R1_SUB_DAILY,
                                   R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_SET,
                                   R1_STATUS_ACK_OK, payload, length);
      assert(f.length && r1Decode(f.bytes, f.length, out) && r1DecodedIsTrusted(out));
    };
    // 23:00 -> 06:00 local: 10 + 400 + 200 + 230 epochs x 30 s = 7 h.
    const Stage night[] = {{0, 10}, {2, 400}, {3, 200}, {1, 230}};
    const uint32_t start = day - 3600u, end = day + 21600u;
    uint8_t payload[256];
    R1Decoded d{};
    R1SleepDailyResult sleep{};
    for (const R1ProtocolProfile profile : {R1_PROFILE_FW_2_3_2_0007, R1_PROFILE_FW_2_3_2_0008}) {
      size_t n = record(payload, R1_SLEEP_TYPE_LONG, -240, start, end, night, 4);
      decode(payload, n, d);
      assert(r1ParseSleepDaily(profile, d, sleep) == R1_PARSE_OK);
      assert(sleep.type == R1_SLEEP_TYPE_LONG && sleep.timezoneMinutes == -240 &&
             sleep.startEpoch == start && sleep.endEpoch == end && sleep.anchored &&
             sleep.stageCount == 4 && sleep.trailer == 9000u &&
             sleep.stageEpochs[R1_SLEEP_STAGE_AWAKE] == 10 &&
             sleep.stageEpochs[R1_SLEEP_STAGE_LIGHT] == 400 &&
             sleep.stageEpochs[R1_SLEEP_STAGE_DEEP] == 200 &&
             sleep.stageEpochs[R1_SLEEP_STAGE_REM] == 230);
      char note[160];
      assert(r1AnnotatePayload(profile, d, note, sizeof(note)) > 0 &&
             strstr(note, "sleep type=1") && strstr(note, "anchored"));

      // A short period: no stages.
      n = record(payload, R1_SLEEP_TYPE_SHORT, -240, day + 50400u, day + 52200u, nullptr, 0);
      decode(payload, n, d);
      assert(r1ParseSleepDaily(profile, d, sleep) == R1_PARSE_OK &&
             sleep.type == R1_SLEEP_TYPE_SHORT && sleep.stageCount == 0);

      // Stages that do not account for the period, a bad stage code, an
      // unknown type, a wrong length and an inverted period all fail closed.
      const Stage shortNight[] = {{0, 10}, {2, 400}, {3, 200}, {1, 229}};
      n = record(payload, R1_SLEEP_TYPE_LONG, -240, start, end, shortNight, 4);
      decode(payload, n, d);
      assert(r1ParseSleepDaily(profile, d, sleep) == R1_PARSE_VALUE_RANGE);
      const Stage badStage[] = {{4, 840}};
      n = record(payload, R1_SLEEP_TYPE_LONG, -240, start, end, badStage, 1);
      decode(payload, n, d);
      assert(r1ParseSleepDaily(profile, d, sleep) == R1_PARSE_VALUE_RANGE);
      n = record(payload, 3, -240, start, end, night, 4);
      decode(payload, n, d);
      assert(r1ParseSleepDaily(profile, d, sleep) == R1_PARSE_UNSUPPORTED_LAYOUT);
      n = record(payload, R1_SLEEP_TYPE_LONG, -240, start, end, night, 4);
      decode(payload, n - 1, d);
      assert(r1ParseSleepDaily(profile, d, sleep) == R1_PARSE_LENGTH);
      n = record(payload, R1_SLEEP_TYPE_SHORT, -240, end, start, nullptr, 0);
      decode(payload, n, d);
      assert(r1ParseSleepDaily(profile, d, sleep) == R1_PARSE_VALUE_RANGE);

      // Recorded before the ring knew the wall time: parses, never anchored.
      n = record(payload, R1_SLEEP_TYPE_LONG, 0, 200000u, 225200u, night, 4);
      decode(payload, n, d);
      assert(r1ParseSleepDaily(profile, d, sleep) == R1_PARSE_OK && !sleep.anchored);

      // A long night split across notifications reaches the same parser
      // through the reassembler: 80 stages, 12 + 36 + 240 model bytes.
      static uint8_t model[R1_ACTIVITY_REASSEMBLED_MODEL_MAX];
      Stage many[80];
      for (int i = 0; i < 80; ++i) many[i] = {(uint8_t)(i % 4), 10};
      memset(model, 0, sizeof(model));
      const size_t payloadLength = record(model + 12, R1_SLEEP_TYPE_LONG, -240,
                                          day - 3600u, day - 3600u + 80u * 10u * 30u,
                                          many, 80);
      const size_t modelLength = 12 + payloadLength;
      model[0] = 0x64; model[1] = R1_MODULE_HEALTH; model[2] = 0x64;
      u16(model + 3, 31);
      model[5] = 0x02;  // notify/set/ok
      model[6] = R1_CMD_SLEEP; model[7] = R1_SUB_DAILY;
      u16(model + 8, (uint16_t)modelLength);
      assert(r1ParseReassembledSleepDaily(profile, model, modelLength,
                                          r1Crc32(model, modelLength), sleep) == R1_PARSE_OK);
      assert(sleep.stageCount == 80 && sleep.sourceSerial == 31 &&
             sleep.stageEpochs[R1_SLEEP_STAGE_DEEP] == 200);
      assert(r1ParseReassembledSleepDaily(profile, model, modelLength,
                                          r1Crc32(model, modelLength) ^ 1u, sleep) ==
             R1_PARSE_BAD_CRC);
    }
    // Older firmware: no typed sleep, no sleep ACK, no charge status.
    size_t n = record(payload, R1_SLEEP_TYPE_LONG, -240, start, end, night, 4);
    decode(payload, n, d);
    for (const R1ProtocolProfile profile : {R1_PROFILE_FW_2_2_7_0005, R1_PROFILE_FW_2_2_9_0003,
                                            R1_PROFILE_UNKNOWN}) {
      assert(!r1ProfileSupportsSleepRecords(profile));
      assert(!r1ProfileSupportsChargeStatus(profile));
      assert(!r1ProfileSupportsWearQueryOnConnect(profile));
      assert(!r1ProfileSupportsDeviceStatusPoll(profile));
      assert(r1ParseSleepDaily(profile, d, sleep) == R1_PARSE_WRONG_PROFILE);
    }
    assert(!r1ProfileSupportsDailyPacketAck(R1_PROFILE_FW_2_2_9_0003, R1_CMD_SLEEP));

    // 2.2.9 keeps its captured seconds-within-day reading of a sub-day latest.
    uint8_t hrPage[16 + 4] = {1};
    u16(hrPage + 1, (uint16_t)(int16_t)-240);
    u32(hrPage + 3, day);
    u32(hrPage + 7, 40000u);
    hrPage[11] = 70;
    const uint8_t rec[] = {9, 70, 80, 60};
    memcpy(hrPage + 12, rec, sizeof(rec));
    u32(hrPage + 16, 9000u);
    const R1Frame f = ring.build(R1_MODULE_HEALTH, R1_CMD_HEARTRATE, R1_SUB_DAILY,
                                 R1_STATUS_TYPE_NOTIFY, R1_STATUS_METHOD_SET,
                                 R1_STATUS_ACK_OK, hrPage, sizeof(hrPage));
    assert(f.length && r1Decode(f.bytes, f.length, d));
    R1CommonDailyResult hr{};
    assert(r1ParseCommonDaily(R1_PROFILE_FW_2_2_9_0003, d, hr) == R1_PARSE_OK &&
           hr.latestTimestampMode == R1_DAILY_TIMESTAMP_SECONDS_WITHIN_DAY &&
           hr.latestTimestamp == day + 40000u);
  }

  // The older wear reading of deviceStatus byte 1 stays with 2.2.x only.
  assert(r1ProfileSupportsDeviceStatusWear(R1_PROFILE_FW_2_2_7_0005));
  assert(r1ProfileSupportsDeviceStatusWear(R1_PROFILE_FW_2_2_9_0003));
  assert(!r1ProfileSupportsDeviceStatusWear(R1_PROFILE_UNKNOWN));
  // Ring-pushed wearStatus: 2.2.7 (with its query) and 2.3.2 only.
  assert(r1ProfileSupportsWearStatusIngestion(R1_PROFILE_FW_2_2_7_0005));
  assert(!r1ProfileSupportsWearStatusIngestion(R1_PROFILE_FW_2_2_9_0003));
  assert(!r1ProfileSupportsWearStatusIngestion(R1_PROFILE_UNKNOWN));
  assert(!r1ProfileSupportsAdvStart(R1_PROFILE_UNKNOWN));
  puts("R1 codec fixtures, exact 2.3.2.0007/2.3.2.0008 identities, DAILY history admission and setup checks passed");
}
