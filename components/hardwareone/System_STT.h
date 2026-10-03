#pragma once

#include "System_BuildConfig.h"
#include "System_User.h"
#include "System_STTLocal.h"
#include "System_Transcript.h"

// Shared dictation session broker. Audio remains bounded even for sessions
// with no duration limit. Recognized words are text, never device commands.
// A backend processes independent raw PCM segments; transport/UI own delivery.
constexpr size_t STT_MAX_TEXT = 512;
constexpr uint32_t STT_SAMPLE_RATE = 16000;
constexpr uint32_t STT_MAX_CAPTURE_MS = 20000;
using STTToken = uint64_t;

struct STTOwner {
  CommandSource source = SOURCE_INTERNAL;
  TransportSessionEpoch epoch = 0;
};

enum class STTState : uint8_t {
  Idle, Preparing, Recording, Transcribing, Stopping, Done, Cancelled, Failed
};

struct STTSnapshot {
  STTToken token = 0;
  STTState state = STTState::Idle;
  STTLocalPhase phase = STTLocalPhase::Loading;
  uint32_t elapsedMs = 0;
  uint64_t recordedSamples = 0;
  uint32_t captureLimitMs = 0;
  uint32_t workerStackFreeBytes = 0;
  uint32_t captureStackFreeBytes = 0;
  uint8_t audioSource = 0;
  int level = 0;
  uint16_t rms = 0;
  uint16_t noiseRms = 0;
  uint16_t thresholdRms = 0;
  uint16_t peakRms = 0;
  bool workerActive = false;
  bool textReady = false;
  bool continuous = false;
  bool captureActive = false;
  bool inferenceActive = false;
  uint32_t segmentsCaptured = 0;
  uint32_t segmentsCompleted = 0;
  uint32_t pendingAudio = 0;
  uint32_t pendingTexts = 0;
  uint32_t audioOverruns = 0;
  uint64_t sessionMs = 0;
  STTLocalStats stats;
  TranscriptStatus transcript;
  char error[96] = {};
};

// Each result carries its place in the session, independent of its backend.
// Offsets count raw 16 kHz samples since capture began, including idle gaps.
struct STTTextChunk {
  uint32_t sequence = 0;
  uint64_t startSample = 0;
  uint64_t endSample = 0;
  bool forcedBoundary = false;
  char text[STT_MAX_TEXT + 1] = {};
};

#if ENABLE_LOCAL_STT
// Owner must represent an authenticated, live session. Headless serial/web,
// authenticated BLE and future OLED/G2 callers use the same session fence.
// Start requires SR, microphone sensor and recorder already stopped. It never
// changes saved audio settings, writes a WAV, or implicitly re-arms voice.
bool sttBegin(STTOwner owner, uint32_t captureMs, STTToken* token,
              char* error, size_t errorCap);
// Continuous sessions have no duration cap. Natural pauses produce bounded
// segments; capture proceeds while the previous segment is transcribed. Stop
// drains admitted audio. Cancel discards it. A slow backend/client causes an
// explicit bounded-queue failure, never silent audio/text overwrite.
bool sttBeginContinuous(STTOwner owner, STTToken* token,
                        char* error, size_t errorCap,
                        const TranscriptOptions* transcriptOptions = nullptr);
// Retry-safe oldest-result peek. Acknowledge only after a consumer accepts the
// text; an ack can remove only the exact oldest chunk (or repeat an earlier ack).
// Both APIs enforce the same live owner/epoch/token fence as one-shot results.
bool sttReadChunk(STTOwner owner, STTToken token, STTTextChunk* out);
// Live mode: provisional text for the utterance still being spoken. `sequence`
// is the chunk sequence it will become; `version` changes on every update and
// when the draft is cleared (its final chunk was published or the run ended).
struct STTDraft {
  uint32_t sequence = 0;
  uint32_t version = 0;
  char text[STT_MAX_TEXT + 1] = {};
};
bool sttReadDraft(STTOwner owner, STTToken token, STTDraft* out);
bool sttAcknowledgeChunk(STTOwner owner, STTToken token, uint32_t sequence);
// Internal join predicate: contains no transcript/session data and remains
// usable after revocation. Unknown or terminal tokens are inactive.
bool sttRunActive(STTToken token);
// Continuous transcription on the native Conversate microphone stream of the
// exact left-temple connection `leftGeneration` (see audioCaptureStartG2Native).
bool sttBeginContinuousG2Native(STTOwner owner, uint32_t leftGeneration, STTToken* token,
                                char* error, size_t errorCap,
                                const TranscriptOptions* transcriptOptions = nullptr);
// Wearer pause/resume of a native session: no PCM arrives while paused, which
// must not be mistaken for a failed microphone; the open phrase is closed.
bool sttSetPaused(STTOwner owner, STTToken token, bool paused);
// True while any STT worker (capture or transcription tail) is running.
// Owner-free and read-only: lets UI keep-alives follow device activity.
bool sttAnySessionActive();
bool sttRequestFinish(STTOwner owner, STTToken token);
bool sttCancel(STTOwner owner, STTToken token);
// token=0 means this owner's current run. No other session's data is exposed.
bool sttSnapshot(STTOwner owner, STTToken token, STTSnapshot* out);
// Idempotent exact-owner result read; retained until next same-owner run,
// logout/revocation or five-minute expiry. UI consumers append once per token.
bool sttResult(STTOwner owner, STTToken token, char* text, size_t textCap);
const char* sttStateName(STTState state);

struct CommandEntry;
extern const CommandEntry sttCommands[];
extern const size_t sttCommandsCount;
#endif
