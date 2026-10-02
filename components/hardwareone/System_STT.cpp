#include "System_STT.h"

#if ENABLE_LOCAL_STT
#if !ENABLE_MICROPHONE
#error "ENABLE_LOCAL_STT requires the shared microphone HAL"
#endif

#include "HAL_Audio.h"
#include "G2_Glasses.h"
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
constexpr size_t kStreamSamples = STT_SAMPLE_RATE * STT_MAX_CAPTURE_MS / 1000;
constexpr size_t kStreamReadSamples = 1024; // Whole PDM DMA blocks; segmenting is downstream.
constexpr uint32_t kCaptureStackBytes = 8192; // 2 KiB PCM block plus HAL/BLE startup and G2 lens container depth.
enum class StreamSlotState : uint8_t { Free, Ready, Processing };
struct StreamSlot {
  int16_t* pcm = nullptr;
  size_t samples = 0;
  uint64_t start = 0, end = 0;
  uint32_t sequence = 0;
  uint32_t queuedMs = 0;  // when the segmenter closed this segment (sttperf)
  bool forced = false;
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
  slot->queuedMs = millis();
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
  bool selected = false, claimed = false, g2Held = false;
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
    if (source == AUDIO_SRC_G2_LEFT) g2Held = sttG2CaptureBegin();
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
        // Live mode: offer the utterance in progress for a draft once it has
        // >=1 s of audio and >=0.6 s more than the last snapshot. The worker
        // paces itself: a new snapshot waits until the previous draft is done.
        const int16_t* live = nullptr;
        size_t liveSamples = 0;
        if (run.draftPcm && run.segmenter.in_progress(&live, &liveSamples)) {
          portENTER_CRITICAL(&gSTTMux);
          const uint32_t nextSequence = gSTT.snapshot.segmentsCaptured + 1;
          const bool idle = !run.draftReady && !run.draftBusy;
          if (run.draftSequence != nextSequence) run.draftTakenSamples = 0;
          const bool grown = liveSamples >= STT_SAMPLE_RATE &&
                             liveSamples >= run.draftTakenSamples + STT_SAMPLE_RATE * 6 / 10;
          portEXIT_CRITICAL(&gSTTMux);
          if (idle && grown) {
            memcpy(run.draftPcm, live, liveSamples * sizeof(int16_t));
            portENTER_CRITICAL(&gSTTMux);
            run.draftSamples = liveSamples;
            run.draftSequence = nextSequence;
            run.draftTakenSamples = liveSamples;
            run.draftReady = true;
            portEXIT_CRITICAL(&gSTTMux);
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
      if (millis() - lastAudioMs >= sttSourceTimeoutMs(source)) {
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
    // Draft buffer is optional: without PSRAM for it, live mode just stays off.
    run.draftPcm = static_cast<int16_t*>(ps_alloc(kStreamSamples * sizeof(int16_t), AllocPref::RequirePSRAM, "stt.draft"));
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
        if (gSTTPerfLog)
          INFO_SYSTEMF("[STTPERF] warm-up %s in %lu ms (load %lu, lm %s)", warmed ? "ok" : warmError,
                       (unsigned long)(millis() - warmStart), (unsigned long)warmStats.loadMs,
                       warmStats.lmUsed ? "ready" : "not used");
        // A warm-up failure is not fatal: the first real segment reports it.
      }
    }
#endif
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
        bool drafting = false;
        size_t draftSamples = 0;
        uint32_t draftSequence = 0;
        portENTER_CRITICAL(&gSTTMux);
        if (run.draftReady && !run.failed && !gSTT.cancelRequested) {
          run.draftReady = false; run.draftBusy = drafting = true;
          draftSamples = std::max(run.draftSamples, size_t(320));
          draftSequence = run.draftSequence;
        }
        portEXIT_CRITICAL(&gSTTMux);
        if (drafting) {
          if (draftSamples > run.draftSamples)
            memset(run.draftPcm + run.draftSamples, 0, (draftSamples - run.draftSamples) * sizeof(int16_t));
          char draftText[STT_MAX_TEXT + 1] = {};
          char draftError[96] = {};
          STTLocalStats draftStats;
          const STTLocalControl draftControl{&run, sttStreamCancelled, sttStreamProgress};
          const bool ok = sttLocalTranscribe(run.draftPcm, draftSamples, draftText, sizeof(draftText), draftControl,
                                             draftStats, draftError, sizeof(draftError), &backendSession);
          portENTER_CRITICAL(&gSTTMux);
          // Publish only while its final chunk has not been produced yet.
          if (ok && memchr(draftText, '\0', sizeof(draftText)) && !gSTT.cancelRequested && !run.failed &&
              gSTT.snapshot.segmentsCompleted < draftSequence) {
            memcpy(gSTT.draft.text, draftText, sizeof(draftText));
            gSTT.draft.sequence = draftSequence;
            ++gSTT.draft.version;
          }
          run.draftBusy = false;
          portEXIT_CRITICAL(&gSTTMux);
          sttWipe(draftText, sizeof(draftText));
          if (gSTTPerfLog)
            INFO_SYSTEMF("[STTPERF] draft seq %lu audio %.1fs in %lu ms (%s)", (unsigned long)draftSequence,
                         draftSamples / (double)STT_SAMPLE_RATE,
                         (unsigned long)(draftStats.loadMs + draftStats.frontendMs + draftStats.inferenceMs + draftStats.decodeMs),
                         ok ? "ok" : draftError);
          sttStreamTick(run);
          continue;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        sttStreamTick(run);
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
      perf.doneMs = millis();
      perf.stats = stats;
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
  if (ready) {
    *out = gSTTChunks[gSTT.textHead];
    // First read of a chunk by its consumer (lens, UI, CLI) closes its pacing record.
    for (auto& r : gSTTPerf)
      if (r.sequence == out->sequence && r.doneMs && !r.deliveredMs) { r.deliveredMs = millis(); break; }
  }
  portEXIT_CRITICAL(&gSTTMux);
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
