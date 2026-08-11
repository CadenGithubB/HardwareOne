/**
 * Microphone Sensor Module - ESP32-S3 PDM Microphone Support
 * 
 * Supports PDM microphone on XIAO ESP32S3 Sense via I2S interface.
 * Records audio samples for visualization, recording, and ML inference.
 */

#ifndef SYSTEM_MICROPHONE_H
#define SYSTEM_MICROPHONE_H

#include <Arduino.h>
#include "System_BuildConfig.h"

// WAV capture lifecycle. This is deliberately separate from gMicRunning:
// gMicRunning owns the underlying audio source, while every non-IDLE state
// below owns one recording's path/file/task state and rejects a second start.
enum class MicRecordingState : uint8_t {
  IDLE = 0,
  STARTING,
  CAPTURING,
  STOPPING,
  FINALIZING,
};

// Optional correlation token for machine-owned captures. Zero is reserved for
// the legacy/manual recorder API. EvenAI producers encode a nonzero 32-bit boot
// nonce in the high half and a nonzero exchange counter in the low half, then
// repeat the exact value in every status/stop/discard/delete operation.
using MicRecordingOwner = uint64_t;
static constexpr MicRecordingOwner MIC_RECORDING_OWNER_MANUAL = 0;

// Result of an owner-scoped operation. OWNER_MISMATCH is deliberately distinct
// from NOT_FOUND: callers must never fall back to the global/manual last result
// after discovering that another capture owns the recorder.
enum class MicRecordingOwnedOp : uint8_t {
  OK = 0,
  INVALID_OWNER,
  OWNER_MISMATCH,
  NOT_READY,
  NOT_FOUND,
  TIMED_OUT,
  DELETE_FAILED,
  PATH_MISMATCH,
};

struct MicRecordingResult {
  bool valid;
  bool failed;
  bool discarded;
  // Sealed at finalize by the recorder task: the capture was G2-sourced AND
  // the delivered-rate watchdog latched during it. Reply formatting MUST use
  // this sealed bit, never the live latch — the latch is per-stream state
  // that a later capture clears/sets, which would misattribute `degraded=1`
  // across results in both directions.
  bool degraded;
  MicRecordingOwner owner;
  uint32_t samples;
  uint32_t sampleRate;
  char path[64];
  char failure[48];
};

MicRecordingState getMicRecordingState();
const char* micRecordingStateName(MicRecordingState state);
bool micRecordingBusy();
bool micRecordingCapturing();

#if ENABLE_MICROPHONE

// Microphone sensor state
extern bool gMicRunning;
extern bool micConnected;

// Microphone info
extern int micSampleRate;
extern int micBitDepth;
extern int micChannels;
extern int micGain;

// Microphone initialization
bool initMicrophone();
// Stops the source and joins any recorder through FINALIZING. False means the
// bounded join expired and callers must not reconfigure/restart the source.
bool stopMicrophone();

// Capture audio samples (returns raw PCM data)
// Caller must free the buffer with free() when done
int16_t* captureAudioSamples(size_t sampleCount, size_t* outLen);

// Get audio level (0-100 for VU meter display)
int getAudioLevel();

// Audio preprocessing (shared with ESP-SR)
// Applies: DC offset removal, high-pass filter, pre-emphasis, software gain
// Pass gainMultiplier <= 0 to use default from micGain setting
// Pass filtersEnabled = false to skip high-pass and pre-emphasis (for ESP-SR AFE testing)
void applyMicAudioProcessing(int16_t* buf, size_t sampleCount, float gainMultiplier = 0.0f, bool filtersEnabled = true);
void resetMicAudioProcessingState();
float getMicSoftwareGainMultiplier();
int32_t getMicDcOffset();

// Get microphone status JSON
const char* buildMicrophoneStatusJson();

// Recording functions
// silenceStopMs >0 = opt-in silence auto-stop (STT flow). trim additionally drops
// leading room tone (while preserving pre-roll) and most trailing silence from
// the FILE without shortening the detection window — a separate opt-in so
// tools/vad_replay.py can still get a byte-exact WAV.
// Both default off, so every other recorder is unchanged and needs no edit.
bool startRecording(uint32_t silenceStopMs = 0, bool trim = false);
// Requests STOPPING and waits through FINALIZING. False means the bounded wait
// expired; callers must not treat the current path as fetchable in that case.
bool stopRecording(uint32_t timeoutMs = 3000);
// Owner-scoped recorder contract used by one EvenAI exchange. A token is
// single-use within the recorder's bounded completion history. Owned stop is
// idempotent for the matching completed capture and refuses to affect any
// other active owner. `discard=true` is monotonic and orthogonal to the first
// terminal stop cause: the task still finalizes/closes the WAV before removing
// the exact token-derived path and publishes IDLE only afterwards.
bool startRecordingOwned(MicRecordingOwner owner,
                         uint32_t silenceStopMs = 0, bool trim = false);
MicRecordingOwnedOp stopRecordingOwned(MicRecordingOwner owner,
                                       bool discard = false,
                                       uint32_t timeoutMs = 3000);
MicRecordingOwnedOp getRecordingResultOwned(MicRecordingOwner owner,
                                            MicRecordingResult* out);
// Delete/redact only the completed result belonging to `owner`. The caller's
// quoted basename is checked against that result before the recorder-retained
// absolute path is removed; a delayed cleanup therefore cannot cross owners.
MicRecordingOwnedOp deleteRecordingOwned(MicRecordingOwner owner,
                                         const char* expectedFilename);
// Non-blocking source-loss hook for BLE/HAL callbacks. It never touches the
// filesystem; the recorder task owns FINALIZING and publishes IDLE last.
void microphoneNotifySourceLost();
int getRecordingCount();
String getRecordingsList();
bool deleteRecording(const char* filename);

// Command handlers
const char* cmd_mic(const String& argsInput);
const char* cmd_micstart(const String& argsInput);
const char* cmd_micstop(const String& argsInput);
const char* cmd_miclevel(const String& argsInput);
const char* cmd_micrecord(const String& argsInput);
const char* cmd_miclist(const String& argsInput);
const char* cmd_micdelete(const String& argsInput);
const char* cmd_micdeleteid(const String& argsInput);
const char* cmd_micsamplerate(const String& argsInput);
const char* cmd_micgain(const String& argsInput);
const char* cmd_micbitdepth(const String& argsInput);
const char* cmd_micsource(const String& argsInput);   // get/set source pref {auto,pdm,g2}

// Command registry
struct CommandEntry;
extern const CommandEntry micCommands[];
extern const size_t micCommandsCount;

#endif // ENABLE_MICROPHONE

#endif // SYSTEM_MICROPHONE_H
