#!/usr/bin/env bash
# Compile the isolated codec probe; never flash or touch serial ports.
set -euo pipefail
experiment_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
target="${1:-}"
case "$target" in p4) chip=esp32p4 ;; s3) chip=esp32s3 ;; *) echo "usage: $0 p4|s3" >&2; exit 2 ;; esac
: "${IDF_PATH:?Activate ESP-IDF 5.5.5 first}"
dependency="${experiment_dir}/private/app-${target}/managed_components/espressif__esp_jpeg"
test -d "$dependency"
extra_components="$dependency"
if [[ "$target" == p4 ]]; then
    python3 -B "${experiment_dir}/prepare_idf_jpeg.py" --idf "$IDF_PATH"
    extra_components="${extra_components};${experiment_dir}/private/idf-components/esp_driver_jpeg"
fi
build_dir="${experiment_dir}/private/build-codec-${target}"
IDF_COMPONENT_MANAGER=0 idf.py -C "${experiment_dir}/codec" -B "$build_dir" \
    -DIDF_TARGET="$chip" -DSDKCONFIG="${build_dir}/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="${experiment_dir}/codec/sdkconfig.defaults;${experiment_dir}/codec/sdkconfig.${target}.defaults" \
    -DEXTRA_COMPONENT_DIRS="$extra_components" build
