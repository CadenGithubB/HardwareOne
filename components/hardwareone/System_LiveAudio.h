// System_LiveAudio.h — opt-in live-pcm-v1 UART transport.
#ifndef SYSTEM_LIVEAUDIO_H
#define SYSTEM_LIVEAUDIO_H

#include <Arduino.h>

struct CommandEntry;

// True from synth admission through END/ABORT finalization. Bulk voicefetch is
// rejected during this window so two high-volume producers cannot contend for
// the UART TX ring.
bool liveAudioStreamActive();

// Atomic bulk/live exclusion for voicefetch. A successful claim prevents any
// new synth or recorder shadow BEGIN until the matching release; it fails if a
// live stream already owns the framed lane. Callers must release on every exit.
bool liveAudioTryBeginBulkTransfer();
void liveAudioEndBulkTransfer();

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
const char* cmd_liveaudio(const String& argsInput);
extern const CommandEntry liveAudioCommands[];
extern const size_t liveAudioCommandsCount;

#endif  // SYSTEM_LIVEAUDIO_H
