# Local P4 STT results — 2026-09-28

The connected ESP32-P4 now runs buffered English speech-to-text inside the full
HardwareOne application. Its onboard PDM microphone records through `HAL_Audio`;
the P4 loads and runs the complete int8 QuartzNet5x5 model, then returns text to
the originating authenticated session. The final app remains installed. No Pi,
UART inference, Wi-Fi association or cloud API was used. This is an experimental
dictation milestone, not continuous live captions or an accuracy qualification.

## Installed build and storage

- IDF 5.5.5, ESP-DL 3.3.12, ESP-SR 2.5.5; P4 rev 3.2 at 400 MHz, 32 MiB PSRAM.
- App: 6,372,000 bytes; SHA256
  `75c6e184932a2df4cbda16f0848fd7e65f40d65fb36aa6c4646598b051bc21e5`.
- Existing app partition: 6,377,472 bytes. Only **5,472 bytes remain**; further
  firmware growth needs a deliberate size/layout decision.
- Model package: 5,490,696 bytes in `/STT Models/quartznet5x5.p4.stt`; expands to
  7,172,016 bytes in PSRAM. Its exact provenance and hashes are in
  `model-provenance.json` and `validation.json`.
- Fresh filesystem backup, additive model staging and USB write verification
  preserved every existing file at installation, including the ESP-SR bundle.
  The partition layout, C6 firmware and S3 firmware were not changed.
- The final source snapshot checks all 7,954 build inputs. Final app v3 is
  byte-identical to the v2 app used for the earlier full-app microphone tests.

## Physical microphone tests in the full app

Mac speaker playback of held-out synthetic sentences was captured acoustically
by the P4 microphone. The Mac only supplied test sound; all recognition ran on
the board. Five speech captures completed, including early manual stop and a
run with the CSI camera enabled. Transcripts contained errors.

| Capture | Model load | Frontend | Inference | Processing total* |
| --- | ---: | ---: | ---: | ---: |
| 8 s, Samantha | 3.958 s | 0.992 s | 2.650 s | 7.602 s |
| 8 s, Daniel | 3.696 s | 0.880 s | 2.866 s | 7.442 s |
| 8 s, Karen, after ESP-SR shutdown | 3.497 s | 0.930 s | 2.736 s | 7.164 s |
| 7 s, camera enabled | 3.719 s | 0.724 s | 2.468 s | 6.912 s |
| 5.704 s, manually stopped | 3.933 s | 0.535 s | 1.957 s | 6.425 s |
| 20 s maximum-duration capture | 3.947 s | 2.186 s | 6.668 s | 12.803 s |

*Sum of measured engine phases, including decode; excludes recording and
command/polling overhead. The maximum-duration case checked capacity and
completion in the room, not recognition accuracy. The final 20-second run took
32.990 seconds from session start to completion. Loading includes decompression
and model construction; weights are released after each utterance.

Also passed on hardware: cancellation during capture, cancellation during an
actual inference phase, a two-second silence capture returning empty text,
logout/result revocation across re-login, ESP-SR startup with its existing
WakeNet9/MultiNet7 bundle, STT rejection while ESP-SR owns audio, and STT after
ESP-SR shutdown. Ordinary microphone ownership rejection was checked in the
initial integrated image. No audio gain/source/rate preference was changed.

After the final edge tests, the app reported 32,842,568 bytes of free PSRAM,
247,107 bytes of free internal heap, and zero tracked allocation failures.
Every STT worker had stopped. Final reboot and all **20 saved-settings checks**
passed: mesh identity/preferences, BLE preferences, power, HTTP/Wi-Fi state,
camera preferences, microphone source/rate/gain, speech preferences and battery
voltage availability. Camera, microphone and ESP-SR were left stopped, with
voice commands disarmed. The existing S3 speech setup was left alone.

## Full-model numeric and lifecycle checks

A separate minimal P4 probe compiled the canonical production runtime with the
same size optimization and exact operator/kernel whitelist. Two public speech
recordings plus a repeated recording passed; quantized input **and output**
tensor SHA256 values matched the host byte-for-byte. Free PSRAM returned to
33,551,716 bytes after every probe run.

| Recording | Load | Frontend | Inference | Activation arena in PSRAM |
| --- | ---: | ---: | ---: | ---: |
| 6.625 s | 2.623 s | 0.491 s | 1.379 s | 679,936 B |
| 16.715 s | 2.640 s | 1.287 s | 2.901 s | 1,712,128 B |
| 6.625 s repeat | 2.627 s | 0.491 s | 1.369 s | 679,936 B |

These timings exclude the full application's other work; use the microphone
table when assessing the current user experience. ESP-SR's ESP-DL pruning is
handled by registering the generated STT requirements alongside its own:
Add/Conv/Relu, the runtime RequantizeLinear dependency and four P4 kernels.
Both standalone STT and the ESP-SR/STT union were checked against the pinned
component's official finalizer.

Host tests cover frontend/CTC parity, real envelope corruption/truncation,
model identity and load ordering, bounded allocation, session/run ownership,
cancel/cleanup races, microphone source fallback rejection, direct HTTP/BLE
result delivery and redaction from shared output, local dictation ownership
and UI status transitions. Focused C++ harnesses passed ASan/UBSan, including
the existing HTTP single/batch handler suite (1,770 assertions). Numerical
test details are recorded in `frontend/README.md`.

## Limits and next work

The current model makes noticeable word errors. Four synthesized sentences
excluded from calibration had eight word errors in fifty reference words on
both float and int8 host inference. Room playback introduced more errors.
This small test set does not establish human-speech accuracy. A representative
recording set, audio-level investigation, broader quantization calibration and
decoder/model evaluation are the next accuracy work.

Dictation supports 1–20-second recordings, lowercase English and apostrophes.
There is no punctuation restoration or word-by-word streaming. Saved `micgain`
is deliberately not applied: the model receives raw HAL PCM and applies its
own pinned frontend once. The existing OLED/G2 keyboard integration is wired
and host-tested, but no display/glasses were attached for physical validation.
The G2 microphone also remains to be tested with this local model. Keyboard
delivery retains the existing bounded 256-byte dictation result and the field's
own length limit; the CLI broker supports up to 512 bytes.

The backend is optional (`ENABLE_LOCAL_STT`, enabled in this P4 profile).
Other profiles retain the Pi provider; this work does not establish that the
S3 can run this local model. The portable capture, broker, frontend and result
delivery stay shared, with P4 inference behind `System_STTLocal`.

`validation.json` contains structured measurements and hashes of ignored
private evidence files. Backups, credentials, recordings and model binaries
are not committed. See `README.md` for the command workflow.
