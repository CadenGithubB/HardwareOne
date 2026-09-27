#!/usr/bin/env bash
# Build only. Activate the pinned ESP-IDF 5.5.5 environment first.
set -euo pipefail
experiment_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
app_dir="${experiment_dir}/private/app"
build_dir="${experiment_dir}/private/build-p4"
test -f "${app_dir}/CMakeLists.txt"
export HW_BOARD=p4x_eye
unset HW_DEPLOYMENT HW_OTA_LAYOUT
idf.py -C "${app_dir}" -B "${build_dir}" \
  -DIDF_TARGET=esp32p4 \
  -DSDKCONFIG="${build_dir}/sdkconfig" \
  -DHW1_MESH_EXPERIMENT_FEATURE_FILE="${experiment_dir}/features.h" \
  -DHW1_MESH_EXPERIMENT_SDKCONFIG_FILE="${experiment_dir}/sdkconfig.mesh.defaults" \
  build
