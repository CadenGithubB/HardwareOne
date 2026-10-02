# P4 offline translation feasibility

Research date: 2026-09-29 (America/New_York). Repository baseline: `154ad982`.
This is a feasibility investigation, not a working P4 translation implementation.
Confirmed first milestone: a **standalone English -> Spanish text translator**.
Enter English text and receive Spanish text. Microphone/STT integration, reverse
translation and spoken output are deferred. The target remains fully offline
execution on the P4; a Mac reference run is a model-screening step only.

The standalone Mac reference now runs real English-to-Spanish model inference
offline. The initial 20-sentence screen found both useful translations and
material errors; see [host results](HOST_RESULTS.md). This establishes a
reproducible reference, not a working P4 feature. The [port assessment](PORTING.md)
identifies the native runtime and storage work still required.

The [training feasibility report](TRAINING_REPORT.md) identifies a public FP32
checkpoint and a proposed fine-tuning/evaluation path. Translation training
is on hold while the user's 15×5 speech-to-text training runs. No translation
training or automatic follow-up job has been started; resuming compute work
requires the user to lift that hold.

## Run the standalone host prototype

From the project root, download and verify the public assets once:

```sh
python3 experiments/translation_p4/fetch_runtime.py
python3 experiments/translation_p4/fetch_mozilla_model.py
```

With Node.js available, translate a typed sentence offline:

```sh
node experiments/translation_p4/screen.mjs --text "I would like a glass of water without ice."
```

The JSON result includes `translated_text`. The selected default is Mozilla
tiny with the full output vocabulary; `--shortlist` reproduces the earlier
comparison configuration, which made additional errors. On this Mac the inspected bundled
Node executable is
`~/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node`;
substitute that absolute path if `node` is not on PATH. The recorded run used
Node 24.19.0. Other Node versions have not been exercised here.

Run the synthetic fixture set and save a new report:

```sh
node experiments/translation_p4/screen.mjs \
  --fixtures experiments/translation_p4/fixtures.json \
  --output experiments/translation_p4/private/new-screen.json
```

Output paths must be new; prior results are not overwritten. Downloads remain
under ignored `private/`; no npm lifecycle scripts or system installs are used.
The inference runner verifies hashes and rejects network requests. Setup
downloads need network access; inference does not.

The comparison-only EuroNano run is reproducible with
`python3 experiments/translation_p4/fetch_euronano_model.py` followed by
`node experiments/translation_p4/screen.mjs --candidate euronano`. It is not the
selected default; see the compatibility and quality findings in the host report.

## Existing hardware and speech path

The latest committed qualification records P4 revision 3.2 at 400 MHz with
32 MiB PSRAM and 16 MiB flash. English speech recognition already runs locally:
`HAL_Audio` -> `System_STT` -> QuartzNet5x5/ESP-DL -> authenticated text chunks.
Weights occupy 7,172,016 bytes and continuous audio buffers 2,560,000 bytes,
before frontend, activation and application allocations. See
[STT implementation and limits](../stt_p4/README.md) and
[cache qualification](../stt_p4/CACHE_RESULTS.md).

Current speech results arrive by phrase: an eight-second minimum segment span
with a quiet endpoint, followed by processing. Warm full-app processing with
camera took 1.17-5.93 seconds across varying segment lengths. Accuracy remains
experimental; the small synthetic test set and speaker playback tests do not
establish human conversation accuracy. Foreign-language speech requires another
recognizer. Text translation alone cannot make the current English recognizer
understand Spanish, French or other languages.

The committed STT image has only 15,056 bytes of app-partition headroom.
Its compressed speech model occupies 5,490,696 bytes in LittleFS alongside
ESP-SR. The qualified profile disables SD/SDMMC; the board's physical microSD
slot does not establish working model storage in this build. A translation
experiment therefore needs both a runtime-memory plan and a firmware/storage
plan. These are historical qualification facts, not a fresh device inspection.

## Concrete neural candidates

| Candidate | Verified published footprint | Assessment |
| --- | --- | --- |
| Mozilla/Bergamot tiny English to Spanish | 17,140,755-byte model, 16,907,062 parameters; vocabulary and optional shortlist are additional | A real bilingual translation model worth screening; no demonstrated P4 runtime or timing found. |
| Mozilla/Bergamot tiny Spanish to English | Same model byte count | Two direction models alone exceed 32 MiB; directions would need separate loading or another memory strategy. |
| TranslatePsy-EuroNano Tiny | 17,141,227-byte model in each direction; English to nine European languages and reverse use separate packs | Interesting multilingual alternative, but current published runtime is much too large for stock P4 deployment. |

Sources: Mozilla's [English to Spanish metadata](https://raw.githubusercontent.com/mozilla/firefox-translations-models/main/models/tiny/enes/metadata.json)
and [Spanish to English metadata](https://raw.githubusercontent.com/mozilla/firefox-translations-models/main/models/tiny/esen/metadata.json),
and the [EuroNano model card](https://huggingface.co/qvac/TranslatePsy-EuroNano).
EuroNano file sizes were independently read through the public
[Hugging Face file metadata API](https://huggingface.co/api/models/qvac/TranslatePsy-EuroNano/tree/main?recursive=true&limit=1000).
The original metadata-only investigation was followed by the pinned host
experiments documented in [HOST_RESULTS.md](HOST_RESULTS.md).

EuroNano's English-to-European Tiny pack also contains a 805,231-byte tokenizer
and 614,636-byte shortlist: 18,561,094 bytes total. Keeping that pack, current
STT weights and audio buffers resident would leave only 5,261,322 bytes of the
entire 32 MiB PSRAM, before either engine's working memory or other firmware.
This is a partial storage arithmetic exercise, not a RAM-fit result; tokenizer
structures and weight preparation can expand in memory.

The EuroNano authors report **666 MB peak RAM** for Tiny in their server test
configuration (four threads, beam size four), and a 36 MB two-direction bundle.
That is not a minimum-RAM bound or a P4 measurement. A custom bounded runtime
could use less; its feasibility remains untested. The authors' headline quality
comparison is for Base and must not be attributed to Tiny.
[Published evaluation](https://huggingface.co/blog/qvac/translatepsy-nano).

Bergamot's upstream [intgemm kernels](https://raw.githubusercontent.com/kpu/intgemm/master/intgemm/intgemm.h)
dispatch to x86 instruction sets. A P4 port needs suitable matrix kernels,
tokenization, encoder/decoder execution, memory planning and output search.
[ESP-DL supports relevant operators](https://raw.githubusercontent.com/espressif/esp-dl/master/operator_support_state.md),
but operator names alone do not establish an accurate converted translation
graph. A model file fitting in PSRAM is insufficient evidence.

## Remaining P4 experiment gates

1. Resolve the quality issues identified by the initial English-to-Spanish
   screen before selecting a model for the firmware port. Expand representative
   unseen typed sentences and obtain bilingual human review.
2. Start with one model, short bounded input/output and greedy decoding.
   Measure all allocations, including loading peaks, tokenizer, activation
   workspace and decoder state. Compare output against the reference runtime.
3. Establish the storage and app-size plan. Larger model packs likely need
   qualified SD storage or a materially smaller model; SD adds storage, not RAM.
4. Run a standalone P4 model probe and measure cold/warm latency, peak internal
   RAM/PSRAM, repeated-run recovery and cancellation. No P4 translation speed
   estimate is justified yet.
5. Only after those gates pass, connect translated text to a separate bounded
   service consuming STT results. Keep original text, session ownership and
   explicit errors. Start with sequential speech recognition then translation
   if both engines cannot coexist; measure the cost of loading them in turns.

English speech to Spanish text could reuse today's recognizer. Spanish speech
to English text additionally needs Spanish ASR; spoken output additionally
needs TTS and an audio output path. Native G2 Conversate currently rejects every
translation choice except OFF in `components/hardwareone/G2_Glasses.cpp` because
no translator is attached. That UI capability should follow a working backend.

Investigation status: pinned model/runtime downloads, standalone offline Mac
inference, synthetic quality screening and actual binary inspection. The P4
translator is not implemented or installed. No device was accessed and no
firmware was changed. Existing unrelated `experiments/p4_peripherals/` work was
preserved.
