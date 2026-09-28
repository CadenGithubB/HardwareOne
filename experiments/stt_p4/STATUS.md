# Status

## 2026-09-28 — session-cached P4 STT qualified

- Installed and verified cache-app-v1 on the P4: SHA-256
  `56c0dba75ed40c32959085f2134b4c5f8c3283c1660cc1e22f1cc325ed26d945`,
  6,367,776 bytes; 9,696 bytes of unchanged app-partition headroom.
- Verified weights retained only for one continuous session; per-segment hash
  checks, fresh model/arena, explicit release before FreeRTOS task deletion.
  One-shot, shared HAL/Dictation/Pi UART and recognition settings unchanged.
- Full-app warm load median 173 ms versus 3,379 ms cold without camera.
  Same-input hardware probe: 4.497 s cold to 2.003 s warm, byte-exact tensors;
  exact PSRAM recovery after explicit reset and destructor.
- Final 301.6 s run: 29 acknowledged chunks, five camera captures,
  zero reported overruns; cancel/logout/queue-full/one-shot/SR checks passed.
  Initial serial JSON interleaving and read-only retry mitigation documented.
- Cache/broker host sanitizers, concurrent TSan, full P4 build and all 7,956
  source checks passed. All 20 saved-settings checks passed after reboot.
  Mic, camera and SR off; voice disarmed; serial coordinators closed.
- Accuracy remains limited; beam/gain/padding diagnostics gave no consistent
  improvement. Eight-second minimum segment span is unchanged.
- No new exclusions, partitions, model installs, S3/C6 or primary-checkout edits.
  Private evidence ignored; local branch only, no push or PR.
- See CACHE_RESULTS.md, cache-validation.json and DECODER_LATENCY_NOTES.md.

## 2026-09-28 — continuous P4 STT milestone complete

- Installed continuous-v4 on the connected P4; app-only flash verified. App SHA
  `08a961f324a7f54ce31f6d3b96e42d87fd670e8986d2ff5c501744befce637e7`,
  6,367,280 bytes, 10,192 bytes of existing app-partition headroom.
- Physical 600.9 s run delivered 48 acknowledged chunks with
  concurrent capture/inference and 10 successful camera captures; stop drained
  both queues. Zero reported HAL overruns; no claim of exhaustive loss detection.
- Quiet, two-minute speech, retry/ACK, inference cancellation, logout revocation,
  explicit receiver backpressure, bounded-mode and ESP-SR checks passed. Initial
  serial-output corruption and the final polling mitigation are documented.
- ASan/UBSan actual-source tests, concurrent ThreadSanitizer, full P4 build and
  all 7,956 source checks passed. No compiler optimization changes retained.
- Optional R1 health web page/APIs omitted only in this experiment profile;
  ring, core health, Bluetooth and other health interfaces remain enabled.
- Shared HAL, adaptive VAD decisions and Dictation UI/mailbox retained. Pi UART v1
  unchanged; continuous Pi adapter and physical OLED/G2 tests remain future work.
- All 20 saved-settings postchecks passed after final reboot. Mic, camera and SR
  are stopped, voice commands disarmed. All serial coordinators are closed.
- Primary checkout and S3/C6 firmware untouched; no model/partition changes.
  Private logs/models/audio/credentials/backups remain ignored. No push or PR.
- See CONTINUOUS_RESULTS.md and continuous-validation.json for final evidence,
  README.md for use, and CONTINUOUS_PI.md for the Pi adaptation.

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
