#!/usr/bin/env bash
# Isolated diagnostic firmware; activate the pinned IDF 5.5.5 environment first.
set -euo pipefail
private_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
experiment_dir="$(dirname -- "${private_dir}")"
app_dir="${private_dir}/app"
build_dir="${private_dir}/build-p4-smpdiag"
diagnostic_bt="${private_dir}/diagnostic-components/bt"
test -f "${diagnostic_bt}/host/bluedroid/stack/smp/smp_hw1_diag.h"
test -f "${private_dir}/build-p4/sdkconfig"
mkdir -p "${build_dir}"
if [[ ! -f "${build_dir}/sdkconfig" ]]; then
  cp "${private_dir}/build-p4/sdkconfig" "${build_dir}/sdkconfig"
fi
export HW_BOARD=p4x_eye
unset HW_DEPLOYMENT HW_OTA_LAYOUT
idf.py -C "${app_dir}" -B "${build_dir}" \
  -DIDF_TARGET=esp32p4 \
  -DSDKCONFIG="${build_dir}/sdkconfig" \
  -DEXTRA_COMPONENT_DIRS="${diagnostic_bt}" \
  -DHW1_MESH_EXPERIMENT_FEATURE_FILE="${experiment_dir}/features.h" \
  -DHW1_MESH_EXPERIMENT_SDKCONFIG_FILE="${experiment_dir}/sdkconfig.connectivity.defaults;${experiment_dir}/sdkconfig.p4.bluetooth.defaults" \
  build
