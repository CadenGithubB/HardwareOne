#include "System_STT.h"

#if ENABLE_LOCAL_STT
#if !ENABLE_MICROPHONE
#error "ENABLE_LOCAL_STT requires the shared microphone HAL"
#endif

#include "HAL_Audio.h"
#include "System_AuthIdentity.h"
#include "System_Command.h"
#include "System_ESPSR.h"
#include "System_MemUtil.h"
#include "System_Microphone.h"
#include "System_Settings.h"
#include "System_TaskUtils.h"
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

struct STTRun {
  STTOwner owner;
  STTSnapshot snapshot;
  uint32_t startedMs = 0;
  uint32_t completedMs = 0;
  bool cancelRequested = false;
  bool finishRequested = false;
  AudioSource requestedSource = AUDIO_SRC_NONE;
  char text[STT_MAX_TEXT + 1] = {};
};
static portMUX_TYPE gSTTMux = portMUX_INITIALIZER_UNLOCKED;
static STTRun gSTT;
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

static void sttWorker(void*) {
  STTToken token;
  uint32_t captureMs;
  AudioSource requestedSource;
  portENTER_CRITICAL(&gSTTMux);
  token = gSTT.snapshot.token;
  captureMs = gSTT.snapshot.captureLimitMs;
  requestedSource = gSTT.requestedSource;
  portEXIT_CRITICAL(&gSTTMux);

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
  const uint32_t stackFree = uxTaskGetStackHighWaterMark(nullptr);
  portENTER_CRITICAL(&gSTTMux);
  if (gSTT.snapshot.token == token) {
    sttClearTextLocked();
    gSTT.snapshot.stats = stats;
    gSTT.snapshot.recordedSamples = samples;
    gSTT.snapshot.workerStackFreeBytes = stackFree;
    gSTT.snapshot.state = (cancelled || gSTT.cancelRequested) ? STTState::Cancelled
                              : ok ? STTState::Done : STTState::Failed;
    if (gSTT.snapshot.state == STTState::Done) {
      memcpy(gSTT.text, text, sizeof(gSTT.text));
      gSTT.snapshot.textReady = true;
    } else if (gSTT.snapshot.state == STTState::Failed) {
      snprintf(gSTT.snapshot.error, sizeof(gSTT.snapshot.error), "%s",
               error[0] ? error : "Local transcription failed");
    }
    gSTT.completedMs = millis();
    gSTT.snapshot.workerActive = false;
  }
  portEXIT_CRITICAL(&gSTTMux);
  volatile char* privateText = text;
  for (size_t i = 0; i < sizeof(text); ++i) privateText[i] = 0;
  vTaskDelete(nullptr);
}
} // namespace

bool sttBegin(STTOwner owner, uint32_t captureMs, STTToken* token,
              char* error, size_t errorCap) {
  sttReapTerminal();
  if (token) *token = 0;
  if (!token || !sttOwnerLive(owner)) return sttError(error, errorCap, "A live authenticated session is required");
  if (captureMs < 1000 || captureMs > STT_MAX_CAPTURE_MS)
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
    gSTT.requestedSource = requestedSource;
    gSTT.snapshot.token = (static_cast<uint64_t>(gSTTNonce) << 32) | gSTTCounter;
    gSTT.snapshot.state = STTState::Preparing;
    gSTT.snapshot.captureLimitMs = captureMs;
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
  if (xTaskCreatePinnedToCore(sttWorker, "stt", kWorkerStackBytes, nullptr,
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
      (gSTT.snapshot.state == STTState::Preparing || gSTT.snapshot.state == STTState::Recording);
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
      gSTT.snapshot.state == STTState::Done && gSTT.snapshot.textReady &&
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
  if (op == "record") {
    if (args.count() > 2) return "Error: Usage: stt record [1..20 seconds]";
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
    if (!sttBegin(owner, seconds * 1000, &token, error, sizeof(error))) {
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
  if (args.count() > 2 || (args.count() == 2 && !sttParseToken(args.arg(1), token)))
    return "Error: STT id must be exactly 16 hex digits";
  if (op == "stop" || op == "cancel") {
    if (!token) return "Error: Usage: stt stop|cancel <id>";
    const bool accepted = op == "stop" ? sttRequestFinish(owner, token) : sttCancel(owner, token);
    return accepted ? "OK: STT request accepted; poll status until terminal"
                    : "Error: no matching STT run accepts that operation";
  }
  STTSnapshot snap;
  if (op != "status" && op != "result")
    return "Error: Usage: stt record [seconds] | status [id] | stop|cancel|result <id>";
  if (op == "result" && !token) return "Error: Usage: stt result <id>";
  if (!sttSnapshot(owner, token, &snap)) return "Error: no STT run belongs to this session";
  JsonDocument doc;
  char id[17];
  snprintf(id, sizeof(id), "%08lx%08lx", static_cast<unsigned long>(snap.token >> 32),
           static_cast<unsigned long>(snap.token & 0xffffffffu));
  doc["id"] = id;
  doc["state"] = sttStateName(snap.state);
  if (op == "result") {
    char text[STT_MAX_TEXT + 1] = {};
    if (!sttResult(owner, token, text, sizeof(text))) return "Error: no completed STT result for this id";
    // This explicit tag makes the shared command-output sinks redact the whole
    // reply; only the originating transport receives the private transcript.
    doc["sttText"] = text;
  } else {
    doc["backend"] = "local";
    doc["phase"] = sttPhaseName(snap.phase);
    doc["workerActive"] = snap.workerActive;
    doc["textReady"] = snap.textReady;
    doc["elapsedMs"] = snap.elapsedMs;
    doc["captureLimitMs"] = snap.captureLimitMs;
    doc["samples"] = snap.recordedSamples;
    doc["sampleRate"] = STT_SAMPLE_RATE;
    doc["source"] = snap.audioSource == AUDIO_SRC_LOCAL_PDM ? "pdm"
                     : snap.audioSource == AUDIO_SRC_G2_LEFT ? "g2" : "none";
    doc["processing"] = "raw-hal; model frontend only; saved micgain not applied";
    doc["level"] = snap.level;
    doc["workerStackFreeBytes"] = snap.workerStackFreeBytes;
    doc["error"] = snap.error;
    doc["modelBytes"] = snap.stats.modelBytes;
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
  {"stt", "Local buffered speech-to-text with private session-owned results.", false, cmd_stt,
   "Usage: stt record [1..20 seconds] | status [id] | stop|cancel|result <id>"}
};
const size_t sttCommandsCount = sizeof(sttCommands) / sizeof(sttCommands[0]);
#endif // ENABLE_LOCAL_STT
