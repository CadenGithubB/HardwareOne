#pragma once

#include <stddef.h>
#include <stdint.h>

// Synchronous model backend. The caller owns PCM and the output buffer for the
// whole call. An optional session retains verified weights between calls;
// per-segment model/frontend allocations remain temporary.
// Input is 16 kHz mono signed int16 BEFORE recorder DSP (no pre-emphasis/gain).
// Cancellation is cooperative: return only after all engine work has stopped.
enum class SttLocalPhase : uint8_t { Loading, Frontend, Inference, Decoding };

struct SttLocalControl {
  void* context = nullptr;
  bool (*cancelled)(void*) = nullptr;
  void (*progress)(void*, SttLocalPhase) = nullptr;
};

struct SttLocalStats {
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
struct SttFeaturePrefix {
  const float* rows = nullptr;
  size_t count = 0;
};

// PSRAM one sttLocalTranscribe call of `samples` needs beside the session's
// verified weights, by the runtime's own admission rule, and the part of it
// that must be one contiguous block (hw1::stt::callBudget). All zero when no
// backend is built in. The broker sizes its capture-time feature pool
// against it (HW1_STT_CAPTURE_FEATURES).
struct SttLocalCallBudget {
  size_t total = 0;
  size_t contiguous = 0;
};
class SttLocalSession;
SttLocalCallBudget sttLocalCallBudget(size_t samples, const SttLocalSession* session = nullptr);

// Speaker labels: an optional speaker-embedding model beside the STT model,
// "/sd/STT Models/speaker.spk" (format and contract: stt/speaker_model.h), loaded
// after each run's warm-up and freed when the run ends. Each finished phrase
// gets a run-local label (0 = the first voice heard, 1, ...) from
// stt/speaker_cluster.h. Any embedding model works that its file describes.
// ENABLE_STT_SPEAKER_LABELS comes from System_BuildConfig.h, which System_STT.h
// and System_STTLocal.cpp include first.
#if defined(CONFIG_IDF_TARGET_ESP32P4) && defined(ENABLE_STT_SPEAKER_LABELS) && ENABLE_STT_SPEAKER_LABELS
#define HW1_STT_SPEAKER_LABELS 1
#else
#define HW1_STT_SPEAKER_LABELS 0
#endif
// Speaker diagnostics (kept, compiled out by default): fixture self-checks at load
// and every 16th job (TL spkfix), per-phrase voiceprints (TL spke), ESP-DL kernel
// binds (TL kbind) and per-layer internal-RAM dips (TL spkram). 1 to enable.
#ifndef HW1_STT_SPEAKER_DIAGNOSTICS
#define HW1_STT_SPEAKER_DIAGNOSTICS 0
#endif
struct SttSpeakerStatus {
  bool ready = false;
  size_t bytes = 0;            // speaker.spk size
  size_t psramUsed = 0;        // PSRAM the loaded model, its arena and workspace hold
  long internalUsed = 0;       // internal RAM change across the load (should be ~0)
  uint32_t loadMs = 0;
  float threshold = 0;         // from the file
  float fixtureCos = -2;       // cosine to the Mac's embedding of speaker_fixture.bin (-2: none)
  char why[48] = {};
  // where the load time and memory went
  uint32_t readMs = 0, shaUs = 0, buildUs = 0, testUs = 0, fixtureUs = 0;
  int testResult = -1;         // ESP-DL self-test on the file's test values (0 = pass)
  size_t arenaPsram = 0, arenaInternal = 0, workspaceBytes = 0;
  unsigned modules = 0;        // trunk execution-plan modules (layers)
};
struct SttSpeakerLabel {
  int8_t speaker = -1;         // 0-based session label; -1 none
  float similarity = 0;
  bool created = false, tentative = false;
  uint32_t featureUs = 0, trunkUs = 0, headUs = 0, totalUs = 0;
  // sections: feature = fft + mel + norm; head = sap + pool + fc; cluster = label assignment
  uint32_t fftUs = 0, melUs = 0, normUs = 0, quantUs = 0, dequantUs = 0, sapUs = 0, poolUs = 0, fcUs = 0, clusterUs = 0;
  uint16_t frames = 0;         // feature frames computed
  bool cropped = false;        // phrase longer than the model window (centre 3 s used)
};
// Per-layer trunk profile of the loaded speaker model, summed over the session
// (TL spkstg lines), then reset. No-op without a model.
void sttSpeakerProfileLog();
bool sttSpeakerBegin(SttLocalSession* session, size_t maxSamples, SttSpeakerStatus* status);
// Background labelling: a priority-1 task on core 1 owns the speaker model.
// Text is published before submission. Submit waits for one of four fixed PCM
// slots in cancellation-aware slices, for at most 30 s, then copies the centre
// window (<= 96 KB). Optional canWait(control.context) is consulted only when
// no slot is immediately available: false preserves capture headroom by ending
// the wait. True means background handling, including a terminal unknown on
// cancellation/timeout/audio pressure; false means no background worker and permits
// inline fallback. A busy worker must never fall through to concurrent inline use.
// End requests cooperative stop and joins the current embed before freeing any
// queues/model; queued jobs are discarded. It may wait longer than the admission
// deadline because an active ESP-DL embed cannot safely be force-deleted.
bool sttSpeakerSubmit(SttLocalSession* session, uint32_t seq, const int16_t* pcm, size_t samples,
                      const SttLocalControl& control = {}, bool (*canWait)(void*) = nullptr);
void sttSpeakerEnd(SttLocalSession* session);
// Labels for the transcript writer (the STT worker): true while a background task labels phrases.
bool sttSpeakerBackground(SttLocalSession* session);
// One finished label (non-blocking): speaker -1 = phrase skipped/failed (write it unlabelled).
bool sttSpeakerTakeLabel(SttLocalSession* session, uint32_t* seq, int8_t* speaker);
// Wait for submitted work, bounded by timeoutMs and cancellation. This does not
// free/cancel a live worker; consume available results, then call End to join it.
void sttSpeakerDrain(SttLocalSession* session, uint32_t timeoutMs,
                     const SttLocalControl& control = {});
bool sttSpeakerLabel(SttLocalSession* session, const int16_t* pcm, size_t samples, SttSpeakerLabel* out);

// Single-worker backend lifetime: one task uses a session at a time. Reset
// after the final call has returned, including failure/cancellation.
// FreeRTOS self-deleting tasks must reset explicitly before vTaskDelete().
// A session holds the pinned, SHA-verified model weights, the system language
// model and, with HW1_STT_RESIDENT_MODEL, its resident model, whose arena
// keeps the last call's activations until the next call of the same run
// overwrites them. moveTo frees the resident model first, so an idle session
// the broker hands to its next run (sttKeepWarm) carries only weights and LM:
// PCM, features, activations and text never pass from one run to the next.
class SttLocalSession {
 public:
  SttLocalSession() = default;
  ~SttLocalSession();
  void reset();
  // Verified weights are resident (the next call skips the model load).
  bool warm() const;
  // Hands this idle session's backend to the empty, idle `to`; this one is
  // left empty. A language model the old owner shed under memory pressure
  // gets one load attempt again, as in a new session.
  // A resident model is freed, never handed over (dropCallState); the new
  // owner's first call builds its own.
  void moveTo(SttLocalSession& to);
  // Frees what calls left for this session's later calls, i.e. the resident
  // model and its activation arena (HW1_STT_RESIDENT_MODEL; otherwise
  // nothing); verified weights and the LM stay.
  void dropCallState();
  SttLocalSession(const SttLocalSession&) = delete;
  SttLocalSession& operator=(const SttLocalSession&) = delete;
 private:
  void* backendState_ = nullptr;
  friend bool sttLocalTranscribe(const int16_t*, size_t, char*, size_t,
                                const SttLocalControl&, SttLocalStats&,
                                char*, size_t, SttLocalSession*, const SttFeaturePrefix*);
  friend SttLocalCallBudget sttLocalCallBudget(size_t, const SttLocalSession*);
  friend bool sttSpeakerBegin(SttLocalSession*, size_t, SttSpeakerStatus*);
  friend bool sttSpeakerLabel(SttLocalSession*, const int16_t*, size_t, SttSpeakerLabel*);
  friend bool sttSpeakerSubmit(SttLocalSession*, uint32_t, const int16_t*, size_t, const SttLocalControl&, bool (*)(void*));
  friend void sttSpeakerEnd(SttLocalSession*);
  friend bool sttSpeakerBackground(SttLocalSession*);
  friend bool sttSpeakerTakeLabel(SttLocalSession*, uint32_t*, int8_t*);
  friend void sttSpeakerDrain(SttLocalSession*, uint32_t, const SttLocalControl&);
};

// Cheap availability check for the configured, fixed model path; does not load
// a model. A true result is readiness to attempt a run, not proof of its result.
// Optional reason points to a static actionable message, safe to cache.
bool sttLocalAvailable(char* error, size_t cap, const char** reason = nullptr);
bool sttLocalTranscribe(const int16_t* pcm, size_t samples,
                        char* text, size_t textCap,
                        const SttLocalControl& control, SttLocalStats& stats,
                        char* error, size_t errorCap,
                        SttLocalSession* session = nullptr,
                        const SttFeaturePrefix* prefix = nullptr);
