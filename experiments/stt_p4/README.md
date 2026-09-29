# Fully local P4 speech-to-text

Experimental English dictation for HardwareOne, on `codex/jpeg-portable`, based
on the qualified ESP-SR milestone `f9bfe98`. The P4 transcribes onboard microphone
audio without a Pi, UART inference, Wi-Fi connection or cloud API.

The Transcription interfaces add app controls and saved-file browsing on G2,
OLED and Web Sensors → Microphone. See [TRANSCRIPTION_UI.md](TRANSCRIPTION_UI.md)
for use and [TRANSCRIPTION_UI_RESULTS.md](TRANSCRIPTION_UI_RESULTS.md) for
qualification and hardware limits.

The previous `transcripts-app-v1` added optional saved transcripts through the
shared local/Pi result path. See [TRANSCRIPTS.md](TRANSCRIPTS.md) for the persistent
setting and downloads, and [TRANSCRIPT_RESULTS.md](TRANSCRIPT_RESULTS.md) for
qualification. It retains the session weight cache measured in
[CACHE_RESULTS.md](CACHE_RESULTS.md) and [cache-validation.json](cache-validation.json).
Earlier continuous and bounded-model evidence remains in
[CONTINUOUS_RESULTS.md](CONTINUOUS_RESULTS.md),
[continuous-validation.json](continuous-validation.json), [RESULTS.md](RESULTS.md)
and [validation.json](validation.json).

## Shared application path

`HAL_Audio` supplies raw 16 kHz mono int16 PCM to `System_STT`. The broker owns
the authenticated session, audio lease, bounded buffers and cancellation. PDM
and G2 microphones use the same capture interface. A dedicated capture task
continues draining PCM while a separate worker transcribes completed segments.
The local engine loads pinned QuartzNet5x5 weights, computes log-mel features,
runs P4 int8 ESP-DL kernels and greedily decodes CTC text. A session-owned
`ModelCache` retains 7,172,016 verified raw model bytes in PSRAM across segments.
Every segment checks the pinned file header and hashes the resident bytes before
vendor parsing. Each segment creates a fresh `dl::Model` and activation arena,
then releases those per-segment allocations; session weights are released on
stop, cancellation or error after inference returns and capture joins. One-shot inference remains
self-contained and unchanged. No entire-session audio or transcript accumulates
in RAM, and raw audio is not written to storage.

Segmentation is a portable C++ energy detector, **not neural VAD**. It measures
DC-removed RMS without changing the PCM supplied to the model. Its amplitude
decision helper is shared with the existing Pi recorder; that recorder retains
its original DSP, thresholds and timing. Continuous capture measures the ambient
level for 500 ms while status is `preparing`, then uses a bounded five-second
noise window. Calibration audio is excluded from model input and pre-roll,
while still counting toward absolute sample positions. Raw-RMS onset/silence
minimums are **45/16**, chosen to separate measured room noise (3–31 RMS) from
speech peaks (124–339 RMS). Ambient-relative and utterance-peak gates can raise
those thresholds; they remain configurable for other microphones or rooms.
The noise estimate cannot rise through an active utterance, and its peak resets
after a natural pause. Current defaults retain 300 ms of pre-roll, require
200 ms of voiced audio, and end on 600 ms of quiet once a segment spans at least
eight seconds. That minimum span is unchanged by weight caching; throughput and
endpoint timing still need to be considered together. Continuous speech is cut at twenty seconds;
normal stop also flushes a shorter voiced tail. Indefinite silence does not
trigger periodic inference.

One twenty-second working buffer and three twenty-second queue slots bound the
PCM storage at 2,560,000 bytes. The result queue holds at most eight chunks of
512 text bytes each. If inference or a receiver cannot keep up, the session
fails visibly rather than overwriting audio or text. HAL loss counters are
checked at startup, during capture and at stop. Cancellation waits for both
inference and the exact HAL owner to release their resources before reuse.

`System_Dictation` remains the common OLED/G2 keyboard API and result mailbox.
Both providers use its non-destructive peek and exact receipt/commit operations;
the UI has no local-provider branch. `ENABLE_LOCAL_STT=1` selects continuous local
capture. Otherwise the existing **Pi UART v1** provider still records one WAV and
returns one final result. Pi continuous transport is a design/adaptation task,
not implemented or qualified here; see [CONTINUOUS_PI.md](CONTINUOUS_PI.md).
Local failure never exports audio to another provider. No S3 firmware or S3
speech configuration was changed for this milestone.

The portable frontend and CTC decoder are ordinary C++; P4 inference is behind
`System_STTLocal`. Production `components/hardwareone/stt/` is the single
implementation, and experiment tests compile those sources directly.

## Continuous CLI

Log into the usual serial console, stop other microphone consumers, and start:

```text
closesr
closemic
stt start
```

The reply contains a 16-character run ID. Use that ID below and speak only once
`state` is `recording` (the microphone is already busy during calibration):

```text
stt status <id>
stt next <id>
stt ack <id> <sequence>
stt stop <id>
stt cancel <id>
```

- `status` reports capture/inference activity, 64-bit session time and sample
  count, completed segments, pending audio/text, overruns, RMS/noise/threshold
  readings, and errors. `weightsReused` describes the most recently processed
  segment, not whether weights are currently allocated; it may remain true in
  terminal status after the cache has been released. There is no total
  capture-duration limit. Individual segments and queues remain bounded;
  sequence exhaustion is an explicit failure requiring a new session.
- `next` peeks at the oldest result. When `available` is true, it returns
  `sequence`, `startSample`, `endSample`, `forcedBoundary` and `sttText`.
  Sample offsets are absolute 16 kHz session positions, with an exclusive end.
  Silence gaps may therefore appear between chunks.
- After accepting the text, acknowledge its exact sequence with `ack`. Retrying
  `next` returns the same unacknowledged chunk; retrying a completed ack is safe.
  Future/out-of-order acks fail. Keep polling and acknowledging while recording
  so the eight-chunk mailbox does not fill.
- `stop` finishes capture and drains admitted segments, including the voiced
  tail. Continue fetching and acknowledging until `workerActive` is false and
  `pendingTexts` is zero. An empty `next` while the worker is active is not an
  end-of-session signal.
- `cancel` discards queued audio and private text. Poll until the worker stops;
  cancellation never forcibly frees memory still used by capture or inference.
  Accepted text already saved to a transcript is retained.

Only the originating live transport session may retrieve or acknowledge text.
Logout revokes the session; retained terminal results expire after five minutes.
Speech is text input, never a device command. Shared console/web/file/BLE-debug
mirrors redact transcripts.

## Bounded recordings and finite input fields

The earlier one-shot CLI remains available:

```text
stt record 8
stt status <id>
stt stop <id>
stt result <id>
```

`record` accepts 1–20 seconds (default 10). Its `stop` finishes early and
transcribes; `result` reads the completed one-shot result. Use `next`/`ack` for
continuous sessions and `result` for bounded recordings.

OLED/G2 dictation now shares continuous chunk delivery when the local provider
is selected. The input field is still finite: the existing 256-byte keyboard
buffer and each field's own capacity remain in force. Chunks can be consumed in
pieces without dropping an unconsumed tail, and the backend is acknowledged only
after the complete chunk has been accepted. A full field stops visibly and
discards the remainder instead of acknowledging text that was never inserted.
G2's existing character filtering remains a consumer policy. These physical UI
paths still require accessory qualification; the attached P4 has no display.

## Current limits

- This produces **phrase-level results, not streaming tokens or word-by-word
  captions**. A natural endpoint normally waits for the eight-second minimum
  segment span, then model loading and inference add latency. Capture continues
  during that processing. A same-input numeric probe measured 4.497 seconds
  cold and 2.003 seconds with cached weights. Initial full-app model-loading
  measurements were about 3.4 seconds cold and 0.17 seconds warm; these are
  loading times, not end-to-end speech latency. See
  [the current qualification report](CACHE_RESULTS.md) for continuous throughput
  and camera contention. Earlier bounded full-app timings in
  [RESULTS.md](RESULTS.md) predate the session cache.
- Hard boundaries have **zero overlap**. Raw samples are not duplicated or
  silently skipped across a forced cut, but a word split at twenty seconds can
  be misrecognized because each segment has independent normalization, context
  and CTC decoding. There is no automatic text-overlap or repeated-word removal.
- The energy detector can mistake sudden noise for speech or miss very quiet
  speech. Its raw-RMS minimum thresholds are configurable in the segmenter;
  physical testing currently targets the P4 onboard microphone. G2 microphone
  qualification remains outstanding.
- English lowercase text with apostrophes; no punctuation restoration. Each
  result is limited to 512 bytes, with an explicit failure on overflow.
- ESP-SR and the ordinary microphone/recorder must be stopped first. STT does
  not silently suspend or re-arm command recognition. It preserves saved audio
  settings; software `micgain` does not alter this raw-HAL capture path.
- Accuracy remains limited. Four synthesized held-out sentences had eight word
  errors in fifty reference words on float and int8 inference. Room speaker-to-
  microphone tests made additional errors. These small historical checks are
  not a representative WER benchmark or proof of human-speech accuracy.
  Offline beam decoding, input-amplitude scaling and trailing-silence tests
  found no consistent accuracy improvement; see
  [DECODER_LATENCY_NOTES.md](DECODER_LATENCY_NOTES.md). The cache changes no
  decoder, gain, frontend, model or segmentation behavior.
- The 16 MB flash layout is unchanged. The model occupies 5,490,696 compressed
  bytes including its envelope, expands to 7,172,016 bytes in PSRAM, and shares
  LittleFS with the existing ESP-SR model.
- To fit the continuous experiment, only `CUSTOM_ENABLE_WEB_R1_HEALTH` is disabled:
  the R1 health navigation/page, `GET /api/health/status`, and
  `POST /api/health/action` are absent. Ring Bluetooth/input, core health,
  logging/history and the other health interfaces remain enabled. No compiler
  optimization settings were changed.

The installed `transcription-ui-v1` build is 6,362,416 bytes, leaving 15,056
bytes in the unchanged app partition. Its SHA-256 is
`20a22d79b0ed9656aefa13ca6ff9108739a2db7e3ab2053adc38108910090f4d`.
The source release version 0.99.95 was separately rebuilt and checked without
flashing; that image has the same size and SHA-256
`1a9d5f690583e7db81315dffe6054449dc5046a2bc9f5ef7a690c7be128ecf4a`.
The inherited experiment-only `camerajpegprobe` command remains retired;
production camera/JPEG support remains enabled. The Transcription interfaces
fit by storing one existing web script as a lossless gzip asset. Current
interface evidence is in [TRANSCRIPTION_UI_RESULTS.md](TRANSCRIPTION_UI_RESULTS.md)
and [transcription-ui-validation.json](transcription-ui-validation.json);
previous image identities remain in their historical qualification reports.

## Reproduce

Use the pinned IDF 5.5.5 environment and preserved qualification snapshots.
These ignored application snapshots are required inputs: a fresh clone alone
cannot recreate the full experimental P4 application yet. Models are generated
separately using the provenance and export tooling below.
`prepare.py --capture` records production changes against `f9bfe98`;
`prepare.py` creates an isolated `private/app-p4`; `--refresh` refreshes only
that checked copy. `build.sh` verifies sources, inherited radio/JPEG overlays,
feature profile and dependency versions before building. It never flashes.
The ESP-DL 3.3.12 manifest explicitly registers the model's operations and four
P4 kernels, plus the runtime's tensor-conversion dependency, so ESP-SR pruning
cannot remove them.

The concurrent broker tests compile the actual production source with real host
threads and controlled HAL/model fakes:

```sh
python3 components/hardwareone/test/host/test_quartznet_cache.py --sanitize
python3 components/hardwareone/test/host/test_stt_runtime.py --sanitize
python3 components/hardwareone/test/host/test_stt_continuous.py --sanitize
python3 components/hardwareone/test/host/test_stt_continuous.py --thread-sanitize
python3 components/hardwareone/test/host/test_stt_segmenter.py --sanitize
```

ASan/UBSan and ThreadSanitizer checks cover capture during blocked inference,
stop/drain and cancellation joins, long sessions, queue pressure, loss at stop,
retry/ack/session fences, fault injection, and counter boundaries. Adaptive
segmenter regressions cover exact 500 ms calibration exclusion, two minutes of
3–31 RMS fluctuations without a segment, following 60-RMS speech detection,
DC/chunk-size invariance, peak reset, and uninterrupted hard-cut continuity.
Explicit 12/6 settings retain coverage for configurable 20-RMS speech detection.
These tests do not measure ESP scheduling, actual stack margins, microphone
quality or throughput.
Shared dictation and OLED/G2 consumer regressions are described in
[CONTINUOUS_PI.md](CONTINUOUS_PI.md).

`setup_export_env.py`, `export_quartznet.py`, `pack_model.py` and
`generate_compile_requirements.py` document the model build. See their `--help`,
`model-provenance.json`, `compile-requirements-provenance.json`,
`heldout-provenance.json`, and `frontend/README.md`. Model/frontend identities
are checked before the vendor parser sees any data. Generated weights, fixtures,
environments and device logs are ignored under `private/`.

The standalone `probe/` mounts existing LittleFS read-only, never formats it,
and processes embedded public recordings with the canonical production runtime.
Its partition CSV is supplied explicitly and compared before any app-only flash.
`stage_model.py` stages an additive model install against a fresh filesystem
image and verifies existing files are unchanged. USB coordination and full board
backups are private artifacts, not automatic setup.
