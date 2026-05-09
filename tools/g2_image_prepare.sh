#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  g2_image_prepare.sh <input_image> <output_bmp> [options]

Options:
  --width <n>         Output width in pixels (default: 80)
  --height <n>        Output height in pixels (default: 80)
  --fit <mode>        contain | cover | stretch (default: contain)
  --dither <mode>     none | floyd (default: floyd)
  --invert            Invert grayscale output
  -h, --help          Show this help

Notes:
  - Produces a 4bpp indexed BMP (16 colors), suitable for G2 image path.
  - Default size is intentionally small; use --width/--height as needed.
  - If your container is 288x144, output must be exactly 288x144.
EOF
}

require_cmd() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "Missing required command: $1" >&2
    exit 1
  fi
}

WIDTH=80
HEIGHT=80
FIT="contain"
DITHER="floyd"
INVERT=0

if [[ $# -eq 1 && ( "$1" == "-h" || "$1" == "--help" ) ]]; then
  usage
  exit 0
fi

if [[ $# -lt 2 ]]; then
  usage
  exit 1
fi

INPUT="$1"
OUTPUT="$2"
shift 2

while [[ $# -gt 0 ]]; do
  case "$1" in
    --width)
      WIDTH="${2:-}"
      shift 2
      ;;
    --height)
      HEIGHT="${2:-}"
      shift 2
      ;;
    --fit)
      FIT="${2:-}"
      shift 2
      ;;
    --dither)
      DITHER="${2:-}"
      shift 2
      ;;
    --invert)
      INVERT=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown option: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [[ ! -f "$INPUT" ]]; then
  echo "Input not found: $INPUT" >&2
  exit 1
fi

if ! [[ "$WIDTH" =~ ^[0-9]+$ ]] || ! [[ "$HEIGHT" =~ ^[0-9]+$ ]] || [[ "$WIDTH" -le 0 ]] || [[ "$HEIGHT" -le 0 ]]; then
  echo "Width and height must be positive integers." >&2
  exit 1
fi

if [[ "$FIT" != "contain" && "$FIT" != "cover" && "$FIT" != "stretch" ]]; then
  echo "Invalid --fit mode: $FIT" >&2
  exit 1
fi

if [[ "$DITHER" != "none" && "$DITHER" != "floyd" ]]; then
  echo "Invalid --dither mode: $DITHER" >&2
  exit 1
fi

if command -v magick >/dev/null 2>&1; then
  IM_BIN="magick"
elif command -v convert >/dev/null 2>&1; then
  IM_BIN="convert"
else
  echo "ImageMagick is required. Install 'magick' (or 'convert')." >&2
  exit 1
fi

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

PALETTE="$TMP_DIR/g2_palette.png"

"$IM_BIN" -size 16x1 gradient:black-white -colorspace Gray -depth 8 "$PALETTE"

SIZE_ARG="${WIDTH}x${HEIGHT}"
RESIZE_ARGS=()
case "$FIT" in
  contain)
    RESIZE_ARGS=(-resize "${SIZE_ARG}" -background black -gravity center -extent "${SIZE_ARG}")
    ;;
  cover)
    RESIZE_ARGS=(-resize "${SIZE_ARG}^" -gravity center -extent "${SIZE_ARG}")
    ;;
  stretch)
    RESIZE_ARGS=(-resize "${SIZE_ARG}!")
    ;;
esac

if [[ "$DITHER" == "none" ]]; then
  DITHER_OPT="+dither"
else
  DITHER_OPT="-dither FloydSteinberg"
fi

if [[ "$INVERT" -eq 1 ]]; then
  INVERT_OPT="-negate"
else
  INVERT_OPT=""
fi

if [[ "$FIT" == "contain" ]]; then
  FIT_ARGS=(-resize "${SIZE_ARG}" -background black -gravity center -extent "${SIZE_ARG}")
elif [[ "$FIT" == "cover" ]]; then
  FIT_ARGS=(-resize "${SIZE_ARG}^" -gravity center -extent "${SIZE_ARG}")
else
  FIT_ARGS=(-resize "${SIZE_ARG}!")
fi

STAGE_PNG="$TMP_DIR/g2_stage.png"

if [[ -n "$INVERT_OPT" ]]; then
  # shellcheck disable=SC2086
  "$IM_BIN" "$INPUT" "${FIT_ARGS[@]}" -colorspace Gray $INVERT_OPT \
    -define "dither:diffusion-amount=85%" $DITHER_OPT \
    -remap "$PALETTE" -type Palette -colors 16 -depth 4 \
    "PNG8:$STAGE_PNG"
else
  # shellcheck disable=SC2086
  "$IM_BIN" "$INPUT" "${FIT_ARGS[@]}" -colorspace Gray \
    -define "dither:diffusion-amount=85%" $DITHER_OPT \
    -remap "$PALETTE" -type Palette -colors 16 -depth 4 \
    "PNG8:$STAGE_PNG"
fi

# The Even G2 firmware expects top-down BMPs — every BMP the firmware
# itself builds passes -kImgH (negative height). ImageMagick's BMP3
# encoder writes bottom-up by default, which the firmware would render
# either upside-down or not at all.
#
# Two-step fix:
#   1. -flip in IM so file row 0 = original top row when written bottom-up.
#   2. Rewrite the DIB height field to negative so the file's metadata
#      matches its scan order. Firmware sees: top-down + row 0 = top.
"$IM_BIN" "$STAGE_PNG" -colors 16 -type Palette -depth 4 \
  -flip \
  -define bmp:bits-per-pixel=4 "BMP3:$OUTPUT"

python3 - "$OUTPUT" <<'PY'
import struct
import sys

path = sys.argv[1]
with open(path, "r+b") as f:
    f.seek(0x16)
    (h,) = struct.unpack("<i", f.read(4))
    if h <= 0:
        sys.exit(0)  # already top-down (or invalid); leave alone
    f.seek(0x16)
    f.write(struct.pack("<i", -h))
PY

echo "Wrote $OUTPUT (${WIDTH}x${HEIGHT}, 16-color 4bpp BMP, top-down)"
