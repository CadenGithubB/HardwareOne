#!/usr/bin/env bash
# Build the ESP32-C6 companion firmware (pinned ESP-Hosted slave + HardwareOne
# ESP-NOW bridge) and the P4 service image that programs the C6. Build only:
# no flashing, no serial access, no ESP-IDF installation.
#
#   . "$IDF_PATH/export.sh"            # ESP-IDF 5.5.5
#   tools/p4/companion/build.sh        # c6 + programmer
#   tools/p4/companion/build.sh c6     # one of: c6 | programmer | all
#
# Outputs land in the ignored private/ workspace next to this script:
#   private/build-c6/          network_adapter.bin, bootloader, partition table
#   private/build-programmer/  hw1_c6_programmer.bin
# README.md documents backing up and flashing the C6 with them.
set -euo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
selection="${1:-all}"
if [[ $# -gt 1 ]]; then
    printf 'Usage: %s [c6|programmer|all]\n' "$0" >&2
    exit 2
fi
case "$selection" in
    c6|programmer|all) ;;
    *) printf 'Usage: %s [c6|programmer|all]\n' "$0" >&2; exit 2 ;;
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

python3 "$root/prepare.py"

build_one() {
    local name="$1" source_dir target build_dir config_file
    case "$name" in
        programmer) source_dir="$root/c6_programmer"; target=esp32p4 ;;
        c6) source_dir="$root/private/c6-slave"; target=esp32c6 ;;
    esac
    build_dir="$root/private/build-$name"
    config_file="$source_dir/sdkconfig"
    # Preserve existing sdkconfig and build output. Refuse a stale target or a
    # build directory belonging to another project instead of invoking
    # set-target (which can remove configuration) or fullclean.
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
    build_one programmer
    build_one c6
else
    build_one "$selection"
fi
