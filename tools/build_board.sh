#!/usr/bin/env bash
# build_board.sh — per-board isolated builds.
#
#   tools/build_board.sh <board> [idf.py action/args...]      (default action: build)
#
#   tools/build_board.sh feathers3                 # build FeatherS3[D] in build-feathers3/
#   tools/build_board.sh qtpy_esp32 build          # classic-ESP32 board, own toolchain cache
#   tools/build_board.sh xiao_s3 flash monitor     # actions pass straight through to idf.py
#
# Each board gets its own build directory (build-<board>/) AND its own sdkconfig
# (build-<board>/sdkconfig), so switching boards never needs `idf.py fullclean`
# and never touches the tracked ./sdkconfig or the default ./build/ directory.
# The PSRAM-mode/bootloader mismatch that fullclean used to solve simply cannot
# happen — every board keeps its own bootloader in its own cache.
#
# The chip target comes from the board file's `# HW_TARGET:` marker, and
# HW_BOARD is always passed — which closes the known footgun where a bare
# `idf.py build` reconfigure defaults an S3 tree to flash=16mb and regenerates
# partitions.csv with a layout the sdkconfig can't fit.
#
# ONE SHARED FILE REMAINS: the root partitions.csv is (re)generated at CMake
# configure time from partitions_<sr>_<flash>.csv and is READ during the build.
# Do NOT run two different boards' builds concurrently — serialize them. The
# last configure wins; the file is gitignored/generated, so the next configure
# of any board simply rewrites it.

set -euo pipefail

usage() {
  echo "usage: $(basename "$0") <board> [idf.py args...]" >&2
  echo "boards:" >&2
  for f in "$REPO"/boards/*.defaults; do
    b="$(basename "$f" .defaults)"
    t="$(sed -n 's/^# *HW_TARGET:[[:space:]]*\([A-Za-z0-9]*\).*/\1/p' "$f" | head -1)"
    echo "  $b (${t:-target marker missing!})" >&2
  done
  exit 1
}

REPO="$(cd "$(dirname "$0")/.." && pwd)"

BOARD="${1:-}"
[[ -n "$BOARD" ]] || usage
shift || true

BOARD_FILE="$REPO/boards/$BOARD.defaults"
if [[ ! -f "$BOARD_FILE" ]]; then
  echo "error: unknown board '$BOARD' (no boards/$BOARD.defaults)" >&2
  usage
fi

TARGET="$(sed -n 's/^# *HW_TARGET:[[:space:]]*\([A-Za-z0-9]*\).*/\1/p' "$BOARD_FILE" | head -1)"
if [[ -z "$TARGET" ]]; then
  echo "error: $BOARD_FILE has no '# HW_TARGET: <chip>' marker" >&2
  exit 1
fi

if ! command -v idf.py >/dev/null 2>&1; then
  echo "error: idf.py not on PATH — source \$IDF_PATH/export.sh first" >&2
  exit 1
fi

BUILD_DIR="$REPO/build-$BOARD"

env IDF_TARGET="$TARGET" HW_BOARD="$BOARD" \
  idf.py -C "$REPO" -B "$BUILD_DIR" -DSDKCONFIG="$BUILD_DIR/sdkconfig" "${@:-build}"
status=$?

# Refresh the build manifest while the tree state still matches this build --
# feature flags are resolved by re-running the compiler over
# System_BuildConfig.h, so generating it later (after the header is edited for
# another board) cannot reproduce them. Never fails the build.
if [[ $status -eq 0 && -f "$BUILD_DIR/compile_commands.json" ]]; then
  python3 "$REPO/tools/gen_build_info.py" "$BUILD_DIR" "$BOARD" || \
    echo "warning: build manifest generation failed (build itself is fine)" >&2
fi

exit $status
