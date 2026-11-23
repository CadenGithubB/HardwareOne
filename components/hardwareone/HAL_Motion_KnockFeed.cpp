// HAL_Motion_KnockFeed.cpp - the knock detector's shared firmware side
// (HAL_Motion_KnockFeed.h). Moved out of HAL_Motion_LSM6DSOX.cpp when the
// BNO055 got the detector too; the 4517's behaviour is unchanged.
#include "HAL_Motion_KnockFeed.h"

#if ENABLE_IMU_KNOCK_DETECTOR

#include <esp_attr.h>  // EXT_RAM_BSS_ATTR
#include <atomic>
#include <freertos/FreeRTOS.h>  // portMUX: the knock trace

#include "HAL_MotionBackend.h"
#include "HAL_MotionHookCore.h"  // kMotionTapKind*

namespace {

// The default threshold of this build's feed (the imuTapThresholdMg setting's
// default too, IMU_TAP_THRESHOLD_DEFAULT_MG in System_BuildConfig.h).
constexpr uint32_t kDefaultThresholdMg = IMU_TAP_THRESHOLD_DEFAULT_MG;

// The tuning: threshold mg | max ms << 16 (both fit 16 bits), the ring window
// and the side rule; read by the IMU task at each sample.
std::atomic<uint32_t> sKnockTuning{kDefaultThresholdMg | (120u << 16)};
std::atomic<uint16_t> sKnockRingMs{80};
std::atomic<uint32_t> sKnockSide{1u | (60u << 8)};  // axis | sidePct << 8
std::atomic<uint16_t> sSampleHz{416};
std::atomic<bool> sActive{false};

// The trace: written by the IMU task, copied by commands (a short critical
// section each way). PSRAM, cleared at each start.
EXT_RAM_BSS_ATTR knock::Trace sKnockTrace;
portMUX_TYPE sKnockMux = portMUX_INITIALIZER_UNLOCKED;
bool sKnockTraceLive = false;  // false: nothing written yet (the PSRAM bytes are not initialised)

// The IMU task's feed. PSRAM (NOLOAD: no initialisers run), so knockFeedStart
// sets it.
EXT_RAM_BSS_ATTR knock::Feed sFeed;

// The taught surfaces and the teach step: set by commands and the main loop,
// read by the IMU task at each sample, the step's knocks added by it. A short
// critical section each way (a few dozen bytes; internal RAM, initialised).
knock::Surfaces sSurfaces;
knock::Teacher sTeacher;
portMUX_TYPE sTeachMux = portMUX_INITIALIZER_UNLOCKED;

void traceClear() {
  portENTER_CRITICAL(&sKnockMux);
  sKnockTrace.clear();
  sKnockTraceLive = true;
  portEXIT_CRITICAL(&sKnockMux);
}

}  // namespace

void knockFeedStart(bool active, uint16_t sampleHz) {
  sFeed.reset();
  sSampleHz.store(sampleHz);
  sActive.store(active);
  if (active) traceClear();
}

knock::Feed& knockFeed() { return sFeed; }

bool knockFeedActive() { return sActive.load(); }

knock::Config knockFeedConfig() {
  knock::Config c;
  const uint32_t t = sKnockTuning.load();
  c.thresholdMg = (uint16_t)(t & 0xFFFF);
  c.maxMs = (uint16_t)(t >> 16);
  c.ringMs = sKnockRingMs.load();
  const uint32_t side = sKnockSide.load();
  c.sideAxis = (uint8_t)(side & 0xFF);
  c.sidePct = (uint16_t)(side >> 8);
  c.sampleHz = sSampleHz.load();
  portENTER_CRITICAL(&sTeachMux);
  c.surfaces = sSurfaces;
  portEXIT_CRITICAL(&sTeachMux);
  return c;
}

void knockFeedRecord(const KnockFeedRead& read, const knock::Event* events, uint8_t count) {
  // A teach step under way takes the taps as its examples; they press nothing.
  bool teaching = false;
  portENTER_CRITICAL(&sTeachMux);
  if (sTeacher.state == knock::TeachState::Recording) {
    teaching = true;
    for (uint8_t i = 0; i < count; ++i) sTeacher.add(events[i]);
  }
  portEXIT_CRITICAL(&sTeachMux);
  if (teaching) sFeed.pending.clear();
  const float dev = sFeed.detector.lastDevMg();
  const bool inKnock = sFeed.detector.inKnock();
  portENTER_CRITICAL(&sKnockMux);
  if (!sKnockTraceLive) {
    sKnockTrace.clear();
    sKnockTraceLive = true;
  }
  knock::Trace::Feed& f = sKnockTrace.feed;
  ++f.reads;
  if (!read.ok) ++f.failedReads;
  if (read.ok && read.samples == 0) ++f.emptyReads;
  if (read.ok && read.overrun) ++f.overruns;
  f.samples += read.samples;
  f.fifo = read.fifo;
  f.lastUnread = read.unread;
  f.lastPattern = read.pattern;
  if (read.haveLast) {
    for (int a = 0; a < 3; ++a) f.lastMg[a] = read.lastMg[a];
  }
  f.lastDevMg = (uint16_t)(dev > 65535 ? 65535 : dev);
  f.inKnock = inKnock;
  for (uint8_t i = 0; i < count; ++i) sKnockTrace.push(events[i]);
  portEXIT_CRITICAL(&sKnockMux);
}

bool knockFeedTakeTap(uint8_t& kind) {
  bool side = false;
  if (!sFeed.pending.pop(side)) {
    kind = kMotionTapKindUnknown;
    return false;
  }
  kind = side ? kMotionTapKindSide : kMotionTapKindFace;
  return true;
}

void knockFeedSetSurfaces(const knock::Surfaces& s) {
  portENTER_CRITICAL(&sTeachMux);
  sSurfaces = s;
  portEXIT_CRITICAL(&sTeachMux);
}

knock::Surfaces knockFeedSurfaces() {
  portENTER_CRITICAL(&sTeachMux);
  const knock::Surfaces s = sSurfaces;
  portEXIT_CRITICAL(&sTeachMux);
  return s;
}

void knockFeedTeachStart(bool side, uint32_t nowMs) {
  portENTER_CRITICAL(&sTeachMux);
  sTeacher.start(side, nowMs);
  portEXIT_CRITICAL(&sTeachMux);
}

void knockFeedTeachCancel() {
  portENTER_CRITICAL(&sTeachMux);
  sTeacher.cancel();
  portEXIT_CRITICAL(&sTeachMux);
}

knock::Teacher knockFeedTeachPoll(uint32_t nowMs, bool consume) {
  portENTER_CRITICAL(&sTeachMux);
  sTeacher.expire(nowMs);
  const knock::Teacher t = sTeacher;
  if (consume && (t.state == knock::TeachState::Done || t.state == knock::TeachState::TimedOut)) sTeacher.cancel();
  portEXIT_CRITICAL(&sTeachMux);
  return t;
}

// ---- The backend API's knock part (HAL_MotionBackend.h) ----------------------------
void motionBackendSetKnockTuning(long thresholdMg, long maxMs, long ringMs) {
  const uint32_t thr = (uint32_t)(thresholdMg < 50 ? 50 : thresholdMg > 4000 ? 4000 : thresholdMg);
  const uint32_t mx = (uint32_t)(maxMs < 5 ? 5 : maxMs > 500 ? 500 : maxMs);
  sKnockTuning.store(thr | (mx << 16));
  sKnockRingMs.store((uint16_t)(ringMs < 0 ? 0 : ringMs > 500 ? 500 : ringMs));
}

void motionBackendSetKnockSide(long axis, long sidePct) {
  const uint32_t a = (uint32_t)(axis < 0 ? 0 : axis > 2 ? 2 : axis);
  const uint32_t pct = (uint32_t)(sidePct < 10 ? 10 : sidePct > 1000 ? 1000 : sidePct);
  sKnockSide.store(a | (pct << 8));
}

bool motionBackendKnockTrace(knock::Trace& out, knock::Config& cfg) {
  cfg = knockFeedConfig();
  if (!sActive.load()) return false;
  portENTER_CRITICAL(&sKnockMux);
  if (sKnockTraceLive) out = sKnockTrace; else out.clear();
  portEXIT_CRITICAL(&sKnockMux);
  return true;
}

void motionBackendKnockTraceClear() { traceClear(); }

#endif  // ENABLE_IMU_KNOCK_DETECTOR
