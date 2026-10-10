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
  // Profiling detail for `sttperf` (P4 runtime only; zero elsewhere).
  uint32_t frontendConditionUs = 0, frontendFftUs = 0, frontendMelUs = 0, frontendNormUs = 0;
  uint32_t quantizeUs = 0, inferenceYieldUs = 0;
  uint16_t inferenceBlocks = 0;  // blocking vTaskDelay(1) yields in this run
  uint32_t featureCachedFrames = 0;  // rows taken from the capture-time cache
  uint16_t slowStage[3] = {0xffff, 0xffff, 0xffff};  // three slowest model stages
  uint32_t slowStageUs[3] = {};
  bool lmUsed = false;  // decode used the language model (else greedy)
};

// Log-mel rows already computed during capture for the first `count` 10 ms
// frames of this exact PCM (hw1::stt::log_mel_frame, before CMVN). The
// runtime computes only the remaining rows. Ignored when count is 0 or
// exceeds the call's frames.
struct STTFeaturePrefix {
  const float* rows = nullptr;
  size_t count = 0;
};

// PSRAM one sttLocalTranscribe call of `samples` needs beside the session's
// verified weights, by the runtime's own admission rule, and the part of it
// that must be one contiguous block (hw1::stt::callBudget). All zero when no
// backend is built in. The broker sizes its capture-time feature pool
// against it (HW1_STT_CAPTURE_FEATURES).
struct STTLocalCallBudget {
  size_t total = 0;
  size_t contiguous = 0;
};
class STTLocalSession;
STTLocalCallBudget sttLocalCallBudget(size_t samples, const STTLocalSession* session = nullptr);

// Single-worker backend lifetime: one task uses a session at a time. Reset
// after the final call has returned, including failure/cancellation.
// FreeRTOS self-deleting tasks must reset explicitly before vTaskDelete().
// A session holds the pinned, SHA-verified model weights, the system language
// model and, with HW1_STT_RESIDENT_MODEL, its resident model, whose arena
// keeps the last call's activations until the next call of the same run
// overwrites them. moveTo frees the resident model first, so an idle session
// the broker hands to its next run (sttKeepWarm) carries only weights and LM:
// PCM, features, activations and text never pass from one run to the next.
class STTLocalSession {
 public:
  STTLocalSession() = default;
  ~STTLocalSession();
  void reset();
  // Verified weights are resident (the next call skips the model load).
  bool warm() const;
  // Hands this idle session's backend to the empty, idle `to`; this one is
  // left empty. A language model the old owner shed under memory pressure
  // gets one load attempt again, as in a new session.
  // A resident model is freed, never handed over (dropCallState); the new
  // owner's first call builds its own.
  void moveTo(STTLocalSession& to);
  // Frees what calls left for this session's later calls, i.e. the resident
  // model and its activation arena (HW1_STT_RESIDENT_MODEL; otherwise
  // nothing); verified weights and the LM stay.
  void dropCallState();
  STTLocalSession(const STTLocalSession&) = delete;
  STTLocalSession& operator=(const STTLocalSession&) = delete;
 private:
  void* backendState_ = nullptr;
  friend bool sttLocalTranscribe(const int16_t*, size_t, char*, size_t,
                                const STTLocalControl&, STTLocalStats&,
                                char*, size_t, STTLocalSession*, const STTFeaturePrefix*);
  friend STTLocalCallBudget sttLocalCallBudget(size_t, const STTLocalSession*);
};

// Cheap availability check for the configured, fixed model path; does not load
// a model. A true result is readiness to attempt a run, not proof of its result.
bool sttLocalAvailable(char* error, size_t cap);
bool sttLocalTranscribe(const int16_t* pcm, size_t samples,
                        char* text, size_t textCap,
                        const STTLocalControl& control, STTLocalStats& stats,
                        char* error, size_t errorCap,
                        STTLocalSession* session = nullptr,
                        const STTFeaturePrefix* prefix = nullptr);
