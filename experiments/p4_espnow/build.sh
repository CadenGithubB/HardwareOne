#!/usr/bin/env bash
# Build only. No flashing, serial access, or automatic ESP-IDF installation.
#
# The native S3 and Hosted P4 probes build here. The C6 companion firmware and
# the P4 programmer service moved to tools/p4/companion; `programmer`, `c6` and
# `all` delegate to its build script, so their outputs live under
# tools/p4/companion/private/.
set -euo pipefail

experiment_root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
companion_root="$(cd -- "$experiment_root/../../tools/p4/companion" && pwd -P)"
selection="${1:-all}"
if [[ $# -gt 1 ]]; then
    printf 'Usage: %s [programmer|s3|p4|c6|all]\n' "$0" >&2
    exit 2
fi
case "$selection" in
    programmer|s3|p4|c6|all) ;;
    *) printf 'Usage: %s [programmer|s3|p4|c6|all]\n' "$0" >&2; exit 2 ;;
esac

if [[ -z "${IDF_PATH:-}" ]] || ! command -v idf.py >/dev/null 2>&1; then
    printf 'Export the ESP-IDF 5.5.5 environment before running this script.\n' >&2
    exit 1
fi
idf_version="$(idf.py --version)"
case "$idf_version" in
    'ESP-IDF v5.5.5'|'ESP-IDF v5.5.5-'*) ;;
    *) printf 'ESP-IDF 5.5.5 required; found %s\n' "$idf_version" >&2; exit 1 ;;
esac

build_one() {
    local name="$1" source_dir target build_dir config_file
    case "$name" in
        programmer|c6) "$companion_root/build.sh" "$name"; return ;;
        s3) source_dir="$experiment_root/native_s3"; target=esp32s3 ;;
        p4) source_dir="$experiment_root/hosted_p4"; target=esp32p4 ;;
    esac
    build_dir="$experiment_root/private/build-$name"
    config_file="$source_dir/sdkconfig"
    # Preserve existing sdkconfig and build output. Refuse a stale target or a
    # build directory belonging to another project instead of invoking set-target
    # (which can remove configuration) or fullclean.
    python3 - "$config_file" "$build_dir/CMakeCache.txt" "$target" "$source_dir" <<'PY'
import pathlib, re, sys
config, cache = map(pathlib.Path, sys.argv[1:3])
target, source = sys.argv[3:5]
if config.exists():
    match = re.search(r'^CONFIG_IDF_TARGET="([^"]+)"$', config.read_text(), re.M)
    if match and match.group(1) != target:
        raise SystemExit(f"Refusing {config}: target {match.group(1)}, expected {target}")
if cache.exists():
    contents = cache.read_text()
    for key, expected in (("IDF_TARGET", target), ("CMAKE_HOME_DIRECTORY", source)):
        match = re.search(r'^' + key + r':[^=]+=(.*)$', contents, re.M)
        if match and match.group(1) != expected:
            raise SystemExit(f"Refusing {cache}: {key}={match.group(1)}, expected {expected}")
PY
    printf 'Building %s (%s) in %s\n' "$name" "$target" "$build_dir"
    idf.py -C "$source_dir" -B "$build_dir" \
        -D "IDF_TARGET=$target" \
        -D "SDKCONFIG=$config_file" \
        -D "SDKCONFIG_DEFAULTS=$source_dir/sdkconfig.defaults" build
}

if [[ "$selection" == all ]]; then
    for name in programmer s3 p4 c6; do build_one "$name"; done
else
    build_one "$selection"
fi
