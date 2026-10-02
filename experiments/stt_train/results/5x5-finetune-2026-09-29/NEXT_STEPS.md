# STT fine-tuning: follow-ups

## If dev WER plateaus: change the training material (user request, 2026-09-29)
Check run1/train.jsonl eval events. Plateau = best score improves <1% relative over 3 evals.
Then, starting from the best checkpoint (never from scratch):
1. Look at which dev set is worst/stuck (ami-sdm = distant room mic, ami-ihm = headset, libri = clean).
2. Shift the mix toward the weak domain, but keep >=20% LibriSpeech so general English isn't forgotten.
3. Add new material rather than only repeating: candidates (licence check first)
   - TED-LIUM 3 (talks, varied speakers), VoxPopuli EN (parliament, accents), Common Voice EN (many voices/mics),
   - AMI multi-channel array (other distant mics), ICSI meetings,
   - the user's own meeting recordings with corrected transcripts (best signal for their voices/room).
4. Stronger augmentation (MUSAN noise/babble, real RIRs) once they're prepared.
5. Short run with a lower LR (~5e-5); keep it only if dev WER beats the previous best.
Risk: a big sudden switch can make it forget what it learned; mixing old + new avoids that.

## Rollback
Every eval is saved under run1/evals/step_NNNNNN (with metrics.json). Any of them can be exported,
or used to resume (train.py --init <that dir> --out run2 ...). The trainer also auto-restores the best
weights and halves the LR after 2 worse evals in a row.

## LM decoder (built 2026-09-29 night)
- Host: experiments/stt_train/lm/ (build_lm.py, tune.py, hw1lm.py reference, README.md). Shipped LM:
  /Volumes/USB2/stt/work/lm/meeting.lm (2.5 MB; alpha .45 beta 2 beam 32). Baseline model: 5-9% rel WER gain.
- Firmware: components/hardwareone/stt/stt_lm.{h,cpp}, ENABLE_STT_LM, loads /STT Models/meeting.lm (+custom_words.txt);
  falls back to greedy if absent. Host tests stt_lm_tests + stt_lm_parity pass.
- Morning: retune for the new checkpoint:
  nice -n 15 $P lm/tune.py --ckpt /Volumes/USB2/stt/work/run1/best --lm /Volumes/USB2/stt/work/lm/meeting.lm --out-lm /Volumes/USB2/stt/work/lm/meeting-run1.lm
- Unresolved: STTLocalStats has no "LM used" field; measure decode time on the P4; LittleFS space (9.86 MB) for 5.5 MB model + 2.5 MB LM.

## Other pending
- Export best -> espdl, recalibrate int8 quantisation with more clips, sync the P4 build copy
  (experiments/stt_p4/private/app-p4-mic) with stt_lm + runtime changes, build, flash P4, upload meeting.lm.

## Night of 2026-09-29: what happened
- run1 (AdamW lr 2e-4) wrecked the model: conv weights moved 50-100% (weight RMS is only ~0.05); clean WER 5% -> 53%.
  Stopped at 02:22. The first eval said "new best" only because no step-0 baseline existed; that is fixed now (step-0 eval).
- Also fixed: MPS memory leak (graph cache per batch shape -> fixed bucketed shapes, CPU eval, 6 GB self-restart).
- run2 (lr 2e-5, frozen BN stats) from 02:25. At step 1698: sdm 72.7->66.9, ihm 52.5->46.5, libri 5.2->8.8.
- Trade-off to fix next: clean speech degrades (spelling-type substitutions). Try: more LibriSpeech weight (0.3 -> 0.5),
  lower augmentation strength on the clean share, or add other clean/varied corpora; resume from run2/best.

## 2026-09-29 morning: deployed to the cased P4 (MAC 02:48:57:31:04:73)
- Full flash backup before changes: /Volumes/USB2/p4-backups/p4-024857310473-20260929-0633-full.bin
- Firmware: headless (web UI off to fit the LM decoder; features private/features-p4-noweb.h), LM decoder,
  new model identity (run2 step 7484), G2 fix: lens DISPLAY_OFF no longer cancels a running transcription.
- LittleFS: /STT Models/quartznet5x5.p4.stt (run2) + meeting.lm (unk_log10 -5, hotword 3; -7.5 glued OOV words).
- Speaker->PDM test (8 held-out clips): 44.7% WER; read speech near perfect, meeting clips weak.
- Boot log: ring 02:48:57:31:02:1C bond removed on SMP failure (ring likely bonded to the other P4) -> re-pair.
- TODO: STTLocalStats has no "LM used" flag; merge G2 fix + LM into main properly; retune LM for run3.
