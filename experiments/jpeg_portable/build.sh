#!/usr/bin/env bash
# Build only: this script never opens serial ports or writes a device.
set -euo pipefail
experiment_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
roles_dir="${experiment_dir}/../p4_ble_roles"
target="${1:-}"
case "$target" in
    p4)
        chip=esp32p4; board=p4x_eye
        features="${experiment_dir}/../p4_io/features.h"
        defaults="${roles_dir}/sdkconfig.connectivity.defaults;${roles_dir}/sdkconfig.p4.bluetooth.defaults"
        ;;
    s3)
        chip=esp32s3; board=xiao_s3
        features="${experiment_dir}/features-s3.h"
        defaults="${roles_dir}/sdkconfig.connectivity.defaults"
        ;;
    *) echo "usage: $0 p4|s3" >&2; exit 2 ;;
esac
app_dir="${experiment_dir}/private/app-${target}"
build_dir="${experiment_dir}/private/build-${target}"
python3 -B "${experiment_dir}/prepare.py" --target "$target" --check
python3 -B "${roles_dir}/prepare_idf_bt.py" \
    --idf "${IDF_PATH:?Activate ESP-IDF 5.5.5 first}" \
    --destination "${experiment_dir}/private/idf-components/bt"
extra_components="${experiment_dir}/private/idf-components/bt"
if [[ "$target" == p4 ]]; then
    python3 -B "${experiment_dir}/prepare_idf_jpeg.py" --idf "$IDF_PATH"
    extra_components="${extra_components};${experiment_dir}/private/idf-components/esp_driver_jpeg"
fi
export HW_BOARD="$board"
unset HW_DEPLOYMENT HW_OTA_LAYOUT
idf.py -C "$app_dir" -B "$build_dir" \
    -DIDF_TARGET="$chip" -DSDKCONFIG="${build_dir}/sdkconfig" \
    -DEXTRA_COMPONENT_DIRS="$extra_components" \
    -DHW1_MESH_EXPERIMENT_FEATURE_FILE="$features" \
    -DHW1_MESH_EXPERIMENT_SDKCONFIG_FILE="$defaults" build
