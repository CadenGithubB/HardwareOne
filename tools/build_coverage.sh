#!/usr/bin/env bash
# build_coverage.sh — compile-coverage builds for code the shipping profile hides.
#
# WHY THIS EXISTS
# ---------------
# The default board profile leaves I2C_FEATURE_LEVEL=0, DISPLAY_TYPE=0,
# INPUT_DEVICE_TYPE=0 and ENABLE_MQTT=0. That compiles OUT every OLED handler,
# the gamepad/joystick paths, and every MQTT branch — roughly half of the
# setup-wizard code. A green shipping build proves nothing about those files:
# they reduce to empty translation units (~2 KB objects).
#
# Two passes, reported separately:
#   pass 1  OLED + gamepad + I2C + MQTT on            (ESP-NOW left as-is)
#   pass 2  ...and CUSTOM_ENABLE_NET_ESPNOW=0         (the !ENABLE_ESPNOW arms)
#
# It restores System_BuildConfig.h BYTE-IDENTICALLY (md5-verified) from an EXIT
# trap, so Ctrl-C or a failed build cannot leave the user's live flags altered.
# The binaries are throwaway — the point is the COMPILER. Do not flash them.
#
#   usage: tools/build_coverage.sh [board] [1|2|both]     (default: xiao_s3 both)
#
# ---------------------------------------------------------------------------
# THE ASSERT IS THE POINT — read its output, not just the exit code.
#
# `sed -i '' 's/^#define FOO 0/.../'` exits 0 when it matches NOTHING. So a
# reformatted, renamed or re-indented flag turns this script into a silent
# no-op that rebuilds the SHIPPING profile and cheerfully reports
# "compile-errors=0". That is worse than no coverage, because it manufactures
# confidence. CUSTOM_ENABLE_NET_ESPNOW is exactly that shape already: it lives
# at System_BuildConfig.h:71 indented by two spaces, so a `^#define` anchor
# cannot match it.
#
# Therefore every flag is asserted on its EFFECTIVE VALUE after the edits, not
# on whether a substitution fired. Asserting the value (not the substitution)
# also keeps this correct when the user has already set a flag to the coverage
# value by hand — a legitimate no-op.
# ---------------------------------------------------------------------------

set -euo pipefail

BOARD="${1:-xiao_s3}"
WHICH="${2:-both}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BC="$ROOT/components/hardwareone/System_BuildConfig.h"
BAK="$(mktemp -t bccov)"
SUM="$(mktemp -t bcsum)"

cp "$BC" "$BAK"
md5 -q "$BC" > "$SUM"

restore() {
  cp "$BAK" "$BC"
  if [ "$(md5 -q "$BC")" = "$(cat "$SUM")" ]; then
    echo "-- System_BuildConfig.h restored byte-identical --"
  else
    echo "!! RESTORE MISMATCH — recover from $BAK !!" >&2
    return 1
  fi
  rm -f "$BAK" "$SUM"
}
trap restore EXIT

# set_flag NAME VALUE — indentation-tolerant, value-anything.
set_flag() {
  local name="$1" val="$2"
  sed -i '' -E "s/^([[:space:]]*)#define[[:space:]]+${name}[[:space:]]+[0-9]+/\\1#define ${name}       ${val}/" "$BC"
}

# assert_flag NAME EXPECTED — hard-fail on the EFFECTIVE value.
assert_flag() {
  local name="$1" want="$2" got
  got=$(grep -E "^[[:space:]]*#define[[:space:]]+${name}[[:space:]]+[0-9]+" "$BC" \
        | head -1 | sed -E "s/.*#define[[:space:]]+${name}[[:space:]]+([0-9]+).*/\1/")
  if [ -z "$got" ]; then
    echo "  ASSERT FAIL: ${name} not found — the anchor is stale, coverage is FAKE" >&2
    exit 90
  fi
  if [ "$got" != "$want" ]; then
    echo "  ASSERT FAIL: ${name}=${got}, expected ${want} — coverage is FAKE" >&2
    exit 91
  fi
  printf "    %-28s = %s\n" "$name" "$got"
}

# NOTE: `local f="$1" o="...$f..."` would read $f before it is assigned — under
# `set -u` that aborts. Two statements, deliberately.
objsize() {
  local f="$1"
  local o="$ROOT/build-$BOARD/esp-idf/hardwareone/CMakeFiles/__idf_hardwareone.dir/$f.cpp.obj"
  [ -f "$o" ] && stat -f%z "$o" || echo 0
}

ESPNOW_OBJ_PASS1=0
RC_TOTAL=0

run_pass() {
  local pass="$1" espnow="$2"
  echo
  echo "=========== PASS $pass  (board=$BOARD, ESP-NOW=$espnow) ==========="
  cp "$BAK" "$BC"                       # always start from the user's real file
  set_flag I2C_FEATURE_LEVEL 4
  # Level 4 derives the real OLED/gamepad gates from these CUSTOM selectors.
  # Setting DISPLAY_TYPE/INPUT_DEVICE_TYPE alone makes CMake add the source
  # files while their internal ENABLE_* guards still compile them out.
  set_flag CUSTOM_ENABLE_OLED 1
  set_flag CUSTOM_ENABLE_GAMEPAD 1
  set_flag DISPLAY_TYPE      1
  set_flag INPUT_DEVICE_TYPE 1
  set_flag ENABLE_MQTT       1
  [ "$espnow" = "0" ] && set_flag CUSTOM_ENABLE_NET_ESPNOW 0

  echo "  -- asserted effective values --"
  assert_flag I2C_FEATURE_LEVEL 4
  assert_flag CUSTOM_ENABLE_OLED 1
  assert_flag CUSTOM_ENABLE_GAMEPAD 1
  assert_flag DISPLAY_TYPE      1
  assert_flag INPUT_DEVICE_TYPE 1
  assert_flag ENABLE_MQTT       1
  assert_flag CUSTOM_ENABLE_NET_ESPNOW "$espnow"

  local log; log="$(mktemp -t covbuild)"
  set +e; "$ROOT/tools/build_board.sh" "$BOARD" build > "$log" 2>&1; local rc=$?; set -e
  local errs; errs=$(grep -cE "^/.*: (error|fatal error):" "$log" || true)
  echo "  exit=$rc  compile-errors=$errs"
  [ "$errs" != "0" ] && { echo "  --- distinct errors ---";
    grep -E "^/.*: (error|fatal error):" "$log" | sed 's|.*/components/hardwareone/||' | sort -u | head -30 || true; }
  grep -Ei "undefined reference" "$log" | sed 's/.*undefined reference to //' | sort -u | head -10 || true

  # Coverage GATE, not a print: an empty TU still produces an object (~2 KB).
  #
  # This gate — not the flag assert — is the real proof. assert_flag reads the
  # LITERAL #define, and a literal is not the effective preprocessor value:
  # System_BuildConfig.h:749 carries `#if !ENABLE_WIFI / #undef ENABLE_MQTT /
  # #define ENABLE_MQTT 0`, and ENABLE_OLED_DISPLAY is likewise re-derived from
  # DISPLAY_TYPE. A derived override would sail past assert_flag. Object size
  # cannot be fooled: the compiler either emitted the code or it did not.
  local oled; oled=$(objsize OLED_SetupWizard)
  local mqtt; mqtt=$(objsize System_MQTT)
  local espn; espn=$(objsize System_ESPNow)
  printf "  coverage: OLED_SetupWizard=%s B  System_MQTT=%s B  System_ESPNow=%s B\n" "$oled" "$mqtt" "$espn"
  if [ "$oled" -lt 100000 ]; then
    echo "  GATE FAIL: OLED_SetupWizard.cpp.obj is ${oled} B (<100 KB) — the OLED half did NOT compile" >&2
    exit 92
  fi
  if [ "$mqtt" -lt 100000 ]; then
    echo "  GATE FAIL: System_MQTT.cpp.obj is ${mqtt} B (<100 KB) — MQTT did NOT compile (derived override?)" >&2
    exit 94
  fi
  if [ "$pass" = "1" ]; then
    ESPNOW_OBJ_PASS1="$espn"
  elif [ "$ESPNOW_OBJ_PASS1" -gt 0 ] && [ "$espn" -ge "$ESPNOW_OBJ_PASS1" ]; then
    echo "  GATE FAIL: System_ESPNow.cpp.obj did not shrink (${espn} >= ${ESPNOW_OBJ_PASS1}) — ESP-NOW was NOT disabled" >&2
    exit 93
  fi
  echo "  log: $log"
  [ "$rc" != "0" ] && RC_TOTAL=1
  return 0
}

case "$WHICH" in
  1)    run_pass 1 1 ;;
  2)    run_pass 2 0 ;;
  both) run_pass 1 1; run_pass 2 0 ;;
  *)    echo "usage: $0 [board] [1|2|both]" >&2; exit 2 ;;
esac

echo
echo "=== coverage complete (overall rc=$RC_TOTAL) ==="
exit $RC_TOTAL
