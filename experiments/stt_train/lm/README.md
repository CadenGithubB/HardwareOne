# Word n-gram LM for the on-device QuartzNet decoder

Host-side tools for the `HW1LM1` trigram LM and CTC prefix beam decoder.
`FORMAT.md` is the binding contract shared with the firmware decoder
(`components/hardwareone/stt/`); everything here follows it.

| file | purpose |
|---|---|
| `FORMAT.md` | file layout, scoring and decoder contract (do not change unilaterally) |
| `hw1lm.py` | writer, reader/validator (magic, sizes, sort order, SHA-256), backoff scoring, reference decoder `decode(log_probs, lm, custom_words, **overrides)` |
| `build_lm.py` | transcripts -> modified Kneser-Ney trigrams (AMI and LibriSpeech separately) -> static mixture -> ARPA backoff form -> Stolcke entropy pruning to a byte budget -> `.lm` |
| `tune.py` | caches checkpoint logits on fixed dev/test subsets, searches decoder params on dev, writes them into the LM header, reports test WER |
| `make_fixtures.py` | firmware parity fixtures in `components/hardwareone/test/host/fixtures/stt_lm/` |
| `test_hw1lm.py` | unit tests: hand-made backoff LM, format validation, exhaustive CTC oracle, KN normalisation |

Environment used for everything below (CPU only):

```sh
P=~/.codex/worktrees/jpeg-portable/hardwareone/experiments/stt_p4/private/export-venv/bin/python
export PYTHONPATH=/Volumes/USB2/stt/pydeps
cd experiments/stt_train/lm
```

## Rebuild the LM

```sh
nice -n 15 $P build_lm.py --out /Volumes/USB2/stt/work/lm/meeting.lm \
    [--target-bytes 2500000] [--custom-words words.txt] [--params tuned.lm]
```

* Training text is **only** AMI train (sdm transcripts; ihm has the same
  transcripts) and LibriSpeech train-clean-100, normalised like
  `common.normalize_text`. Dev text (AMI validation) is used only for
  perplexity reporting and `--ami-weight auto`.
* Vocabulary: every training word (38,375 words; OOV rate 0.78% on AMI dev,
  2.3% on LibriSpeech dev), plus `--custom-words`. Use `--max-vocab N` for a
  smaller vocabulary (the rest map to `<unk>`).
* Weighting: two separate KN models, mixed `0.8*AMI + 0.2*Libri`
  (`--ami-weight`), then converted back into backoff form with renormalised
  backoff weights. The results table below explains the choice.
* Pruning: Stolcke relative-entropy pruning with one threshold for both
  orders. The threshold is bisected until the file fits `--target-bytes`. A
  bigram that is the context of a kept trigram is always kept.
* Decoder params come from `--params` (a tuned `.lm` or JSON) or the
  `--alpha ... --hotword-bonus` flags. Build the n-grams first and tune
  afterwards: `tune.py` rewrites only the header, and the SHA-256 trailer is
  recomputed.

## Retune for a new checkpoint (for example the fine-tuned one)

```sh
nice -n 15 $P tune.py --ckpt /Volumes/USB2/stt/work/run1/best \
    --lm /Volumes/USB2/stt/work/lm/meeting.lm --out-lm /Volumes/USB2/stt/work/lm/meeting-run1.lm
```

* The subsets are fixed and seeded, so every checkpoint sees the same
  utterances: AMI sdm validation 200, AMI ihm validation 150, LibriSpeech
  dev-clean 100 for tuning, and AMI sdm test 200, ihm test 150, LibriSpeech
  test-clean 100 for reporting only.
* Logits are computed on the CPU with 2 threads and no dither, then cached in
  `/Volumes/USB2/stt/work/lm/logits/<ckpt>-<sha12>/`. This takes about 4 min
  per checkpoint.
* Search (dev only). The objective is `0.4*sdm + 0.4*ihm + 0.2*libri` WER.
  1. alpha x beta x `unk_log10` grid (5 x 5 x 4), run at a fixed
     `--beam-width 32 --char-prune -7`.
  2. Coordinate refinement from the best three grid points.
  3. `hotword_bonus` from a simulated custom list: the 63 LM-OOV words that
     occur in the dev references plus 193 rare in-vocabulary decoys that never
     occur, applied to every dev utterance so that false insertions count
     against it. The smallest bonus within 0.1% absolute of the best is chosen.
  4. A beam/prune sweep, reported for information only. The beam is not
     chosen by dev WER: the objective is noisy at about +/-0.3, and picking it
     that way fitted noise (see below).
* Search time is about 7 minutes with 2 workers.
* The report goes to `/Volumes/USB2/stt/work/lm/tune/<ckpt>-<sha12>.json`. It
  holds every search row, test WER, WER with int8-quantised logits (the device
  output scale 2^-2), a cost estimate and example transcripts.
* The LM n-grams do not depend on the checkpoint. Only the header params
  change.

## Regenerate the parity fixtures

```sh
nice -n 15 $P make_fixtures.py --params /Volumes/USB2/stt/work/lm/meeting.lm
nice -n 15 $P test_hw1lm.py
```

## Results: baseline checkpoint (`stt-quartznet`, weights sha 51a92f946c8c)

Shipped LM: `/Volumes/USB2/stt/work/lm/meeting.lm`

| property | value |
|---|---|
| size | 2,499,996 bytes (default budget 2.5 MB, set by the LittleFS space on the P4) |
| vocabulary | 38,375 words |
| n-grams | 147,292 bigrams, 85,541 trigrams |
| n-grams before pruning | 496,180 bigrams, 1,170,383 trigrams (14.0 MB) |
| perplexity | 88.0 on AMI dev (OOV 0.78%), 739 on LibriSpeech dev (OOV 2.33%); OOV tokens excluded |

Header parameters, tuned on dev: `alpha 0.45, beta 2.0, unk_log10 -6.5,
beam_width 32, char_prune -7, hotword_bonus 3`.

WER, in %. "int8" decodes device-like int8 logits (scale 2^-2, no clipping
occurs). The 4 MB LM was tuned separately and landed on the same parameters.

| set | utts / words | greedy | LM 2.5 MB | LM 2.5 MB, int8 | LM 4 MB |
|---|---|---:|---:|---:|---:|
| ami-sdm-dev | 200 / 1852 | 73.49 | 66.90 | 67.60 | 66.95 |
| ami-ihm-dev | 150 / 1154 | 50.52 | 45.41 | 45.67 | 45.58 |
| libri-dev | 100 / 2109 | 4.74 | 4.46 | 4.41 | 4.31 |
| **ami-sdm-test** | 200 / 1794 | 77.20 | **72.35** | 72.58 | 72.30 |
| **ami-ihm-test** | 150 / 1316 | 52.43 | **47.57** | 47.34 | 47.72 |
| **libri-test** | 100 / 1817 | 4.18 | **3.96** | 3.96 | 4.07 |

On test, the LM cuts WER by 6.3% relative on AMI sdm, 9.3% on AMI ihm and
5.3% on LibriSpeech. The absolute gains are limited by the acoustic model:
the baseline was trained on LibriSpeech and gets 73-77% greedy WER on the
distant AMI microphone. Retune after fine-tuning.

**The 2.5 MB cap costs nothing measurable.** The 4 MB, 2 MB and unpruned
14 MB LMs are all within noise of 2.5 MB.

### Mixing AMI and LibriSpeech text

Dev objective, lower is better; greedy is 50.55. Each LM gets its own
alpha/beta grid around the tuned point.

| LM | dev objective | AMI dev ppl | Libri dev ppl |
|---|---:|---:|---:|
| **mixture 0.8 AMI + 0.2 Libri, 2.5 MB (shipped)** | **45.81** | 88.0 | 739 |
| mixture, weight by EM on AMI dev (0.944), 2.5 MB | 45.88 | 85.4 | 1162 |
| mixture 0.8, words seen at least twice (24.4k vocab), 2.5 MB | 45.84 | 85.4* | 661* |
| AMI only, 4 MB | 45.74 | 85.0 | 1660 |
| LibriSpeech only, 4 MB | 47.37 | 808 | 359 |

\* Vocabulary differs, so these perplexities are not comparable with the
other rows.

* Pooled counts, with AMI repeated 1x or 4x, were clearly worse in an
  earlier beam-16 comparison: 47.10 and 47.00, against 46.65-46.86 for the
  mixtures. Repeating text breaks the Kneser-Ney count-of-counts discounts.
  AMI dev perplexity goes *up* from 98 to 126 at 4x.
* Every mixture weight from 0.7 to 0.944, and AMI-only, falls within +/-0.15 of
  each other, which is noise at this dev size. Meeting text is worth about
  1.5 points compared with LibriSpeech-only.
* 0.8 was chosen because AMI train and dev share the same scenario topics
  (remote-control design). The EM weight fits those topics, while 0.8 keeps
  general-English perplexity much lower (739 vs 1162) at no measurable cost.

### Beam, prune and hotword sweeps (dev objective, information only)

| beam_width | 4 | 8 | 16 | 24 | **32** | 48 | 64 |
|---|---:|---:|---:|---:|---:|---:|---:|
| objective | 48.03 | 47.17 | 46.58 | 46.06 | **45.82** | 46.04 | 46.08 |

`char_prune` at beam 32: -5 gives 45.85; -7, -10 and -14 are identical at
45.82. -7 is therefore lossless and has half the expansions of -10.

A first tune picked the beam by dev WER and chose beam 16 at other
alpha/beta values, where wider beams looked *worse*. That was noise-fitting,
so the beam is now fixed at 32.

| hotword_bonus | 0 | 1 | 2 | **3** | 4 | 6 | 8 |
|---|---:|---:|---:|---:|---:|---:|---:|
| objective | 45.81 | 45.80 | 45.71 | **45.68** | 45.67 | 45.67 | 45.60 |
| hits (of 65) | 26 | 28 | 31 | 34 | 35 | 35 | 36 |
| false insertions | 0 | 0 | 0 | 1 | 1 | 1 | 1 |

The curve is flat above 3. Real lists may name short or common-sounding
words, so the smaller bonus is kept.

### Decode cost

| decoder | cost per second of audio |
|---|---|
| Python reference (M1, beam 32) | 2.6 ms CPU |
| firmware C++ decoder, built for the host (M1, -O2) | 0.14 ms (`stt_lm_tool`) |

* At beam 32 / prune -7 the search makes about 7.4k beam expansions per
  second of audio: 50 frames/s x 32 beams x 4.6 labels, counting blank and
  the repeat.
* A 400 MHz P4 core should need well under 1% of real time. That figure is
  an extrapolation, not a measurement.

### Parity

`make_fixtures.py` writes 6 cases:
* 4 real int8 dev utterances (AMI sdm, AMI ihm, LibriSpeech, and one
  custom-word case that is also checked without the list).
* All-blank/spaces.
* Leading and double spaces, repeated letters, an apostrophe, an OOV word and
  an in-vocabulary custom word.

Every case keeps at least a 0.3 nat margin between the top two final beams
and passes a 2e-4 nat jitter test.

The firmware decoder built on the host (`stt_lm_tool` + `test_stt_lm_parity.py`)
matches 6/6 cases. It also matched the Python reference on all 450 dev
utterances with the real `meeting.lm`, for both LM output and device greedy.

## FORMAT.md readings (implicit in the spec, implemented this way)

* `char_prune` applies to every non-blank label except the repeat of the
  prefix's last character. A space after a space is that repeat, so it is not
  pruned; it is added to `p_b` like a blank. A leading space is pruned like
  any other label.
* The returned text is the best prefix with the trailing space trimmed. The
  finish tie-break is the same as the per-frame one.
* The unigram entry for `<s>` stores -32.768 (the i16 minimum), because -99
  does not fit. It is never scored.
* Custom-word lines are stripped and normalised like transcripts, and kept if
  they form exactly one word of 1-31 bytes; duplicates are dropped and the
  first 256 are kept. The fixture list is already normalised, so both
  readings agree on it.
* Vocabulary words are capped at 31 bytes by the builder; the longest actual
  word is 19 bytes.
