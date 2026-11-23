// HAL_Motion_KnockCore.h - knock (tap) detection in software from the
// accelerometer at its full rate (416 Hz from the chip's FIFO), with a record
// of every knock and why it did or did not count. Header-only, no Arduino:
// host-tested (test/host/test_motion_knock_core.cpp).
//
// A knock is the acceleration jumping away from its recent level (a slow
// per-axis baseline, so gravity and tilting do not count) and settling again:
//   - it starts when the jump passes half the threshold (so softer knocks are
//     still recorded, as "soft": the trace shows how hard a missed knock was);
//   - it ends once the jump stays under that level for kKnockQuietMs;
//   - it counts as a tap when its peak reached the threshold, it lasted at
//     most maxMs (longer is handling, not a knock: "long"), and it did not
//     start within ringMs of the previous tap's end (the board ringing after
//     a knock: "ring").
#pragma once

#include <math.h>
#include <stdint.h>

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

struct Config {
  uint16_t thresholdMg = 500;  // peak jump a tap must reach
  uint16_t maxMs = 40;         // longest a tap may last
  uint16_t ringMs = 80;        // a knock starting this soon after a tap is its ringing
};

struct Event {
  uint32_t seq = 0;     // numbered by Trace::push (1 = the first since the clear)
  uint32_t ms = 0;      // start, in the caller's millisecond clock
  uint16_t peakMg = 0;  // the largest jump
  uint16_t durMs = 0;   // start to the last sample above the start level
  uint16_t gapMs = 0;   // since the previous knock's start (0xFFFF: none or longer)
  Verdict verdict = Verdict::Tap;
};

constexpr float kBaselineTauMs = 40.0f;  // the slow level the jump is measured from
constexpr uint32_t kKnockQuietUs = 8000; // under the start level this long ends a knock
constexpr uint16_t kMinStartMg = 60;     // the start level never goes below this (noise)

class Detector {
 public:
  void reset() { *this = Detector{}; }
  // For the trace's live view: the latest jump, and whether a knock is open.
  float lastDevMg() const { return lastDev_; }
  bool inKnock() const { return inKnock_; }

  // One accelerometer sample in mg at time tUs (microseconds, wrapping). True
  // when a knock ended with this sample; `ev` then holds it.
  bool add(const Config& cfg, const float mg[3], uint32_t tUs, Event& ev) {
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
      }
    } else {
      if (dev > peak_) peak_ = dev;
      if (dev >= startMg) {
        lastAboveUs_ = tUs;
      } else if (static_cast<uint32_t>(tUs - lastAboveUs_) >= kKnockQuietUs) {
        ended = true;
        finish(cfg, ev);
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
    ev.durMs = static_cast<uint16_t>(durUs / 1000u > 65535u ? 65535u : durUs / 1000u);
    const uint32_t gapUs = static_cast<uint32_t>(startUs_ - prevStartUs_);
    ev.gapMs = (!havePrev_ || gapUs / 1000u >= 0xFFFFu) ? 0xFFFF : static_cast<uint16_t>(gapUs / 1000u);
    havePrev_ = true;
    prevStartUs_ = startUs_;
    if (ev.peakMg < cfg.thresholdMg) {
      ev.verdict = Verdict::Soft;
    } else if (ev.durMs > cfg.maxMs) {
      ev.verdict = Verdict::Long;
    } else if (haveTap_ && static_cast<uint32_t>(startUs_ - lastTapEndUs_) < static_cast<uint32_t>(cfg.ringMs) * 1000u) {
      ev.verdict = Verdict::Ring;
    } else {
      ev.verdict = Verdict::Tap;
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
  float lastDev_ = 0;
  bool havePrev_ = false;
  uint32_t prevStartUs_ = 0;
  bool haveTap_ = false;
  uint32_t lastTapEndUs_ = 0;
};

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

}  // namespace knock
