/**
 * HAL_Audio.h — Audio capture Hardware Abstraction Layer
 *
 * Single owner of microphone PCM capture. Abstracts the physical sources —
 * the onboard PDM mic (I2S) and the G2 glasses left-temple mic (BLE LC3 → PCM
 * ring) — behind ONE pull API, the same way HAL_Input abstracts the gamepad and
 * ANO encoder behind a single inputTask()/gInputCache. Unlike HAL_Input (which
 * is poll-and-cache: many cheap readers), audio is an exclusive single-drainer
 * STREAM — one owner captures at a time and destructively pulls samples; a UI
 * that wants a live level while another consumer owns the stream must read the
 * owner's published derived scalar, NOT open a second capture.
 *
 * Every source delivers the same canonical format: 16 kHz / mono / int16 PCM,
 * so consumers (the WAV recorder, the ESP-SR AFE feed, VU metering) never
 * resample and never care which source is live.
 *
 * Availability is RUNTIME, not a compile-time default: PDM is available iff the
 * board physically has it (ENABLE_MICROPHONE_SENSOR — I2S/PDM is unprobeable, so
 * compiled == present); G2 is available iff the LEFT temple is connected and
 * subscribed. There is NO implicit default source — gAudioSource starts NONE and
 * is resolved against availability at each capture start.
 *
 * Concurrency model A (exclusive): exactly one consumer owns capture at a time.
 * audioCaptureStart() fails if a *different* owner already holds it — recording
 * and speech-recognition are mutually exclusive, as they already are today (they
 * share the single capture stream).
 */
#ifndef HAL_AUDIO_H
#define HAL_AUDIO_H

#include <Arduino.h>
#include "System_BuildConfig.h"

// Gate on the mic SUBSYSTEM (PDM silicon OR a G2 that can supply audio), NOT on
// ENABLE_MICROPHONE_SENSOR — the G2 path needs no PDM and must survive on
// PDM-less boards (e.g. FeatherS3). PDM-specific internals stay inner-gated on
// ENABLE_MICROPHONE_SENSOR inside the .cpp.
#if ENABLE_MICROPHONE

// Canonical capture format — every source produces this.
static const uint32_t AUDIO_HAL_SAMPLE_RATE = 16000;
static const uint8_t  AUDIO_HAL_BITS        = 16;
static const uint8_t  AUDIO_HAL_CHANNELS    = 1;

enum AudioSource : uint8_t {
  AUDIO_SRC_NONE      = 0,   // no source resolved yet — the default; never captures
  AUDIO_SRC_LOCAL_PDM = 1,   // onboard PDM mic via I2S
  AUDIO_SRC_G2_LEFT   = 2,   // G2 glasses LEFT-temple mic, BLE LC3 ring (RIGHT never a source)
};

// Runtime availability. PDM: compiled-present (ENABLE_MICROPHONE_SENSOR).
// G2_LEFT: g2LeftConnected(). NONE: never available.
bool   audioSourceAvailable(AudioSource src);
bool   audioAnySourceAvailable();
// Enumerate the currently-AVAILABLE sources (PDM-first) into `out`; returns the
// count written (<= cap). The mic-page source selector uses this so it only ever
// lists sources that are actually connected right now.
size_t audioListAvailableSources(AudioSource* out, size_t cap);

// Current source. A change only takes effect at the next audioCaptureStart();
// audioSetSource() refuses while capture is active (to avoid a half-switched
// stream) AND refuses an unavailable source. Returns false if refused — callers
// that want a live switch must stop → audioSetSource → start.
AudioSource audioGetSource();
bool        audioSetSource(AudioSource src);

// Exclusive capture lifecycle. `owner` is a short diagnostic tag
// ("mic", "sr"). audioCaptureStart returns false if capture is already
// held by a *different* owner, if no source is available, or if the source's
// hardware/stream fails to start. Re-starting with the same owner is an
// idempotent success. If the current source is NONE or has become unavailable,
// start resolves to an available source (PDM-first) and latches it; it fails if
// none is available. `sampleRate` applies to the PDM source (pass 0 for the
// 16 kHz default); the G2 source is always 16 kHz.
bool        audioCaptureStart(const char* owner, uint32_t sampleRate = 0);
void        audioCaptureStop(const char* owner);
bool        audioCaptureActive();   // STARTING or ACTIVE (false once stop wins)
bool        audioCaptureBusy();     // STARTING, ACTIVE, or STOPPING
bool        audioCaptureOwnedBy(const char* owner); // owner holds any busy phase
const char* audioCaptureOwner();   // "" when idle

// Pull PCM from the active source into `out` (int16 mono). Returns the number
// of samples read; 0 on timeout, stall, or when not capturing. Blocks up to
// timeoutMs. Safe to call with a short read — consumers already tolerate it.
size_t      audioReadPcm(int16_t* out, size_t maxSamples, uint32_t timeoutMs);

// Establish a fresh consumer boundary without restarting the physical source.
// Only the exact active owner may trim. PDM currently has no software ring and
// returns 0; G2 discards decoded samples older than `keepNewestSamples`.
// The HAL state mutex is never held while the source-specific ring is locked.
size_t      audioTrimBufferedPcm(const char* owner, size_t keepNewestSamples);

#endif // ENABLE_MICROPHONE
#endif // HAL_AUDIO_H
