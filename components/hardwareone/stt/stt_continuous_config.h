#pragma once
// The continuous STT broker's speech-segmenter settings (System_STT), in one
// place so host replay tools (tools/stt_raw) reproduce the device's segments
// from a raw recording without restating them.
#include "stt_segmenter.h"

namespace hw1::stt {

// rate: PCM sample rate. maxSamples: the phrase cap. shortClose: the
// HW1_STT_SHORT_CLOSE build choice. singleUtterance: a bounded question
// (stops after the first phrase) instead of continuous listening.
// speechFloor / silenceFloor / noisePercentile: the detector's tunable levels
// (device settings sttSpeechFloor, sttSilenceFloor, sttNoisePercentile); the
// defaults are the shipped values, so callers that pass nothing are unchanged.
// decisionPreEmphasis / decisionModulationPct / decisionPreHold: the level-robust
// onset and its two opt-in noise controls (sttdecisionpre, sttdecisionmod,
// sttdecisionhold and the sttg2* three); 0 = raw level only, the 1a51eac
// detector bit for bit. Any value the settings can hold gives a valid config.
//
// The level-robust onset's ambient gain, in percent of the learned ratio
// (SegmenterConfig::decision_gain_pct). 150 is the value the onset was scored
// with (docs/STT_ENGINE_INTEGRATION.md in the speech repository): the struct's
// own default of 100 gave 17 more word errors over three bench recordings.
inline constexpr uint16_t kContinuousDecisionGainPct = 150;
// The speech detector's implementation, written into `stt status`, `sttdetector`,
// the sttraw sidecar and stt_raw_replay's output: stt_segmenter.h as released in
// the speech repository's version 3.1. A recording without this marker ran
// HardwareOne's earlier pre-emphasis judge whenever its decision_pre was above 0.
inline constexpr char kContinuousDetectorVersion[] = "v3.1";
inline SegmenterConfig continuousSegmenterConfig(size_t rate, size_t maxSamples,
                                                 bool shortClose, bool singleUtterance,
                                                 uint16_t speechFloor = 45, uint16_t silenceFloor = 16,
                                                 uint8_t noisePercentile = 25, uint8_t decisionPreEmphasis = 0,
                                                 uint16_t decisionModulationPct = 0, bool decisionPreHold = false) {
  SegmenterConfig config;
  config.adaptive = true;
  if (speechFloor == 0) speechFloor = 1;
  if (silenceFloor == 0) silenceFloor = 1;
  if (silenceFloor > speechFloor) silenceFloor = speechFloor;  // the segmenter rejects silence > speech
  if (noisePercentile > 50) noisePercentile = 50;     // the segmenter rejects a percentile above the median
  config.adaptive_speech_floor = speechFloor;
  config.adaptive_silence_floor = silenceFloor;
  // Level-robust onset (device setting sttdecisionpre, 0 = raw level only; 97 = the
  // default): a frame also opens or holds a phrase on the level of x[n] - C/100 x[n-1],
  // matched to the room by an ambient gain re-learned while idle (stt_segmenter.h).
  config.decision_pre_emphasis = decisionPreEmphasis > 100 ? 100 : decisionPreEmphasis;
  config.decision_gain_pct = kContinuousDecisionGainPct;
  // Opt-in noise controls, ignored while the onset is off. The modulation gate's
  // ratio must exceed 1.00: 1..100 become 101 (the most permissive gate), above
  // 1000 becomes 1000. Off by default; a noisy-room mode, not a default candidate.
  if (decisionModulationPct > 1000) decisionModulationPct = 1000;
  if (decisionModulationPct && decisionModulationPct <= 100) decisionModulationPct = 101;
  config.decision_modulation_ratio_pct = decisionModulationPct;
  config.decision_pre_hold_requires_raw = decisionPreHold;
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
  config.noise_percentile = noisePercentile;
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
