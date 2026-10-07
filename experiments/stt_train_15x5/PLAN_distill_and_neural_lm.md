# Plan: a smaller acoustic model, then a neural language model on the P4

Status: plan (2026-10-02). Nothing here is built yet. Heavy jobs on the Mac mini run one at a
time; every phase below is queued behind the previous one.

## Why

The P4X-EYE has 32 MB of PSRAM and today it is nearly full during transcription:

| Resident during a session | Size |
|---|---:|
| QuartzNet15x5 int8 weights (from the SD card) | ~20 MB |
| Inference working memory (longest phrase) | up to ~5 MB |
| Word trigram LM (`meeting.lm`) | 2.5 MB |
| Session audio queue, drafts, transcript | ~3 MB |
| **Left over** | **~1–3 MB (estimate; to be measured)** |

A neural language model that reads the whole phrase plus the phrase before it (instead of the
trigram's two words) needs ~4–5 MB, so it cannot be added to today's setup. The idea is to make
room first: train the best possible 15x5, then distill it into a QuartzNet5x5 (6.7 M parameters,
~5.5 MB int8 vs ~20 MB, ~2.6x less compute), measure what that costs in accuracy and gains in
speed, and only then spend the freed ~14 MB on the neural LM.

The question each phase answers is whether to continue: there are explicit decision gates.

---

## Phase 0 — Measurement first (small)

1. Flash the PSRAM logging already written in the test tree (`TL warm ... psram_free=`,
   `TL seg ... lm= psram_min=`, `TL lm_released`), run one ordinary Conversate session, and
   record the real headroom with the 15x5. This replaces the estimate in the table above.
2. Fix the comparison basis used in every later phase:
   - `BENCHMARK.md` greedy WER on the five full test sets (AMI far/headset, LibriSpeech, and the
     two simulated-glasses copies);
   - int8 + LM WER on the `tune.py` dev/test subsets (this is what the device actually runs);
   - P4 speed from the `TL seg` lines (inference time ÷ phrase length) and load time.

## Phase 1 — The 15x5 teacher (one clean run)

One run from NVIDIA's original QuartzNet15x5Base-En with everything that worked so far
(all available as `train.py` options):

- data: AMI far-field + headset kept at ≥55% of the mix; People's Speech (filtered),
  LibriSpeech-100/360 and VoxPopuli added on top, not in place of AMI;
- augmentation: MUSAN noise/babble, room impulse responses, band-pass, speed, SpecAugment,
  simulated G2 glasses channel on 30% of clips;
- AdamW 2e-5, warmup, cosine schedule, frozen BatchNorm statistics, rollback guard,
  model selection on dev sets including the glasses-channel copies;
- ~350 h of audio in one pass (the deployed model saw 2 × 175 h), ~26 h on the Mac.

Then quantization-aware fine-tuning (short, final stage), LM retune, export, and a benchmark row.
This model is useful on its own (it replaces today's deployment if it wins) and it is the
teacher for Phase 2.

**Gate 1:** int8 + LM WER of the new 15x5 vs the deployed one. Deploy it if better; either way,
the better of the two is the teacher.

## Phase 2 — Distill the 15x5 into a 5x5

The 5x5 and 15x5 share the input features (64 mel), the frame rate (one output per 20 ms) and the
29-symbol output, so the student can learn directly from the teacher's per-frame probabilities.

- **Student start:** the best existing 5x5 checkpoint (NVIDIA's QuartzNet5x5 or our earlier
  fine-tunes), not random weights.
- **Loss:** `λ · CTC(transcript) + (1 − λ) · T² · KL(teacher_T ‖ student_T)` per frame, with
  temperature T ≈ 1–2 and λ ≈ 0.3–0.5 (tuned on a short trial). The teacher runs in eval mode on
  the same augmented features in the same training step (MPS), so augmentation stays on the fly.
- **Free extra data:** the distillation term needs no transcript, so untranscribed audio can be
  added (spare People's Speech audio, and later real glasses recordings).
- **Budget:** ~350 h of audio; the 5x5 trains ~2.6x faster than the 15x5 but the teacher forward
  pass is added, so ~12–15 h on the Mac.
- Then QAT for the student, LM retune, export, P4 test.

What to expect, honestly: a distilled 5x5 will not match the 15x5. Today the gap between our
earlier 5x5 and the 15x5 is ~6 WER points on AMI with the LM (int8). Distillation typically
recovers a third to a half of such a gap; the first task of this phase is to measure the
starting gap on the Phase 0 basis.

**Gate 2:** report the trade for the decision, all on the same basis:

| | 15x5 (Phase 1) | Distilled 5x5 |
|---|---|---|
| int8 + LM WER (AMI far / headset / Libri) | measured | measured |
| P4 time per phrase (× real time) | ~0.48 | expected ~0.2 |
| Model memory | ~20 MB | ~5.5 MB |
| PSRAM free during a session | Phase 0 | measured |

Continue to Phase 3 only if the 5x5's accuracy loss is one we are willing to trade, because the
neural LM then has to win back more than that loss to be worth it.

## Phase 3 — Neural LM re-scoring

### 3a. Host experiment first (no firmware)

1. **Model:** a small transformer language model trained on the Mac: word-piece vocabulary
   ~2,000, 4 layers, width 256, 4 heads, context up to ~128 tokens (the current phrase plus the
   previous one or two phrases). ~3.5–4 M parameters → ~4 MB int8.
2. **Text:** AMI, People's Speech, LibriSpeech and VoxPopuli transcripts (~8 M words, all
   already on disk). Few hours on the Mac. Compare perplexity with the trigram.
3. **N-best:** extend the Python reference decoder (`hw1lm.py`) to return its top 8 phrase
   candidates with their acoustic and trigram scores.
4. **Re-score:** final score = acoustic + a · neural LM + b · length (+ c · trigram); a, b, c tuned
   on dev, reported on test, with the existing `tune.py` sets and the consecutive-meeting set from
   `context_eval.py` (which is where cross-phrase context can show).

**Gate 3:** ≥ ~1 WER point gain on AMI (int8 logits, with the Phase 2 student) — otherwise stop
here; the firmware work below is the expensive part.

### 3b. Firmware

- C decoder (`stt_lm.cpp`) returns N-best instead of one answer (decoder parity tests with the
  Python reference, as for the existing decoder).
- Export the transformer to ESP-DL int8 (the needed operators — Gather, MatMul, Softmax,
  LayerNormalization, Add, Mul — are in our ESP-DL version); load it from the SD card next to
  `meeting.lm`.
- Run it only on **final** phrases (drafts stay trigram-only), after the acoustic model finishes.
- Cost estimate: 8 candidates × ~25 tokens × ~7 M operations ≈ 1.5 G operations per phrase,
  ~0.5–1 s on the P4. With the 5x5 at ~0.2× real time this stays well within real time.
- Memory: ~4 MB weights + <1 MB working memory, released under PSRAM pressure exactly like the
  trigram LM is today (falls back to the trigram answer, logged as `TL lm_released`).

**Gate 4:** on-device session, `TL` timings and PSRAM low-water mark, then real use.

---

## Timeline (Mac time, one job at a time)

| Step | Mac time | Needs you |
|---|---|---|
| Phase 0 PSRAM measurement | flash + one session | one Conversate session |
| Phase 1 15x5 run + QAT + LM retune + export | ~30 h | flash + test |
| Phase 2 distillation trial + full run + QAT + export | ~16 h | flash + test, decision |
| Phase 3a neural LM training + host re-scoring | ~5 h | decision |
| Phase 3b firmware | development work | flash + test |

## Risks

- The distilled 5x5 may lose more accuracy than a neural LM can win back (Gate 2 decides).
- Much of today's error is acoustic (sounds misheard); a language model only re-ranks what the
  acoustic model offers, so the neural LM's gain is capped by the N-best list.
- PSRAM numbers are estimates until Phase 0.
- All accuracy numbers are AMI/LibriSpeech stand-ins. Real glasses recordings (saved to the SD
  card and transcribed on the Mac) would be the better yardstick for every gate.

## Not in this plan (already measured, no benefit)

Cross-phrase trigram context, a larger trigram LM, Citrinet-384, NovoGrad / higher learning rates.
