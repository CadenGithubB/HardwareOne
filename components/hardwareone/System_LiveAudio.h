// System_LiveAudio.h — opt-in live-pcm-v1 UART transport.
#ifndef SYSTEM_LIVEAUDIO_H
#define SYSTEM_LIVEAUDIO_H

#include <Arduino.h>
#include "System_BuildConfig.h"

struct CommandEntry;

// UART control-plane fast path for the high-frequency `liveaudio ready`
// renewal.  A healthy, already-created lease can be extended without
// allocating, entering cmd_exec, or marking the CM5 application busy.
// Initial acquisition, expiry recovery, and every mismatched request return
// NotHandled so the ordinary registry command retains full lifecycle and
// authorization semantics.
enum class LiveAudioReadyIntrinsicResult : uint8_t {
  NotHandled,
  Handled,
};
#if ENABLE_UART_HOST_LINK
LiveAudioReadyIntrinsicResult liveAudioHandleReadyIntrinsic(
    const char* line, uint32_t namedSessionEpoch,
    bool namedSessionMayControl, char* reply, size_t replySize);

// Read-only machine inspection that must not extend the CM5 command-busy
// bridge or displace useful human commands from the web feed. Healthy ready
// renewals return from the intrinsic above before this classifier is reached;
// initial/repair ready plus shadow/release/synth/abort remain ordinary work.
bool liveAudioIsHousekeepingCommand(const char* line);

// True from synth admission through END/ABORT finalization. Bulk voicefetch is
// rejected during this window so two high-volume producers cannot contend for
// the UART TX ring.
bool liveAudioStreamActive();

// Atomic bulk/live exclusion for voicefetch. A successful claim prevents any
// new synth or recorder shadow BEGIN until the matching release; it fails if a
// live stream already owns the framed lane. Callers must release on every exit.
bool liveAudioTryBeginBulkTransfer();
void liveAudioEndBulkTransfer();
#endif  // ENABLE_UART_HOST_LINK

// Recorder shadow is deliberately best-effort: these entry points never own
// the recorder lifecycle or its WAV. They only mirror an exact, non-manual
// capture when a UART controller has explicitly armed shadow mode.
enum class LiveAudioRecorderSource : uint8_t {
  PDM = 1,
  G2 = 2,
};

enum class LiveAudioRecorderOutcome : uint8_t {
  SAVED,
  DISCARDED,
  FAILED,
};

struct LiveAudioRecorderAuthorization {
  bool valid = false;
  bool native = false;
  uint64_t controller = 0;
  uint64_t exchange = 0;
  uint32_t sessionEpoch = 0;
};

// ---------------------------------------------------------------------------
// Everything below needs the module itself. The TYPES above stay unconditional:
// System_Microphone.cpp holds LiveAudioRecorderAuthorization BY VALUE and takes
// the enums by reference in eight places, so they must exist on every build.
// ---------------------------------------------------------------------------
#if ENABLE_UART_HOST_LINK

bool liveAudioRecorderBegin(uint64_t exchangeId,
                            LiveAudioRecorderSource source,
                            uint32_t sampleRate,
                            const LiveAudioRecorderAuthorization& auth);
bool liveAudioRecorderOffer(uint64_t exchangeId, const int16_t* samples,
                            size_t sampleCount);
void liveAudioRecorderFinish(uint64_t exchangeId,
                             LiveAudioRecorderOutcome outcome);
// Nonblocking exact-ID fence for wearer/host cancellation.
void liveAudioRecorderAbort(uint64_t exchangeId);

// Provenance is consumed once, before the asynchronous recorder task starts.
// Native mode is first bound to an exact G2 exchange/login epoch; the command
// admission then latches the one-shot eligibility bit into the recorder FSM.
bool liveAudioRecorderArmNative(uint64_t exchangeId, uint32_t sessionEpoch);
bool liveAudioRecorderCaptureEligible(
    uint64_t exchangeId, LiveAudioRecorderAuthorization* outAuth);

// Registry command and module table. The command remains registered on boards
// without a UART host link and fails cleanly there.
#else  // UART host link compiled out — inert stubs
//
// Every return value is behaviourally exact, not merely safe: with the module
// gone the `liveaudio` command is unregistered, so NotHandled / false are what
// the live code would have returned anyway. liveAudioTryBeginBulkTransfer()
// returning true cannot lose mutual exclusion — its only caller is inside
// System_UartLink.cpp's port arm, which rides this same flag.

inline LiveAudioReadyIntrinsicResult liveAudioHandleReadyIntrinsic(
    const char*, uint32_t, bool, char*, size_t) {
  return LiveAudioReadyIntrinsicResult::NotHandled;
}
inline bool liveAudioIsHousekeepingCommand(const char*) { return false; }
inline bool liveAudioStreamActive() { return false; }
inline bool liveAudioTryBeginBulkTransfer() { return true; }
inline void liveAudioEndBulkTransfer() {}
inline bool liveAudioRecorderBegin(uint64_t, LiveAudioRecorderSource, uint32_t,
                                   const LiveAudioRecorderAuthorization&) { return false; }
inline bool liveAudioRecorderOffer(uint64_t, const int16_t*, size_t) { return false; }
inline void liveAudioRecorderFinish(uint64_t, LiveAudioRecorderOutcome) {}
inline void liveAudioRecorderAbort(uint64_t) {}
inline bool liveAudioRecorderArmNative(uint64_t, uint32_t) { return false; }
inline bool liveAudioRecorderCaptureEligible(
    uint64_t, LiveAudioRecorderAuthorization* outAuth) {
  if (outAuth) *outAuth = LiveAudioRecorderAuthorization{};
  return false;
}

#endif  // ENABLE_UART_HOST_LINK

// Stays unconditional: the command remains REGISTERED on builds without the
// link and fails cleanly there (definition in System_SensorStubs.cpp when the
// module is not built).
const char* cmd_liveaudio(const String& argsInput);
extern const CommandEntry liveAudioCommands[];
extern const size_t liveAudioCommandsCount;

#endif  // SYSTEM_LIVEAUDIO_H
