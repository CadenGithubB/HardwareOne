#pragma once

#include <stddef.h>
#include <stdint.h>

// Synchronous model backend. The caller owns PCM and the output buffer for the
// whole call; the engine owns/frees all temporary model/frontend allocations.
// Input is 16 kHz mono signed int16 BEFORE recorder DSP (no pre-emphasis/gain).
// Cancellation is cooperative: return only after all engine work has stopped.
enum class STTLocalPhase : uint8_t { Loading, Frontend, Inference, Decoding };

struct STTLocalControl {
  void* context = nullptr;
  bool (*cancelled)(void*) = nullptr;
  void (*progress)(void*, STTLocalPhase) = nullptr;
};

struct STTLocalStats {
  uint32_t samples = 0;
  uint32_t featureFrames = 0;
  uint32_t outputFrames = 0;
  uint32_t modelBytes = 0;
  uint32_t loadMs = 0;
  uint32_t frontendMs = 0;
  uint32_t inferenceMs = 0;
  uint32_t decodeMs = 0;
};

// Cheap availability check for the configured, fixed model path; does not load
// a model. A true result is readiness to attempt a run, not proof of its result.
bool sttLocalAvailable(char* error, size_t cap);
bool sttLocalTranscribe(const int16_t* pcm, size_t samples,
                        char* text, size_t textCap,
                        const STTLocalControl& control, STTLocalStats& stats,
                        char* error, size_t errorCap);
