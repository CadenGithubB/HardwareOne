// System_Dictation.h — provider-neutral keyboard and application speech-to-text.
//
// A shared OLED/G2 keyboard and OLED/G2/web App service with one provider
// latched per exchange. The two consumers have separate authority handles.
// ENABLE_LOCAL_STT builds use the shared local broker with bounded raw-HAL
// continuous capture and ordered text chunks. Other builds retain CM5 and owned VAD
// WAV capture. Both accept PDM or G2 audio through the same HAL. Local failure
// never falls back to exporting audio to a host.
//
// ESP-SR provides fixed commands, not arbitrary dictation. The legacy CM5
// provider performs free-text transcription on Linux with this round trip:
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
// started by an authenticated keyboard/App owner, so it has no admitting UART
// session and that fence would drop it. The push below therefore targets the
// CURRENTLY authenticated UART session instead. That widening is scoped as
// tightly as it can be: it fires only for an owner this module minted, only
// while that exact dictation is still pending, and only when the input-surface
// session that armed it is still live. Web Apps must use a named cookie epoch;
// Basic Auth and other stateless callers cannot arm it.
#ifndef SYSTEM_DICTATION_H
#define SYSTEM_DICTATION_H

#include <Arduino.h>

#include "System_BuildConfig.h"
#include "System_DictationPolicy.h"  // exact direct-UART namespace predicate
#include "System_User.h"   // TransportSessionEpoch
#include "System_Transcript.h"

// Longest transcript accepted from the host. Sized to the keyboard buffer —
// anything past the field's own maxLength is truncated at append time anyway,
// and a bounded copy here keeps an oversize host reply from reaching the UI.
#define DICTATION_MAX_TEXT 256

enum class DictationState : uint8_t {
  IDLE = 0,     // nothing in flight; the mode shows "ready"
  RECORDING,    // capture running; VAD or the wearer will end it
  WAITING,      // provider is transcribing; final text awaits a one-time drain
  FAILED,       // terminal, with a reason; cleared by the next arm
};

struct DictationSnapshot {
  DictationState state;
  CommandSource ownerSource;  // OLED or G2 surface that owns this exchange
  const char* sourceName;   // "PDM", "G2", or "none" — which mic is live
  uint32_t elapsedMs;       // time in the current non-idle state
  int level;                // 0..100 audio level while RECORDING, else 0
  char failure[40];
  bool continuous = false; // Session keeps listening between completed phrases.
  bool captureActive = false;
  bool inferenceActive = false;
  bool preparing = false;    // Do not prompt SPEAK NOW before HAL capture.
  TranscriptStatus transcript; // Generic status; this unscoped snapshot never exposes a path.
};

// Exact delivery receipt: committing an old field, chunk or byte offset is a no-op.
struct DictationTextReceipt {
  uint64_t exchange = 0;
  uint32_t sequence = 0;
  uint16_t offset = 0;
  uint16_t length = 0;
};

// An application lease is distinct from the keyboard consumer, even on the
// same display/session. Treat exchange as an opaque exact-run token. Possessing
// it never replaces the live source+epoch authority checked by every operation.
struct DictationAppLease {
  CommandSource source = SOURCE_INTERNAL;
  TransportSessionEpoch epoch = kNoTransportSessionEpoch;
  uint64_t exchange = 0;
};

struct DictationAppSnapshot {
  bool valid = false;
  bool busy = false;       // Shared service is occupied, including cleanup.
  bool active = false;     // This lease still owns work, delivery or saving.
  bool done = false;       // Terminal and drained; inspect status.state/failure.
  bool textPending = false;
  DictationSnapshot status{}; // Owner-scoped: includes this lease's saved path.
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

// Optional frontend nudge. The existing service worker independently enforces
// capture/host timeouts and session revocation even when no UI is polling.
void dictationTick();

// Compatibility drain of one bounded delivery piece. Returns false when nothing
// is waiting; small buffers leave the remaining bytes for the next call. The keyboard appends what it gets rather than replacing, so the
// wearer can dictate and then fix it with the character grid.
bool dictationTakeText(char* out, size_t outSize);
bool dictationTakeTextFor(CommandSource displaySource,
                          char* out, size_t outSize);

// Shared provider-neutral delivery. Pi v1 publishes one final chunk; local STT
// can publish several. Peek never consumes. Commit only input bytes the field
// actually accepted (including bytes deliberately filtered by its input policy).
// Inference/transport acknowledgment happens only after every piece is accepted.
bool dictationPeekTextFor(CommandSource source, char* out, size_t outSize,
                         DictationTextReceipt* receipt);
bool dictationCommitTextFor(CommandSource source,
                           const DictationTextReceipt& receipt, size_t accepted);
// A finite field cannot consume an unlimited session. Stop/discard the remainder
// with a visible reason instead of ACKing text which was never inserted.
void dictationFieldFullFor(CommandSource source);

// Standalone Apps and the web microphone panel use these exact-lease APIs.
// SOURCE_WEB requires a live cookie epoch; OLED/G2 use their named UI epoch.
// The most recently accepted exact App receipt/count can be ACKed again after
// a lost response; a later commit/admission invalidates that retry cache.
// Begin can initialize the Pi microphone: invoke outside display/I2C rendering.
// Peek/commit have the same retry-safe semantics as the keyboard mailbox. The
// caller keeps a bounded text tail and commits only after accepting that piece.
// Terminal leases remain queryable until the next admitted exchange. A revoked
// epoch or stale lease can never read, stop, cancel or acknowledge a successor.
// The Pi provider returns one final segment; continuous=true identifies local
// repeated-segment capture. Neither provider promises partial-word streaming.
bool dictationAppBegin(CommandSource source, TransportSessionEpoch epoch,
                       DictationAppLease* lease);
// Recover an active or terminal lease only for this live exact owner. Use a
// separate output object: every failure clears it.
bool dictationAppCurrent(CommandSource source, TransportSessionEpoch epoch,
                         DictationAppLease* lease);
// A live caller with an invalid/zero exchange receives only generic busy and
// false; all private fields stay empty. This supports an idle app's Start gate.
bool dictationAppSnapshot(const DictationAppLease& lease,
                          DictationAppSnapshot* out);
bool dictationAppRequestStop(const DictationAppLease& lease);
bool dictationAppCancel(const DictationAppLease& lease);
bool dictationAppPeekText(const DictationAppLease& lease, char* out, size_t outSize,
                          DictationTextReceipt* receipt);
bool dictationAppCommitText(const DictationAppLease& lease,
                            const DictationTextReceipt& receipt, size_t accepted);

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
inline bool dictationPeekTextFor(CommandSource, char*, size_t, DictationTextReceipt*) { return false; }
inline bool dictationCommitTextFor(CommandSource, const DictationTextReceipt&, size_t) { return false; }
inline void dictationFieldFullFor(CommandSource) {}
inline bool dictationAppBegin(CommandSource, TransportSessionEpoch, DictationAppLease* lease) {
  if (lease) *lease = DictationAppLease{};
  return false;
}
inline bool dictationAppCurrent(CommandSource, TransportSessionEpoch, DictationAppLease* lease) {
  if (lease) *lease = DictationAppLease{};
  return false;
}
inline bool dictationAppSnapshot(const DictationAppLease&, DictationAppSnapshot* out) {
  if (out) *out = DictationAppSnapshot{};
  return false;
}
inline bool dictationAppRequestStop(const DictationAppLease&) { return false; }
inline bool dictationAppCancel(const DictationAppLease&) { return false; }
inline bool dictationAppPeekText(const DictationAppLease&, char* out, size_t size,
                                 DictationTextReceipt* receipt) {
  if (out && size) out[0] = '\0';
  if (receipt) *receipt = DictationTextReceipt{};
  return false;
}
inline bool dictationAppCommitText(const DictationAppLease&, const DictationTextReceipt&, size_t) { return false; }
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
