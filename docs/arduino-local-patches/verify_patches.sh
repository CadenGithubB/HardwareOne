#!/usr/bin/env bash
# Verify the local patches to the vendored (git-ignored) components/arduino
# are still applied. A `git clean -dfx` or re-vendor of the component silently
# reverts them while the app code that depends on them survives — most
# critically the raise-only local-MTU guard in BLEClient::setMTU, without
# which ring-first glasses discovery breaks again ("pkt size: 102, PDU size: 64").
#
# Usage: bash docs/arduino-local-patches/verify_patches.sh
# Exits nonzero if any marked patch is missing.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
ARD="$ROOT/components/arduino"
fail=0

check_markers() { # relative-file min_marker_count
  local f="$ARD/$1" want="$2" have
  if [ ! -f "$f" ]; then
    echo "MISSING FILE: components/arduino/$1"
    fail=1
    return
  fi
  have=$(grep -c "hardwareone local patch" "$f" 2>/dev/null)
  have=${have:-0}
  if [ "$have" -lt "$want" ]; then
    echo "PATCHES MISSING: components/arduino/$1 has $have markers, expected >= $want"
    fail=1
  else
    echo "ok: components/arduino/$1 ($have markers)"
  fi
}

# Expected minimum marker counts as of 2026-07-28. If you ADD a marked patch,
# bump the count here and regenerate arduino-local-patches.patch (see README).
check_markers libraries/BLE/src/BLEClient.cpp 11
check_markers libraries/BLE/src/BLEDevice.cpp 1
check_markers libraries/BLE/src/BLERemoteCharacteristic.cpp 5
check_markers libraries/BLE/src/BLERemoteDescriptor.cpp 4

# NetworkEvents.cpp carries an UNMARKED local change — detectable only via git.
# A clean diff here means it was either reverted or committed into the nested
# repo; check `git -C components/arduino log` before assuming it is fine.
if git -C "$ARD" diff --quiet -- libraries/Network/src/NetworkEvents.cpp 2>/dev/null; then
  echo "WARNING: libraries/Network/src/NetworkEvents.cpp shows no local diff (reverted, or committed in the nested repo?)"
fi

if [ "$fail" -ne 0 ]; then
  echo ""
  echo "Local patches are MISSING. Re-apply with:"
  echo "  git -C \"$ARD\" apply --check \"$ROOT/docs/arduino-local-patches/arduino-local-patches.patch\" \\"
  echo "    && git -C \"$ARD\" apply \"$ROOT/docs/arduino-local-patches/arduino-local-patches.patch\""
  exit 1
fi
echo "All arduino local patches present."
