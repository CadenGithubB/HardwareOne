// HAL_Motion_KnockCore.h - knock (tap) detection in software from the
// accelerometer at its full rate, with a record of every knock and why it did
// or did not count. Two feeds use it: the Adafruit 4517's LSM6DS3TR-C at
// 416 Hz from its FIFO, and the BNO055's fusion accelerometer read at 100 Hz
// (HAL_Motion_KnockFeed.cpp holds what both share). Header-only, no Arduino:
// host-testable (test_motion_knock_core.cpp, archived 2026-11-26).
//
// A knock is the acceleration jumping away from its recent level (a slow
// per-axis baseline, so gravity and tilting do not count) and settling again:
//   - it starts when the jump passes half the threshold (so softer knocks are
//     still recorded, as "soft": the trace shows how hard a missed knock was);
//   - it ends once the jump stays under that level for the quiet time
//     (knockQuietUs: kKnockQuietUs, or two sample periods at a slow rate);
//   - it counts as a tap when its peak reached the threshold, it lasted at
//     most maxMs (longer is handling, not a knock: "long"), and it did not
//     start within ringMs of the previous hard knock's end (the board
//     ringing after a knock: "ring").
//
// Side or face: by the knock's direction (Event::peakVec). Taught surfaces
// (imutapteach: five knocks on each) sort it by the face's signed direction
// against the side's axis; untaught, a one-axis ratio rule (Config::sideAxis /
// sidePct).
#pragma once

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace knock {

enum class Verdict : uint8_t { Tap = 0, Soft = 1, Long = 2, Ring = 3 };

inline const char* verdictName(Verdict v) {
  switch (v) {
    case Verdict::Tap: return "tap";
    case Verdict::Soft: return "soft";
    case Verdict::Long: return "long";
    case Verdict::Ring: return "ring";
  }
  return "?";
}

// ---- Taught surfaces ---------------------------------------------------------------
// imutapteach: five knocks on each surface. The face is kept as the signed
// mean direction of its knocks and the side as the axis of its knocks with
// the sign folded away (unit vectors, milli-units: 1000 = 1). A knock is the
// face when its signed cos to the face direction beats its |cos| to the side
// axis, else the side.
//
// Why signed for the face and folded for the side (pod bench 2026-11-23,
// BNO055 glued down): face knocks always hit +Z (Z +1750..+2243 mg, Y about
// -0.45 of Z), while side knocks are Y-dominant with Y and Z of either sign,
// some pointing straight against the face direction (+57,+362,-485 has cos
// -0.98 to it). With |cos| for both (sign ignored) such a side knock reads as
// the face; keeping the five taught knocks and taking the best |cos| misreads
// face knocks as sides once a side example points against the face. A face
// whose knocks disagree in sign shows as a short mean (faceAgree) and is
// warned about at teaching.
constexpr uint8_t kTeachKnocks = 5;
constexpr uint32_t kTeachTimeoutMs = 30000;  // a teach step not finished by then changes nothing
constexpr uint16_t kTeachCloseDeg = 25;      // taught surfaces closer than this cannot be told apart
constexpr uint16_t kTeachAgreeMin = 600;     // face knocks whose mean is shorter than this disagree in sign
constexpr size_t kTaughtTextMax = 24;        // a direction as setting text: "-1000,-1000,-1000" + 1

// A teach step's knocks: unit vectors in milli-units.
struct Surface {
  int16_t ex[kTeachKnocks][3] = {};
  uint8_t count = 0;
};

// What the detector sorts by: the side's axis and the face's direction.
struct Surfaces {
  int16_t side[3] = {0, 0, 0};
  int16_t face[3] = {0, 0, 0};
  bool haveSide = false, haveFace = false;
  bool taught() const { return haveSide && haveFace; }
};

// v as a unit vector in milli-units; false for a zero vector.
inline bool unitMilli(const int16_t v[3], int16_t out[3]) {
  float n2 = 0;
  for (int i = 0; i < 3; ++i) n2 += static_cast<float>(v[i]) * v[i];
  if (n2 <= 0) return false;
  const float n = sqrtf(n2);
  for (int i = 0; i < 3; ++i) out[i] = static_cast<int16_t>(lroundf(v[i] * 1000.0f / n));
  return true;
}

// cos of the angle between a and b, -1000..1000; 0 when either is zero.
inline int16_t cosMilli(const int16_t a[3], const int16_t b[3]) {
  float dot = 0, na = 0, nb = 0;
  for (int i = 0; i < 3; ++i) {
    dot += static_cast<float>(a[i]) * b[i];
    na += static_cast<float>(a[i]) * a[i];
    nb += static_cast<float>(b[i]) * b[i];
  }
  if (na <= 0 || nb <= 0) return 0;
  float c = dot / sqrtf(na * nb);
  if (c > 1.0f) c = 1.0f;
  if (c < -1.0f) c = -1.0f;
  return static_cast<int16_t>(lroundf(c * 1000.0f));
}
inline uint16_t absCosMilli(const int16_t a[3], const int16_t b[3]) {
  const int16_t c = cosMilli(a, b);
  return static_cast<uint16_t>(c < 0 ? -c : c);
}

// The side's axis: the principal axis of its knocks, sign folded away (out
// points the way most of them do). False when there are none.
inline bool sideAxisOf(const Surface& s, int16_t out[3]) {
  if (!s.count) return false;
  float m[3][3] = {};
  for (uint8_t k = 0; k < s.count && k < kTeachKnocks; ++k)
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) m[i][j] += (s.ex[k][i] / 1000.0f) * (s.ex[k][j] / 1000.0f);
  int col = 0;
  for (int i = 1; i < 3; ++i)
    if (m[i][i] > m[col][col]) col = i;
  float x[3] = {m[0][col], m[1][col], m[2][col]};
  for (int it = 0; it < 40; ++it) {
    float y[3] = {0, 0, 0}, n2 = 0;
    for (int i = 0; i < 3; ++i) {
      for (int j = 0; j < 3; ++j) y[i] += m[i][j] * x[j];
      n2 += y[i] * y[i];
    }
    if (n2 <= 0) return false;
    const float n = sqrtf(n2);
    for (int i = 0; i < 3; ++i) x[i] = y[i] / n;
  }
  float lean = 0;
  for (uint8_t k = 0; k < s.count && k < kTeachKnocks; ++k)
    for (int i = 0; i < 3; ++i) lean += x[i] * s.ex[k][i];
  int16_t v[3];
  for (int i = 0; i < 3; ++i) v[i] = static_cast<int16_t>(lroundf((lean < 0 ? -x[i] : x[i]) * 1000.0f));
  return unitMilli(v, out);
}

// The face's direction: the signed mean of its knocks. agreeMilli is the
// mean's length (1000: all the same way; near 0: they cancel out). False
// when there are none or they cancel exactly.
inline bool faceDirectionOf(const Surface& s, int16_t out[3], uint16_t& agreeMilli) {
  agreeMilli = 0;
  if (!s.count) return false;
  float sum[3] = {0, 0, 0};
  for (uint8_t k = 0; k < s.count && k < kTeachKnocks; ++k)
    for (int i = 0; i < 3; ++i) sum[i] += s.ex[k][i];
  const float n = sqrtf(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
  agreeMilli = static_cast<uint16_t>(lroundf(n / s.count));
  if (agreeMilli > 1000) agreeMilli = 1000;
  if (n <= 0) return false;
  for (int i = 0; i < 3; ++i) out[i] = static_cast<int16_t>(lroundf(sum[i] * 1000.0f / n));
  return true;
}

// Default thresholds per feed (the imuTapThresholdMg setting's default). The
// 4517's from the sign (bench 2026-11-23). The BNO055 filters its
// accelerometer at 62.5 Hz and is read at 100 Hz, so a knock's short spike
// reaches it at roughly a third of what the 416 Hz stream sees. Pod bench
// 2026-11-23: soft side taps peaked 255-472 mg, so 300 mg; their echoes
// (230-300 mg, 70-100 ms after the tap) fall in the ring window. Tuned live
// with 'imutaptrace'.
constexpr uint16_t kKnockThresholdMg416Hz = 1300;
constexpr uint16_t kKnockThresholdMg100Hz = 300;

struct Config {
  uint16_t thresholdMg = 500;  // peak jump a tap must reach
  uint16_t maxMs = 120;        // longest a tap may last (bench 2026-11-23: knocks on the sign ring 45-95 ms)
  uint16_t ringMs = 80;        // a knock starting this soon after a tap is its ringing
  // Side or face: a knock whose jump on sideAxis (0 X, 1 Y, 2 Z) is at least
  // sidePct % of its largest other axis hit the side; any other hit the
  // face. A ratio, so a soft knock sorts like a hard one. Bench 2026-11-23
  // (the sign, IMU in its bottom-right corner): right-side knocks had |Y| at
  // 113-450 % of the next axis, face knocks at most 20 % of |Z|.
  uint8_t sideAxis = 1;
  uint16_t sidePct = 60;
  // Taught surfaces (imutapteach): with both taught they decide side or
  // face instead of the ratio rule.
  Surfaces surfaces;
  // The feed's sample rate: the quiet time that ends a knock is at least two
  // sample periods (knockQuietUs), and the trace names it.
  uint16_t sampleHz = 416;
};

struct Event {
  uint32_t seq = 0;     // numbered by Trace::push (1 = the first since the clear)
  uint32_t ms = 0;      // start, in the caller's millisecond clock
  uint16_t peakMg = 0;  // the largest jump
  uint16_t durMs = 0;   // start to the last sample above the start level
  uint16_t gapMs = 0;   // since the previous knock's start (0xFFFF: none or longer)
  int16_t peakVec[3] = {0, 0, 0};  // the jump at the peak, per axis (mg): which way the knock hit
  Verdict verdict = Verdict::Tap;
  bool side = false;    // hit the side, else the face (classifySurface)
  bool taught = false;  // sorted by the taught surfaces (cosSide / cosFace), else by the ratio rule
  uint16_t cosSide = 0;  // with taught: |cos| to the side's axis, 0..1000
  int16_t cosFace = 0;   // ...and the signed cos to the face's direction, -1000..1000
};

// The one-axis ratio rule: side when the jump along sideAxis is at least
// sidePct % of its largest other axis. A ratio, so a soft knock sorts like a
// hard one.
inline bool ratioSide(const Config& cfg, const int16_t v[3]) {
  const int axis = cfg.sideAxis < 3 ? cfg.sideAxis : 1;
  const int32_t along = v[axis] < 0 ? -v[axis] : v[axis];
  int32_t other = 0;
  for (int i = 0; i < 3; ++i) {
    if (i == axis) continue;
    const int32_t a = v[i] < 0 ? -v[i] : v[i];
    if (a > other) other = a;
  }
  return along * 100 >= static_cast<int32_t>(cfg.sidePct) * other;
}

// Side or face for a knock's peak jump v, into e (side, taught, cosSide,
// cosFace): by the taught surfaces when both are taught (the face when its
// signed cos beats the side's |cos|), else by the ratio rule.
inline void classifySurface(const Config& cfg, const int16_t v[3], Event& e) {
  e.taught = cfg.surfaces.taught();
  if (e.taught) {
    e.cosSide = absCosMilli(v, cfg.surfaces.side);
    e.cosFace = cosMilli(v, cfg.surfaces.face);
    e.side = !(e.cosFace > static_cast<int16_t>(e.cosSide));
  } else {
    e.cosSide = e.cosFace = 0;
    e.side = ratioSide(cfg, v);
  }
}

constexpr float kBaselineTauMs = 40.0f;  // the slow level the jump is measured from
constexpr uint32_t kKnockQuietUs = 8000; // under the start level this long ends a knock
// A knock still open after this long is no knock: the baseline was wrong (a
// first reading taken before the chip had data, or a new pose held still).
// It ends as "long" and the baseline restarts from the present reading, so
// the detector can never stay "inside a knock" for good.
constexpr uint32_t kKnockMaxOpenUs = 1000000;
constexpr uint16_t kMinStartMg = 60;     // the start level never goes below this (noise)
// At a slow rate the quiet time is at least this many sample periods: at
// 100 Hz one period (10 ms) is already longer than kKnockQuietUs, and a single
// sample under the start level inside a ringing knock would split it in two.
constexpr uint32_t kKnockQuietSamples = 2;

// The quiet time for a feed: kKnockQuietUs at 416 Hz (two periods are 4.8 ms),
// 20 ms at 100 Hz.
inline uint32_t knockQuietUs(uint16_t sampleHz) {
  if (sampleHz == 0) return kKnockQuietUs;
  const uint32_t periods = kKnockQuietSamples * 1000000u / sampleHz;
  return periods > kKnockQuietUs ? periods : kKnockQuietUs;
}

class Detector {
 public:
  void reset() { *this = Detector{}; }
  // For the trace's live view: the latest jump, and whether a knock is open.
  float lastDevMg() const { return lastDev_; }
  bool inKnock() const { return inKnock_; }

  // One accelerometer sample in mg at time tUs (microseconds, wrapping). True
  // when a knock ended with this sample; `ev` then holds it.
  bool add(const Config& cfg, const float mg[3], uint32_t tUs, Event& ev) {
    // An all-zero reading is no measurement (a chip still starting): never a
    // baseline. A knock already open still ends at the 1 s cap, or zeros
    // would hold it open for as long as they continued.
    if (mg[0] == 0.0f && mg[1] == 0.0f && mg[2] == 0.0f) {
      if (!inKnock_ || static_cast<uint32_t>(tUs - startUs_) < kKnockMaxOpenUs) return false;
      lastAboveUs_ = tUs;
      finish(cfg, ev);
      haveBase_ = false;  // the next real reading is the baseline; zeros are not
      return true;
    }
    if (!haveBase_) {
      for (int i = 0; i < 3; ++i) base_[i] = mg[i];
      haveBase_ = true;
      lastUs_ = tUs;
      return false;
    }
    const float dtMs = static_cast<float>(static_cast<uint32_t>(tUs - lastUs_)) / 1000.0f;
    lastUs_ = tUs;
    float d2 = 0;
    for (int i = 0; i < 3; ++i) {
      const float d = mg[i] - base_[i];
      d2 += d * d;
    }
    const float dev = sqrtf(d2);
    lastDev_ = dev;
    uint16_t startMg = cfg.thresholdMg / 2;
    if (startMg < kMinStartMg) startMg = kMinStartMg;

    bool ended = false;
    if (!inKnock_) {
      if (dev >= startMg) {
        inKnock_ = true;
        startUs_ = lastAboveUs_ = tUs;
        peak_ = dev;
        for (int i = 0; i < 3; ++i) peakVec_[i] = mg[i] - base_[i];
      }
    } else {
      if (dev > peak_) {
        peak_ = dev;
        for (int i = 0; i < 3; ++i) peakVec_[i] = mg[i] - base_[i];
      }
      if (dev >= startMg) {
        lastAboveUs_ = tUs;
      } else if (static_cast<uint32_t>(tUs - lastAboveUs_) >= knockQuietUs(cfg.sampleHz)) {
        ended = true;
        finish(cfg, ev);
      }
      if (!ended && static_cast<uint32_t>(tUs - startUs_) >= kKnockMaxOpenUs) {
        lastAboveUs_ = tUs;
        ended = true;
        finish(cfg, ev);
        for (int i = 0; i < 3; ++i) base_[i] = mg[i];  // restart the baseline here
        lastUs_ = tUs;
        return true;
      }
    }
    // The baseline follows slow changes (tilt) but holds still during a knock.
    if (!inKnock_ || ended) {
      float a = dtMs / kBaselineTauMs;
      if (a > 1.0f) a = 1.0f;
      if (a < 0.0f) a = 0.0f;
      for (int i = 0; i < 3; ++i) base_[i] += a * (mg[i] - base_[i]);
    }
    return ended;
  }

 private:
  void finish(const Config& cfg, Event& ev) {
    inKnock_ = false;
    const uint32_t durUs = static_cast<uint32_t>(lastAboveUs_ - startUs_);
    ev.ms = startUs_ / 1000u;
    ev.peakMg = peak_ > 65535.0f ? 65535 : static_cast<uint16_t>(peak_ + 0.5f);
    for (int i = 0; i < 3; ++i) {
      const float v = peakVec_[i];
      ev.peakVec[i] = static_cast<int16_t>(v > 32767.0f ? 32767 : v < -32767.0f ? -32767 : v);
    }
    classifySurface(cfg, ev.peakVec, ev);
    ev.durMs = static_cast<uint16_t>(durUs / 1000u > 65535u ? 65535u : durUs / 1000u);
    const uint32_t gapUs = static_cast<uint32_t>(startUs_ - prevStartUs_);
    ev.gapMs = (!havePrev_ || gapUs / 1000u >= 0xFFFFu) ? 0xFFFF : static_cast<uint16_t>(gapUs / 1000u);
    havePrev_ = true;
    prevStartUs_ = startUs_;
    // Ringing follows any hard knock (a tap, or one too long to be a tap).
    const bool ringing =
        haveTap_ && static_cast<uint32_t>(startUs_ - lastTapEndUs_) < static_cast<uint32_t>(cfg.ringMs) * 1000u;
    if (ringing) {
      ev.verdict = Verdict::Ring;
    } else if (ev.peakMg < cfg.thresholdMg) {
      ev.verdict = Verdict::Soft;
    } else if (ev.durMs > cfg.maxMs) {
      ev.verdict = Verdict::Long;
    } else {
      ev.verdict = Verdict::Tap;
    }
    if (ev.peakMg >= cfg.thresholdMg) {
      haveTap_ = true;
      lastTapEndUs_ = lastAboveUs_;
    }
  }

  float base_[3] = {0, 0, 0};
  bool haveBase_ = false;
  uint32_t lastUs_ = 0;
  bool inKnock_ = false;
  uint32_t startUs_ = 0, lastAboveUs_ = 0;
  float peak_ = 0;
  float peakVec_[3] = {0, 0, 0};
  float lastDev_ = 0;
  bool havePrev_ = false;
  uint32_t prevStartUs_ = 0;
  bool haveTap_ = false;
  uint32_t lastTapEndUs_ = 0;
};

// The knock's main axis and sign, as "+X" .. "-Z" (the largest |component|
// of the peak jump), and how much it dominates: that component's share of
// the jump's size, 0..100 %.
inline void knockDirection(const Event& e, char out[3], uint8_t& sharePct) {
  int best = 0;
  for (int i = 1; i < 3; ++i)
    if ((e.peakVec[i] < 0 ? -e.peakVec[i] : e.peakVec[i]) > (e.peakVec[best] < 0 ? -e.peakVec[best] : e.peakVec[best]))
      best = i;
  out[0] = e.peakVec[best] < 0 ? '-' : '+';
  out[1] = static_cast<char>('X' + best);
  out[2] = 0;
  float n2 = 0;
  for (int i = 0; i < 3; ++i) n2 += static_cast<float>(e.peakVec[i]) * e.peakVec[i];
  const float a = e.peakVec[best] < 0 ? -e.peakVec[best] : e.peakVec[best];
  sharePct = n2 > 0 ? static_cast<uint8_t>(a * a / n2 * 100.0f + 0.5f) : 0;
}

// ---- Teaching -------------------------------------------------------------------------
// One teach step: the next kTeachKnocks taps (any side or face verdict) are
// examples of one surface. The IMU task adds the knocks; the caller checks
// the time (expire) and, once it is Done, saves the surface (withTaught).
enum class TeachState : uint8_t { Idle = 0, Recording = 1, Done = 2, TimedOut = 3 };

struct Teacher {
  TeachState state = TeachState::Idle;
  bool side = false;     // which surface is being taught
  uint32_t startMs = 0;  // the caller's millisecond clock
  Surface got;

  void start(bool sideSurface, uint32_t nowMs) {
    *this = Teacher{};
    state = TeachState::Recording;
    side = sideSurface;
    startMs = nowMs;
  }
  void cancel() { *this = Teacher{}; }
  // A knock the detector ended: a tap becomes an example. True when taken.
  bool add(const Event& e) {
    if (state != TeachState::Recording || e.verdict != Verdict::Tap || got.count >= kTeachKnocks) return false;
    if (!unitMilli(e.peakVec, got.ex[got.count])) return false;
    if (++got.count >= kTeachKnocks) state = TeachState::Done;
    return true;
  }
  // Recording for kTeachTimeoutMs without the knocks: TimedOut (true once).
  bool expire(uint32_t nowMs) {
    if (state != TeachState::Recording || static_cast<uint32_t>(nowMs - startMs) < kTeachTimeoutMs) return false;
    state = TeachState::TimedOut;
    return true;
  }
  uint32_t msLeft(uint32_t nowMs) const {
    const uint32_t used = static_cast<uint32_t>(nowMs - startMs);
    return state == TeachState::Recording && used < kTeachTimeoutMs ? kTeachTimeoutMs - used : 0;
  }
};

// A finished teach step joined to the surfaces taught before: the new
// surface replaces its old one. faceAgree is the face knocks' agreement in
// sign (faceDirectionOf), 1000 when the step taught the side.
inline Surfaces withTaught(const Surfaces& before, const Teacher& t, uint16_t& faceAgree) {
  Surfaces s = before;
  faceAgree = 1000;
  if (t.side)
    s.haveSide = sideAxisOf(t.got, s.side);
  else
    s.haveFace = faceDirectionOf(t.got, s.face, faceAgree);
  return s;
}

// How many of a teach step's knocks the surfaces would sort to the other
// surface (0 until both are taught).
inline uint8_t teachMisfits(const Teacher& t, const Surfaces& s) {
  if (!s.taught()) return 0;
  Config cfg;
  cfg.surfaces = s;
  uint8_t n = 0;
  for (uint8_t k = 0; k < t.got.count && k < kTeachKnocks; ++k) {
    Event e;
    classifySurface(cfg, t.got.ex[k], e);
    if (e.side != t.side) ++n;
  }
  return n;
}

// The angle between the side's axis and the face's direction, sign folded:
// 0..90 degrees; close when under kTeachCloseDeg (the two cannot be told
// apart). Both must be taught.
struct TeachReport {
  uint16_t sepDeg = 0;
  bool close = false;
};
inline TeachReport teachReport(const Surfaces& s) {
  TeachReport r;
  if (!s.taught()) return r;
  const float c = absCosMilli(s.side, s.face) / 1000.0f;
  r.sepDeg = static_cast<uint16_t>(lroundf(acosf(c > 1.0f ? 1.0f : c) * 180.0f / 3.14159265f));
  r.close = r.sepDeg < kTeachCloseDeg;
  return r;
}

// ---- Judging what was taught -----------------------------------------------------
// imutapteach (one surface) and the shake configuration (both, side then
// face) save and report what was taught the same way: the surfaces after the
// step(s) and the first thing worth a warning, in this order.
enum class TeachVerdict : uint8_t {
  Ok = 0,         // both taught, kTeachCloseDeg or more apart, every knock sorts as its own surface
  NeedOther,      // one surface taught, the other not yet
  FaceDisagrees,  // the face's knocks point opposite ways (agreement under kTeachAgreeMin)
  Close,          // the two are under kTeachCloseDeg apart: they cannot be told apart
  Misfits,        // some of the step's knocks would sort as the other surface
};
struct TeachJudgement {
  Surfaces surfaces;          // to save
  TeachVerdict verdict = TeachVerdict::Ok;
  uint16_t sepDeg = 0;        // with both taught: how far apart
  uint8_t knocks = 0;         // the step(s)' knocks
  uint8_t misfits = 0;        // ...of which would sort as the other surface
  uint16_t faceAgree = 1000;  // the face step's agreement in sign (1000 without one)
};
// side and/or face: finished teach steps (Done) joined to `before`.
inline TeachJudgement judgeTaught(const Surfaces& before, const Teacher* side, const Teacher* face) {
  TeachJudgement j;
  j.surfaces = before;
  uint16_t agree = 1000;
  if (side) j.surfaces = withTaught(j.surfaces, *side, agree);
  if (face) {
    j.surfaces = withTaught(j.surfaces, *face, agree);
    j.faceAgree = agree;
  }
  const Teacher* steps[2] = {side, face};
  for (const Teacher* t : steps) {
    if (!t) continue;
    j.knocks = static_cast<uint8_t>(j.knocks + t->got.count);
    j.misfits = static_cast<uint8_t>(j.misfits + teachMisfits(*t, j.surfaces));
  }
  const TeachReport r = teachReport(j.surfaces);
  j.sepDeg = r.sepDeg;
  if (face && j.faceAgree < kTeachAgreeMin)
    j.verdict = TeachVerdict::FaceDisagrees;
  else if (!j.surfaces.taught())
    j.verdict = TeachVerdict::NeedOther;
  else if (r.close)
    j.verdict = TeachVerdict::Close;
  else if (j.misfits)
    j.verdict = TeachVerdict::Misfits;
  else
    j.verdict = TeachVerdict::Ok;
  return j;
}

// A direction as setting text: "x,y,z" (milli-units), "" untaught.
inline void formatDirection(bool have, const int16_t v[3], char* out, size_t len) {
  if (!len) return;
  out[0] = 0;
  if (!have) return;
  const int w = snprintf(out, len, "%d,%d,%d", v[0], v[1], v[2]);
  if (w < 0 || static_cast<size_t>(w) >= len) out[0] = 0;  // did not fit: untaught rather than cut
}

// Setting text back to a direction (re-normalised). "" (or only spaces) is
// untaught (true); anything else must be three whole numbers -1000..1000,
// not all zero, or it is untaught and the answer false.
inline bool parseDirection(const char* text, bool& have, int16_t out[3]) {
  have = false;
  out[0] = out[1] = out[2] = 0;
  if (!text) return true;
  const char* p = text;
  while (*p == ' ') ++p;
  if (!*p) return true;
  int16_t v[3];
  for (int i = 0; i < 3; ++i) {
    while (*p == ' ') ++p;
    bool neg = false;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if (*p < '0' || *p > '9') return false;
    int32_t x = 0;
    while (*p >= '0' && *p <= '9') {
      x = x * 10 + (*p++ - '0');
      if (x > 1000) return false;
    }
    v[i] = static_cast<int16_t>(neg ? -x : x);
    while (*p == ' ') ++p;
    if (i < 2 && *p++ != ',') return false;
  }
  if (*p) return false;
  int16_t u[3];
  if (!unitMilli(v, u)) return false;
  for (int i = 0; i < 3; ++i) out[i] = u[i];
  have = true;
  return true;
}

// The last kTraceLen knocks and counts per verdict since the last clear.
constexpr uint8_t kTraceLen = 24;
struct Trace {
  Event events[kTraceLen];
  uint8_t head = 0;   // next slot
  uint8_t count = 0;  // filled slots
  uint32_t seq = 0;   // knocks recorded since the clear (numbers the events)
  uint32_t counts[4] = {0, 0, 0, 0};
  uint16_t softMaxMg = 0;   // the strongest soft knock
  uint16_t longMaxMs = 0;   // the longest long knock
  uint16_t ringMinMs = 0xFFFF;  // the closest ring knock to its tap (start gap)

  // The stream feeding the detector (filled by the backend).
  struct Feed {
    uint32_t reads = 0, failedReads = 0, samples = 0, overruns = 0, emptyReads = 0;
    uint16_t lastUnread = 0, lastPattern = 0;
    int16_t lastMg[3] = {0, 0, 0};
    uint16_t lastDevMg = 0;
    bool inKnock = false;
    bool fifo = false;  // the 4517's FIFO (unread / pattern are its own); else one read per sample
  } feed;

  void clear() { *this = Trace{}; }
  void push(Event e) {
    e.seq = seq + 1;
    events[head] = e;
    head = static_cast<uint8_t>((head + 1) % kTraceLen);
    if (count < kTraceLen) ++count;
    ++seq;
    ++counts[static_cast<uint8_t>(e.verdict)];
    if (e.verdict == Verdict::Soft && e.peakMg > softMaxMg) softMaxMg = e.peakMg;
    if (e.verdict == Verdict::Long && e.durMs > longMaxMs) longMaxMs = e.durMs;
    if (e.verdict == Verdict::Ring && e.gapMs < ringMinMs) ringMinMs = e.gapMs;
  }
  // i = 0 is the oldest kept event.
  const Event& at(uint8_t i) const { return events[(head + kTraceLen - count + i) % kTraceLen]; }
};

// Taps the detector accepted, handed out one per sample (MotionSample::hwTap),
// oldest first, with where each hit and its peak jump (Event::peakVec, mg:
// the shake configuration teaches the surfaces from it). At most kPendingMax
// wait; more are dropped (eight taps inside one sample period is not a
// person).
constexpr uint8_t kPendingMax = 8;
struct PendingTaps {
  uint8_t count = 0;
  uint8_t sideBits = 0;  // bit i: the i-th waiting tap (oldest first) hit the side
  int16_t vec[kPendingMax][3] = {};  // the i-th waiting tap's peak jump
  void clear() { count = sideBits = 0; }
  void push(bool side, const int16_t* v = nullptr) {
    if (count >= kPendingMax) return;
    if (side) sideBits = static_cast<uint8_t>(sideBits | (1u << count));
    for (int i = 0; i < 3; ++i) vec[count][i] = v ? v[i] : 0;
    ++count;
  }
  // The oldest waiting tap (and its peak jump into v, when given); false
  // when none waits.
  bool pop(bool& side, int16_t* v = nullptr) {
    if (!count) return false;
    side = (sideBits & 1u) != 0;
    sideBits = static_cast<uint8_t>(sideBits >> 1);
    if (v)
      for (int i = 0; i < 3; ++i) v[i] = vec[0][i];
    for (uint8_t k = 1; k < count; ++k)
      for (int i = 0; i < 3; ++i) vec[k - 1][i] = vec[k][i];
    --count;
    return true;
  }
};

// The samples' clock (microseconds, wrapping): it only moves forward, since
// the detector's lengths and gaps are differences.
struct SampleClock {
  bool have = false;
  uint32_t nextUs = 0;
  void reset() { have = false; }
  // A batch whose first sample is estimated at estimateUs (the FIFO): keep
  // counting from the last batch unless the reads fell more than jumpUs
  // behind the chip, then start from the estimate.
  void sync(uint32_t estimateUs, uint32_t jumpUs = 20000) {
    if (!have || static_cast<int32_t>(estimateUs - nextUs) > static_cast<int32_t>(jumpUs)) {
      nextUs = estimateUs;
      have = true;
    }
  }
  // The next sample of a batch, periodUs after the one before.
  uint32_t take(uint32_t periodUs) {
    const uint32_t t = nextUs;
    nextUs += periodUs;
    return t;
  }
  // One sample read at nowUs (a feed without a FIFO): its read time, or just
  // after the previous sample's when the clock would go back.
  uint32_t at(uint32_t nowUs) {
    const uint32_t t = (have && static_cast<int32_t>(nowUs - nextUs) < 0) ? nextUs : nowUs;
    have = true;
    nextUs = t + 1;
    return t;
  }
};

// What a backend feeds: the detector, the taps it accepted and the clock.
// Owned by the IMU task (HAL_Motion_KnockFeed.cpp keeps one, in PSRAM).
struct Feed {
  Detector detector;
  PendingTaps pending;
  SampleClock clock;
  void reset() { *this = Feed{}; }
  // Samples were lost (a FIFO overrun): the baseline and the clock start
  // over; taps already accepted still go out.
  void restart() {
    detector.reset();
    clock.reset();
  }
  // One sample (mg at tUs); true when a knock ended, `ev` then holds it. A tap
  // joins the pending taps.
  bool add(const Config& cfg, const float mg[3], uint32_t tUs, Event& ev) {
    if (!detector.add(cfg, mg, tUs, ev)) return false;
    if (ev.verdict == Verdict::Tap) pending.push(ev.side, ev.peakVec);
    return true;
  }
};

}  // namespace knock
