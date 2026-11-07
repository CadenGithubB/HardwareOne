#pragma once
// The continuous STT broker's speech-segmenter settings (System_STT), in one
// place so host replay tools (tools/stt_raw) reproduce the device's segments
// from a raw recording without restating them.
#include "stt_segmenter.h"

namespace hw1::stt {

// rate: PCM sample rate. maxSamples: the phrase cap. shortClose: the
// HW1_STT_SHORT_CLOSE build choice. singleUtterance: a bounded question
// (stops after the first phrase) instead of continuous listening.
inline SegmenterConfig continuousSegmenterConfig(size_t rate, size_t maxSamples,
                                                 bool shortClose, bool singleUtterance) {
  SegmenterConfig config;
  config.adaptive = true;
  // Close a phrase after >=4 s (was the 8 s default) plus the usual 600 ms
  // pause, so text is finalised while the speaker is still talking and a
  // Stop leaves only a short tail. The 15x5 transcribes at ~0.45x real time,
  // so 4 s phrases keep up; less context at each cut is the trade-off.
  config.min_segment_samples = 4 * rate;
  config.max_segment_samples = maxSamples;
  // Below 4 s a phrase closes after 1 s of silence instead of waiting for
  // the 4 s floor (a short reply gets its caption); at 4 s or more the
  // 600 ms end wait is unchanged.
  if (shortClose) config.short_close_silence_samples = rate;
  // The G2 glasses mic's quiet level jitters (~21..60+ RMS per 10 ms frame,
  // measured 2026-09-30), so the window minimum underestimated the room and
  // stray blips kept restarting the 600 ms pause: phrases ran to the 20 s
  // limit and noise opened new ones. Track the lower quartile instead, and
  // let up to 30 ms of louder frames pass without restarting the pause.
  // Both apply to every source, the onboard PDM mic included.
  config.noise_percentile = 25;
  config.silence_blip_samples = 3 * kSegmentAnalysisSamples;
  // A phrase that reaches the length limit is cut at the quietest point of
  // its last 2 s (usually between words), not mid-word at the exact limit.
  config.hard_cut_search_samples = 2 * rate;
  if (singleUtterance) {
    config.min_segment_samples = config.min_voice_samples;
    config.end_silence_samples = rate * 3 / 2;
    config.short_close_silence_samples = 0;
  }
  return config;
}

}  // namespace hw1::stt
