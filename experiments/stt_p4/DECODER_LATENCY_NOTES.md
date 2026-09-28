# Decoder and input-length diagnostics

Offline follow-up to milestone `512f201`, 2026-09-28. These results support keeping the existing greedy decoder. They do not justify changing microphone gain or the eight-second minimum segment span for accuracy.

The inputs are four saved synthetic sentences containing 50 reference words, recorded in [heldout-provenance.json](heldout-provenance.json). The two original calibration clips are evaluated separately. Both diagnostics replayed the saved greedy baseline and reproduced both original quantized output fixtures byte for byte. No model, calibration, firmware, microphone recording, or hardware setting changed.

## Decoder search

[evaluate_ctc_beam.py](evaluate_ctc_beam.py) compares greedy decoding with CTC prefix-beam widths 1, 4, 8, 16, 32, and 64, using all 29 classes and no language model, lexicon, length bonus, or token pruning. Prefix search sums alternate blank/nonblank alignments for each text; greedy decoding selects the highest-scoring alignment. See the primary explanation in [Sequence Modeling With CTC](https://distill.pub/2017/ctc/). The implementation also agrees with exhaustive three-class enumeration in four short test cases to within `1e-12` log probability.

Every tested width retained **8 errors / 50 words**, for both float and frozen int8 inference. The only substantive top-result change was “camaris” to “camaries”; neither recovered the synthetic reference “camera is.” On the two calibration clips, every width likewise retained the existing float 1 / 66 and int8 2 / 66 errors.

Exact CTC sequence scoring places the four correct references below the erroneous int8 winners by 2.69, 8.21, 31.16, and 6.47 natural-log probability units. A wider search under these unchanged scores therefore cannot prefer those references. This points to model scoring on these inputs, rather than greedy search, as the main remaining issue. It does not rule out a language model or other model improvements, which were not tested. Four synthetic sentences are too few for a general accuracy claim.

## Input amplitude and trailing silence

[evaluate_input_sensitivity.py](evaluate_input_sensitivity.py) scales the same int16 PCM by 1, 1/16, and 1/64, rounds to nearest-even int16, then appends zero, two, or four seconds of exact digital silence. All 36 captures remain below 20 seconds; the longest is 7.978 seconds. The portable frontend, weights, and exported quantization scales remain frozen.

Each cell below is **float / int8 word errors out of the same 50 words**. The conditions are repeated measurements of those words, not independent additional test material.

| Input amplitude | No added silence | +2 seconds | +4 seconds |
| --- | ---: | ---: | ---: |
| 1 | 8 / 8 | 8 / 8 | 8 / 8 |
| 1/16 | 8 / 8 | 8 / 7 | 8 / 8 |
| 1/64 | 7 / 8 | 6 / 7 | 10 / 6 |

No condition clipped any samples. At 1/64 amplitude the original audio spans have RMS 59.7–93.0, absolute peaks 396–410, and 95.0–96.4% nonzero samples. There is no consistent collapse at low amplitude or benefit from added silence. Choosing the best cell would tune to this tiny set rather than demonstrate an accuracy improvement.

Scaling also scales the source noise, preserving its acoustic signal-to-noise ratio before rounding and frontend dither. Exact appended zeros are not room tone. These results cannot establish a suitable physical microphone gain, the detection threshold, or recognition performance near the measured room-noise floor. Utterance normalization also changes when silence is appended, so padding is not numerically neutral.

## Latency implication

The frontend emits `T = ceil(samples / 160)` frames; runtime input shape is `[1,T,1,64]` and model output is `[1,ceil(T/2),1,29]`.

| Added silence | Feature frames across the four clips | Output frames |
| --- | --- | --- |
| 0 seconds | 388, 364, 398, 396 | 194, 182, 199, 198 |
| 2 seconds | 588, 564, 598, 596 | 294, 282, 299, 298 |
| 4 seconds | 788, 764, 798, 796 | 394, 382, 399, 398 |

Four added seconds cost 400 feature frames and 200 output frames, approximately doubling temporal convolution work for these roughly four-second clips. This is a shape/work comparison, **not measured P4 latency**. A useful next hardware experiment is to measure endpoint waiting, model loading, and inference separately before changing minimum segment duration, while preserving speech tails and verifying the continuous queue still keeps up.

## Reproduction and evidence

From the repository root, using the existing isolated export environment and saved private fixtures, run with new empty output directories:

```sh
PYTHONDONTWRITEBYTECODE=1 experiments/stt_p4/private/export-venv/bin/python \
  experiments/stt_p4/evaluate_ctc_beam.py \
  --output experiments/stt_p4/private/ctc-decoder-audit-repeat
PYTHONDONTWRITEBYTECODE=1 experiments/stt_p4/private/export-venv/bin/python \
  experiments/stt_p4/evaluate_input_sensitivity.py \
  --output experiments/stt_p4/private/input-sensitivity-repeat
```

The scripts verify pinned model artifacts and fixture parity before accepting results. Reports include model, frontend, source-script, fixture, and output hashes. Timing fields vary on rerun. Saved private evidence remains ignored by Git; this report contains no private acoustic transcript.

Pinned identities:

- ESP-DL model SHA-256: `8a6766f69597e319c585613e94faba97dd2054a8d527fd21b1aa7d408981b925`
- Frontend contract SHA-256: `ca91eb9683f7151b8be1a01532a6790c6eb7d515f67169a207751e3fc48cc58b`

Completed evidence, relative to this directory:

| Artifact | SHA-256 |
| --- | --- |
| `private/ctc-decoder-audit-v2/results.json` | `a34f499f492acdb8dfd5a5e65b52b098eb7c811eeeabd31736d0d2bc8b091cc5` |
| `private/ctc-decoder-audit-v2/logits.npz` | `289a45487fd949c8014114ea55f7bf111ac908ad7d5109d4a3566860fd586157` |
| `private/input-sensitivity-v1/results.json` | `860c58b3cafd254a34d894201da3bc411555c9f71c295ee7017451b0474e0f02` |
