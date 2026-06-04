/**
 * HAL_Audio.h — Audio capture Hardware Abstraction Layer
 *
 * Single owner of microphone PCM capture. Abstracts the physical sources —
 * the onboard PDM mic (I2S) and (from Phase 2) the G2 glasses left-temple mic
 * (BLE LC3 → PCM ring) — behind ONE pull API, the same way HAL_Input abstracts
 * the gamepad and ANO encoder behind a single inputTask()/gInputCache.
 *
 * Every source delivers the same canonical format: 16 kHz / mono / int16 PCM,
 * so consumers (the WAV recorder, the ESP-SR AFE feed, VU metering) never
 * resample and never care which source is live.
 *
 * Concurrency model A (exclusive): exactly one consumer owns capture at a
 * time. audioCaptureStart() fails if a *different* owner already holds it —
 * recording and speech-recognition are mutually exclusive, as they already
 * are today (they share I2S_NUM_0).
 */
#ifndef HAL_AUDIO_H
#define HAL_AUDIO_H

#include <Arduino.h>
#include "System_BuildConfig.h"

#if ENABLE_MICROPHONE_SENSOR

// Canonical capture format — every source produces this.
static const uint32_t AUDIO_HAL_SAMPLE_RATE = 16000;
static const uint8_t  AUDIO_HAL_BITS        = 16;
static const uint8_t  AUDIO_HAL_CHANNELS    = 1;

enum AudioSource : uint8_t {
  AUDIO_SRC_LOCAL_PDM = 0,   // onboard PDM mic via I2S (Phase 1)
  AUDIO_SRC_G2_LEFT   = 1,   // G2 glasses left-temple mic, BLE LC3 ring (Phase 2)
};

// Current source. A change only takes effect at the next audioCaptureStart();
// audioSetSource() refuses while capture is active to avoid a half-switched
// stream. Returns false if a switch was refused.
AudioSource audioGetSource();
bool        audioSetSource(AudioSource src);

// Exclusive capture lifecycle. `owner` is a short diagnostic tag
// ("mic.record", "sr"). audioCaptureStart returns false if capture is already
// held by a *different* owner, or if the source's hardware/stream fails to
// start. Re-starting with the same owner is an idempotent success.
// `sampleRate` applies to the PDM source (pass 0 for the 16 kHz default); the
// G2 source is always 16 kHz.
bool        audioCaptureStart(const char* owner, uint32_t sampleRate = 0);
void        audioCaptureStop(const char* owner);
bool        audioCaptureActive();
const char* audioCaptureOwner();   // "" when idle

// Pull PCM from the active source into `out` (int16 mono). Returns the number
// of samples read; 0 on timeout, stall, or when not capturing. Blocks up to
// timeoutMs. Safe to call with a short read — consumers already tolerate it.
size_t      audioReadPcm(int16_t* out, size_t maxSamples, uint32_t timeoutMs);

#endif // ENABLE_MICROPHONE_SENSOR
#endif // HAL_AUDIO_H
