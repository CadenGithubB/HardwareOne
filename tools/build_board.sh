#!/usr/bin/env bash
# build_board.sh — per-board isolated builds.
#
#   tools/build_board.sh <board> [--config <name>] [idf.py action/args...]
#
#   tools/build_board.sh feathers3                 # build FeatherS3[D] in build-feathers3/
#   tools/build_board.sh qtpy_esp32 build          # classic-ESP32 board, own toolchain cache
#   tools/build_board.sh xiao_s3 flash monitor     # actions pass straight through to idf.py
#   tools/build_board.sh xiao_s3 --config xiao-mqtt # saved features, own build directory
#
# Each board gets its own build directory (build-<board>/) AND its own sdkconfig
# (build-<board>/sdkconfig), so switching boards never needs `idf.py fullclean`
# and never touches the tracked ./sdkconfig or the default ./build/ directory.
# The PSRAM-mode/bootloader mismatch that fullclean used to solve simply cannot
# happen — every board keeps its own bootloader in its own cache.
#
# The chip target comes from the board file's `# HW_TARGET:` marker, and
# HW_BOARD is always passed — which closes the known footgun where a bare
# `idf.py build` could combine one board's sdkconfig with another board's
# partition selection. Each build uses its authoritative partition table.
# Dependency resolution still shares managed_components: serialize cross-target
# configuration before running builds concurrently.

set -euo pipefail

usage() {
  echo "usage: $(basename "$0") <board> [--config <name>] [idf.py args...]" >&2
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

CONFIG_NAME=""
if [[ "${1:-}" == "--config" ]]; then
  CONFIG_NAME="${2:?--config requires a saved configuration name}"
  shift 2
  for action in "$@"; do
    case "$action" in
      -D*|-B*|-C*|--build-dir*|--project-dir*|--define-cache-entry*)
        echo "error: saved configurations manage CMake inputs and build paths; remove '$action'" >&2
        exit 1
        ;;
    esac
    if [[ "$action" == "fullclean" || "$action" == "set-target" ]]; then
      echo "error: saved configurations own their target and generated inputs; use clean or reconfigure instead of $action" >&2
      exit 1
    fi
  done
  if [[ -n "${HW_DEPLOYMENT:-}" ]]; then
    echo "error: saved feature configurations cannot override deployment contracts" >&2
    exit 1
  fi
  python3 "$REPO/tools/build_config.py" prepare "$CONFIG_NAME" "$BOARD"
fi

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

# Every board compiles against the vendored, git-ignored components/arduino,
# and every board depends on the local patches recorded in
# docs/arduino-local-patches (BLE fixes for the S3 boards, ESP-Hosted radio
# support for the P4). A fresh clone or re-vendor silently drops them while the
# firmware still compiles, so refuse to build until they are present.
if [[ ! -d "$REPO/components/arduino" ]]; then
  echo "error: components/arduino is missing. Vendor arduino-esp32 3.3.5 there and" >&2
  echo "       apply docs/arduino-local-patches/arduino-local-patches.patch (see its README)." >&2
  exit 1
fi
if ! bash "$REPO/docs/arduino-local-patches/verify_patches.sh" >/dev/null; then
  echo "error: components/arduino is missing local patches; re-run" >&2
  echo "       bash docs/arduino-local-patches/verify_patches.sh for the list and the re-apply command." >&2
  exit 1
fi

BUILD_DIR="$REPO/build-$BOARD"
FEATURE_FILE=""
SDK_FEATURE_FILE=""
if [[ -n "$CONFIG_NAME" ]]; then
  BUILD_DIR="$REPO/build-config-$CONFIG_NAME"
  FEATURE_FILE="$BUILD_DIR/features.h"
  SDK_FEATURE_FILE="$BUILD_DIR/sdkconfig.features"
  # Resolve with the real SDK and compiler before spending time compiling (or
  # running a requested flash action). No second dependency implementation.
  env IDF_TARGET="$TARGET" HW_BOARD="$BOARD" \
    idf.py -C "$REPO" -B "$BUILD_DIR" -DSDKCONFIG="$BUILD_DIR/sdkconfig" \
    -DHW1_FEATURE_CONFIG_FILE="$FEATURE_FILE" \
    -DHW1_SDK_CONFIG_FILE="$SDK_FEATURE_FILE" reconfigure
  python3 "$REPO/tools/build_config.py" check "$CONFIG_NAME"
fi

env IDF_TARGET="$TARGET" HW_BOARD="$BOARD" \
  idf.py -C "$REPO" -B "$BUILD_DIR" -DSDKCONFIG="$BUILD_DIR/sdkconfig" \
  -DHW1_FEATURE_CONFIG_FILE="$FEATURE_FILE" \
  -DHW1_SDK_CONFIG_FILE="$SDK_FEATURE_FILE" "${@:-build}"
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
