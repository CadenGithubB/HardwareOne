#!/bin/bash
# Consolidate STT data onto the exFAT SSD (Audiodrive, reformatted 2026-10-03
# so the Ubuntu A4000 PC can read and write it). Nothing is ever deleted.
#   consolidate_exfat.sh usb    copy USB/stt now (training never reads it), verify
#   consolidate_exfat.sh usb2   after the general run: copy USB2/stt, turn the
#                               symlinked dev views into real stores (exFAT has
#                               no symlinks), verify
# exFAT: no owners/permissions/symlinks, so rsync -rt (not -a); macOS "._"
# AppleDouble files are removed with dot_clean at the end.
set -u
DST=/Volumes/Audiodrive/stt
LOG=/Volumes/Audiodrive/consolidate.log
HERE=$(cd "$(dirname "$0")" && pwd)
P=${HW1_STT_PYTHON:-$HERE/../stt_p4/private/export-venv/bin/python}
say() { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }
mkdir -p "$DST"

verify() {  # <src> : checksum every regular file against the copy
  local diffs
  diffs=$(rsync -rc --dry-run --itemize-changes --no-links "$1/" "$DST/" | grep -v '^\.d' | grep -v '^cd' || true)
  if [ -z "$diffs" ]; then say "  verify $1: all files identical by checksum"
  else say "  verify $1: DIFFERENCES"; echo "$diffs" | head -50 | tee -a "$LOG"; return 1; fi
}

case "${1:-}" in
  usb)
    say "copy /Volumes/USB/stt"
    caffeinate -i rsync -rt /Volumes/USB/stt/ "$DST/" >> "$LOG" 2>&1 || { say "copy FAILED"; exit 1; }
    verify /Volumes/USB/stt
    dot_clean -m "$DST" 2>/dev/null; say "usb done" ;;
  usb2)
    say "waiting for the general training pipeline"
    until grep -q "general pipeline finished" /Volumes/USB2/stt/logs/general.log 2>/dev/null; do sleep 120; done
    while pgrep -f "train.py|evaluate.py|benchmark.py" >/dev/null; do sleep 60; done
    say "copy /Volumes/USB2/stt (symlinks skipped)"
    caffeinate -i rsync -rt --no-links /Volumes/USB2/stt/ "$DST/" >> "$LOG" 2>&1 || { say "copy FAILED"; exit 1; }
    say "materialise dev views"
    for link in $(find /Volumes/USB2/stt/stores -type l -name audio.bin); do
      store=$(basename "$(dirname "$link")")
      "$P" - "$store" "$link" >> "$LOG" 2>&1 <<'EOF' || say "  $store FAILED"
import json, sys
import numpy as np
from pathlib import Path
store, link = sys.argv[1], Path(sys.argv[2])
src_index = json.loads((link.parent / 'index.json').read_text())
audio = np.memmap(link.resolve(), dtype='<i2', mode='r')
out = Path('/Volumes/Audiodrive/stt/stores') / store
offsets, pos = [], 0
with open(out / 'audio.bin.tmp', 'wb') as f:
    for o, n in zip(src_index['offsets'], src_index['lengths']):
        f.write(np.asarray(audio[o:o + n]).tobytes()); offsets.append(pos); pos += n
(out / 'audio.bin.tmp').replace(out / 'audio.bin')
src_index['offsets'] = offsets
src_index.setdefault('source', {})['materialised_from'] = str(link.resolve())
(out / 'index.json').write_text(json.dumps(src_index))
# Same samples item by item as through the link.
chk = np.memmap(out / 'audio.bin', dtype='<i2', mode='r')
old = json.loads((link.parent / 'index.json').read_text())
assert all(np.array_equal(chk[a:a + n], audio[b:b + n]) for a, b, n in zip(offsets, old['offsets'], old['lengths']))
print(f'{store}: {len(offsets)} items, {pos / 16000 / 3600:.2f} h, identical samples')
EOF
    done
    verify /Volumes/USB/stt
    verify /Volumes/USB2/stt
    dot_clean -m "$DST" 2>/dev/null; say "=== consolidation finished (originals untouched) ===" ;;
  *) echo "usage: $0 usb|usb2" >&2; exit 2 ;;
esac
