# Standalone English-to-Spanish P4 port assessment

2026-09-29. The first feature is independent typed English -> Spanish text.
Microphone input, STT coexistence, spoken output and application UI wiring are
later work. This assessment and the host experiment do not establish working
P4 translation.

## Actual model inspection

`inspect_model.py` checked the downloaded Mozilla tiny model's SHA-256 before
parsing its Marian v1 binary structure. [model-tensors.json](model-tensors.json)
records every tensor's shape, offset, length, quantization multiplier and hash.
All 17,140,755 bytes are accounted for in non-overlapping ranges: 195 items,
including 54 intgemm8 arrays, 140 float32 arrays and one embedded configuration.
Excluding configuration text, 16,907,062 elements match the published parameter
count. The parser does not execute the model or parse its YAML configuration.

The model is SHA-256
`fa7460037a3163e03fe1d23602f964bff2331da6ee813637e092ddf37156ef53`.
Its embedded settings and tensor names identify a six-layer Transformer encoder,
two-layer SSRU recurrent decoder, 256-wide embeddings, eight attention heads,
1,536-wide feed-forward layers, and a shared 32,000-token vocabulary. The
`enc-cell: gru` legacy setting does not turn this Transformer encoder into a
GRU encoder. The published metadata agrees with the inspected configuration.
[Mozilla metadata](https://raw.githubusercontent.com/mozilla/firefox-translations-models/main/models/tiny/enes/metadata.json)

Reproduce from the project root after fetching the model:

```sh
python3 -B experiments/translation_p4/inspect_model.py \
  experiments/translation_p4/private/mozilla-enes/model.enes.intgemm.alphas.bin \
  --sha256 fa7460037a3163e03fe1d23602f964bff2331da6ee813637e092ddf37156ef53
python3 -B -m unittest discover -s experiments/translation_p4 -p test_inspect_model.py -v
```

The seven inspector tests passed, including incorrect hashes, truncation,
trailing bytes, oversized counts/dimensions, invalid names/types and invalid
quantization scales. This is format validation, not numerical model validation.

## Why the existing runtime is not a direct P4 port

The inspected `Wemb` table contains 8,192,000 int8 values (32,000 × 256).
Marian's binary loader expands a Wemb-named intgemm item into float32 and copies
other items into separate buffers. That makes the main embedding alone
32,768,000 bytes. Applying those source rules to the actual item map produces
**41,703,703 bytes of logical item payload**, before tokenizer, activations,
input model buffer, graph objects, allocator overhead or temporary packing.
This is a source-derived allocation calculation, not a measured runtime peak.
It already exceeds the board's 33,554,432-byte PSRAM capacity. A direct build of
stock Bergamot therefore fails the memory premise for this model.
[Marian binary loader](https://raw.githubusercontent.com/browsermt/marian-dev/master/src/common/binary.cpp)

The binary stores architecture-independent intgemm weights; non-embedding
matrices arrive transposed and require layout preparation for the chosen kernel.
Quantization multipliers are floats, with extra activation-alpha items. The
actual embedding multiplier is approximately 88.24735. Preserve these scales,
activation clipping and bias corrections rather than interpreting the bytes as
an already compatible ESP-DL graph.
[Marian integer preparation](https://raw.githubusercontent.com/browsermt/marian-dev/master/src/tensors/cpu/integer_common.h)
The alpha-model inference setting is `int8shiftAlphaAll`.
[Model format guidance](https://raw.githubusercontent.com/mozilla/firefox-translations-models/main/README.md)

Current upstream ESP-DL documents P4 acceleration and relevant operators such
as MatMul/Gemm, Gather, Softmax, LayerNormalization and elementary arithmetic.
It also states dtype restrictions: float32 MatMul/Gemm are not provided, and
mixed w8a16 MatMul requires static weights. Operator availability does not
establish scale compatibility, graph conversion or adequate translation quality.
[ESP-DL operator support](https://raw.githubusercontent.com/espressif/esp-dl/master/operator_support_state.md)
The project's STT qualification pins ESP-DL 3.3.12, but its ignored component
source tree is absent in this checkout. The current upstream table must not be
presented as verification of that pinned release.

## Bounded implementation path

Use the host Bergamot result as a reference and build a small inference-only
runtime for this single model architecture. The first proposed limits are one
sentence, at most 64 source tokens and 96 generated tokens, greedy decoding,
explicit rejection of overlong input and an explicit output-limit error.
These are engineering bounds to test, not user requirements or proven usable
conversation limits. Do not silently drop sentence tails.

1. Start with a portable host evaluator using the pinned int8 weights. Keep
   embeddings compressed, dequantizing only the required 256-value rows. Read
   or repack each weight once into its final layout; avoid retaining raw and
   prepared full-model copies. Preserve exact tokenizer normalization and IDs.
2. Validate a first encoder block against instrumented Marian: token IDs,
   embedding/position input, one quantized projection, attention and normalized
   output. This isolates transpose, scale, alpha, rounding and normalization
   errors before an entire port is built.
3. Extend to six encoder blocks and one SSRU decoder step with explicit
   recurrent state and source-attention cache. Then add the bounded greedy
   loop. Compare complete output-token sequences and intermediate tensor errors
   with the reference, including negation, names, numbers and punctuation.
4. Accelerate the measured matrix bottlenecks on P4. A custom kernel adapter
   preserving Marian arithmetic is one option; calibrated ESP-DL conversion is
   another and needs fresh end-to-end quality checks. Keep float normalization
   and softmax initially if their measured cost is acceptable. Freeze scratch
   lifetimes and instrument every allocation, including model loading.

Useful kernel shapes are `[N,256] × [256,256]`,
`[N,256] × [256,1536]`, `[N,1536] × [1536,256]`, and batched attention
matrices with head width 32. Full-vocabulary output adds 8,192,000 multiply-
accumulates per generated token solely for the 256 × 32,000 projection.
A shortlist may reduce this work but introduces another data structure and
can change output. Test both settings on the host before choosing.

For illustration only, with 64 source tokens and float32 intermediates:

| Individual allocation | Bytes |
| --- | ---: |
| Source hidden state, 64 × 256 | 65,536 |
| One feed-forward expansion, 64 × 1,536 | 393,216 |
| Eight source attention score matrices, 8 × 64 × 64 | 131,072 |
| K/V cache for two decoder layers, 2 × 2 × 64 × 256 | 262,144 |
| Two 256-value SSRU state vectors | 2,048 |
| Full-vocabulary output logits, 32,000 | 128,000 |

These are dimension arithmetic, not a complete memory budget or a fit claim.
Buffer overlap, Q/K/V projections, tokenizer expansion, packing, stacks,
allocator behavior and firmware allocations still need measurement. Layer-by-
layer buffer reuse makes this targeted route worth testing; no P4 latency
estimate is justified yet.

## Storage is a separate gate

The downloaded uncompressed pack is 21,313,322 bytes: model 17,140,755,
SentencePiece vocabulary 825,463, optional shortlist 3,347,104. Even without the
shortlist, the model plus vocabulary exceed total 16-MiB flash. Model weights
alone exceed total flash by 363,539 bytes.

The published gzip model plus vocabulary total 13,017,419 bytes, leaving only
3,759,797 bytes of the entire flash before firmware, bootloader, partition and
filesystem overhead. Compression therefore does not fit alongside the existing
approximately 6.37-MB qualified application. A genuinely smaller standalone
probe may use another layout, but that requires a concrete build/partition
budget and preserving existing device data before any flash operation.
[Model gzip metadata](https://raw.githubusercontent.com/mozilla/firefox-translations-models/main/models/tiny/enes/model.enes.intgemm.alphas.bin.gz),
[vocabulary gzip metadata](https://raw.githubusercontent.com/mozilla/firefox-translations-models/main/models/tiny/enes/vocab.enes.spm.gz)

For a full-app feature, qualifying local SD storage is the straightforward
capacity option; it is not currently established. The historical P4 profile
disables both card features, and board qualification records no installed SD
card. SD supplies storage, not extra inference RAM. A RAM-only model transfer
could characterize a standalone compute probe, but is not a self-contained
offline feature after reboot. See [P4 I/O](../p4_io/README.md) and
[board qualification](../board_qualification/README.md).

## Later integration constraints

The standalone result must be UTF-8. Spanish output needs accents, ñ, ¿ and ¡.
The existing G2 keyboard helper in `G2_Page_TextEntry.cpp` deliberately rejects
bytes above ASCII 0x7e, and `System_Dictation` stages raw 256-byte slices without
UTF-8 boundaries. Translation results need a separate bounded text consumer;
feeding them through that keyboard path would corrupt Spanish. Existing G2
display paths already handle UTF-8, but translation display still needs its own
qualification. Preserve both English source text and Spanish output.

When microphone input is added, existing STT contributes 7,172,016 resident
weight bytes plus 2,560,000 PCM bytes. The full translation pack plus those two
allocations totals 31,045,338 bytes, leaving 2,509,094 bytes before both runtimes
and the app. That is not a coexistence budget. Start by finishing recognition,
joining its workers and releasing its model/audio allocations before loading
translation; retain the source text through that handoff. Continuous capture
would require a separately measured design. See [STT cache](../stt_p4/CACHE_RESULTS.md).

No firmware was edited, built, flashed or accessed for this assessment. Next
work is the bounded evaluator and a storage-backed standalone P4 probe, after
host model screening; neither has been implemented here.
