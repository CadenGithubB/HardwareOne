#pragma once

#include <stddef.h>
#include <stdint.h>

// Synchronous model backend. The caller owns PCM and the output buffer for the
// whole call. An optional session retains verified weights between calls;
// per-segment model/frontend allocations remain temporary.
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
  bool weightsReused = false;
  uint32_t loadMs = 0;
  uint32_t frontendMs = 0;
  uint32_t inferenceMs = 0;
  uint32_t decodeMs = 0;
};

// Single-worker backend lifetime, never shared between authenticated runs.
// Reset after the final call has returned, including failure/cancellation.
// FreeRTOS self-deleting tasks must reset explicitly before vTaskDelete().
class STTLocalSession {
 public:
  STTLocalSession() = default;
  ~STTLocalSession();
  void reset();
  STTLocalSession(const STTLocalSession&) = delete;
  STTLocalSession& operator=(const STTLocalSession&) = delete;
 private:
  void* backendState_ = nullptr;
  friend bool sttLocalTranscribe(const int16_t*, size_t, char*, size_t,
                                const STTLocalControl&, STTLocalStats&,
                                char*, size_t, STTLocalSession*);
};

// Cheap availability check for the configured, fixed model path; does not load
// a model. A true result is readiness to attempt a run, not proof of its result.
bool sttLocalAvailable(char* error, size_t cap);
bool sttLocalTranscribe(const int16_t* pcm, size_t samples,
                        char* text, size_t textCap,
                        const STTLocalControl& control, STTLocalStats& stats,
                        char* error, size_t errorCap,
                        STTLocalSession* session = nullptr);
