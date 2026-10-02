#!/bin/bash
# LM v2 sweep (2026-10-01): new text (People's Speech, LibriSpeech-360) x size.
# Builds each candidate, then tunes it fully against the deployed acoustic
# checkpoint (run15x5b step 8375; logits cached by tune.py) so the WERs are
# directly comparable with meeting-15x5b.lm (2.5 MB, AMI + Libri-100 text).
set -u
cd "$(dirname "$0")"
P=$HOME/.codex/worktrees/jpeg-portable/hardwareone/experiments/stt_p4/private/export-venv/bin/python
export PYTHONPATH=/Volumes/USB2/stt/pydeps
OUT=/Volumes/USB2/stt/work/lm/v2
CKPT=/Volumes/USB2/stt/work/run15x5b/full/best
LOG=/Volumes/USB2/stt/logs/lm-v2-sweep.log
mkdir -p "$OUT"
# name weights max_vocab target_bytes
CANDIDATES=(
  "A-w60-v40k-2.5M 0.6,0.25,0.15 40000 2500000"
  "B-auto-v40k-2.5M auto 40000 2500000"
  "C-w60-v50k-5M 0.6,0.25,0.15 50000 5000000"
  "D-w60-v65k-8M 0.6,0.25,0.15 65000 8000000"
)
for c in "${CANDIDATES[@]}"; do
  set -- $c
  name=$1 w=$2 v=$3 b=$4
  if [ ! -f "$OUT/$name.lm" ]; then
    echo "$(date +%T) build $name" >> "$LOG"
    nice -n 5 $P build_lm.py --mode mixn --weights "$w" --max-vocab "$v" --target-bytes "$b" \
      --params /Volumes/USB2/stt/work/lm/meeting-15x5b.lm --out "$OUT/$name.raw.lm" \
      --report "$OUT/$name.build.json" >> "$LOG" 2>&1 || { echo "build $name FAILED" >> "$LOG"; continue; }
  fi
  if [ ! -f "$OUT/$name.tune.json" ]; then
    echo "$(date +%T) tune $name" >> "$LOG"
    nice -n 5 $P tune.py --ckpt "$CKPT" --lm "$OUT/$name.raw.lm" --out-lm "$OUT/$name.lm" \
      --workers 2 --report "$OUT/$name.tune.json" >> "$LOG" 2>&1 || echo "tune $name FAILED" >> "$LOG"
  fi
done
echo "$(date +%T) sweep done" >> "$LOG"
