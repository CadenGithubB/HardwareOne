#!/usr/bin/env bash
# Passive HCI snoop pull — after Even app OTA succeeds.
# Never MitM. Never connect Hardware One during the update.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LABEL="${1:-ota}"
SERIAL="${ADB_SERIAL:-}"
STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="${ROOT}/.scratch/btsnoop/${LABEL}-${STAMP}"

cat <<'EOF'
============================================================
  Passive OTA / HCI capture — PREFLIGHT
============================================================
  1. Android Developer options → Enable Bluetooth HCI snoop log
  2. Hardware One / ESP32 / any BLE proxy: DISCONNECTED
     (phone Even app must own the link for the whole update)
  3. Run the update in the Even app until it SUCCEEDS
  4. Then run this script

  This script only pulls a bugreport and extracts btsnoop logs.
  It does not talk to the glasses or ring.

  Android rotates two files (default 65535 packets each):
    btsnoop_hci.log       +  btsnoop_hci.log.last
  A long G2 flash often spans both — we copy both and build
  btsnoop_hci.combined.log (.last then current, header stripped).
============================================================
EOF

ADB=(adb)
if [[ -n "$SERIAL" ]]; then
  ADB=(adb -s "$SERIAL")
fi

if ! command -v adb >/dev/null 2>&1; then
  echo "error: adb not found in PATH" >&2
  exit 1
fi

mkdir -p "$OUT"
ZIP="$OUT/bugreport.zip"
LOGDIR="$OUT/extracted"

echo
echo "==> bugreport → $ZIP"
"${ADB[@]}" bugreport "$ZIP"

echo "==> extracting Bluetooth HCI logs"
mkdir -p "$LOGDIR"
unzip -o -q "$ZIP" \
  'FS/data/misc/bluetooth/logs/*' \
  -d "$LOGDIR" 2>/dev/null || true

# Normalize path to a top-level copy for convenience
HCI=""
if [[ -f "$LOGDIR/FS/data/misc/bluetooth/logs/btsnoop_hci.log" ]]; then
  HCI="$LOGDIR/FS/data/misc/bluetooth/logs/btsnoop_hci.log"
elif [[ -f "$LOGDIR/btsnoop_hci.log" ]]; then
  HCI="$LOGDIR/btsnoop_hci.log"
else
  HCI="$(find "$LOGDIR" -name 'btsnoop_hci.log' -print -quit 2>/dev/null || true)"
fi

if [[ -z "$HCI" || ! -f "$HCI" ]]; then
  echo "error: btsnoop_hci.log not found in bugreport" >&2
  echo "       Is HCI snoop enabled? Check $LOGDIR" >&2
  exit 1
fi

cp -f "$HCI" "$OUT/btsnoop_hci.log"
LAST="$(dirname "$HCI")/btsnoop_hci.log.last"
HAVE_LAST=0
if [[ -f "$LAST" ]]; then
  cp -f "$LAST" "$OUT/btsnoop_hci.log.last"
  HAVE_LAST=1
fi

# Combined = older (.last) then current without repeating 16-byte btsnoop header
COMBINED="$OUT/btsnoop_hci.combined.log"
if [[ "$HAVE_LAST" -eq 1 ]]; then
  # shellcheck disable=SC2002
  {
    cat "$OUT/btsnoop_hci.log.last"
    # skip 16-byte file header on current
    tail -c +17 "$OUT/btsnoop_hci.log"
  } > "$COMBINED"
else
  cp -f "$OUT/btsnoop_hci.log" "$COMBINED"
fi

BYTES="$(wc -c < "$OUT/btsnoop_hci.log" | tr -d ' ')"
COMB_BYTES="$(wc -c < "$COMBINED" | tr -d ' ')"
echo
echo "OK: $OUT/btsnoop_hci.log ($BYTES bytes)"
if [[ "$HAVE_LAST" -eq 1 ]]; then
  LAST_BYTES="$(wc -c < "$OUT/btsnoop_hci.log.last" | tr -d ' ')"
  echo "    $OUT/btsnoop_hci.log.last ($LAST_BYTES bytes)"
fi
echo "    $COMBINED ($COMB_BYTES bytes)  ← prefer this for extract"
echo
echo "Next (offline, read-only) — use absolute paths from ~ if needed:"
echo
echo "  # G2 OTA / EFS (combined spans snoop rotation):"
echo "  python3 $ROOT/tools/btsnoop/ota_extract.py \\"
echo "    $COMBINED \\"
echo "    -o $OUT/out"
echo
echo "  # R1 Secure DFU (often entirely in current log; combined also OK):"
echo "  python3 $ROOT/tools/btsnoop/r1_dfu_extract.py \\"
echo "    $OUT/btsnoop_hci.log \\"
echo "    -o $OUT/dfu_out"
echo "  python3 $ROOT/tools/btsnoop/r1_fw_analyze.py \\"
echo "    --bin $OUT/dfu_out/r1_dfu_application.bin \\"
echo "    --dat $OUT/dfu_out/r1_dfu_init.dat \\"
echo "    -o $OUT/dfu_out"
echo
echo "See docs/OTA_PASSIVE_CAPTURE.md and tools/btsnoop/README.md"
echo
