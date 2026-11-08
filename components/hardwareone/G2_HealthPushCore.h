#ifndef G2_HEALTH_PUSH_CORE_H
#define G2_HEALTH_PUSH_CORE_H

// Stock G2 Health panel feed: the sid 0x0E HealthDataPackage the Even app
// sends to the right temple. Pure, host-testable, no Arduino/IDF.
//
// Only ONE message shape is ever built: MULT_DATA (commandId 2) with exactly
// six entries in the app's order, every scalar written explicitly (the app
// writes zeros instead of omitting them). The encoder clamps every value, and
// validateSnapshotPb() re-parses the bytes and refuses anything outside that
// shape before it may go on the wire.
//
// Stock behaviour this relies on (see docs/G2_HEALTH_PANEL.md):
//   * the glasses keep the last snapshot in RAM only (no flash/KV write);
//   * they check only dataType (2..9), copy goal/value/avg/duration raw and
//     cast floats to integers for display, so the sender must keep values
//     finite and in range;
//   * a stored snapshot is echoed back as {1 cmd, 2 magic, 4 {1 type ...}}.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace hw1_g2_health_push {

static constexpr uint8_t kSid = 0x0E;
static constexpr uint8_t kCmdMultData = 2;
static constexpr uint8_t kTypeAll = 1;
static constexpr size_t kEntryCount = 6;
static constexpr size_t kMaxPbBytes = 160;  // one envelope fragment

enum DataType : uint8_t {
  kSteps = 2,
  kCalories = 3,
  kSleep = 4,
  kHeartRate = 5,
  kBloodOxygen = 6,
  kProductivity = 9,
};

// Entry order of the app's dashboard snapshot (sendDashboardSnapshot).
static constexpr uint8_t kEntryOrder[kEntryCount] = {
    kSteps, kCalories, kSleep, kHeartRate, kBloodOxygen, kProductivity};

// Inclusive limits. A value outside [min, max] is sent as 0, which the
// glasses render as "-". Goals outside their range fall back to the default.
static constexpr uint32_t kStepsMax = 100000;
static constexpr uint32_t kStepGoalMin = 1000, kStepGoalMax = 100000,
                          kStepGoalDefault = 10000;
static constexpr uint32_t kKcalMax = 20000;
static constexpr uint32_t kKcalGoalMin = 100, kKcalGoalMax = 10000,
                          kKcalGoalDefault = 300;
static constexpr uint32_t kSleepScoreMax = 100;
static constexpr uint32_t kSleepSecondsMax = 86400;
static constexpr uint32_t kHeartRateMin = 30, kHeartRateMax = 220;
static constexpr uint32_t kSpo2Min = 70, kSpo2Max = 100;

// Raw inputs. Negative = unknown.
struct Input {
  int32_t steps = -1;
  int32_t stepGoal = -1;
  int32_t kcal = -1;
  int32_t kcalGoal = -1;
  int32_t sleepScore = -1;
  int32_t sleepSeconds = -1;
  int32_t heartRate = -1;
  int32_t heartRateAvg = -1;
  int32_t spo2 = -1;
  int32_t spo2Avg = -1;
};

// Clamped values exactly as they will be encoded.
struct Snapshot {
  uint32_t steps = 0, stepGoal = kStepGoalDefault;
  uint32_t kcal = 0, kcalGoal = kKcalGoalDefault;
  uint32_t sleepScore = 0, sleepSeconds = 0;
  uint32_t heartRate = 0, heartRateAvg = 0;
  uint32_t spo2 = 0, spo2Avg = 0;
  bool operator==(const Snapshot& o) const {
    return steps == o.steps && stepGoal == o.stepGoal && kcal == o.kcal &&
           kcalGoal == o.kcalGoal && sleepScore == o.sleepScore &&
           sleepSeconds == o.sleepSeconds && heartRate == o.heartRate &&
           heartRateAvg == o.heartRateAvg && spo2 == o.spo2 &&
           spo2Avg == o.spo2Avg;
  }
  bool operator!=(const Snapshot& o) const { return !(*this == o); }
};

inline uint32_t inRangeOrZero(int32_t v, uint32_t lo, uint32_t hi) {
  if (v < 0) return 0;
  const uint32_t u = (uint32_t)v;
  return (u >= lo && u <= hi) ? u : 0;
}

inline uint32_t goalOrDefault(int32_t v, uint32_t lo, uint32_t hi,
                              uint32_t def) {
  if (v < 0) return def;
  const uint32_t u = (uint32_t)v;
  return (u >= lo && u <= hi) ? u : def;
}

inline Snapshot sanitize(const Input& in) {
  Snapshot s;
  s.steps = inRangeOrZero(in.steps, 0, kStepsMax);
  s.stepGoal = goalOrDefault(in.stepGoal, kStepGoalMin, kStepGoalMax,
                             kStepGoalDefault);
  s.kcal = inRangeOrZero(in.kcal, 0, kKcalMax);
  s.kcalGoal = goalOrDefault(in.kcalGoal, kKcalGoalMin, kKcalGoalMax,
                             kKcalGoalDefault);
  s.sleepScore = inRangeOrZero(in.sleepScore, 0, kSleepScoreMax);
  s.sleepSeconds = inRangeOrZero(in.sleepSeconds, 0, kSleepSecondsMax);
  s.heartRate = inRangeOrZero(in.heartRate, kHeartRateMin, kHeartRateMax);
  s.heartRateAvg = inRangeOrZero(in.heartRateAvg, kHeartRateMin, kHeartRateMax);
  s.spo2 = inRangeOrZero(in.spo2, kSpo2Min, kSpo2Max);
  s.spo2Avg = inRangeOrZero(in.spo2Avg, kSpo2Min, kSpo2Max);
  return s;
}

// ------------------------------------------------------------- sleep score
// HardwareOne's own 0-100 sleep score for the tile. The glasses draw a score
// of 0 as "-" and then hide the duration too, so a night needs a non-zero
// score to show at all. The Even app shows a score Even's backend computes
// from duration, efficiency and stage balance; that formula is not in the app,
// so this one is ours (documented in docs/G2_HEALTH_PANEL.md):
//   duration 50: full at 7-9 h asleep, 0 at <= 2 h, -5 per hour past 9 h (>= 35)
//   efficiency 25: asleep / (asleep + awake), full at >= 90 %, 0 at <= 65 %
//   stages 25: deep share full at 13-23 % (12 pts), REM 18-25 % (13 pts),
//              each falling linearly to 0 at 0 % and at twice the upper bound
// A record without stages gets neutral efficiency (15) and stage (12) points.
struct SleepStages {
  uint32_t asleepSec = 0, awakeSec = 0, remSec = 0, lightSec = 0, deepSec = 0;
};

// `full` inside [lo, hi]; linear to 0 at zeroLo below and at zeroHi above.
inline uint32_t bandPoints(uint32_t x, uint32_t zeroLo, uint32_t lo, uint32_t hi,
                           uint32_t zeroHi, uint32_t full) {
  if (x >= lo && x <= hi) return full;
  if (x < lo) {
    if (x <= zeroLo) return 0;
    return (uint32_t)((uint64_t)full * (x - zeroLo) / (lo - zeroLo));
  }
  if (x >= zeroHi) return 0;
  return (uint32_t)((uint64_t)full * (zeroHi - x) / (zeroHi - hi));
}

inline uint32_t sleepScore(const SleepStages& st) {
  if (st.asleepSec == 0) return 0;
  const uint32_t asleep = st.asleepSec > kSleepSecondsMax ? kSleepSecondsMax : st.asleepSec;
  uint32_t duration;
  if (asleep <= 9u * 3600u) {
    duration = bandPoints(asleep, 2u * 3600u, 7u * 3600u, 9u * 3600u, 9u * 3600u + 1u, 50);
  } else {
    const uint32_t penalty = (asleep - 9u * 3600u) * 5u / 3600u;
    duration = penalty >= 15 ? 35 : 50 - penalty;
  }
  const bool staged = st.remSec || st.lightSec || st.deepSec;
  uint32_t efficiency = 15, stages = 12;
  if (staged) {
    const uint32_t awake = st.awakeSec > kSleepSecondsMax ? kSleepSecondsMax : st.awakeSec;
    const uint32_t effPermille = (uint32_t)((uint64_t)asleep * 1000u / (asleep + awake));
    efficiency = bandPoints(effPermille, 650, 900, 1000, 1001, 25);
    const uint32_t deep = (uint32_t)((uint64_t)st.deepSec * 1000u / asleep);
    const uint32_t rem = (uint32_t)((uint64_t)st.remSec * 1000u / asleep);
    stages = bandPoints(deep, 0, 130, 230, 460, 12) + bandPoints(rem, 0, 180, 250, 500, 13);
  }
  uint32_t total = duration + efficiency + stages;
  if (total > kSleepScoreMax) total = kSleepScoreMax;
  return total ? total : 1;  // a real night never reads as "no data"
}

// ---------------------------------------------------------------- encoding

struct Writer {
  uint8_t* buf;
  size_t cap;
  size_t len;
  bool ok;
  void byte(uint8_t b) {
    if (len >= cap) { ok = false; return; }
    buf[len++] = b;
  }
  void varint(uint32_t v) {
    do {
      uint8_t b = (uint8_t)(v & 0x7F);
      v >>= 7;
      if (v) b |= 0x80;
      byte(b);
    } while (v);
  }
  void fieldVarint(uint8_t field, uint32_t v) { byte((uint8_t)(field << 3)); varint(v); }
  // Small integers only (<= 2^24): exact as float.
  void fieldFloat(uint8_t field, uint32_t v) {
    byte((uint8_t)((field << 3) | 5));
    const float f = (float)v;
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    for (int i = 0; i < 4; i++) byte((uint8_t)(bits >> (8 * i)));
  }
};

struct EntryValues {
  uint8_t type;
  uint32_t goal, value, avg, duration;
};

inline void entryValues(const Snapshot& s, size_t i, EntryValues& e) {
  e = EntryValues{kEntryOrder[i], 0, 0, 0, 0};
  switch (e.type) {
    case kSteps: e.goal = s.stepGoal; e.value = s.steps; break;
    case kCalories: e.goal = s.kcalGoal; e.value = s.kcal; break;
    case kSleep: e.value = s.sleepScore; e.duration = s.sleepSeconds; break;
    case kHeartRate: e.value = s.heartRate; e.avg = s.heartRateAvg; break;
    case kBloodOxygen: e.value = s.spo2; e.avg = s.spo2Avg; break;
    default: break;  // productivity: no score without Even's cloud
  }
}

// HealthSingleData {1 type, 2 goal, 3 value f32, 4 avg f32, 5 duration,
// 6 errorCode 0, 7 trend 0}; every field written, as the app does.
inline void writeEntry(Writer& w, const EntryValues& e) {
  w.fieldVarint(1, e.type);
  w.fieldVarint(2, e.goal);
  w.fieldFloat(3, e.value);
  w.fieldFloat(4, e.avg);
  w.fieldVarint(5, e.duration);
  w.fieldVarint(6, 0);
  w.fieldVarint(7, 0);
}

inline size_t entryLen(const EntryValues& e) {
  uint8_t tmp[32];
  Writer w{tmp, sizeof tmp, 0, true};
  writeEntry(w, e);
  return w.ok ? w.len : 0;
}

// Returns the payload length, or 0 if it does not fit `cap`/kMaxPbBytes.
inline size_t encodeSnapshot(const Snapshot& s, uint8_t magic, uint8_t* out,
                             size_t cap) {
  if (!out || cap == 0) return 0;
  if (cap > kMaxPbBytes) cap = kMaxPbBytes;
  EntryValues e[kEntryCount];
  size_t inner = 2 + 2;  // 08 01 ... 18 00
  for (size_t i = 0; i < kEntryCount; i++) {
    entryValues(s, i, e[i]);
    const size_t n = entryLen(e[i]);
    if (n == 0 || n > 127) return 0;
    inner += 2 + n;
  }
  if (inner > 0x3FFF) return 0;
  Writer w{out, cap, 0, true};
  w.fieldVarint(1, kCmdMultData);
  w.fieldVarint(2, magic);
  w.byte((4 << 3) | 2);
  w.varint((uint32_t)inner);
  w.fieldVarint(1, kTypeAll);
  for (size_t i = 0; i < kEntryCount; i++) {
    w.byte((2 << 3) | 2);
    w.varint((uint32_t)entryLen(e[i]));
    writeEntry(w, e[i]);
  }
  w.fieldVarint(3, 0);
  return w.ok ? w.len : 0;
}

// ---------------------------------------------------------------- parsing

struct Reader {
  const uint8_t* p;
  size_t len;
  size_t pos;
  bool varint(uint32_t& v) {
    v = 0;
    for (int shift = 0; shift < 35; shift += 7) {
      if (pos >= len) return false;
      const uint8_t b = p[pos++];
      v |= (uint32_t)(b & 0x7F) << shift;
      if (!(b & 0x80)) return true;
    }
    return false;
  }
  bool expectByte(uint8_t b) {
    if (pos >= len || p[pos] != b) return false;
    pos++;
    return true;
  }
  bool expectVarintField(uint8_t field, uint32_t& v) {
    return expectByte((uint8_t)(field << 3)) && varint(v);
  }
  bool expectFloatField(uint8_t field, uint32_t& integerValue) {
    if (!expectByte((uint8_t)((field << 3) | 5)) || pos + 4 > len) return false;
    uint32_t bits = 0;
    for (int i = 0; i < 4; i++) bits |= (uint32_t)p[pos + i] << (8 * i);
    pos += 4;
    float f;
    memcpy(&f, &bits, sizeof f);
    if (!(f >= 0.0f) || f > 16777216.0f) return false;  // rejects NaN, <0
    const uint32_t u = (uint32_t)f;
    if ((float)u != f) return false;                     // integers only
    integerValue = u;
    return true;
  }
  bool expectLength(uint8_t field, size_t& end) {
    uint32_t n = 0;
    if (!expectByte((uint8_t)((field << 3) | 2)) || !varint(n) ||
        n > len - pos)
      return false;
    end = pos + n;
    return true;
  }
};

inline bool entryValueAllowed(const EntryValues& e) {
  const auto zeroOr = [](uint32_t v, uint32_t lo, uint32_t hi) {
    return v == 0 || (v >= lo && v <= hi);
  };
  switch (e.type) {
    case kSteps:
      return e.goal >= kStepGoalMin && e.goal <= kStepGoalMax &&
             e.value <= kStepsMax && e.avg == 0 && e.duration == 0;
    case kCalories:
      return e.goal >= kKcalGoalMin && e.goal <= kKcalGoalMax &&
             e.value <= kKcalMax && e.avg == 0 && e.duration == 0;
    case kSleep:
      return e.goal == 0 && e.value <= kSleepScoreMax && e.avg == 0 &&
             e.duration <= kSleepSecondsMax;
    case kHeartRate:
      return e.goal == 0 && e.duration == 0 &&
             zeroOr(e.value, kHeartRateMin, kHeartRateMax) &&
             zeroOr(e.avg, kHeartRateMin, kHeartRateMax);
    case kBloodOxygen:
      return e.goal == 0 && e.duration == 0 && zeroOr(e.value, kSpo2Min, kSpo2Max) &&
             zeroOr(e.avg, kSpo2Min, kSpo2Max);
    case kProductivity:
      return e.goal == 0 && e.value == 0 && e.avg == 0 && e.duration == 0;
    default:
      return false;
  }
}

// Strict whitelist check of an encoded payload. True only for the exact
// MULT_DATA shape encodeSnapshot() produces with in-range values. On success
// optionally returns the magic and decoded snapshot.
inline bool validateSnapshotPb(const uint8_t* pb, size_t len,
                               uint8_t* magicOut = nullptr,
                               Snapshot* snapshotOut = nullptr) {
  if (!pb || len == 0 || len > kMaxPbBytes) return false;
  Reader r{pb, len, 0};
  uint32_t cmd = 0, magic = 0, type = 0, err = 0;
  size_t innerEnd = 0;
  if (!r.expectVarintField(1, cmd) || cmd != kCmdMultData) return false;
  if (!r.expectVarintField(2, magic) || magic > 0xFF) return false;
  if (!r.expectLength(4, innerEnd) || innerEnd != len) return false;
  if (!r.expectVarintField(1, type) || type != kTypeAll) return false;
  Snapshot s;
  for (size_t i = 0; i < kEntryCount; i++) {
    size_t entryEnd = 0;
    if (!r.expectLength(2, entryEnd)) return false;
    EntryValues e{};
    uint32_t t = 0, eErr = 0, trend = 0;
    if (!r.expectVarintField(1, t) || t != kEntryOrder[i]) return false;
    e.type = (uint8_t)t;
    if (!r.expectVarintField(2, e.goal) || !r.expectFloatField(3, e.value) ||
        !r.expectFloatField(4, e.avg) || !r.expectVarintField(5, e.duration) ||
        !r.expectVarintField(6, eErr) || eErr != 0 ||
        !r.expectVarintField(7, trend) || trend != 0 || r.pos != entryEnd)
      return false;
    if (!entryValueAllowed(e)) return false;
    switch (e.type) {
      case kSteps: s.steps = e.value; s.stepGoal = e.goal; break;
      case kCalories: s.kcal = e.value; s.kcalGoal = e.goal; break;
      case kSleep: s.sleepScore = e.value; s.sleepSeconds = e.duration; break;
      case kHeartRate: s.heartRate = e.value; s.heartRateAvg = e.avg; break;
      case kBloodOxygen: s.spo2 = e.value; s.spo2Avg = e.avg; break;
      default: break;
    }
  }
  if (!r.expectVarintField(3, err) || err != 0 || r.pos != len) return false;
  if (magicOut) *magicOut = (uint8_t)magic;
  if (snapshotOut) *snapshotOut = s;
  return true;
}

// Glasses echo after a successful store: {1 cmd, 2 magic, ...}. Only the
// leading cmd/magic pair is read; anything after it is ignored.
inline bool parseAck(const uint8_t* pb, size_t len, uint8_t& cmd,
                     uint8_t& magic) {
  if (!pb || len < 4) return false;
  Reader r{pb, len, 0};
  uint32_t c = 0, m = 0;
  if (!r.expectVarintField(1, c) || !r.expectVarintField(2, m) || c > 0xFF ||
      m > 0xFF)
    return false;
  cmd = (uint8_t)c;
  magic = (uint8_t)m;
  return true;
}

// Rolling request id, never 0 (0 would be omitted by a proto3 encoder and
// cannot be told apart from "no magic" in an echo).
inline uint8_t nextMagic(uint8_t prev) { return (uint8_t)(prev == 0xFF ? 1 : prev + 1); }

// ---------------------------------------------------------------- policy

static constexpr uint32_t kConnectSettleMs = 2000;
static constexpr uint32_t kMinIntervalMs = 60000;
static constexpr uint32_t kAckTimeoutMs = 3000;
static constexpr uint8_t kMaxRetries = 1;

enum class Action : uint8_t { None, Send };

struct Policy {
  uint32_t generation = 0;      // right-temple link generation being served
  uint32_t readySinceMs = 0;
  bool sentThisLink = false;
  uint32_t lastSendMs = 0;
  Snapshot lastAcked;
  bool haveAcked = false;
  // Last snapshot that reached the radio (acked or not). An unchanged
  // snapshot is never re-sent after its retry, acked or rejected.
  Snapshot lastAttempted;
  bool haveAttempted = false;
  bool awaitingAck = false;
  uint8_t awaitingMagic = 0;
  uint8_t retries = 0;
  Snapshot pending;
};

// Decide whether to send now. `ready` = right temple session ready at
// `generation`. Wrap-safe millisecond arithmetic throughout.
// True when at least one value came from the ring. Goals are defaults, not
// data. An all-zero snapshot would blank every row on the glasses (e.g. right
// after boot, when the glasses connect before the ring), so it is never sent
// automatically; the panel keeps what it last showed instead.
inline bool hasRingData(const Snapshot& s) {
  return s.steps || s.kcal || s.sleepScore || s.sleepSeconds || s.heartRate ||
         s.heartRateAvg || s.spo2 || s.spo2Avg;
}

inline Action policyTick(Policy& p, bool enabled, bool ready,
                         uint32_t generation, uint32_t nowMs,
                         const Snapshot& current) {
  if (!ready || generation == 0) {
    p.generation = 0;
    p.awaitingAck = false;
    return Action::None;
  }
  if (generation != p.generation) {
    p = Policy{};
    p.generation = generation;
    p.readySinceMs = nowMs;
  }
  if (!enabled || !hasRingData(current)) return Action::None;
  if ((uint32_t)(nowMs - p.readySinceMs) < kConnectSettleMs) return Action::None;
  if (p.awaitingAck) {
    if ((uint32_t)(nowMs - p.lastSendMs) < kAckTimeoutMs) return Action::None;
    p.awaitingAck = false;
    if (p.retries < kMaxRetries) {
      p.retries++;
      p.pending = current;
      return Action::Send;
    }
    // Give up until the data changes.
  }
  if (!p.sentThisLink) {
    p.retries = 0;
    p.pending = current;
    return Action::Send;
  }
  if ((uint32_t)(nowMs - p.lastSendMs) < kMinIntervalMs) return Action::None;
  if (p.haveAttempted && current == p.lastAttempted) return Action::None;
  p.retries = 0;
  p.pending = current;
  return Action::Send;
}

inline void policySent(Policy& p, uint32_t nowMs, uint8_t magic) {
  p.sentThisLink = true;
  p.lastSendMs = nowMs;
  p.awaitingAck = true;
  p.awaitingMagic = magic;
  p.lastAttempted = p.pending;
  p.haveAttempted = true;
}

// The write itself failed (locks/link): nothing reached the glasses, so the
// same snapshot may be tried again once the minimum interval has passed.
inline void policySendFailed(Policy& p, uint32_t nowMs) {
  p.sentThisLink = true;
  p.lastSendMs = nowMs;
  p.awaitingAck = false;
}

inline bool policyAck(Policy& p, uint8_t cmd, uint8_t magic) {
  if (!p.awaitingAck || cmd != kCmdMultData || magic != p.awaitingMagic)
    return false;
  p.awaitingAck = false;
  p.lastAcked = p.pending;
  p.haveAcked = true;
  p.retries = 0;
  return true;
}

}  // namespace hw1_g2_health_push

#endif  // G2_HEALTH_PUSH_CORE_H
