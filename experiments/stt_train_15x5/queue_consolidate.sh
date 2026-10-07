#!/bin/bash
# Consolidate STT data onto Audiodrive (2026-10-03), after the general run:
#  1. wait for run_general.sh to log "general pipeline finished"
#  2. copy USB2/stt (stores, work, models, musan, rirs, pydeps, logs) to
#     /Volumes/Audiodrive/stt (USB/stt was copied and verified 2026-10-03 06:40)
#  3. re-point the dev-view audio symlinks (absolute USB2 paths) relatively
#  4. full checksum comparison of both halves (rsync -c dry run; prints any
#     difference). Nothing is deleted anywhere.
set -u
DST=/Volumes/Audiodrive/stt
LOG=/Volumes/Audiodrive/stt-consolidate.log
say() { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }

say "=== queued (pid $$): waiting for the general training pipeline ==="
until grep -q "general pipeline finished" /Volumes/USB2/stt/logs/general.log 2>/dev/null; do sleep 120; done
while pgrep -f "train.py|evaluate.py|benchmark.py" >/dev/null; do sleep 60; done

say "2. copying USB2/stt"
caffeinate -i rsync -a /Volumes/USB2/stt/ "$DST/" >> "$LOG" 2>&1 || { say "copy FAILED"; exit 1; }

say "3. dev-view symlinks"
find "$DST/stores" -type l | while read -r link; do
  target=$(readlink "$link")
  case "$target" in
    /Volumes/USB2/stt/stores/*)
      rel="../${target#/Volumes/USB2/stt/stores/}"
      ln -sfn "$rel" "$link" && say "  $link -> $rel" ;;
  esac
done

say "4. checksum verification (USB, then USB2)"
for src in /Volumes/USB/stt /Volumes/USB2/stt; do
  diffs=$(rsync -rc --dry-run --itemize-changes --no-links "$src/" "$DST/" | grep -v '^\.d' | grep -v '^cd' || true)
  if [ -z "$diffs" ]; then say "  $src: all files identical by checksum"
  else say "  $src: DIFFERENCES:"; echo "$diffs" | head -50 | tee -a "$LOG"; fi
done
say "=== consolidation finished (originals untouched) ==="
