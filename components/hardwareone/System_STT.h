#pragma once

#include "System_BuildConfig.h"
#include "System_User.h"
#include "System_STTLocal.h"

// Shared bounded dictation broker. These APIs supply text; they never execute
// recognized words as commands. The first local backend is buffered, not live
// captions. Existing Pi-backed System_Dictation is independent and unchanged.
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
  uint32_t recordedSamples = 0;
  uint32_t captureLimitMs = 0;
  uint32_t workerStackFreeBytes = 0;
  uint8_t audioSource = 0;
  int level = 0;
  bool workerActive = false;
  bool textReady = false;
  STTLocalStats stats;
  char error[96] = {};
};

#if ENABLE_LOCAL_STT
// Owner must represent an authenticated, live session. Headless serial/web,
// authenticated BLE and future OLED/G2 callers use the same session fence.
// Start requires SR, microphone sensor and recorder already stopped. It never
// changes saved audio settings, writes a WAV, or implicitly re-arms voice.
bool sttBegin(STTOwner owner, uint32_t captureMs, STTToken* token,
              char* error, size_t errorCap);
// Internal join predicate: contains no transcript/session data and remains
// usable after revocation. Unknown or terminal tokens are inactive.
bool sttRunActive(STTToken token);
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
