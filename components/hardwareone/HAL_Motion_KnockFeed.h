// HAL_Motion_KnockFeed.h - the software knock detector's firmware side, shared
// by every backend that feeds it (HAL_Motion_KnockCore.h has the detector):
// the Adafruit 4517's LSM6DS3TR-C (416 Hz from its FIFO) and the BNO055 in a
// build with IMU input (100 Hz, one read per sample).
//
// Holds the live tuning (atomics: any task sets it, the IMU task reads it at
// each sample), the knock trace (PSRAM, under a short critical section each
// way) and the IMU task's feed (detector, pending taps, clock; PSRAM). It
// also defines the knock part of the backend API (HAL_MotionBackend.h):
// motionBackendSetKnockTuning / SetKnockSide / KnockTrace / KnockTraceClear.
#pragma once

#include <stdint.h>

#include "HAL_Motion_KnockCore.h"
#include "System_BuildConfig.h"

#if ENABLE_IMU_KNOCK_DETECTOR

// ---- IMU task only ------------------------------------------------------------
// At each start: active is whether this start feeds the detector (the 4517's
// LSM6DSOX keeps its chip's tap engine: false), at sampleHz. Resets the feed
// and, when active, clears the trace.
void knockFeedStart(bool active, uint16_t sampleHz);
// The feed: the detector, the taps it accepted, the samples' clock.
knock::Feed& knockFeed();
// The tuning as the commands last set it, with this start's sample rate.
knock::Config knockFeedConfig();
// One read of the stream into the trace's feed counters, with the knocks
// that ended in it (at most a few; the trace keeps the last 24).
struct KnockFeedRead {
  bool ok = false;        // the read worked
  bool overrun = false;   // samples were lost (the FIFO overran)
  bool fifo = false;      // from a FIFO: unread and pattern are its status
  uint16_t samples = 0;   // samples fed to the detector
  uint16_t unread = 0, pattern = 0;
  bool haveLast = false;  // lastMg holds the newest sample
  int16_t lastMg[3] = {0, 0, 0};
};
void knockFeedRecord(const KnockFeedRead& read, const knock::Event* events, uint8_t count);
// The oldest tap waiting to go out with this sample: true and its kind
// (kMotionTapKindSide / Face), or false (none waits).
bool knockFeedTakeTap(uint8_t& kind);

// ---- Any task -------------------------------------------------------------------
// Whether this start feeds the detector (imustatus: taps found by the firmware).
bool knockFeedActive();

#endif  // ENABLE_IMU_KNOCK_DETECTOR
