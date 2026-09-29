#!/usr/bin/env bash
# Build only, using the pinned IDF 5.5.5 + Arduino/Hosted connectivity baseline.
set -euo pipefail
experiment_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
app_dir="${experiment_dir}/private/app"
build_dir="${experiment_dir}/private/build-p4"
feature_file="${experiment_dir}/features.h"
case "${1:-}" in
  "") ;;
  --peripherals-off)
    feature_file="${experiment_dir}/features-disabled.h"
    build_dir="${experiment_dir}/private/build-p4-disabled"
    ;;
  *) echo "usage: $0 [--peripherals-off]" >&2; exit 2 ;;
esac
test -f "${app_dir}/CMakeLists.txt"
export HW_BOARD=p4x_eye
unset HW_DEPLOYMENT HW_OTA_LAYOUT
idf.py -C "${app_dir}" -B "${build_dir}" \
  -DIDF_TARGET=esp32p4 \
  -DSDKCONFIG="${build_dir}/sdkconfig" \
  -DHW1_MESH_EXPERIMENT_FEATURE_FILE="${feature_file}" \
  -DHW1_MESH_EXPERIMENT_SDKCONFIG_FILE="${experiment_dir}/../p4_connectivity/sdkconfig.connectivity.defaults;${experiment_dir}/../p4_connectivity/sdkconfig.p4.bluetooth.defaults" \
  build
