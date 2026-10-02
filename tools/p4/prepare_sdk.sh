#!/usr/bin/env bash
# Stage the patched ESP-IDF components that every ESP32-P4 build uses, in
# .sdk-overrides/esp32p4/ (ignored by git). The exported SDK is only read.
# Re-running verifies an existing copy instead of rewriting it.
#
#   . "$IDF_PATH/export.sh"      # ESP-IDF 5.5.5
#   tools/p4/prepare_sdk.sh      # add --check to verify without staging
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
: "${IDF_PATH:?Export ESP-IDF 5.5.5 first}"
if [[ "${1:-}" == "--check" ]]; then
  python3 -B "$here/prepare_idf_bt.py" --check
  python3 -B "$here/prepare_idf_jpeg.py" --idf "$IDF_PATH" --check
else
  python3 -B "$here/prepare_idf_bt.py" --idf "$IDF_PATH"
  python3 -B "$here/prepare_idf_jpeg.py" --idf "$IDF_PATH"
fi
