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

## Fine-tuned model export (run2, 2026-09-29)

`components/hardwareone/stt/stt_model_identity.h` now identifies a model
exported from the AMI/LibriSpeech fine-tune `stt_train` run2 (best step 7484),
not the NGC checkpoint in `model-provenance.json`. The byte counts quoted above
(5,490,696 / 7,172,016) describe the previous model.

| item | value |
|---|---|
| weights | `model_weights.ckpt` sha256 `d6e7f963195194345e313d0a4ec6182967313a71acfff49a4622a42c968d9900`; config unchanged (`1e829dd4...`) |
| ESPDL | 7,171,984 bytes, sha256 `f6b81ad35e808db481b72bcf8ba7c197dfe81836e292926be79bda364fd80df0`; 71 nodes (61 Conv, 5 Add, 5 Relu) |
| exponents | input **-4** (was -5), output -2 |
| `.stt` | 5,487,504 bytes (compressed 5,487,408), sha256 `5848439b399c9197b71b9df1417c021c19cb29416897fe7f3ed61430a0243adb` |
| frontend | unchanged, `ca91eb96...` |

Tooling changes are all opt-in. A default-path re-export of the original
checkpoint reproduced the deployed graph: the parsed initializers, nodes and
exponents are identical, and the fixture I/O is byte-identical. The ESPDL file
itself is 48 bytes shorter and its hash differs. The cause of that byte
difference was not identified. `$P` below is `private/export-venv/bin/python`
(see `setup_export_env.py`).

* `quartznet_graph.load_checkpoint` accepts one extra weights hash from
  `HW1_STT_ALLOW_WEIGHTS_SHA256`. The config hash is never overridable.
* `export_quartznet.py --calib-stores STORE:COUNT ...` adds seeded clips from
  the read-only `stt_train` dev stores to the calibration set (test stores are
  refused). The fixture cases, graph input shape and embedded test values stay
  those of fixture 0. With a non-pinned checkpoint, the pinned float
  cross-check decoder is skipped (`torch_original_text` is null) and the manifest
  records the actual hashes.
* `evaluate_dev_int8.py` compares float and frozen-int8 greedy WER on
  seeded dev-store subsets. It excludes calibration clips and first requires
  byte-exact reproduction of each export's fixture outputs.

```sh
export HW1_STT_ALLOW_WEIGHTS_SHA256=d6e7f963195194345e313d0a4ec6182967313a71acfff49a4622a42c968d9900
nice -n 10 $P export_quartznet.py --checkpoint /Volumes/USB2/stt/work/deploy/ckpt-run2 \
    --output /Volumes/USB2/stt/work/deploy/export-run2 --threads 6 \
    --calib-stores ami-sdm-validation:20 ami-ihm-validation:20 librispeech-dev-clean:20
nice -n 10 $P pack_model.py --manifest /Volumes/USB2/stt/work/deploy/export-run2/manifest.json \
    --frontend-sha256 ca91eb9683f7151b8be1a01532a6790c6eb7d515f67169a207751e3fc48cc58b
```

Greedy WER (%) on 150 seeded utterances per dev store, with the 60 calibration
clips excluded. "orig" is the previously deployed `full-portable-v2` export.
"2-clip" is the same run2 weights with the stock fixture-only calibration.

| set (words) | orig float | orig int8 | run2 float | run2 int8 (62 clips, shipped) | run2 int8 (2 clips) |
|---|---:|---:|---:|---:|---:|
| ami-sdm-validation (1322) | 73.07 | 74.21 | 62.56 | 64.67 | 63.92 |
| ami-ihm-validation (1294) | 55.02 | 56.57 | 42.04 | 42.04 | 42.89 |
| librispeech-dev-clean (2954) | 5.28 | 6.36 | 6.80 | 7.48 | 8.43 |

No int8 output reaches -128. One value in 1.48 M reaches +127 on LibriSpeech,
and none on AMI. The largest float logit is 42.1, against an int8 limit of 31.75.
The fine-tune costs about 1.1 points of int8 WER on LibriSpeech.

## QuartzNet15x5 base export (separate artifact, 2026-09-29)

An int8 export of the NGC `QuartzNet15x5Base-En` checkpoint (not fine-tuned) is
kept under `/Volumes/USB2/stt/work/deploy15/`. It is **not** installed. The
deployed 5x5 run2 model and `components/hardwareone/stt/stt_model_identity.h`
are unchanged. The packed `quartznet15x5.p4.stt` is 15,315,794 bytes. It
inflates to a 20,112,032-byte ESPDL with 201 nodes (171 Conv, 15 Add,
15 Relu). The input exponent is -4 and the output exponent is **-1** (the 5x5
uses -2). The generated header is `stt_model_identity_15x5.h`.

The 15x5 preprocessor config is identical to the 5x5 one, and its `fb` tensor
is bit-identical. Its stored Hann `window` differs from the 5x5 checkpoint's in
6 of 320 values, each by at most 6e-8 (the 5x5 copy is 1 ULP off
`torch.hann_window(320, periodic=False)`). Calibration and evaluation therefore
use the deployed 5x5 window/fb, which is what the device frontend
(`ca91eb96...`) computes.

The tooling changes are opt-in, and the 5x5 default path behaves as before.
`test_graph.py` and `test_pack_model.py` pass, and a default re-pack of run2
is byte-identical to the installed header and artifact.

* `quartznet_graph.CONFIG_15X5_SHA256` is a second pinned config (`023e8e2c...`).
  It is accepted only with non-pinned weights, which requires
  `HW1_STT_ALLOW_WEIGHTS_SHA256`, and only when the weights' encoder block
  count equals the config's (18). The block structure is always derived from
  the config.
* `export_quartznet.py --frontend-checkpoint DIR` takes the window/fb tensors
  from another pinned checkpoint. When used, the manifest records it under
  `frontend_checkpoint`.
* `pack_model.py --artifact-name/--identity-name` set the output file names.
  The defaults are unchanged.
* `evaluate_dev_int8.py --frontend-max-abs-diff X` accepts later models whose
  window/fb differ from the first model's by at most X. The first model's
  tensors are always used. The default is 0, meaning exact equality.

```sh
D=/Volumes/USB2/stt/work/deploy15   # ckpt-base15/ = symlinks to the NGC files + fixtures
export HW1_STT_ALLOW_WEIGHTS_SHA256=b2eb3ebc66e9e82829818131c4da02cf9e1109003c3d445b824d9489869143f9
nice -n 10 $P export_quartznet.py --checkpoint $D/ckpt-base15 --output $D/export-base15 --threads 4 \
    --frontend-checkpoint ../speech_portable/private/stt-quartznet \
    --calib-stores ami-sdm-validation:20 ami-ihm-validation:20 librispeech-dev-clean:20
nice -n 10 $P pack_model.py --manifest $D/export-base15/manifest.json \
    --frontend-sha256 ca91eb9683f7151b8be1a01532a6790c6eb7d515f67169a207751e3fc48cc58b \
    --artifact-name quartznet15x5.p4.stt --identity-name stt_model_identity_15x5.h
```

The calibration clips are the same 60 as for run2. The table shows greedy WER
(%) on the same 150 utterances per set as the run2 table; the run2 numbers
reproduced exactly. The LM column uses the deployed `meeting.lm` unchanged,
with the 29-class vocabulary, blank 28, and alpha/beta not retuned.

| set | 15x5 float | 15x5 int8 | 15x5 int8 + LM | 5x5 run2 int8 | 5x5 run2 int8 + LM |
|---|---:|---:|---:|---:|---:|
| ami-sdm-validation | 53.63 | 56.13 | 52.34 | 64.67 | 58.40 |
| ami-ihm-validation | 31.45 | 35.32 | 31.14 | 42.04 | 36.63 |
| librispeech-dev-clean | 3.86 | 4.98 | 4.81 | 7.48 | 5.96 |

No int8 output reaches +127 or -128, and no float logit exceeds the int8
range of +/-63.5. The largest float logit is 50.4. Quantization costs more
than on the 5x5: 2.5, 3.9 and 1.1 points.

Costs:
* MACs are 2.82x the 5x5.
* Single-thread host CPU time on an 8 s clip is 2.98x (3.53 s vs 1.18 s).
* Peak live activations are still 2048 B per output frame, taken from the
  ONNX liveness at the final 1024->1024 layer, so the runtime's arena rule
  carries over.
* PSRAM needed for a 20 s segment is 20.11 MB of weights + 2.05 MB arena +
  0.52 MB of features, about 22.7 MB. The runtime admission check wants
  23.8 MB including its 1 MiB margin (5x5: 10.9 MB), plus one contiguous
  20.1 MB block for the weights.
