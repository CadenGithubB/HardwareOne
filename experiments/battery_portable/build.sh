#!/usr/bin/env bash
# Build only. Does not open ports, flash hardware or alter qualified snapshots.
set -euo pipefail
battery_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
jpeg_dir="${battery_dir}/../jpeg_portable"
roles_dir="${battery_dir}/../p4_ble_roles"
: "${IDF_PATH:?Activate the pinned ESP-IDF 5.5.5 first}"
target="${1:-}"
case "$target" in
  p4)
    chip=esp32p4; board=p4x_eye
    defaults="${roles_dir}/sdkconfig.connectivity.defaults;${roles_dir}/sdkconfig.p4.bluetooth.defaults"
    ;;
  s3)
    chip=esp32s3; board=xiao_s3
    defaults="${roles_dir}/sdkconfig.connectivity.defaults"
    ;;
  *) echo "usage: $0 p4|s3" >&2; exit 2 ;;
esac
python3 -B "${battery_dir}/prepare.py" --target "$target" --check
python3 -B "${roles_dir}/prepare_idf_bt.py" --destination "${jpeg_dir}/private/idf-components/bt" --check
extra="${jpeg_dir}/private/idf-components/bt"
if [[ "$target" == p4 ]]; then
  python3 -B "${jpeg_dir}/prepare_idf_jpeg.py" --idf "$IDF_PATH" --check
  extra="${extra};${jpeg_dir}/private/idf-components/esp_driver_jpeg"
fi
build_dir="${battery_dir}/private/build-${target}"
export HW_BOARD="$board"
unset HW_DEPLOYMENT HW_OTA_LAYOUT
idf.py -C "${battery_dir}/private/app-${target}" -B "$build_dir" \
  -DIDF_TARGET="$chip" -DSDKCONFIG="${build_dir}/sdkconfig" \
  -DEXTRA_COMPONENT_DIRS="$extra" \
  -DHW1_MESH_EXPERIMENT_FEATURE_FILE="${battery_dir}/features-${target}.h" \
  -DHW1_MESH_EXPERIMENT_SDKCONFIG_FILE="$defaults" build
