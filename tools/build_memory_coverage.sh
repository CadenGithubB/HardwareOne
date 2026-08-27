#!/usr/bin/env bash
# Compile allocator-sensitive code hidden by the shipping feature profile.
#
# The live build configuration is restored byte-for-byte from an EXIT trap.
# These binaries are compile coverage only; do not flash them.
#
# Usage: tools/build_memory_coverage.sh [board] [sr|llm|both]

set -euo pipefail

MEMCOV_BOARD="${1:-xiao_s3}"
MEMCOV_PASS="${2:-both}"
MEMCOV_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MEMCOV_CONFIG="$MEMCOV_ROOT/components/hardwareone/System_BuildConfig.h"
MEMCOV_BOARD_FILE="$MEMCOV_ROOT/boards/$MEMCOV_BOARD.defaults"

if [[ ! -f "$MEMCOV_BOARD_FILE" ]]; then
  echo "memory coverage: unknown board '$MEMCOV_BOARD'" >&2
  exit 2
fi
MEMCOV_TARGET="$(sed -n 's/^# *HW_TARGET:[[:space:]]*\([A-Za-z0-9]*\).*/\1/p' "$MEMCOV_BOARD_FILE" | head -1)"
if [[ -z "$MEMCOV_TARGET" ]]; then
  echo "memory coverage: $MEMCOV_BOARD_FILE has no HW_TARGET marker" >&2
  exit 2
fi

MEMCOV_BACKUP="$(mktemp -t hw1-memcfg)"
cp "$MEMCOV_CONFIG" "$MEMCOV_BACKUP"

restore_config() {
  local status="$1"
  trap - EXIT

  if ! cp "$MEMCOV_BACKUP" "$MEMCOV_CONFIG" || \
      ! cmp -s "$MEMCOV_BACKUP" "$MEMCOV_CONFIG"; then
    echo "memory coverage: failed to restore System_BuildConfig.h byte-for-byte" >&2
    status=93
  fi
  rm -f "$MEMCOV_BACKUP"

  # CMake caches the temporary definitions in build-<board>. Restore that
  # directory too, so a later ordinary build cannot accidentally inherit the
  # slim profile even though the checked-in header has already been restored.
  if command -v idf.py >/dev/null 2>&1 && \
      [[ -d "$MEMCOV_ROOT/build-$MEMCOV_BOARD" ]]; then
    echo "memory coverage: restoring ordinary $MEMCOV_BOARD build configuration"
    if ! "$MEMCOV_ROOT/tools/build_board.sh" "$MEMCOV_BOARD" reconfigure \
        >/dev/null; then
      echo "memory coverage: failed to restore the ordinary build configuration" >&2
      [[ "$status" -ne 0 ]] || status=94
    fi
  fi

  exit "$status"
}
trap 'restore_config $?' EXIT

set_flag() {
  local name="$1"
  local value="$2"
  sed -i '' -E \
    "s/^([[:space:]]*)#define[[:space:]]+${name}[[:space:]]+[0-9]+/\\1#define ${name}       ${value}/" \
    "$MEMCOV_CONFIG"
}

assert_flag() {
  local name="$1"
  local expected="$2"
  local actual
  actual="$(sed -n -E \
    "s/^[[:space:]]*#define[[:space:]]+${name}[[:space:]]+([0-9]+).*/\\1/p" \
    "$MEMCOV_CONFIG" | head -1)"
  if [[ "$actual" != "$expected" ]]; then
    echo "memory coverage: ${name}=${actual:-missing}, expected ${expected}" >&2
    exit 90
  fi
}

prepare_coverage_profile() {
  cp "$MEMCOV_BACKUP" "$MEMCOV_CONFIG"
  [[ "$MEMCOV_BOARD" == "xiao_s3" ]] || return 0

  # Temporary compile-coverage profile for the 8 MB XIAO. Keep WiFi/HTTP,
  # camera, microphone, and the feature under test; remove unrelated hardware
  # and product surfaces so feature-enabled images have honest link coverage.
  # The EXIT trap restores the shipping configuration byte-for-byte.
  set_flag I2C_FEATURE_LEVEL 0
  set_flag DISPLAY_TYPE 0
  set_flag INPUT_DEVICE_TYPE 0
  set_flag ENABLE_BLUETOOTH 0
  set_flag ENABLE_G2_GLASSES 0
  set_flag ENABLE_R1_HEALTH 0
  set_flag ENABLE_AUTOMATION 0
  set_flag ENABLE_HTTPS 0
  set_flag CUSTOM_ENABLE_NET_ESPNOW 0
  set_flag CUSTOM_ENABLE_WEB_BLUETOOTH 0
  set_flag CUSTOM_ENABLE_WEB_ESPNOW 0
  set_flag CUSTOM_ENABLE_WEB_R1_HEALTH 0
  set_flag ENABLE_RASPBERRY_PI_HOST_POWER 0
  set_flag ENABLE_RASPBERRY_PI_HOST_FAN 0
  set_flag ENABLE_BONDED_MODE 0

  for disabled in I2C_FEATURE_LEVEL DISPLAY_TYPE INPUT_DEVICE_TYPE \
                  ENABLE_BLUETOOTH ENABLE_G2_GLASSES ENABLE_R1_HEALTH \
                  ENABLE_AUTOMATION ENABLE_HTTPS CUSTOM_ENABLE_NET_ESPNOW \
                  CUSTOM_ENABLE_WEB_BLUETOOTH CUSTOM_ENABLE_WEB_ESPNOW \
                  CUSTOM_ENABLE_WEB_R1_HEALTH \
                  ENABLE_RASPBERRY_PI_HOST_POWER \
                  ENABLE_RASPBERRY_PI_HOST_FAN ENABLE_BONDED_MODE; do
    assert_flag "$disabled" 0
  done
  echo "memory coverage: applied temporary slim XIAO profile"
}

object_size() {
  local source_stem="$1"
  local object="$MEMCOV_ROOT/build-$MEMCOV_BOARD/esp-idf/hardwareone/CMakeFiles/__idf_hardwareone.dir/${source_stem}.cpp.obj"
  [[ -f "$object" ]] && stat -f%z "$object" || echo 0
}

build_and_check_object() {
  local source_stem="$1"
  local description="$2"
  local failure_code="$3"
  local build_status=0 bytes
  local object_target="esp-idf/hardwareone/CMakeFiles/__idf_hardwareone.dir/${source_stem}.cpp.obj"

  "$MEMCOV_ROOT/tools/build_board.sh" "$MEMCOV_BOARD" build || build_status=$?
  # A feature-heavy image can overflow its partition, and an unrelated source
  # can fail later in the parallel full build. Ask Ninja for the exact object
  # so this gate answers the narrower question it advertises: did the hidden
  # allocator-sensitive implementation compile under the selected flags?
  if ! env IDF_TARGET="$MEMCOV_TARGET" HW_BOARD="$MEMCOV_BOARD" \
      ninja -C "$MEMCOV_ROOT/build-$MEMCOV_BOARD" "$object_target"; then
    echo "memory coverage: ${description} did not compile in this pass" >&2
    exit "$failure_code"
  fi
  bytes="$(object_size "$source_stem")"
  echo "memory coverage: ${source_stem}.cpp.obj=${bytes} B"

  if [[ "$bytes" -lt 50000 ]]; then
    echo "memory coverage: ${description} did not compile in this pass" >&2
    exit "$failure_code"
  fi
  if [[ "$build_status" -ne 0 ]]; then
    echo "memory coverage: ${description} compiled; full feature image did not link (status ${build_status})" >&2
    if [[ "$MEMCOV_BOARD" == "xiao_s3" ]]; then
      exit "$failure_code"
    fi
  fi
}

run_sr() {
  prepare_coverage_profile
  set_flag ENABLE_ESP_SR 1
  set_flag ENABLE_LLM_BACKEND 0
  set_flag ENABLE_LLM_SOURCE_ONBOARD 0
  set_flag ENABLE_LLM_SOURCE_CM5 0
  assert_flag ENABLE_ESP_SR 1
  assert_flag ENABLE_LLM_BACKEND 0
  assert_flag ENABLE_LLM_SOURCE_ONBOARD 0
  assert_flag ENABLE_LLM_SOURCE_CM5 0
  build_and_check_object System_ESPSR "ESP-SR implementation" 91
}

run_llm() {
  prepare_coverage_profile
  set_flag ENABLE_ESP_SR 0
  set_flag ENABLE_LLM_BACKEND 1
  set_flag ENABLE_LLM_SOURCE_ONBOARD 1
  set_flag ENABLE_LLM_SOURCE_CM5 0
  assert_flag ENABLE_ESP_SR 0
  assert_flag ENABLE_LLM_BACKEND 1
  assert_flag ENABLE_LLM_SOURCE_ONBOARD 1
  assert_flag ENABLE_LLM_SOURCE_CM5 0
  build_and_check_object System_LLM_Model "onboard LLM implementation" 92
}

case "$MEMCOV_PASS" in
  sr) run_sr ;;
  llm) run_llm ;;
  both) run_sr; run_llm ;;
  *)
    echo "usage: $(basename "$0") [board] [sr|llm|both]" >&2
    exit 2
    ;;
esac
