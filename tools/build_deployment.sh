#!/usr/bin/env bash
# Build one checked-in deployment as a paired factory-updater/main release.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"

usage() {
  echo "usage: HW1_OTA_SIGNING_KEY=/absolute/key.pem $(basename "$0") <deployment> <board>" >&2
  echo "available deployments:" >&2
  find "$REPO/deployments" -path '*/boards/*/contract.conf' -type f 2>/dev/null |
    sed -E "s#^$REPO/deployments/([^/]+)/boards/([^/]+)/contract.conf#  \1 \2#" >&2
  exit 1
}

DEPLOYMENT="${1:-}"
BOARD="${2:-}"
[[ -n "$DEPLOYMENT" && -n "$BOARD" && $# -eq 2 ]] || usage
[[ "$DEPLOYMENT" =~ ^[a-z0-9_]+$ && "$BOARD" =~ ^[a-z0-9_]+$ ]] || usage

SELECTOR="$DEPLOYMENT/$BOARD"
CONTRACT="$REPO/deployments/$DEPLOYMENT/boards/$BOARD/contract.conf"
BOARD_FILE="$REPO/boards/$BOARD.defaults"
[[ -f "$CONTRACT" ]] || { echo "error: unknown deployment $SELECTOR" >&2; usage; }
[[ -f "$BOARD_FILE" ]] || { echo "error: missing physical board file boards/$BOARD.defaults" >&2; exit 1; }

contract_value() {
  local key="$1"
  sed -n "s/^${key}=//p" "$CONTRACT" | head -1
}

CONTRACT_BOARD="$(contract_value BOARD_ID)"
TARGET="$(contract_value TARGET)"
[[ "$CONTRACT_BOARD" == "$BOARD" ]] || {
  echo "error: $CONTRACT says BOARD_ID=$CONTRACT_BOARD, expected $BOARD" >&2
  exit 1
}
[[ -n "$TARGET" ]] || { echo "error: $CONTRACT has no TARGET" >&2; exit 1; }

SIGNING_KEY="${HW1_OTA_SIGNING_KEY:-}"
[[ -n "$SIGNING_KEY" ]] || {
  echo "error: set HW1_OTA_SIGNING_KEY to the deployment RSA-3072 private key" >&2
  exit 1
}
[[ "$SIGNING_KEY" = /* && -f "$SIGNING_KEY" ]] || {
  echo "error: HW1_OTA_SIGNING_KEY must name an existing absolute path" >&2
  exit 1
}
command -v idf.py >/dev/null 2>&1 || {
  echo "error: idf.py not on PATH; source the ESP-IDF export script first" >&2
  exit 1
}

OUTPUT_ROOT="$REPO/build/deployments/$DEPLOYMENT/$BOARD"
UPDATER_BUILD="$OUTPUT_ROOT/updater"
MAIN_BUILD="$OUTPUT_ROOT/main"
RELEASE_DIR="$OUTPUT_ROOT/release"
LOCK_DIR="$OUTPUT_ROOT/.build.lock"
STAGING_RELEASE=""
PREVIOUS_RELEASE=""
PUBLISHED_RELEASE=0

mkdir -p "$OUTPUT_ROOT"
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
  LOCK_HOLDER=""
  if [[ -r "$LOCK_DIR/pid" ]]; then
    read -r LOCK_HOLDER < "$LOCK_DIR/pid" || true
  fi
  if [[ "$LOCK_HOLDER" =~ ^[0-9]+$ ]] && kill -0 "$LOCK_HOLDER" 2>/dev/null; then
    echo "error: another $SELECTOR deployment build is running (pid $LOCK_HOLDER)" >&2
  else
    echo "error: stale or incomplete deployment build lock: $LOCK_DIR" >&2
    echo "       verify no build is running, then remove that directory" >&2
  fi
  exit 1
fi
printf '%s\n' "$$" > "$LOCK_DIR/pid"

cleanup() {
  if [[ -n "$STAGING_RELEASE" && -d "$STAGING_RELEASE" ]]; then
    rm -rf -- "$STAGING_RELEASE"
  fi
  if [[ -n "$PREVIOUS_RELEASE" && -e "$PREVIOUS_RELEASE" ]]; then
    if [[ "$PUBLISHED_RELEASE" -eq 1 ]]; then
      rm -rf -- "$PREVIOUS_RELEASE"
    elif [[ ! -e "$RELEASE_DIR" ]]; then
      mv "$PREVIOUS_RELEASE" "$RELEASE_DIR" || true
    fi
  fi
  LOCK_HOLDER=""
  if [[ -r "$LOCK_DIR/pid" ]]; then
    read -r LOCK_HOLDER < "$LOCK_DIR/pid" || true
  fi
  if [[ "$LOCK_HOLDER" == "$$" ]]; then
    rm -f -- "$LOCK_DIR/pid"
    rmdir "$LOCK_DIR" 2>/dev/null || true
  fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo "==> Building $SELECTOR factory updater"
env IDF_TARGET="$TARGET" HW_BOARD="$BOARD" HW_DEPLOYMENT="$SELECTOR" \
  HW1_OTA_SIGNING_KEY="$SIGNING_KEY" \
  idf.py -C "$REPO/updater" -B "$UPDATER_BUILD" build

UPDATER_BIN="$UPDATER_BUILD/hw1-updater.bin"
[[ -f "$UPDATER_BIN" ]] || { echo "error: updater output missing: $UPDATER_BIN" >&2; exit 1; }

echo "==> Building $SELECTOR main image"
env IDF_TARGET="$TARGET" HW_BOARD="$BOARD" HW_DEPLOYMENT="$SELECTOR" \
  HW_OTA_LAYOUT=1 HW1_OTA_SIGNING_KEY="$SIGNING_KEY" \
  HW1_UPDATER_BIN="$UPDATER_BIN" \
  idf.py -C "$REPO" -B "$MAIN_BUILD" -DSDKCONFIG="$MAIN_BUILD/sdkconfig" build

echo "==> Auditing paired artifacts"
python3 "$REPO/tools/ota/check_ota_builds.py" \
  --board "$BOARD" --deployment "$SELECTOR" \
  --main-build "$MAIN_BUILD" --updater-build "$UPDATER_BUILD"

python3 "$REPO/tools/gen_build_info.py" "$MAIN_BUILD" "$BOARD" "$SELECTOR"

STAGING_RELEASE="$(mktemp -d "$OUTPUT_ROOT/.release-stage.XXXXXX")"

MANIFEST="$STAGING_RELEASE/manifest.json"
BUNDLE="$STAGING_RELEASE/HardwareOne-$DEPLOYMENT-$BOARD.hw1ota"
MAIN_BIN="$MAIN_BUILD/hardwareone-idf.bin"

echo "==> Creating signed manifest and offline bundle"
python3 "$REPO/tools/ota/make_manifest.py" create \
  --board "$BOARD" --deployment "$SELECTOR" \
  --image "$MAIN_BIN" --key "$SIGNING_KEY" \
  --output "$MANIFEST"
python3 "$REPO/tools/ota/make_bundle.py" create \
  --image "$MAIN_BIN" --manifest "$MANIFEST" \
  --public-key "$MAIN_BUILD/hw1_ota_public_key.pem" \
  --output "$BUNDLE"

cp "$MAIN_BIN" "$STAGING_RELEASE/firmware.bin"
cp "$UPDATER_BIN" "$STAGING_RELEASE/factory-updater.bin"
cp "$MAIN_BUILD/partition_table/partition-table.bin" "$STAGING_RELEASE/partition-table.bin"
cp "$MAIN_BUILD/hw1_ota_public_key.pem" "$STAGING_RELEASE/public-key.pem"
cp "$MAIN_BUILD/BUILD_INFO.md" "$STAGING_RELEASE/BUILD_INFO.md"
cp "$MAIN_BUILD/bootloader/bootloader.bin" "$STAGING_RELEASE/bootloader.bin"
cp "$MAIN_BUILD/ota_data_initial.bin" "$STAGING_RELEASE/ota-data-initial.bin"
cp "$MAIN_BUILD/littlefs.bin" "$STAGING_RELEASE/littlefs.bin"
cp "$CONTRACT" "$STAGING_RELEASE/contract.conf"
cp "$REPO/deployments/$DEPLOYMENT/boards/$BOARD/partitions.csv" \
  "$STAGING_RELEASE/partitions.csv"
if [[ -f "$REPO/deployments/$DEPLOYMENT/boards/$BOARD/MIGRATION.md" ]]; then
  cp "$REPO/deployments/$DEPLOYMENT/boards/$BOARD/MIGRATION.md" \
    "$STAGING_RELEASE/MIGRATION.md"
fi

# Publish only a complete release.  Keep the previous successful directory
# recoverable until the staged directory has moved into place.
if [[ -e "$RELEASE_DIR" ]]; then
  PREVIOUS_RELEASE="$OUTPUT_ROOT/.release-previous.$$"
  mv "$RELEASE_DIR" "$PREVIOUS_RELEASE"
fi
if mv "$STAGING_RELEASE" "$RELEASE_DIR"; then
  STAGING_RELEASE=""
  PUBLISHED_RELEASE=1
  if [[ -n "$PREVIOUS_RELEASE" ]]; then
    rm -rf -- "$PREVIOUS_RELEASE"
    PREVIOUS_RELEASE=""
  fi
else
  if [[ -n "$PREVIOUS_RELEASE" && -e "$PREVIOUS_RELEASE" ]]; then
    mv "$PREVIOUS_RELEASE" "$RELEASE_DIR"
  fi
  echo "error: could not publish staged release" >&2
  exit 1
fi

echo "==> $SELECTOR deployment ready: $RELEASE_DIR"
