#include "System_STT.h"

#if ENABLE_LOCAL_STT
#if !ENABLE_MICROPHONE
#error "ENABLE_LOCAL_STT requires the shared microphone HAL"
#endif

#include "HAL_Audio.h"
#include "System_AuthIdentity.h"
#include "System_Command.h"
#include "System_Debug.h"
#include "System_ESPSR.h"
#include "System_MemUtil.h"
#include "System_Microphone.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
#include "stt/stt_segmenter.h"
#include <ArduinoJson.h>
#include <esp_random.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
constexpr uint32_t kWorkerStackBytes = 12288; // IDF task stack sizes are bytes.
constexpr uint32_t kResultRetentionMs = 300000;
constexpr size_t kReadSamples = 640; // 40 ms at the model's fixed 16 kHz rate.
constexpr uint32_t kSourceTimeoutMs = 2000;
constexpr size_t kTextQueueDepth = 8;

struct STTRun {
  STTOwner owner;
  TranscriptOptions transcriptOptions;
  STTSnapshot snapshot;
  uint32_t startedMs = 0;
  uint32_t completedMs = 0;
  bool cancelRequested = false;
  bool finishRequested = false;
  AudioSource requestedSource = AUDIO_SRC_NONE;
  char text[STT_MAX_TEXT + 1] = {};
  uint8_t textHead = 0;
  uint32_t acknowledgedSequence = 0;
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
      if (millis() - lastAudioMs >= kSourceTimeoutMs ||
          millis() - captureStartedMs > captureMs + kSourceTimeoutMs) {
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
constexpr size_t kStreamSamples = STT_SAMPLE_RATE * STT_MAX_CAPTURE_MS / 1000;
constexpr size_t kStreamReadSamples = 1024; // Whole PDM DMA blocks; segmenting is downstream.
constexpr uint32_t kCaptureStackBytes = 6144; // 2 KiB PCM block plus HAL/BLE startup depth.
enum class StreamSlotState : uint8_t { Free, Ready, Processing };
struct StreamSlot {
  int16_t* pcm = nullptr;
  size_t samples = 0;
  uint64_t start = 0, end = 0;
  uint32_t sequence = 0;
  bool forced = false;
  StreamSlotState state = StreamSlotState::Free;
};
struct StreamRun {
  STTToken token = 0;
  AudioSource requested = AUDIO_SRC_NONE;
  int16_t* working = nullptr;
  StreamSlot slots[kAudioQueueDepth];
  hw1::stt::Segmenter segmenter;
  bool captureDone = false;
  bool failed = false;
  uint32_t clockMs = 0;
};
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
static void sttStreamProgress(void* context, STTLocalPhase phase) {
  auto& run = *static_cast<StreamRun*>(context);
  sttProgress(&run.token, phase);
  sttStreamTick(run);
}
static void sttWipe(void* memory, size_t bytes) {
  if (!memory) return;
  volatile uint8_t* p = static_cast<volatile uint8_t*>(memory);
  while (bytes--) *p++ = 0;
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
  portENTER_CRITICAL(&gSTTMux);
  slot->sequence = ++gSTT.snapshot.segmentsCaptured;
  slot->state = StreamSlotState::Ready;
  ++gSTT.snapshot.pendingAudio;
  portEXIT_CRITICAL(&gSTTMux);
  return run.segmenter.release();
}
static void sttCaptureWorker(void* context) {
  auto& run = *static_cast<StreamRun*>(context);
  const AudioSource previous = audioGetSource();
  AudioSource source = AUDIO_SRC_NONE;
  bool selected = false, claimed = false;
  int16_t block[kStreamReadSamples];
  do {
    if (sttStreamCancelled(&run)) break;
    if (audioCaptureBusy() || micRecordingBusy() || gMicRunning) {
      sttStreamFail(run, "Audio became busy; close SR/mic and retry"); break;
    }
    if (run.requested != AUDIO_SRC_NONE && !audioSourceAvailable(run.requested)) {
      sttStreamFail(run, "Selected microphone is unavailable"); break;
    }
    selected = audioSetSource(run.requested);
    if (!selected || !audioCaptureStart("stt", STT_SAMPLE_RATE)) {
      sttStreamFail(run, "Could not claim microphone; close SR/mic first"); break;
    }
    claimed = true;
    source = audioGetSource();
    if (run.requested != AUDIO_SRC_NONE && source != run.requested) {
      sttStreamFail(run, "Selected microphone changed during startup"); break;
    }
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
    portEXIT_CRITICAL(&gSTTMux);
    uint32_t lastAudioMs = millis();
    while (!sttStreamCancelled(&run)) {
      bool finish;
      portENTER_CRITICAL(&gSTTMux);
      finish = gSTT.finishRequested;
      portEXIT_CRITICAL(&gSTTMux);
      if (finish) break;
      if (!audioCaptureOwnedBy("stt") || !audioCaptureActive() || !audioSourceAvailable(source)) {
        sttStreamFail(run, "Microphone source lost"); break;
      }
      const size_t count = audioReadPcm(block, kStreamReadSamples, 80);
      if (count > kStreamReadSamples) { sttStreamFail(run, "Invalid microphone sample count"); break; }
      uint32_t overruns = 0;
      if (!audioCaptureOverruns("stt", &overruns)) {
        sttStreamFail(run, "Microphone integrity monitor lost"); break;
      }
      if (overruns != initialOverruns) {
        portENTER_CRITICAL(&gSTTMux);
        gSTT.snapshot.audioOverruns = overruns - initialOverruns;
        portEXIT_CRITICAL(&gSTTMux);
        sttStreamFail(run, "Microphone audio loss detected; session stopped"); break;
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
        portENTER_CRITICAL(&gSTTMux);
        gSTT.snapshot.recordedSamples = run.segmenter.samples_seen();
        if (run.segmenter.calibrated()) gSTT.snapshot.state = STTState::Recording;
        gSTT.snapshot.rms = run.segmenter.current_rms();
        gSTT.snapshot.noiseRms = run.segmenter.noise_rms();
        gSTT.snapshot.thresholdRms = run.segmenter.threshold_rms();
        gSTT.snapshot.peakRms = run.segmenter.peak_rms();
        gSTT.snapshot.level = level;
        gSTT.snapshot.captureStackFreeBytes = uxTaskGetStackHighWaterMark(nullptr);
        portEXIT_CRITICAL(&gSTTMux);
      } else vTaskDelay(pdMS_TO_TICKS(1));
      sttStreamTick(run);
      if (millis() - lastAudioMs >= kSourceTimeoutMs) {
        sttStreamFail(run, "Microphone PCM delivery timed out"); break;
      }
    }
    // A normal stop seals and drains the final voiced tail. Cancel/error never
    // turns abandoned partial audio into a new inference request.
    if (!sttStreamCancelled(&run)) {
      uint32_t finalOverruns = 0;
      if (!audioCaptureOverruns("stt", &finalOverruns)) {
        sttStreamFail(run, "Microphone integrity monitor lost at stop");
      } else if (finalOverruns) {
        portENTER_CRITICAL(&gSTTMux);
        gSTT.snapshot.audioOverruns = finalOverruns;
        portEXIT_CRITICAL(&gSTTMux);
        sttStreamFail(run, "Microphone audio loss detected at stop");
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
  gSTT.snapshot.state = STTState::Stopping;
  gSTT.snapshot.level = 0;
  portEXIT_CRITICAL(&gSTTMux);
  if (claimed || audioCaptureOwnedBy("stt")) {
    audioCaptureStop("stt");
    while (audioCaptureOwnedBy("stt")) vTaskDelay(pdMS_TO_TICKS(10));
  }
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
static void sttContinuousWorker(void*) {
  StreamRun run;
  STTLocalSession backendSession;
  TranscriptOptions transcriptOptions;
  portENTER_CRITICAL(&gSTTMux);
  run.token = gSTT.snapshot.token;
  run.requested = gSTT.requestedSource;
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
    if (sttStreamCancelled(&run)) break;
    hw1::stt::SegmenterConfig segmentConfig;
    segmentConfig.adaptive = true;
    if (!run.segmenter.reset(run.working, kStreamSamples, segmentConfig)) {
      sttStreamFail(run, "Invalid continuous segmentation settings"); break;
    }
    taskStackRecord("stt_capture", kCaptureStackBytes);
    if (xTaskCreatePinnedToCore(sttCaptureWorker, "stt_capture", kCaptureStackBytes,
                                &run, 3, nullptr, 1) != pdPASS) {
      sttStreamFail(run, "Could not create continuous capture worker"); break;
    }
    captureStarted = true;
    for (;;) {
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
        vTaskDelay(pdMS_TO_TICKS(10));
        sttStreamTick(run);
        continue;
      }
      char text[STT_MAX_TEXT + 1] = {};
      char error[96] = {};
      STTLocalStats stats;
      const STTLocalControl control{&run, sttStreamCancelled, sttStreamProgress};
      // A stop immediately after a forced boundary can leave fewer than20 ms
      // of real audio. Pad only the backend copy; published positions retain
      // the exact captured extent, so no samples disappear or move in time.
      const size_t inferenceSamples = std::max(slot->samples, size_t(320));
      if (inferenceSamples > slot->samples)
        memset(slot->pcm + slot->samples, 0, (inferenceSamples - slot->samples) * sizeof(int16_t));
      const bool ok = sttLocalTranscribe(slot->pcm, inferenceSamples, text, sizeof(text), control,
                                        stats, error, sizeof(error), &backendSession);
      bool acceptedText = false;
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
          } else if (!gSTT.cancelRequested && !run.failed) {
            run.failed = true;
            snprintf(gSTT.snapshot.error, sizeof(gSTT.snapshot.error), "%s", "STT text queue full; receiver must acknowledge results");
          }
          portEXIT_CRITICAL(&gSTTMux);
        }
      }
      // Save the whole accepted chunk once, before UI splitting/ACK can erase
      // its mailbox copy. Only this inference worker performs transcript I/O.
      if (acceptedText) {
        (void)transcript.append(slot->sequence, text, strlen(text));
        sttPublishTranscript(run.token, transcript.snapshot());
      }
      sttWipe(text, sizeof(text));
      sttWipe(slot->pcm, inferenceSamples * sizeof(int16_t));
      portENTER_CRITICAL(&gSTTMux);
      gSTT.snapshot.stats = stats;
      gSTT.snapshot.inferenceActive = false;
      slot->state = StreamSlotState::Free;
      portEXIT_CRITICAL(&gSTTMux);
      sttStreamTick(run);
    }
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
  // FreeRTOS self-delete does not unwind C++ locals. Close the backend before
  // publishing workerActive=false, after every inference/capture user joined.
  backendSession.reset();
  if (run.working) { sttWipe(run.working, kStreamSamples * sizeof(int16_t)); free(run.working); }
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
                         const TranscriptOptions* suppliedOptions = nullptr) {
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
  const AudioSource requestedSource = gSettings.micSource == "pdm" ? AUDIO_SRC_LOCAL_PDM
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
    gSTT.requestedSource = requestedSource;
    gSTT.snapshot.token = (static_cast<uint64_t>(gSTTNonce) << 32) | gSTTCounter;
    gSTT.snapshot.state = STTState::Preparing;
    gSTT.snapshot.captureLimitMs = continuous ? 0 : captureMs;
    gSTT.snapshot.continuous = continuous;
    gSTT.snapshot.workerActive = true;
    gSTT.startedMs = millis();
    *token = gSTT.snapshot.token;
    admitted = true;
  }
  portEXIT_CRITICAL(&gSTTMux);
  if (!admitted) return sttError(error, errorCap, "STT is busy or a result belongs to another session");
  taskStackRecord("stt", kWorkerStackBytes);
  // Core 1 avoids putting the frontend/capture worker on the radio/ISR core;
  // the model backend may use its own qualified multicore operator workers.
  if (xTaskCreatePinnedToCore(continuous ? sttContinuousWorker : sttWorker, "stt", kWorkerStackBytes, nullptr,
                              1, nullptr, 1) != pdPASS) {
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
                        const TranscriptOptions* transcriptOptions) {
  return sttBeginMode(owner, 0, true, token, error, errorCap, transcriptOptions);
}

bool sttReadChunk(STTOwner owner, STTToken token, STTTextChunk* out) {
  if (out) *out = {};
  sttReapTerminal();
  if (!out || !sttOwnerLive(owner)) return false;
  portENTER_CRITICAL(&gSTTMux);
  const bool ready = sttMatchesLocked(owner, token) &&
      gSTT.snapshot.continuous && !gSTT.cancelRequested && gSTT.snapshot.pendingTexts;
  if (ready) *out = gSTTChunks[gSTT.textHead];
  portEXIT_CRITICAL(&gSTTMux);
  if (!sttOwnerLive(owner)) { sttWipe(out, sizeof(*out)); return false; }
  return ready;
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
    out->elapsedMs = (gSTT.snapshot.workerActive ? millis() : gSTT.completedMs) - gSTT.startedMs;
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

const CommandEntry sttCommands[] = {
  {"stt", "Local continuous or bounded speech-to-text with private session-owned results.", false, cmd_stt,
   "Usage: stt start | record [1..20 seconds] | status [id] | stop|cancel|result|next <id> | ack <id> <sequence>"}
};
const size_t sttCommandsCount = sizeof(sttCommands) / sizeof(sttCommands[0]);
#endif // ENABLE_LOCAL_STT
