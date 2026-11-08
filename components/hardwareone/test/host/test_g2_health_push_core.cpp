// Host tests for G2_HealthPushCore.h: the sid 0x0E stock Health panel feed.
//
// The expected payloads were produced by an encoder that reproduces the Even
// app's own sid 0x0E frames byte for byte (app 2.2.7 -> stock G2 2.2.6.10).
// Synthetic values are used here so no real capture data is in the repo.
#include "../../G2_HealthPushCore.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace hw1_g2_health_push;

static int gFailures = 0;
#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                   #cond);                                             \
      gFailures++;                                                     \
    }                                                                  \
  } while (0)

static std::vector<uint8_t> hex(const char* h) {
  std::vector<uint8_t> out;
  for (size_t i = 0; h[i] && h[i + 1]; i += 2) {
    unsigned v = 0;
    std::sscanf(h + i, "%2x", &v);
    out.push_back((uint8_t)v);
  }
  return out;
}

static std::vector<uint8_t> encode(const Snapshot& s, uint8_t magic) {
  uint8_t buf[kMaxPbBytes];
  const size_t n = encodeSnapshot(s, magic, buf, sizeof buf);
  return std::vector<uint8_t>(buf, buf + n);
}

static Snapshot sample() {
  Input in;
  in.steps = 1234;
  in.stepGoal = 10000;
  in.kcal = 210;
  in.kcalGoal = 300;
  in.sleepSeconds = 27000;
  in.heartRate = 72;
  in.heartRateAvg = 68;
  in.spo2 = 98;
  in.spo2Avg = 97;
  return sanitize(in);
}

static const char* kSampleHex =
    "0802102a228c0108011215080210904e1d00409a4425000000002800300038001215"
    "080310ac021d0000524325000000002800300038001216080410001d000000002500"
    "00000028f8d201300038001214080510001d0000904225000088422800300038001214"
    "080610001d0000c442250000c2422800300038001214080910001d00000000250000"
    "00002800300038001800";

static const char* kEmptyHex =
    "08021001228a0108011215080210904e1d0000000025000000002800300038001215"
    "080310ac021d0000000025000000002800300038001214080410001d000000002500"
    "00000028003000380012140805100"
    "01d0000000025000000002800300038001214080610001d00000000250000000028"
    "00300038001214080910001d0000000025000000002800300038001800";

static void testEncodingMatchesApp() {
  CHECK(encode(sample(), 0x2a) == hex(kSampleHex));
  CHECK(encode(sanitize(Input{}), 0x01) == hex(kEmptyHex));
  CHECK(hex(kSampleHex).size() == 147);
  CHECK(hex(kEmptyHex).size() == 145);
}

static void testSanitize() {
  Input in;
  in.steps = 250000;       // over max -> 0
  in.stepGoal = 5;         // under min -> default
  in.kcal = -3;            // unknown -> 0
  in.kcalGoal = 99999;     // over max -> default
  in.sleepScore = 101;     // over max -> 0
  in.sleepSeconds = 90000; // over a day -> 0
  in.heartRate = 29;       // implausible -> 0
  in.heartRateAvg = 221;
  in.spo2 = 69;
  in.spo2Avg = 101;
  const Snapshot s = sanitize(in);
  CHECK(s.steps == 0 && s.stepGoal == kStepGoalDefault);
  CHECK(s.kcal == 0 && s.kcalGoal == kKcalGoalDefault);
  CHECK(s.sleepScore == 0 && s.sleepSeconds == 0);
  CHECK(s.heartRate == 0 && s.heartRateAvg == 0);
  CHECK(s.spo2 == 0 && s.spo2Avg == 0);

  Input edge;
  edge.steps = (int32_t)kStepsMax;
  edge.heartRate = (int32_t)kHeartRateMin;
  edge.heartRateAvg = (int32_t)kHeartRateMax;
  edge.spo2 = (int32_t)kSpo2Max;
  edge.sleepSeconds = (int32_t)kSleepSecondsMax;
  const Snapshot e = sanitize(edge);
  CHECK(e.steps == kStepsMax && e.heartRate == kHeartRateMin &&
        e.heartRateAvg == kHeartRateMax && e.spo2 == kSpo2Max &&
        e.sleepSeconds == kSleepSecondsMax);
  uint8_t magic = 0;
  Snapshot back;
  const auto pb = encode(e, 7);
  CHECK(validateSnapshotPb(pb.data(), pb.size(), &magic, &back));
  CHECK(magic == 7 && back == e);
}

static void testValidatorRoundTrip() {
  const auto pb = encode(sample(), 0x2a);
  uint8_t magic = 0;
  Snapshot back;
  CHECK(validateSnapshotPb(pb.data(), pb.size(), &magic, &back));
  CHECK(magic == 0x2a);
  CHECK(back == sample());
}

static void writeFloatAt(std::vector<uint8_t>& pb, size_t off, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof bits);
  for (int i = 0; i < 4; i++) pb[off + i] = (uint8_t)(bits >> (8 * i));
}

static size_t findFloatTag(const std::vector<uint8_t>& pb, uint8_t type,
                           uint8_t floatField) {
  // Entry layout: 12 len 08 <type> 10 <goal...> 1d f32 25 f32 ...
  for (size_t i = 0; i + 3 < pb.size(); i++) {
    if (pb[i] == 0x12 && pb[i + 2] == 0x08 && pb[i + 3] == type) {
      for (size_t j = i + 4; j < pb.size(); j++)
        if (pb[j] == (uint8_t)((floatField << 3) | 5)) return j + 1;
    }
  }
  return 0;
}

static void testValidatorRejects() {
  const auto good = encode(sample(), 0x2a);
  CHECK(validateSnapshotPb(good.data(), good.size()));
  CHECK(!validateSnapshotPb(nullptr, 0));
  CHECK(!validateSnapshotPb(good.data(), 0));

  // Truncated anywhere.
  for (size_t n = 1; n < good.size(); n++)
    CHECK(!validateSnapshotPb(good.data(), n));

  // Trailing garbage.
  auto longer = good;
  longer.push_back(0x00);
  CHECK(!validateSnapshotPb(longer.data(), longer.size()));

  // Every single-byte corruption must either be rejected or still decode to
  // an in-range snapshot (the validator is the last gate before the radio).
  for (size_t i = 0; i < good.size(); i++) {
    for (int delta : {1, 0x40, 0x80}) {
      auto bad = good;
      bad[i] = (uint8_t)(bad[i] + delta);
      Snapshot s;
      if (validateSnapshotPb(bad.data(), bad.size(), nullptr, &s))
        CHECK(sanitize(Input{(int32_t)s.steps, (int32_t)s.stepGoal,
                             (int32_t)s.kcal, (int32_t)s.kcalGoal,
                             (int32_t)s.sleepScore, (int32_t)s.sleepSeconds,
                             (int32_t)s.heartRate, (int32_t)s.heartRateAvg,
                             (int32_t)s.spo2, (int32_t)s.spo2Avg}) == s);
    }
  }

  // Wrong command, outer type, error code.
  auto badCmd = good;
  badCmd[1] = 0x01;
  CHECK(!validateSnapshotPb(badCmd.data(), badCmd.size()));

  // NaN, infinity, negative, fractional and out-of-range floats.
  const size_t hrValue = findFloatTag(good, kHeartRate, 3);
  CHECK(hrValue != 0);
  for (float f : {NAN, INFINITY, -1.0f, 72.5f, 29.0f, 221.0f, 1e9f}) {
    auto bad = good;
    writeFloatAt(bad, hrValue, f);
    CHECK(!validateSnapshotPb(bad.data(), bad.size()));
  }
  const size_t spo2Avg = findFloatTag(good, kBloodOxygen, 4);
  CHECK(spo2Avg != 0);
  auto badSpo2 = good;
  writeFloatAt(badSpo2, spo2Avg, 100.5f);
  CHECK(!validateSnapshotPb(badSpo2.data(), badSpo2.size()));
  const size_t prodValue = findFloatTag(good, kProductivity, 3);
  CHECK(prodValue != 0);
  auto badProd = good;
  writeFloatAt(badProd, prodValue, 80.0f);
  CHECK(!validateSnapshotPb(badProd.data(), badProd.size()));

  // Entry order swapped (steps <-> calories type bytes).
  auto swapped = good;
  for (size_t i = 0; i + 3 < swapped.size(); i++) {
    if (swapped[i] == 0x12 && swapped[i + 2] == 0x08 && swapped[i + 3] == kSteps) {
      swapped[i + 3] = kCalories;
      break;
    }
  }
  CHECK(!validateSnapshotPb(swapped.data(), swapped.size()));

  // Too large to be ours.
  std::vector<uint8_t> huge(kMaxPbBytes + 1, 0);
  CHECK(!validateSnapshotPb(huge.data(), huge.size()));

  // Encoder refuses a too-small buffer instead of truncating.
  uint8_t tiny[40];
  CHECK(encodeSnapshot(sample(), 1, tiny, sizeof tiny) == 0);
}

static void testAck() {
  // Shape of the glasses' MULT_DATA echo: {1 2, 2 magic, 4 {1 1}}.
  const auto ack = hex("0802102a22020801");
  uint8_t cmd = 0, magic = 0;
  CHECK(parseAck(ack.data(), ack.size(), cmd, magic));
  CHECK(cmd == kCmdMultData && magic == 0x2a);
  CHECK(!parseAck(ack.data(), 3, cmd, magic));
  CHECK(nextMagic(0) == 1 && nextMagic(1) == 2 && nextMagic(0xFF) == 1);
}

static void testPolicy() {
  Policy p;
  const Snapshot a = sample();
  Snapshot b = a;
  b.steps += 100;
  uint32_t t = 1000;

  // Not ready / disabled: never send.
  CHECK(policyTick(p, true, false, 5, t, a) == Action::None);
  CHECK(policyTick(p, false, true, 5, t, a) == Action::None);
  // Settle window after ready.
  CHECK(policyTick(p, true, true, 5, t + 100, a) == Action::None);
  CHECK(policyTick(p, true, true, 5, t + kConnectSettleMs, a) == Action::Send);
  policySent(p, t + kConnectSettleMs, 9);
  t += kConnectSettleMs;

  // Waiting for ack: nothing; ack for another magic is ignored.
  CHECK(policyTick(p, true, true, 5, t + 10, a) == Action::None);
  CHECK(!policyAck(p, kCmdMultData, 8));
  CHECK(policyAck(p, kCmdMultData, 9));
  // Unchanged data: never re-sent, even after the interval.
  CHECK(policyTick(p, true, true, 5, t + kMinIntervalMs + 1, a) == Action::None);
  // Changed data: only after the minimum interval.
  CHECK(policyTick(p, true, true, 5, t + 1000, b) == Action::None);
  CHECK(policyTick(p, true, true, 5, t + kMinIntervalMs, b) == Action::Send);
  policySent(p, t + kMinIntervalMs, 10);
  t += kMinIntervalMs;

  // No ack: exactly one retry, then wait for a change.
  CHECK(policyTick(p, true, true, 5, t + kAckTimeoutMs, b) == Action::Send);
  policySent(p, t + kAckTimeoutMs, 11);
  t += kAckTimeoutMs;
  CHECK(policyTick(p, true, true, 5, t + kAckTimeoutMs, b) == Action::None);
  CHECK(policyTick(p, true, true, 5, t + 10 * kMinIntervalMs, b) == Action::None);

  // New link generation: fresh connect push after the settle window.
  CHECK(policyTick(p, true, true, 6, t + 20 * kMinIntervalMs, b) == Action::None);
  CHECK(policyTick(p, true, true, 6, t + 20 * kMinIntervalMs + kConnectSettleMs,
                   b) == Action::Send);

  // A failed write (nothing reached the glasses) may repeat after the interval.
  Policy q;
  CHECK(policyTick(q, true, true, 1, 0, a) == Action::None);
  CHECK(policyTick(q, true, true, 1, kConnectSettleMs, a) == Action::Send);
  policySendFailed(q, kConnectSettleMs);
  CHECK(policyTick(q, true, true, 1, kConnectSettleMs + 1000, a) == Action::None);
  CHECK(policyTick(q, true, true, 1, kConnectSettleMs + kMinIntervalMs, a) ==
        Action::Send);

  // millis() wrap.
  Policy w;
  const uint32_t near = 0xFFFFFF00u;
  CHECK(policyTick(w, true, true, 2, near, a) == Action::None);
  CHECK(policyTick(w, true, true, 2, near + kConnectSettleMs, a) == Action::Send);
}

static void testSleepScore() {
  const uint32_t H = 3600, M = 60;
  SleepStages none;
  CHECK(sleepScore(none) == 0);  // no record -> "-"

  // A full night with typical stages scores 100.
  SleepStages good;
  good.asleepSec = 8 * H;
  good.awakeSec = 30 * M;
  good.deepSec = 90 * M;   // 18.75 %
  good.remSec = 105 * M;   // 21.9 %
  good.lightSec = good.asleepSec - good.deepSec - good.remSec;
  CHECK(sleepScore(good) == 100);

  // Unstaged records get neutral efficiency/stage points.
  SleepStages flat;
  flat.asleepSec = 7 * H;
  CHECK(sleepScore(flat) == 50 + 15 + 12);
  flat.asleepSec = 10 * H;
  CHECK(sleepScore(flat) == 45 + 27);
  flat.asleepSec = 20 * H;
  CHECK(sleepScore(flat) == 35 + 27);  // long-sleep floor
  flat.asleepSec = 2 * H;
  CHECK(sleepScore(flat) == 27);
  flat.asleepSec = 4 * H + 30 * M;     // halfway from 2 h to 7 h
  CHECK(sleepScore(flat) == 25 + 27);

  // A short night (3 h 45 min) with ideal stages: duration earns 17 of 50.
  SleepStages shortNight = good;
  shortNight.asleepSec = 13500;
  shortNight.awakeSec = 0;
  shortNight.deepSec = 13500 / 5;
  shortNight.remSec = 13500 / 5;
  shortNight.lightSec = 13500 - 2 * (13500 / 5);
  CHECK(sleepScore(shortNight) == 17 + 25 + 25);

  // Restless night: efficiency 60 % earns nothing; no deep sleep loses 12.
  SleepStages restless = good;
  restless.awakeSec = good.asleepSec * 2 / 3;
  restless.deepSec = 0;
  restless.lightSec = good.asleepSec - restless.remSec;
  CHECK(sleepScore(restless) == 50 + 0 + 13);

  // Corrupt input stays in range and never reads as "no data".
  SleepStages bad;
  bad.asleepSec = 60;
  bad.awakeSec = 0xFFFFFFFFu;
  bad.deepSec = 0xFFFFFFFFu;
  bad.remSec = 0xFFFFFFFFu;
  const uint32_t b = sleepScore(bad);
  CHECK(b >= 1 && b <= 100);
  bad.asleepSec = 0xFFFFFFFFu;
  CHECK(sleepScore(bad) <= 100);

  // Every score survives sanitize() and the validator.
  for (uint32_t sec = 0; sec <= 30 * H; sec += 997) {
    SleepStages s;
    s.asleepSec = sec;
    s.awakeSec = sec / 7;
    s.deepSec = sec / 6;
    s.remSec = sec / 5;
    s.lightSec = sec - s.deepSec - s.remSec;
    const uint32_t v = sleepScore(s);
    CHECK(v <= 100);
    CHECK((sec == 0) == (v == 0));
    Input in;
    in.sleepScore = (int32_t)v;
    in.sleepSeconds = (int32_t)sec;
    const Snapshot snap = sanitize(in);
    CHECK(snap.sleepScore == v);
    uint8_t pb[kMaxPbBytes];
    const size_t n = encodeSnapshot(snap, 7, pb, sizeof pb);
    CHECK(n && validateSnapshotPb(pb, n));
  }
}

static void testNoEmptyPush() {
  const Snapshot empty = sanitize(Input{});
  CHECK(!hasRingData(empty));
  Snapshot hrOnly = empty;
  hrOnly.heartRate = 70;
  CHECK(hasRingData(hrOnly));

  // Glasses up before the ring: nothing goes out, however long it waits.
  Policy p;
  CHECK(policyTick(p, true, true, 3, 0, empty) == Action::None);
  CHECK(policyTick(p, true, true, 3, kConnectSettleMs, empty) == Action::None);
  CHECK(policyTick(p, true, true, 3, 10 * kMinIntervalMs, empty) == Action::None);
  // First ring data: the link's first push goes out at once.
  CHECK(policyTick(p, true, true, 3, 10 * kMinIntervalMs + 1, hrOnly) == Action::Send);
  policySent(p, 10 * kMinIntervalMs + 1, 4);
  CHECK(policyAck(p, kCmdMultData, 4));
  // Data vanishing later (day rollover, ring reset) never blanks the panel.
  CHECK(policyTick(p, true, true, 3, 30 * kMinIntervalMs, empty) == Action::None);
}

int main() {
  testEncodingMatchesApp();
  testSanitize();
  testValidatorRoundTrip();
  testValidatorRejects();
  testAck();
  testPolicy();
  testSleepScore();
  testNoEmptyPush();
  if (gFailures) {
    std::fprintf(stderr, "%d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("g2_health_push_core_tests: OK\n");
  return 0;
}
