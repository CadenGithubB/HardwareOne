#pragma once
// Raw STT input recorder: a bench/diagnostic tap that saves exactly the PCM
// the continuous STT segmenter receives (raw HAL samples plus any pause
// silence the broker feeds) to the SD card, so speech-detector and noise-floor
// experiments can be replayed on a host with identical audio.
//
// Ownership: this module owns the arm flag, the PSRAM ring, the output files
// and the drain task. The STT capture task (System_STT) calls SessionBegin,
// Push and SessionEnd; nothing else touches the ring. Arming is never
// persisted: it lasts until `sttraw off` or a reboot. Files land in
// /sd/STT Raw as raw_<id>.pcm (s16le mono) with a raw_<id>.json sidecar.
//
// A failure here never fails or delays STT: SessionBegin reports a reason in
// status and the session runs without recording; a full ring drops whole
// blocks and records them as gaps in the sidecar.
#include <cstddef>
#include <cstdint>

// Command-side operations (any task). Arm/disarm apply to the next continuous
// session; disarming also stops an active recording at its next drain.
// Return false with a reason when the request cannot be accepted.
bool sttRawCaptureArm(bool on, char* reason, size_t reasonCap);
// One-line human-readable status ("OK: ..." form) into out.
const char* sttRawCaptureStatus(char* out, size_t cap);

// Capture-task operations (the STT capture task only).
void sttRawCaptureSessionBegin(uint8_t audioSource, uint32_t sampleRate);
void sttRawCapturePush(const int16_t* pcm, size_t samples);  // real-time: copy only
void sttRawCaptureSessionEnd();
