#!/usr/bin/env bash
# Build only. Activate the pinned ESP-IDF 5.5.5 environment first.
set -euo pipefail
experiment_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
app_dir="${experiment_dir}/private/app"
build_dir="${experiment_dir}/private/build-s3"
test -f "${app_dir}/CMakeLists.txt"
export HW_BOARD=xiao_s3
unset HW_DEPLOYMENT HW_OTA_LAYOUT
idf.py -C "${app_dir}" -B "${build_dir}" \
  -DIDF_TARGET=esp32s3 \
  -DSDKCONFIG="${build_dir}/sdkconfig" \
  -DHW1_MESH_EXPERIMENT_FEATURE_FILE="${experiment_dir}/features.h" \
  -DHW1_MESH_EXPERIMENT_SDKCONFIG_FILE="${experiment_dir}/sdkconfig.mesh.defaults" \
  build
