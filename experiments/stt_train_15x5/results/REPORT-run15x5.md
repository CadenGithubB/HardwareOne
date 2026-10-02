# QuartzNet15x5 vs QuartzNet5x5: like-for-like fine-tuning (run2 recipe)

Greedy CTC WER on the full held-out test sets (no LM).

| Test set | 5x5 original | 5x5 run2 (on P4) | 15x5 original | 15x5 fine-tuned |
|---|---:|---:|---:|---:|
| ami-sdm-test | 79.2% | 68.7% | 60.3% | 48.8% |
| ami-ihm-test | 53.3% | 43.0% | 31.5% | 22.4% |
| librispeech-test-clean | 5.6% | 7.5% | 3.8% | 5.3% |

## Training

- Recipe: run2's mix [['ami-sdm-train', 0.35, True], ['ami-ihm-train', 0.35, False], ['librispeech-train-clean-100', 0.3, False]], AdamW lr 2e-05, warmup 300, frozen BN stats, effective batch 110 s / 32 items as 4 micro-batches of 27.5 s / 8 items, target 175.0 audio hours.
- Stopped by: target_reached at step 8877, 175.01 audio hours.
- Best dev checkpoint: step 8877 (score 0.2369); finished 05:55:05 after 8877 optimizer steps.
- Rollbacks: 0, memory restarts: 0.

### Dev curve (400 utts per set; score = mean WER)

| Model | step | audio h | ami-sdm-val | ami-ihm-val | libri-dev-clean | score |
|---|---:|---:|---:|---:|---:|---:|
| 5x5 run2 | 0 |  | 72.7% | 52.5% | 5.2% | 0.4349 |
| 5x5 run2 | 552 |  | 71.7% | 51.9% | 8.1% | 0.4391 |
| 5x5 run2 | 1698 |  | 66.9% | 46.5% | 8.8% | 0.4071 |
| 5x5 run2 | 2843 |  | 67.0% | 45.4% | 8.4% | 0.4027 |
| 5x5 run2 | 3987 |  | 64.2% | 43.4% | 8.3% | 0.3865 |
| 5x5 run2 | 5135 |  | 64.3% | 41.8% | 7.6% | 0.3790 |
| 5x5 run2 | 6285 |  | 63.0% | 41.9% | 7.2% | 0.3735 |
| 5x5 run2 | 7484 |  | 62.7% | 40.8% | 6.9% | 0.3683 |
| 5x5 run2 | 7557 |  | 63.1% | 40.9% | 6.8% | 0.3693 |
| 15x5 | 0 | 0.0 | 51.8% | 31.2% | 3.9% | 0.2898 |
| 15x5 | 644 | 12.62 | 50.2% | 28.6% | 7.6% | 0.2880 |
| 15x5 | 1968 | 38.83 | 49.5% | 27.9% | 8.1% | 0.2849 |
| 15x5 | 3305 | 65.04 | 47.7% | 27.7% | 7.8% | 0.2772 |
| 15x5 | 4630 | 91.25 | 46.9% | 25.1% | 6.9% | 0.2630 |
| 15x5 | 5965 | 117.45 | 45.9% | 25.4% | 5.9% | 0.2569 |
| 15x5 | 7292 | 143.66 | 43.6% | 22.6% | 5.7% | 0.2399 |
| 15x5 | 8618 | 169.86 | 43.4% | 22.8% | 5.8% | 0.2401 |
| 15x5 | 8877 | 175.01 | 43.2% | 22.4% | 5.5% | 0.2369 |

## Cost

Acoustic model only, one utterance at a time on the M1 CPU (60 dev utterances, 200.3 s of audio).

| Model | params | CPU s per audio s (1 thread) | CPU s per audio s (all threads, wall) | int8 size estimate |
|---|---:|---:|---:|---:|
| 5x5 | 6.7 M | 0.0131 | 0.0524 | 5.5 MB |
| 15x5 | 18.9 M | 0.0344 | 0.1568 | 15.5 MB |

P4 fit: LittleFS is 9.86 MB and currently holds the 5.5 MB 5x5 model + 2.5 MB LM. The int8 estimate is the deployed 5x5 file scaled by BN-folded parameter count, so the 15x5 does not fit as is (it would need a bigger partition/SD card or much heavier compression), and it costs ~3x the compute per second of audio.

## Provenance

- Checkpoint: https://api.ngc.nvidia.com/v2/models/nvidia/nemospeechmodels/versions/1.0.0a5/files/QuartzNet15x5Base-En.nemo
- sha256 1fae09384a4285d941b57a185a05ef847e2c374056cfc3e37460923c62599cd1, 71087043 bytes (QuartzNet15x5Base-En, NGC nvidia/nemospeechmodels 1.0.0a5, same listing as the 5x5 QuartzNet5x5LS-En; trained on LibriSpeech, Common Voice, WSJ, Fisher, Switchboard, NSC).
- Code: experiments/stt_train_15x5 (copy of experiments/stt_train with run2 mix, audio-hour budget, gradient accumulation).

## Example transcripts (15x5 fine-tuned)

### ami-sdm-test
- ref: okay
  hyp: 
- ref: yeah yeah
  hyp: yeah
- ref: exactly
  hyp: tat
- ref: yeah
  hyp: yeah
- ref: yeah
  hyp: yeah
- ref: okay
  hyp: okay
- ref: yeah
  hyp: yah
- ref: but yeah
  hyp: yeah
- ref: okay
  hyp: kay
- ref: thank you
  hyp: 
- ref: mm hmm
  hyp: m
- ref: yeah yeah
  hyp: yeah yeah
### ami-ihm-test
- ref: cutest
  hyp: 
- ref: think we do
  hyp: thin we do
- ref: right
  hyp: right
- ref: okay
  hyp: okay
- ref: i mean
  hyp: i mean
- ref: mm
  hyp: m
- ref: that
  hyp: ne
- ref: yeah
  hyp: yeah
- ref: exactly
  hyp: jactly
- ref: um
  hyp: um
- ref: mm hmm
  hyp: mm hm
- ref: yes
  hyp: yesh
### librispeech-test-clean
- ref: a story
  hyp: a story
- ref: direction
  hyp: direction
- ref: verse two
  hyp: first two
- ref: oh emil
  hyp: oh amil
- ref: indeed ah
  hyp: indeed ah
- ref: farewell madam
  hyp: farewell madame
- ref: poor alice
  hyp: poor alice
- ref: there just in front
  hyp: there just in front
- ref: hans stirs not
  hyp: hans sters not
- ref: venice
  hyp: venice
- ref: marie sighed
  hyp: marie sighed
- ref: what was that
  hyp: what was that
