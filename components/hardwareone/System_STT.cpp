#include "System_STT.h"

#if ENABLE_LOCAL_STT
#if !ENABLE_MICROPHONE
#error "ENABLE_LOCAL_STT requires the shared microphone HAL"
#endif

#include "HAL_Audio.h"
#include "G2_Glasses.h"
#include "System_AuthIdentity.h"
#include "System_Command.h"
#include "System_Utils.h"  // secureClearString and validate-only command guard
#include "System_Debug.h"
#include "System_ESPSR.h"
#include "System_MemUtil.h"
#include "System_Microphone.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "stt/stt_segmenter.h"
#include "stt/quartznet_frontend.h"
#include <ArduinoJson.h>
#include <esp_random.h>
#include <algorithm>
#ifdef ESP_PLATFORM
#include "esp_log.h"
#endif
#include <cmath>
#include <cstring>

#ifndef ESP_PLATFORM
size_t sttPsramFree();  // host builds: the test harness supplies free PSRAM
#endif

// Continuous-session latency features (measured on the P4 with the 15x5
// model, speech end -> caption ~3.6 s -> ~1.7 s together with the runtime's
// HW1_STT_RESIDENT_MODEL). Each is a compile-time switch, default ON (the
// measured design); a features file or -D may set one to 0, which restores
// that part of the earlier behaviour exactly. They apply to every continuous
// session (Dictation, QuickNote, the Transcription page and Conversate).

// Log-mel rows are computed while a phrase is still being captured; the final
// call only computes the rows after the cache and runs CMVN + quantize.
#ifndef HW1_STT_CAPTURE_FEATURES
#define HW1_STT_CAPTURE_FEATURES 1
#endif
// A phrase closed by a pause or the session end is inferred only up to its
// voiced end + 200 ms (published positions and the saved text are unchanged).
#ifndef HW1_STT_TRIM_SILENCE
#define HW1_STT_TRIM_SILENCE 1
#endif
// Needs HW1_STT_TRIM_SILENCE: once a phrase's trimmed input is captured and
// its pause will close it, its final is computed early and published when
// the phrase closes exactly as predicted.
#ifndef HW1_STT_SPECULATIVE_FINAL
#define HW1_STT_SPECULATIVE_FINAL HW1_STT_TRIM_SILENCE
#endif
#if HW1_STT_SPECULATIVE_FINAL && !HW1_STT_TRIM_SILENCE
#error "HW1_STT_SPECULATIVE_FINAL needs HW1_STT_TRIM_SILENCE"
#endif
// A phrase shorter than the 4 s minimum closes after 1 s of silence instead of
// waiting for more speech (a short reply gets its caption).
#ifndef HW1_STT_SHORT_CLOSE
#define HW1_STT_SHORT_CLOSE 1
#endif
// The broker calls the consumer's publishWake (Conversate's caption pump)
// right after it publishes a final or a draft, instead of leaving it to the
// consumer's next poll.
#ifndef HW1_STT_PUBLISH_WAKE
#define HW1_STT_PUBLISH_WAKE 1
#endif
// A native (glasses) pause feeds 1.25 s of silence instead of 1 s, so a short
// phrase whose last 10 ms frame was only partly voiced still reaches the 1 s
// short-close pause.
#ifndef HW1_STT_LONG_PAUSE_FEED
#define HW1_STT_LONG_PAUSE_FEED 1
#endif

namespace {
constexpr uint32_t kWorkerStackBytes = 12288; // IDF task stack sizes are bytes.
constexpr uint32_t kResultRetentionMs = 300000;
constexpr size_t kReadSamples = 640; // 40 ms at the model's fixed 16 kHz rate.
constexpr uint32_t kSourceTimeoutMs = 2000;
// The glasses drop their mic stream on every lens page swap and it is only
// re-armed once a new page is up, so a normal navigation costs ~1-2 s of PCM.
constexpr uint32_t kG2SourceTimeoutMs = 4000;
static uint32_t sttSourceTimeoutMs(AudioSource source) {
  return source == AUDIO_SRC_G2_LEFT ? kG2SourceTimeoutMs : kSourceTimeoutMs;
}
constexpr size_t kTextQueueDepth = 8;

struct STTRun {
  STTOwner owner;
  TranscriptOptions transcriptOptions;
  STTSnapshot snapshot;
  uint32_t startedMs = 0;
  uint32_t completedMs = 0;
  // When continuous capture stopped (0 while recording). The session clock
  // the UIs show is recording time, so it freezes here even though the worker
  // keeps transcribing the tail afterwards.
  uint32_t captureEndedMs = 0;
  uint32_t captureStartedMs = 0;  // audio sample 0 of this session (timeline log)
  uint8_t captureSource = 0;
  // Native G2 (Conversate) capture: the exact left-temple connection that the
  // native session's audio belongs to (0 = ordinary STT-owned capture), and
  // whether that session is paused by the wearer.
  uint32_t nativeG2Generation = 0;
  bool paused = false;
  // The consumer's wake (Conversate: g2ControlWake), called by the worker
  // right after a final or a draft is published, outside gSTTMux, when
  // HW1_STT_PUBLISH_WAKE. The consumer keeps its own periodic poll as a
  // fallback.
  void (*publishWake)() = nullptr;
  bool cancelRequested = false;
  bool finishRequested = false;
  AudioSource requestedSource = AUDIO_SRC_NONE;
  char text[STT_MAX_TEXT + 1] = {};
  uint8_t textHead = 0;
  uint32_t acknowledgedSequence = 0;
  STTDraft draft;  // live mode: provisional text of the utterance in progress
};
static portMUX_TYPE gSTTMux = portMUX_INITIALIZER_UNLOCKED;
static STTRun gSTT;
static STTTextChunk gSTTChunks[kTextQueueDepth];
static void sttContinuousWorker(void*);
static uint32_t gSTTNonce = 0;
static uint32_t gSTTCounter = 0;

static bool sttOwnerLive(STTOwner owner) {
  switch (owner.source) {
    case SOURCE_SERIAL: case SOURCE_WEB: case SOURCE_BLUETOOTH:
    case SOURCE_LOCAL_DISPLAY: case SOURCE_G2_GLASSES:
      return owner.epoch && transportSessionEpochIsLive(owner.source, owner.epoch);
    default: return false;
  }
}

static bool sttSameOwner(STTOwner a, STTOwner b) {
  return a.epoch != 0 && a.source == b.source && a.epoch == b.epoch;
}

static bool sttMatchesLocked(STTOwner owner, STTToken token) {
  return token && gSTT.snapshot.token == token && sttSameOwner(owner, gSTT.owner);
}

static bool sttError(char* error, size_t cap, const char* reason) {
  if (error && cap) snprintf(error, cap, "%s", reason);
  return false;
}

static void sttClearTextLocked() {
  // Volatile clear keeps a discarded private transcript out of retained RAM.
  volatile char* p = gSTT.text;
  for (size_t i = 0; i < sizeof(gSTT.text); ++i) p[i] = 0;
  volatile uint8_t* q = reinterpret_cast<volatile uint8_t*>(gSTTChunks);
  for (size_t i = 0; i < sizeof(gSTTChunks); ++i) q[i] = 0;
  gSTT.textHead = 0;
  gSTT.snapshot.pendingTexts = 0;
  gSTT.snapshot.textReady = false;
  volatile char* d = gSTT.draft.text;
  for (size_t i = 0; i < sizeof(gSTT.draft.text); ++i) d[i] = 0;
  gSTT.draft.sequence = 0;
  ++gSTT.draft.version;
}

// Terminal results have bounded retention. Re-check the original session at
// every broker access; revocation can never transfer a mailbox to a successor.
static void sttReapTerminal() {
  STTOwner owner;
  STTToken token;
  portENTER_CRITICAL(&gSTTMux);
  owner = gSTT.owner;
  token = gSTT.snapshot.token;
  portEXIT_CRITICAL(&gSTTMux);
  const bool live = sttOwnerLive(owner);
  portENTER_CRITICAL(&gSTTMux);
  if (gSTT.snapshot.token == token && !gSTT.snapshot.workerActive &&
      (!live || millis() - gSTT.completedMs >= kResultRetentionMs)) {
    sttClearTextLocked();
  }
  portEXIT_CRITICAL(&gSTTMux);
}

static bool sttCancelled(void* context) {
  const STTToken token = *static_cast<const STTToken*>(context);
  STTOwner owner;
  bool cancelled;
  portENTER_CRITICAL(&gSTTMux);
  owner = gSTT.owner;
  cancelled = token != gSTT.snapshot.token || gSTT.cancelRequested;
  portEXIT_CRITICAL(&gSTTMux);
  return cancelled || !sttOwnerLive(owner);
}

static void sttProgress(void* context, STTLocalPhase phase) {
  const STTToken token = *static_cast<const STTToken*>(context);
  portENTER_CRITICAL(&gSTTMux);
  if (gSTT.snapshot.token == token && gSTT.snapshot.workerActive) {
    gSTT.snapshot.phase = phase;
    gSTT.snapshot.workerStackFreeBytes = uxTaskGetStackHighWaterMark(nullptr);
  }
  portEXIT_CRITICAL(&gSTTMux);
}

static void sttPublishTranscript(STTToken token, const TranscriptStatus& status) {
  bool newError = false;
  portENTER_CRITICAL(&gSTTMux);
  if (gSTT.snapshot.token == token) {
    newError = status.error[0] && !gSTT.snapshot.transcript.error[0];
    gSTT.snapshot.transcript = status;
  }
  portEXIT_CRITICAL(&gSTTMux);
  if (newError) INFO_SYSTEMF("[STT] Transcript saving failed: %s", status.error);
}

// Same recording-scoped G2 support the recorder uses (System_Microphone
// startRecordingInternal): FAST link interval, a lens container (the glasses
// drop mic audio without one) and an immediate stream kick. The heartbeat
// keepalive covers the "stt" owner via g2MicCaptureWanted().
static bool sttG2CaptureBegin() {
  g2MicLinkFastAcquire();
  g2MicEnsureCaptureContainer();
  (void)g2MicKickStream();
  return true;
}
static void sttG2CaptureEnd() {
  g2MicLinkFastRelease();
  g2MicReleaseCaptureContainer();
}

// ---- Keep-warm (sttKeepWarm) -------------------------------------------------
// Between runs the verified model may stay parked in gWarmSession while a
// caller holds it warm. A session carries only the pinned, SHA-verified
// weights and the system LM (System_STTLocal.h), never call data, so any next
// run may take it. gWarmSession belongs to whoever holds the claim (taken
// under gSTTMux): the warm loader while it loads, a worker while it adopts or
// parks, the releaser while it frees. Admission runs one worker at a time and
// a worker parks before it publishes workerActive=false, so the weights are
// only ever in one place and a run never loads a second copy beside them.
#ifdef ESP_PLATFORM
// heap_caps comes in with System_MemUtil.h (above the namespace).
static size_t sttPsramFree() { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }
#endif
constexpr uint32_t kWarmMaxHoldMs = 600000;    // the longest hold a caller may ask
constexpr uint32_t kWarmRetryMs = 60000;       // no new warm load this soon after a failure
constexpr uint32_t kWarmAdoptWaitMs = 20000;   // a run's longest wait on a claim other than a load
constexpr size_t kWarmMinFreePsram = 4u << 20; // less free PSRAM releases the parked model
struct STTWarm {
  uint32_t untilMs = 0;    // hold deadline (millis), while `wanted`
  uint32_t failedMs = 0;   // last failed load (or low-PSRAM release), while `failed`
  size_t loadBytes = 0;    // PSRAM the last successful warm load took (0: none yet)
  bool wanted = false, parked = false, claimed = false, loading = false, failed = false;
};
static STTWarm gWarm;
static STTLocalSession gWarmSession;
static bool sttWarmHeldLocked(uint32_t now) {
  return gWarm.wanted && static_cast<int32_t>(gWarm.untilMs - now) > 0;
}
// The warm load stops early once nobody holds it any more.
static bool sttWarmLoadCancelled(void*) {
  portENTER_CRITICAL(&gSTTMux);
  const bool stop = !sttWarmHeldLocked(millis());
  portEXIT_CRITICAL(&gSTTMux);
  return stop;
}
// Short-lived task: load and verify the model into gWarmSession (its claim
// was taken by sttKeepWarm) with half a second of silence, like a session's
// own warm-up, then park it.
static void sttWarmLoader(void*) {
  const size_t freeBefore = sttPsramFree();
  constexpr size_t kSamples = STT_SAMPLE_RATE / 2;
  auto* silence = static_cast<int16_t*>(ps_alloc(kSamples * sizeof(int16_t), AllocPref::RequirePSRAM, "stt.warm"));
  bool ok = false;
  STTLocalStats stats;
  if (silence) {
    memset(silence, 0, kSamples * sizeof(int16_t));
    char text[STT_MAX_TEXT + 1] = {};
    char error[96] = {};
    const STTLocalControl control{nullptr, sttWarmLoadCancelled, nullptr};
    ok = sttLocalTranscribe(silence, kSamples, text, sizeof(text), control, stats, error,
                            sizeof(error), &gWarmSession) && gWarmSession.warm();
    // Parked, the session holds only weights and the LM (a resident model the
    // load built is freed; the adopting run builds its own).
    gWarmSession.dropCallState();
    free(silence);
  }
  if (!ok) gWarmSession.reset();
  const size_t freeAfter = sttPsramFree();
  const uint32_t now = millis();
  portENTER_CRITICAL(&gSTTMux);
  gWarm.parked = ok;
  if (ok) gWarm.loadBytes = freeBefore > freeAfter ? freeBefore - freeAfter : 0;
  gWarm.claimed = gWarm.loading = false;
  // A load stopped because the hold ended is no failure (no back-off).
  gWarm.failed = !ok && sttWarmHeldLocked(now);
  gWarm.failedMs = now;
  portEXIT_CRITICAL(&gSTTMux);
  // The PSRAM left beside the resident model is what kWarmMinFreePsram guards.
  INFO_SYSTEMF("[STT] Warm model %s (load %lu ms, %u KB PSRAM free)", ok ? "resident" : "not loaded",
               static_cast<unsigned long>(stats.loadMs), static_cast<unsigned>(sttPsramFree() / 1024));
  vTaskDelete(nullptr);
}
// A run's session takes the parked model before its first call. A warm load
// in progress is awaited to its end (it stops by itself once the hold
// lapses; `cancelled` ends the wait) rather than raced with a second copy;
// the caller's capture keeps running meanwhile. Any other claim (a release
// under way) is awaited at most kWarmAdoptWaitMs.
static void sttWarmAdopt(STTLocalSession& session, bool (*cancelled)(void*), void* context) {
  const uint32_t start = millis();
  for (;;) {
    bool take = false, busy = false, loading = false;
    portENTER_CRITICAL(&gSTTMux);
    take = gWarm.parked && !gWarm.claimed;
    if (take) gWarm.claimed = true;
    busy = gWarm.claimed && !take;
    loading = gWarm.loading;
    portEXIT_CRITICAL(&gSTTMux);
    if (take) {
      gWarmSession.moveTo(session);
      portENTER_CRITICAL(&gSTTMux);
      gWarm.parked = gWarm.claimed = false;
      portEXIT_CRITICAL(&gSTTMux);
      return;
    }
    if (!busy || (!loading && millis() - start >= kWarmAdoptWaitMs) || cancelled(context)) return;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
// A one-shot run makes room instead: it takes the parked model (awaiting a
// warm load in progress, like a continuous run) and frees it.
static void sttWarmYield(bool (*cancelled)(void*), void* context) {
  STTLocalSession parked;
  sttWarmAdopt(parked, cancelled, context);
  parked.reset();
}
// A run's end, before workerActive=false: while a hold lasts its model stays
// parked for the next run, otherwise the session is closed as before.
static void sttWarmPark(STTLocalSession& session) {
  const bool warm = session.warm();
  bool park = false;
  portENTER_CRITICAL(&gSTTMux);
  park = warm && sttWarmHeldLocked(millis()) && !gWarm.parked && !gWarm.claimed;
  if (park) gWarm.claimed = true;
  portEXIT_CRITICAL(&gSTTMux);
  if (park) {
    session.moveTo(gWarmSession);
    portENTER_CRITICAL(&gSTTMux);
    gWarm.parked = true;
    gWarm.claimed = false;
    portEXIT_CRITICAL(&gSTTMux);
  }
  session.reset();
}
// Frees the parked model once no hold covers it, or at once for low PSRAM
// (then no warm load for kWarmRetryMs, so it cannot thrash).
static void sttWarmRelease(bool lowMemory) {
  const uint32_t now = millis();
  bool release = false;
  portENTER_CRITICAL(&gSTTMux);
  if (!sttWarmHeldLocked(now)) gWarm.wanted = false;
  release = gWarm.parked && !gWarm.claimed && (lowMemory || !gWarm.wanted);
  if (release) gWarm.claimed = true;
  portEXIT_CRITICAL(&gSTTMux);
  if (!release) return;
  gWarmSession.reset();
  portENTER_CRITICAL(&gSTTMux);
  gWarm.parked = gWarm.claimed = false;
  if (lowMemory) {
    gWarm.failed = true;
    gWarm.failedMs = now;
  }
  portEXIT_CRITICAL(&gSTTMux);
  INFO_SYSTEMF("[STT] Warm model released (%s)", lowMemory ? "low PSRAM" : "idle");
}

static void sttWorker(void*) {
  STTToken token;
  uint32_t captureMs;
  AudioSource requestedSource;
  TranscriptOptions transcriptOptions;
  portENTER_CRITICAL(&gSTTMux);
  token = gSTT.snapshot.token;
  captureMs = gSTT.snapshot.captureLimitMs;
  requestedSource = gSTT.requestedSource;
  transcriptOptions = gSTT.transcriptOptions;
  portEXIT_CRITICAL(&gSTTMux);
  TranscriptSession transcript;
  transcript.begin(transcriptOptions, "local", token);
  sttPublishTranscript(token, transcript.snapshot());

  int16_t* pcm = nullptr;
  size_t samples = 0;
  const size_t capacity = static_cast<size_t>(captureMs) * STT_SAMPLE_RATE / 1000;
  char text[STT_MAX_TEXT + 1] = {};
  char error[96] = {};
  STTLocalStats stats;
  bool ok = false;
  bool claimed = false;
  bool selected = false;
  bool g2Held = false;
  const AudioSource previousSource = audioGetSource();
  AudioSource source = AUDIO_SRC_NONE;

  // The worker owns every blocking call. No UI/command task waits for a model
  // or forcibly deletes this task on cancel, so engine pointers remain valid.
  do {
    if (sttCancelled(&token)) break;
    if (audioCaptureBusy() || micRecordingBusy() || gMicRunning) {
      sttError(error, sizeof(error), "Audio became busy; close SR/mic and retry");
      break;
    }
    pcm = static_cast<int16_t*>(ps_alloc(capacity * sizeof(int16_t),
                                        AllocPref::RequirePSRAM, "stt.pcm"));
    if (!pcm) {
      sttError(error, sizeof(error), "Insufficient PSRAM for bounded capture");
      break;
    }
    // Resolve preference afresh without changing it. Unavailable explicit
    // sources fail instead of silently recording a different microphone.
    source = requestedSource;
    if (source != AUDIO_SRC_NONE && !audioSourceAvailable(source)) {
      sttError(error, sizeof(error), "Selected microphone is unavailable");
      break;
    }
    if (sttCancelled(&token)) break;
    selected = audioSetSource(source);
    if (!selected || !audioCaptureStart("stt", STT_SAMPLE_RATE)) {
      sttError(error, sizeof(error), "Could not claim microphone; close SR/mic first");
      break;
    }
    claimed = true;
    source = audioGetSource();
    // A requested source can disappear between availability and HAL claim.
    // The generic HAL may then fall back; an explicit STT preference must not
    // silently record another microphone. Fail before draining any PCM.
    if (requestedSource != AUDIO_SRC_NONE && source != requestedSource) {
      sttError(error, sizeof(error), "Selected microphone changed during startup");
      break;
    }
    if (source == AUDIO_SRC_G2_LEFT) g2Held = sttG2CaptureBegin();
    // G2 discards the preceding owner's tail; PDM already flushes on start.
    audioTrimBufferedPcm("stt", 0);
    portENTER_CRITICAL(&gSTTMux);
    gSTT.snapshot.state = STTState::Recording;
    gSTT.snapshot.audioSource = static_cast<uint8_t>(source);
    portEXIT_CRITICAL(&gSTTMux);

    const uint32_t captureStartedMs = millis();
    uint32_t lastAudioMs = captureStartedMs;
    while (samples < capacity && !sttCancelled(&token)) {
      bool finish;
      portENTER_CRITICAL(&gSTTMux);
      finish = gSTT.finishRequested;
      portEXIT_CRITICAL(&gSTTMux);
      if (finish) break;
      if (!audioCaptureOwnedBy("stt") || !audioCaptureActive() ||
          !audioSourceAvailable(source)) {
        sttError(error, sizeof(error), "Microphone source lost");
        break;
      }
      const size_t requested = std::min(kReadSamples, capacity - samples);
      const size_t count = audioReadPcm(pcm + samples, requested, 50);
      if (count > requested) {
        sttError(error, sizeof(error), "Invalid microphone sample count");
        break;
      }
      if (count) {
        // Input contract is raw HAL PCM. No recorder pre-emphasis, software
        // gain or DC state is applied here; the model frontend owns its DSP.
        double sum = 0;
        for (size_t i = 0; i < count; ++i) {
          const double v = pcm[samples + i];
          sum += v * v;
        }
        samples += count;
        lastAudioMs = millis();
        const int level = std::min(100, static_cast<int>(
            std::sqrt(sum / count) * 100.0 / 32768.0));
        portENTER_CRITICAL(&gSTTMux);
        gSTT.snapshot.recordedSamples = samples;
        gSTT.snapshot.level = level;
        portEXIT_CRITICAL(&gSTTMux);
      } else {
        vTaskDelay(pdMS_TO_TICKS(1));
      }
      if (millis() - lastAudioMs >= sttSourceTimeoutMs(source) ||
          millis() - captureStartedMs > captureMs + sttSourceTimeoutMs(source)) {
        sttError(error, sizeof(error), "Microphone PCM delivery timed out");
        break;
      }
    }
    if (error[0] || sttCancelled(&token)) break;
    if (samples < STT_SAMPLE_RATE / 10) {
      sttError(error, sizeof(error), "Capture too short (minimum 100 ms)");
      break;
    }
    portENTER_CRITICAL(&gSTTMux);
    gSTT.snapshot.state = STTState::Transcribing;
    gSTT.snapshot.level = 0;
    portEXIT_CRITICAL(&gSTTMux);
    STTLocalControl control{&token, sttCancelled, sttProgress};
    // One-shot runs keep the self-contained backend path: a kept-warm model
    // is freed first rather than loaded a second time beside this call's.
    sttWarmYield(sttCancelled, &token);
    ok = sttLocalTranscribe(pcm, samples, text, sizeof(text), control,
                            stats, error, sizeof(error));
    if (ok && !memchr(text, '\0', sizeof(text))) {
      ok = false;
      sttError(error, sizeof(error), "Backend transcript exceeds output limit");
    }
  } while (false);

  portENTER_CRITICAL(&gSTTMux);
  gSTT.snapshot.state = STTState::Stopping;
  gSTT.snapshot.level = 0;
  portEXIT_CRITICAL(&gSTTMux);
  // Keep admission closed until our exact HAL lease is actually released.
  // If a backend stop is still unwinding, stay Stopping rather than publishing
  // a false terminal state or allowing a new run to reuse its owner tag.
  if (claimed || audioCaptureOwnedBy("stt")) {
    audioCaptureStop("stt");
    while (audioCaptureOwnedBy("stt")) vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (g2Held) sttG2CaptureEnd();
  if (selected && !audioCaptureBusy()) {
    audioSetSource(audioSourceAvailable(previousSource) ? previousSource
                                                       : AUDIO_SRC_NONE);
  }
  if (pcm) {
    // Raw audio is private and not persisted. The buffer is freed only after
    // synchronous inference and HAL teardown have both finished.
    volatile int16_t* privatePCM = pcm;
    for (size_t i = 0; i < capacity; ++i) privatePCM[i] = 0;
    free(pcm);
  }
  const bool cancelled = sttCancelled(&token);
  bool acceptedText = false;
  portENTER_CRITICAL(&gSTTMux);
  if (gSTT.snapshot.token == token) {
    sttClearTextLocked();
    gSTT.snapshot.stats = stats;
    gSTT.snapshot.recordedSamples = samples;
    gSTT.snapshot.state = (cancelled || gSTT.cancelRequested) ? STTState::Cancelled
                              : ok ? STTState::Done : STTState::Failed;
    if (gSTT.snapshot.state == STTState::Done) {
      memcpy(gSTT.text, text, sizeof(gSTT.text));
      gSTT.snapshot.textReady = true;
      acceptedText = true;
    } else if (gSTT.snapshot.state == STTState::Failed) {
      snprintf(gSTT.snapshot.error, sizeof(gSTT.snapshot.error), "%s",
               error[0] ? error : "Local transcription failed");
    }
  }
  portEXIT_CRITICAL(&gSTTMux);
  // Recognition and saving have separate outcomes. Admission remains closed
  // through filesystem work; a late mailbox retirement never reopens the file.
  if (acceptedText) (void)transcript.append(1, text, strlen(text));
  const bool finallyCancelled = sttCancelled(&token);
  // This fixes the saved outcome before finalization I/O. A cancel arriving
  // during finish only retires delivery; it cannot rewrite an accepted file.
  transcript.finish(finallyCancelled ? "cancelled" : ok ? "done" : "failed");
  sttPublishTranscript(token, transcript.snapshot());
  // Include the optional writer/VFS call depth in this worker's final margin.
  const uint32_t stackFree = uxTaskGetStackHighWaterMark(nullptr);
  portENTER_CRITICAL(&gSTTMux);
  if (gSTT.snapshot.token == token) {
    gSTT.snapshot.workerStackFreeBytes = stackFree;
    if (finallyCancelled || gSTT.cancelRequested) {
      sttClearTextLocked();
      gSTT.snapshot.state = STTState::Cancelled;
    }
    gSTT.completedMs = millis();
    gSTT.snapshot.workerActive = false;
  }
  portEXIT_CRITICAL(&gSTTMux);
  volatile char* privateText = text;
  for (size_t i = 0; i < sizeof(text); ++i) privateText[i] = 0;
  vTaskDelete(nullptr);
}
// Independent producer/consumer tasks keep draining the microphone while a
// synchronous backend works. This bounded pipeline has no total-duration cap.
constexpr size_t kAudioQueueDepth = 3;
// Continuous phrases are capped at 10 s (was STT_MAX_CAPTURE_MS, 20 s): a longer stretch without a pause is
// cut at its quietest 10 ms in the last 2 s and handed to the worker while capture continues. One-shot captures
// keep STT_MAX_CAPTURE_MS. Halves the five session audio buffers, the feature caches and the resident arena.
constexpr uint32_t kStreamPhraseMs = 10000;
constexpr size_t kStreamSamples = STT_SAMPLE_RATE * kStreamPhraseMs / 1000;
constexpr size_t kStreamReadSamples = 1024; // Whole PDM DMA blocks; segmenting is downstream.
constexpr uint32_t kCaptureStackBytes = 8192; // 2 KiB PCM block plus HAL/BLE startup and G2 lens container depth.
#if HW1_STT_CAPTURE_FEATURES
// Log-mel rows are computed while a phrase is captured, by a separate
// priority-1 task on core 0 (below the STT worker and ESP-DL's workers at 2,
// so it only uses idle time; never the capture task).
// The capture task only copies each PCM block into a stream ring and
// publishes where the phrase in progress starts and how long it is. At the
// close the phrase's cache travels with its queue slot; the worker copies the
// finished rows, computes the rest and runs CMVN + quantize (bit-identical to
// the batch frontend: hw1::stt::log_mel_frame). Drafts use the same rows. The
// pool (2 x 512 KB rows + 128 KB ring) is allocated after the warm-up, so the
// LM and the resident model are admitted exactly as without it.
constexpr size_t kFeRingSamples = 65536;              // 4.1 s of stream; power of two
constexpr size_t kFeMaxRows = kStreamSamples / 160;  // the 10 s phrase cap
constexpr int kFeCaches = 2;                          // phrase in progress + one queued
constexpr size_t kFeBatchRows = 8;
constexpr uint32_t kFeStackBytes = 3072;
constexpr size_t kFeRowFloats = hw1::stt::kMelBins;
enum class FeState : uint8_t { Free, Live, Attached, Orphan };
struct FeCache {
  float* rows = nullptr;        // kFeMaxRows x 64 floats, fixed for the session
  uint64_t start = 0;           // stream sample of the phrase's first sample
  size_t frames = 0;            // committed rows (final: never rewritten)
  uint32_t gen = 0;             // bumped on every reuse
  FeState state = FeState::Free;
  bool overrun = false;         // the feature task fell a ring behind: stop
};
#endif
#if HW1_STT_TRIM_SILENCE
// A phrase that closes on a pause, or at the session end, is transcribed only
// up to its voiced end + 200 ms. The voiced end is the end of the last
// ambient-voiced run longer than 30 ms (the
// segmenter's trimCut = max(16, 2 x floor) gate with the silence-blip rule),
// never the peak/8 stop gate, so quiet word endings stay in. Hard-limit cuts
// are never trimmed. Only the inference copy is shortened: the published
// start/end samples, the saved transcript and the wipe cover the whole phrase.
constexpr size_t kTrimTailSamples = STT_SAMPLE_RATE / 5;
#endif
#if HW1_STT_SPECULATIVE_FINAL
// Speculative final, on top of the trim: once a phrase's voiced end + 200 ms
// has been captured and its pause would close it as an ordinary Pause
// (Segmenter::speculation), the capture task snapshots exactly the trimmed
// input into the draft buffer (no extra PSRAM; a running draft is preempted
// first) and the worker starts the final early. When the phrase then closes
// on that pause with the same start and voiced end, the queued slot is tagged
// with the request id and the finished result is published as its final:
// bit-identical to the trimmed final for the same audio. Speech that resumes
// (a new voiced run), a hard-limit cut, a session end or any other close
// discards it. The capture task never logs; the worker prints TL spec_*.
#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define STT_SPEC_TEXT_ATTR EXT_RAM_BSS_ATTR
#else
#define STT_SPEC_TEXT_ATTR
#endif
STT_SPEC_TEXT_ATTR static char gSpecText[STT_MAX_TEXT + 1];  // worker only
#endif
enum class StreamSlotState : uint8_t { Free, Ready, Processing };
struct StreamSlot {
  int16_t* pcm = nullptr;
  size_t samples = 0;
  uint64_t start = 0, end = 0;
  uint32_t sequence = 0;
  uint32_t queuedMs = 0;  // when the segmenter closed this segment (sttperf)
  bool forced = false;
#if HW1_STT_CAPTURE_FEATURES
  int fe = -1;            // attached feature cache (-1: none)
  uint32_t feGen = 0;
  size_t feRows = 0;      // rows of that cache valid for this phrase
#endif
#if HW1_STT_TRIM_SILENCE
  size_t voicedEnd = 0;   // SpeechSegment::voiced_end_samples
  hw1::stt::SegmentEnd endKind = hw1::stt::SegmentEnd::Pause;
#endif
#if HW1_STT_SPECULATIVE_FINAL
  uint32_t specId = 0;    // speculative request this phrase matched (0: none)
#endif
#if HW1_STT_SHORT_CLOSE
  bool pauseEnd = false;  // closed on a pause (TL short)
#endif
  StreamSlotState state = StreamSlotState::Free;
};
struct StreamRun {
  STTToken token = 0;
  AudioSource requested = AUDIO_SRC_NONE;
  int16_t* working = nullptr;
  StreamSlot slots[kAudioQueueDepth];
  hw1::stt::Segmenter segmenter;
  // Live mode: the capture task copies the utterance in progress here; the
  // worker transcribes it when no finished segment is waiting.
  int16_t* draftPcm = nullptr;
  size_t draftSamples = 0;
  uint32_t draftSequence = 0;
  size_t draftTakenSamples = 0;  // capture-side: length at the last snapshot
  bool draftReady = false, draftBusy = false;
  // The running draft has finished its frontend, so it no longer reads draftPcm (the
  // network and decoder work on its features); a speculative snapshot may overwrite it.
  bool draftPastPcm = false;
  // Compute time of the last completed draft. The next snapshot waits for at
  // least this much new audio, so drafts can never occupy more than about
  // half the worker's time, whatever the model's cost (15x5 ~ 3x the 5x5).
  uint32_t lastDraftMs = 0;
  bool captureDone = false;
  bool failed = false;
  uint32_t clockMs = 0;
  uint32_t nativeGeneration = 0;  // see STTRun::nativeG2Generation
  void (*publishWake)() = nullptr;  // see STTRun::publishWake (worker only)
  // Glasses-mic faults survived instead of ending the session (capture task
  // counts under gSTTMux; the worker logs changes). Conversate owns the
  // session's lifetime, and a few lost 10 ms frames or one dropped phrase
  // must not end the captions or split the saved transcript.
  uint32_t lossEvents = 0, droppedSegments = 0, stalls = 0;
  uint32_t loggedFaults = 0;  // worker-side: sum already logged
#if HW1_STT_CAPTURE_FEATURES
  // Capture-time features (under gSTTMux, except the contents of
  // rows/ring/workspace).
  FeCache fe[kFeCaches];
  int16_t* feRing = nullptr;
  hw1::stt::FrontendWorkspace* feWork = nullptr;
  uint64_t feRingStart = 0, feRingHead = 0;  // stream samples [start, head) were written
  bool feRingStarted = false;
  int feLive = -1;            // cache of the phrase in progress
  size_t feLiveLen = 0;
  TaskHandle_t feTask = nullptr;
  bool feStop = false, feRunning = false;
  int draftFe = -1;           // cache rows a pending draft snapshot may use
  uint32_t draftFeGen = 0;
  size_t draftFeRows = 0;
  uint32_t feRowsComputed = 0, feOverruns = 0, feFinals = 0, feCachedFinals = 0;
  uint32_t feCachedRows = 0, feDraftRows = 0;
#endif
#if HW1_STT_SPECULATIVE_FINAL
  // Speculative final (all under gSTTMux). Capture task: the live request
  // (id, segment start, voiced end, input length) and its snapshot state.
  uint32_t specNextId = 0, specLiveId = 0, specSequence = 0;
  uint64_t specStart = 0;
  size_t specVoiced = 0, specSamples = 0;
  bool specCopied = false, specWanted = false;
  // Snapshot handed to the worker (in draftPcm), the run and a held result.
  bool specReady = false, specBusy = false;
  uint32_t specReadyId = 0, specReadySequence = 0, specRunId = 0, specDoneId = 0;
  uint64_t specReadyStart = 0;
  size_t specReadySamples = 0, specDoneSamples = 0;
  uint32_t specDoneStartMs = 0;
  STTLocalStats specDoneStats;
#if HW1_STT_CAPTURE_FEATURES
  // Feature cache of the phrase the snapshot was taken from.
  int specReadyFe = -1;
  uint32_t specReadyFeGen = 0;
#endif
#endif
};
// Timeline level samples: written by the capture task under gSTTMux, printed
// by the STT worker (never by the real-time capture task).
#ifdef ESP_PLATFORM
struct TLLevel { uint32_t t; uint16_t rms, noise, thr; bool open; };
static constexpr uint32_t kTLLevels = 64;
static TLLevel gTLLevels[kTLLevels];
static uint32_t gTLLevelHead = 0, gTLLevelTail = 0;
#endif
static void sttDrainLevelLog(StreamRun& run) {
#ifdef ESP_PLATFORM
  portENTER_CRITICAL(&gSTTMux);
  const uint32_t loss = run.lossEvents, dropped = run.droppedSegments, stalls = run.stalls;
  const uint32_t overruns = gSTT.snapshot.audioOverruns;
  portEXIT_CRITICAL(&gSTTMux);
  if (loss + dropped + stalls != run.loggedFaults) {
    run.loggedFaults = loss + dropped + stalls;
    ESP_LOGW("STT", "TL fault audio_loss=%lu (frames %lu) dropped_phrases=%lu mic_stalls=%lu; session continues",
             (unsigned long)loss, (unsigned long)overruns, (unsigned long)dropped, (unsigned long)stalls);
  }
  for (;;) {
    TLLevel e{};
    bool have = false;
    portENTER_CRITICAL(&gSTTMux);
    if (gTLLevelHead - gTLLevelTail > kTLLevels) gTLLevelTail = gTLLevelHead - kTLLevels;
    if (gTLLevelTail != gTLLevelHead) { e = gTLLevels[gTLLevelTail++ % kTLLevels]; have = true; }
    portEXIT_CRITICAL(&gSTTMux);
    if (!have) break;
    ESP_LOGI("STT", "TL level t=%lu rms=%u noise=%u thr=%u open=%d", (unsigned long)e.t,
             unsigned(e.rms), unsigned(e.noise), unsigned(e.thr), e.open ? 1 : 0);
  }
#else
  (void)run;
#endif
}
static void sttStreamTick(StreamRun& run) {
  portENTER_CRITICAL(&gSTTMux);
  const uint32_t now = millis();
  gSTT.snapshot.sessionMs += static_cast<uint32_t>(now - run.clockMs);
  run.clockMs = now;
  portEXIT_CRITICAL(&gSTTMux);
}
static void sttStreamFail(StreamRun& run, const char* message) {
  portENTER_CRITICAL(&gSTTMux);
  if (!run.failed) snprintf(gSTT.snapshot.error, sizeof(gSTT.snapshot.error), "%s", message);
  run.failed = true;
  portEXIT_CRITICAL(&gSTTMux);
}
static bool sttStreamCancelled(void* context) {
  auto& run = *static_cast<StreamRun*>(context);
  portENTER_CRITICAL(&gSTTMux);
  const bool failed = run.failed;
  portEXIT_CRITICAL(&gSTTMux);
  return failed || sttCancelled(&run.token);
}
// A draft is only provisional text. Abandon it (the runtime checks between
// model stages) as soon as a finished segment is queued, so a final result
// never waits behind a whole draft of a long utterance.
static bool sttDraftPreempted(void* context) {
  auto& run = *static_cast<StreamRun*>(context);
  bool finalWaiting = false;
  portENTER_CRITICAL(&gSTTMux);
  for (const auto& candidate : run.slots) {
    if (candidate.state == StreamSlotState::Ready) { finalWaiting = true; break; }
  }
#if HW1_STT_SPECULATIVE_FINAL
  // The speculative snapshot needs the draft buffer (specWanted), or was already copied
  // over it after the draft's frontend and outranks the draft (specReady).
  if (run.specWanted || run.specReady) finalWaiting = true;
#endif
  portEXIT_CRITICAL(&gSTTMux);
  return finalWaiting || sttStreamCancelled(context);
}
#if HW1_STT_SPECULATIVE_FINAL
// A speculative request id is still worth something while it is the live
// request or a queued/processing phrase matched it. Caller holds gSTTMux.
static bool sttSpecReferencedLocked(const StreamRun& run, uint32_t id) {
  if (!id) return false;
  if (id == run.specLiveId) return true;
  for (const auto& c : run.slots)
    if (c.state != StreamSlotState::Free && c.specId == id) return true;
  return false;
}
// Stop a speculative run whose phrase changed, or when a final it does not
// match is waiting; keep going for the phrase it matched. Only the oldest
// Ready phrase counts, because the worker takes that one next; later phrases
// waiting behind a matched one do not abort the run (which made the matched
// phrase start again from scratch: up to ~1.2 s close->start).
static bool sttSpecPreempted(void* context) {
  auto& run = *static_cast<StreamRun*>(context);
  bool stop;
  portENTER_CRITICAL(&gSTTMux);
  const StreamSlot* oldest = nullptr;
  for (const auto& candidate : run.slots)
    if (candidate.state == StreamSlotState::Ready && (!oldest || candidate.sequence < oldest->sequence))
      oldest = &candidate;
  stop = oldest ? oldest->specId != run.specRunId : run.specRunId != run.specLiveId;
  portEXIT_CRITICAL(&gSTTMux);
  return stop || sttStreamCancelled(context);
}
#endif
static void sttStreamProgress(void* context, STTLocalPhase phase) {
  auto& run = *static_cast<StreamRun*>(context);
  sttProgress(&run.token, phase);
  sttStreamTick(run);
}
// A draft reports Inference once its features are quantized; from then on it never
// reads draftPcm again (the runtime reads PCM only in its frontend), so sttSpecCapture may
// copy a speculative snapshot over the buffer at once instead of at the next capture block.
static void sttDraftProgress(void* context, STTLocalPhase phase) {
  auto& run = *static_cast<StreamRun*>(context);
  if (phase == STTLocalPhase::Inference || phase == STTLocalPhase::Decoding) {
    portENTER_CRITICAL(&gSTTMux);
    run.draftPastPcm = true;
    portEXIT_CRITICAL(&gSTTMux);
  }
  sttStreamProgress(context, phase);
}
static void sttWipe(void* memory, size_t bytes) {
  if (!memory) return;
  volatile uint8_t* p = static_cast<volatile uint8_t*>(memory);
  while (bytes--) *p++ = 0;
}
// ---- sttperf: per-segment pacing and profiling (never transcript text) ----
struct STTPerfTask { char name[12]; uint8_t pct; };
struct STTPerfRecord {
  uint32_t sequence = 0, audioMs = 0, queuedMs = 0, startMs = 0, doneMs = 0, deliveredMs = 0;
  uint8_t backlog = 0, idle0 = 255, idle1 = 255;
  STTLocalStats stats;
  STTPerfTask tasks[4] = {};
};
constexpr size_t kPerfRecords = 16;
static STTPerfRecord gSTTPerf[kPerfRecords];
static uint32_t gSTTPerfCount = 0;
static volatile bool gSTTPerfLog = false;

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "freertos/task.h"
#define STT_PERF_TEXT_ATTR EXT_RAM_BSS_ATTR
#else
#define STT_PERF_TEXT_ATTR
#endif
#ifdef ESP_PLATFORM
// CPU share per task over one segment's processing, from FreeRTOS run-time
// counters (percent of one core; IDLE0/IDLE1 show each core's headroom).
struct STTTaskSample {
  TaskStatus_t* tasks = nullptr;
  UBaseType_t count = 0;
  configRUN_TIME_COUNTER_TYPE total = 0;
};
constexpr UBaseType_t kPerfMaxTasks = 72;
static bool sttTaskSample(STTTaskSample& s) {
  if (!s.tasks)
    s.tasks = static_cast<TaskStatus_t*>(heap_caps_malloc(kPerfMaxTasks * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM));
  s.count = 0;
  if (!s.tasks || uxTaskGetNumberOfTasks() > kPerfMaxTasks) return false;
  s.count = uxTaskGetSystemState(s.tasks, kPerfMaxTasks, &s.total);
  return s.count > 0;
}
static STTTaskSample gPerfBefore, gPerfAfter;
static void sttTaskShares(STTPerfRecord& r) {
  const auto span = gPerfAfter.total - gPerfBefore.total;
  if (!gPerfBefore.count || !gPerfAfter.count || !span) return;
  for (UBaseType_t i = 0; i < gPerfAfter.count; ++i) {
    const TaskStatus_t& a = gPerfAfter.tasks[i];
    configRUN_TIME_COUNTER_TYPE before = 0;
    bool found = false;
    for (UBaseType_t j = 0; j < gPerfBefore.count; ++j)
      if (gPerfBefore.tasks[j].xHandle == a.xHandle) { before = gPerfBefore.tasks[j].ulRunTimeCounter; found = true; break; }
    if (!found) continue;
    const uint32_t pct = static_cast<uint32_t>((static_cast<uint64_t>(a.ulRunTimeCounter - before) * 100) / span);
    const uint8_t share = static_cast<uint8_t>(std::min<uint32_t>(pct, 100));
    if (!strncmp(a.pcTaskName, "IDLE", 4)) {
      const char core = a.pcTaskName[strlen(a.pcTaskName) - 1];
      (core == '1' ? r.idle1 : r.idle0) = share;
      continue;
    }
    for (int k = 0; k < 4; ++k) {
      if (share > r.tasks[k].pct || !r.tasks[k].name[0]) {
        for (int m = 3; m > k; --m) r.tasks[m] = r.tasks[m - 1];
        snprintf(r.tasks[k].name, sizeof(r.tasks[k].name), "%s", a.pcTaskName);
        r.tasks[k].pct = share;
        break;
      }
    }
  }
}
#endif

static int sttPerfFormat(const STTPerfRecord& r, char* out, size_t cap) {
  const STTLocalStats& s = r.stats;
  const uint32_t total = r.doneMs - r.startMs;
  int n = snprintf(out, cap,
      "seg %lu audio %.1fs wait %.1fs | load %lu fe %lu (cond %lu fft %lu mel %lu norm %lu q %lu) "
      "inf %lu (yield %lu; slow #%u %lu #%u %lu #%u %lu) dec %lu %s | total %.1fs rtf %.2f lag %.1fs",
      (unsigned long)r.sequence, r.audioMs / 1000.0, (r.startMs - r.queuedMs) / 1000.0,
      (unsigned long)s.loadMs, (unsigned long)s.frontendMs,
      (unsigned long)(s.frontendConditionUs / 1000), (unsigned long)(s.frontendFftUs / 1000),
      (unsigned long)(s.frontendMelUs / 1000), (unsigned long)(s.frontendNormUs / 1000),
      (unsigned long)(s.quantizeUs / 1000), (unsigned long)s.inferenceMs,
      (unsigned long)(s.inferenceYieldUs / 1000),
      (unsigned)s.slowStage[0], (unsigned long)(s.slowStageUs[0] / 1000),
      (unsigned)s.slowStage[1], (unsigned long)(s.slowStageUs[1] / 1000),
      (unsigned)s.slowStage[2], (unsigned long)(s.slowStageUs[2] / 1000),
      (unsigned long)s.decodeMs, s.lmUsed ? "lm" : "greedy",
      total / 1000.0, r.audioMs ? (double)total / r.audioMs : 0.0, (r.doneMs - r.queuedMs) / 1000.0);
  if (n < 0 || (size_t)n >= cap) return n;
  if (r.deliveredMs)
    n += snprintf(out + n, cap - n, " read +%.1fs", (r.deliveredMs - r.doneMs) / 1000.0);
  n += snprintf(out + n, cap - n, " backlog %u | cpu", (unsigned)r.backlog);
  for (const auto& t : r.tasks)
    if (t.name[0] && (size_t)n < cap) n += snprintf(out + n, cap - n, " %s %u%%", t.name, (unsigned)t.pct);
  if (r.idle0 != 255 && (size_t)n < cap)
    n += snprintf(out + n, cap - n, " idle0 %u%% idle1 %u%%", (unsigned)r.idle0, (unsigned)r.idle1);
  return n;
}

static bool sttQueueSegment(StreamRun& run) {
  const auto* segment = run.segmenter.segment();
  if (!segment) return true;
  StreamSlot* slot = nullptr;
  portENTER_CRITICAL(&gSTTMux);
  for (auto& candidate : run.slots) {
    if (candidate.state == StreamSlotState::Free) { slot = &candidate; break; }
  }
  const bool exhausted = gSTT.snapshot.segmentsCaptured == UINT32_MAX;
  portEXIT_CRITICAL(&gSTTMux);
  if (!slot && !exhausted && run.nativeGeneration) {
    // Conversate: drop this phrase (it is never shown or saved) and keep the
    // session; the backlog drains and later phrases continue in order.
    portENTER_CRITICAL(&gSTTMux);
    ++run.droppedSegments;
#if HW1_STT_CAPTURE_FEATURES
    if (run.feLive >= 0) { run.fe[run.feLive].state = FeState::Orphan; run.feLive = -1; }
#endif
#if HW1_STT_SPECULATIVE_FINAL
    run.specLiveId = 0; run.specWanted = false;  // that phrase is gone
#endif
    portEXIT_CRITICAL(&gSTTMux);
    // Release it, or the segmenter stays Ready and capture stalls until a slot
    // frees (then queues the "dropped" phrase late after all).
    return run.segmenter.release();
  }
  if (!slot || exhausted) {
    sttStreamFail(run, exhausted ? "STT sequence exhausted; start a new session"
                                : "STT audio queue full; backend cannot keep up");
    return false;
  }
  // Only the capture producer claims Free slots; the consumer sees Ready only
  // after this copy and metadata publication complete under the same lock.
  memcpy(slot->pcm, segment->pcm, segment->samples * sizeof(int16_t));
  slot->samples = segment->samples;
  slot->start = segment->start_sample;
  slot->end = segment->end_sample;
  slot->forced = segment->end == hw1::stt::SegmentEnd::HardLimit;
#if HW1_STT_TRIM_SILENCE
  slot->voicedEnd = segment->voiced_end_samples;
  slot->endKind = segment->end;
#endif
#if HW1_STT_SHORT_CLOSE
  slot->pauseEnd = segment->end == hw1::stt::SegmentEnd::Pause;
#endif
  slot->queuedMs = millis();
  portENTER_CRITICAL(&gSTTMux);
#if HW1_STT_CAPTURE_FEATURES
  // The phrase's feature cache goes with it; rows that read past its
  // published end (or a sub-320-sample phrase, which inference pads) are not
  // used.
  slot->fe = -1; slot->feRows = 0;
  if (run.feLive >= 0) {
    FeCache& c = run.fe[run.feLive];
    if (c.state == FeState::Live && c.start == segment->start_sample && segment->samples >= 320) {
      slot->fe = run.feLive; slot->feGen = c.gen;
      slot->feRows = std::min(c.frames, segment->samples / 160);
      c.state = FeState::Attached;
    } else {
      c.state = FeState::Orphan;
    }
    run.feLive = -1;
  }
#endif
#if HW1_STT_SPECULATIVE_FINAL
  // The live speculative request belonged to this phrase. It matches only if
  // the phrase closed on a pause with the same start and voiced end, i.e. if
  // the trim will infer exactly the snapshot; either way the request is now
  // settled.
  slot->specId = run.specLiveId && segment->end == hw1::stt::SegmentEnd::Pause &&
      segment->start_sample == run.specStart && segment->voiced_end_samples == run.specVoiced &&
      run.specVoiced + kTrimTailSamples <= segment->samples ? run.specLiveId : 0;
  run.specLiveId = 0; run.specWanted = false;
#endif
  slot->sequence = ++gSTT.snapshot.segmentsCaptured;
  slot->state = StreamSlotState::Ready;
  ++gSTT.snapshot.pendingAudio;
  portEXIT_CRITICAL(&gSTTMux);
  return run.segmenter.release();
}
#if HW1_STT_CAPTURE_FEATURES
// Computes the phrase in progress's final rows from the stream ring.
// Single writer of every cache's rows; a batch is committed only if its cache
// is still the same live phrase and the ring was not overwritten under it.
static void sttFeatureWorker(void* context) {
  auto& run = *static_cast<StreamRun*>(context);
  bool stop = false;
  while (!stop) {
    (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
    for (;;) {
      int idx = -1;
      uint32_t gen = 0;
      uint64_t start = 0, head = 0;
      size_t done = 0, len = 0;
      portENTER_CRITICAL(&gSTTMux);
      stop = run.feStop;
      if (run.feLive >= 0 && run.fe[run.feLive].state == FeState::Live && !run.fe[run.feLive].overrun) {
        idx = run.feLive;
        gen = run.fe[idx].gen; start = run.fe[idx].start; done = run.fe[idx].frames;
        len = run.feLiveLen;
      }
      head = run.feRingHead;
      portEXIT_CRITICAL(&gSTTMux);
      if (stop || idx < 0) break;
      const size_t stable = len >= 320 ? std::min(len / 160, kFeMaxRows) : 0;
      if (done >= stable) break;
      const size_t end = std::min(stable, done + kFeBatchRows);
      // Oldest sample this batch reads (row r: [r*160-161, r*160+160)).
      const uint64_t lo = start + (done * 160 > 161 ? done * 160 - 161 : 0);
      const bool safe = lo + kFeRingSamples >= head + kStreamReadSamples;
      if (safe)
        for (size_t r = done; r < end; ++r)
          (void)hw1::stt::log_mel_frame_ring(run.feRing, kFeRingSamples - 1, start, len, r,
                                             run.fe[idx].rows + r * kFeRowFloats, run.feWork);
      portENTER_CRITICAL(&gSTTMux);
      FeCache& c = run.fe[idx];
      if (c.gen == gen && c.state == FeState::Live) {
        // The capture task writes at most one block past the published head.
        if (safe && lo + kFeRingSamples >= run.feRingHead + kStreamReadSamples) {
          c.frames = end;
          run.feRowsComputed += static_cast<uint32_t>(end - done);
        } else {
          c.overrun = true;
          ++run.feOverruns;
        }
      }
      portEXIT_CRITICAL(&gSTTMux);
    }
  }
  portENTER_CRITICAL(&gSTTMux);
  run.feRunning = false;
  portEXIT_CRITICAL(&gSTTMux);
  vTaskDelete(nullptr);
}
// Capture task, per block: copy PCM into the stream ring (before the
// segmenter sees it) ...
static void sttFeatureRingWrite(StreamRun& run, int16_t* ring, const int16_t* block, size_t count) {
  const uint64_t base = run.segmenter.samples_seen();
  const size_t at = static_cast<size_t>(base & (kFeRingSamples - 1));
  const size_t first = std::min(count, kFeRingSamples - at);
  memcpy(ring + at, block, first * sizeof(int16_t));
  if (count > first) memcpy(ring, block + first, (count - first) * sizeof(int16_t));
  portENTER_CRITICAL(&gSTTMux);
  if (!run.feRingStarted) { run.feRingStarted = true; run.feRingStart = base; }
  run.feRingHead = base + count;
  portEXIT_CRITICAL(&gSTTMux);
}
// ... and after it: give the phrase in progress a cache (one that started
// inside the ring), publish its length, retire one whose phrase vanished.
static void sttFeatureTrack(StreamRun& run) {
  uint64_t liveStart = 0;
  size_t liveLen = 0;
  const bool active = run.segmenter.live_view(&liveStart, &liveLen);
  portENTER_CRITICAL(&gSTTMux);
  if (run.feLive >= 0 && (!active || run.fe[run.feLive].start != liveStart)) {
    run.fe[run.feLive].state = FeState::Orphan;  // discarded candidate
    run.feLive = -1;
  }
  if (active && run.feLive < 0 && run.feRingStarted && liveStart >= run.feRingStart) {
    for (int i = 0; i < kFeCaches; ++i) {
      if (run.fe[i].state != FeState::Free) continue;
      FeCache& c = run.fe[i];
      c.state = FeState::Live; c.start = liveStart; c.frames = 0; c.overrun = false; ++c.gen;
      run.feLive = i;
      break;
    }
  }
  if (run.feLive >= 0) run.feLiveLen = liveLen;
  const TaskHandle_t task = run.feLive >= 0 ? run.feTask : nullptr;
  portEXIT_CRITICAL(&gSTTMux);
  if (task) xTaskNotifyGive(task);
}
#endif
#if HW1_STT_SPECULATIVE_FINAL
// Capture task, after each pushed block. Issues/keeps/drops the live
// speculative request and copies its snapshot into the draft buffer when that
// buffer is free (a running draft is asked to stop first). Never logs.
static void sttSpecCapture(StreamRun& run) {
  if (!run.draftPcm) return;
  uint64_t start = 0;
  size_t voiced = 0, samples = 0, liveSamples = 0;
  const int16_t* live = nullptr;
  const bool want = run.segmenter.speculation(kTrimTailSamples, &start, &voiced, &samples) &&
                    run.segmenter.in_progress(&live, &liveSamples) && samples <= liveSamples;
  bool copy = false;
  portENTER_CRITICAL(&gSTTMux);
  if (!want) {
    run.specLiveId = 0; run.specWanted = false;
  } else {
    if (!run.specLiveId || run.specStart != start || run.specVoiced != voiced) {
      if (!++run.specNextId) ++run.specNextId;
      run.specLiveId = run.specNextId;
      run.specStart = start; run.specVoiced = voiced; run.specSamples = samples;
      run.specSequence = gSTT.snapshot.segmentsCaptured + 1;
      run.specCopied = false;
    }
    if (!run.specCopied) {
      // A running draft past its frontend no longer reads the buffer either.
      if ((!run.draftBusy || run.draftPastPcm) && !run.specBusy) {
        // Nothing reads the buffer until a Ready flag is set again below.
        run.draftReady = run.specReady = false;
        copy = true;
      } else {
        run.specWanted = true;
      }
    }
  }
  portEXIT_CRITICAL(&gSTTMux);
  if (!copy) return;
  memcpy(run.draftPcm, live, samples * sizeof(int16_t));
  portENTER_CRITICAL(&gSTTMux);
  run.specReady = true; run.specReadyId = run.specLiveId; run.specReadySamples = samples;
  run.specReadyStart = run.specStart; run.specReadySequence = run.specSequence;
  run.specCopied = true; run.specWanted = false;
#if HW1_STT_CAPTURE_FEATURES
  // The live phrase's feature cache may serve this snapshot's rows. Only the
  // cache identity is noted here; the worker takes the rows that are
  // committed when the run starts, clipped to the snapshot, i.e. only rows of
  // samples the snapshot itself contains (sttFeatureTrack ran just before
  // this, so feLive is the phrase being speculated on).
  run.specReadyFe = -1;
  if (run.feLive >= 0 && run.fe[run.feLive].state == FeState::Live && run.fe[run.feLive].start == run.specStart) {
    run.specReadyFe = run.feLive; run.specReadyFeGen = run.fe[run.feLive].gen;
  }
#endif
  portEXIT_CRITICAL(&gSTTMux);
}
#endif
static void sttCaptureWorker(void* context) {
  auto& run = *static_cast<StreamRun*>(context);
  const AudioSource previous = audioGetSource();
  AudioSource source = AUDIO_SRC_NONE;
  bool selected = false, claimed = false, g2Held = false;
  int16_t block[kStreamReadSamples];
  do {
    if (sttStreamCancelled(&run)) break;
    if (audioCaptureBusy() || micRecordingBusy() || gMicRunning) {
      sttStreamFail(run, "Audio became busy; close SR/mic and retry"); break;
    }
    if (run.nativeGeneration) {
      // Conversate owns the glasses mic lifecycle (START/pause/close); claim
      // its native stream for this exact connection. The HAL selects the left
      // G2 source itself and restores the previous source on release.
      if (!audioCaptureStartG2Native("stt", run.nativeGeneration)) {
        sttStreamFail(run, "Could not claim the Conversate microphone"); break;
      }
    } else {
      if (run.requested != AUDIO_SRC_NONE && !audioSourceAvailable(run.requested)) {
        sttStreamFail(run, "Selected microphone is unavailable"); break;
      }
      selected = audioSetSource(run.requested);
      if (!selected || !audioCaptureStart("stt", STT_SAMPLE_RATE)) {
        sttStreamFail(run, "Could not claim microphone; close SR/mic first"); break;
      }
    }
    claimed = true;
    source = audioGetSource();
    if (run.requested != AUDIO_SRC_NONE && source != run.requested) {
      sttStreamFail(run, "Selected microphone changed during startup"); break;
    }
    if (source == AUDIO_SRC_G2_LEFT && !run.nativeGeneration) g2Held = sttG2CaptureBegin();
    audioTrimBufferedPcm("stt", 0);
    uint32_t initialOverruns = 0;
    if (!audioCaptureOverruns("stt", &initialOverruns)) {
      sttStreamFail(run, "Continuous capture integrity monitoring unavailable"); break;
    }
    if (initialOverruns) {
      portENTER_CRITICAL(&gSTTMux);
      gSTT.snapshot.audioOverruns = initialOverruns;
      portEXIT_CRITICAL(&gSTTMux);
      sttStreamFail(run, "Microphone audio loss during capture startup"); break;
    }
    portENTER_CRITICAL(&gSTTMux);
    // Measure the ambient floor before prompting for speech. Capture remains
    // owned during this short calibration; the UI still shows Preparing.
    gSTT.snapshot.state = STTState::Preparing;
    gSTT.snapshot.captureActive = true;
    gSTT.snapshot.audioSource = static_cast<uint8_t>(source);
    gSTT.captureStartedMs = millis();
    gSTT.captureSource = static_cast<uint8_t>(source);
    portEXIT_CRITICAL(&gSTTMux);
    uint32_t lastAudioMs = millis();
    bool wasPaused = false;
    size_t pauseSilence = 0;  // zero samples still to feed after a pause edge
    while (!sttStreamCancelled(&run)) {
      bool finish, paused;
      portENTER_CRITICAL(&gSTTMux);
      finish = gSTT.finishRequested;
      paused = gSTT.paused;
      portEXIT_CRITICAL(&gSTTMux);
      if (finish) break;
#if HW1_STT_LONG_PAUSE_FEED
      // A native pause stops PCM delivery. Feed 1.25 s of silence so the
      // phrase in progress can end on its normal pause rule, then wait without
      // treating the missing audio as a failed microphone. A short phrase
      // closes only after 1 s of silence from its last voiced 10 ms frame
      // (HW1_STT_SHORT_CLOSE), and a wearer who pauses mid-word leaves that
      // frame partly voiced: a 1 s feed stopped one frame short and the phrase
      // stayed open (no caption) until speech resumed.
      if (paused && !wasPaused) pauseSilence = STT_SAMPLE_RATE * 5 / 4;
#else
      // A native pause stops PCM delivery. Feed one second of silence so the
      // phrase in progress can end on its normal pause rule, then wait without
      // treating the missing audio as a failed microphone.
      if (paused && !wasPaused) pauseSilence = STT_SAMPLE_RATE;
#endif
      wasPaused = paused;
      if (paused) lastAudioMs = millis();
      if (!audioCaptureOwnedBy("stt") || !audioCaptureActive() || !audioSourceAvailable(source)) {
        sttStreamFail(run, "Microphone source lost"); break;
      }
      size_t count = audioReadPcm(block, kStreamReadSamples, 80);
      if (count > kStreamReadSamples) { sttStreamFail(run, "Invalid microphone sample count"); break; }
      if (!count && paused && pauseSilence) {
        count = std::min<size_t>(kStreamReadSamples, pauseSilence);
        memset(block, 0, count * sizeof(block[0]));
        pauseSilence -= count;
      }
      uint32_t overruns = 0;
      if (!audioCaptureOverruns("stt", &overruns)) {
        sttStreamFail(run, "Microphone integrity monitor lost"); break;
      }
      if (overruns != initialOverruns) {
        portENTER_CRITICAL(&gSTTMux);
        gSTT.snapshot.audioOverruns = overruns;  // startup required a zero baseline
        if (source == AUDIO_SRC_G2_LEFT) ++run.lossEvents;
        portEXIT_CRITICAL(&gSTTMux);
        // Glasses mic: lost LC3 frames are already concealed (PLC) and a short
        // gap only nudges a caption; count it and continue. The onboard mic
        // keeps the strict rule (a gap there means the device is overloaded).
        if (source != AUDIO_SRC_G2_LEFT) {
          sttStreamFail(run, "Microphone audio loss detected; session stopped"); break;
        }
        initialOverruns = overruns;
      }
      if (count) {
        lastAudioMs = millis();
        int64_t sum = 0;
        uint64_t square = 0;
        for (size_t i = 0; i < count; ++i) {
          sum += block[i]; square += static_cast<int64_t>(block[i]) * block[i];
        }
        const double variance = static_cast<double>(square) / count -
                                static_cast<double>(sum) * sum / count / count;
        const int level = std::min(100, static_cast<int>(std::sqrt(std::max(0.0, variance)) * 100.0 / 32768));
#if HW1_STT_CAPTURE_FEATURES
        portENTER_CRITICAL(&gSTTMux);
        int16_t* const feRing = run.feRing;  // published by the worker after warm-up
        portEXIT_CRITICAL(&gSTTMux);
        if (feRing) sttFeatureRingWrite(run, feRing, block, count);
#endif
        size_t offset = 0;
        while (offset < count && !sttStreamCancelled(&run)) {
          const auto fed = run.segmenter.push(block + offset, count - offset);
          offset += fed.consumed;
          if (fed.status == hw1::stt::SegmentStatus::Ready) {
            if (!sttQueueSegment(run)) break;
          } else if (fed.status != hw1::stt::SegmentStatus::Ok || !fed.consumed) {
            sttStreamFail(run, "Invalid continuous audio segment"); break;
          }
        }
#if HW1_STT_CAPTURE_FEATURES
        if (feRing) sttFeatureTrack(run);
#endif
#if HW1_STT_SPECULATIVE_FINAL
        sttSpecCapture(run);  // before drafts, which it outranks
#endif
        // Live mode: offer the utterance in progress for a draft once it has
        // >=1 s of audio and >=0.6 s more than the last snapshot. The worker
        // paces itself: a new snapshot waits until the previous draft is done.
        const int16_t* live = nullptr;
        size_t liveSamples = 0;
        if (run.draftPcm && run.segmenter.in_progress(&live, &liveSamples)) {
          portENTER_CRITICAL(&gSTTMux);
          const uint32_t nextSequence = gSTT.snapshot.segmentsCaptured + 1;
#if HW1_STT_SPECULATIVE_FINAL
          const bool idle = !run.draftReady && !run.draftBusy &&
                            !run.specLiveId && !run.specReady && !run.specBusy;
#else
          const bool idle = !run.draftReady && !run.draftBusy;
#endif
          if (run.draftSequence != nextSequence) run.draftTakenSamples = 0;
          const size_t paceSamples = std::max<size_t>(STT_SAMPLE_RATE * 6 / 10,
                                                      size_t(run.lastDraftMs) * (STT_SAMPLE_RATE / 1000));
          const bool grown = liveSamples >= STT_SAMPLE_RATE &&
                             liveSamples >= run.draftTakenSamples + paceSamples;
          portEXIT_CRITICAL(&gSTTMux);
          if (idle && grown) {
            memcpy(run.draftPcm, live, liveSamples * sizeof(int16_t));
            portENTER_CRITICAL(&gSTTMux);
            run.draftSamples = liveSamples;
            run.draftSequence = nextSequence;
            run.draftTakenSamples = liveSamples;
            run.draftReady = true;
#if HW1_STT_CAPTURE_FEATURES
            // The live cache's finished rows cover this snapshot's start.
            run.draftFe = -1; run.draftFeRows = 0;
            if (run.feLive >= 0 && run.fe[run.feLive].state == FeState::Live && liveSamples >= 320) {
              run.draftFe = run.feLive; run.draftFeGen = run.fe[run.feLive].gen;
              run.draftFeRows = std::min(run.fe[run.feLive].frames, liveSamples / 160);
            }
#endif
            portEXIT_CRITICAL(&gSTTMux);
          }
        }
        portENTER_CRITICAL(&gSTTMux);
        gSTT.snapshot.recordedSamples = run.segmenter.samples_seen();
        if (run.segmenter.calibrated()) gSTT.snapshot.state = STTState::Recording;
        gSTT.snapshot.rms = run.segmenter.current_rms();
        gSTT.snapshot.noiseRms = run.segmenter.noise_rms();
        gSTT.snapshot.thresholdRms = run.segmenter.threshold_rms();
#ifdef ESP_PLATFORM
        {
          // Timeline: once a second, the endpointing inputs (is a phrase open?).
          static uint32_t lastLevelLogMs = 0;
          const uint32_t nowMs = millis();
          if (nowMs - lastLevelLogMs >= 1000) {
            // Never log from this real-time task (console writes can block and
            // drop audio); queue the sample for the worker to print.
            lastLevelLogMs = nowMs;
            const int16_t* openPcm = nullptr; size_t openSamples = 0;
            TLLevel& e = gTLLevels[gTLLevelHead++ % kTLLevels];
            e.t = nowMs; e.rms = gSTT.snapshot.rms; e.noise = gSTT.snapshot.noiseRms;
            e.thr = gSTT.snapshot.thresholdRms;
            e.open = run.segmenter.in_progress(&openPcm, &openSamples);
          }
        }
#endif
        gSTT.snapshot.peakRms = run.segmenter.peak_rms();
        gSTT.snapshot.level = level;
        gSTT.snapshot.captureStackFreeBytes = uxTaskGetStackHighWaterMark(nullptr);
        portEXIT_CRITICAL(&gSTTMux);
      } else vTaskDelay(pdMS_TO_TICKS(1));
      sttStreamTick(run);
      if (millis() - lastAudioMs >= sttSourceTimeoutMs(source)) {
        if (!run.nativeGeneration) { sttStreamFail(run, "Microphone PCM delivery timed out"); break; }
        // Conversate decides when the session ends (CLOSE, or the temple's
        // disconnect, which "source lost" above catches); keep waiting.
        portENTER_CRITICAL(&gSTTMux);
        ++run.stalls;
        portEXIT_CRITICAL(&gSTTMux);
        lastAudioMs = millis();
      }
    }
    // A normal stop seals and drains the final voiced tail. Cancel/error never
    // turns abandoned partial audio into a new inference request.
    if (!sttStreamCancelled(&run)) {
      uint32_t finalOverruns = 0;
      if (!audioCaptureOverruns("stt", &finalOverruns)) {
        sttStreamFail(run, "Microphone integrity monitor lost at stop");
      } else if (finalOverruns) {
        // Loss first seen here happened after the last in-session check, i.e.
        // around the stop gesture itself: tapping the G2 touchpad to stop can
        // drop a concealed (PLC) mic frame. Record it, but still seal and
        // transcribe the tail instead of failing the whole session and
        // discarding it. Loss during recording is still a hard failure above.
        portENTER_CRITICAL(&gSTTMux);
        gSTT.snapshot.audioOverruns = finalOverruns;
        portEXIT_CRITICAL(&gSTTMux);
#ifdef ESP_PLATFORM
        ESP_LOGW("STT", "audio integrity count %lu at stop; tail kept", (unsigned long)finalOverruns);
#endif
      }
    }
    if (!sttStreamCancelled(&run)) {
      const auto status = run.segmenter.finish();
      if (status == hw1::stt::SegmentStatus::Ready) (void)sttQueueSegment(run);
      else if (status != hw1::stt::SegmentStatus::Finished)
        sttStreamFail(run, "Could not finish continuous audio segment");
    }
  } while (false);
  portENTER_CRITICAL(&gSTTMux);
  gSTT.snapshot.captureActive = false;
  if (!gSTT.captureEndedMs) gSTT.captureEndedMs = millis();
  gSTT.snapshot.state = STTState::Stopping;
  gSTT.snapshot.level = 0;
  portEXIT_CRITICAL(&gSTTMux);
  if (claimed || audioCaptureOwnedBy("stt")) {
    audioCaptureStop("stt");
    while (audioCaptureOwnedBy("stt")) vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (g2Held) sttG2CaptureEnd();
  if (selected && !audioCaptureBusy())
    audioSetSource(audioSourceAvailable(previous) ? previous : AUDIO_SRC_NONE);
  sttWipe(block, sizeof(block));
  // Publish join last: the parent may now free run.working and its own stack.
  portENTER_CRITICAL(&gSTTMux);
  gSTT.snapshot.captureStackFreeBytes = uxTaskGetStackHighWaterMark(nullptr);
  run.captureDone = true;
  portEXIT_CRITICAL(&gSTTMux);
  vTaskDelete(nullptr);
}
#if HW1_STT_CAPTURE_FEATURES
// Feature-cache helpers for the worker. Only the worker frees caches (so a
// final or draft it is running can never lose its rows); orphans are retired
// here.
static void sttFeatureRetireOrphans(StreamRun& run) {
  for (int i = 0; i < kFeCaches; ++i) {
    size_t used = 0;
    portENTER_CRITICAL(&gSTTMux);
    const bool orphan = run.fe[i].state == FeState::Orphan;
    if (orphan) used = run.fe[i].frames;
    portEXIT_CRITICAL(&gSTTMux);
    if (!orphan) continue;
    sttWipe(run.fe[i].rows, std::min(used + kFeBatchRows, kFeMaxRows) * kFeRowFloats * sizeof(float));
    portENTER_CRITICAL(&gSTTMux);
    run.fe[i].state = FeState::Free; ++run.fe[i].gen;
    portEXIT_CRITICAL(&gSTTMux);
  }
}
static void sttFeatureRelease(StreamRun& run, int idx, uint32_t gen) {
  if (idx < 0) return;
  size_t used = 0;
  portENTER_CRITICAL(&gSTTMux);
  const bool mine = run.fe[idx].gen == gen && run.fe[idx].state == FeState::Attached;
  if (mine) used = run.fe[idx].frames;
  portEXIT_CRITICAL(&gSTTMux);
  if (!mine) return;
  sttWipe(run.fe[idx].rows, std::min(used + kFeBatchRows, kFeMaxRows) * kFeRowFloats * sizeof(float));
  portENTER_CRITICAL(&gSTTMux);
  run.fe[idx].state = FeState::Free; ++run.fe[idx].gen;
  portEXIT_CRITICAL(&gSTTMux);
}
// After the warm-up (weights and LM loaded): allocate the pool and start the
// feature task, or leave capture-time features off for this session. The pool
// must never cost a phrase its inference or its LM: what stays free beside it
// must admit the largest call the broker makes (a 10 s phrase or draft) by the
// runtime's own rule (sttLocalCallBudget: with a resident model only that
// call's frontend; with per-call models its arena, frontend and metadata
// margin, ~3.7 MB, ~3.1 MB of it contiguous), plus 1 MiB for LM beams, and
// never less than 2 MiB. Freed again at session end (sttFeatureStop).
static void sttFeatureStart(StreamRun& run, const STTLocalSession& session) {
  constexpr size_t kRowsBytes = kFeMaxRows * kFeRowFloats * sizeof(float);
  constexpr size_t kPoolBytes = kFeCaches * kRowsBytes + kFeRingSamples * sizeof(int16_t) +
                                sizeof(hw1::stt::FrontendWorkspace);
  const char* why = nullptr;
  const STTLocalCallBudget call = sttLocalCallBudget(kStreamSamples, &session);
  const size_t keepFree = std::max<size_t>(2u << 20, call.total + (1u << 20));
#ifdef ESP_PLATFORM
  if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < kPoolBytes + keepFree) why = "psram";
#else
  (void)kPoolBytes; (void)keepFree;
#endif
  int16_t* ring = nullptr;
  hw1::stt::FrontendWorkspace* work = nullptr;
  float* rows[kFeCaches] = {};
  if (!why) {
    ring = static_cast<int16_t*>(ps_alloc(kFeRingSamples * sizeof(int16_t), AllocPref::RequirePSRAM, "stt.fe_ring"));
    work = static_cast<hw1::stt::FrontendWorkspace*>(ps_alloc(sizeof(hw1::stt::FrontendWorkspace), AllocPref::RequirePSRAM, "stt.fe_work"));
    for (auto& r : rows) r = static_cast<float*>(ps_alloc(kRowsBytes, AllocPref::RequirePSRAM, "stt.fe_rows"));
    if (!ring || !work || !rows[0] || !rows[1]) why = "alloc";
  }
#ifdef ESP_PLATFORM
  // The pool may have split the block a per-call arena needs: then give it back.
  if (!why && (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < keepFree ||
               heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < call.contiguous))
    why = "contiguous";
#endif
  if (!why) {
    portENTER_CRITICAL(&gSTTMux);
    for (int i = 0; i < kFeCaches; ++i) run.fe[i].rows = rows[i];
    run.feWork = work;
    run.feRunning = true;
    portEXIT_CRITICAL(&gSTTMux);
    TaskHandle_t task = nullptr;
    taskStackRecord("stt_fe", kFeStackBytes);
    if (xTaskCreatePinnedToCore(sttFeatureWorker, "stt_fe", kFeStackBytes, &run, 1, &task, 0) != pdPASS) {
      portENTER_CRITICAL(&gSTTMux);
      run.feRunning = false;
      for (auto& c : run.fe) c.rows = nullptr;
      run.feWork = nullptr;
      portEXIT_CRITICAL(&gSTTMux);
      why = "task";
    } else {
      portENTER_CRITICAL(&gSTTMux);
      run.feTask = task;
      run.feRing = ring;  // last: the capture task starts feeding it now
      portEXIT_CRITICAL(&gSTTMux);
    }
  }
  if (why) {
    if (ring) { sttWipe(ring, kFeRingSamples * sizeof(int16_t)); free(ring); }
    if (work) { sttWipe(work, sizeof(*work)); free(work); }
    for (auto& r : rows) if (r) { sttWipe(r, kRowsBytes); free(r); }
  }
#ifdef ESP_PLATFORM
  ESP_LOGI("STT", "TL fe_pool ok=%d caches=%d bytes=%u keep=%u need_block=%u psram_free=%u psram_largest=%u%s%s",
           why ? 0 : 1, kFeCaches, unsigned(kPoolBytes), unsigned(keepFree), unsigned(call.contiguous),
           unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
           unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)), why ? " off=" : "", why ? why : "");
#endif
}
// Session end, after the capture task joined: stop and join the feature task,
// then wipe and free the pool.
static void sttFeatureStop(StreamRun& run) {
  // No notification here: the task may already be deleting itself. It wakes
  // at least every 20 ms, sees feStop, clears feRunning as its last access
  // to `run`, and deletes itself.
  portENTER_CRITICAL(&gSTTMux);
  run.feStop = true;
  portEXIT_CRITICAL(&gSTTMux);
  for (;;) {
    portENTER_CRITICAL(&gSTTMux);
    const bool running = run.feRunning;
    portEXIT_CRITICAL(&gSTTMux);
    if (!running) break;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  run.feTask = nullptr;
  if (run.feRing) { sttWipe(run.feRing, kFeRingSamples * sizeof(int16_t)); free(run.feRing); run.feRing = nullptr; }
  if (run.feWork) { sttWipe(run.feWork, sizeof(*run.feWork)); free(run.feWork); run.feWork = nullptr; }
  for (auto& c : run.fe)
    if (c.rows) { sttWipe(c.rows, kFeMaxRows * kFeRowFloats * sizeof(float)); free(c.rows); c.rows = nullptr; }
}
#endif
#if HW1_STT_SPEAKER_LABELS
// Speaker labels: saved transcript lines wait (up to kLabelWaitMs) for their phrase's speaker label from the background
// task, then go out strictly in sequence as "[Person N] text" (plain text when the phrase was skipped, the label
// is late, or the session ends). Only this worker writes the transcript; the labelling task only posts labels.
namespace {
constexpr size_t kLabelledLines = 8;
constexpr uint32_t kLabelWaitMs = 10000;
struct LabelledLine {
  uint32_t seq = 0, queuedMs = 0;
  int8_t speaker = -1;
  bool labelled = false;
  uint16_t len = 0;
  char text[STT_MAX_TEXT + 1] = {};
};
struct LabelledTranscript {
  LabelledLine* lines = nullptr;   // PSRAM ring
  size_t head = 0, count = 0;
  bool tried = false;
  ~LabelledTranscript() {
    if (lines) { sttWipe(lines, kLabelledLines * sizeof(LabelledLine)); heap_caps_free(lines); }
  }
};
void sttLabelledPump(LabelledTranscript& lt, STTLocalSession& session, TranscriptSession& transcript,
                     STTToken token, bool flushAll) {
  uint32_t seq = 0;
  int8_t speaker = -1;
  while (sttSpeakerTakeLabel(&session, &seq, &speaker)) {
    for (size_t i = 0; i < lt.count; ++i) {
      LabelledLine& l = lt.lines[(lt.head + i) % kLabelledLines];
      if (l.seq == seq) { l.speaker = speaker; l.labelled = true; break; }
    }
  }
  const uint32_t now = millis();
  bool wrote = false;
  while (lt.count) {
    LabelledLine& l = lt.lines[lt.head];
    if (!(l.labelled || flushAll || now - l.queuedMs > kLabelWaitMs)) break;
    char line[STT_MAX_TEXT + 1];
    size_t n = 0;
    if (l.labelled && l.speaker >= 0) n = size_t(snprintf(line, sizeof(line), "[Person %d] ", int(l.speaker) + 1));
    const size_t take = std::min<size_t>(l.len, STT_MAX_TEXT - n);
    memcpy(line + n, l.text, take);
    n += take;
    line[n] = '\0';
    (void)transcript.append(l.seq, line, n);
    ESP_LOGI("STT", "TL tsave seq=%lu spk=%d labelled=%d wait_ms=%lu", (unsigned long)l.seq, int(l.speaker),
             l.labelled ? 1 : 0, (unsigned long)(now - l.queuedMs));
    sttWipe(line, sizeof(line));
    sttWipe(l.text, sizeof(l.text));
    l = LabelledLine{};
    lt.head = (lt.head + 1) % kLabelledLines;
    --lt.count;
    wrote = true;
  }
  if (wrote) sttPublishTranscript(token, transcript.snapshot());
}
// False: no ring (allocation failed) - the caller appends directly, keeping the sequence order.
bool sttLabelledQueue(LabelledTranscript& lt, STTLocalSession& session, TranscriptSession& transcript, STTToken token,
                      uint32_t seq, const char* text) {
  if (!lt.lines && !lt.tried) {
    lt.tried = true;
    lt.lines = static_cast<LabelledLine*>(heap_caps_calloc(kLabelledLines, sizeof(LabelledLine), MALLOC_CAP_SPIRAM));
  }
  if (!lt.lines) return false;
  if (lt.count == kLabelledLines) {   // full: the oldest line goes out unlabelled first
    LabelledLine& oldest = lt.lines[lt.head];
    oldest.queuedMs = millis() - kLabelWaitMs - 1;
    sttLabelledPump(lt, session, transcript, token, false);
  }
  LabelledLine& l = lt.lines[(lt.head + lt.count) % kLabelledLines];
  l = LabelledLine{};
  l.seq = seq;
  l.queuedMs = millis();
  l.len = uint16_t(strnlen(text, STT_MAX_TEXT));
  memcpy(l.text, text, l.len);
  ++lt.count;
  return true;
}
}  // namespace
#endif
static void sttContinuousWorker(void*) {
  StreamRun run;
  STTLocalSession backendSession;
  TranscriptOptions transcriptOptions;
  portENTER_CRITICAL(&gSTTMux);
  run.token = gSTT.snapshot.token;
  run.requested = gSTT.requestedSource;
  run.nativeGeneration = gSTT.nativeG2Generation;
  run.publishWake = gSTT.publishWake;
  run.clockMs = millis();
  transcriptOptions = gSTT.transcriptOptions;
  portEXIT_CRITICAL(&gSTTMux);
  TranscriptSession transcript;
  transcript.begin(transcriptOptions, "local", run.token);
  sttPublishTranscript(run.token, transcript.snapshot());
  bool captureStarted = false;
  do {
    if (sttCancelled(&run.token)) break;
    run.working = static_cast<int16_t*>(ps_alloc(kStreamSamples * sizeof(int16_t), AllocPref::RequirePSRAM, "stt.segment"));
    if (!run.working) { sttStreamFail(run, "Insufficient PSRAM for continuous audio"); break; }
    for (auto& slot : run.slots) {
      slot.pcm = static_cast<int16_t*>(ps_alloc(kStreamSamples * sizeof(int16_t), AllocPref::RequirePSRAM, "stt.queue"));
      if (!slot.pcm) { sttStreamFail(run, "Insufficient PSRAM for continuous queue"); break; }
    }
    // Draft buffer is optional: without PSRAM for it, live mode just stays off.
    run.draftPcm = static_cast<int16_t*>(ps_alloc(kStreamSamples * sizeof(int16_t), AllocPref::RequirePSRAM, "stt.draft"));
    if (sttStreamCancelled(&run)) break;
    hw1::stt::SegmenterConfig segmentConfig;
    segmentConfig.adaptive = true;
    // Close a phrase after >=4 s (was the 8 s default) plus the usual 600 ms
    // pause, so text is finalised while the speaker is still talking and a
    // Stop leaves only a short tail. The 15x5 transcribes at ~0.45x real time,
    // so 4 s phrases keep up; less context at each cut is the trade-off.
    segmentConfig.min_segment_samples = 4 * STT_SAMPLE_RATE;
    segmentConfig.max_segment_samples = kStreamSamples;  // the 10 s phrase cap
#if HW1_STT_SHORT_CLOSE
    // Below 4 s a phrase closes after 1 s of silence instead of waiting for
    // the 4 s floor (a short reply gets its caption); at 4 s or more the
    // 600 ms end wait is unchanged.
    segmentConfig.short_close_silence_samples = STT_SAMPLE_RATE;
#endif
    // The G2 glasses mic's quiet level jitters (~21..60+ RMS per 10 ms frame,
    // measured 2026-09-30), so the window minimum underestimated the room and
    // stray blips kept restarting the 600 ms pause: phrases ran to the 20 s
    // limit and noise opened new ones. Track the lower quartile instead, and
    // let up to 30 ms of louder frames pass without restarting the pause.
    segmentConfig.noise_percentile = 25;
    segmentConfig.silence_blip_samples = 3 * hw1::stt::kSegmentAnalysisSamples;
    // A phrase that reaches the length limit is cut at the quietest point of
    // its last 2 s (usually between words), not mid-word at the exact limit.
    segmentConfig.hard_cut_search_samples = 2 * STT_SAMPLE_RATE;
    if (!run.segmenter.reset(run.working, kStreamSamples, segmentConfig)) {
      sttStreamFail(run, "Invalid continuous segmentation settings"); break;
    }
    taskStackRecord("stt_capture", kCaptureStackBytes);
    if (xTaskCreatePinnedToCore(sttCaptureWorker, "stt_capture", kCaptureStackBytes,
                                &run, 3, nullptr, 1) != pdPASS) {
      sttStreamFail(run, "Could not create continuous capture worker"); break;
    }
    captureStarted = true;
    // A model kept warm (sttKeepWarm) is adopted, not loaded again; a warm
    // load still in progress is awaited while capture runs.
    sttWarmAdopt(backendSession, sttStreamCancelled, &run);
#ifdef ESP_PLATFORM
    {
      // Warm-up: load/verify the 7 MB model and the LM now, while the capture
      // task calibrates and the wearer starts talking. Otherwise the first
      // segment pays ~11 s of loading before any text (sttperf seg 1).
      constexpr size_t kWarmSamples = STT_SAMPLE_RATE / 2;
      auto* silence = static_cast<int16_t*>(heap_caps_calloc(kWarmSamples, sizeof(int16_t), MALLOC_CAP_SPIRAM));
      if (silence) {
        char warmText[STT_MAX_TEXT + 1] = {};
        char warmError[96] = {};
        STTLocalStats warmStats;
        const STTLocalControl warmControl{&run, sttStreamCancelled, sttStreamProgress};
        const uint32_t warmStart = millis();
        const bool warmed = sttLocalTranscribe(silence, kWarmSamples, warmText, sizeof(warmText), warmControl,
                                               warmStats, warmError, sizeof(warmError), &backendSession);
        heap_caps_free(silence);
        ESP_LOGI("STT", "TL capture_start t=%lu source=%u", (unsigned long)gSTT.captureStartedMs,
                 unsigned(gSTT.captureSource));
        // PSRAM after the model and LM are resident: the room a larger LM has.
        ESP_LOGI("STT", "TL warm s=%lu e=%lu load=%lu ok=%d lm=%d cpu=%luMHz psram_free=%u psram_largest=%u",
                 (unsigned long)warmStart, (unsigned long)millis(), (unsigned long)warmStats.loadMs,
                 warmed ? 1 : 0, warmStats.lmUsed ? 1 : 0, (unsigned long)getCpuFrequencyMhz(),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
#if HW1_STT_SPEAKER_LABELS
        {
          // The optional speaker model, after the STT model and LM are resident.
          STTSpeakerStatus sp;
          (void)sttSpeakerBegin(&backendSession, kStreamSamples, &sp);
          ESP_LOGI("STT", "TL spk_load ok=%d why=%s bytes=%u psram_used=%u int_used=%ld ms=%lu th=%.2f fixture_cos=%.4f psram_free=%u",
                   sp.ready ? 1 : 0, sp.why[0] ? sp.why : "none", unsigned(sp.bytes), unsigned(sp.psramUsed),
                   sp.internalUsed, (unsigned long)sp.loadMs, double(sp.threshold), double(sp.fixtureCos),
                   unsigned(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
          // where the load went: file read, SHA-256, ESP-DL build, ESP-DL self-test, fixture embed; memory parts
          ESP_LOGI("STT", "TL spk_load2 read_ms=%lu sha_us=%lu build_us=%lu test=%d test_us=%lu fixture_us=%lu arena_psram=%u arena_int=%u work=%u modules=%u int_min=%u",
                   (unsigned long)sp.readMs, (unsigned long)sp.shaUs, (unsigned long)sp.buildUs, sp.testResult,
                   (unsigned long)sp.testUs, (unsigned long)sp.fixtureUs, unsigned(sp.arenaPsram), unsigned(sp.arenaInternal),
                   unsigned(sp.workspaceBytes), sp.modules,
                   unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        }
#endif
        if (gSTTPerfLog)
          INFO_SYSTEMF("[STTPERF] warm-up %s in %lu ms (load %lu, lm %s)", warmed ? "ok" : warmError,
                       (unsigned long)(millis() - warmStart), (unsigned long)warmStats.loadMs,
                       warmStats.lmUsed ? "ready" : "not used");
        // A warm-up failure is not fatal: the first real segment reports it.
      }
    }
#endif
#if HW1_STT_CAPTURE_FEATURES
    if (!sttStreamCancelled(&run)) sttFeatureStart(run, backendSession);
#endif
#if HW1_STT_SPEAKER_LABELS
    LabelledTranscript labelledLines;
#if HW1_STT_SPEAKER_DIAGNOSTICS
    bool speakerProfileDumped = false;
#endif
#endif
    for (;;) {
#if HW1_STT_SPEAKER_LABELS
      if (labelledLines.count) sttLabelledPump(labelledLines, backendSession, transcript, run.token, false);
#if HW1_STT_SPEAKER_DIAGNOSTICS
      // Per-layer speaker trunk profile (TL spkstg), 240 s after capture start: the
      // conversation playlists end by ~233 s and the harness reads 20 s past the end.
      if (!speakerProfileDumped && gSTT.captureStartedMs && millis() - gSTT.captureStartedMs > 240000) {
        speakerProfileDumped = true;
        sttSpeakerProfileLog();
      }
#endif
#endif
      StreamSlot* slot = nullptr;
      bool captureDone;
      portENTER_CRITICAL(&gSTTMux);
      captureDone = run.captureDone;
      for (auto& candidate : run.slots) {
        if (candidate.state == StreamSlotState::Ready &&
            (!slot || candidate.sequence < slot->sequence)) slot = &candidate;
      }
      if (slot) {
        slot->state = StreamSlotState::Processing;
        --gSTT.snapshot.pendingAudio;
        gSTT.snapshot.inferenceActive = true;
      }
      portEXIT_CRITICAL(&gSTTMux);
      if (sttStreamCancelled(&run)) break;
      if (!slot) {
        if (captureDone) break;
#if HW1_STT_CAPTURE_FEATURES
        // Retire orphaned feature caches on every idle pass (before the
        // speculative path too, which skips the draft code below).
        sttFeatureRetireOrphans(run);
#endif
#if HW1_STT_SPECULATIVE_FINAL
        // A speculative snapshot outranks drafts. A held result whose phrase
        // no longer matches it (speech resumed, other close) is dropped.
        bool speculating = false;
        size_t specSamples = 0;
        uint32_t specId = 0, specSeq = 0, staleDone = 0, staleSnapshot = 0;
        uint64_t specA0 = 0;
        STTFeaturePrefix specPrefix;  // capture-time rows (HW1_STT_CAPTURE_FEATURES)
        portENTER_CRITICAL(&gSTTMux);
        if (run.specDoneId && !sttSpecReferencedLocked(run, run.specDoneId)) {
          staleDone = run.specDoneId; run.specDoneId = 0;
        }
        if (run.specReady && !run.failed && !gSTT.cancelRequested) {
          run.specReady = false;
          if (sttSpecReferencedLocked(run, run.specReadyId)) {
            speculating = run.specBusy = true;
            specId = run.specRunId = run.specReadyId;
            specSamples = run.specReadySamples;
            specSeq = run.specReadySequence;
            specA0 = run.specReadyStart;
#if HW1_STT_CAPTURE_FEATURES
            // Rows of the speculated phrase's cache committed by now, clipped
            // to rows whose samples all lie inside the snapshot
            // ((r + 1) * 160 <= specSamples: the same rows the batch frontend
            // computes on it). Only the worker frees a cache, so they stay put
            // during the run; gen/start guard against a reused cache.
            if (run.specReadyFe >= 0 && specSamples >= 320) {
              const FeCache& c = run.fe[run.specReadyFe];
              if (c.gen == run.specReadyFeGen && c.state != FeState::Free && c.start == specA0) {
                specPrefix.rows = c.rows;
                specPrefix.count = std::min(c.frames, specSamples / 160);
              }
            }
#endif
          } else {
            staleSnapshot = run.specReadyId;
          }
        }
        portEXIT_CRITICAL(&gSTTMux);
        if (staleDone) sttWipe(gSpecText, sizeof(gSpecText));
#ifdef ESP_PLATFORM
        if (staleDone)
          ESP_LOGI("STT", "TL spec_discard id=%lu t=%lu why=2 stale", (unsigned long)staleDone, (unsigned long)millis());
        if (staleSnapshot)
          ESP_LOGI("STT", "TL spec_discard id=%lu t=%lu why=3 unrun", (unsigned long)staleSnapshot, (unsigned long)millis());
#else
        (void)staleSnapshot;
#endif
        if (speculating) {
          char specError[96] = {};
          STTLocalStats specStats;
          const STTLocalControl specControl{&run, sttSpecPreempted, sttStreamProgress};
          const uint32_t specStart = millis();
#ifdef ESP_PLATFORM
          // a0: phrase start on the TL seg clock (capture start + samples).
          ESP_LOGI("STT", "TL spec_start seq=%lu id=%lu a0=%lu samples=%u t=%lu", (unsigned long)specSeq,
                   (unsigned long)specId, (unsigned long)(gSTT.captureStartedMs + specA0 / (STT_SAMPLE_RATE / 1000)),
                   unsigned(specSamples), (unsigned long)specStart);
#else
          (void)specA0; (void)specSeq;
#endif
          const bool ok = sttLocalTranscribe(run.draftPcm, specSamples, gSpecText, sizeof(gSpecText), specControl,
                                             specStats, specError, sizeof(specError), &backendSession,
                                             specPrefix.count ? &specPrefix : nullptr);
          bool kept = false;
          portENTER_CRITICAL(&gSTTMux);
          run.specBusy = false;
          if (ok && memchr(gSpecText, '\0', sizeof(gSpecText)) && !run.failed && !gSTT.cancelRequested &&
              sttSpecReferencedLocked(run, specId)) {
            run.specDoneId = specId; run.specDoneSamples = specSamples;
            run.specDoneStartMs = specStart; run.specDoneStats = specStats;
            kept = true;
          }
          portEXIT_CRITICAL(&gSTTMux);
          if (!kept) sttWipe(gSpecText, sizeof(gSpecText));
#ifdef ESP_PLATFORM
          // cached = feature rows this run took from the capture-time cache.
          ESP_LOGI("STT", "TL spec_end seq=%lu id=%lu samples=%u s=%lu e=%lu ok=%d kept=%d m=%lu f=%lu n=%lu d=%lu cached=%u",
                   (unsigned long)specSeq, (unsigned long)specId, unsigned(specSamples), (unsigned long)specStart,
                   (unsigned long)millis(), ok ? 1 : 0, kept ? 1 : 0, (unsigned long)specStats.loadMs,
                   (unsigned long)specStats.frontendMs, (unsigned long)specStats.inferenceMs,
                   (unsigned long)specStats.decodeMs, unsigned(specStats.featureCachedFrames));
          if (!kept)
            ESP_LOGI("STT", "TL spec_discard seq=%lu id=%lu t=%lu why=1 %s", (unsigned long)specSeq,
                     (unsigned long)specId, (unsigned long)millis(), ok ? "superseded" : "aborted");
#endif
          sttStreamTick(run);
          sttDrainLevelLog(run);
          continue;
        }
#endif
        bool drafting = false;
        size_t draftSamples = 0;
        uint32_t draftSequence = 0;
        STTFeaturePrefix draftPrefix;  // capture-time rows (HW1_STT_CAPTURE_FEATURES)
        portENTER_CRITICAL(&gSTTMux);
        if (run.draftReady && !run.failed && !gSTT.cancelRequested) {
          run.draftReady = false; run.draftBusy = drafting = true; run.draftPastPcm = false;
          draftSamples = std::max(run.draftSamples, size_t(320));
          draftSequence = run.draftSequence;
#if HW1_STT_CAPTURE_FEATURES
          // Only the worker frees a cache, so these rows stay put during the draft.
          if (run.draftFe >= 0 && run.draftFeRows && run.fe[run.draftFe].gen == run.draftFeGen &&
              run.fe[run.draftFe].state != FeState::Free && draftSamples == run.draftSamples) {
            draftPrefix.rows = run.fe[run.draftFe].rows; draftPrefix.count = run.draftFeRows;
          }
          run.draftFe = -1;
#endif
        }
        portEXIT_CRITICAL(&gSTTMux);
        if (drafting) {
          if (draftSamples > run.draftSamples)
            memset(run.draftPcm + run.draftSamples, 0, (draftSamples - run.draftSamples) * sizeof(int16_t));
          char draftText[STT_MAX_TEXT + 1] = {};
          char draftError[96] = {};
          STTLocalStats draftStats;
          const STTLocalControl draftControl{&run, sttDraftPreempted, sttDraftProgress};
          const uint32_t draftStart = millis();
          const bool ok = sttLocalTranscribe(run.draftPcm, draftSamples, draftText, sizeof(draftText), draftControl,
                                             draftStats, draftError, sizeof(draftError), &backendSession,
                                             draftPrefix.count ? &draftPrefix : nullptr);
#if HW1_STT_CAPTURE_FEATURES
          portENTER_CRITICAL(&gSTTMux);
          run.feDraftRows += draftStats.featureCachedFrames;
          portEXIT_CRITICAL(&gSTTMux);
#endif
          bool draftPublished = false;
          portENTER_CRITICAL(&gSTTMux);
          // Publish only while its final chunk has not been produced yet.
          if (ok && memchr(draftText, '\0', sizeof(draftText)) && !gSTT.cancelRequested && !run.failed &&
              gSTT.snapshot.segmentsCompleted < draftSequence) {
            memcpy(gSTT.draft.text, draftText, sizeof(draftText));
            gSTT.draft.sequence = draftSequence;
            ++gSTT.draft.version;
            draftPublished = true;
          }
          run.draftBusy = false; run.draftPastPcm = false;
          if (ok) run.lastDraftMs = millis() - draftStart;
          portEXIT_CRITICAL(&gSTTMux);
#if HW1_STT_PUBLISH_WAKE
          // Wake the consumer now (never inside gSTTMux) rather than at its
          // next poll; the console write below then follows its read.
          if (draftPublished && run.publishWake) run.publishWake();
#else
          (void)draftPublished;
#endif
#ifdef ESP_PLATFORM
          ESP_LOGI("STT", "TL draft seq=%lu audio_ms=%lu s=%lu e=%lu ok=%d",
                   (unsigned long)draftSequence, (unsigned long)(draftSamples / (STT_SAMPLE_RATE / 1000)),
                   (unsigned long)draftStart, (unsigned long)millis(), ok ? 1 : 0);
#endif
          sttWipe(draftText, sizeof(draftText));
          if (gSTTPerfLog)
            INFO_SYSTEMF("[STTPERF] draft seq %lu audio %.1fs in %lu ms (%s)", (unsigned long)draftSequence,
                         draftSamples / (double)STT_SAMPLE_RATE,
                         (unsigned long)(draftStats.loadMs + draftStats.frontendMs + draftStats.inferenceMs + draftStats.decodeMs),
                         ok ? "ok" : draftError);
          sttStreamTick(run);
          sttDrainLevelLog(run);
          continue;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        sttStreamTick(run);
        sttDrainLevelLog(run);
        continue;
      }
      char text[STT_MAX_TEXT + 1] = {};
      char error[96] = {};
      STTLocalStats stats;
      const STTLocalControl control{&run, sttStreamCancelled, sttStreamProgress};
      STTPerfRecord perf;
      perf.sequence = slot->sequence;
      perf.audioMs = static_cast<uint32_t>(slot->samples / (STT_SAMPLE_RATE / 1000));
      perf.queuedMs = slot->queuedMs;
      perf.startMs = millis();
      portENTER_CRITICAL(&gSTTMux);
      perf.backlog = static_cast<uint8_t>(gSTT.snapshot.pendingAudio);
      portEXIT_CRITICAL(&gSTTMux);
#ifdef ESP_PLATFORM
      sttTaskSample(gPerfBefore);
#endif
      // A stop immediately after a forced boundary can leave fewer than20 ms
      // of real audio. Pad only the backend copy; published positions retain
      // the exact captured extent, so no samples disappear or move in time.
#if HW1_STT_TRIM_SILENCE
      // Trim the inference copy of a pause/session-end phrase to its voiced
      // end + 200 ms (no qualifying voiced run: keep it whole).
      size_t trimmedSamples = slot->samples;
      if (slot->endKind != hw1::stt::SegmentEnd::HardLimit && slot->voicedEnd &&
          slot->voicedEnd + kTrimTailSamples < slot->samples)
        trimmedSamples = slot->voicedEnd + kTrimTailSamples;
      const size_t inferenceSamples = std::max(trimmedSamples, size_t(320));
#else
      const size_t inferenceSamples = std::max(slot->samples, size_t(320));
#endif
      if (inferenceSamples > slot->samples)
        memset(slot->pcm + slot->samples, 0, (inferenceSamples - slot->samples) * sizeof(int16_t));
#if HW1_STT_CAPTURE_FEATURES
      // Orphaned feature caches are retired before every final.
      sttFeatureRetireOrphans(run);
#endif
#if HW1_STT_SPECULATIVE_FINAL
      // Promote a finished speculative run on exactly this input.
      bool promoted = false;
      uint32_t promotedId = 0;
      portENTER_CRITICAL(&gSTTMux);
      if (slot->specId && slot->specId == run.specDoneId) {
        promotedId = run.specDoneId;
        promoted = run.specDoneSamples == inferenceSamples;
        if (promoted) { stats = run.specDoneStats; perf.startMs = run.specDoneStartMs; }
        run.specDoneId = 0;
      }
      portEXIT_CRITICAL(&gSTTMux);
      bool ok = promoted;
      if (promoted) memcpy(text, gSpecText, sizeof(text));
#else
      constexpr bool promoted = false;
      bool ok = false;
#endif
      if (!promoted) {
        STTFeaturePrefix prefix;
#if HW1_STT_CAPTURE_FEATURES
        // Rows computed during capture for this phrase. The trim may infer
        // only a prefix (voiced end + 200 ms); then only the cached rows whose
        // samples all lie inside that prefix are used, row r with
        // (r + 1) * 160 <= inferenceSamples. Such a row never reflects at the
        // end, so it equals the batch frontend's row of the trimmed input; the
        // runtime computes the rest on the trimmed PCM. A zero-padded input
        // (< 320 samples captured) has no cache (none is attached).
        portENTER_CRITICAL(&gSTTMux);
        if (slot->fe >= 0 && slot->feRows && run.fe[slot->fe].gen == slot->feGen &&
            run.fe[slot->fe].state == FeState::Attached && inferenceSamples <= slot->samples) {
          prefix.rows = run.fe[slot->fe].rows;
          prefix.count = std::min(slot->feRows, inferenceSamples / 160);
        }
        portEXIT_CRITICAL(&gSTTMux);
#endif
        ok = sttLocalTranscribe(slot->pcm, inferenceSamples, text, sizeof(text), control,
                                stats, error, sizeof(error), &backendSession,
                                prefix.count ? &prefix : nullptr);
      }
#if HW1_STT_SPECULATIVE_FINAL
      if (promotedId) sttWipe(gSpecText, sizeof(gSpecText));
#endif
#if HW1_STT_CAPTURE_FEATURES
      // Feature counters; a promoted final counts with its speculative run's rows.
      portENTER_CRITICAL(&gSTTMux);
      ++run.feFinals;
      if (stats.featureCachedFrames) { ++run.feCachedFinals; run.feCachedRows += stats.featureCachedFrames; }
      portEXIT_CRITICAL(&gSTTMux);
#endif
      bool acceptedText = false;
      uint32_t tlPubMs = 0;  // millis() (esp_timer) at the mailbox publish
      if (!sttStreamCancelled(&run)) {
        if (!ok || !memchr(text, '\0', sizeof(text))) {
          sttStreamFail(run, error[0] ? error : "Continuous transcription failed");
        } else {
          portENTER_CRITICAL(&gSTTMux);
          if (!gSTT.cancelRequested && !run.failed && gSTT.snapshot.pendingTexts < kTextQueueDepth) {
            auto& chunk = gSTTChunks[(gSTT.textHead + gSTT.snapshot.pendingTexts) % kTextQueueDepth];
            chunk.sequence = slot->sequence;
            chunk.startSample = slot->start;
            chunk.endSample = slot->end;
            chunk.forcedBoundary = slot->forced;
            memcpy(chunk.text, text, sizeof(chunk.text));
            ++gSTT.snapshot.pendingTexts;
            ++gSTT.snapshot.segmentsCompleted;
            gSTT.snapshot.textReady = true;
            acceptedText = true;
            tlPubMs = millis();
            if (gSTT.draft.sequence && gSTT.draft.sequence <= slot->sequence) {
              // The final text supersedes the draft for this utterance.
              volatile char* d = gSTT.draft.text;
              for (size_t i = 0; i < sizeof(gSTT.draft.text); ++i) d[i] = 0;
              gSTT.draft.sequence = 0;
              ++gSTT.draft.version;
            }
          } else if (!gSTT.cancelRequested && !run.failed) {
            run.failed = true;
            snprintf(gSTT.snapshot.error, sizeof(gSTT.snapshot.error), "%s", "STT text queue full; receiver must acknowledge results");
          }
          portEXIT_CRITICAL(&gSTTMux);
          if (acceptedText) {
#if HW1_STT_PUBLISH_WAKE
            // Wake the consumer first (Conversate's caption pump) so the
            // console write below is not part of its publish-to-read time.
            // Outside gSTTMux: the woken task may preempt this one.
            if (run.publishWake) run.publishWake();
#endif
#ifdef ESP_PLATFORM
            // t is the publish instant (the mailbox write above).
            ESP_LOGI("STT", "TL pub seq=%lu t=%lu", (unsigned long)slot->sequence, (unsigned long)tlPubMs);
#else
            (void)tlPubMs;
#endif
          }
        }
      }
      // Save the whole accepted chunk once, before UI splitting/ACK can erase
      // its mailbox copy. Only this inference worker performs transcript I/O.
      uint32_t saveStartMs = 0, saveEndMs = 0;
      if (acceptedText) {
        saveStartMs = millis();
#if HW1_STT_SPEAKER_LABELS
        // With background labelling the line waits for its "[Person N]" (sttLabelledPump).
        if (!(sttSpeakerBackground(&backendSession) &&
              sttLabelledQueue(labelledLines, backendSession, transcript, run.token, slot->sequence, text)))
#endif
        {
          (void)transcript.append(slot->sequence, text, strlen(text));
          sttPublishTranscript(run.token, transcript.snapshot());
        }
        saveEndMs = millis();
      }
#if HW1_STT_SPEAKER_LABELS
      if (acceptedText) {
        // Label the published phrase (after publish and save, so captions
        // are never delayed by it); the voiced-trimmed audio, as inferred.
        // Hand the phrase to the background labelling task; inline only if it has none.
        STTSpeakerLabel spk;
        if (sttSpeakerSubmit(&backendSession, slot->sequence, slot->pcm, trimmedSamples)) {
        } else if (sttSpeakerLabel(&backendSession, slot->pcm, trimmedSamples, &spk)) {
          ESP_LOGI("STT", "TL spk seq=%lu spk=%d sim=%.3f new=%d tent=%d samples=%u fe_us=%lu nn_us=%lu head_us=%lu us=%lu",
                   (unsigned long)slot->sequence, int(spk.speaker), double(spk.similarity), spk.created ? 1 : 0,
                   spk.tentative ? 1 : 0, unsigned(trimmedSamples), (unsigned long)spk.featureUs,
                   (unsigned long)spk.trunkUs, (unsigned long)spk.headUs, (unsigned long)spk.totalUs);
          // sections of the same phrase: features (fft, mel+log, norm+window), int16 in, trunk, out, SAP, softmax+pool, fc, cluster
          ESP_LOGI("STT", "TL spkt seq=%lu frames=%u crop=%d fft=%lu mel=%lu norm=%lu q=%lu nn=%lu dq=%lu sap=%lu pool=%lu fc=%lu clu=%lu",
                   (unsigned long)slot->sequence, unsigned(spk.frames), spk.cropped ? 1 : 0, (unsigned long)spk.fftUs,
                   (unsigned long)spk.melUs, (unsigned long)spk.normUs, (unsigned long)spk.quantUs, (unsigned long)spk.trunkUs,
                   (unsigned long)spk.dequantUs, (unsigned long)spk.sapUs, (unsigned long)spk.poolUs, (unsigned long)spk.fcUs,
                   (unsigned long)spk.clusterUs);
        }
      }
#endif
      sttWipe(text, sizeof(text));
      sttWipe(slot->pcm, std::max(inferenceSamples, slot->samples) * sizeof(int16_t));
#if HW1_STT_CAPTURE_FEATURES
      sttFeatureRelease(run, slot->fe, slot->feGen);  // after publish and save
      slot->fe = -1;
#endif
      perf.doneMs = millis();
      perf.stats = stats;
#ifdef ESP_PLATFORM
      {
        // One timeline record per finished phrase, all times ms since boot:
        // audio a0..a1, closed q, inference start s (after model build m,
        // frontend f, network n, decode d), saved sv0..sv1, done e.
        const uint32_t base = gSTT.captureStartedMs;
        // lm: this phrase was LM-decoded; psram_min: PSRAM low-water mark since
        // boot (includes this phrase's inference arena and LM workspace).
        // int_free/int_min: internal RAM now and its low-water mark since
        // boot, so anything eroding the runtime's per-call internal floor (or
        // BLE's and the 6 KB g2_ctrl_owner stack's headroom) shows on every
        // phrase.
        ESP_LOGI("STT", "TL seg=%lu a0=%lu a1=%lu q=%lu s=%lu m=%lu f=%lu n=%lu d=%lu sv0=%lu sv1=%lu e=%lu ok=%d lm=%d psram_min=%u"
                 " int_free=%u int_min=%u",
                 (unsigned long)slot->sequence,
                 (unsigned long)(base + slot->start / (STT_SAMPLE_RATE / 1000)),
                 (unsigned long)(base + slot->end / (STT_SAMPLE_RATE / 1000)),
                 (unsigned long)slot->queuedMs, (unsigned long)perf.startMs,
                 (unsigned long)stats.loadMs, (unsigned long)stats.frontendMs,
                 (unsigned long)stats.inferenceMs, (unsigned long)stats.decodeMs,
                 (unsigned long)saveStartMs, (unsigned long)saveEndMs,
                 (unsigned long)perf.doneMs, ok ? 1 : 0, stats.lmUsed ? 1 : 0,
                 unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM)),
                 unsigned(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
                 unsigned(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
        // Blocking yields (timed vTaskDelay(1)) and total yield time in n.
        ESP_LOGI("STT", "TL yield seg=%lu blocks=%u yield_us=%lu", (unsigned long)slot->sequence,
                 unsigned(stats.inferenceBlocks), (unsigned long)stats.inferenceYieldUs);
#if HW1_STT_CAPTURE_FEATURES
        // Feature rows of this final, and how many came from the capture-time cache.
        ESP_LOGI("STT", "TL fe seg=%lu frames=%u cached=%u", (unsigned long)slot->sequence,
                 unsigned(stats.featureFrames), unsigned(stats.featureCachedFrames));
#endif
#if HW1_STT_TRIM_SILENCE
        // Captured vs inferred samples (a0/a1 above stay the captured span),
        // voiced end offset (0 = none) and end kind (0 pause, 1 hard limit,
        // 2 session end).
        ESP_LOGI("STT", "TL trim seg=%lu samples=%u inf=%u voiced_end=%u end=%u cut_ms=%u",
                 (unsigned long)slot->sequence, unsigned(slot->samples), unsigned(inferenceSamples),
                 unsigned(slot->voicedEnd), unsigned(slot->endKind),
                 unsigned((slot->samples - trimmedSamples) / (STT_SAMPLE_RATE / 1000)));
#endif
#if HW1_STT_SPECULATIVE_FINAL
        // This phrase matched a speculative request: promoted (s = that run's
        // start, before q; saved_ms = q - its end), or missed (the run had not
        // finished, was aborted, or its length differed).
        if (promoted)
          ESP_LOGI("STT", "TL spec_promote seg=%lu id=%lu s=%lu q=%lu t=%lu", (unsigned long)slot->sequence,
                   (unsigned long)promotedId, (unsigned long)perf.startMs, (unsigned long)slot->queuedMs,
                   (unsigned long)perf.doneMs);
        else if (slot->specId)
          ESP_LOGI("STT", "TL spec_miss seg=%lu id=%lu held=%d", (unsigned long)slot->sequence,
                   (unsigned long)slot->specId, promotedId ? 1 : 0);
#endif
#if HW1_STT_SHORT_CLOSE
        // A pause closed this phrase below the 4 s minimum.
        if (slot->pauseEnd && slot->samples < size_t(4 * STT_SAMPLE_RATE))
          ESP_LOGI("STT", "TL short seg=%lu len_ms=%lu", (unsigned long)slot->sequence,
                   (unsigned long)(slot->samples / (STT_SAMPLE_RATE / 1000)));
#endif
      }
#else
      (void)saveStartMs; (void)saveEndMs;
#endif
#ifdef ESP_PLATFORM
      if (sttTaskSample(gPerfAfter)) sttTaskShares(perf);
#endif
      if (gSTTPerfLog) {
        char line[512];
        sttPerfFormat(perf, line, sizeof(line));
        INFO_SYSTEMF("[STTPERF] %s", line);
      }
      portENTER_CRITICAL(&gSTTMux);
      gSTTPerf[gSTTPerfCount++ % kPerfRecords] = perf;
      gSTT.snapshot.stats = stats;
      gSTT.snapshot.inferenceActive = false;
      slot->state = StreamSlotState::Free;
      portEXIT_CRITICAL(&gSTTMux);
      sttStreamTick(run);
      sttDrainLevelLog(run);
    }
#if HW1_STT_SPEAKER_LABELS
    // The last phrases' labels (bounded wait), then every held transcript line, in order.
    if (labelledLines.count) {
      if (!sttStreamCancelled(&run)) sttSpeakerDrain(&backendSession, 6000);
      sttLabelledPump(labelledLines, backendSession, transcript, run.token, true);
    }
#endif
  } while (false);
  // On normal stop the producer has already joined; on failure/cancel its
  // callback sees the same fence. Never free buffers while either task uses them.
  if (captureStarted) {
    for (;;) {
      portENTER_CRITICAL(&gSTTMux);
      const bool done = run.captureDone;
      portEXIT_CRITICAL(&gSTTMux);
      if (done) break;
      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
#if HW1_STT_CAPTURE_FEATURES
#ifdef ESP_PLATFORM
  if (run.feRing)
    ESP_LOGI("STT", "TL fe_end rows=%lu overruns=%lu finals=%lu cached_finals=%lu cached_rows=%lu draft_rows=%lu",
             (unsigned long)run.feRowsComputed, (unsigned long)run.feOverruns, (unsigned long)run.feFinals,
             (unsigned long)run.feCachedFinals, (unsigned long)run.feCachedRows, (unsigned long)run.feDraftRows);
#endif
  sttFeatureStop(run);  // the capture task has joined; nothing feeds the ring
#endif
#if HW1_STT_SPEAKER_LABELS
  sttSpeakerEnd(&backendSession);  // stop background labelling and free the speaker model before the session goes
#endif
  // FreeRTOS self-delete does not unwind C++ locals. Close the backend (or
  // park it for the next run while a hold lasts) before publishing
  // workerActive=false, after every inference/capture user joined.
  sttWarmPark(backendSession);
#if HW1_STT_SPECULATIVE_FINAL
  sttWipe(gSpecText, sizeof(gSpecText));  // a held, never-promoted result
#endif
  if (run.working) { sttWipe(run.working, kStreamSamples * sizeof(int16_t)); free(run.working); }
  if (run.draftPcm) { sttWipe(run.draftPcm, kStreamSamples * sizeof(int16_t)); free(run.draftPcm); }
  for (auto& slot : run.slots) {
    if (slot.pcm) { sttWipe(slot.pcm, kStreamSamples * sizeof(int16_t)); free(slot.pcm); }
  }
  sttStreamTick(run);
  const bool cancelled = sttCancelled(&run.token);
  // Saving has a completion boundary here. Later cancellation can retire the
  // mailbox, but cannot rewrite this already-admitted final outcome.
  transcript.finish(cancelled ? "cancelled" : run.failed ? "failed" : "done");
  sttPublishTranscript(run.token, transcript.snapshot());
  portENTER_CRITICAL(&gSTTMux);
  if (cancelled || gSTT.cancelRequested) sttClearTextLocked();
  gSTT.snapshot.state = (cancelled || gSTT.cancelRequested) ? STTState::Cancelled
                           : run.failed ? STTState::Failed : STTState::Done;
  gSTT.snapshot.captureActive = false;
  gSTT.snapshot.inferenceActive = false;
  gSTT.snapshot.pendingAudio = 0;
  gSTT.snapshot.workerStackFreeBytes = uxTaskGetStackHighWaterMark(nullptr);
  gSTT.completedMs = millis();
  gSTT.snapshot.workerActive = false;
  portEXIT_CRITICAL(&gSTTMux);
  vTaskDelete(nullptr);
}

} // namespace

static bool sttBeginMode(STTOwner owner, uint32_t captureMs, bool continuous,
                         STTToken* token, char* error, size_t errorCap,
                         const TranscriptOptions* suppliedOptions = nullptr,
                         uint32_t nativeG2Generation = 0,
                         AudioSource preferredSource = AUDIO_SRC_NONE,
                         void (*publishWake)() = nullptr) {
  sttReapTerminal();
  if (token) *token = 0;
  if (!token || !sttOwnerLive(owner)) return sttError(error, errorCap, "A live authenticated session is required");
  if (!continuous && (captureMs < 1000 || captureMs > STT_MAX_CAPTURE_MS))
    return sttError(error, errorCap, "Capture duration must be 1 to 20 seconds");
#if ENABLE_ESP_SR
  if (isESPSRRunning() || audioCaptureOwnedBy("sr"))
    return sttError(error, errorCap, "Stop command recognition with closesr first");
#endif
  if (gMicRunning || micRecordingBusy() || audioCaptureOwnedBy("mic"))
    return sttError(error, errorCap, "Stop recording and closemic first");
  if (audioCaptureBusy()) return sttError(error, errorCap, "Microphone is busy");
  if (!audioAnySourceAvailable()) return sttError(error, errorCap, "No microphone is available");
  if (!sttLocalAvailable(error, errorCap)) return false;

  STTOwner previousOwner;
  portENTER_CRITICAL(&gSTTMux);
  previousOwner = gSTT.owner;
  portEXIT_CRITICAL(&gSTTMux);
  const bool previousLive = sttOwnerLive(previousOwner);
  const uint32_t nonce = esp_random();
  // A caller's per-session source wins over the setting; both are strict.
  const AudioSource requestedSource = preferredSource != AUDIO_SRC_NONE ? preferredSource
      : gSettings.micSource == "pdm" ? AUDIO_SRC_LOCAL_PDM
      : gSettings.micSource == "g2" ? AUDIO_SRC_G2_LEFT : AUDIO_SRC_NONE;
  TranscriptOptions transcriptOptions = suppliedOptions ? *suppliedOptions
      : transcriptCaptureOptions(owner.source, owner.epoch);
  if (transcriptOptions.enabled && (transcriptOptions.source != owner.source ||
                                    transcriptOptions.epoch != owner.epoch)) {
    // A deferred UI handoff can carry only this exact broker owner. Invalid
    // saving identity fails the optional sink, never recognition itself.
    memset(transcriptOptions.user, 0, sizeof(transcriptOptions.user));
    transcriptOptions.source = owner.source;
    transcriptOptions.epoch = owner.epoch;
  }
  bool admitted = false;
  portENTER_CRITICAL(&gSTTMux);
  if (!gSTT.snapshot.workerActive &&
      (!gSTT.snapshot.textReady || sttSameOwner(owner, gSTT.owner) ||
       (sttSameOwner(previousOwner, gSTT.owner) && !previousLive) ||
       millis() - gSTT.completedMs >= kResultRetentionMs)) {
    sttClearTextLocked();
    gSTT = STTRun{};
    if (!gSTTNonce) gSTTNonce = nonce ? nonce : 1;
    if (++gSTTCounter == 0) ++gSTTCounter;
    gSTT.owner = owner;
    gSTT.transcriptOptions = transcriptOptions;
    gSTT.snapshot.transcript.enabled = transcriptOptions.enabled;
    gSTT.requestedSource = nativeG2Generation ? AUDIO_SRC_G2_LEFT : requestedSource;
    gSTT.nativeG2Generation = nativeG2Generation;
    gSTT.publishWake = publishWake;
    gSTT.snapshot.token = (static_cast<uint64_t>(gSTTNonce) << 32) | gSTTCounter;
    gSTT.snapshot.state = STTState::Preparing;
    gSTT.snapshot.captureLimitMs = continuous ? 0 : captureMs;
    gSTT.snapshot.continuous = continuous;
    gSTT.snapshot.workerActive = true;
    gSTT.startedMs = millis();
    gSTT.captureEndedMs = 0;
    *token = gSTT.snapshot.token;
    admitted = true;
  }
  portEXIT_CRITICAL(&gSTTMux);
  if (!admitted) return sttError(error, errorCap, "STT is busy or a result belongs to another session");
  taskStackRecord("stt", kWorkerStackBytes);
  // Core 1 avoids putting the frontend/capture worker on the radio/ISR core;
  // the model backend may use its own qualified multicore operator workers.
  // Priority 2: above the Arduino loop/command work that sttperf showed taking
  // 20-30% of core 1, still below the capture producer (3) so audio never starves.
  if (xTaskCreatePinnedToCore(continuous ? sttContinuousWorker : sttWorker, "stt", kWorkerStackBytes, nullptr,
                              2, nullptr, 1) != pdPASS) {
    portENTER_CRITICAL(&gSTTMux);
    gSTT.snapshot.workerActive = false;
    gSTT.snapshot.state = STTState::Failed;
    gSTT.completedMs = millis();
    snprintf(gSTT.snapshot.error, sizeof(gSTT.snapshot.error), "%s", "Could not create STT worker");
    portEXIT_CRITICAL(&gSTTMux);
    return sttError(error, errorCap, "Could not create STT worker");
  }
  if (error && errorCap) error[0] = '\0';
  return true;
}

bool sttBegin(STTOwner owner, uint32_t captureMs, STTToken* token,
              char* error, size_t errorCap) {
  return sttBeginMode(owner, captureMs, false, token, error, errorCap);
}
bool sttBeginContinuous(STTOwner owner, STTToken* token,
                        char* error, size_t errorCap,
                        const TranscriptOptions* transcriptOptions,
                        AudioSource requestedSource) {
  return sttBeginMode(owner, 0, true, token, error, errorCap, transcriptOptions,
                      /*nativeG2Generation=*/0, requestedSource);
}

bool sttBeginContinuousG2Native(STTOwner owner, uint32_t leftGeneration, STTToken* token,
                                char* error, size_t errorCap,
                                const TranscriptOptions* transcriptOptions,
                                void (*publishWake)()) {
  if (!leftGeneration) {
    if (token) *token = 0;
    return sttError(error, errorCap, "No glasses microphone connection");
  }
  // The glasses stream is the source; the caller's wake is kept for the run.
  return sttBeginMode(owner, 0, true, token, error, errorCap, transcriptOptions, leftGeneration,
                      AUDIO_SRC_NONE, publishWake);
}

bool sttSetPaused(STTOwner owner, STTToken token, bool paused) {
  bool ok = false;
  portENTER_CRITICAL(&gSTTMux);
  if (sttMatchesLocked(owner, token) && gSTT.snapshot.continuous && gSTT.snapshot.workerActive) {
    gSTT.paused = paused;
    ok = true;
  }
  portEXIT_CRITICAL(&gSTTMux);
  return ok;
}

bool sttAnySessionActive() {
  portENTER_CRITICAL(&gSTTMux);
  const bool active = gSTT.snapshot.workerActive;
  portEXIT_CRITICAL(&gSTTMux);
  return active;
}

void sttKeepWarm(uint32_t holdMs, bool load) {
  const uint32_t now = millis();
  // Never beside ESP-SR (no STT run starts while it runs), and never without
  // room for the model, as the last warm load took, above the floor that
  // would free it again at once (sttWarmTick): no load/release loop.
#if ENABLE_ESP_SR
  if (isESPSRRunning()) load = false;
#endif
  const size_t freePsram = load ? sttPsramFree() : 0;
  bool spawn = false;
  portENTER_CRITICAL(&gSTTMux);
  if (!holdMs) {
    gWarm.wanted = false;
  } else {
    const uint32_t until = now + std::min(holdMs, kWarmMaxHoldMs);
    if (!sttWarmHeldLocked(now) || static_cast<int32_t>(until - gWarm.untilMs) > 0) gWarm.untilMs = until;
    gWarm.wanted = true;
    // A run loads (or adopts) its own model; only an idle broker warms one.
    spawn = load && freePsram >= gWarm.loadBytes + kWarmMinFreePsram && !gWarm.parked &&
            !gWarm.claimed && !gSTT.snapshot.workerActive &&
            (!gWarm.failed || now - gWarm.failedMs >= kWarmRetryMs);
    if (spawn) gWarm.claimed = gWarm.loading = true;
  }
  portEXIT_CRITICAL(&gSTTMux);
  if (spawn) {
    taskStackRecord("stt_warm", kWorkerStackBytes);
    // Priority 1, core 1: background work beside the loop, below a run (2).
    if (xTaskCreatePinnedToCore(sttWarmLoader, "stt_warm", kWorkerStackBytes, nullptr, 1,
                                nullptr, 1) != pdPASS) {
      portENTER_CRITICAL(&gSTTMux);
      gWarm.claimed = gWarm.loading = false;
      gWarm.failed = true;
      gWarm.failedMs = now;
      portEXIT_CRITICAL(&gSTTMux);
    }
  }
  if (!holdMs) sttWarmRelease(false);
}

void sttWarmTick() {
  static uint32_t lastMs = 0;
  const uint32_t now = millis();
  if (now - lastMs < 1000) return;
  lastMs = now;
  // Expired stamps are dropped here even with nothing parked (a failed or
  // freed load, a run that took the model past the hold): a stale deadline
  // would read as held again once millis has wrapped half way (~24.9 days).
  // millis is read under the lock, so no stamp it compares with is newer.
  portENTER_CRITICAL(&gSTTMux);
  const uint32_t lockedMs = millis();
  if (!sttWarmHeldLocked(lockedMs)) gWarm.wanted = false;
  if (gWarm.failed && lockedMs - gWarm.failedMs >= kWarmRetryMs) gWarm.failed = false;
  portEXIT_CRITICAL(&gSTTMux);
  if (sttWarmResident()) sttWarmRelease(sttPsramFree() < kWarmMinFreePsram);
}

bool sttWarmResident() {
  portENTER_CRITICAL(&gSTTMux);
  const bool parked = gWarm.parked;
  portEXIT_CRITICAL(&gSTTMux);
  return parked;
}

bool sttReadChunk(STTOwner owner, STTToken token, STTTextChunk* out) {
  uint32_t tlReadSeq = 0, tlReadMs = 0;  // logged after the critical section
  if (out) *out = {};
  sttReapTerminal();
  if (!out || !sttOwnerLive(owner)) return false;
  portENTER_CRITICAL(&gSTTMux);
  const bool ready = sttMatchesLocked(owner, token) &&
      gSTT.snapshot.continuous && !gSTT.cancelRequested && gSTT.snapshot.pendingTexts;
  if (ready) {
    *out = gSTTChunks[gSTT.textHead];
    // First read of a chunk by its consumer (lens, UI, CLI) closes its pacing record.
    for (auto& r : gSTTPerf)
      if (r.sequence == out->sequence && r.doneMs && !r.deliveredMs) { r.deliveredMs = millis(); break; }
    // The perf record is stored after the text is published, so log the first
    // read of each sequence here regardless of it.
    static uint32_t lastReadLogged = 0;
    // Sequences restart at 1 per session (e.g. a Conversate restart): forget
    // the previous session's last.
    static STTToken lastReadToken = 0;
    if (token != lastReadToken) { lastReadToken = token; lastReadLogged = 0; }
    if (out->sequence != lastReadLogged) {
      lastReadLogged = out->sequence;
      tlReadSeq = out->sequence; tlReadMs = millis();
    }
  }
  portEXIT_CRITICAL(&gSTTMux);
#ifdef ESP_PLATFORM
  if (tlReadSeq) ESP_LOGI("STT", "TL read seg=%lu t=%lu", (unsigned long)tlReadSeq, (unsigned long)tlReadMs);
#else
  (void)tlReadSeq; (void)tlReadMs;
#endif
  if (!sttOwnerLive(owner)) { sttWipe(out, sizeof(*out)); return false; }
  return ready;
}

bool sttReadDraft(STTOwner owner, STTToken token, STTDraft* out) {
  if (out) *out = {};
  if (!out || !sttOwnerLive(owner)) return false;
  portENTER_CRITICAL(&gSTTMux);
  const bool ok = sttMatchesLocked(owner, token) && gSTT.snapshot.continuous && !gSTT.cancelRequested;
  if (ok) *out = gSTT.draft;
  portEXIT_CRITICAL(&gSTTMux);
  if (!sttOwnerLive(owner)) { sttWipe(out, sizeof(*out)); return false; }
  return ok;
}

bool sttAcknowledgeChunk(STTOwner owner, STTToken token, uint32_t sequence) {
  if (!sequence || !sttOwnerLive(owner)) return false;
  bool accepted = false;
  portENTER_CRITICAL(&gSTTMux);
  if (sttMatchesLocked(owner, token) && gSTT.snapshot.continuous && !gSTT.cancelRequested) {
    accepted = sequence <= gSTT.acknowledgedSequence;
    if (gSTT.snapshot.pendingTexts && gSTTChunks[gSTT.textHead].sequence == sequence) {
      volatile uint8_t* bytes = reinterpret_cast<volatile uint8_t*>(&gSTTChunks[gSTT.textHead]);
      for (size_t i = 0; i < sizeof(STTTextChunk); ++i) bytes[i] = 0;
      gSTT.textHead = (gSTT.textHead + 1) % kTextQueueDepth;
      --gSTT.snapshot.pendingTexts;
      gSTT.snapshot.textReady = gSTT.snapshot.pendingTexts != 0;
      gSTT.acknowledgedSequence = sequence;
      accepted = true;
    }
  }
  portEXIT_CRITICAL(&gSTTMux);
  return accepted && sttOwnerLive(owner);
}

bool sttRunActive(STTToken token) {
  portENTER_CRITICAL(&gSTTMux);
  const bool active = token && gSTT.snapshot.token == token && gSTT.snapshot.workerActive;
  portEXIT_CRITICAL(&gSTTMux);
  return active;
}

bool sttRequestFinish(STTOwner owner, STTToken token) {
  if (!sttOwnerLive(owner)) return false;
  portENTER_CRITICAL(&gSTTMux);
  const bool accepted = sttMatchesLocked(owner, token) &&
      (gSTT.snapshot.state == STTState::Preparing || gSTT.snapshot.state == STTState::Recording ||
       (gSTT.snapshot.continuous && gSTT.snapshot.workerActive));
  if (accepted) gSTT.finishRequested = true;
  portEXIT_CRITICAL(&gSTTMux);
  return accepted;
}

bool sttCancel(STTOwner owner, STTToken token) {
  if (!sttOwnerLive(owner)) return false;
  portENTER_CRITICAL(&gSTTMux);
  const bool accepted = sttMatchesLocked(owner, token);
  if (accepted) {
    gSTT.cancelRequested = true;
    sttClearTextLocked();
    if (!gSTT.snapshot.workerActive) gSTT.snapshot.state = STTState::Cancelled;
  }
  portEXIT_CRITICAL(&gSTTMux);
  return accepted;
}

bool sttSnapshot(STTOwner owner, STTToken token, STTSnapshot* out) {
  sttReapTerminal();
  if (!out || !sttOwnerLive(owner)) return false;
  portENTER_CRITICAL(&gSTTMux);
  const bool matched = sttSameOwner(owner, gSTT.owner) && gSTT.snapshot.token &&
                       (!token || token == gSTT.snapshot.token);
  if (matched) {
    *out = gSTT.snapshot;
    const uint32_t clockEnd = gSTT.captureEndedMs ? gSTT.captureEndedMs
                              : gSTT.snapshot.workerActive ? millis() : gSTT.completedMs;
    out->elapsedMs = clockEnd - gSTT.startedMs;
  }
  portEXIT_CRITICAL(&gSTTMux);
  return matched && sttOwnerLive(owner);
}

bool sttResult(STTOwner owner, STTToken token, char* text, size_t textCap) {
  sttReapTerminal();
  if (text && textCap) text[0] = '\0';
  if (!text || textCap == 0 || !sttOwnerLive(owner)) return false;
  bool ready = false;
  portENTER_CRITICAL(&gSTTMux);
  if (sttMatchesLocked(owner, token) && !gSTT.snapshot.workerActive &&
      millis() - gSTT.completedMs < kResultRetentionMs &&
      !gSTT.snapshot.continuous && gSTT.snapshot.state == STTState::Done && gSTT.snapshot.textReady &&
      strlen(gSTT.text) < textCap) {
    memcpy(text, gSTT.text, strlen(gSTT.text) + 1);
    ready = true;
  }
  portEXIT_CRITICAL(&gSTTMux);
  if (!sttOwnerLive(owner)) {
    memset(text, 0, textCap);
    return false;
  }
  return ready;
}

const char* sttStateName(STTState state) {
  switch (state) {
    case STTState::Idle: return "idle";
    case STTState::Preparing: return "preparing";
    case STTState::Recording: return "recording";
    case STTState::Transcribing: return "transcribing";
    case STTState::Stopping: return "stopping";
    case STTState::Done: return "done";
    case STTState::Cancelled: return "cancelled";
    case STTState::Failed: return "failed";
  }
  return "unknown";
}

// CLI_ADAPTER_BEGIN — host tests compile the broker above with fake HAL/tasks.
static bool sttParseToken(const String& value, STTToken& token) {
  token = 0;
  if (value.length() != 16) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    const int digit = c >= '0' && c <= '9' ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (digit < 0) return false;
    token = (token << 4) | static_cast<unsigned>(digit);
  }
  return token != 0;
}

static const char* sttPhaseName(STTLocalPhase phase) {
  switch (phase) {
    case STTLocalPhase::Loading: return "loading";
    case STTLocalPhase::Frontend: return "frontend";
    case STTLocalPhase::Inference: return "inference";
    case STTLocalPhase::Decoding: return "decoding";
  }
  return "unknown";
}

static const char* cmd_stt(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  static String reply;
  secureClearString(reply);
  const AuthContext& ctx = currentAuthContext();
  STTOwner owner{ctx.transport, captureTransportSessionEpoch(ctx)};
  if (ctx.user.length() == 0 || ctx.user == "AuthBypass" || !sttOwnerLive(owner))
    return "Error: STT requires a live authenticated serial, web, BLE, OLED or G2 session";
  CommandArgs args(argsInput);
  const String op = args.count() ? args.arg(0) : "status";
  if (args.unterminatedQuote()) return "Error: malformed STT arguments";
  if (op == "record" || op == "start") {
    const bool continuous = op == "start";
    if ((continuous && args.count() != 1) || args.count() > 2) return "Error: Usage: stt start | stt record [1..20 seconds]";
    uint32_t seconds = 10;
    if (args.count() == 2) {
      seconds = 0;
      const String& value = args.arg(1);
      if (!value.length() || value.length() > 2) return "Error: duration must be 1..20 seconds";
      for (size_t i = 0; i < value.length(); ++i) {
        if (value[i] < '0' || value[i] > '9') return "Error: duration must be 1..20 seconds";
        seconds = seconds * 10 + value[i] - '0';
      }
    }
    STTToken token = 0;
    char error[96] = {};
    if (!(continuous ? sttBeginContinuous(owner, &token, error, sizeof(error))
                     : sttBegin(owner, seconds * 1000, &token, error, sizeof(error)))) {
      reply = String("Error: ") + error;
      return reply.c_str();
    }
    char id[17];
    snprintf(id, sizeof(id), "%08lx%08lx", static_cast<unsigned long>(token >> 32),
             static_cast<unsigned long>(token & 0xffffffffu));
    reply = String("{\"accepted\":true,\"id\":\"") + id + "\",\"state\":\"preparing\"}";
    return reply.c_str();
  }
  STTToken token = 0;
  if (op == "ack") {
    if (args.count() != 3 || !sttParseToken(args.arg(1), token))
      return "Error: Usage: stt ack <id> <sequence>";
    const String& value = args.arg(2);
    uint32_t sequence = 0;
    if (!value.length() || value.length() > 10) return "Error: invalid STT sequence";
    for (size_t i = 0; i < value.length(); ++i) {
      const char c = value[i];
      if (c < '0' || c > '9' || sequence > (UINT32_MAX - (c - '0')) / 10)
        return "Error: invalid STT sequence";
      sequence = sequence * 10 + (c - '0');
    }
    return sttAcknowledgeChunk(owner, token, sequence) ? "OK: STT chunk acknowledged"
        : "Error: sequence is not the oldest chunk for this session";
  }
  if (args.count() > 2 || (args.count() == 2 && !sttParseToken(args.arg(1), token)))
    return "Error: STT id must be exactly 16 hex digits";
  if (op == "stop" || op == "cancel") {
    if (!token) return "Error: Usage: stt stop|cancel <id>";
    const bool accepted = op == "stop" ? sttRequestFinish(owner, token) : sttCancel(owner, token);
    return accepted ? "OK: STT request accepted; poll status until terminal"
                    : "Error: no matching STT run accepts that operation";
  }
  STTSnapshot snap;
  if (op == "draft") {
    if (args.count() != 2 || !token) return "Error: Usage: stt draft <id>";
    STTDraft draft;
    if (!sttReadDraft(owner, token, &draft)) return "Error: no STT run belongs to this session";
    JsonDocument doc;
    doc["sequence"] = draft.sequence;
    doc["version"] = draft.version;
    doc["sttText"] = draft.text;  // private tag: shared sinks redact the reply
    serializeJson(doc, reply);
    sttWipe(&draft, sizeof(draft));
    return reply.c_str();
  }
  if (op != "status" && op != "result" && op != "next")
    return "Error: Usage: stt start | record [seconds] | status [id] | stop|cancel|result|next <id> | ack <id> <sequence>";
  if ((op == "result" || op == "next") && !token) return "Error: Usage: stt result <id>";
  if (!sttSnapshot(owner, token, &snap)) return "Error: no STT run belongs to this session";
  JsonDocument doc;
  char id[17];
  snprintf(id, sizeof(id), "%08lx%08lx", static_cast<unsigned long>(snap.token >> 32),
           static_cast<unsigned long>(snap.token & 0xffffffffu));
  doc["id"] = id;
  doc["state"] = sttStateName(snap.state);
  if (op == "next") {
    if (!snap.continuous) return "Error: use stt result for a bounded recording";
    STTTextChunk chunk;
    const bool ready = sttReadChunk(owner, token, &chunk);
    doc["available"] = ready;
    if (ready) {
      doc["sequence"] = chunk.sequence;
      doc["startSample"] = chunk.startSample;
      doc["endSample"] = chunk.endSample;
      doc["forcedBoundary"] = chunk.forcedBoundary;
    }
    doc["sttText"] = chunk.text;
    serializeJson(doc, reply);
    sttWipe(&chunk, sizeof(chunk));
    return reply.c_str();
  } else if (op == "result") {
    if (snap.continuous) return "Error: use stt next and stt ack for continuous results";
    char text[STT_MAX_TEXT + 1] = {};
    if (!sttResult(owner, token, text, sizeof(text))) return "Error: no completed STT result for this id";
    // This explicit tag makes the shared command-output sinks redact the whole
    // reply; only the originating transport receives the private transcript.
    doc["sttText"] = text;
  } else {
    doc["backend"] = "local";
    doc["phase"] = sttPhaseName(snap.phase);
    doc["workerActive"] = snap.workerActive;
    doc["continuous"] = snap.continuous;
    doc["captureActive"] = snap.captureActive;
    doc["inferenceActive"] = snap.inferenceActive;
    doc["sessionMs"] = snap.sessionMs;
    doc["segmentsCaptured"] = snap.segmentsCaptured;
    doc["segmentsCompleted"] = snap.segmentsCompleted;
    doc["pendingAudio"] = snap.pendingAudio;
    doc["pendingTexts"] = snap.pendingTexts;
    doc["audioOverruns"] = snap.audioOverruns;
    doc["textReady"] = snap.textReady;
    doc["elapsedMs"] = snap.elapsedMs;
    doc["captureLimitMs"] = snap.captureLimitMs;
    doc["samples"] = snap.recordedSamples;
    doc["sampleRate"] = STT_SAMPLE_RATE;
    doc["source"] = snap.audioSource == AUDIO_SRC_LOCAL_PDM ? "pdm"
                     : snap.audioSource == AUDIO_SRC_G2_LEFT ? "g2" : "none";
    doc["processing"] = "raw-hal; model frontend only; saved micgain not applied";
    doc["level"] = snap.level;
    doc["rms"] = snap.rms;
    doc["noiseRms"] = snap.noiseRms;
    doc["thresholdRms"] = snap.thresholdRms;
    doc["peakRms"] = snap.peakRms;
    doc["workerStackFreeBytes"] = snap.workerStackFreeBytes;
    doc["captureStackFreeBytes"] = snap.captureStackFreeBytes;
    doc["error"] = snap.error;
    doc["transcriptEnabled"] = snap.transcript.enabled;
    doc["transcriptSaved"] = snap.transcript.saved;
    doc["transcriptComplete"] = snap.transcript.complete;
    doc["transcriptChunks"] = snap.transcript.chunks;
    doc["transcriptBytes"] = snap.transcript.bytes;
    doc["transcriptPath"] = snap.transcript.path;
    doc["transcriptError"] = snap.transcript.error;
    doc["modelBytes"] = snap.stats.modelBytes;
    doc["weightsReused"] = snap.stats.weightsReused;
    doc["featureFrames"] = snap.stats.featureFrames;
    doc["outputFrames"] = snap.stats.outputFrames;
    doc["loadMs"] = snap.stats.loadMs;
    doc["frontendMs"] = snap.stats.frontendMs;
    doc["inferenceMs"] = snap.stats.inferenceMs;
    doc["decodeMs"] = snap.stats.decodeMs;
  }
  reply = "";
  serializeJson(doc, reply);
  return reply.c_str();
}

#if defined(CONFIG_IDF_TARGET_ESP32P4)
#include "stt/quartznet_runtime.h"
// Per-network-stage profile: average ms per segment, slowest first.
static const char* sttStageReport(char* out, size_t cap) {
  static uint32_t totals[256];
  uint32_t segments = 0;
  const size_t n = hw1::stt::stageProfile(totals, 256, &segments);
  if (!segments) return "STT stage profile: no segments recorded yet (run a transcription)";
  uint64_t sum = 0;
  for (size_t i = 0; i < n; ++i) sum += totals[i];
  int w = snprintf(out, cap, "STT stage profile: %u stages, %lu segment(s), avg %.1f ms/segment in stages\n"
                   "idx avg_ms pct (slowest 40; 'sttperf stages all' lists every stage)\n",
                   (unsigned)n, (unsigned long)segments, sum / 1000.0 / segments);
  bool used[256] = {};
  for (int k = 0; k < 40 && w > 0 && (size_t)w < cap; ++k) {
    int best = -1;
    for (size_t i = 0; i < n; ++i) if (!used[i] && (best < 0 || totals[i] > totals[best])) best = (int)i;
    if (best < 0) break;
    used[best] = true;
    w += snprintf(out + w, cap - w, "%3d %7.2f %5.1f%%\n", best, totals[best] / 1000.0 / segments,
                  sum ? 100.0 * totals[best] / sum : 0.0);
  }
  return out;
}
static const char* sttStageDump(char* out, size_t cap) {
  static uint32_t totals[256];
  uint32_t segments = 0;
  const size_t n = hw1::stt::stageProfile(totals, 256, &segments);
  int w = snprintf(out, cap, "stages %u segments %lu us:", (unsigned)n, (unsigned long)segments);
  for (size_t i = 0; i < n && w > 0 && (size_t)w < cap; ++i)
    w += snprintf(out + w, cap - w, "%s%lu", i ? "," : "", (unsigned long)(segments ? totals[i] / segments : 0));
  return out;
}
#endif

static const char* cmd_sttperf(const String& argsInput) {
  RETURN_VALID_IF_VALIDATE_CSTR();
  STT_PERF_TEXT_ATTR static char out[16 * 440 + 256];
  CommandArgs args(argsInput);
  const String op = args.count() ? args.arg(0) : "show";
  if (op == "log") {
    const String v = args.count() > 1 ? args.arg(1) : "";
    if (v == "on") gSTTPerfLog = true;
    else if (v == "off") gSTTPerfLog = false;
    else if (v.length()) return "Error: Usage: sttperf log on|off";
    snprintf(out, sizeof(out), "STT perf live log: %s", gSTTPerfLog ? "ON" : "OFF");
    return out;
  }
#if defined(CONFIG_IDF_TARGET_ESP32P4)
  if (op == "stages") {
    const String v = args.count() > 1 ? args.arg(1) : "";
    if (v == "clear") { hw1::stt::stageProfileReset(); return "OK: STT stage profile cleared"; }
    return v == "all" ? sttStageDump(out, sizeof(out)) : sttStageReport(out, sizeof(out));
  }
#endif
  if (op == "clear") {
    portENTER_CRITICAL(&gSTTMux);
    for (auto& r : gSTTPerf) r = STTPerfRecord{};
    gSTTPerfCount = 0;
    portEXIT_CRITICAL(&gSTTMux);
    return "OK: STT perf records cleared";
  }
  if (op != "show") return "Error: Usage: sttperf [show|clear|log on|off|stages [all|clear]]";
  STTPerfRecord copy[kPerfRecords];
  uint32_t count;
  portENTER_CRITICAL(&gSTTMux);
  memcpy(copy, gSTTPerf, sizeof(copy));
  count = gSTTPerfCount;
  portEXIT_CRITICAL(&gSTTMux);
  const uint32_t shown = std::min<uint32_t>(count, kPerfRecords);
  int n = snprintf(out, sizeof(out),
      "STT perf: %lu segment(s) recorded, newest %lu shown (ms unless noted; rtf=processing/audio; "
      "lag=segment end to text ready; read=+time until the consumer fetched it)\n",
      (unsigned long)count, (unsigned long)shown);
  uint64_t audio = 0, busy = 0;
  for (uint32_t i = count - shown; i < count && n > 0 && (size_t)n < sizeof(out); ++i) {
    const STTPerfRecord& r = copy[i % kPerfRecords];
    n += sttPerfFormat(r, out + n, sizeof(out) - n);
    if ((size_t)n < sizeof(out)) out[n++] = '\n', out[n] = '\0';
    audio += r.audioMs; busy += r.doneMs - r.startMs;
  }
  if (shown && (size_t)n < sizeof(out))
    snprintf(out + n, sizeof(out) - n, "average rtf %.2f over %.1fs of audio", audio ? (double)busy / audio : 0.0, audio / 1000.0);
  return out;
}

const CommandEntry sttCommands[] = {
  {"sttperf", "STT pacing/profiling per segment (timings and CPU only, never text).", false, cmd_sttperf,
   "Usage: sttperf [show|clear|log on|off|stages [all|clear]]"},
  {"stt", "Local continuous or bounded speech-to-text with private session-owned results.", false, cmd_stt,
   "Usage: stt start | record [1..20 seconds] | status [id] | stop|cancel|result|next|draft <id> | ack <id> <sequence>"}
};
const size_t sttCommandsCount = sizeof(sttCommands) / sizeof(sttCommands[0]);
#endif // ENABLE_LOCAL_STT
