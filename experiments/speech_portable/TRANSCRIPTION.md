# Live transcription on P4: feasibility notes

Investigated 2026-09-28. This is a follow-up to fixed voice-command recognition;
no complete transcription model has been installed or benchmarked on our boards.
A standalone P4 convolution probe has passed; its measured scope is recorded below.
The requested feature is **fully local speech-to-text on the P4**: capture,
model inference and decoding run on the board. A Raspberry Pi, UART link,
phone, Mac or cloud recognizer is outside this target. The existing S3/XIAO
plus Pi feature remains useful precedent for the interfaces, not the proposed
implementation. The Mac is used only to inspect, convert and validate models
before measuring them on P4. Text-to-speech (TTS) speaks written text and needs
a different model.

## What the current speech upgrade provides

ESP-SR's WakeNet detects a wake phrase; MultiNet recognizes the configured
command vocabulary. The P4 and S3 can share this behavior through HardwareOne's
existing audio HAL. It does not produce arbitrary sentences or live captions.
The official component supports both targets:
[ESP-SR](https://github.com/espressif/esp-sr),
[2.5.5 package](https://components.espressif.com/components/espressif/esp-sr/versions/2.5.5/readme).

## What 32 MiB of PSRAM buys us

It is useful space for quantized weights, audio buffers and model state. It is
not all available to a model: HardwareOne, Bluetooth, camera frames, inference
scratch buffers and intermediate activations share it. Nonvolatile storage is
separate: this board has 16 MiB of flash, so a model can fit RAM but still need a
microSD card to store it between boots.

At 16 kHz, mono, signed 16-bit PCM, audio costs 32,000 bytes per second: ten
seconds costs 320 KB. Buffering speech is easy compared with recognizing it.
An int8 model with 7 million parameters has roughly 7 MB of weights, before
scales, activations and caches. That is a plausible research budget here, not
a performance guarantee. Sustained inference must take less time than the
incoming audio, while leaving room for the rest of the application.

## Candidate reality check

| Approach | Evidence and implication |
|---|---|
| ESP-SR MultiNet | Suitable for local commands and a wake phrase. It cannot substitute for dictation. |
| Whisper tiny | The standard whisper.cpp table lists 75 MiB of model storage and about 273 MB memory. Quantization reduces these, but does not establish a working 32 MiB P4 port. It is not a straightforward installation on this board. |
| Moonshine Tiny Streaming | Official model list gives 34 million parameters. Even a rough one-byte-per-weight estimate uses about the entire PSRAM before working memory. The published runtime uses ONNX Runtime; no P4 result was established in this investigation. |
| A purpose-built small streaming CTC model | The most plausible fully local research direction: approximately 5–10 million int8 parameters, bounded state, short streaming chunks, and an MCU-oriented runtime. English accuracy and measured speed must be established before integration. |
| P4 capture plus a phone/computer recognizer | Outside the requested scope. Existing Pi-backed STT is not evidence that inference runs on P4; no companion transport is proposed for this milestone. |

Sources: [whisper.cpp memory table](https://github.com/ggml-org/whisper.cpp#memory-usage),
[Moonshine model list](https://moonshine-voice.readthedocs.io/en/latest/models/available-models/),
[Moonshine latency benchmark definition](https://moonshine-voice.readthedocs.io/en/latest/using/benchmarks/).
The memory estimates above are our calculations, not measured P4 results.

A useful caution: Moonshine's approximately 1.3 MiB Micro model recognizes a
small set of isolated letters, digits and commands. It is not the general
streaming recognizer in the table.
[Micro STT source](https://github.com/moonshine-ai/moonshine/blob/main/micro/stt/README.md).

The [Shenava ESP32-S3 research repository](https://github.com/Reza2kn/shenava-esp32s3)
investigates a 6.9-million-parameter Persian streaming model. It contains measured
quantization and PSRAM microbenchmarks alongside throughput estimates and
unfinished integration gates. That supports investigating this model size; it
does not establish a ready English model or a verified full P4 recognizer. Its
int4 work also demonstrates that reducing weight size can seriously damage
recognition accuracy. Do not treat its estimated real-time factors as our
hardware results.

## Next experiment for the fully local P4 target

Use the same HAL to supply 16 kHz PCM. First benchmark a suitable small English
model in a standalone P4 experiment, with known audio/transcripts, before adding
it to HardwareOne. Measure peak internal RAM and PSRAM, sustained real-time
factor, time to first partial/final text, word error rate, and behavior under
camera/Bluetooth load. A file fitting in PSRAM is only the first acceptance
check. Do not spend time porting a decoder before validating quantized accuracy.

Keep microphone capture separate from recognition backends: ESP-SR can remain
the command backend, while a future STT backend emits partial/final text. This
allows S3, P4 and G2 microphone inputs to remain shared even if only P4 supports
a particular local transcription model. HardwareOne's existing dictation code
already has an audio-to-host direction, but it currently depends on the UART
host link, which is disabled in the P4 speech profile. The local backend must
remove that dependency from local admission and completion, without enabling
a host link or weakening the existing Pi adapter's checks. The concrete seam
below separates those responsibilities.


## Concrete English candidates for the next P4 experiment

These are research candidates, not verified P4 ports. After the P4 recognized
and dispatched a spoken command, the official QuartzNet-5x5 checkpoint was
downloaded and inspected on the Mac. The host results below do not establish
P4 inference speed or general live transcription support.

| Candidate | Verified source evidence | P4 implication |
|---|---|---|
| QuartzNet-5x5 English | The authors list 6.7 million parameters and greedy LibriSpeech dev-clean/dev-other WER of 5.39%/15.69%. NVIDIA publishes `QuartzNet5x5LS-En.nemo`, listed as 24.06 MB compressed. | First candidate for a host-side quantization experiment. About 6.7 MB of one-byte weights is a calculation, before scales, activations and runtime state; it is not the measured export size. |
| NVIDIA Conformer-CTC Small English | The official model card lists about 13 million parameters and greedy LibriSpeech test-clean/test-other WER of 3.7%/8.1%. | Useful accuracy comparison, but attention, export/operator coverage and bounded streaming state add porting work. About 13 MB of int8 weights alone does not establish that the complete runtime fits. |
| English streaming Zipformer “20M”, 2023-02-17 | The maintainer's actual int8 ONNX encoder is 42.8 MB, with a 539 kB decoder and 260 kB joiner. | Reject this published export for an all-in-PSRAM 32 MiB P4 implementation. Parameter-count shorthand understated the delivered model size. |

Primary sources: [QuartzNet paper, Tables 1 and 2](https://arxiv.org/html/1910.10261v1),
[NVIDIA's official checkpoint listing](https://catalog.ngc.nvidia.com/orgs/nvidia/-/models/nemospeechmodels/1.0.0a5/file-browser),
[NVIDIA Conformer-CTC Small model card](https://huggingface.co/nvidia/stt_en_conformer_ctc_small),
and [Zipformer maintainer's files](https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-en-20M-2023-02-17/tree/main).
These WER numbers use different evaluation splits and unquantized source
models, so they are reference results rather than a direct comparison or P4
accuracy prediction.

QuartzNet's depthwise/pointwise convolution, BatchNorm, ReLU and CTC design is
a simpler conversion target than attention-heavy alternatives. ESP-DL
supports quantized 1D convolution with ordinary or depthwise groups; the
already-pinned 3.3.12 `dl_module_conv.hpp` also handles 3D input shapes and
routes depthwise convolution separately. This is an operator-level match,
not proof that a complete exported QuartzNet graph will run unchanged.
[Espressif operator support](https://github.com/espressif/esp-dl/blob/master/operator_support_state.md).

The original QuartzNet model uses long temporal kernels and requires future
audio context. Buffered transcription is a plausible first experiment; low
latency streaming is a separate accuracy/latency problem. Reducing future
context changes model behavior and must be evaluated. NVIDIA documents
symmetric padding by default and explicit controls for future context.
[NVIDIA convolution context API](https://docs.nvidia.com/nemo-framework/user-guide/25.07/nemotoolkit/asr/api.html).

### Checkpoint inspection before conversion

Official checkpoint URL (NVIDIA NGC model version `1.0.0a5`):

```text
https://api.ngc.nvidia.com/v2/models/nvidia/nemospeechmodels/versions/1.0.0a5/files/QuartzNet5x5LS-En.nemo
```

An initial headers-only request on 2026-09-28 returned HTTP 302. The subsequent
authorized host experiment downloaded this exact checkpoint; its measured
size and SHA-256 are recorded below. The [official model card](https://catalog.ngc.nvidia.com/orgs/nvidia/-/models/nemospeechmodels/1.0.0a5)
identifies this checkpoint as trained on LibriSpeech only, so conversational
and noisy-room accuracy needs direct evaluation.

The inspection and conversion sequence is:

1. Download the official `QuartzNet5x5LS-En.nemo` into an ignored experiment
   directory and record its URL, version, byte length and SHA-256. Retain its
   license/model metadata alongside the inspection report.
2. List the archive before extracting; accept only bounded regular files with
   paths confined to that directory. Inspect its YAML configuration with a
   safe YAML loader. Record sample rate, preprocessing, vocabulary, kernel
   sizes, strides, dilation and padding before assuming any streaming window.
3. Read tensor state on the host with `torch.load(..., map_location="cpu",
   weights_only=True)` in an isolated environment. Do not automatically retry
   an incompatible legacy checkpoint with unrestricted pickle loading. Report
   parameter and buffer counts separately, tensor shapes/dtypes and total
   bytes; compare them with the paper's rounded count.
4. Reproduce float transcripts on known English clips, then export/calibrate
   a P4-targeted int8 graph and compare transcripts/WER. Inspect unsupported
   operators, actual model size, activation peaks and required lookahead.
   Only then benchmark bounded audio chunks on the board through the shared
   HAL and measure sustained real-time factor and first/final text latency.

This sequence can reject an unsuitable model before adding a recognizer to
HardwareOne or changing board storage. Keeping STT behind a separate consumer
interface also leaves the existing microphone HAL and ESP-SR command backend
usable independently.


## Measured host inspection and float sanity check

The official archive was downloaded on 2026-09-28 into ignored
`private/stt-quartznet/`. It contains only `model_config.yaml` and
`model_weights.ckpt`, plus their enclosing directory entry. The inspection
rejects unexpected paths, links, duplicate entries and size overruns; the
checkpoint SHA is pinned before parsing. Tensor inspection uses
`torch.load(weights_only=True, map_location="meta")`, with no unrestricted
pickle fallback. The separate float decoder verifies the extracted hashes
before loading tensor values on the CPU.

The source is NVIDIA's [versioned checkpoint listing](https://catalog.ngc.nvidia.com/orgs/nvidia/-/models/nemospeechmodels/1.0.0a5/file-browser) and [model card](https://catalog.ngc.nvidia.com/orgs/nvidia/-/models/nemospeechmodels/1.0.0a5); the figures below come from our `inspection.json` and configuration calculations.

| Measured item | Result |
|---|---|
| Archive bytes | 25,225,193 |
| Archive SHA-256 | `d12bae27c1aa66cf69111ce3f8e56ac7aa25dac27734ea4c3315a259966a02f5` |
| Named tensors | 229 |
| Parameters identified by state keys | 6,713,181 |
| Buffer elements | 44,961; no unclassified tensor elements |
| Stored tensor bytes | 27,032,700 (about 25.78 MiB) |
| Frontend | 16 kHz mono; 320-sample Hann window, 160-sample hop, 512-point FFT, 64 mel bands; per-feature utterance normalization |
| Output vocabulary | 28 characters (space, lowercase letters, apostrophe) plus CTC blank |
| Convolution context, derived from configuration | 2,937 input feature frames; about 14.68 s right context, excluding STFT centering |
| Convolution compute estimate | 334,249,600 MACs per audio second, excluding frontend, normalization, activation and data movement |

Parameter/buffer classification follows the explicit tensor names: running
statistics, batch counters and frontend tensors are buffers; weight/bias
entries are parameters. These counts are measured from the checkpoint. The
context and MAC figures are calculations from its configuration and weights,
not measured timing. One-byte storage for the identified parameters alone
would be about 6.40 MiB, before quantization scales and working memory. The
actual quantized graph has not been exported. The 16 MiB board flash budget
must also accommodate firmware, existing user files and the separate ESP-SR
bundle; fitting PSRAM does not establish that all files fit onboard storage.

Two public English WAV fixtures and their published reference text were used
for a small host-only sanity check with PyTorch 2.10.0 and four CPU threads:

| Fixture | Audio duration | Word errors | Host decode time |
|---|---:|---:|---:|
| `0.wav` | 6.625 s | 1 / 18 words | 0.237 s |
| `1.wav` | 16.715 s | 0 / 48 words | 0.265 s |

[Fixture files and transcripts](https://huggingface.co/csukuangfj/sherpa-onnx-streaming-zipformer-en-20M-2023-02-17/tree/d42f2d9/test_wavs).
The first error was a missing letter in the last word. This confirms that the
inspected tensors can produce coherent English through our fixed-topology
float decoder. It is a two-clip smoke test, not a representative accuracy
benchmark or official NeMo numerical-parity test. Mac timing does not predict
P4 speed. No user's recorded audio was used or uploaded.

The frontend's utterance-wide normalization and long future context remain
major obstacles to exact low-latency streaming. Short overlapping chunks could
produce provisional text but change the model's inputs/context; accuracy and
latency would have to be measured. The useful next gate is a host-calibrated
quantized graph and comparison over a larger speech set, followed by actual
P4 model execution. The isolated pointwise probe below has passed, but no P4
STT model has been installed.

### License and reproduction

NVIDIA's maintainer states that NGC NeMo pretrained checkpoints use CC-BY-4.0
unless a model specifies another license, separately from the Apache-2.0 NeMo
software. This old archive and its current model/version API metadata contain
no license file or explicit override; retain the NVIDIA/model attribution and
this provenance with any future exported model.
[Maintainer's checkpoint-license clarification](https://github.com/NVIDIA-NeMo/Speech/discussions/1681).

The host implementation follows the checkpoint configuration and the
preprocessing/normalization/masking semantics inspected in NVIDIA's
[v1.0.0rc1 feature code](https://github.com/NVIDIA/NeMo/blob/v1.0.0rc1/nemo/collections/asr/parts/features.py)
and [Jasper block code](https://github.com/NVIDIA/NeMo/blob/v1.0.0rc1/nemo/collections/asr/parts/jasper.py).
It does not import or execute code from the model archive.

Create a disposable host venv under `private/stt-quartznet/venv` and install
`stt-requirements.txt`. From that environment:

```sh
python experiments/speech_portable/inspect_quartznet.py --download
# On subsequent runs use no --download; existing checkpoints are never overwritten.
python experiments/speech_portable/inspect_quartznet.py
python experiments/speech_portable/float_quartznet.py --fetch-fixtures
```

The fixture URL/length/SHA manifest, tensor/config inspection, full environment
freeze and individual float results remain in ignored private evidence. The
scripts access neither serial ports nor any firmware/build copies.


## Incremental alternative: PocketSphinx, with a smaller vocabulary

PocketSphinx is worth a separate bounded-streaming experiment because its C
API accepts incremental PCM and can expose partial hypotheses. It does not
require QuartzNet's long neural right-context window. That does not establish
that its default English recognizer fits this board or gives useful dictation
accuracy. [Official application/API guide](https://cmusphinx.github.io/wiki/tutorialpocketsphinx/).

Read-only inspection of upstream **v5.1.0** repository metadata and allocation
code found the following. No PocketSphinx model weights were downloaded or
runtime memory measured.

| Default English files | Bytes on disk, from upstream Git metadata |
|---|---:|
| `en-us.lm.bin` | 27,114,385 |
| Acoustic `mdef` | 2,959,176 |
| Acoustic means + variances | 1,677,464 |
| Acoustic `sendump` + transitions | 1,971,104 |
| Pronunciation dictionary | 3,275,645 |

[Versioned model files](https://github.com/cmusphinx/pocketsphinx/tree/v5.1.0/model/en-us),
[Git tree metadata with byte lengths](https://api.github.com/repos/cmusphinx/pocketsphinx/git/trees/v5.1.0?recursive=1).
These are storage sizes, not an assertion that the decoder allocates exactly
that much memory. The default bundle already exceeds the P4's total flash,
and its LM plus acoustic payloads alone approach/exceed the whole 32 MiB
PSRAM before HardwareOne and search state.

The RAM concern is supported by the implementation: `lm_trie_read_bin()`
allocates the unigram/quantization tables and n-gram byte buffer, then reads
the binary into those allocations. `ngram_search_init()` adds tables indexed
by dictionary size, active-word arrays, backpointers and score stacks. The
backpointer and frame tables can double as an utterance grows; two-pass search
also enables a growing acoustic feature buffer. Merely turning on `mmap`
does not turn this trie or its search state into zero-RAM data, and regular
LittleFS/SD files should not be assumed to offer Linux-style file mapping.
[LM allocation/read path](https://github.com/cmusphinx/pocketsphinx/blob/v5.1.0/src/lm/lm_trie.c#L359),
[search allocations and growth](https://github.com/cmusphinx/pocketsphinx/blob/v5.1.0/src/ngram_search.c#L135),
[acoustic buffer growth](https://github.com/cmusphinx/pocketsphinx/blob/v5.1.0/src/acmod.c#L381).

There is historical evidence for a smaller configuration: the official 2010
release described memory below 20 MB for medium-vocabulary recognition. That
is an old configuration-specific result, not a v5.1.0/P4 RAM measurement.
The 2006 paper's 0.87 real-time factor on a 206 MHz device used a **994-word**
bigram task and traded accuracy for speed; it does not establish general
English dictation at that speed.
[Historical memory claim](https://cmusphinx.github.io/page35/),
[original experiment, sections 2 and 5](https://www.cs.cmu.edu/~dhuggins/Publications/pocketsphinx.pdf).

A meaningful next gate would be a small, explicitly domain-limited language
model **and matching reduced pronunciation dictionary**, followed by allocation
instrumentation with memory mapping disabled. Measure initialization peak,
steady state and worst-case utterance peak separately. Test first-pass-only
search (`fwdflat=false`, `bestpath=false`), narrow search beams only with an
accuracy comparison, and enforce a maximum utterance duration plus endpointer
resets to bound history growth. Then benchmark real-time factor and partial
text delay on P4. These settings are an experiment proposal, not a tested
configuration.

Reducing vocabulary/LM size changes what speech can be recognized; a small
command grammar is not free-form STT. General English names and words outside
the selected dictionary remain a limitation. A pruned PocketSphinx setup may
fit the available memory, but the default bundle is not a credible drop-in
for HardwareOne's current 32 MiB/16 MiB P4. The evidence supports measuring a
smaller configuration, not promising general live captions.
[CMUSphinx language-model and grammar distinction](https://cmusphinx.github.io/wiki/tutoriallm/).


## Concrete integration seam in HardwareOne

This section is a code review and interface proposal, not an implemented STT
feature. Locations refer to the current research checkout; symbols are the
stable references if later edits move line numbers. The firmware-side Pi
protocol and its consumers were inspected here; the separately maintained
Pi daemon was not audited as part of this local-P4 step.

### What already exists

| Layer | Current code and behavior | Reuse for P4 |
|---|---|---|
| Physical capture | `components/hardwareone/HAL_Audio.h:69` exposes exclusive `audioCaptureStart(owner, rate)` / `audioReadPcm()` / `audioCaptureStop(owner)`. PDM is board-configured; G2 is fixed at 16 kHz. Reads may be short. | One mono int16, 16 kHz input contract. No P4 microphone driver inside the recognizer. |
| Owned, bounded utterance capture | `System_Microphone.h:28` defines the 30 s STT ceiling; `startRecordingOwned()` uses exact owner tokens, VAD, finalization and deferred cleanup. `System_Microphone.cpp:338` publishes the stable result and IDLE before calling `dictationOnCapturePublished()`. | A first buffered local experiment can consume the closed owned WAV. Do not infer or perform filesystem work from the recorder's terminal callback. |
| Dictation session manager | `System_Dictation.cpp:337` validates the owner/result/path before dispatch; `:588` currently requires a live, authenticated, capable UART host. `:865` supervises host loss/timeouts; `:993` delivers once to the initiating display epoch. | Keep owner, field/session, cancel, timeout and exact-path cleanup logic; replace the host-specific decision/dispatch with a selected STT provider. |
| Existing Pi adapter | The firmware emits `dictate_request <id> <path>` and `dictate_cancel <id>`. `System_UartLink.cpp:930` accepts direct, session-pinned `dictate result/fail`; `:1434` implements authenticated `voicefetch`. `System_LiveAudio.h` separately offers opt-in `live-pcm-v1` recorder shadow and native Conversate streaming. | Preserve as an optional legacy provider. Local P4 inference must not create a pretend UART session, announce `hostready`, or route a local result through a UART command string. |
| OLED keyboard | `OLED_Utils.cpp:2397`, `oledKeyboardDictationTick()`, drains final text with `dictationTakeText()`, appends within the field limit and refuses denied/secret fields. | Keep this consumer unchanged for final text where a physical display is present. This P4 test board has no attached display. |
| G2 keyboard | `G2_Glasses.cpp:30671` arms `dictationBeginFor(SOURCE_G2_GLASSES, epoch)` and drains `dictationTakeTextFor()`; `G2_Page_TextEntry.cpp:619` appends through the current field's serial/identity guard and printable-text filter. | A local provider can feed the same final-text path without changing G2 audio transport or text rendering. Physical G2 qualification remains a later test. |
| Web and CLI | `System_Microphone_Web.h` exposes microphone settings, levels and recording playback. `WebPage_Speech.cpp:29` serves ESP-SR command status; `WebServer_Server.cpp:6134` registers authenticated `/api/cli`. There is no general STT transcript consumer on these surfaces in this inspected path. | Reuse authenticated command/session infrastructure, then add an owner-scoped STT result/status interface and small UI. Existing audio controls do not by themselves make web/CLI dictation available. |

All bare filenames in the table are under `components/hardwareone/`. Native G2
**Conversate captions** are separate from G2 keyboard dictation:
`docs/G2_CONVERSATE_AUDIO.md:126` still lists session-fenced partial/final return
to native TRANSCRIBE command 6 as future work. A working keyboard transcript
would not prove that native live captions work.

### Proposed provider contract and ownership

Add a small `System_STT.h/.cpp` broker and a local implementation such as
`STT_LocalP4.cpp`, with an optional `STT_UartHost.cpp` adapter for existing
builds. These are proposed filenames. The public contract should describe
capability, PCM format, bounded input and text results, with no chip-specific
callers. Only backend selection, optimized kernels and model storage belong
behind target/configuration gates. A board without a compiled, loaded and
qualified local model reports that capability unavailable.

The minimum contract would be:

```cpp
// Interface sketch only: not declarations present in this checkout.
SttCapabilities sttCapabilities(); // readiness, format, max duration, partials
SttStartResult sttBegin(const SttRequest& request, SttToken* token);
SttOfferResult sttOfferPcm(SttToken token, const int16_t* pcm, size_t count);
bool sttFinishInput(SttToken token);
void sttCancel(SttToken token);     // exact request; completion is asynchronous
bool sttPollEvent(SttToken token, SttEvent* event); // partial/final/error/stopped
```

`SttRequest` binds an exchange ID to the initiating source and its live
transport/session epoch, the chosen provider generation and input format.
`SttToken` prevents stale callbacks from a previous run reaching a new field.
`SttEvent` carries the same identity, monotonic revision/segment ID, bounded
text and a terminal/error indication. Partial text is replaceable; a final
segment is delivered exactly once. A stop acknowledgment is required before
reusing/freeing provider queues, model state or buffers. `sttOfferPcm()` copies
into a fixed-capacity queue before returning and explicitly reports overflow;
it cannot retain a borrowed recorder/DMA pointer or silently skip audio.

The broker, rather than the model worker, checks the initiating session again
when publishing a result. `System_User.h:59` already provides session epochs,
including web and serial; G2 keeps its existing paired-owner epoch checks.
A local provider uses a provider-run generation, not a host epoch. The UART
adapter retains its additional named/physical UART-session fences. Extract
the source-neutral part of `dictDeliver()` into an internal completion path;
leave `dictate result` as an authenticated adapter entry point. Do not feed
transcribed text into `executeCommand()` or ESP-SR's voice-command dispatcher:
dictation supplies text for the user to review/submit.

For OLED/G2 compatibility, retain `dictationBeginFor()`, snapshots and
`dictationTakeTextFor()` around this broker. Change `dictationAvailable()`,
`dictationProcessPublished()`, waiting supervision and cancellation to consult
the latched provider. Update the host-only explanation in
`System_BuildConfig.h:1459` and `System_Dictation.h`; local STT readiness must
not depend on `ENABLE_UART_HOST_LINK`. Keep a generic STT build gate independent
of display support so a headless web/serial build can use it, while existing
keyboard dictation stays enabled only where a keyboard surface exists.

Web/CLI require a small new consumer, not a separate recognition engine.
Owner-scoped start/status/cancel/result commands can use the existing
command permissions and `captureTransportSessionEpoch()` machinery. A web
view can poll its own result or use a session-confined event channel; a serial
client can poll its own result. Do not publish transcripts to the global log,
shared command feed or a source-wide single mailbox: multiple web sessions
can exist simultaneously. Preserve the current 256-byte keyboard limit and
printable-text rules; a captions view would need its own bounded segment
history rather than silently increasing the keyboard buffer.

### Preprocessing contract: HAL PCM is not the current PDM WAV

The inspected dictation path calls `startRecordingOwned(owner, 1200, true)`
(`System_Dictation.cpp:719`) without overriding preprocessing. The recorder
reads `audioReadPcm()` at `System_Microphone.cpp:1016`, then calls
`micProcessForSource()` **before** level/VAD calculation, live-shadow delivery
and WAV writing. For onboard PDM, that helper (`:691`) calls
`applyMicAudioProcessing()` with `filtersEnabled=true`, its declared default.
The chain at `:755` is chunk-adaptive DC subtraction, approximately 50 Hz
high-pass, **0.97 pre-emphasis**, software gain and int16 clipping. For G2,
`micProcessForSource()` skips this entire software chain and records decoded
LC3 PCM. Neither recorder path performs QuartzNet log-mel normalization.

`HAL_Audio.cpp:426` itself only pulls PDM I2S PCM or the decoded G2 ring; it does
not apply that HardwareOne software chain. Here “raw HAL PCM” means before
HardwareOne's post-capture processing, not unprocessed sensor bits or a claim
that the glasses perform no acoustic processing. `captureAudioSamples()` is
**not** an interchangeable raw-HAL adapter: it also calls
`micProcessForSource()` (`System_Microphone.cpp:2157`).

The inspected QuartzNet frontend (`float_quartznet.py`, `Decoder.features()`)
expects audio scaled consistently with its float WAV loader, adds dither,
applies **0.97 pre-emphasis**, calculates log-mel features and normalizes each
feature over the utterance. Running an ordinary PDM recording through that
frontend therefore applies pre-emphasis twice. Undoing a clipped, gained,
DC/HP-filtered WAV is not a reliable recovery strategy. Also distinguish
int16-to-float scaling (normally divide by 32768) from waveform loudness
normalization and log-mel feature normalization: they are different steps.
Model pre-emphasis and feature normalization each belong in exactly one place,
including when an exported graph embeds its own frontend.

For the proposed local provider, specify 16 kHz mono int16 PCM **before the
recorder's high-pass/pre-emphasis**, plus an explicit source, processing mode
and latched gain value. Prefer raw HAL PCM with the model's own validated
frontend. If physical PDM needs level/DC correction, qualify a separate
DC/gain-only conditioning stage; `applyMicAudioProcessing(..., false)` already
skips HP/pre-emphasis but still subtracts DC and applies gain, so it is not a
no-op. Do not apply PDM gain to G2 automatically. Do not normalize or apply
pre-emphasis again when accepting already transformed features.

A buffered WAV proof consequently needs an explicit per-capture raw/model
processing mode, or a fixture with known compatible provenance. A WAV header
alone cannot tell whether pre-emphasis/gain already occurred. A raw streaming
tap must copy before `micProcessForSource()`; the existing live-shadow offer at
`:1174` is **after** processing and cannot silently be reused as raw input.
Keep the current recorder/Pi behavior as the default for existing consumers.

Saved gain is user state: `initMicrophone()` restores
`gSettings.microphoneGain` (`System_Microphone.cpp:1973`), and the PDM software
multiplier is `24 * gain / 50` with gain zero producing silence. The local
provider must explicitly snapshot any gain it uses, document whether it is
applied before the model, and preserve saved gain/source/rate settings across
start/stop. Do not call the persistent `micgain`/`micsamplerate` commands merely
to establish a model's input format. Raw HAL capture does not automatically
apply the saved gain. Calibration fixtures and the future on-board path must
use the same declared preprocessing contract.

### Buffered first, streaming only after measurement

For the first end-to-end local proof, feed a finalized, exact-owner WAV into
the local broker worker in bounded blocks, then deliver final text through
the existing keyboard consumer or the new CLI result interface. The WAV must
be validated as 16 kHz, mono, signed 16-bit PCM for this model. The current
recorder inherits the user's sample-rate setting and applies source-specific
processing, so this is not automatic: introduce a scoped 16 kHz capture option
or explicitly reject incompatible input, without changing the saved mic
preferences. Verify frontend/gain/filter expectations against model fixtures;
do not assume the recorder's current pre-emphasis matches model training.
Close the model's reader before acknowledging stop/terminal completion and
releasing the manager's exact-path cleanup debt. This avoids deleting/reusing
the exchange file while a canceled inference worker still reads it.

For a subsequently proven streaming model, add a source-neutral, bounded PCM
sink before the recorder's `micProcessForSource()` call when raw input is
required, following the current live-audio shadow's nonblocking handoff pattern,
or use a dedicated capture worker that exclusively owns
`audioCaptureStart("stt", 16000)`. Choose one capture owner per run. The former
reuses recorder/VAD ownership; the latter avoids mandatory WAV writes for
continuous captions and must provide equivalent duration/cancel/source-loss
handling. Neither starts a second `audioReadPcm()` drainer. The existing UART
shadow queue/lease is not the local STT broker; only its bounded handoff pattern
is reusable. Publish levels from the active owner for UI readers.

ESP-SR currently owns capture as `"sr"`; recording uses `"mic"`. STT cannot
run a second independent microphone reader beside them. Initially use explicit
mode switching with checked worker joins and source release, preserving saved
microphone/source settings. If wake-triggered STT is later wanted, use an
explicit command-to-dictation handoff, not two concurrent drainers. On P4,
releasing ESP-SR model/AFE state while STT is active may also be necessary to
meet the measured memory budget; that is a lifecycle decision to test, not an
assumption that both model sets fit together.

The integration gate remains actual P4 results: load/steady/peak internal RAM
and PSRAM, model storage fit, real-time factor, first/final text latency,
accuracy on representative speech, cancellation/source loss, repeated runs
without a leak, and behavior with the normal web/BLE/camera features enabled.
Mac transcripts establish model sanity only. No interface refactor can remove
QuartzNet's measured long context or establish that its kernels meet the P4
budget; these must pass before calling the feature live local transcription.


## First measured P4 kernel result

The [standalone operator probe](operator_probe/README.md) ran on the P4 at
400 MHz with PSRAM at 200 MHz. It tested QuartzNet's dominant 512-to-512
pointwise convolution at T800 with deterministic synthetic int8 inputs and
weights. All three paths matched byte-for-byte, passed 128 independently
computed scalar-reference samples, and produced stable repeat checksums.

| Execution path | Median of three runs after warm-up |
| --- | ---: |
| Native Conv1D, single core | 88.245 ms |
| Equivalent Conv2D, single core | 88.843 ms |
| Equivalent Conv2D, two cores | 58.767 ms |

ESP-DL's native 1D mapping cannot use its height-based core split. Expressing
the same computation with time as the 2D height allowed the explicit multicore
path to run about 1.50 times faster than native Conv1D in this test. Every path
used the same PSRAM placement. This is a measured optimization for one shape,
not an assumption about all layers.

The signature occurs 17 times for a 16-second feature input. Multiplying the
multicore median gives 0.999 seconds, an operator-only screening ratio of
0.0624. That is an arithmetic projection, **not whole-model real-time factor**.
PSRAM free space returned exactly to its initial measurement after buffer
cleanup. The probe ran separately from HardwareOne, so it does not measure
interference from Bluetooth, camera, web or microphone tasks.

The result supports proceeding to per-channel scales, folded bias, the longest
depthwise convolution and a calibrated real-weight export. Full transcription
accuracy, complete runtime memory, flash fit and total inference latency remain
unmeasured. The model's long future context and utterance normalization still
make buffered dictation the nearer target; this probe does not establish live
captions. The tested HardwareOne speech application was restored afterward.

## Current P4 flash budget

The preserved P4 partition layout has a 10,334,208-byte LittleFS region and a
6,377,472-byte application region. The v8 installation inventory contains
3,808,085 bytes of file payload across 22 files, including the 3,052,231-byte
ESP-SR bundle. Thus even ignoring filesystem overhead, at most 6,526,123 bytes
remain. This is less than QuartzNet's 6,684,992 convolution-weight bytes at
one byte per weight, before bias, scales, model metadata or alignment.

These are measured file payload totals and calculated upper bounds, not a
filesystem free-block measurement. No unrelated files were deleted. A full
local STT deployment therefore needs an explicit storage plan as well as a RAM
and speed result: for example, an actually measured compressed model format,
external storage if later available, or a deliberate choice of resident model
set. Do not repartition the user's board or remove their files implicitly.
The standalone operator probe has no model storage requirement and must not
mount or write the filesystem.
