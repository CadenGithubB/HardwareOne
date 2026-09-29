# Status

## 2026-09-28 — shared Transcription interfaces installed on P4

- Added **Apps → Transcription** for G2 and OLED, plus collapsible
  **Sensors → Microphone → Transcription** on the web. Each controls the existing
  STT service, displays bounded recent text, browses saved transcripts and uses
  the saved next-session policy. Exact App leases keep keyboards and other
  authenticated sessions isolated; no recognition engine/provider fork.
- P4 app-only image `transcription-ui-v1` verified and booted, SHA-256
  `20a22d79b0ed9656aefa13ca6ff9108739a2db7e3ab2053adc38108910090f4d`.
  6,362,416 bytes, **15,056 bytes free**; 7,972 source checks passed.
  Models, partition layout, C6 and S3 firmware and primary checkout unchanged.
- P4 USB capability and private saved-file commands passed; the existing
  183-byte transcript matches its prior hash. Other-account reads are denied;
  missing SD is explicit. All 20 existing P4 settings/peripheral postchecks
  passed. Saving remains on; mic/camera/SR stopped, voice disarmed.
- Actual-source host sanitizers passed for both providers, keyboard regressions,
  App leases, private file windows, all three interfaces and passive polling.
  All 45 web tests passed. Physical OLED/G2/SD/Pi tests remain unqualified.
- One existing ESP-NOW JavaScript block is now a byte-exact gzip asset, saving
  64,947 asset bytes. Deterministic source consistency check runs on builds.
  No additional standard feature or test-diagnostic removal was needed.
- **Preserve the S3 for the user's concurrent OpenClaw tests.** Its saved Wi-Fi
  connection is active and HTTP reports running on port 80 at 192.168.21.24.
  This Mac's direct HTTP request timed out; the special S3 HTTP fixture insists
  on its isolated test AP, so the P4 network test was deferred rather than
  moving S3 off the user's network. Do not apply the old S3 Wi-Fi-off/channel-6
  cleanup sequence or reboot it for this task.
- Initial standard serial opening caused an S3 `USB_UART_CHIP_RESET`. Its saved
  Wi-Fi/web service was restored with `openwifi` then `openhttp`; no credentials
  or firmware were changed. Later USB access used a Serial subclass that does
  not toggle DTR/RTS and clears HUPCL; no further reset was observed. During
  `openwifi`, wait for its command result before sending anything else: the
  console helper's automatic `whoami` barrier otherwise cancels connection.
- No serial coordinators remain open. Work stays local; no push/PR. Details:
  TRANSCRIPTION_UI.md, TRANSCRIPTION_UI_RESULTS.md, transcription-ui-validation.json.

## 2026-09-28 — shared transcript saving qualified on P4

- Persistent `sttsavetranscripts` option, off by default and latched at admission.
  Shared account-bound writer serves local STT and Pi Dictation; saves accepted
  chunks independently of UI reads/ACKs, with separate storage errors.
- Installed app-only `transcripts-app-v1`: SHA-256
  `7c8e2a54a75ad0daf80d2169ad99c0a7a0d3c40e569040241b667acc48debc35`,
  6,375,872 bytes, 1,600 bytes free in the existing app partition.
- Seven-chunk enabled and five-chunk disabled continuous runs verified both
  mid-session toggle directions. Silence created no file; bounded mode saved
  once. Three camera captures, zero reported overruns; file hashes and setting
  survived reboot. All 20 existing configuration postchecks passed.
- Per-account file guards, private retrieval and accountless mesh RPC rejection
  tested with actual-source host sanitizers; local/Pi lifecycle and TSan passed.
  Physical Pi/SD/OLED/G2 qualification remains outstanding.
- Only inherited `camerajpegprobe` diagnostic removed to fit; no further standard
  feature exclusions, model/partition changes, S3/C6 or primary-checkout edits.
- P4 saving left enabled; mic/camera/SR off, voice disarmed, USB coordinators
  closed. Private evidence ignored; local branch only, no push or PR.
- See TRANSCRIPTS.md, TRANSCRIPT_RESULTS.md and transcript-validation.json.

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
