#!/usr/bin/env bash
# build_board.sh — per-board isolated builds.
#
#   tools/build_board.sh <board> [--config <name>] [--wdt-soak] [idf.py action/args...]
#
#   tools/build_board.sh feathers3                 # build FeatherS3[D] in build-feathers3/
#   tools/build_board.sh qtpy_esp32 build          # classic-ESP32 board, own toolchain cache
#   tools/build_board.sh xiao_s3 flash monitor     # actions pass straight through to idf.py
#   tools/build_board.sh xiao_s3 --config xiao-mqtt # saved features, own build directory
#   tools/build_board.sh feathers3 --wdt-soak      # watchdog soak image in build-feathers3-wdt-soak/
#
# --wdt-soak layers config/sdkconfig.wdt_soak.defaults: the
# task watchdog panics on a trip and the main loop is subscribed to it. It
# always builds in its own directory (suffix -wdt-soak) because sdkconfig
# values stick; ordinary builds and their directories are never touched.
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
#
# All build directories of one checkout share managed_components/, which the
# component manager syncs to the configuring target's lock, and the per-target
# locks pin different versions (P4 deliberately differs from S3/ESP32).
# cmake/HW1ManagedComponents.cmake records which lock last populated it, so
# building boards of different targets one after another just works: the next
# build of another target reconfigures and resyncs on its own, and a rebuild
# of the same target stays incremental. Builds of the same target may run at
# the same time; starting a build of another target while one runs is refused
# (tools/checkout_build_lock.sh) with an error naming the running build, since
# its sync would change sources under that compile. Use a separate git
# worktree to build two targets in parallel. A `monitor`-only invocation of an
# already configured build directory takes no lock, so flash and monitor as two
# commands to keep building other targets next to a long monitor session.

set -euo pipefail

usage() {
  echo "usage: $(basename "$0") <board> [--config <name>] [--wdt-soak] [idf.py args...]" >&2
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
WDT_SOAK=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --config)
      CONFIG_NAME="${2:?--config requires a saved configuration name}"
      shift 2
      ;;
    --wdt-soak)
      WDT_SOAK=1
      shift
      ;;
    *)
      break
      ;;
  esac
done
if [[ "$CONFIG_NAME" == *-wdt-soak ]]; then
  # The soak build of "<name>" uses build-config-<name>-wdt-soak/; a saved
  # configuration with that suffix would share it and inherit the soak.
  echo "error: configuration names ending in -wdt-soak are reserved; use --wdt-soak" >&2
  exit 1
fi
# build_config.py names the saved configuration's directory from this.
export HW1_WDT_SOAK="$WDT_SOAK"
if [[ -n "$CONFIG_NAME" ]]; then
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

# One target at a time per checkout (see the header). A serial monitor alone
# compiles nothing, and in a configured build directory it does not configure
# either, so it may run next to any build. idf.py monitor does configure (and
# sync managed_components) when project_description.json is missing, and
# --config always reconfigures first, so those still take the lock.
monitor_only=0
if [[ $# -gt 0 ]]; then
  monitor_only=1
  saw_monitor=0
  takes_value=0
  for arg in "$@"; do
    if [[ $takes_value -eq 1 ]]; then
      takes_value=0
    elif [[ "$arg" == "monitor" ]]; then
      saw_monitor=1
    elif [[ "$arg" == "-p" || "$arg" == "--port" || "$arg" == "-b" || "$arg" == "--baud" ]]; then
      takes_value=1
    elif [[ "$arg" != -* ]]; then
      monitor_only=0
    fi
  done
  [[ $saw_monitor -eq 1 ]] || monitor_only=0
  [[ -z "$CONFIG_NAME" && -f "$REPO/build-$BOARD/project_description.json" ]] || monitor_only=0
fi
if [[ $monitor_only -eq 0 ]]; then
  . "$REPO/tools/checkout_build_lock.sh"
  hw1_take_checkout_build_lock "$REPO" "$TARGET" "tools/build_board.sh $BOARD${CONFIG_NAME:+ --config $CONFIG_NAME}"
fi

if [[ -n "$CONFIG_NAME" ]]; then
  python3 "$REPO/tools/build_config.py" prepare "$CONFIG_NAME" "$BOARD"
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

SOAK_SUFFIX=""
SOAK_ARGS=()
if [[ "$WDT_SOAK" == 1 ]]; then
  SOAK_SUFFIX="-wdt-soak"
  SOAK_ARGS=(-DHW1_WDT_SOAK=ON)
fi
BUILD_DIR="$REPO/build-$BOARD$SOAK_SUFFIX"
FEATURE_FILE=""
SDK_FEATURE_FILE=""
if [[ -n "$CONFIG_NAME" ]]; then
  BUILD_DIR="$REPO/build-config-$CONFIG_NAME$SOAK_SUFFIX"
  FEATURE_FILE="$BUILD_DIR/features.h"
  SDK_FEATURE_FILE="$BUILD_DIR/sdkconfig.features"
  # Resolve with the real SDK and compiler before spending time compiling (or
  # running a requested flash action). No second dependency implementation.
  env IDF_TARGET="$TARGET" HW_BOARD="$BOARD" \
    idf.py -C "$REPO" -B "$BUILD_DIR" -DSDKCONFIG="$BUILD_DIR/sdkconfig" \
    -DHW1_FEATURE_CONFIG_FILE="$FEATURE_FILE" \
    -DHW1_SDK_CONFIG_FILE="$SDK_FEATURE_FILE" ${SOAK_ARGS[@]+"${SOAK_ARGS[@]}"} reconfigure
  python3 "$REPO/tools/build_config.py" check "$CONFIG_NAME"
fi

env IDF_TARGET="$TARGET" HW_BOARD="$BOARD" \
  idf.py -C "$REPO" -B "$BUILD_DIR" -DSDKCONFIG="$BUILD_DIR/sdkconfig" \
  -DHW1_FEATURE_CONFIG_FILE="$FEATURE_FILE" \
  -DHW1_SDK_CONFIG_FILE="$SDK_FEATURE_FILE" ${SOAK_ARGS[@]+"${SOAK_ARGS[@]}"} "${@:-build}"
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
