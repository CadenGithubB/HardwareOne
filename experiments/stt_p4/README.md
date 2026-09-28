# Fully local P4 speech-to-text

Experimental buffered English dictation for HardwareOne, on
`codex/jpeg-portable`, based on the qualified ESP-SR milestone `f9bfe98`.
The onboard microphone now produces text in the full P4 application without a
Pi, UART inference, Wi-Fi connection or cloud API. The first hardware-tested
version is installed; see [results](RESULTS.md), [validation data](validation.json)
and [status](STATUS.md) for evidence and remaining limits.

## Shared application path

`HAL_Audio` supplies raw 16 kHz mono PCM to `System_STT`. The broker owns a
bounded recording, its authenticated session and cancellation. The local engine
loads a pinned QuartzNet5x5 model, computes log-mel features, executes P4 int8
ESP-DL kernels and greedily decodes CTC text. Model memory is released after each
utterance. The PDM and G2 sources use the same capture interface.

Existing OLED/G2 text-entry dictation selects this provider when
`ENABLE_LOCAL_STT=1`; otherwise the existing Pi provider remains selected.
Local failure never exports audio to another provider. No S3 firmware was
flashed or S3 speech configuration changed for this work. The portable frontend
and CTC decoder are ordinary C++; the P4 inference runtime is behind the shared
`System_STTLocal` interface. Production `components/hardwareone/stt/` is the
single implementation; experiment tests compile it directly.

## Use the installed P4 profile

Log into the usual serial console, then stop existing microphone consumers:

```text
closesr
closemic
stt record 8
```

The reply contains a 16-character run ID. Speak after its status says recording.
Use that returned ID in these commands:

```text
stt status <id>
stt stop <id>
stt result <id>
stt cancel <id>
```

`record` accepts 1–20 seconds (default 10). `stop` finishes early and transcribes;
`cancel` discards the run. Poll status until the worker has stopped. `result`
returns final `sttText` only to the originating live session. Logout revokes the
run/result, and completed results expire after five minutes. Speech is text
input only; it is never executed as a device command. Raw audio is not written
to storage, and shared console/web/file/BLE-debug mirrors redact transcripts.

The existing dictation input mode on the OLED/G2 keyboards uses a 20-second
maximum and manual finish. These physical UI paths still need an accessory test;
the device in this session has no display attached. Their provider, session and
result-delivery paths have host regression tests.

## Current limits

- English, lowercase text with apostrophes; no punctuation restoration. CLI
  results hold up to 512 bytes. OLED/G2 entry retains its 256-byte limit, then
  applies the receiving field's own length limit.
- Record a sentence, then wait for its result. This is not word-by-word live
  captions. Whole-utterance normalization and model context make a naive sliding
  window a poor substitute for a streaming model.
- ESP-SR and the ordinary microphone/recorder must be stopped first. STT does
  not silently suspend or re-arm command recognition.
- Small-model accuracy is imperfect. Four synthesized held-out sentences had
  8 word errors in 50 reference words on both float and int8 inference. Room
  speaker-to-microphone tests made additional errors. These tiny checks are not
  a representative WER benchmark or evidence of human-speech accuracy.
- The 16 MB board flash is tight. The model is stored compressed (5,490,696
  bytes including its envelope), expands to 7,172,016 bytes in PSRAM, and shares
  LittleFS with the existing ESP-SR model. No partition layout was changed.
  The current app has only 5,472 bytes of partition headroom.
- Full-app microphone tests took about 6–8 seconds of processing after short
  recordings, including model loading. The 20-second capture took about 13
  seconds afterward. Timing depends on other running device work; see
  [measured results](RESULTS.md) rather than kernel-only speed.
- STT uses raw HAL audio with its own frontend. The saved software `micgain`
  setting is preserved and does not alter this capture path.

## Reproduce

Use the pinned IDF 5.5.5 environment and the preserved qualification snapshots.
`prepare.py --capture` records production changes against `f9bfe98`;
`prepare.py` creates an isolated `private/app-p4`; `--refresh` refreshes only
that checked copy. `build.sh` verifies sources, inherited radio/JPEG overlays,
feature profile and dependency versions before building. It never flashes.
The ESP-DL 3.3.12 manifest explicitly registers the model's operations and four
P4 kernels, plus the runtime's tensor-conversion dependency, so ESP-SR pruning
cannot remove them.

`setup_export_env.py`, `export_quartznet.py`, `pack_model.py` and
`generate_compile_requirements.py` document the model build. See their `--help`,
`model-provenance.json`, `compile-requirements-provenance.json`,
`heldout-provenance.json`, and `frontend/README.md`. The model/frontend identities
are checked before the vendor model parser sees any data. Generated weights,
fixtures, environments and device logs are ignored under `private/`.

The standalone `probe/` mounts existing LittleFS read-only, never formats it,
and processes embedded public test recordings with the canonical production
runtime. Its partition CSV is supplied explicitly and compared before any
app-only flash. `stage_model.py` stages an additive model install against a
fresh filesystem image and verifies every existing file is unchanged; the USB
coordinator and full board backups are private artifacts, not automatic setup.
