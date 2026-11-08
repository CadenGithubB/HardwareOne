#!/usr/bin/env bash
# Stage the patched ESP-IDF, ESP-DL and ESP-Hosted components that every
# ESP32-P4 build uses, in .sdk-overrides/esp32p4/ (ignored by git). The
# exported SDK and the component manager's packages are only read. On a fresh
# machine the pinned ESP-DL and ESP-Hosted registry packages are fetched first
# (fetch_p4_packages.py; needs network access to components.espressif.com).
# Re-running verifies an existing copy instead of rewriting it. Both modes end
# with whether the IDF tree carries the PIE q3 context-switch fix that
# speaker-label (p4x_eye) builds need (check_idf_pie.py; reported, not fatal).
#
#   . "$IDF_PATH/export.sh"      # ESP-IDF 5.5.5
#   tools/p4/prepare_sdk.sh      # add --check to verify without staging
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
: "${IDF_PATH:?Export ESP-IDF 5.5.5 first}"
if [[ "${1:-}" == "--check" ]]; then
  # Run every check so one report shows the whole state.
  failed=0
  python3 -B "$here/prepare_idf_bt.py" --check || failed=1
  python3 -B "$here/prepare_idf_jpeg.py" --idf "$IDF_PATH" --check || failed=1
  python3 -B "$here/prepare_esp_dl.py" --check || failed=1
  python3 -B "$here/prepare_esp_hosted.py" --check || failed=1
  python3 -B "$here/check_idf_pie.py" --idf "$IDF_PATH"
  if (( failed )); then
    echo "prepare_sdk --check: overrides missing or changed; run tools/p4/prepare_sdk.sh" >&2
    exit 1
  fi
else
  python3 -B "$here/prepare_idf_bt.py" --idf "$IDF_PATH"
  python3 -B "$here/prepare_idf_jpeg.py" --idf "$IDF_PATH"
  python3 -B "$here/fetch_p4_packages.py" --idf "$IDF_PATH"
  python3 -B "$here/prepare_esp_dl.py"
  python3 -B "$here/prepare_esp_hosted.py"
  python3 -B "$here/check_idf_pie.py" --idf "$IDF_PATH"
fi
