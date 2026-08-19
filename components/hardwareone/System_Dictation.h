// System_Dictation.h — speech-to-text as a text INPUT METHOD.
//
// This is the firmware half of "dictate into the OLED keyboard". It is
// deliberately source-agnostic: a dictation capture is an ordinary owned
// recording, so whichever source the mic layer has resolved — onboard PDM or
// the G2 glasses' LEFT-temple mic over BLE — feeds it with no branch here.
// startRecordingInternal() re-resolves the preference and owns every G2-only
// concern (capture container, FAST conn params, stream kick), so the only
// source-specific thing at this layer is the label shown to the user.
//
// Why the CM5 is mandatory: nothing on this device turns speech into arbitrary
// text. ESP-SR is WakeNet + MultiNet — a fixed command grammar that recognizes
// phrases from a table and cannot emit an unseen word — and it is compiled out
// on this build anyway. Free-text transcription lives on the Linux
// co-processor, so a dictation is a round trip:
//
//   1. wearer arms it on the OLED keyboard          dictationBegin()
//   2. owned VAD capture runs, WAV closes           (recorder task)
//   3. firmware pushes `dictate_request <id> <path>`  EVT on the UART link
//   4. host voicefetches the WAV, transcribes it
//   5. host calls `dictate result <id> <text>`      cmd_dictate()
//   6. keyboard drains the text and appends it      dictationTakeText()
//
// SECURITY NOTE — read before touching the EVT push. Every other capture-path
// event is fenced to the epoch latched at ADMISSION, because the admitting
// session is the only party entitled to learn the recording's path (see the
// comment at the mic_autostop push in System_Microphone.cpp). A dictation is
// started by the person physically at the device, so it has no admitting UART
// session and that fence would drop it. The push below therefore targets the
// CURRENTLY authenticated UART session instead. That widening is scoped as
// tightly as it can be: it fires only for an owner this module minted, only
// while that exact dictation is still pending, and only when the local display
// session that armed it is still live. Do not generalize it to other owners.
#ifndef SYSTEM_DICTATION_H
#define SYSTEM_DICTATION_H

#include <Arduino.h>

#include "System_BuildConfig.h"
#include "System_User.h"   // TransportSessionEpoch

struct CommandEntry;

// Longest transcript accepted from the host. Sized to the keyboard buffer —
// anything past the field's own maxLength is truncated at append time anyway,
// and a bounded copy here keeps an oversize host reply from reaching the UI.
#define DICTATION_MAX_TEXT 128

enum class DictationState : uint8_t {
  IDLE = 0,     // nothing in flight; the mode shows "ready"
  RECORDING,    // capture running; VAD or the wearer will end it
  WAITING,      // WAV closed, request pushed, host is transcribing
  FAILED,       // terminal, with a reason; cleared by the next arm
};

struct DictationSnapshot {
  DictationState state;
  const char* sourceName;   // "PDM", "G2", or "none" — which mic is live
  uint32_t elapsedMs;       // time in the current non-idle state
  int level;                // 0..100 audio level while RECORDING, else 0
  char failure[40];
};

#if ENABLE_DICTATION

// Gate for the keyboard mode. False means the mode must not enter the SELECT
// rotation at all — a mode you can cycle into but never use is worse than one
// that isn't there. `whyNot` (optional) receives a short static reason.
bool dictationAvailable(const char** whyNot = nullptr);

// Arm a capture. `displayEpoch` is the local-display transport session that is
// collecting the text; the result is refused later if it no longer matches, so
// a transcript can never land in a different user's field.
bool dictationBegin(TransportSessionEpoch displayEpoch);

// Non-blocking stop request (the wearer pressed record again). The recorder
// finalizes on its own task and the terminal hook below does the rest — this
// never waits, because it runs on the display task.
void dictationRequestStop();

// Abandon whatever is in flight and discard its WAV.
void dictationCancel();

DictationSnapshot dictationSnapshotNow();

// Supervises the recording cap and the host-reply timeout. Called once per
// OLED tick; cheap and safe when idle.
void dictationTick();

// Drains a delivered transcript exactly once. Returns false when nothing is
// waiting. The keyboard appends what it gets rather than replacing, so the
// wearer can dictate and then fix it with the character grid.
bool dictationTakeText(char* out, size_t outSize);

// Recorder-task hook, called from the terminal event site once the WAV is
// closed and fetchable. No-op unless `owner` is the pending dictation.
void dictationOnCaptureClosed(uint64_t owner, const char* path, bool saved);

// Session-boundary reset: a display identity swap must not leave a dictation
// armed against the departed user's field.
void dictationResetForSessionBoundary();

const char* cmd_dictate(const String& argsInput);
extern const CommandEntry dictationCommands[];
extern const size_t dictationCommandsCount;

#else

inline bool dictationAvailable(const char** whyNot = nullptr) {
  if (whyNot) *whyNot = "not built";
  return false;
}
inline bool dictationBegin(TransportSessionEpoch) { return false; }
inline void dictationRequestStop() {}
inline void dictationCancel() {}
inline DictationSnapshot dictationSnapshotNow() {
  return DictationSnapshot{DictationState::IDLE, "none", 0, 0, {}};
}
inline void dictationTick() {}
inline bool dictationTakeText(char*, size_t) { return false; }
inline void dictationOnCaptureClosed(uint64_t, const char*, bool) {}
inline void dictationResetForSessionBoundary() {}

#endif  // ENABLE_DICTATION

#endif  // SYSTEM_DICTATION_H
