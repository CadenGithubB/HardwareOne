#!/usr/bin/env bash
# Build camera qualification applications. Never flash or open serial ports.
set -euo pipefail
probe_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd -- "${probe_dir}/../../.." && pwd)"
case "${1:-}" in
  p4) target=p4; chip=esp32p4 ;;
  s3) target=s3; chip=esp32s3 ;;
  *) echo "usage: $0 p4|s3" >&2; exit 2 ;;
esac
: "${IDF_PATH:?Activate the qualified ESP-IDF 5.5.5 environment first}"
expected_idf=b774170ff46c393eeb5e495ea37936038d3f4f4f
if [[ "$(git -C "$IDF_PATH" rev-parse HEAD)" != "$expected_idf" ]]; then
  echo "This probe is qualified with the pinned ESP-IDF 5.5.5 commit only." >&2
  exit 1
fi
project_dir="$(python3 -B "${probe_dir}/prepare.py" "$target")"
build_dir="${probe_dir}/../private/build-camera-repro-${target}"
idf_args=(-C "$project_dir" -B "$build_dir" -DIDF_TARGET="$chip"
  "-DSDKCONFIG=${build_dir}/sdkconfig"
  "-DSDKCONFIG_DEFAULTS=${project_dir}/sdkconfig.defaults")
if [[ "$target" == p4 ]]; then
  python3 -B "${repo_dir}/experiments/jpeg_portable/prepare_idf_jpeg.py" --idf "$IDF_PATH"
  idf_args+=("-DEXTRA_COMPONENT_DIRS=${repo_dir}/experiments/jpeg_portable/private/idf-components/esp_driver_jpeg")
fi
# Always start Kconfig selection from the checked-in defaults. Preserve prior
# sdkconfig separately for diagnostics instead of silently inheriting changes.
if [[ -f "${build_dir}/sdkconfig" ]]; then
  cp "${build_dir}/sdkconfig" "${build_dir}/sdkconfig.previous"
  rm "${build_dir}/sdkconfig"
fi
IDF_COMPONENT_MANAGER=1 idf.py "${idf_args[@]}" build
# Fail if dependency resolution changed the reviewed lock. Source copies and
# resulting binaries remain private for diagnosis; do not flash on this error.
cmp "${probe_dir}/${target}/dependencies.lock" "${project_dir}/dependencies.lock"
python3 - "$build_dir" <<'PY'
import hashlib
import sys
from pathlib import Path
for app in Path(sys.argv[1]).glob('hw1_camera*_probe.bin'):
    print(f'APPLICATION ONLY: {app.resolve()}')
    print(f'bytes={app.stat().st_size} sha256={hashlib.sha256(app.read_bytes()).hexdigest()}')
print('No device was accessed. Preserve bootloader/settings; never use the generated full-flash command.')
PY
