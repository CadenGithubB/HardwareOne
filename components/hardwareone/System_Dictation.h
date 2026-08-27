// System_Dictation.h — speech-to-text as a text INPUT METHOD.
//
// This is the firmware half of "dictate into a keyboard". It is
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
//   1. wearer arms it on an OLED/G2 keyboard        dictationBegin*()
//   2. owned VAD capture runs, WAV/result/IDLE publish (recorder task)
//   3. service worker pushes `dictate_request <id> <path>` EVT on UART
//   4. host voicefetches the WAV, transcribes it
//   5. host sends `dictate result <id> <text>`      direct UART control plane
//   6. owning keyboard drains and appends it        dictationTakeText*()
// If the wearer cancels after step 3, firmware sends
// `dictate_cancel <id>` to that same UART epoch so the host can tombstone and
// stop the exact queued/running job. Cancel before the final request fence emits
// neither event.
//
// SECURITY NOTE — read before touching the EVT push. Every other capture-path
// event is fenced to the epoch latched at ADMISSION, because the admitting
// session is the only party entitled to learn the recording's path (see the
// comment at the mic_autostop push in System_Microphone.cpp). A dictation is
// started by the person physically at the device, so it has no admitting UART
// session and that fence would drop it. The push below therefore targets the
// CURRENTLY authenticated UART session instead. That widening is scoped as
// tightly as it can be: it fires only for an owner this module minted, only
// while that exact dictation is still pending, and only when the input-surface
// session that armed it is still live. Do not generalize it to other owners.
#ifndef SYSTEM_DICTATION_H
#define SYSTEM_DICTATION_H

#include <Arduino.h>

#include "System_BuildConfig.h"
#include "System_DictationPolicy.h"  // exact direct-UART namespace predicate
#include "System_User.h"   // TransportSessionEpoch

// Longest transcript accepted from the host. Sized to the keyboard buffer —
// anything past the field's own maxLength is truncated at append time anyway,
// and a bounded copy here keeps an oversize host reply from reaching the UI.
#define DICTATION_MAX_TEXT 256

enum class DictationState : uint8_t {
  IDLE = 0,     // nothing in flight; the mode shows "ready"
  RECORDING,    // capture running; VAD or the wearer will end it
  WAITING,      // WAV closed, request pushed, host is transcribing
  FAILED,       // terminal, with a reason; cleared by the next arm
};

struct DictationSnapshot {
  DictationState state;
  CommandSource ownerSource;  // OLED or G2 surface that owns this exchange
  const char* sourceName;   // "PDM", "G2", or "none" — which mic is live
  uint32_t elapsedMs;       // time in the current non-idle state
  int level;                // 0..100 audio level while RECORDING, else 0
  char failure[40];
};

enum class DictationUartIntrinsicResult : uint8_t {
  NotHandled = 0,
  Handled,
};

#if ENABLE_DICTATION

// Gate for the keyboard mode. False means the mode must not enter the SELECT
// rotation at all — a mode you can cycle into but never use is worse than one
// that isn't there. `whyNot` (optional) receives a short static reason.
bool dictationAvailable(const char** whyNot = nullptr);

// OLED-default arm. `displayEpoch` is the local-display transport session that
// is collecting the text; the result is refused later if it no longer matches,
// so a transcript can never land in a different user's field.
bool dictationBegin(TransportSessionEpoch displayEpoch);

// G2 counterpart to the OLED-default API above. The source is part of both
// the authority fence and the consumer identity: an OLED reset must not cancel
// a glasses-owned recording, and one surface must never drain the other's
// transcript. Only SOURCE_LOCAL_DISPLAY and SOURCE_G2_GLASSES are accepted.
bool dictationBeginFor(CommandSource displaySource,
                       TransportSessionEpoch displayEpoch);

// Non-blocking stop request (the wearer pressed record again). The recorder
// finalizes on its own task and the terminal hook below does the rest — this
// never waits, because it runs on the display task.
void dictationRequestStop();
void dictationRequestStopFor(CommandSource displaySource);

// Abandon whatever is in flight and discard its WAV.
void dictationCancel();
void dictationCancelFor(CommandSource displaySource);

DictationSnapshot dictationSnapshotNow();

// Supervises the recording cap and the host-reply timeout. Called once per
// OLED tick; cheap and safe when idle.
void dictationTick();

// Drains a delivered transcript exactly once. Returns false when nothing is
// waiting. The keyboard appends what it gets rather than replacing, so the
// wearer can dictate and then fix it with the character grid.
bool dictationTakeText(char* out, size_t outSize);
bool dictationTakeTextFor(CommandSource displaySource,
                          char* out, size_t outSize);

// Post-publication hook. The mic layer invokes this only AFTER it has published
// the owner-scoped completion result and IDLE. It copies the stable local result
// and wakes the service worker; it never does UART or filesystem work on the
// recorder task. No-op unless `owner` is this module's live/cleanup exchange.
void dictationOnCapturePublished(uint64_t owner, const char* path, bool saved,
                                 const char* failure);

// OLED/local-display session-boundary reset. G2 owns an independent paired-user
// epoch, so a local-display identity swap must not cancel a glasses dictation.
void dictationResetForSessionBoundary();

// Host control plane (`dictate hostready/result/fail/status`). UART invokes
// this before cmd_exec, like CM5 heartbeat/time/LLM callbacks. The caller must
// pin and pass one coherent named+physical session snapshot, then admit the
// returned reply only to that same physical transport incarnation.
DictationUartIntrinsicResult dictationHandleUartIntrinsic(
    const char* line, uint32_t namedEpoch, bool controlAllowed,
    char* replyOut, size_t replyOutSize);
bool dictationIsUartProtocolLine(const char* line);

#else

inline bool dictationAvailable(const char** whyNot = nullptr) {
  if (whyNot) *whyNot = "not built";
  return false;
}
inline bool dictationBegin(TransportSessionEpoch) { return false; }
inline bool dictationBeginFor(CommandSource, TransportSessionEpoch) {
  return false;
}
inline void dictationRequestStop() {}
inline void dictationRequestStopFor(CommandSource) {}
inline void dictationCancel() {}
inline void dictationCancelFor(CommandSource) {}
inline DictationSnapshot dictationSnapshotNow() {
  return DictationSnapshot{DictationState::IDLE, SOURCE_INTERNAL,
                           "none", 0, 0, {}};
}
inline void dictationTick() {}
inline bool dictationTakeText(char*, size_t) { return false; }
inline bool dictationTakeTextFor(CommandSource, char*, size_t) {
  return false;
}
inline void dictationOnCapturePublished(uint64_t, const char*, bool,
                                        const char*) {}
inline void dictationResetForSessionBoundary() {}
inline DictationUartIntrinsicResult dictationHandleUartIntrinsic(
    const char* line, uint32_t, bool, char* replyOut, size_t replyOutSize) {
  if (!dictationUartLineIsProtocol(line)) {
    return DictationUartIntrinsicResult::NotHandled;
  }
  if (replyOut && replyOutSize) {
    snprintf(replyOut, replyOutSize, "%s", "Error: dictation is not built");
  }
  return DictationUartIntrinsicResult::Handled;
}
inline bool dictationIsUartProtocolLine(const char* line) {
  return dictationUartLineIsProtocol(line);
}

#endif  // ENABLE_DICTATION

#endif  // SYSTEM_DICTATION_H
