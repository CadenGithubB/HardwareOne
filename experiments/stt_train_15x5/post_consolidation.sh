#!/bin/bash
# After the SSD consolidation (consolidate_exfat.sh), one job at a time on the
# Mac, all reading and writing the SSD (STT_ROOT=/Volumes/Audiodrive/stt):
#  1. benchmark table from the SSD (sanity: cached results visible)
#  2. DEMAND noise stores; People's Speech +60 shards; Common Voice train/test
#  3. noisy test sets and the glasses copy of the held-out noise
#  4. every model scored on the new columns (Common Voice, noisy speech, phantom words)
#  5. session-cache sweep (bonus and word-rarity threshold) on the deployed model
#  6. the A4000 kit on the SSD (code snapshot + scripts) and a short smoke test of
#     its training path on this Mac (all new stores, noise options, no rollback)
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
cd "$HERE"
export STT_ROOT=/Volumes/Audiodrive/stt
export PYTHONPATH=$STT_ROOT/pydeps
export PYTORCH_ENABLE_MPS_FALLBACK=1
P=${HW1_STT_PYTHON:-$PWD/../stt_p4/private/export-venv/bin/python}
LOG=$STT_ROOT/logs/post-consolidation.log
KIT=/Volumes/Audiodrive/hw1-stt-kit
REPO=$(cd "$HERE/../.." && pwd)
mkdir -p "$STT_ROOT/logs"
say() { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }

say "=== queued (pid $$): waiting for the verified consolidation (marker $STT_ROOT/.consolidation-verified) ==="
until [ -f "$STT_ROOT/.consolidation-verified" ]; do sleep 60; done

say "1. benchmark table from the SSD"
$P benchmark.py >> "$LOG" 2>&1 && grep -q '23.11' BENCHMARK.md && say "   table OK (cached results found on the SSD)" \
  || { say "   benchmark table check FAILED"; exit 1; }

say "2. new stores"
$P prepare_noise.py >> "$LOG" 2>&1 || say "   DEMAND FAILED"
(cd ../stt_train && $P prepare_data.py peoples --store peoples-clean-train2 --shards new --max-hours 500) >> "$LOG" 2>&1 \
  || say "   People's Speech 2 FAILED"
(cd ../stt_train && $P prepare_data.py commonvoice --split train --max-hours 300) >> "$LOG" 2>&1 || say "   Common Voice train FAILED"
(cd ../stt_train && $P prepare_data.py commonvoice --split dev --max-hours 50) >> "$LOG" 2>&1 || say "   Common Voice test FAILED"
grep -E "items, .* h$|^kept" "$LOG" | tail -8 | tee -a "$LOG" >/dev/null

say "3. noisy test sets"
$P make_noisy_stores.py librispeech-test-clean peoples-clean-test >> "$LOG" 2>&1 || say "   noisy stores FAILED"
$P make_g2_stores.py demand-noise-test >> "$LOG" 2>&1 || say "   noise g2 copy FAILED"

say "4. benchmark: new columns for every model"
$P benchmark.py --run >> "$LOG" 2>&1 || say "   benchmark FAILED"

say "5. session-cache sweep (deployed general model gen-17868)"
L=$STT_ROOT/work/lm
for rare in -4.0 -3.5 -4.5; do
  (cd ../stt_train/lm && $P session_cache_eval.py --ckpt $STT_ROOT/work/general/quartznet15x5/best \
      --lm $L/meeting-gen-17868.lm --meetings 10 --segments 300 --rare $rare --bonuses 1,2,3,4,6 \
      --out $L/context/session-cache-gen-rare$rare.json) >> "$LOG" 2>&1 || say "   sweep rare $rare FAILED"
done

say "6. A4000 kit"
mkdir -p "$KIT/experiments" "$KIT/components/hardwareone_libs"
for d in experiments/stt_train experiments/stt_train_15x5; do
  rsync -rt --delete --exclude build/ --exclude __pycache__/ --exclude '*.pyc' --exclude kit/ \
    "$REPO/$d/" "$KIT/$d/" >> "$LOG" 2>&1 || say "   copy $d FAILED"
done
rsync -rt --delete "$REPO/components/hardwareone_libs/liblc3/" "$KIT/components/hardwareone_libs/liblc3/" >> "$LOG" 2>&1
cp "$HERE"/kit/setup_ubuntu.sh "$HERE"/kit/run_continuation.sh "$HERE"/kit/requirements.txt "$HERE"/kit/README.md "$KIT/"
dot_clean -m "$KIT" 2>/dev/null; dot_clean -m "$STT_ROOT" 2>/dev/null
say "   kit at $KIT"
say "   smoke test: the kit's training path, 20 steps on this Mac"
rm -rf "$STT_ROOT/work/kit-smoke"
(cd "$KIT/experiments/stt_train_15x5" && $P train.py --init $STT_ROOT/work/general/quartznet15x5/best \
    --out $STT_ROOT/work/kit-smoke --deadline +1h --mix general2 --dev broad --g2-prob 0.3 \
    --noise-pool musan+demand --noise-only-share 0.04 --rollback off --lr 1e-5 --accum 4 \
    --benchmark-steps 20) >> "$LOG" 2>&1 && say "   smoke test OK: $(grep '"benchmark"' $STT_ROOT/work/kit-smoke/train.jsonl)" \
  || say "   smoke test FAILED"
say "7. remount the SSD (drops cached copies), then re-read every new store against its write checksum"
DEV=$(diskutil info /Volumes/Audiodrive | awk -F': *' '/Device Node/{print $2}')
for i in 1 2 3 4 5; do diskutil unmount /Volumes/Audiodrive >> "$LOG.remount" 2>&1 && break; sleep 30; done
diskutil mount "$DEV" >> "$LOG.remount" 2>&1; sleep 5
[ -d "$STT_ROOT/stores" ] || { echo "SSD did not come back at /Volumes/Audiodrive" >> "$LOG.remount"; exit 1; }
cat "$LOG.remount" >> "$LOG"; rm -f "$LOG.remount"
$P verify_stores.py >> "$LOG" 2>&1 && say "   all new stores read back intact" || say "   STORE VERIFY FAILED (see BAD lines)"
say "=== post-consolidation finished ==="
