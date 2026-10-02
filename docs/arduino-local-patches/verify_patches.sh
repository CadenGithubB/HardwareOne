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

# Expected minimum marker counts as of 2026-08-25. If you ADD a marked patch,
# bump the count here and regenerate arduino-local-patches.patch (see README).
check_markers libraries/BLE/src/BLECharacteristic.cpp 2
check_markers libraries/BLE/src/BLECharacteristic.h 1
check_markers libraries/BLE/src/BLEClient.cpp 13
check_markers libraries/BLE/src/BLEDevice.cpp 2
check_markers libraries/BLE/src/BLERemoteCharacteristic.cpp 7
check_markers libraries/BLE/src/BLERemoteDescriptor.cpp 4
check_markers libraries/WiFi/src/STA.cpp 1
check_markers cores/esp32/esp32-hal-i2c-ng.c 1
check_markers cores/esp32/esp32-hal-periman.c 2

# ESP32-P4 (ESP-Hosted companion radio) support, 2026-09-29. These hunks are
# unmarked upstream-style changes; check a signature symbol from each instead.
check_symbol() { # relative-file symbol
  if grep -q "$2" "$ARD/$1" 2>/dev/null; then
    echo "ok: components/arduino/$1 ($2)"
  else
    echo "PATCH MISSING: components/arduino/$1 lacks $2"
    fail=1
  fi
}
check_symbol cores/esp32/esp32-hal-hosted.h hostedSetPins
check_symbol cores/esp32/esp32-hal-bt.h btHostedControllerStatus
check_symbol libraries/BLE/src/BLEDevice.h CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID
check_symbol libraries/BLE/src/BLECharacteristic.h takeOwnership
check_symbol cores/esp32/esp32-hal-uart.c "CONFIG_IDF_TARGET_ESP32C5 || CONFIG_IDF_TARGET_ESP32P4"

# Companion "transport lost" teardown, 2026-10-02 (marked): Wi-Fi and BLE
# deinit must complete on a dead or restarting C6 (System_RadioCompanion).
check_symbol cores/esp32/esp32-hal-hosted.h hostedSetTransportLost
check_symbol cores/esp32/esp32-hal-bt.c hostedTransportLost
check_symbol libraries/WiFi/src/WiFiGeneric.cpp hostedTransportLost

# NetworkEvents.cpp carries an UNMARKED local change — detectable only via git.
# A clean diff here means it was either reverted or committed into the nested
# repo; check `git -C components/arduino log` before assuming it is fine.
# Only meaningful when components/arduino is its own nested checkout; a plain
# directory copy resolves `git -C` to the enclosing repo, which ignores it.
if [ -e "$ARD/.git" ] && git -C "$ARD" diff --quiet -- libraries/Network/src/NetworkEvents.cpp 2>/dev/null; then
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
