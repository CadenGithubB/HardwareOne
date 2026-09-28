# Status

## 2026-09-28 — local P4 STT first milestone complete

- Branch: `codex/jpeg-portable`; starting point: qualified ESP-SR `f9bfe98`.
- Full HardwareOne app and model are installed on the connected P4. Onboard
  microphone transcription, early stop, maximum 20-second recording, silence,
  capture/inference cancellation and session revocation passed physical tests.
- Final app SHA256:
  `75c6e184932a2df4cbda16f0848fd7e65f40d65fb36aa6c4646598b051bc21e5`.
  Private artifact: `private/images/app-v3/hardwareone-idf.bin`.
- Final exact-pruned model probe passed two public recordings plus repeat,
  with byte-identical host input/output tensors and full PSRAM recovery.
- All 20 saved-settings postchecks passed after the final app reboot. Camera,
  mic and ESP-SR are stopped; voice commands disarmed; preferences retained.
- `README.md` documents use/reproduction; `RESULTS.md` and `validation.json`
  record the evidence, timings and limits. Model and frontend identities are
  pinned; all 7,954 build-source checks passed.
- This is experimental buffered English dictation, not streaming captions.
  Accuracy needs improvement; short captures process in about 6–8 seconds and
  the 20-second capacity check took about 13 seconds after capture.
- Next validation: human speech and physical G2/OLED dictation, then audio/
  model accuracy and latency work. Only 5,472 app-partition bytes remain.
- No S3 flash or speech changes. Primary project checkout remains untouched.
- Private serial coordinators and backups remain under `private/`; none are
  committed. Existing speech V8 image remains the known fallback, not installed.
