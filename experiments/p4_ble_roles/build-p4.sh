#!/usr/bin/env bash
# Build only. Activate the pinned ESP-IDF 5.5.5 environment first.
set -euo pipefail
experiment_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
app_dir="${experiment_dir}/private/app"
build_dir="${experiment_dir}/private/build-p4"
test -f "${app_dir}/CMakeLists.txt"
python3 -B "${experiment_dir}/prepare_idf_bt.py" --idf "${IDF_PATH:?Activate ESP-IDF 5.5.5 first}"
export HW_BOARD=p4x_eye
unset HW_DEPLOYMENT HW_OTA_LAYOUT
idf.py -C "${app_dir}" -B "${build_dir}" \
  -DIDF_TARGET=esp32p4 \
  -DSDKCONFIG="${build_dir}/sdkconfig" \
  -DEXTRA_COMPONENT_DIRS="${experiment_dir}/private/idf-components/bt" \
  -DHW1_MESH_EXPERIMENT_FEATURE_FILE="${experiment_dir}/features.h" \
  -DHW1_MESH_EXPERIMENT_SDKCONFIG_FILE="${experiment_dir}/sdkconfig.connectivity.defaults;${experiment_dir}/sdkconfig.p4.bluetooth.defaults" \
  build
