# STT fine-tuning (QuartzNet 5x5)

Host-side fine-tuning of the QuartzNet 5x5 speech model that runs on the
P4X-EYE, aimed at meetings with several speakers, plus the word language
model used by on-device decoding. Training runs on a Mac with the export
environment from `experiments/stt_p4`; data lives on an external drive.

| File | Purpose |
|---|---|
| `download.sh` | Resumable downloads of the training corpora (AMI, LibriSpeech and others from OpenSLR / Hugging Face) |
| `prepare_data.py` | Converts corpora into flat int16 stores for fast random access |
| `train.py`, `data.py`, `common.py` | Fine-tuning until a wall-clock deadline, keeping the best checkpoint by held-out WER |
| `evaluate.py` | Greedy-CTC word error rate of a checkpoint on prepared test sets |
| `run_overnight.sh`, `run_day.sh` | Unattended pipelines: prepare, baseline, train, report |
| `lm/` | Word n-gram language model builder, tuner and parity tools (format in `lm/FORMAT.md`) |
| `results/` | Reports from completed runs |

## Results

[5x5 fine-tune, 2026-09-29](results/5x5-finetune-2026-09-29/REPORT.md):
word error rate on AMI single distant mic fell from 79.2% to 68.7% and on AMI
headset from 53.3% to 43.0%, while LibriSpeech clean rose from 5.6% to 7.5%.
[REPORT-run3.md](results/5x5-finetune-2026-09-29/REPORT-run3.md) is the third
run and [NEXT_STEPS.md](results/5x5-finetune-2026-09-29/NEXT_STEPS.md) the plan
that followed, including the move to the 15x5 model in `experiments/stt_train_15x5`.

Paths in the scripts assume the data drives are mounted at `/Volumes/USB` and
`/Volumes/USB2`.
